// bigint.cpp — Arbitrary-precision integer implementation for OmScript.
// Implements OmBigUInt and OmBigInt declared in include/bigint.h.
#include "bigint.h"
#include <cassert>
#include <cctype>
#include <stdexcept>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// OmBigUInt helpers
// ─────────────────────────────────────────────────────────────────────────────

// from_i64: treat i64 as unsigned 64-bit (sign-extending would give all-ones)
OmBigUInt OmBigUInt::from_i64(int64_t v) {
    // Interpret v as a 64-bit pattern (same as reinterpret_cast to uint64_t).
    uint64_t uv = static_cast<uint64_t>(v);
    return OmBigUInt(uv);
}

size_t OmBigUInt::bit_length() const {
    if (limbs.empty()) return 0;
    uint64_t top = limbs.back();
    size_t bits = (limbs.size() - 1) * 64;
    while (top) { ++bits; top >>= 1; }
    return bits;
}

bool OmBigUInt::test_bit(size_t n) const {
    size_t idx = n / 64, bit = n % 64;
    if (idx >= limbs.size()) return false;
    return (limbs[idx] >> bit) & 1;
}

void OmBigUInt::set_bit(size_t n) {
    size_t idx = n / 64, bit = n % 64;
    if (idx >= limbs.size()) limbs.resize(idx + 1, 0);
    limbs[idx] |= (uint64_t(1) << bit);
}

int OmBigUInt::compare(const OmBigUInt& o) const {
    if (limbs.size() != o.limbs.size())
        return limbs.size() < o.limbs.size() ? -1 : 1;
    for (size_t i = limbs.size(); i-- > 0;) {
        if (limbs[i] != o.limbs[i])
            return limbs[i] < o.limbs[i] ? -1 : 1;
    }
    return 0;
}

OmBigUInt OmBigUInt::operator+(const OmBigUInt& o) const {
    const size_t n = std::max(limbs.size(), o.limbs.size());
    std::vector<uint64_t> res(n + 1, 0);
    uint64_t carry = 0;
    for (size_t i = 0; i < n || carry; ++i) {
        uint64_t a = (i < limbs.size()) ? limbs[i] : 0;
        uint64_t b = (i < o.limbs.size()) ? o.limbs[i] : 0;
        // Use __uint128_t to avoid overflow detection complexity
        unsigned __int128 s = static_cast<unsigned __int128>(a) + b + carry;
        res[i] = static_cast<uint64_t>(s);
        carry = static_cast<uint64_t>(s >> 64);
    }
    return OmBigUInt(std::move(res));
}

OmBigUInt OmBigUInt::operator-(const OmBigUInt& o) const {
    // Assumes *this >= o
    std::vector<uint64_t> res(limbs.size(), 0);
    uint64_t borrow = 0;
    for (size_t i = 0; i < limbs.size(); ++i) {
        uint64_t b = (i < o.limbs.size()) ? o.limbs[i] : 0;
        unsigned __int128 sub = static_cast<unsigned __int128>(limbs[i]) - b - borrow;
        res[i] = static_cast<uint64_t>(sub);
        borrow = (sub >> 127) ? 1 : 0; // borrowed if underflowed
    }
    return OmBigUInt(std::move(res));
}

OmBigUInt OmBigUInt::operator*(const OmBigUInt& o) const {
    if (is_zero() || o.is_zero()) return OmBigUInt();
    const size_t n = limbs.size(), m = o.limbs.size();
    std::vector<uint64_t> res(n + m, 0);
    for (size_t i = 0; i < n; ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < m; ++j) {
            unsigned __int128 prod = static_cast<unsigned __int128>(limbs[i]) * o.limbs[j]
                                   + res[i + j] + carry;
            res[i + j] = static_cast<uint64_t>(prod);
            carry = static_cast<uint64_t>(prod >> 64);
        }
        res[i + m] += carry;
    }
    return OmBigUInt(std::move(res));
}

// divmod: Knuth Algorithm D (long division in base 2^64)
std::pair<OmBigUInt, OmBigUInt> OmBigUInt::divmod(const OmBigUInt& u, const OmBigUInt& v) {
    if (v.is_zero()) throw std::runtime_error("OmBigUInt: division by zero");
    if (u < v) return {OmBigUInt(), u};
    if (v.limbs.size() == 1) {
        // Short division
        uint64_t d = v.limbs[0];
        std::vector<uint64_t> q(u.limbs.size(), 0);
        unsigned __int128 rem = 0;
        for (size_t i = u.limbs.size(); i-- > 0;) {
            unsigned __int128 cur = (rem << 64) | u.limbs[i];
            q[i] = static_cast<uint64_t>(cur / d);
            rem = cur % d;
        }
        return {OmBigUInt(std::move(q)), OmBigUInt(static_cast<uint64_t>(rem))};
    }

    // Multi-limb Knuth D: normalize by left-shifting so high bit of v is set
    const size_t n = v.limbs.size();
    const size_t m = u.limbs.size() - n;

    // Normalize: shift left by `s` bits so top bit of divisor is 1
    int s = 0;
    {
        uint64_t top = v.limbs.back();
        while ((top & (uint64_t(1) << 63)) == 0) { ++s; top <<= 1; }
    }

    // Shift u and v left by s bits
    OmBigUInt vn = v << static_cast<size_t>(s);
    OmBigUInt un = u << static_cast<size_t>(s);
    // Ensure un has m+n+1 limbs
    un.limbs.resize(m + n + 1, 0);

    std::vector<uint64_t> q(m + 1, 0);

    for (size_t j = m + 1; j-- > 0;) {
        // Estimate q[j]
        unsigned __int128 num = (static_cast<unsigned __int128>(un.limbs[j + n]) << 64)
                              | un.limbs[j + n - 1];
        uint64_t vTop = vn.limbs[n - 1];
        unsigned __int128 qhat = num / vTop;
        unsigned __int128 rhat = num % vTop;

        // Refine qhat
        typedef unsigned __int128 u128;
        const u128 maxU64 = (u128(1) << 64);
        while (qhat >= maxU64 ||
               qhat * vn.limbs[n - 2] > ((rhat << 64) | (j + n >= 2 ? un.limbs[j + n - 2] : 0))) {
            --qhat;
            rhat += vTop;
            if (rhat >= maxU64) break;
        }

        // Multiply and subtract
        uint64_t msub_borrow = 0;
        for (size_t i = 0; i <= n; ++i) {
            unsigned __int128 vd = (i < vn.limbs.size()) ? vn.limbs[i] : 0;
            unsigned __int128 prod = qhat * vd + msub_borrow;
            uint64_t sub_val = static_cast<uint64_t>(prod);
            msub_borrow = static_cast<uint64_t>(prod >> 64);
            if (un.limbs[j + i] < sub_val) ++msub_borrow;
            un.limbs[j + i] -= sub_val;
        }

        q[j] = static_cast<uint64_t>(qhat);
        if (msub_borrow) {
            // qhat was one too large — add back
            --q[j];
            uint64_t carry = 0;
            for (size_t i = 0; i <= n; ++i) {
                uint64_t vd = (i < vn.limbs.size()) ? vn.limbs[i] : 0;
                unsigned __int128 s2 = static_cast<unsigned __int128>(un.limbs[j + i]) + vd + carry;
                un.limbs[j + i] = static_cast<uint64_t>(s2);
                carry = static_cast<uint64_t>(s2 >> 64);
            }
        }
    }

    // Remainder is un >> s
    OmBigUInt remainder = OmBigUInt(std::move(un)) >> static_cast<size_t>(s);
    return {OmBigUInt(std::move(q)), std::move(remainder)};
}

OmBigUInt OmBigUInt::operator/(const OmBigUInt& o) const {
    return divmod(*this, o).first;
}

OmBigUInt OmBigUInt::operator%(const OmBigUInt& o) const {
    return divmod(*this, o).second;
}

OmBigUInt OmBigUInt::operator&(const OmBigUInt& o) const {
    const size_t n = std::min(limbs.size(), o.limbs.size());
    std::vector<uint64_t> res(n);
    for (size_t i = 0; i < n; ++i) res[i] = limbs[i] & o.limbs[i];
    return OmBigUInt(std::move(res));
}

OmBigUInt OmBigUInt::operator|(const OmBigUInt& o) const {
    const size_t n = std::max(limbs.size(), o.limbs.size());
    std::vector<uint64_t> res(n, 0);
    for (size_t i = 0; i < n; ++i) {
        uint64_t a = (i < limbs.size()) ? limbs[i] : 0;
        uint64_t b = (i < o.limbs.size()) ? o.limbs[i] : 0;
        res[i] = a | b;
    }
    return OmBigUInt(std::move(res));
}

OmBigUInt OmBigUInt::operator^(const OmBigUInt& o) const {
    const size_t n = std::max(limbs.size(), o.limbs.size());
    std::vector<uint64_t> res(n, 0);
    for (size_t i = 0; i < n; ++i) {
        uint64_t a = (i < limbs.size()) ? limbs[i] : 0;
        uint64_t b = (i < o.limbs.size()) ? o.limbs[i] : 0;
        res[i] = a ^ b;
    }
    return OmBigUInt(std::move(res));
}

OmBigUInt OmBigUInt::operator<<(size_t n) const {
    if (is_zero() || n == 0) return *this;
    const size_t limb_shift = n / 64, bit_shift = n % 64;
    std::vector<uint64_t> res(limbs.size() + limb_shift + 1, 0);
    for (size_t i = 0; i < limbs.size(); ++i) {
        res[i + limb_shift] |= limbs[i] << bit_shift;
        if (bit_shift && i + limb_shift + 1 < res.size())
            res[i + limb_shift + 1] |= limbs[i] >> (64 - bit_shift);
    }
    return OmBigUInt(std::move(res));
}

OmBigUInt OmBigUInt::operator>>(size_t n) const {
    if (is_zero() || n == 0) return *this;
    const size_t limb_shift = n / 64, bit_shift = n % 64;
    if (limb_shift >= limbs.size()) return OmBigUInt();
    std::vector<uint64_t> res(limbs.size() - limb_shift, 0);
    for (size_t i = 0; i < res.size(); ++i) {
        res[i] = limbs[i + limb_shift] >> bit_shift;
        if (bit_shift && i + limb_shift + 1 < limbs.size())
            res[i] |= limbs[i + limb_shift + 1] << (64 - bit_shift);
    }
    return OmBigUInt(std::move(res));
}

OmBigUInt OmBigUInt::pow(const OmBigUInt& exp) const {
    if (exp.is_zero()) return OmBigUInt(1);
    OmBigUInt base(*this), result(1);
    OmBigUInt e = exp;
    while (!e.is_zero()) {
        if (e.test_bit(0)) result *= base;
        base *= base;
        e = e >> 1;
    }
    return result;
}

OmBigUInt OmBigUInt::gcd(const OmBigUInt& a, const OmBigUInt& b) {
    OmBigUInt x = a, y = b;
    while (!y.is_zero()) {
        OmBigUInt r = x % y;
        x = std::move(y);
        y = std::move(r);
    }
    return x;
}

std::string OmBigUInt::to_string(int base) const {
    if (is_zero()) return "0";
    if (base == 16) {
        std::string hex;
        for (size_t i = limbs.size(); i-- > 0;) {
            char buf[17];
            snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)limbs[i]);
            hex += buf;
        }
        // Strip leading zeros but keep at least one digit
        size_t start = hex.find_first_not_of('0');
        return start == std::string::npos ? "0" : hex.substr(start);
    }
    // Base 10: repeated division
    OmBigUInt ten(10ULL);
    std::string s;
    OmBigUInt n(*this);
    while (!n.is_zero()) {
        auto [q, r] = divmod(n, ten);
        s += static_cast<char>('0' + r.to_u64());
        n = std::move(q);
    }
    std::reverse(s.begin(), s.end());
    return s;
}

OmBigUInt OmBigUInt::from_string(const std::string& s, int base) {
    OmBigUInt result;
    OmBigUInt bv(static_cast<uint64_t>(base));
    for (char c : s) {
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else continue;
        result = result * bv + OmBigUInt(static_cast<uint64_t>(digit));
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// OmBigInt operations
// ─────────────────────────────────────────────────────────────────────────────

int OmBigInt::compare(const OmBigInt& o) const {
    if (neg != o.neg) return neg ? -1 : 1;
    int cmp = mag.compare(o.mag);
    return neg ? -cmp : cmp;
}

OmBigInt OmBigInt::operator+(const OmBigInt& o) const {
    if (neg == o.neg) {
        return OmBigInt(mag + o.mag, neg);
    }
    // Different signs: subtract magnitudes
    int cmp = mag.compare(o.mag);
    if (cmp == 0) return OmBigInt();
    if (cmp > 0) return OmBigInt(mag - o.mag, neg);
    return OmBigInt(o.mag - mag, o.neg);
}

OmBigInt OmBigInt::operator-(const OmBigInt& o) const {
    return *this + OmBigInt(o.mag, !o.neg || o.mag.is_zero());
}

OmBigInt OmBigInt::operator*(const OmBigInt& o) const {
    return OmBigInt(mag * o.mag, neg != o.neg);
}

OmBigInt OmBigInt::operator/(const OmBigInt& o) const {
    auto q = mag / o.mag;
    return OmBigInt(std::move(q), neg != o.neg);
}

OmBigInt OmBigInt::operator%(const OmBigInt& o) const {
    auto r = mag % o.mag;
    return OmBigInt(std::move(r), neg);
}

OmBigInt OmBigInt::pow(const OmBigInt& exp) const {
    if (exp.is_negative()) throw std::runtime_error("OmBigInt::pow: negative exponent");
    OmBigUInt result_mag = mag.pow(exp.mag);
    // Negative base ^ odd exponent = negative
    bool result_neg = neg && !exp.is_zero() && exp.mag.test_bit(0);
    return OmBigInt(std::move(result_mag), result_neg);
}

OmBigInt OmBigInt::gcd(const OmBigInt& a, const OmBigInt& b) {
    return OmBigInt(OmBigUInt::gcd(a.mag, b.mag), false);
}

int64_t OmBigInt::to_i64() const {
    int64_t v = static_cast<int64_t>(mag.to_u64());
    return neg ? -v : v;
}

std::string OmBigInt::to_string(int base) const {
    if (is_zero()) return "0";
    std::string s = mag.to_string(base);
    if (neg) s = "-" + s;
    return s;
}

OmBigInt OmBigInt::from_string(const std::string& s, int base) {
    if (s.empty()) return OmBigInt();
    bool negative = (s[0] == '-');
    std::string rest = negative ? s.substr(1) : s;
    OmBigUInt mag = OmBigUInt::from_string(rest, base);
    return OmBigInt(std::move(mag), negative);
}

} // namespace omscript
