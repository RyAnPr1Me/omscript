OmScript Reference Counting Memory Management
==============================================

MEMORY LAYOUT
-------------

Value (stack allocated):
┌──────────────────┐
│ Type: STRING     │
├──────────────────┤
│ data pointer ────┼──────┐
└──────────────────┘      │
                          │
                          ▼
StringData (heap allocated with malloc):
┌─────────────┬─────────┬──────────────────┐
│  refCount   │ length  │    chars...      │
│   (size_t)  │(size_t) │ (flexible array) │
└─────────────┴─────────┴──────────────────┘
  16 bytes total metadata + actual string data


REFERENCE COUNTING LIFECYCLE
-----------------------------

1. Creation:
   var s1 = "Hello";
   
   ┌──────┐
   │ s1   │──┐
   └──────┘  │
             ▼
   ┌─────────────────────────────┐
   │ refCount = 1 │ "Hello"      │
   └─────────────────────────────┘
   
2. Copy (sharing):
   var s2 = s1;
   
   ┌──────┐      ┌──────┐
   │ s1   │──┐   │ s2   │──┐
   └──────┘  │   └──────┘  │
             └──────┬───────┘
                    ▼
   ┌─────────────────────────────┐
   │ refCount = 2 │ "Hello"      │  (same malloc!)
   └─────────────────────────────┘
   
3. Concatenation:
   var s3 = s1 + " World";
   
   ┌──────┐      ┌──────┐       ┌──────┐
   │ s1   │──┐   │ s2   │──┐    │ s3   │──┐
   └──────┘  │   └──────┘  │    └──────┘  │
             └──────┬───────┘              │
                    ▼                      ▼
   ┌─────────────────────────┐   ┌──────────────────────────┐
   │ refCount = 2│ "Hello"   │   │ refCount = 1│ "Hello Wo  │
   └─────────────────────────┘   │              rld"        │
                                 └──────────────────────────┘
                                 (new malloc allocation)

4. Destruction:
   } // end of scope
   
   s3 destroyed:
   ┌──────────────────────────┐
   │ refCount = 0│ "Hello Wo  │ ← free() called!
   │              rld"        │
   └──────────────────────────┘
   
   s2 destroyed:
   ┌─────────────────────────┐
   │ refCount = 1│ "Hello"   │
   └─────────────────────────┘
   
   s1 destroyed:
   ┌─────────────────────────┐
   │ refCount = 0│ "Hello"   │ ← free() called!
   └─────────────────────────┘


OPERATIONS AND MEMORY EFFECTS
-------------------------------

Operation          | malloc calls | free calls | RefCount change
-------------------|--------------|------------|----------------
var s = "text"     | 1            | 0          | +1 (create)
var s2 = s         | 0            | 0          | +1 (share)
s = "other"        | 1            | 1          | +1 new, -1 old
var s3 = s1 + s2   | 1            | 0          | +1 (new string)
} (end scope)      | 0            | N          | -N refs


EFFICIENCY EXAMPLES
-------------------

Example 1: String Sharing
--------------------------
fn share_example() {
    var original = "Shared";     // 1 malloc
    var c1 = original;           // 0 malloc (share!)
    var c2 = original;           // 0 malloc (share!)
    var c3 = original;           // 0 malloc (share!)
    var c4 = original;           // 0 malloc (share!)
    // Total: 1 malloc for 5 variables!
}

Example 2: String Building
---------------------------
fn build_example() {
    var result = "";
    for (i in 0...100) {
        result = result + "x";   // 100 mallocs
    }                             // 99 frees (intermediate)
}                                 // 1 final free

Example 3: Empty Strings
-------------------------
var empty = "";                   // 0 malloc (nullptr)
var empty2 = "";                  // 0 malloc (nullptr)
var empty3 = empty + empty2;      // 0 malloc (nullptr)


MEMORY OVERHEAD ANALYSIS
------------------------

Scenario              | Memory Used
----------------------|----------------------------------------
Empty string          | 8 bytes (pointer = nullptr)
"A" (1 char)          | 8 + 16 + 2 = 26 bytes (ptr + meta + char + null)
"Hello" (5 chars)     | 8 + 16 + 6 = 30 bytes
100 char string       | 8 + 16 + 101 = 125 bytes
Shared string (10x)   | Same as above! (just 10 pointers)

Compare to std::string (typically):
- Empty: 24-32 bytes (SSO buffer)
- "Hello": 32 bytes (SSO)
- 100 chars: ~132 bytes
- Shared: 10 × full size = massive overhead!


PERFORMANCE CHARACTERISTICS
----------------------------

O(1) Operations:
- String copy (increment refCount)
- String destroy (decrement refCount)
- Empty string operations

O(n) Operations:
- String creation (malloc + memcpy)
- String concatenation (malloc + memcpy)

O(1) Memory:
- Overhead per unique string: 16 bytes
- Overhead per reference: 8 bytes (pointer)


THREAD SAFETY NOTES
-------------------

Current implementation:
- NOT thread-safe (refCount is size_t, not atomic)
- Fine for single-threaded runtime

For multi-threading:
- Replace size_t refCount with std::atomic<size_t>
- Add memory barriers
- Minimal performance impact


DEBUGGING
---------

To check for memory leaks:

1. Valgrind:
   valgrind --leak-check=full ./your_program

2. Address Sanitizer:
   g++ -fsanitize=address program.cpp
   ./a.out

3. Custom tracking:
   Add static counters to malloc/free wrappers:
   - Track alloc_count++
   - Track free_count++
   - Verify alloc_count == free_count at exit
