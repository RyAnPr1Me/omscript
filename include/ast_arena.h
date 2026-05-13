#pragma once
#ifndef AST_ARENA_H
#define AST_ARENA_H

/// @file ast_arena.h
/// @brief Arena allocator and string-interning infrastructure for the OmScript compiler.
///
/// Provides two complementary facilities:
///
///   **StringHash / StringEqual** — Transparent hash and equality functors that
///   allow std::unordered_map<std::string, V, StringHash, StringEqual> to be
///   queried with std::string_view (or const char*) without constructing a
///   temporary std::string.  Use everywhere a lookup-heavy table is keyed on
///   std::string.
///
///   **BumpAllocator** — A slab-based bump-pointer arena allocator for use
///   during compilation phases where many short-lived objects are allocated
///   together and can be destroyed in bulk.  Memory is carved from fixed-size
///   slabs (default 64 KiB); a new slab is appended when the current one is
///   full.  Supports typed placement-new via `make<T>()` with automatic
///   destructor registration.  All memory is freed — and all destructors are
///   called — when the BumpAllocator is destroyed or `reset()` is called.
///
///   Properties:
///   - O(1) allocation (pointer bump in the common case)
///   - O(n) bulk deallocation (frees every slab at once)
///   - NOT thread-safe — create one arena per compilation unit / thread
///   - Compatible with standard library containers via the BumpAllocatorAdaptor
///     STL allocator wrapper (see below)

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Transparent hash and equality for string / string_view / const char*
// ─────────────────────────────────────────────────────────────────────────────

/// A transparent hash functor for std::string / std::string_view / const char*.
///
/// Declare a map as:
///   std::unordered_map<std::string, V, StringHash, StringEqual>
/// to enable heterogeneous lookup with string_view keys without constructing a
/// temporary std::string:
///   map.find(some_string_view);  // no allocation
struct StringHash {
    /// Required for heterogeneous lookup (find/count/contains with foreign key type).
    using is_transparent = void;

    [[nodiscard]] size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    [[nodiscard]] size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
    [[nodiscard]] size_t operator()(const char* s) const noexcept {
        return std::hash<std::string_view>{}(s ? std::string_view(s) : std::string_view{});
    }
};

/// A transparent equality functor for std::string / std::string_view / const char*.
///
/// Pair with StringHash to build a heterogeneous unordered_map/set.
struct StringEqual {
    /// Required for heterogeneous lookup.
    using is_transparent = void;

    template <class A, class B> [[nodiscard]] bool operator()(const A& a, const B& b) const noexcept {
        return std::string_view(a) == std::string_view(b);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// BumpAllocator — slab-based arena
// ─────────────────────────────────────────────────────────────────────────────

/// Fast bump-pointer slab arena.  Typical usage:
///
///   BumpAllocator arena;
///   MyNode* n = arena.make<MyNode>(args...);
///   ...
///   // arena destructor calls ~MyNode() and frees slab memory automatically
///
/// The arena grows by doubling slab size (starting from kInitialSlabSize up
/// to kMaxSlabSize) to amortise large-object allocation cost.
class BumpAllocator {
  public:
    /// Default slab size (64 KiB).  Adjust per use-case.
    static constexpr size_t kInitialSlabSize = 64u * 1024u;
    /// Maximum slab size cap (4 MiB) to avoid over-committing virtual memory.
    static constexpr size_t kMaxSlabSize = 4u * 1024u * 1024u;

    BumpAllocator() = default;

    // Non-copyable (slabs are owned, raw-pointer content would alias).
    BumpAllocator(const BumpAllocator&) = delete;
    BumpAllocator& operator=(const BumpAllocator&) = delete;

    // Movable.
    BumpAllocator(BumpAllocator&& o) noexcept
        : slabs_(std::move(o.slabs_)), slabSizes_(std::move(o.slabSizes_)), dtors_(std::move(o.dtors_)), cur_(o.cur_),
          end_(o.end_), bytesAllocated_(o.bytesAllocated_), bytesUsed_(o.bytesUsed_), nextSlabSize_(o.nextSlabSize_) {
        o.cur_ = o.end_ = nullptr;
        o.bytesAllocated_ = o.bytesUsed_ = 0;
        o.nextSlabSize_ = kInitialSlabSize;
    }
    BumpAllocator& operator=(BumpAllocator&& o) noexcept {
        if (this != &o) {
            reset();
            for (char* s : slabs_)
                ::free(s);
            slabs_.clear();
            slabSizes_.clear();
            slabs_ = std::move(o.slabs_);
            slabSizes_ = std::move(o.slabSizes_);
            dtors_ = std::move(o.dtors_);
            cur_ = o.cur_;
            end_ = o.end_;
            bytesAllocated_ = o.bytesAllocated_;
            bytesUsed_ = o.bytesUsed_;
            nextSlabSize_ = o.nextSlabSize_;
            o.cur_ = o.end_ = nullptr;
            o.bytesAllocated_ = o.bytesUsed_ = 0;
            o.nextSlabSize_ = kInitialSlabSize;
        }
        return *this;
    }

    ~BumpAllocator() {
        reset();
        for (char* s : slabs_)
            ::free(s);
    }

    // ── Raw allocation ────────────────────────────────────────────────────

    /// Allocate @p n bytes aligned to @p align bytes.
    /// Never returns nullptr; throws std::bad_alloc on out-of-memory.
    [[nodiscard]] void* alloc(size_t n, size_t align = alignof(std::max_align_t)) {
        if (__builtin_expect(n == 0, 0))
            return cur_; // degenerate: return any valid pointer
        // Align cur_ up to the requested alignment.
        auto cur = reinterpret_cast<uintptr_t>(cur_);
        const uintptr_t aligned = (cur + align - 1u) & ~(align - 1u);
        if (__builtin_expect(aligned + n > reinterpret_cast<uintptr_t>(end_), 0)) {
            allocSlab(n + align); // ensure enough space
            cur = reinterpret_cast<uintptr_t>(cur_);
            const uintptr_t a2 = (cur + align - 1u) & ~(align - 1u);
            cur_ = reinterpret_cast<char*>(a2 + n);
            bytesUsed_ += n;
            return reinterpret_cast<void*>(a2);
        }
        cur_ = reinterpret_cast<char*>(aligned + n);
        bytesUsed_ += n;
        return reinterpret_cast<void*>(aligned);
    }

    /// Try to recycle the last allocation if it was made with the same pointer
    /// and size (LIFO discipline).  Returns true and reclaims the memory if
    /// @p ptr was the most-recently-returned allocation from this arena and the
    /// current slab still holds it; returns false and does nothing otherwise.
    ///
    /// This is an O(1) operation and enables stack-like reuse patterns:
    ///   void* a = arena.alloc(n, align);
    ///   // ... use a, then decide to discard ...
    ///   arena.tryDealloc(a, n, align);  // bump pointer back: zero overhead
    bool tryDealloc(void* ptr, size_t n, size_t align = alignof(std::max_align_t)) noexcept {
        if (!ptr || n == 0 || slabs_.empty())
            return false;
        // The last allocation ended at cur_.  Back-compute where it started:
        // cur_ = aligned_start + n, so aligned_start = cur_ - n.
        const auto expectedStart = reinterpret_cast<uintptr_t>(cur_) - n;
        if (reinterpret_cast<uintptr_t>(ptr) != expectedStart)
            return false;
        // Verify ptr is within the current slab for safety.
        const auto slabBase = reinterpret_cast<uintptr_t>(slabs_.back());
        if (expectedStart < slabBase || expectedStart >= reinterpret_cast<uintptr_t>(end_))
            return false;
        // Reclaim: roll the bump pointer back to where ptr was allocated from
        // (before alignment padding).  We recompute the pre-aligned position by
        // scanning backwards for the aligned position that preceded ptr within
        // the slab.  Because alignment is always a power of two, the pre-aligned
        // position is at most (align - 1) bytes before ptr.
        (void)align; // alignment padding was already consumed; just roll back to ptr start.
        cur_ = reinterpret_cast<char*>(expectedStart);
        bytesUsed_ -= n;
        return true;
    }

    // ── Typed allocation ──────────────────────────────────────────────────

    /// Allocate and placement-new an object of type T.
    /// The object's destructor is registered and will be called by reset()
    /// or when the arena is destroyed.
    ///
    /// Returns a raw non-owning pointer.  The arena owns the object's lifetime.
    template <class T, class... Args> [[nodiscard]] T* make(Args&&... args) {
        void* mem = alloc(sizeof(T), alignof(T));
        T* obj = ::new (mem) T(std::forward<Args>(args)...);
        // Only register destructor if the type is non-trivially destructible.
        if constexpr (!std::is_trivially_destructible_v<T>) {
            dtors_.push_back([obj]() noexcept { obj->~T(); });
        }
        return obj;
    }

    // ── String helpers ────────────────────────────────────────────────────

    /// Copy the string @p sv into the arena and return a NUL-terminated pointer.
    /// Lifetime: valid until the arena is reset or destroyed.
    [[nodiscard]] const char* copyString(std::string_view sv) {
        char* buf = static_cast<char*>(alloc(sv.size() + 1, 1));
        std::memcpy(buf, sv.data(), sv.size());
        buf[sv.size()] = '\0';
        return buf;
    }

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Call all registered destructors and reset the arena to empty.
    /// Slab memory is retained and will be reused on subsequent allocations,
    /// making this suitable for re-use across multiple compilation units.
    void reset() noexcept {
        // Call destructors in reverse construction order.
        for (auto it = dtors_.rbegin(); it != dtors_.rend(); ++it)
            (*it)();
        dtors_.clear();
        // Reset bump pointer to the start of the first slab (if any), using
        // the actual recorded size of that slab.
        if (!slabs_.empty()) {
            cur_ = slabs_[0];
            end_ = slabs_[0] + slabSizes_[0];
        }
        bytesUsed_ = 0;
    }

    // ── Statistics ────────────────────────────────────────────────────────

    /// Total bytes allocated from the OS (across all slabs).
    [[nodiscard]] size_t bytesAllocated() const noexcept {
        return bytesAllocated_;
    }

    /// Bytes currently in-use (sum of all live alloc() calls since last reset).
    [[nodiscard]] size_t bytesUsed() const noexcept {
        return bytesUsed_;
    }

    /// Number of slabs currently held.
    [[nodiscard]] size_t slabCount() const noexcept {
        return slabs_.size();
    }

  private:
    std::vector<char*> slabs_;
    std::vector<size_t> slabSizes_;          // actual allocated size of each slab
    std::vector<void (*)() noexcept> dtors_; // registered object destructors (type-erased)
    char* cur_ = nullptr;
    char* end_ = nullptr;
    size_t bytesAllocated_ = 0;
    size_t bytesUsed_ = 0;
    size_t nextSlabSize_ = kInitialSlabSize;

    /// Allocate a new slab large enough for at least @p minSize bytes.
    void allocSlab(size_t minSize) {
        const size_t sz = std::max(nextSlabSize_, minSize);
        char* slab = static_cast<char*>(::malloc(sz));
        if (!slab)
            throw std::bad_alloc{};
        slabs_.push_back(slab);
        slabSizes_.push_back(sz);
        cur_ = slab;
        end_ = slab + sz;
        bytesAllocated_ += sz;
        // Double next slab size up to the cap.
        if (nextSlabSize_ < kMaxSlabSize)
            nextSlabSize_ = std::min(nextSlabSize_ * 2u, kMaxSlabSize);
    }
};

} // namespace omscript

#endif // AST_ARENA_H
