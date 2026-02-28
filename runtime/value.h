#ifndef VALUE_H
#define VALUE_H

#include "refcounted.h"
#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>

namespace omscript {

class Value {
  public:
    enum class Type { INTEGER, FLOAT, STRING, NONE };

    Value() : type(Type::NONE), intValue(0) {}
    Value(int64_t val) : type(Type::INTEGER), intValue(val) {}
    Value(double val) : type(Type::FLOAT), floatValue(val) {}

    Value(const std::string& val) : type(Type::STRING) {
        new (&stringValue) RefCountedString(val.c_str());
    }

    Value(const char* val) : type(Type::STRING) {
        new (&stringValue) RefCountedString(val);
    }

    // Copy constructor
    Value(const Value& other) : type(other.type) {
        switch (type) {
        case Type::INTEGER:
            intValue = other.intValue;
            break;
        case Type::FLOAT:
            floatValue = other.floatValue;
            break;
        case Type::STRING:
            new (&stringValue) RefCountedString(other.stringValue);
            break;
        case Type::NONE:
            intValue = 0;
            break;
        }
    }

    // Move constructor
    Value(Value&& other) noexcept : type(other.type) {
        switch (type) {
        case Type::INTEGER:
            intValue = other.intValue;
            break;
        case Type::FLOAT:
            floatValue = other.floatValue;
            break;
        case Type::STRING:
            new (&stringValue) RefCountedString(std::move(other.stringValue));
            other.stringValue.~RefCountedString();
            other.type = Type::NONE;
            other.intValue = 0;
            break;
        case Type::NONE:
            intValue = 0;
            break;
        }
    }

    // Destructor
    ~Value() {
        if (type == Type::STRING) {
            stringValue.~RefCountedString();
        }
    }

    // Copy assignment
    Value& operator=(const Value& other) {
        if (this != &other) {
            if (type == Type::STRING) {
                stringValue.~RefCountedString();
            }
            type = other.type;
            switch (type) {
            case Type::INTEGER:
                intValue = other.intValue;
                break;
            case Type::FLOAT:
                floatValue = other.floatValue;
                break;
            case Type::STRING:
                new (&stringValue) RefCountedString(other.stringValue);
                break;
            case Type::NONE:
                intValue = 0;
                break;
            }
        }
        return *this;
    }

    // Move assignment
    Value& operator=(Value&& other) noexcept {
        if (this != &other) {
            if (type == Type::STRING) {
                stringValue.~RefCountedString();
            }
            type = other.type;
            switch (type) {
            case Type::INTEGER:
                intValue = other.intValue;
                break;
            case Type::FLOAT:
                floatValue = other.floatValue;
                break;
            case Type::STRING:
                new (&stringValue) RefCountedString(std::move(other.stringValue));
                other.stringValue.~RefCountedString();
                other.type = Type::NONE;
                other.intValue = 0;
                break;
            case Type::NONE:
                intValue = 0;
                break;
            }
        }
        return *this;
    }

    Type getType() const {
        return type;
    }

    int64_t asInt() const {
        if (type != Type::INTEGER) {
            throw std::runtime_error("Value is not an integer");
        }
        return intValue;
    }
    double asFloat() const {
        if (type != Type::FLOAT) {
            throw std::runtime_error("Value is not a float");
        }
        return floatValue;
    }
    const char* asString() const {
        if (type != Type::STRING) {
            throw std::runtime_error("Value is not a string");
        }
        return stringValue.c_str();
    }

    // Unchecked accessors for performance-critical paths (VM dispatch loop,
    // JIT call interface).  Caller must guarantee the type matches.
    int64_t unsafeAsInt() const {
        return intValue;
    }
    double unsafeAsFloat() const {
        return floatValue;
    }

    bool isTruthy() const {
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
    std::string toString() const;

    // Arithmetic operations
    Value operator+(const Value& other) const;
    Value operator-(const Value& other) const;
    Value operator*(const Value& other) const;
    Value operator/(const Value& other) const;
    Value operator%(const Value& other) const;
    Value operator-() const;

    // Comparison operations
    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const;
    bool operator<(const Value& other) const;
    bool operator<=(const Value& other) const;
    bool operator>(const Value& other) const;
    bool operator>=(const Value& other) const;

    // Bitwise operations
    Value operator&(const Value& other) const;
    Value operator|(const Value& other) const;
    Value operator^(const Value& other) const;
    Value operator~() const;
    Value operator<<(const Value& other) const;
    Value operator>>(const Value& other) const;

  private:
    Type type;
    union {
        int64_t intValue;
        double floatValue;
        RefCountedString stringValue;
    };

    /// Return true when both operands are numeric (int or float) and at least
    /// one is float, indicating that integer-to-float promotion is needed.
    bool needsFloatPromotion(const Value& other) const {
        bool thisNumeric = (type == Type::INTEGER || type == Type::FLOAT);
        bool otherNumeric = (other.type == Type::INTEGER || other.type == Type::FLOAT);
        bool hasFloat = (type == Type::FLOAT || other.type == Type::FLOAT);
        return thisNumeric && otherNumeric && hasFloat;
    }

    /// Convert this value to double, assuming it is either INTEGER or FLOAT.
    double toDouble() const {
        return (type == Type::FLOAT) ? floatValue : static_cast<double>(intValue);
    }
};

} // namespace omscript

#endif // VALUE_H
