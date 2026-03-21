#pragma once

#ifndef VALUE_H
#define VALUE_H

#include "refcounted.h"
#include <cstdint>
#include <stdexcept>
#include <string>

namespace omscript {

class Value {
  public:
    enum class Type { INTEGER, FLOAT, STRING, NONE };

    Value() noexcept : type(Type::NONE), intValue(0) {}
    Value(int64_t val) noexcept : type(Type::INTEGER), intValue(val) {}
    Value(double val) noexcept : type(Type::FLOAT), floatValue(val) {}

    Value(const std::string& val) : type(Type::STRING) {
        // Use the length-aware constructor to avoid a redundant strlen call
        // since std::string already knows its size.
        new (&stringValue) RefCountedString(val.data(), val.size());
    }

    Value(const char* val) : type(Type::STRING) {
        new (&stringValue) RefCountedString(val);
    }

    // Copy constructor
    // noexcept: RefCountedString copy ctor is noexcept (retain() is noexcept).
    Value(const Value& other) noexcept : type(other.type) {
        if (__builtin_expect(type != Type::STRING, 1)) {
            // Fast path: POD copy covers INTEGER, FLOAT, and NONE without branching.
            intValue = other.intValue;
        } else {
            new (&stringValue) RefCountedString(other.stringValue);
        }
    }

    // Move constructor
    Value(Value&& other) noexcept : type(other.type) {
        if (__builtin_expect(type != Type::STRING, 1)) {
            intValue = other.intValue;
        } else {
            new (&stringValue) RefCountedString(std::move(other.stringValue));
            other.stringValue.~RefCountedString();
            other.type = Type::NONE;
            other.intValue = 0;
        }
    }

    // Destructor
    ~Value() noexcept {
        if (__builtin_expect(type == Type::STRING, 0)) {
            stringValue.~RefCountedString();
        }
    }

    // Copy assignment
    // noexcept: RefCountedString copy ctor is noexcept (retain() only does an
    // atomic increment), so the placement-new below cannot throw.  Marking this
    // noexcept enables std::vector<Value> to use the copy constructor during
    // reallocation when no move constructor is available, and lets the compiler
    // omit exception-handling overhead throughout the assignment body.
    Value& operator=(const Value& other) noexcept {
        if (__builtin_expect(this != &other, 1)) {
            if (__builtin_expect(type == Type::STRING, 0)) {
                stringValue.~RefCountedString();
            }
            // Since RefCountedString's copy constructor is noexcept, set the
            // type directly rather than using a temporary Type::NONE guard.
            type = other.type;
            if (__builtin_expect(type != Type::STRING, 1)) {
                intValue = other.intValue;
            } else {
                new (&stringValue) RefCountedString(other.stringValue);
            }
        }
        return *this;
    }

    // Move assignment
    Value& operator=(Value&& other) noexcept {
        if (__builtin_expect(this != &other, 1)) {
            if (__builtin_expect(type == Type::STRING, 0)) {
                stringValue.~RefCountedString();
            }
            type = other.type;
            if (__builtin_expect(type != Type::STRING, 1)) {
                intValue = other.intValue;
            } else {
                new (&stringValue) RefCountedString(std::move(other.stringValue));
                other.stringValue.~RefCountedString();
                other.type = Type::NONE;
                other.intValue = 0;
            }
        }
        return *this;
    }

    [[nodiscard]] Type getType() const {
        return type;
    }

    [[nodiscard]] int64_t asInt() const {
        if (__builtin_expect(type != Type::INTEGER, 0)) {
            throw std::runtime_error("Value is not an integer");
        }
        return intValue;
    }
    [[nodiscard]] double asFloat() const {
        if (__builtin_expect(type != Type::FLOAT, 0)) {
            throw std::runtime_error("Value is not a float");
        }
        return floatValue;
    }
    [[nodiscard]] const char* asString() const {
        if (__builtin_expect(type != Type::STRING, 0)) {
            throw std::runtime_error("Value is not a string");
        }
        return stringValue.c_str();
    }

    // Unchecked accessors for performance-critical paths.
    // Caller must guarantee the type matches.
    [[nodiscard]] int64_t unsafeAsInt() const {
        return intValue;
    }
    [[nodiscard]] double unsafeAsFloat() const {
        return floatValue;
    }

    [[nodiscard]] bool isTruthy() const {
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
    [[nodiscard]] std::string toString() const;

    // Arithmetic operations
    [[nodiscard]] Value operator+(const Value& other) const;
    [[nodiscard]] Value operator-(const Value& other) const;
    [[nodiscard]] Value operator*(const Value& other) const;
    [[nodiscard]] Value operator/(const Value& other) const;
    [[nodiscard]] Value operator%(const Value& other) const;
    [[nodiscard]] Value operator-() const;

    // Comparison operations
    [[nodiscard]] bool operator==(const Value& other) const;
    [[nodiscard]] bool operator!=(const Value& other) const;
    [[nodiscard]] bool operator<(const Value& other) const;
    [[nodiscard]] bool operator<=(const Value& other) const;
    [[nodiscard]] bool operator>(const Value& other) const;
    [[nodiscard]] bool operator>=(const Value& other) const;

    // Bitwise operations
    [[nodiscard]] Value operator&(const Value& other) const;
    [[nodiscard]] Value operator|(const Value& other) const;
    [[nodiscard]] Value operator^(const Value& other) const;
    [[nodiscard]] Value operator~() const;
    [[nodiscard]] Value operator<<(const Value& other) const;
    [[nodiscard]] Value operator>>(const Value& other) const;

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

    /// Write a string representation of this value into a caller-supplied stack
    /// buffer without heap allocation, and return a (pointer, length) pair.
    ///
    ///   INTEGER → decimal digits written into @p buf via snprintf
    ///   FLOAT   → "%g" formatted into @p buf via snprintf
    ///   STRING  → returns the existing c_str() / length() — no copy at all
    ///   NONE    → returns the literal "none"
    ///
    /// @p buf must be at least 32 bytes.  The returned pointer is valid until
    /// @p buf goes out of scope or this Value is modified.
    std::pair<const char*, size_t> toCStrBuf(char* buf, size_t bufSize) const noexcept;
};

} // namespace omscript

#endif // VALUE_H
