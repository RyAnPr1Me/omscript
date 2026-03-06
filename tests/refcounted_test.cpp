#include "refcounted.h"
#include <cstring>
#include <gtest/gtest.h>

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

// ===========================================================================
// RefCountedString::concat static helper
// ===========================================================================

TEST(RefCountedStringTest, ConcatStaticBothNonEmpty) {
    RefCountedString r = RefCountedString::concat("hello ", 6, "world", 5);
    EXPECT_STREQ(r.c_str(), "hello world");
    EXPECT_EQ(r.length(), 11u);
}

TEST(RefCountedStringTest, ConcatStaticFirstEmpty) {
    RefCountedString r = RefCountedString::concat("", 0, "world", 5);
    EXPECT_STREQ(r.c_str(), "world");
    EXPECT_EQ(r.length(), 5u);
}

TEST(RefCountedStringTest, ConcatStaticSecondEmpty) {
    RefCountedString r = RefCountedString::concat("hello", 5, "", 0);
    EXPECT_STREQ(r.c_str(), "hello");
    EXPECT_EQ(r.length(), 5u);
}

TEST(RefCountedStringTest, ConcatStaticBothEmpty) {
    RefCountedString r = RefCountedString::concat("", 0, "", 0);
    EXPECT_TRUE(r.empty());
    EXPECT_STREQ(r.c_str(), "");
}

TEST(RefCountedStringTest, ConcatStaticNullTerminated) {
    // Verify the result is properly null-terminated.
    RefCountedString r = RefCountedString::concat("abc", 3, "def", 3);
    EXPECT_EQ(r.length(), 6u);
    EXPECT_EQ(std::strlen(r.c_str()), 6u);
    EXPECT_STREQ(r.c_str(), "abcdef");
}

TEST(RefCountedStringTest, ConcatStaticIndependentFromOperands) {
    // Modifying the source buffers after concat must not corrupt the result.
    char buf1[] = "hello ";
    char buf2[] = "world";
    RefCountedString r = RefCountedString::concat(buf1, 6, buf2, 5);
    buf1[0] = 'X';
    buf2[0] = 'Y';
    EXPECT_STREQ(r.c_str(), "hello world");
}

// ===========================================================================
// Length-aware constructor (const char*, size_t)
// ===========================================================================

TEST(RefCountedStringTest, ConstructFromPtrAndLength) {
    RefCountedString s("hello world", 5); // only first 5 chars
    EXPECT_EQ(s.length(), 5u);
    EXPECT_STREQ(s.c_str(), "hello");
}

TEST(RefCountedStringTest, ConstructFromPtrAndLengthFull) {
    const char* src = "omscript";
    size_t len = std::strlen(src);
    RefCountedString s(src, len);
    EXPECT_EQ(s.length(), len);
    EXPECT_STREQ(s.c_str(), src);
}

TEST(RefCountedStringTest, ConstructFromPtrAndLengthZero) {
    RefCountedString s("non-empty", 0);
    EXPECT_TRUE(s.empty());
    EXPECT_STREQ(s.c_str(), "");
}

TEST(RefCountedStringTest, ConstructFromPtrAndLengthNullTerminated) {
    // Result must be null-terminated even though no '\0' is in the source slice.
    char buf[] = {'a', 'b', 'c', 'd', 'e'};
    RefCountedString s(buf, 3);
    EXPECT_EQ(s.length(), 3u);
    EXPECT_EQ(s.c_str()[3], '\0');
    EXPECT_STREQ(s.c_str(), "abc");
}

TEST(RefCountedStringTest, ConstructFromPtrAndLengthSharesOnCopy) {
    // Two copies of a length-constructed string should share the underlying data.
    RefCountedString a("hello", 5);
    RefCountedString b(a);
    EXPECT_STREQ(a.c_str(), "hello");
    EXPECT_STREQ(b.c_str(), "hello");
    EXPECT_EQ(a.c_str(), b.c_str()); // same pointer — shared data
}

// ===========================================================================
// Copy constructor is noexcept
// ===========================================================================

TEST(RefCountedStringTest, CopyConstructorIsNoexcept) {
    // Verify at compile time that the copy constructor is noexcept.
    static_assert(std::is_nothrow_copy_constructible<RefCountedString>::value,
                  "RefCountedString copy constructor must be noexcept");
    // Also exercise at runtime to ensure no unexpected throw occurs.
    RefCountedString a("noexcept test");
    RefCountedString b(a);
    EXPECT_STREQ(b.c_str(), "noexcept test");
}
