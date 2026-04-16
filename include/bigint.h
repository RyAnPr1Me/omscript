#pragma once
// bigint.h — Self-contained, header-only 128-bit and 256-bit fixed-precision
// integer library for OmScript's runtime.
//
// Design:
//   - OmUInt128 / OmInt128  : 2 × uint64_t, little-endian (lo, hi)
//   - OmUInt256 / OmInt256  : 4 × uint64_t, little-endian (w[0]..w[3])
//   - All functions are `inline` to allow inlining by the compiler.
//   - No heap allocation, no exceptions, no external dependencies.
//   - Signed types use two's-complement arithmetic (same as C integers).

#include <cstdint>
#include <cstring>
#include <string>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// OmUInt128 — 128-bit unsigned integer
// ─────────────────────────────────────────────────────────────────────────────
struct OmUInt128 {
    uint64_t lo{0};
    uint64_t hi{0};

    constexpr OmUInt128() = default;
    constexpr OmUInt128(uint64_t lo_, uint64_t hi_) : lo(lo_), hi(hi_) {}
    constexpr explicit OmUInt128(uint64_t v) : lo(v), hi(0) {}

    static inline OmUInt128 from_i64(int64_t v) {
        // Sign-extend: if negative, hi = all-ones
        return { static_cast<uint64_t>(v),
                 v < 0 ? UINT64_MAX : 0 };
    }

    inline int64_t to_i64() const { return static_cast<int64_t>(lo); }
    inline uint64_t to_u64() const { return lo; }

    inline bool is_zero() const { return lo == 0 && hi == 0; }

    // Addition
    inline OmUInt128 operator+(const OmUInt128& o) const {
        OmUInt128 r;
        r.lo = lo + o.lo;
        r.hi = hi + o.hi + (r.lo < lo ? 1u : 0u);
        return r;
    }
    inline OmUInt128& operator+=(const OmUInt128& o) { *this = *this + o; return *this; }

    // Subtraction
    inline OmUInt128 operator-(const OmUInt128& o) const {
        OmUInt128 r;
        r.lo = lo - o.lo;
        r.hi = hi - o.hi - (lo < o.lo ? 1u : 0u);
        return r;
    }
    inline OmUInt128& operator-=(const OmUInt128& o) { *this = *this - o; return *this; }

    // Unary negation (two's complement)
    inline OmUInt128 operator-() const {
        OmUInt128 r;
        r.lo = ~lo + 1u;
        r.hi = ~hi + (r.lo == 0 ? 1u : 0u);
        return r;
    }

    // Bitwise
    inline OmUInt128 operator&(const OmUInt128& o) const { return {lo & o.lo, hi & o.hi}; }
    inline OmUInt128 operator|(const OmUInt128& o) const { return {lo | o.lo, hi | o.hi}; }
    inline OmUInt128 operator^(const OmUInt128& o) const { return {lo ^ o.lo, hi ^ o.hi}; }
    inline OmUInt128 operator~() const { return {~lo, ~hi}; }

    // Shift left
    inline OmUInt128 operator<<(unsigned s) const {
        if (s == 0) return *this;
        if (s >= 128) return OmUInt128{};
        if (s >= 64) return {0, lo << (s - 64)};
        return {lo << s, (hi << s) | (lo >> (64 - s))};
    }

    // Shift right (logical / unsigned)
    inline OmUInt128 operator>>(unsigned s) const {
        if (s == 0) return *this;
        if (s >= 128) return OmUInt128{};
        if (s >= 64) return {hi >> (s - 64), 0};
        return {(lo >> s) | (hi << (64 - s)), hi >> s};
    }

    // Multiplication (lower 128 bits)
    inline OmUInt128 operator*(const OmUInt128& o) const {
        // Split each into 32-bit halves for safe 64-bit arithmetic
        const uint64_t a0 = lo & 0xFFFFFFFFULL, a1 = lo >> 32;
        const uint64_t b0 = o.lo & 0xFFFFFFFFULL, b1 = o.lo >> 32;
        const uint64_t p00 = a0 * b0;
        const uint64_t p01 = a0 * b1;
        const uint64_t p10 = a1 * b0;
        const uint64_t p11 = a1 * b1;
        const uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);
        const uint64_t rlo = (p00 & 0xFFFFFFFFULL) | (mid << 32);
        const uint64_t rhi = (p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32))
                           + hi * o.lo + lo * o.hi;
        return {rlo, rhi};
    }

    // Division and modulo (unsigned)
    inline OmUInt128 operator/(const OmUInt128& d) const;
    inline OmUInt128 operator%(const OmUInt128& d) const;

    // Comparison
    inline bool operator==(const OmUInt128& o) const { return lo == o.lo && hi == o.hi; }
    inline bool operator!=(const OmUInt128& o) const { return !(*this == o); }
    inline bool operator< (const OmUInt128& o) const {
        return hi < o.hi || (hi == o.hi && lo < o.lo);
    }
    inline bool operator<=(const OmUInt128& o) const { return !(o < *this); }
    inline bool operator> (const OmUInt128& o) const { return o < *this; }
    inline bool operator>=(const OmUInt128& o) const { return !(*this < o); }

    std::string to_string() const;
    static OmUInt128 from_string(const std::string& s);
};

// Simple restoring division
inline OmUInt128 OmUInt128::operator/(const OmUInt128& d) const {
    if (d.is_zero()) return OmUInt128{UINT64_MAX, UINT64_MAX}; // undefined
    if (*this < d) return OmUInt128{};
    OmUInt128 q{}, r{};
    for (int i = 127; i >= 0; --i) {
        r = r << 1;
        // Set bit i of *this into bit 0 of r
        const unsigned word = static_cast<unsigned>(i) / 64;
        const unsigned bit  = static_cast<unsigned>(i) % 64;
        const uint64_t b    = (word == 0 ? lo : hi) >> bit & 1u;
        r.lo |= b;
        if (r >= d) {
            r -= d;
            if (i < 64) q.lo |= UINT64_C(1) << i;
            else         q.hi |= UINT64_C(1) << (i - 64);
        }
    }
    return q;
}

inline OmUInt128 OmUInt128::operator%(const OmUInt128& d) const {
    if (d.is_zero()) return *this;
    OmUInt128 r{};
    for (int i = 127; i >= 0; --i) {
        r = r << 1;
        const unsigned word = static_cast<unsigned>(i) / 64;
        const unsigned bit  = static_cast<unsigned>(i) % 64;
        const uint64_t b    = (word == 0 ? lo : hi) >> bit & 1u;
        r.lo |= b;
        if (r >= d) r -= d;
    }
    return r;
}

inline std::string OmUInt128::to_string() const {
    if (is_zero()) return "0";
    std::string s;
    OmUInt128 v = *this;
    const OmUInt128 ten{10, 0};
    while (!v.is_zero()) {
        s += static_cast<char>('0' + (v % ten).lo);
        v = v / ten;
    }
    std::reverse(s.begin(), s.end());
    return s;
}

inline OmUInt128 OmUInt128::from_string(const std::string& s) {
    OmUInt128 v{};
    const OmUInt128 ten{10, 0};
    for (char c : s) {
        if (c < '0' || c > '9') break;
        v = v * ten + OmUInt128{static_cast<uint64_t>(c - '0'), 0};
    }
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// OmInt128 — 128-bit signed integer (two's complement, wrapping)
// ─────────────────────────────────────────────────────────────────────────────
struct OmInt128 {
    OmUInt128 bits;

    constexpr OmInt128() = default;
    constexpr explicit OmInt128(OmUInt128 b) : bits(b) {}
    explicit OmInt128(int64_t v) : bits(OmUInt128::from_i64(v)) {}

    static inline OmInt128 from_i64(int64_t v) { return OmInt128{v}; }
    inline int64_t to_i64() const { return bits.to_i64(); }

    inline bool is_negative() const { return (bits.hi >> 63) & 1u; }

    inline OmInt128 operator+(const OmInt128& o) const { return OmInt128{bits + o.bits}; }
    inline OmInt128 operator-(const OmInt128& o) const { return OmInt128{bits - o.bits}; }
    inline OmInt128 operator-() const { return OmInt128{-bits}; }
    inline OmInt128 operator*(const OmInt128& o) const { return OmInt128{bits * o.bits}; }

    inline OmInt128 abs_val() const { return is_negative() ? -(*this) : *this; }

    inline OmInt128 operator/(const OmInt128& d) const {
        const bool neg = is_negative() != d.is_negative();
        OmInt128 q{abs_val().bits / d.abs_val().bits};
        return neg ? -q : q;
    }

    inline OmInt128 operator%(const OmInt128& d) const {
        const bool neg = is_negative();
        OmInt128 r{abs_val().bits % d.abs_val().bits};
        return neg ? -r : r;
    }

    inline OmInt128 operator&(const OmInt128& o) const { return OmInt128{bits & o.bits}; }
    inline OmInt128 operator|(const OmInt128& o) const { return OmInt128{bits | o.bits}; }
    inline OmInt128 operator^(const OmInt128& o) const { return OmInt128{bits ^ o.bits}; }
    inline OmInt128 operator~() const { return OmInt128{~bits}; }
    inline OmInt128 operator<<(unsigned s) const { return OmInt128{bits << s}; }
    // Arithmetic right shift
    inline OmInt128 operator>>(unsigned s) const {
        if (s == 0) return *this;
        if (!is_negative()) return OmInt128{bits >> s};
        // Fill with sign bit (1s)
        OmUInt128 r = bits >> s;
        if (s < 128) {
            if (s < 64) {
                r.hi |= (UINT64_MAX << (64 - s));
            } else {
                r.hi = UINT64_MAX;
                if (s < 128) r.lo |= (UINT64_MAX << (128 - s));
            }
        } else {
            r = OmUInt128{UINT64_MAX, UINT64_MAX};
        }
        return OmInt128{r};
    }

    inline bool operator==(const OmInt128& o) const { return bits == o.bits; }
    inline bool operator!=(const OmInt128& o) const { return bits != o.bits; }
    inline bool operator<(const OmInt128& o) const {
        if (is_negative() != o.is_negative()) return is_negative();
        return bits < o.bits ? !is_negative() : (bits == o.bits ? false : is_negative());
    }
    inline bool operator<=(const OmInt128& o) const { return !(o < *this); }
    inline bool operator>(const OmInt128& o) const  { return o < *this; }
    inline bool operator>=(const OmInt128& o) const { return !(*this < o); }

    inline std::string to_string() const {
        if (bits.is_zero()) return "0";
        if (is_negative()) return "-" + (-(*this)).bits.to_string();
        return bits.to_string();
    }
    static inline OmInt128 from_string(const std::string& s) {
        if (!s.empty() && s[0] == '-')
            return -OmInt128{OmUInt128::from_string(s.substr(1))};
        return OmInt128{OmUInt128::from_string(s)};
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// OmUInt256 — 256-bit unsigned integer (4 × uint64_t, little-endian)
// ─────────────────────────────────────────────────────────────────────────────
struct OmUInt256 {
    uint64_t w[4]{};  // w[0] = least significant

    constexpr OmUInt256() = default;
    constexpr explicit OmUInt256(uint64_t v) : w{v, 0, 0, 0} {}
    constexpr OmUInt256(uint64_t w0, uint64_t w1, uint64_t w2, uint64_t w3)
        : w{w0, w1, w2, w3} {}

    static inline OmUInt256 from_i64(int64_t v) {
        const uint64_t fill = v < 0 ? UINT64_MAX : 0;
        return {static_cast<uint64_t>(v), fill, fill, fill};
    }

    static inline OmUInt256 from_u128(const OmUInt128& v) {
        return {v.lo, v.hi, 0, 0};
    }

    inline int64_t to_i64() const { return static_cast<int64_t>(w[0]); }
    inline uint64_t to_u64() const { return w[0]; }

    inline bool is_zero() const { return w[0]==0 && w[1]==0 && w[2]==0 && w[3]==0; }

    inline OmUInt256 operator+(const OmUInt256& o) const {
        OmUInt256 r;
        uint64_t carry = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned __int128 s = static_cast<unsigned __int128>(w[i]) + o.w[i] + carry;
            r.w[i] = static_cast<uint64_t>(s);
            carry   = static_cast<uint64_t>(s >> 64);
        }
        return r;
    }
    inline OmUInt256& operator+=(const OmUInt256& o) { *this = *this + o; return *this; }

    inline OmUInt256 operator-(const OmUInt256& o) const {
        OmUInt256 r;
        uint64_t borrow = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned __int128 s = static_cast<unsigned __int128>(w[i]) - o.w[i] - borrow;
            r.w[i] = static_cast<uint64_t>(s);
            borrow  = static_cast<uint64_t>(-(static_cast<int64_t>(s >> 64)));
        }
        return r;
    }
    inline OmUInt256& operator-=(const OmUInt256& o) { *this = *this - o; return *this; }

    inline OmUInt256 operator-() const {
        OmUInt256 nr;
        unsigned __int128 c = 1;
        for (int i = 0; i < 4; ++i) {
            const unsigned __int128 s = static_cast<unsigned __int128>(~w[i]) + c;
            nr.w[i] = static_cast<uint64_t>(s);
            c = s >> 64;
        }
        return nr;
    }

    inline OmUInt256 operator&(const OmUInt256& o) const {
        return {w[0]&o.w[0], w[1]&o.w[1], w[2]&o.w[2], w[3]&o.w[3]};
    }
    inline OmUInt256 operator|(const OmUInt256& o) const {
        return {w[0]|o.w[0], w[1]|o.w[1], w[2]|o.w[2], w[3]|o.w[3]};
    }
    inline OmUInt256 operator^(const OmUInt256& o) const {
        return {w[0]^o.w[0], w[1]^o.w[1], w[2]^o.w[2], w[3]^o.w[3]};
    }
    inline OmUInt256 operator~() const { return {~w[0], ~w[1], ~w[2], ~w[3]}; }

    inline OmUInt256 operator*(const OmUInt256& o) const {
        OmUInt256 result;
        for (int i = 0; i < 4; ++i) {
            uint64_t carry = 0;
            for (int j = 0; j < 4 - i; ++j) {
                const unsigned __int128 prod =
                    static_cast<unsigned __int128>(w[i]) * o.w[j] + result.w[i+j] + carry;
                result.w[i+j] = static_cast<uint64_t>(prod);
                carry = static_cast<uint64_t>(prod >> 64);
            }
        }
        return result;
    }

    inline OmUInt256 operator<<(unsigned s) const {
        if (s == 0) return *this;
        if (s >= 256) return OmUInt256{};
        OmUInt256 r;
        const unsigned word_shift = s / 64;
        const unsigned bit_shift  = s % 64;
        for (int i = 3; i >= 0; --i) {
            const int src = i - static_cast<int>(word_shift);
            if (src < 0) { r.w[i] = 0; continue; }
            r.w[i] = w[src] << bit_shift;
            if (bit_shift > 0 && src > 0)
                r.w[i] |= w[src-1] >> (64 - bit_shift);
        }
        return r;
    }

    inline OmUInt256 operator>>(unsigned s) const {
        if (s == 0) return *this;
        if (s >= 256) return OmUInt256{};
        OmUInt256 r;
        const unsigned word_shift = s / 64;
        const unsigned bit_shift  = s % 64;
        for (int i = 0; i < 4; ++i) {
            const int src = i + static_cast<int>(word_shift);
            if (src >= 4) { r.w[i] = 0; continue; }
            r.w[i] = w[src] >> bit_shift;
            if (bit_shift > 0 && src < 3)
                r.w[i] |= w[src+1] << (64 - bit_shift);
        }
        return r;
    }

    inline OmUInt256 operator/(const OmUInt256& d) const;
    inline OmUInt256 operator%(const OmUInt256& d) const;

    inline bool operator==(const OmUInt256& o) const {
        return w[0]==o.w[0] && w[1]==o.w[1] && w[2]==o.w[2] && w[3]==o.w[3];
    }
    inline bool operator!=(const OmUInt256& o) const { return !(*this == o); }
    inline bool operator<(const OmUInt256& o) const {
        for (int i = 3; i >= 0; --i) {
            if (w[i] < o.w[i]) return true;
            if (w[i] > o.w[i]) return false;
        }
        return false;
    }
    inline bool operator<=(const OmUInt256& o) const { return !(o < *this); }
    inline bool operator>(const OmUInt256& o)  const { return o < *this; }
    inline bool operator>=(const OmUInt256& o) const { return !(*this < o); }

    std::string to_string() const;
    static OmUInt256 from_string(const std::string& s);
};

inline OmUInt256 OmUInt256::operator/(const OmUInt256& d) const {
    if (d.is_zero()) return OmUInt256{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX};
    if (*this < d) return OmUInt256{};
    OmUInt256 q{}, r{};
    for (int i = 255; i >= 0; --i) {
        r = r << 1;
        const unsigned word = static_cast<unsigned>(i) / 64;
        const unsigned bit  = static_cast<unsigned>(i) % 64;
        r.w[0] |= (w[word] >> bit) & 1u;
        if (r >= d) {
            r -= d;
            q.w[i/64] |= UINT64_C(1) << (i % 64);
        }
    }
    return q;
}

inline OmUInt256 OmUInt256::operator%(const OmUInt256& d) const {
    if (d.is_zero()) return *this;
    OmUInt256 r{};
    for (int i = 255; i >= 0; --i) {
        r = r << 1;
        const unsigned word = static_cast<unsigned>(i) / 64;
        const unsigned bit  = static_cast<unsigned>(i) % 64;
        r.w[0] |= (w[word] >> bit) & 1u;
        if (r >= d) r -= d;
    }
    return r;
}

inline std::string OmUInt256::to_string() const {
    if (is_zero()) return "0";
    std::string s;
    OmUInt256 v = *this;
    const OmUInt256 ten{10};
    while (!v.is_zero()) {
        s += static_cast<char>('0' + (v % ten).w[0]);
        v = v / ten;
    }
    std::reverse(s.begin(), s.end());
    return s;
}

inline OmUInt256 OmUInt256::from_string(const std::string& s) {
    OmUInt256 v{};
    const OmUInt256 ten{10};
    for (char c : s) {
        if (c < '0' || c > '9') break;
        v = v * ten + OmUInt256{static_cast<uint64_t>(c - '0')};
    }
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// OmInt256 — 256-bit signed integer (two's complement, wrapping)
// ─────────────────────────────────────────────────────────────────────────────
struct OmInt256 {
    OmUInt256 bits;

    constexpr OmInt256() = default;
    constexpr explicit OmInt256(OmUInt256 b) : bits(b) {}
    explicit OmInt256(int64_t v) : bits(OmUInt256::from_i64(v)) {}

    static inline OmInt256 from_i64(int64_t v) { return OmInt256{v}; }
    inline int64_t to_i64() const { return bits.to_i64(); }

    inline bool is_negative() const { return (bits.w[3] >> 63) & 1u; }

    inline OmInt256 operator+(const OmInt256& o) const { return OmInt256{bits + o.bits}; }
    inline OmInt256 operator-(const OmInt256& o) const { return OmInt256{bits - o.bits}; }
    inline OmInt256 operator-() const { return OmInt256{-bits}; }
    inline OmInt256 operator*(const OmInt256& o) const { return OmInt256{bits * o.bits}; }

    inline OmInt256 abs_val() const { return is_negative() ? -(*this) : *this; }

    inline OmInt256 operator/(const OmInt256& d) const {
        const bool neg = is_negative() != d.is_negative();
        OmInt256 q{abs_val().bits / d.abs_val().bits};
        return neg ? -q : q;
    }

    inline OmInt256 operator%(const OmInt256& d) const {
        const bool neg = is_negative();
        OmInt256 r{abs_val().bits % d.abs_val().bits};
        return neg ? -r : r;
    }

    inline OmInt256 operator&(const OmInt256& o) const { return OmInt256{bits & o.bits}; }
    inline OmInt256 operator|(const OmInt256& o) const { return OmInt256{bits | o.bits}; }
    inline OmInt256 operator^(const OmInt256& o) const { return OmInt256{bits ^ o.bits}; }
    inline OmInt256 operator~() const { return OmInt256{~bits}; }
    inline OmInt256 operator<<(unsigned s) const { return OmInt256{bits << s}; }

    inline OmInt256 operator>>(unsigned s) const {
        if (s == 0 || !is_negative()) return OmInt256{bits >> s};
        if (s >= 256) return OmInt256{OmUInt256{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
        OmUInt256 r = bits >> s;
        // Fill high bits with 1
        const unsigned fill_words = s / 64;
        const unsigned fill_bits  = s % 64;
        for (unsigned i = 4 - fill_words; i < 4; ++i)
            r.w[i] = UINT64_MAX;
        if (fill_bits > 0 && (4 - fill_words) < 4)
            r.w[4 - fill_words - 1] |= UINT64_MAX << (64 - fill_bits);
        return OmInt256{r};
    }

    inline bool operator==(const OmInt256& o) const { return bits == o.bits; }
    inline bool operator!=(const OmInt256& o) const { return bits != o.bits; }
    inline bool operator<(const OmInt256& o) const {
        if (is_negative() != o.is_negative()) return is_negative();
        if (is_negative()) return o.bits < bits; // both negative
        return bits < o.bits;
    }
    inline bool operator<=(const OmInt256& o) const { return !(o < *this); }
    inline bool operator>(const OmInt256& o)  const { return o < *this; }
    inline bool operator>=(const OmInt256& o) const { return !(*this < o); }

    inline std::string to_string() const {
        if (bits.is_zero()) return "0";
        if (is_negative()) return "-" + (-(*this)).bits.to_string();
        return bits.to_string();
    }
    static inline OmInt256 from_string(const std::string& s) {
        if (!s.empty() && s[0] == '-')
            return -OmInt256{OmUInt256::from_string(s.substr(1))};
        return OmInt256{OmUInt256::from_string(s)};
    }
};

} // namespace omscript
