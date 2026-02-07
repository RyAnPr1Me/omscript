# OmScript

A low-level, C-like programming language with dynamic typing, featuring an AOT compiler using LLVM and a bytecode interpreter runtime.

## Features

- **C-like Syntax**: Familiar syntax for C programmers
- **Dynamic Typing**: Variables are dynamically typed, no explicit type declarations needed
- **AOT Compilation**: Ahead-of-time compilation using LLVM backend for performance
- **Bytecode Runtime**: Interprets dynamically typed sections at runtime
- **Hybrid Approach**: Compiles static code paths with LLVM, uses bytecode VM for dynamic behavior

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
```

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

# Run the compiled program
./output
```

## Examples

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