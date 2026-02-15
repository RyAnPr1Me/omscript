#include <gtest/gtest.h>
#include "refcounted.h"
#include <cstring>

using namespace omscript;

// ===========================================================================
// Construction
// ===========================================================================

TEST(RefCountedStringTest, DefaultConstruction) {
    RefCountedString s;
    EXPECT_TRUE(s.empty());
    EXPECT_EQ(s.length(), 0u);
    EXPECT_STREQ(s.c_str(), "");
}

TEST(RefCountedStringTest, ConstructFromCString) {
    RefCountedString s("hello");
    EXPECT_FALSE(s.empty());
    EXPECT_EQ(s.length(), 5u);
    EXPECT_STREQ(s.c_str(), "hello");
}

TEST(RefCountedStringTest, ConstructFromNullptr) {
    RefCountedString s(nullptr);
    EXPECT_TRUE(s.empty());
    EXPECT_STREQ(s.c_str(), "");
}

TEST(RefCountedStringTest, ConstructFromEmptyString) {
    RefCountedString s("");
    EXPECT_TRUE(s.empty());
    EXPECT_STREQ(s.c_str(), "");
}

// ===========================================================================
// Copy semantics
// ===========================================================================

TEST(RefCountedStringTest, CopyConstruction) {
    RefCountedString a("hello");
    RefCountedString b(a);
    EXPECT_STREQ(a.c_str(), "hello");
    EXPECT_STREQ(b.c_str(), "hello");
}

TEST(RefCountedStringTest, CopyAssignment) {
    RefCountedString a("hello");
    RefCountedString b;
    b = a;
    EXPECT_STREQ(a.c_str(), "hello");
    EXPECT_STREQ(b.c_str(), "hello");
}

TEST(RefCountedStringTest, SelfAssignment) {
    RefCountedString a("hello");
    a = a;
    EXPECT_STREQ(a.c_str(), "hello");
}

// ===========================================================================
// Move semantics
// ===========================================================================

TEST(RefCountedStringTest, MoveConstruction) {
    RefCountedString a("hello");
    RefCountedString b(std::move(a));
    EXPECT_STREQ(b.c_str(), "hello");
    // After move, 'a' should be empty
    EXPECT_TRUE(a.empty());
}

TEST(RefCountedStringTest, MoveAssignment) {
    RefCountedString a("hello");
    RefCountedString b;
    b = std::move(a);
    EXPECT_STREQ(b.c_str(), "hello");
    EXPECT_TRUE(a.empty());
}

// ===========================================================================
// Concatenation
// ===========================================================================

TEST(RefCountedStringTest, ConcatTwoStrings) {
    RefCountedString a("hello ");
    RefCountedString b("world");
    RefCountedString c = a + b;
    EXPECT_STREQ(c.c_str(), "hello world");
    EXPECT_EQ(c.length(), 11u);
}

TEST(RefCountedStringTest, ConcatWithEmpty) {
    RefCountedString a("hello");
    RefCountedString b;
    RefCountedString c = a + b;
    EXPECT_STREQ(c.c_str(), "hello");

    RefCountedString d = b + a;
    EXPECT_STREQ(d.c_str(), "hello");
}

TEST(RefCountedStringTest, ConcatBothEmpty) {
    RefCountedString a;
    RefCountedString b;
    RefCountedString c = a + b;
    EXPECT_TRUE(c.empty());
}

// ===========================================================================
// Comparison
// ===========================================================================

TEST(RefCountedStringTest, EqualitySameContent) {
    RefCountedString a("abc");
    RefCountedString b("abc");
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
}

TEST(RefCountedStringTest, EqualityDifferentContent) {
    RefCountedString a("abc");
    RefCountedString b("def");
    EXPECT_FALSE(a == b);
    EXPECT_TRUE(a != b);
}

TEST(RefCountedStringTest, EqualitySameObject) {
    RefCountedString a("hello");
    RefCountedString b(a); // Same underlying data (shared via copy)
    EXPECT_TRUE(a == b);
}

TEST(RefCountedStringTest, EqualityWithEmpty) {
    RefCountedString a("abc");
    RefCountedString b;
    EXPECT_FALSE(a == b);
    EXPECT_FALSE(b == a);
}

TEST(RefCountedStringTest, EqualityBothEmpty) {
    RefCountedString a;
    RefCountedString b;
    // Both null data -> same data pointer (nullptr) → early return true
    EXPECT_TRUE(a == b);
}

TEST(RefCountedStringTest, LessThan) {
    RefCountedString a("abc");
    RefCountedString b("abd");
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(b < a);
}

TEST(RefCountedStringTest, LessThanEqual) {
    RefCountedString a("abc");
    RefCountedString b("abc");
    EXPECT_FALSE(a < b);
    EXPECT_FALSE(b < a);
}

// ===========================================================================
// Reference counting (stress test)
// ===========================================================================

TEST(RefCountedStringTest, MultiCopySharing) {
    RefCountedString a("shared");
    RefCountedString b(a);
    RefCountedString c(a);
    RefCountedString d(b);
    EXPECT_STREQ(a.c_str(), "shared");
    EXPECT_STREQ(b.c_str(), "shared");
    EXPECT_STREQ(c.c_str(), "shared");
    EXPECT_STREQ(d.c_str(), "shared");
}

TEST(RefCountedStringTest, DestructionChain) {
    RefCountedString* a = new RefCountedString("test");
    RefCountedString b(*a);
    delete a;
    // b should still be valid
    EXPECT_STREQ(b.c_str(), "test");
}

TEST(RefCountedStringTest, ReassignReleasesOld) {
    RefCountedString a("first");
    RefCountedString b("second");
    a = b;
    EXPECT_STREQ(a.c_str(), "second");
}

TEST(RefCountedStringTest, DifferentLengths) {
    RefCountedString a("abc");
    RefCountedString b("abcd");
    EXPECT_FALSE(a == b);
}
