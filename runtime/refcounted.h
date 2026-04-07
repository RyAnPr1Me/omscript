#pragma once

#ifndef REFCOUNTED_H
#define REFCOUNTED_H

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace omscript {

// Reference counted string implementation using malloc/free.
//
// Thread safety model:
//   - The reference count is managed via std::atomic, making retain() and
//     release() safe to call from concurrent threads.
//   - Data access (c_str(), length(), operator+, comparison operators) is
//     safe as long as the caller holds a live reference (i.e., the object
//     has not been destroyed).  Callers must ensure a shared RefCountedString
//     is not destroyed while another thread accesses its contents.
//   - This is the same model as std::shared_ptr: the control block is
//     thread-safe; the managed object must be externally synchronized.
class RefCountedString {
  public:
    RefCountedString() : data(nullptr) {}

    // Create from C string
    explicit RefCountedString(const char* str) {
        if (str && str[0] != '\0') {
            size_t len = std::strlen(str);
            data = allocate(len);
            std::memcpy(data->chars, str, len);
            data->chars[len] = '\0';
        } else {
            data = nullptr;
        }
    }

    // Create from C string with known length — avoids strlen when the
    // caller already has the length (e.g. from std::string::size()).
    RefCountedString(const char* str, size_t len) {
        if (str && len > 0) {
            data = allocate(len);
            std::memcpy(data->chars, str, len);
            data->chars[len] = '\0';
        } else {
            data = nullptr;
        }
    }

    // Copy constructor - increment reference count
    // noexcept: retain() only does an atomic increment, which cannot throw.
    RefCountedString(const RefCountedString& other) noexcept : data(other.data) {
        retain();
    }

    // Move constructor - transfer ownership
    RefCountedString(RefCountedString&& other) noexcept : data(other.data) {
        other.data = nullptr;
    }

    // Destructor - decrement reference count
    ~RefCountedString() noexcept {
        release();
    }

    // Copy assignment
    RefCountedString& operator=(const RefCountedString& other) noexcept {
        if (this != &other) {
            release();
            data = other.data;
            retain();
        }
        return *this;
    }

    // Move assignment
    RefCountedString& operator=(RefCountedString&& other) noexcept {
        if (this != &other) {
            release();
            data = other.data;
            other.data = nullptr;
        }
        return *this;
    }

    // Get C string
    [[nodiscard]] const char* c_str() const {
        return __builtin_expect(data != nullptr, 1) ? data->chars : "";
    }

    // Get length
    [[nodiscard]] size_t length() const {
        return __builtin_expect(data != nullptr, 1) ? data->length : 0;
    }

    // Check if empty
    [[nodiscard]] bool empty() const {
        return __builtin_expect(data == nullptr, 0) || data->length == 0;
    }

    // Concatenation
    RefCountedString operator+(const RefCountedString& other) const {
        if (!data)
            return other;
        if (!other.data)
            return *this;

        size_t newLen = data->length + other.data->length;
        RefCountedString result;
        result.data = allocate(newLen);

        std::memcpy(result.data->chars, data->chars, data->length);
        std::memcpy(result.data->chars + data->length, other.data->chars, other.data->length);
        result.data->chars[newLen] = '\0';

        return result;
    }

    /// Concatenate two C string segments into a new RefCountedString using a
    /// single allocation.  This is more efficient than constructing a temporary
    /// RefCountedString for one operand and then calling operator+, which would
    /// require two allocations.  Intended for mixed-type string concatenation
    /// where one operand must first be converted to a C string (e.g. via
    /// Value::toString()), so the length is already known from the std::string.
    ///
    /// Both @p a and @p b must be valid non-null pointers (use "" for empty).
    static RefCountedString concat(const char* a, size_t lenA, const char* b, size_t lenB) {
        if (lenA == 0 && lenB == 0)
            return RefCountedString();
        size_t newLen = lenA + lenB;
        RefCountedString result;
        result.data = allocate(newLen);
        std::memcpy(result.data->chars, a, lenA);
        std::memcpy(result.data->chars + lenA, b, lenB);
        result.data->chars[newLen] = '\0';
        return result;
    }

    // Comparison
    [[nodiscard]] bool operator==(const RefCountedString& other) const {
        if (data == other.data)
            return true;
        if (!data || !other.data)
            return false;
        if (data->length != other.data->length)
            return false;
        return std::memcmp(data->chars, other.data->chars, data->length) == 0;
    }

    [[nodiscard]] bool operator!=(const RefCountedString& other) const {
        return !(*this == other);
    }

    [[nodiscard]] bool operator<(const RefCountedString& other) const {
        // Same underlying data pointer → strings are identical → not less-than.
        // Mirrors the operator== fast path and avoids an unnecessary memcmp when
        // comparing a string to a copy of itself (common in sorted containers).
        if (data == other.data)
            return false;
        size_t len1 = length();
        size_t len2 = other.length();
        size_t minLen = len1 < len2 ? len1 : len2;
        if (minLen > 0) {
            int cmp = std::memcmp(c_str(), other.c_str(), minLen);
            if (cmp != 0)
                return cmp < 0;
        }
        return len1 < len2;
    }

  private:
    struct StringData {
        std::atomic<size_t> refCount;
        size_t length;
        char chars[1]; // Flexible array member
    };

    StringData* data;

    // Allocate string data using malloc
    static StringData* allocate(size_t length) {
        size_t totalSize = sizeof(StringData) +
                           length; // sizeof includes 1 char; total storage = length + 1 bytes (content + terminator)
        auto* sd = static_cast<StringData*>(std::malloc(totalSize));
        if (!sd) {
            throw std::bad_alloc();
        }
        // Relaxed ordering is safe here: the object has not been shared
        // with other threads yet (single-threaded construction context).
        sd->refCount.store(1, std::memory_order_relaxed);
        sd->length = length;
        return sd;
    }

    // Increment reference count
    void retain() noexcept {
        if (__builtin_expect(data != nullptr, 1)) {
            data->refCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Decrement reference count and free if zero
    void release() noexcept {
        if (__builtin_expect(data != nullptr, 1)) {
            // Use release ordering on the decrement so all prior accesses to
            // the object's data are visible to whichever thread observes the
            // reference count drop to zero.
            // An acquire fence is inserted only on the final decrement (when
            // the count was 1 and now becomes 0).  This is cheaper than
            // acq_rel on every decrement: on x86/TSO both are equivalent, but
            // on ARM/AArch64 the acquire barrier is a full dmb ish — skipping
            // it on non-final decrements avoids the barrier on the common path.
            if (__builtin_expect(data->refCount.fetch_sub(1, std::memory_order_release) == 1, 0)) {
                // Synchronize with all prior release-decrements so we see every
                // write to the object before we free it.
                std::atomic_thread_fence(std::memory_order_acquire);
                std::free(data);
                data = nullptr;
            }
        }
    }
};

} // namespace omscript

#endif // REFCOUNTED_H
