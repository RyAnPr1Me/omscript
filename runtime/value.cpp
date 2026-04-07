#include "value.h"
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace omscript {

std::string Value::toString() const {
    switch (type) {
    case Type::INTEGER: {
        // Use snprintf (like the FLOAT case below) to avoid the extra
        // intermediate allocation that std::to_string performs internally
        // before constructing the returned std::string.
        char buf[32];
        int len = std::snprintf(buf, sizeof(buf), "%" PRId64, intValue);
        if (__builtin_expect(len > 0 && static_cast<size_t>(len) < sizeof(buf), 1))
            return std::string(buf, static_cast<size_t>(len));
        return std::to_string(intValue); // fallback (unreachable in practice)
    }
    case Type::FLOAT: {
        // Use snprintf instead of std::ostringstream to avoid dynamic memory
        // allocation overhead. The buffer is large enough for any double in
        // default '%g' formatting.
        char buf[32];
        int len = std::snprintf(buf, sizeof(buf), "%g", floatValue);
        if (len < 0 || static_cast<size_t>(len) >= sizeof(buf))
            return "<float>";
        return std::string(buf, static_cast<size_t>(len));
    }
    case Type::STRING:
        return std::string(stringValue.c_str(), stringValue.length());
    case Type::NONE:
        return "none";
    }
    return "";
}

std::pair<const char*, size_t> Value::toCStrBuf(char* buf, size_t bufSize) const noexcept {
    switch (type) {
    case Type::INTEGER: {
        int len = std::snprintf(buf, bufSize, "%" PRId64, intValue);
        if (__builtin_expect(len > 0 && static_cast<size_t>(len) < bufSize, 1))
            return {buf, static_cast<size_t>(len)};
        return {buf, 0}; // overflow guard (should never happen for int64_t)
    }
    case Type::FLOAT: {
        int len = std::snprintf(buf, bufSize, "%g", floatValue);
        if (__builtin_expect(len > 0 && static_cast<size_t>(len) < bufSize, 1))
            return {buf, static_cast<size_t>(len)};
        static constexpr char kFallback[] = "<float>";
        return {kFallback, sizeof(kFallback) - 1};
    }
    case Type::STRING:
        return {stringValue.c_str(), stringValue.length()};
    case Type::NONE:
        return {"none", 4};
    }
    return {"", 0};
}

Value Value::operator+(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return Value(intValue + other.intValue);
    }
    if (needsFloatPromotion(other)) {
        return Value(toDouble() + other.toDouble());
    }
    if (type == Type::STRING || other.type == Type::STRING) {
        // Build the result string first; allocation may throw std::bad_alloc.
        // Setting v.type before a successful construction would cause ~Value()
        // to call ~RefCountedString() on uninitialised memory during unwinding.
        RefCountedString result;
        if (type == Type::STRING && other.type == Type::STRING) {
            result = stringValue + other.stringValue;
        } else if (type == Type::STRING) {
            // Use toCStrBuf to avoid heap allocation: formats integer/float into
            // a stack buffer, then concat() builds the result in one allocation.
            char buf[32];
            auto [ptr, len] = other.toCStrBuf(buf, sizeof(buf));
            result = RefCountedString::concat(
                stringValue.c_str(), stringValue.length(),
                ptr, len);
        } else {
            char buf[32];
            auto [ptr, len] = toCStrBuf(buf, sizeof(buf));
            result = RefCountedString::concat(
                ptr, len,
                other.stringValue.c_str(), other.stringValue.length());
        }
        Value v;
        v.type = Type::STRING;
        new (&v.stringValue) RefCountedString(std::move(result));
        return v;
    }
    throw std::runtime_error("Invalid operands for +");
}

Value Value::operator-(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return Value(intValue - other.intValue);
    }
    if (needsFloatPromotion(other)) {
        return Value(toDouble() - other.toDouble());
    }
    throw std::runtime_error("Invalid operands for -");
}

Value Value::operator*(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return Value(intValue * other.intValue);
    }
    if (needsFloatPromotion(other)) {
        return Value(toDouble() * other.toDouble());
    }
    throw std::runtime_error("Invalid operands for *");
}

Value Value::operator/(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        if (__builtin_expect(other.intValue == 0, 0))
            throw std::runtime_error("Division by zero");
        // INT64_MIN / -1 overflows signed 64-bit; trap instead of UB.
        if (__builtin_expect(intValue == INT64_MIN && other.intValue == -1, 0))
            throw std::runtime_error("Integer overflow in division (INT64_MIN / -1)");
        return Value(intValue / other.intValue);
    }
    if (needsFloatPromotion(other)) {
        double b = other.toDouble();
        if (b == 0.0)
            throw std::runtime_error("Division by zero");
        return Value(toDouble() / b);
    }
    throw std::runtime_error("Invalid operands for /");
}

Value Value::operator%(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        if (__builtin_expect(other.intValue == 0, 0))
            throw std::runtime_error("Modulo by zero");
        // INT64_MIN % -1 is UB (overflow); mathematically the result is 0.
        if (__builtin_expect(intValue == INT64_MIN && other.intValue == -1, 0))
            return Value(static_cast<int64_t>(0));
        return Value(intValue % other.intValue);
    }
    throw std::runtime_error("Invalid operands for %");
}

Value Value::operator-() const {
    if (type == Type::INTEGER) {
        // Negating INT64_MIN overflows signed 64-bit; trap instead of UB.
        if (intValue == INT64_MIN)
            throw std::runtime_error("Integer overflow in negation (cannot negate INT64_MIN)");
        return Value(-intValue);
    }
    if (type == Type::FLOAT) {
        return Value(-floatValue);
    }
    throw std::runtime_error("Invalid operand for unary -");
}

bool Value::operator==(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return intValue == other.intValue;
    }
    // Allow numeric type coercion for equality
    if ((type == Type::INTEGER || type == Type::FLOAT) && (other.type == Type::INTEGER || other.type == Type::FLOAT)) {
        return toDouble() == other.toDouble();
    }

    if (type != other.type)
        return false;

    switch (type) {
    case Type::STRING:
        return stringValue == other.stringValue;
    case Type::NONE:
        return true;
    default:
        return false;
    }
}

bool Value::operator!=(const Value& other) const {
    return !(*this == other);
}

bool Value::operator<(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return intValue < other.intValue;
    }
    if (needsFloatPromotion(other)) {
        return toDouble() < other.toDouble();
    }
    if (type == Type::STRING && other.type == Type::STRING) {
        return stringValue < other.stringValue;
    }
    throw std::runtime_error("Invalid operands for <");
}

bool Value::operator<=(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return intValue <= other.intValue;
    }
    if (needsFloatPromotion(other)) {
        return toDouble() <= other.toDouble();
    }
    if (type == Type::STRING && other.type == Type::STRING) {
        return !(other.stringValue < stringValue);
    }
    throw std::runtime_error("Invalid operands for <=");
}

bool Value::operator>(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return intValue > other.intValue;
    }
    if (needsFloatPromotion(other)) {
        return toDouble() > other.toDouble();
    }
    if (type == Type::STRING && other.type == Type::STRING) {
        return other.stringValue < stringValue;
    }
    throw std::runtime_error("Invalid operands for >");
}

bool Value::operator>=(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return intValue >= other.intValue;
    }
    if (needsFloatPromotion(other)) {
        return toDouble() >= other.toDouble();
    }
    if (type == Type::STRING && other.type == Type::STRING) {
        return !(stringValue < other.stringValue);
    }
    throw std::runtime_error("Invalid operands for >=");
}

Value Value::operator&(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return Value(intValue & other.intValue);
    }
    throw std::runtime_error("Invalid operands for & (both must be integers)");
}

Value Value::operator|(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return Value(intValue | other.intValue);
    }
    throw std::runtime_error("Invalid operands for | (both must be integers)");
}

Value Value::operator^(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        return Value(intValue ^ other.intValue);
    }
    throw std::runtime_error("Invalid operands for ^ (both must be integers)");
}

Value Value::operator~() const {
    if (type == Type::INTEGER) {
        return Value(~intValue);
    }
    throw std::runtime_error("Invalid operand for ~ (must be integer)");
}

Value Value::operator<<(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        if (__builtin_expect(other.intValue < 0 || other.intValue >= 64, 0)) {
            throw std::runtime_error("Shift amount out of range (0-63)");
        }
        // Cast to unsigned before shifting to avoid C++17 UB on negative values.
        return Value(static_cast<int64_t>(static_cast<uint64_t>(intValue) << other.intValue));
    }
    throw std::runtime_error("Invalid operands for << (both must be integers)");
}

Value Value::operator>>(const Value& other) const {
    if (__builtin_expect(type == Type::INTEGER && other.type == Type::INTEGER, 1)) {
        if (__builtin_expect(other.intValue < 0 || other.intValue >= 64, 0)) {
            throw std::runtime_error("Shift amount out of range (0-63)");
        }
        // Logical right shift: fill high bits with 0 (unsigned semantics).
        // Cast to uint64_t so the shift is always logical regardless of sign.
        return Value(static_cast<int64_t>(static_cast<uint64_t>(intValue) >> other.intValue));
    }
    throw std::runtime_error("Invalid operands for >> (both must be integers)");
}

} // namespace omscript
