// funcptr_runtime.c — Runtime support for OmScript's `funcptr` type.
//
// `funcptr` is a special pointer that holds the address of executable machine
// code.  Dereferencing a funcptr (`*f`) calls the code at that address as a
// zero-argument function returning i64.
//
// omsc_funcptr_new(bytes, n):
//   Allocates a region of executable memory large enough to hold n bytes,
//   copies the provided machine-code bytes into it, marks it read+execute
//   only (W^X), and returns a pointer that can be called as a function.
//
// Platform support: Linux / macOS (mmap + mprotect), Windows (VirtualAlloc +
// VirtualProtect).  Both paths enforce W^X: the region is writable only
// during the initial memcpy, then downgraded to read+execute before returning.

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <string.h>
#endif
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/// Allocate executable memory, copy n bytes of machine code from `bytes` into
/// it, enforce W^X (downgrade to read+execute), and return a pointer to the
/// executable region.  Returns NULL on failure.
void* omsc_funcptr_new(const void* bytes, int64_t n) {
    if (!bytes || n <= 0) return (void*)0;

#if defined(_WIN32)
    /* Allocate as read+write first, copy, then switch to execute+read. */
    void* mem = VirtualAlloc(NULL, (size_t)n,
                             MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
    if (!mem) return (void*)0;
    memcpy(mem, bytes, (size_t)n);
    DWORD old;
    if (!VirtualProtect(mem, (size_t)n, PAGE_EXECUTE_READ, &old)) {
        VirtualFree(mem, 0, MEM_RELEASE);
        return (void*)0;
    }
    return mem;
#else
    /* Allocate as read+write, copy bytes, then mprotect to read+execute. */
    void* mem = mmap(NULL, (size_t)n,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return (void*)0;
    memcpy(mem, bytes, (size_t)n);
    if (mprotect(mem, (size_t)n, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, (size_t)n);
        return (void*)0;
    }
    return mem;
#endif
}

/// Allocate executable memory from an i64 array where each element holds one
/// byte of machine code (the low 8 bits of each i64 are used).  This is the
/// interface called by `funcptr_new(byte_array)` in OmScript, because
/// OmScript arrays store elements as i64 values.  Returns NULL on failure.
void* omsc_funcptr_new_arr(const int64_t* arr, int64_t n) {
    if (!arr || n <= 0) return (void*)0;
    /* Pack the low byte of each element into a temporary buffer. */
    unsigned char* tmp = (unsigned char*)malloc((size_t)n);
    if (!tmp) return (void*)0;
    for (int64_t i = 0; i < n; ++i)
        tmp[i] = (unsigned char)(arr[i] & 0xff);
    void* mem = omsc_funcptr_new(tmp, n);
    free(tmp);
    return mem;
}
