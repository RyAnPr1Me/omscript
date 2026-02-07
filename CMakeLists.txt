cmake_minimum_required(VERSION 3.13)
project(OmScript VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/runtime)

# Compiler sources
set(COMPILER_SOURCES
    src/lexer.cpp
    src/parser.cpp
    src/ast.cpp
    src/codegen.cpp
    src/bytecode.cpp
    src/compiler.cpp
    src/main.cpp
)

# Runtime sources
set(RUNTIME_SOURCES
    runtime/vm.cpp
    runtime/value.cpp
)

# Compiler executable
add_executable(omsc ${COMPILER_SOURCES} ${RUNTIME_SOURCES})

# Link LLVM libraries
llvm_map_components_to_libnames(llvm_libs
    core
    executionengine
    mcjit
    interpreter
    native
    irreader
    support
    passes
    target
    transformutils
    analysis
)

target_link_libraries(omsc ${llvm_libs})

# Installation
install(TARGETS omsc DESTINATION bin)
