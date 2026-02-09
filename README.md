# OmScript

A low-level, C-like programming language with dynamic typing, featuring a **heavily optimized AOT compiler** using LLVM, a bytecode interpreter runtime, and **automatic reference counting memory management**.

## Key Features

- **C-like Syntax**: Familiar syntax for C programmers
- **Dynamic Typing**: Variables are dynamically typed, no explicit type declarations needed
- **Aggressive AOT Compilation**: Multi-level LLVM optimization (O0/O1/O2/O3) for maximum performance
- **Reference Counted Memory**: Automatic memory management using malloc/free with deterministic deallocation
- **For Loops with Ranges**: Modern range-based iteration with `for (i in start...end...step)`
- **Bytecode Runtime**: Interprets dynamically typed sections at runtime
- **Hybrid Approach**: Compiles static code paths with LLVM, uses bytecode VM for dynamic behavior

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
```

### Variables
```omscript
var x = 10;           // mutable variable
const y = 20;         // constant (immutable)
var z: int = 30;      // optional type annotation
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

// For loop with range
for (i in 0...10) {          // 0 to 9
    // code using i
}

for (i in 0...100...5) {     // 0, 5, 10, ..., 95
    // step by 5
}

// Break and continue (coming soon)
break;
continue;
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
- Arithmetic: `+`, `-`, `*`, `/`, `%`
- Comparison: `==`, `!=`, `<`, `<=`, `>`, `>=`
- Logical: `&&`, `||`, `!`
- Assignment: `=`

### Comments
```omscript
// Single line comment
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

# Inspect tokens
./build/omsc lex source.om

# Parse and summarize the AST
./build/omsc parse source.om

# Emit LLVM IR (stdout or file)
./build/omsc emit-ir source.om
./build/omsc emit-ir source.om -o output.ll

# Keep temporary outputs when running
./build/omsc run source.om --keep-temps

# Clean default outputs
./build/omsc clean

# Flag aliases (equivalent to the commands above)
./build/omsc --build source.om
./build/omsc --lex source.om
./build/omsc --tokens source.om
./build/omsc --run source.om
./build/omsc --ast source.om
./build/omsc --ir source.om
./build/omsc emit-ir source.om --output output.ll
./build/omsc run source.om -k

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
3. **Code Generator**: 
   - Generates LLVM IR for statically analyzable code
   - Emits bytecode for dynamic sections
4. **LLVM Backend**: Optimizes and compiles to native code
5. **Runtime**: Bytecode VM for dynamic execution

### Components

- **Lexer** (`src/lexer.cpp`): Tokenization
- **Parser** (`src/parser.cpp`): Syntax analysis and AST construction
- **AST** (`include/ast.h`): Abstract Syntax Tree node definitions
- **CodeGen** (`src/codegen.cpp`): LLVM IR generation
- **Bytecode** (`src/bytecode.cpp`): Bytecode emission
- **VM** (`runtime/vm.cpp`): Bytecode interpreter
- **Value** (`runtime/value.cpp`): Dynamic value representation
- **Compiler** (`src/compiler.cpp`): Main compiler driver

## Type System

OmScript uses dynamic typing with runtime type inference:
- **Integer**: 64-bit signed integers
- **Float**: 64-bit floating point numbers
- **String**: UTF-8 strings
- **None**: Absence of value

Types are determined at runtime, allowing flexible code while maintaining performance through LLVM compilation where possible.

## Testing

```bash
# Run example programs
make test

# Or manually
./build/omsc examples/factorial.om -o factorial
./factorial
```

## Project Structure

```
omscript/
├── CMakeLists.txt        # Build configuration
├── Makefile             # Convenience build targets
├── include/             # Header files
│   ├── ast.h           # AST node definitions
│   ├── bytecode.h      # Bytecode emitter
│   ├── codegen.h       # LLVM code generator
│   ├── compiler.h      # Compiler driver
│   ├── lexer.h         # Lexer/tokenizer
│   └── parser.h        # Parser
├── src/                # Implementation files
│   ├── ast.cpp
│   ├── bytecode.cpp
│   ├── codegen.cpp
│   ├── compiler.cpp
│   ├── lexer.cpp
│   ├── main.cpp
│   └── parser.cpp
├── runtime/            # Runtime system
│   ├── value.cpp      # Dynamic values
│   ├── value.h
│   ├── vm.cpp         # Bytecode VM
│   └── vm.h
└── examples/          # Example programs
    ├── arithmetic.om
    ├── factorial.om
    ├── fibonacci.om
    └── test.om
```

## License

This project is open source and available under the MIT License.

## Contributing

Contributions are welcome! Please feel free to submit pull requests or open issues for bugs and feature requests.
