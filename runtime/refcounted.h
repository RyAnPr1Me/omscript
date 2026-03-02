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
// Thread safety: reference count operations use std::atomic for safe
// concurrent retain/release across threads.
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

    // Copy constructor - increment reference count
    RefCountedString(const RefCountedString& other) : data(other.data) {
        retain();
    }

    // Move constructor - transfer ownership
    RefCountedString(RefCountedString&& other) noexcept : data(other.data) {
        other.data = nullptr;
    }

    // Destructor - decrement reference count
    ~RefCountedString() {
        release();
    }

    // Copy assignment
    RefCountedString& operator=(const RefCountedString& other) {
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
    const char* c_str() const {
        return data ? data->chars : "";
    }

    // Get length
    size_t length() const {
        return data ? data->length : 0;
    }

    // Check if empty
    bool empty() const {
        return data == nullptr || data->length == 0;
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

    // Comparison
    bool operator==(const RefCountedString& other) const {
        if (data == other.data)
            return true;
        if (!data || !other.data)
            return false;
        if (data->length != other.data->length)
            return false;
        return std::memcmp(data->chars, other.data->chars, data->length) == 0;
    }

    bool operator!=(const RefCountedString& other) const {
        return !(*this == other);
    }

    bool operator<(const RefCountedString& other) const {
        const char* s1 = c_str();
        const char* s2 = other.c_str();
        return std::strcmp(s1, s2) < 0;
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
    void retain() {
        if (data) {
            data->refCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Decrement reference count and free if zero
    void release() {
        if (data) {
            if (data->refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::free(data);
                data = nullptr;
            }
        }
    }
};

} // namespace omscript

#endif // REFCOUNTED_H
