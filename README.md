# OmScript

A low-level, C-like programming language with dynamic typing and **automatic reference counting memory management**. Features a **heavily optimized AOT compiler** using LLVM and a **lightweight adaptive JIT runtime** that recompiles hot functions with even more aggressive optimizations.

## Key Features

- **C-like Syntax**: Familiar syntax for C programmers
- **Dynamic Typing**: Variables are dynamically typed, no explicit type declarations needed
- **Aggressive AOT Compilation**: Multi-level LLVM optimization (O0/O1/O2/O3) for maximum performance
- **Reference Counted Memory**: Automatic memory management using malloc/free with deterministic deallocation
- **Lambda Expressions**: Anonymous functions with `|x| x * 2` syntax for use with higher-order builtins
- **Pipe Operator**: Left-to-right function chaining with `expr |> fn`
- **Spread Operator**: Array unpacking in literals with `[1, ...arr, 2]`
- **For Loops with Ranges**: Modern range-based iteration with `for (i in start...end...step)`
- **For-Each Loops**: Iterate over arrays with `for (x in array)`
- **Switch/Case**: Multi-way branching with `switch`/`case`/`default`
- **Do-While Loops**: Execute body at least once with `do { ... } while (cond);`
- **69 Built-in Functions**: Math, array manipulation, strings, character classification, type conversion, system, and I/O
- **Error Handling**: `try`/`catch`/`throw` for structured error handling
- **Enum Declarations**: Named integer constants with auto-increment
- **Default Parameters**: Optional function parameters with default values
- **Null Coalescing Operator**: `??` for concise null/zero fallback expressions
- **Multi-line Strings**: Triple-quoted `"""..."""` strings with embedded newlines
- **Adaptive JIT Runtime**: Hot functions are automatically recompiled at higher optimization levels using runtime profiling data

## Optimization Levels

OmScript supports multiple optimization levels for maximum performance:

- **O0**: No optimization (fastest compilation)
- **O1**: Basic optimizations (instruction combining, reassociation, CFG simplification)
- **O2**: Moderate optimizations (default)
  - Memory-to-register promotion (mem2reg)
  - Global Value Numbering (GVN)
  - Dead Code Elimination (DCE)
  - Instruction combining and reassociation
  - CFG simplification
- **O3**: Aggressive optimizations
  - All O2 optimizations plus:
  - Loop Invariant Code Motion (LICM)
  - Loop simplification and canonicalization
  - Loop unrolling
  - Tail call elimination
  - Early Common Subexpression Elimination (CSE)
  - Scalar Replacement of Aggregates (SROA)

## Memory Management

OmScript uses **reference counting** for automatic memory management:

- **malloc/free based**: All heap allocations use standard C memory functions
- **Automatic cleanup**: Reference counting ensures deterministic deallocation
- **Zero-copy**: Shared strings through reference counting (copy-on-write)
- **No GC pauses**: Deterministic memory management without stop-the-world collection
- **Minimal overhead**: Only 16 bytes per unique string + data

See [MEMORY_MANAGEMENT.md](MEMORY_MANAGEMENT.md) for detailed documentation.

## Language Syntax

### Functions
```omscript
fn functionName(param1, param2) {
    // function body
    return value;
}

// Default parameters
fn greet(name, greeting = "Hello") {
    println(greeting);
    println(name);
    return 0;
}
```
Functions support forward references — a function can call another function defined later in the file. The compiler performs a two-pass approach: first collecting all function declarations, then generating code. Recursive and mutually recursive calls are also supported.

### Variables
```omscript
var x = 10;           // mutable variable
const y = 20;         // constant (immutable)
var z: int = 30;      // optional type annotation
var h = 0xFF;         // hex literal (255)
var o = 0o17;         // octal literal (15)
var b = 0b1010;       // binary literal (10)
var n = null;         // null value
var t = true;         // boolean true (1)
var f = false;        // boolean false (0)
```
Type annotations are optional in general but required inside `OPTMAX` blocks.

### Control Flow
```omscript
// If-else
if (condition) {
    // code
} else {
    // code
}

// While loop
while (condition) {
    // code
}

// Do-while loop (body executes at least once)
do {
    // code
} while (condition);

// For loop with range
for (i in 0...10) {          // 0 to 9
    // code using i
}

for (i in 0...100...5) {     // 0, 5, 10, ..., 95
    // step by 5
}

// For-each loop over arrays
var arr = [10, 20, 30];
for (x in arr) {
    print(x);
}

// Switch/case
switch (value) {
    case 1:
        print(1);
        break;
    case 2:
        print(2);
        break;
    default:
        print(0);
}

// Error handling
try {
    throw 42;
} catch (err) {
    print(err);  // 42
}

// Break and continue
break;
continue;
```

### Built-in Functions
```omscript
print(42);               // prints "42\n" to stdout
print("hello world");    // prints string
print_char(65);          // prints 'A' (returns the char code)
var n = str_len("hi");   // string length: n = 2
var a = abs(-10);        // absolute value: a = 10
var m = min(3, 7);       // minimum: m = 3
var p = pow(2, 8);       // integer exponentiation: p = 256
var s = sqrt(16);        // integer square root: s = 4
```
See [LANGUAGE_REFERENCE.md](LANGUAGE_REFERENCE.md) for the full list of 69 built-in functions.

### Lambda Expressions
```omscript
// Single parameter
var doubled = array_map([1, 2, 3], |x| x * 2);     // [2, 4, 6]

// Multiple parameters
var sum = array_reduce([1, 2, 3], |acc, x| acc + x, 0);  // 6

// Filter with lambda predicate
var evens = array_filter([1, 2, 3, 4], |x| x % 2 == 0);  // [2, 4]
```

### Pipe Operator
```omscript
// expr |> fn  is equivalent to  fn(expr)
fn double(x) { return x * 2; }
var result = 5 |> double;            // 10
var length = [1, 2, 3] |> len;      // 3
```

### Spread Operator
```omscript
var a = [1, 2, 3];
var b = [0, ...a, 4];    // [0, 1, 2, 3, 4]
```

### OPTMAX Blocks
Use `OPTMAX=:` and `OPTMAX!:` to tag functions that require maximal optimization. Inside these functions:
- Parameters, variables, and loop iterators must include type annotations.
- Only other `OPTMAX` functions may be called.
The compiler applies an extra OPTMAX-only optimization pass stack beyond the default O2 pipeline and custom AST constant folding even when the rest of the program uses O2.

```omscript
OPTMAX=:
fn optmax_sum(n: int) {
    var total: int = 0;
    for (i: int in 0...n) {
        total = total + i;
    }
    return total;
}
OPTMAX!:
```

### Expressions
- Arithmetic: `+`, `-`, `*`, `/`, `%`, `**` (exponentiation)
- Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=`
- Logical: `&&`, `||`, `!`
- Bitwise: `&`, `|`, `^`, `~`, `<<`, `>>`
- Ternary: `condition ? then_expr : else_expr`
- Null Coalescing: `value ?? fallback`
- Assignment: `=`
- Compound Assignment: `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- Increment/Decrement: `++x`, `--x` (prefix), `x++`, `x--` (postfix)
- Pipe: `expr |> fn`
- Lambda: `|x| x * 2`

### Comments
```omscript
// Single line comment

/* Multi-line
   block comment */

var x = 10; /* inline block comment */
```

## Building

### Prerequisites
- CMake 3.13 or higher
- C++17 compatible compiler (GCC, Clang, MSVC)
- LLVM 10+ development libraries
- GCC (for linking)

### Build Instructions

```bash
# Using Makefile
make build

# Or manually
mkdir build
cd build
cmake ..
make
```

## Usage

```bash
# Compile a source file
./build/omsc source.om -o output

# Run a source file (compile + execute)
./build/omsc run source.om

# Validate syntax without compiling
./build/omsc check source.om

# Inspect tokens
./build/omsc lex source.om

# Parse and summarize the AST
./build/omsc parse source.om

# Emit LLVM IR (stdout or file)
./build/omsc emit-ir source.om
./build/omsc emit-ir source.om -o output.ll

# Keep temporary outputs when running
./build/omsc run source.om --keep-temps

# Show timing breakdown
./build/omsc run source.om --time

# Clean default outputs
./build/omsc clean

# Package manager
./build/omsc pkg install <package>
./build/omsc pkg remove <package>
./build/omsc pkg list
./build/omsc pkg search <query>

# Install/update omsc to PATH
./build/omsc install
./build/omsc update

# Optimization levels
./build/omsc run source.om -O0    # No optimization
./build/omsc run source.om -O1    # Basic
./build/omsc run source.om -O2    # Moderate (default)
./build/omsc run source.om -O3    # Aggressive

# Codegen options
./build/omsc source.om -march=native -flto -ffast-math

# Flag aliases (equivalent to the commands above)
./build/omsc --build source.om
./build/omsc --lex source.om
./build/omsc --tokens source.om
./build/omsc --run source.om
./build/omsc --ast source.om
./build/omsc --ir source.om

# Run the compiled program
./output
```

## Examples

### Optimized For Loops
```omscript
fn sum_range(n) {
    var total = 0;
    for (i in 0...n) {
        total = total + i;
    }
    return total;
}

fn main() {
    return sum_range(100);  // Sum of 0..99 = 4950
}
```

**Optimized LLVM IR Output** (with O2):
```llvm
forcond:
  %total.0 = phi i64 [ 0, %entry ], [ %addtmp, %forbody ]
  %i.0 = phi i64 [ 0, %entry ], [ %nextvar, %forbody ]
  %forcond8 = icmp slt i64 %i.0, %n
  br i1 %forcond8, label %forbody, label %forend

forbody:
  %addtmp = add i64 %i.0, %total.0
  %nextvar = add i64 %i.0, 1
  br label %forcond
```

Notice how the optimizer:
- Converted memory allocations to SSA form (PHI nodes)
- Eliminated redundant loads/stores
- Created efficient loop structure

### Factorial (Recursion)
```omscript
fn factorial(n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

fn main() {
    var result = factorial(5);
    return result;
}
```

### Fibonacci (Iteration)
```omscript
fn fibonacci(n) {
    if (n <= 1) {
        return n;
    }
    
    var a = 0;
    var b = 1;
    var i = 2;
    
    while (i <= n) {
        var temp = a + b;
        a = b;
        b = temp;
        i = i + 1;
    }
    
    return b;
}

fn main() {
    return fibonacci(10);
}
```

## Architecture

### Compiler Pipeline
1. **Lexer**: Tokenizes source code into tokens
2. **Parser**: Builds Abstract Syntax Tree (AST) from tokens
3. **Code Generator**: Generates LLVM IR from the AST
4. **LLVM Backend**: Optimizes and compiles to native code
5. **Linker**: Links object files into a final executable

### Adaptive JIT Runtime

When using `omsc run`, the program executes through a two-tier adaptive JIT:

- **Tier 1 (Initial JIT)**: The module is JIT-compiled at O2 via LLVM MCJIT and execution begins immediately.
- **Tier 2 (Hot Recompile)**: Functions that exceed a call-count threshold are recompiled at O3 with profile-guided optimization hints, producing even faster native code for hot paths.

### Components

- **Lexer** (`src/lexer.cpp`): Tokenization
- **Parser** (`src/parser.cpp`): Syntax analysis and AST construction
- **AST** (`include/ast.h`): Abstract Syntax Tree node definitions
- **CodeGen** (`src/codegen.cpp`): LLVM IR generation
- **Compiler** (`src/compiler.cpp`): Main compiler driver
- **AOT Profile** (`runtime/aot_profile.cpp`): Adaptive recompilation of hot functions

## Type System

OmScript uses dynamic typing with runtime type inference:
- **Integer**: 64-bit signed integers
- **Float**: 64-bit floating point numbers
- **String**: Reference-counted UTF-8 strings
- **Array**: Dynamically-sized, heterogeneous arrays
- **Boolean**: `true` (1) and `false` (0), represented as integers
- **None/Null**: Absence of value (`null`)

Types are determined at runtime, allowing flexible code while maintaining performance through LLVM compilation where possible.

## Testing

```bash
# Run integration tests
bash run_tests.sh

# Or run a single example manually
./build/omsc examples/factorial.om -o factorial
./factorial
```

## Project Structure

```
omscript/
├── CMakeLists.txt        # Build configuration
├── include/             # Header files
│   ├── ast.h           # AST node definitions
│   ├── codegen.h       # LLVM code generator
│   ├── compiler.h      # Compiler driver
│   ├── diagnostic.h    # Diagnostic utilities
│   ├── lexer.h         # Lexer/tokenizer
│   ├── parser.h        # Parser
│   └── version.h       # Version constants
├── src/                # Implementation files
│   ├── ast.cpp
│   ├── codegen.cpp
│   ├── compiler.cpp
│   ├── lexer.cpp
│   ├── main.cpp
│   └── parser.cpp
├── runtime/            # Runtime system
│   ├── aot_profile.cpp # Adaptive JIT / AOT profiling
│   ├── aot_profile.h
│   ├── refcounted.h    # Reference-counted types
│   ├── value.cpp       # Dynamic values
│   └── value.h
├── tests/             # Unit tests
├── examples/          # Example programs (90+ examples)
└── user-packages/     # User-installable packages
```

## License

This project is open source and available under the MIT License.

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues for bugs and feature requests.
