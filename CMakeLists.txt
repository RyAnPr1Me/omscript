cmake_minimum_required(VERSION 3.13)
project(OmScript VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable compiler warnings for early defect detection
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find LLVM
find_package(LLVM REQUIRED CONFIG)

if(NOT LLVM_FOUND)
    message(FATAL_ERROR "LLVM not found. Please install LLVM development libraries.")
endif()

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
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
set(LLVM_COMPONENTS
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
    asmparser
    codegen
    mc
    mcparser
)

# Conditionally add target-specific components based on available LLVM targets
if("X86" IN_LIST LLVM_TARGETS_TO_BUILD)
    list(APPEND LLVM_COMPONENTS x86codegen x86asmparser x86desc x86info)
endif()

if("AArch64" IN_LIST LLVM_TARGETS_TO_BUILD)
    list(APPEND LLVM_COMPONENTS aarch64codegen aarch64asmparser aarch64desc aarch64info)
endif()

if("ARM" IN_LIST LLVM_TARGETS_TO_BUILD)
    list(APPEND LLVM_COMPONENTS armcodegen armasmparser armdesc arminfo)
endif()

llvm_map_components_to_libnames(llvm_libs ${LLVM_COMPONENTS})

target_link_libraries(omsc ${llvm_libs})

# Installation
install(TARGETS omsc DESTINATION bin)
