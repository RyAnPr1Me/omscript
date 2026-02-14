#include <gtest/gtest.h>
#include "value.h"

using namespace omscript;

// ===========================================================================
// Construction & type
// ===========================================================================

TEST(ValueTest, DefaultIsNone) {
    Value v;
    EXPECT_EQ(v.getType(), Value::Type::NONE);
}

TEST(ValueTest, IntegerConstruction) {
    Value v(int64_t(42));
    EXPECT_EQ(v.getType(), Value::Type::INTEGER);
    EXPECT_EQ(v.asInt(), 42);
}

TEST(ValueTest, FloatConstruction) {
    Value v(3.14);
    EXPECT_EQ(v.getType(), Value::Type::FLOAT);
    EXPECT_DOUBLE_EQ(v.asFloat(), 3.14);
}

TEST(ValueTest, StringFromStdString) {
    Value v(std::string("hello"));
    EXPECT_EQ(v.getType(), Value::Type::STRING);
    EXPECT_STREQ(v.asString(), "hello");
}

TEST(ValueTest, StringFromCString) {
    Value v("world");
    EXPECT_EQ(v.getType(), Value::Type::STRING);
    EXPECT_STREQ(v.asString(), "world");
}

// ===========================================================================
// Type errors on wrong accessor
// ===========================================================================

TEST(ValueTest, AsIntThrowsForFloat) {
    Value v(1.0);
    EXPECT_THROW(v.asInt(), std::runtime_error);
}

TEST(ValueTest, AsFloatThrowsForInt) {
    Value v(int64_t(1));
    EXPECT_THROW(v.asFloat(), std::runtime_error);
}

TEST(ValueTest, AsStringThrowsForInt) {
    Value v(int64_t(1));
    EXPECT_THROW(v.asString(), std::runtime_error);
}

// ===========================================================================
// Truthiness
// ===========================================================================

TEST(ValueTest, TruthyInteger) {
    EXPECT_TRUE(Value(int64_t(1)).isTruthy());
    EXPECT_TRUE(Value(int64_t(-1)).isTruthy());
    EXPECT_FALSE(Value(int64_t(0)).isTruthy());
}

TEST(ValueTest, TruthyFloat) {
    EXPECT_TRUE(Value(1.0).isTruthy());
    EXPECT_FALSE(Value(0.0).isTruthy());
}

TEST(ValueTest, TruthyString) {
    EXPECT_TRUE(Value("hello").isTruthy());
    EXPECT_FALSE(Value("").isTruthy());
}

TEST(ValueTest, TruthyNone) {
    EXPECT_FALSE(Value().isTruthy());
}

// ===========================================================================
// toString
// ===========================================================================

TEST(ValueTest, ToStringInt) {
    EXPECT_EQ(Value(int64_t(42)).toString(), "42");
}

TEST(ValueTest, ToStringFloat) {
    // Just check it produces something non-empty
    std::string s = Value(3.14).toString();
    EXPECT_FALSE(s.empty());
}

TEST(ValueTest, ToStringString) {
    EXPECT_EQ(Value("abc").toString(), "abc");
}

TEST(ValueTest, ToStringNone) {
    EXPECT_EQ(Value().toString(), "none");
}

// ===========================================================================
// Arithmetic: addition
// ===========================================================================

TEST(ValueTest, AddIntInt) {
    Value result = Value(int64_t(2)) + Value(int64_t(3));
    EXPECT_EQ(result.asInt(), 5);
}

TEST(ValueTest, AddFloatFloat) {
    Value result = Value(1.5) + Value(2.5);
    EXPECT_DOUBLE_EQ(result.asFloat(), 4.0);
}

TEST(ValueTest, AddIntFloat) {
    Value result = Value(int64_t(1)) + Value(2.5);
    EXPECT_DOUBLE_EQ(result.asFloat(), 3.5);
}

TEST(ValueTest, AddFloatInt) {
    Value result = Value(1.5) + Value(int64_t(2));
    EXPECT_DOUBLE_EQ(result.asFloat(), 3.5);
}

TEST(ValueTest, AddStringString) {
    Value result = Value("hello ") + Value("world");
    EXPECT_STREQ(result.asString(), "hello world");
}

TEST(ValueTest, AddStringInt) {
    Value result = Value("num: ") + Value(int64_t(42));
    // String + non-string uses toString
    EXPECT_EQ(result.getType(), Value::Type::STRING);
}

TEST(ValueTest, AddIntString) {
    Value result = Value(int64_t(42)) + Value(" is the answer");
    EXPECT_EQ(result.getType(), Value::Type::STRING);
}

TEST(ValueTest, AddInvalidNone) {
    EXPECT_THROW(Value() + Value(int64_t(1)), std::runtime_error);
}

// ===========================================================================
// Arithmetic: subtraction
// ===========================================================================

TEST(ValueTest, SubIntInt) {
    Value result = Value(int64_t(5)) - Value(int64_t(3));
    EXPECT_EQ(result.asInt(), 2);
}

TEST(ValueTest, SubFloatFloat) {
    Value result = Value(5.0) - Value(3.0);
    EXPECT_DOUBLE_EQ(result.asFloat(), 2.0);
}

TEST(ValueTest, SubIntFloat) {
    Value result = Value(int64_t(5)) - Value(3.0);
    EXPECT_DOUBLE_EQ(result.asFloat(), 2.0);
}

TEST(ValueTest, SubInvalid) {
    EXPECT_THROW(Value("a") - Value("b"), std::runtime_error);
}

// ===========================================================================
// Arithmetic: multiplication
// ===========================================================================

TEST(ValueTest, MulIntInt) {
    Value result = Value(int64_t(3)) * Value(int64_t(4));
    EXPECT_EQ(result.asInt(), 12);
}

TEST(ValueTest, MulFloatFloat) {
    Value result = Value(2.0) * Value(3.0);
    EXPECT_DOUBLE_EQ(result.asFloat(), 6.0);
}

TEST(ValueTest, MulIntFloat) {
    Value result = Value(int64_t(2)) * Value(3.5);
    EXPECT_DOUBLE_EQ(result.asFloat(), 7.0);
}

TEST(ValueTest, MulInvalid) {
    EXPECT_THROW(Value("a") * Value("b"), std::runtime_error);
}

// ===========================================================================
// Arithmetic: division
// ===========================================================================

TEST(ValueTest, DivIntInt) {
    Value result = Value(int64_t(10)) / Value(int64_t(3));
    EXPECT_EQ(result.asInt(), 3);
}

TEST(ValueTest, DivFloatFloat) {
    Value result = Value(10.0) / Value(4.0);
    EXPECT_DOUBLE_EQ(result.asFloat(), 2.5);
}

TEST(ValueTest, DivByZeroInt) {
    EXPECT_THROW(Value(int64_t(1)) / Value(int64_t(0)), std::runtime_error);
}

TEST(ValueTest, DivByZeroFloat) {
    EXPECT_THROW(Value(1.0) / Value(0.0), std::runtime_error);
}

TEST(ValueTest, DivInvalid) {
    EXPECT_THROW(Value("a") / Value("b"), std::runtime_error);
}

// ===========================================================================
// Arithmetic: modulo
// ===========================================================================

TEST(ValueTest, ModIntInt) {
    Value result = Value(int64_t(10)) % Value(int64_t(3));
    EXPECT_EQ(result.asInt(), 1);
}

TEST(ValueTest, ModByZero) {
    EXPECT_THROW(Value(int64_t(10)) % Value(int64_t(0)), std::runtime_error);
}

TEST(ValueTest, ModInvalidFloat) {
    EXPECT_THROW(Value(1.0) % Value(2.0), std::runtime_error);
}

// ===========================================================================
// Unary negation
// ===========================================================================

TEST(ValueTest, NegateInt) {
    Value result = -Value(int64_t(5));
    EXPECT_EQ(result.asInt(), -5);
}

TEST(ValueTest, NegateFloat) {
    Value result = -Value(3.14);
    EXPECT_DOUBLE_EQ(result.asFloat(), -3.14);
}

TEST(ValueTest, NegateInvalid) {
    EXPECT_THROW(-Value("hello"), std::runtime_error);
}

// ===========================================================================
// Equality
// ===========================================================================

TEST(ValueTest, EqIntInt) {
    EXPECT_TRUE(Value(int64_t(1)) == Value(int64_t(1)));
    EXPECT_FALSE(Value(int64_t(1)) == Value(int64_t(2)));
}

TEST(ValueTest, EqIntFloat) {
    EXPECT_TRUE(Value(int64_t(1)) == Value(1.0));
    EXPECT_FALSE(Value(int64_t(1)) == Value(1.5));
}

TEST(ValueTest, EqFloatFloat) {
    EXPECT_TRUE(Value(1.0) == Value(1.0));
    EXPECT_FALSE(Value(1.0) == Value(2.0));
}

TEST(ValueTest, EqStringString) {
    EXPECT_TRUE(Value("abc") == Value("abc"));
    EXPECT_FALSE(Value("abc") == Value("def"));
}

TEST(ValueTest, EqNoneNone) {
    EXPECT_TRUE(Value() == Value());
}

TEST(ValueTest, EqDifferentTypes) {
    EXPECT_FALSE(Value("1") == Value(int64_t(1)));
    EXPECT_FALSE(Value() == Value(int64_t(0)));
}

TEST(ValueTest, NeqBasic) {
    EXPECT_TRUE(Value(int64_t(1)) != Value(int64_t(2)));
    EXPECT_FALSE(Value(int64_t(1)) != Value(int64_t(1)));
}

// ===========================================================================
// Ordering
// ===========================================================================

TEST(ValueTest, LessIntInt) {
    EXPECT_TRUE(Value(int64_t(1)) < Value(int64_t(2)));
    EXPECT_FALSE(Value(int64_t(2)) < Value(int64_t(1)));
}

TEST(ValueTest, LessFloatFloat) {
    EXPECT_TRUE(Value(1.0) < Value(2.0));
    EXPECT_FALSE(Value(2.0) < Value(1.0));
}

TEST(ValueTest, LessIntFloat) {
    EXPECT_TRUE(Value(int64_t(1)) < Value(2.0));
}

TEST(ValueTest, LessStringString) {
    EXPECT_TRUE(Value("abc") < Value("abd"));
    EXPECT_FALSE(Value("abd") < Value("abc"));
}

TEST(ValueTest, LessInvalid) {
    EXPECT_THROW(Value("a") < Value(int64_t(1)), std::runtime_error);
}

TEST(ValueTest, LessOrEqual) {
    EXPECT_TRUE(Value(int64_t(1)) <= Value(int64_t(1)));
    EXPECT_TRUE(Value(int64_t(1)) <= Value(int64_t(2)));
    EXPECT_FALSE(Value(int64_t(2)) <= Value(int64_t(1)));
}

TEST(ValueTest, Greater) {
    EXPECT_TRUE(Value(int64_t(2)) > Value(int64_t(1)));
    EXPECT_FALSE(Value(int64_t(1)) > Value(int64_t(2)));
}

TEST(ValueTest, GreaterOrEqual) {
    EXPECT_TRUE(Value(int64_t(2)) >= Value(int64_t(1)));
    EXPECT_TRUE(Value(int64_t(2)) >= Value(int64_t(2)));
    EXPECT_FALSE(Value(int64_t(1)) >= Value(int64_t(2)));
}

// ===========================================================================
// Copy and move semantics
// ===========================================================================

TEST(ValueTest, CopyConstructor) {
    Value orig("hello");
    Value copy(orig);
    EXPECT_STREQ(copy.asString(), "hello");
    EXPECT_STREQ(orig.asString(), "hello");
}

TEST(ValueTest, MoveConstructor) {
    Value orig("hello");
    Value moved(std::move(orig));
    EXPECT_STREQ(moved.asString(), "hello");
}

TEST(ValueTest, CopyAssignment) {
    Value a("hello");
    Value b;
    b = a;
    EXPECT_STREQ(b.asString(), "hello");
    EXPECT_STREQ(a.asString(), "hello");
}

TEST(ValueTest, MoveAssignment) {
    Value a("hello");
    Value b;
    b = std::move(a);
    EXPECT_STREQ(b.asString(), "hello");
}

TEST(ValueTest, SelfAssignment) {
    Value v("hello");
    v = v;
    EXPECT_STREQ(v.asString(), "hello");
}

TEST(ValueTest, CopyIntToString) {
    Value a(int64_t(42));
    Value b("hello");
    b = a;
    EXPECT_EQ(b.getType(), Value::Type::INTEGER);
    EXPECT_EQ(b.asInt(), 42);
}

TEST(ValueTest, CopyStringToInt) {
    Value a("hello");
    Value b(int64_t(42));
    b = a;
    EXPECT_EQ(b.getType(), Value::Type::STRING);
    EXPECT_STREQ(b.asString(), "hello");
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST(ValueTest, NegativeInt) {
    Value v(int64_t(-42));
    EXPECT_EQ(v.asInt(), -42);
}

TEST(ValueTest, ZeroFloat) {
    Value v(0.0);
    EXPECT_DOUBLE_EQ(v.asFloat(), 0.0);
}

TEST(ValueTest, EmptyStringValue) {
    Value v(std::string(""));
    EXPECT_EQ(v.getType(), Value::Type::STRING);
    // Empty string should result in null data in RefCountedString
    EXPECT_STREQ(v.asString(), "");
}
