#include "value.h"
#include <stdexcept>
#include <sstream>

namespace omscript {

bool Value::isTruthy() const {
    // Truthiness: numbers are true when non-zero, strings when non-empty, and none is false.
    switch (type) {
        case Type::INTEGER:
            return intValue != 0;
        case Type::FLOAT:
            return floatValue != 0.0;
        case Type::STRING:
            return !stringValue.empty();
        case Type::NONE:
            return false;
    }
    return false;
}

std::string Value::toString() const {
    switch (type) {
        case Type::INTEGER:
            return std::to_string(intValue);
        case Type::FLOAT: {
            std::ostringstream oss;
            oss << floatValue;
            return oss.str();
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
    if (type == Type::FLOAT || other.type == Type::FLOAT) {
        double a = (type == Type::FLOAT) ? floatValue : static_cast<double>(intValue);
        double b = (other.type == Type::FLOAT) ? other.floatValue : static_cast<double>(other.intValue);
        return Value(a + b);
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
    if (type == Type::FLOAT || other.type == Type::FLOAT) {
        double a = (type == Type::FLOAT) ? floatValue : static_cast<double>(intValue);
        double b = (other.type == Type::FLOAT) ? other.floatValue : static_cast<double>(other.intValue);
        return Value(a - b);
    }
    throw std::runtime_error("Invalid operands for -");
}

Value Value::operator*(const Value& other) const {
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        return Value(intValue * other.intValue);
    }
    if (type == Type::FLOAT || other.type == Type::FLOAT) {
        double a = (type == Type::FLOAT) ? floatValue : static_cast<double>(intValue);
        double b = (other.type == Type::FLOAT) ? other.floatValue : static_cast<double>(other.intValue);
        return Value(a * b);
    }
    throw std::runtime_error("Invalid operands for *");
}

Value Value::operator/(const Value& other) const {
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        if (other.intValue == 0) throw std::runtime_error("Division by zero");
        return Value(intValue / other.intValue);
    }
    if (type == Type::FLOAT || other.type == Type::FLOAT) {
        double a = (type == Type::FLOAT) ? floatValue : static_cast<double>(intValue);
        double b = (other.type == Type::FLOAT) ? other.floatValue : static_cast<double>(other.intValue);
        if (b == 0.0) throw std::runtime_error("Division by zero");
        return Value(a / b);
    }
    throw std::runtime_error("Invalid operands for /");
}

Value Value::operator%(const Value& other) const {
    if (type == Type::INTEGER && other.type == Type::INTEGER) {
        if (other.intValue == 0) throw std::runtime_error("Modulo by zero");
        return Value(intValue % other.intValue);
    }
    throw std::runtime_error("Invalid operands for %");
}

Value Value::operator-() const {
    if (type == Type::INTEGER) {
        return Value(-intValue);
    }
    if (type == Type::FLOAT) {
        return Value(-floatValue);
    }
    throw std::runtime_error("Invalid operand for unary -");
}

bool Value::operator==(const Value& other) const {
    // Allow numeric type coercion for equality
    if ((type == Type::INTEGER || type == Type::FLOAT) && 
        (other.type == Type::INTEGER || other.type == Type::FLOAT)) {
        double a = (type == Type::FLOAT) ? floatValue : static_cast<double>(intValue);
        double b = (other.type == Type::FLOAT) ? other.floatValue : static_cast<double>(other.intValue);
        return a == b;
    }
    
    if (type != other.type) return false;
    
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
    if (type == Type::FLOAT || other.type == Type::FLOAT) {
        double a = (type == Type::FLOAT) ? floatValue : static_cast<double>(intValue);
        double b = (other.type == Type::FLOAT) ? other.floatValue : static_cast<double>(other.intValue);
        return a < b;
    }
    if (type == Type::STRING && other.type == Type::STRING) {
        return stringValue < other.stringValue;
    }
    throw std::runtime_error("Invalid operands for <");
}

bool Value::operator<=(const Value& other) const {
    return *this < other || *this == other;
}

bool Value::operator>(const Value& other) const {
    return !(*this <= other);
}

bool Value::operator>=(const Value& other) const {
    return !(*this < other);
}

} // namespace omscript
