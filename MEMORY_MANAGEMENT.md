# Memory Management in OmScript

## Overview

OmScript implements **reference counting** for memory management of dynamic values, particularly strings. The system uses `malloc`/`free` for all heap allocations, ensuring minimal overhead and predictable memory behavior.

## Reference Counting Design

### Key Components

1. **RefCountedString Class** (`runtime/refcounted.h`)
   - Implements copy-on-write semantics
   - Uses `malloc` for string data allocation
   - Automatic reference count management
   - Zero-overhead for empty strings

2. **Value Class** (`runtime/value.h`)
   - Union-based storage for different types
   - Automatic ref count increment on copy
   - Automatic ref count decrement on destruction
   - Move semantics for zero-copy transfers

### Memory Layout

```
StringData structure (allocated with malloc):
┌─────────────┬────────┬──────────────┐
│  refCount   │ length │   chars...   │
│  (size_t)   │(size_t)│ (flexible)   │
└─────────────┴────────┴──────────────┘
```

### Reference Counting Rules

1. **Creation**: New strings start with refCount = 1
2. **Copy**: Copying a Value increments refCount
3. **Destruction**: Destroying a Value decrements refCount
4. **Deallocation**: When refCount reaches 0, memory is freed with `free()`

## Implementation Details

### RefCountedString

```cpp
class RefCountedString {
private:
    struct StringData {
        size_t refCount;  // Number of references
        size_t length;    // String length
        char chars[1];    // Flexible array for string data
    };
    
    StringData* data;  // Pointer to heap-allocated data
    
    // Allocate using malloc
    static StringData* allocate(size_t length) {
        size_t totalSize = sizeof(StringData) + length;
        StringData* sd = static_cast<StringData*>(std::malloc(totalSize));
        sd->refCount = 1;
        sd->length = length;
        return sd;
    }
    
    // Free when refCount reaches zero
    void release() {
        if (data && --data->refCount == 0) {
            std::free(data);
        }
    }
};
```

### Value Class Integration

The Value class uses placement new to construct RefCountedString in a union:

```cpp
class Value {
private:
    Type type;
    union {
        int64_t intValue;
        double floatValue;
        RefCountedString stringValue;  // Reference counted
    };
    
    // String constructor
    Value(const char* val) : type(Type::STRING) {
        new (&stringValue) RefCountedString(val);
    }
    
    // Destructor
    ~Value() {
        if (type == Type::STRING) {
            stringValue.~RefCountedString();  // Decrements refCount
        }
    }
};
```

## Memory Management Characteristics

### Advantages

1. **Deterministic Deallocation**: Objects freed immediately when last reference is removed
2. **No GC Pauses**: No stop-the-world garbage collection
3. **Cache-Friendly**: Reference counting is inline with data structure
4. **Minimal Overhead**: Only one pointer + refCount per string
5. **Copy-on-Write**: Shared strings until modification needed

### Performance

- **String Copy**: O(1) - just increment refCount
- **String Destruction**: O(1) - decrement refCount, free if zero
- **String Concatenation**: O(n+m) - allocates new string with malloc
- **Memory Overhead**: 16 bytes per unique string (refCount + length)

### Memory Safety

1. **No Dangling Pointers**: Reference counting ensures data lives as long as needed
2. **No Double-Free**: RefCount prevents freeing until all references are gone
3. **No Memory Leaks**: Automatic cleanup when refCount reaches zero
4. **Exception Safe**: RAII ensures cleanup even during exceptions

## Usage Examples

### Example 1: Automatic Memory Management

```omscript
fn example1() {
    var s1 = "Hello";        // malloc() called, refCount = 1
    var s2 = s1;             // refCount = 2 (shared)
    var s3 = s1 + " World";  // New malloc(), refCount = 1
    return 0;
}
// On function return:
// - s3 destroyed: refCount of "Hello World" -> 0, free() called
// - s2 destroyed: refCount of "Hello" -> 1
// - s1 destroyed: refCount of "Hello" -> 0, free() called
```

### Example 2: String Concatenation

```omscript
fn build_string() {
    var result = "";
    for (i in 0...10) {
        result = result + "x";  // Each iteration:
                                 // 1. Old result refCount--
                                 // 2. New string malloc()
                                 // 3. result = new string
    }
    return 0;
}
// All intermediate strings are freed as they're replaced
```

### Example 3: Reference Sharing

```omscript
fn share_string() {
    var original = "Shared String";  // malloc(), refCount = 1
    var copy1 = original;            // refCount = 2
    var copy2 = original;            // refCount = 3
    var copy3 = original;            // refCount = 4
    
    // Only ONE copy of "Shared String" in memory!
    return 0;
}
// All copies destroyed -> refCount goes 4->3->2->1->0, then free()
```

## Memory Debugging

To detect memory leaks during development, you can:

1. **Count allocations**: Track malloc/free calls
2. **Valgrind**: Run under valgrind for leak detection
3. **Address Sanitizer**: Compile with -fsanitize=address

Example valgrind usage:
```bash
valgrind --leak-check=full ./your_program
```

## Best Practices

### Do's ✓

1. **Pass by value** - Reference counting makes this efficient
2. **Return strings** - Move semantics avoid unnecessary copies
3. **Use string concatenation** - Optimized with reference counting
4. **Rely on automatic cleanup** - RAII handles everything

### Don'ts ✗

1. **Don't manually manage memory** - The runtime handles it
2. **Don't worry about string copies** - They're cheap (just refCount++)
3. **Don't prematurely optimize** - Profile first

## Future Enhancements

Potential improvements:

1. **Small String Optimization (SSO)**: Store short strings inline
2. **String Interning**: Deduplicate identical constant strings
3. **Lazy Copy-on-Write**: Only copy when modifying shared strings
4. **Memory Pooling**: Reduce malloc/free overhead for small strings
5. **Reference Count Saturation**: Prevent overflow for long-lived strings

## Technical Notes

### Why malloc/free?

1. **Control**: Full control over allocation strategy
2. **Predictability**: No hidden allocator behavior
3. **Interop**: Easy to integrate with C libraries
4. **Debugging**: Standard tools work (valgrind, asan)
5. **Portability**: Works everywhere

### Why Not Garbage Collection?

1. **Determinism**: Know exactly when memory is freed
2. **Real-time**: No GC pauses
3. **Simplicity**: Easier to implement and understand
4. **Efficiency**: No scanning overhead

### Why Reference Counting?

1. **Automatic**: No manual memory management
2. **Fast**: Constant-time operations
3. **Predictable**: Immediate deallocation
4. **Simple**: Easy to reason about

## Conclusion

OmScript's reference counting system provides:
- ✅ Automatic memory management
- ✅ Deterministic deallocation
- ✅ Minimal overhead (16 bytes + string data)
- ✅ Zero-copy for shared strings
- ✅ Exception-safe cleanup
- ✅ No garbage collection pauses

The system uses `malloc`/`free` exclusively for heap allocations, ensuring predictable and efficient memory behavior suitable for systems programming.
