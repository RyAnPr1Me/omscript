#ifndef VALUE_H
#define VALUE_H

#include <string>
#include <variant>

namespace omscript {

class Value {
public:
    enum class Type {
        INTEGER,
        FLOAT,
        STRING,
        NONE
    };
    
    Value() : type(Type::NONE), intValue(0) {}
    Value(int64_t val) : type(Type::INTEGER), intValue(val) {}
    Value(double val) : type(Type::FLOAT), floatValue(val) {}
    Value(const std::string& val) : type(Type::STRING), stringValue(val), intValue(0) {}
    
    Type getType() const { return type; }
    
    int64_t asInt() const { return intValue; }
    double asFloat() const { return floatValue; }
    const std::string& asString() const { return stringValue; }
    
    bool isTruthy() const;
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
    
private:
    Type type;
    union {
        int64_t intValue;
        double floatValue;
    };
    std::string stringValue;
};

} // namespace omscript

#endif // VALUE_H
