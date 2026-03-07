cmake_minimum_required(VERSION 3.13)
project(OmScript VERSION 2.0.0)

# Allow overriding the compiler version string at build time (e.g. -DOMSC_VERSION=1.2.3)
if(NOT DEFINED OMSC_VERSION)
    set(OMSC_VERSION "${PROJECT_VERSION}")
endif()
add_compile_definitions(OMSC_VERSION="${OMSC_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release build for production use.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug, Release, RelWithDebInfo)" FORCE)
endif()

# Enable compiler warnings for early defect detection
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Link-time optimization for the compiler binary itself (disabled by default
# to avoid linking issues on some platforms).  Enable with -DLTO=ON.
option(LTO "Enable link-time optimization for the compiler build" OFF)
if(LTO)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
        if(lto_supported)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
            message(STATUS "Link-time optimization (LTO) enabled")
        else()
            message(WARNING "LTO requested but not supported: ${lto_error}")
        endif()
    else()
        message(STATUS "LTO requires Release or RelWithDebInfo build type; ignoring -DLTO=ON")
    endif()
endif()

# Code coverage configuration
option(COVERAGE "Enable code coverage" OFF)
if(COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(--coverage -fno-inline)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    endif()
endif()

# Optional sanitizer support for catching memory and undefined behavior bugs.
# Usage: cmake -DSANITIZE=address  (or: thread, undefined, address,undefined)
if(SANITIZE)
    set(SANITIZE_FLAGS "-fsanitize=${SANITIZE} -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZE_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZE_FLAGS}")
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
    src/codegen_builtins.cpp
    src/codegen_expr.cpp
    src/codegen_stmt.cpp
    src/codegen_opt.cpp
    src/compiler.cpp
    src/main.cpp
)

# Runtime sources
set(RUNTIME_SOURCES
    runtime/value.cpp
    runtime/aot_profile.cpp
    runtime/jit_profiler.cpp
    runtime/deopt.cpp
)

# Compiler executable
add_executable(omsc ${COMPILER_SOURCES} ${RUNTIME_SOURCES})

# Link LLVM libraries
# Only components actually used by the compiler and AOT runtime are listed.
# - core, support: fundamental LLVM infrastructure
# - executionengine, orcjit: adaptive ORC JIT execution (runtime/aot_profile.cpp)
# - native: host target auto-detection
# - passes, transformutils, analysis: optimization pipeline
# - target, codegen, mc, mcparser, asmparser: machine code emission
set(LLVM_COMPONENTS
    core
    executionengine
    interpreter
    orcjit
    orctargetprocess
    native
    support
    passes
    target
    transformutils
    analysis
    asmparser
    codegen
    mc
    mcparser
    bitreader
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

# ORC JIT requires the host binary to export its own symbols so the in-process
# executor can locate ORC runtime helpers (e.g. llvm_orc_registerEHFrameSectionWrapper)
# via dlsym.  --export-dynamic ensures all statically-linked symbols from
# libLLVMOrcTargetProcess are visible to the dynamic linker at runtime.
#
# Note: a linker version script (--version-script) could be used to export
# only the specific ORC symbols needed, reducing symbol-table bloat.  For
# simplicity we use --export-dynamic, which is the approach used by LLVM's
# own in-process JIT examples and test infrastructure.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_link_options(omsc PRIVATE -Wl,--export-dynamic)
endif()

# Installation
install(TARGETS omsc DESTINATION bin)

# --- GTest unit tests ---
find_package(Threads QUIET)
find_package(GTest QUIET)
if(GTest_FOUND)
    enable_testing()

    # Library of compiler/runtime sources (without main.cpp) for tests to link against.
    set(LIB_SOURCES
        src/lexer.cpp
        src/parser.cpp
        src/ast.cpp
        src/codegen.cpp
        src/codegen_builtins.cpp
        src/codegen_expr.cpp
        src/codegen_stmt.cpp
        src/codegen_opt.cpp
        src/compiler.cpp
        runtime/value.cpp
        runtime/aot_profile.cpp
        runtime/jit_profiler.cpp
        runtime/deopt.cpp
    )

    add_library(omscript_lib STATIC ${LIB_SOURCES})
    target_link_libraries(omscript_lib ${llvm_libs})

    # Collect all test source files
    file(GLOB TEST_SOURCES tests/*_test.cpp)

    foreach(test_source ${TEST_SOURCES})
        get_filename_component(test_name ${test_source} NAME_WE)
        add_executable(${test_name} ${test_source})
        target_link_libraries(${test_name} omscript_lib GTest::GTest GTest::Main ${llvm_libs})
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# CPack configuration for distribution packages
# NOTE: CPACK_* variables must be set BEFORE include(CPack).
set(CPACK_PACKAGE_NAME "OmScript")
set(CPACK_PACKAGE_VENDOR "OmScript")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A low-level C-like programming language with LLVM AOT compilation")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "OmScript")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_SOURCE_IGNORE_FILES
    "^/build"
    "^/.git"
    "^/examples"
    "^/tests"
    "^/.github"
    "^/CMakeLists.txt$"
    "^/.clang-format$"
    "^/.clang-tidy$"
    "^/.editorconfig$"
    "^/Makefile$"
    "^/run_tests.sh$"
)
set(CPACK_BINARY_TGZ "ON")
include(CPack)
