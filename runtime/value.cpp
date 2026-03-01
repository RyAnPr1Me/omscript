#include "value.h"
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace omscript {

std::string Value::toString() const {
    switch (type) {
    case Type::INTEGER:
        return std::to_string(intValue);
    case Type::FLOAT: {
        // Use snprintf instead of std::ostringstream to avoid dynamic memory
        // allocation overhead. The buffer is large enough for any double in
        // default '%g' formatting.
        char buf[32];
        int len = std::snprintf(buf, sizeof(buf), "%g", floatValue);
        return std::string(buf, static_cast<size_t>(len));
    }
    case Type::STRING:
        return std::string(stringValue.c_str());
    case Type::NONE:
        return "none";
    }
    return "";
}

Value Value::operator+(const Value& other) const {
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        return Value(intValue + other.intValue);
    }
    if (needsFloatPromotion(other)) {
        return Value(toDouble() + other.toDouble());
    }
    if (type == Type::STRING || other.type == Type::STRING) {
        // String concatenation using reference counted strings
        RefCountedString result;
        if (type == Type::STRING && other.type == Type::STRING) {
            result = stringValue + other.stringValue;
        } else if (type == Type::STRING) {
            RefCountedString otherStr(other.toString().c_str());
            result = stringValue + otherStr;
        } else {
            RefCountedString thisStr(toString().c_str());
            result = thisStr + other.stringValue;
        }
        Value v;
        v.type = Type::STRING;
        new (&v.stringValue) RefCountedString(std::move(result));
        return v;
    }
    throw std::runtime_error("Invalid operands for +");
}

Value Value::operator-(const Value& other) const {
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        return Value(intValue - other.intValue);
    }
    if (needsFloatPromotion(other)) {
        return Value(toDouble() - other.toDouble());
    }
    throw std::runtime_error("Invalid operands for -");
}

Value Value::operator*(const Value& other) const {
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        return Value(intValue * other.intValue);
    }
    if (needsFloatPromotion(other)) {
        return Value(toDouble() * other.toDouble());
    }
    throw std::runtime_error("Invalid operands for *");
}

Value Value::operator/(const Value& other) const {
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        if (other.intValue == 0)
            throw std::runtime_error("Division by zero");
        // INT64_MIN / -1 overflows signed 64-bit; trap instead of UB.
        if (intValue == INT64_MIN && other.intValue == -1)
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
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        if (other.intValue == 0)
            throw std::runtime_error("Modulo by zero");
        // INT64_MIN % -1 is UB (overflow); mathematically the result is 0.
        if (intValue == INT64_MIN && other.intValue == -1)
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
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
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
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
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
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
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
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
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
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
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
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        return Value(intValue & other.intValue);
    }
    throw std::runtime_error("Invalid operands for & (both must be integers)");
}

Value Value::operator|(const Value& other) const {
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        return Value(intValue | other.intValue);
    }
    throw std::runtime_error("Invalid operands for | (both must be integers)");
}

Value Value::operator^(const Value& other) const {
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
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
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        if (other.intValue < 0 || other.intValue >= 64) {
            throw std::runtime_error("Shift amount out of range (0-63)");
        }
        return Value(intValue << other.intValue);
    }
    throw std::runtime_error("Invalid operands for << (both must be integers)");
}

Value Value::operator>>(const Value& other) const {
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        if (other.intValue < 0 || other.intValue >= 64) {
            throw std::runtime_error("Shift amount out of range (0-63)");
        }
        return Value(intValue >> other.intValue);
    }
    throw std::runtime_error("Invalid operands for >> (both must be integers)");
}

} // namespace omscript
