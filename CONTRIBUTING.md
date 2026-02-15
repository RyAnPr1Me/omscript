# Contributing to OmScript

Thank you for considering contributing to OmScript!

## Development Setup

### Prerequisites
- CMake 3.13+
- C++17 compiler (GCC or Clang)
- LLVM 17 development libraries
- Google Test (libgtest-dev)

### Building
```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

### Running Tests
```bash
# Unit tests
cd build && ctest --output-on-failure

# Integration tests
bash run_tests.sh
```

### Building with Sanitizers
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address,undefined
cmake --build build --parallel $(nproc)
cd build && ctest --output-on-failure
```

## Code Style

The project uses a `.clang-format` configuration. Format your code before committing:
```bash
clang-format -i src/*.cpp include/*.h runtime/*.cpp runtime/*.h
```

## Pull Request Process

1. Create a feature branch from `main`
2. Make your changes with clear, focused commits
3. Ensure all tests pass (`ctest` + `run_tests.sh`)
4. Update documentation if adding new language features
5. Add test cases for new functionality
6. Submit a pull request describing your changes

## Adding New Language Features

When adding a new language feature, update all relevant pipeline stages:

1. **Lexer** (`src/lexer.cpp`, `include/lexer.h`) — New tokens
2. **Parser** (`src/parser.cpp`, `include/parser.h`) — Grammar rules
3. **AST** (`include/ast.h`) — New node types
4. **Codegen** (`src/codegen.cpp`) — LLVM IR generation
5. **Bytecode** (`src/codegen.cpp`) — Bytecode emission (if applicable)
6. **Tests** — Unit tests in `tests/` and integration tests in `examples/`
7. **Documentation** — Update `LANGUAGE_REFERENCE.md`
