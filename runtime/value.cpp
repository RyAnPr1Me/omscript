#include "value.h"
#include <stdexcept>
#include <sstream>

namespace omscript {

bool Value::isTruthy() const {
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
        case Type::FLOAT:
            return std::to_string(floatValue);
        case Type::STRING:
            return stringValue;
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
        return Value(toString() + other.toString());
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
    if (type != other.type) return false;
    
    switch (type) {
        case Type::INTEGER:
            return intValue == other.intValue;
        case Type::FLOAT:
            return floatValue == other.floatValue;
        case Type::STRING:
            return stringValue == other.stringValue;
        case Type::NONE:
            return true;
    }
    return false;
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
