cmake_minimum_required(VERSION 3.16)

# ── Compiler cache (ccache / sccache) ─────────────────────────────────────────
# Automatically use ccache or sccache when available to speed up incremental
# builds.  Must be set before project() so the compiler launcher is active for
# all targets.
find_program(CCACHE_PROGRAM ccache)
find_program(SCCACHE_PROGRAM sccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
elseif(SCCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
    message(STATUS "Using sccache: ${SCCACHE_PROGRAM}")
endif()

project(OmScript VERSION 3.5.0)

# ── Export compile_commands.json for IDE / clang-tidy integration ─────────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Allow overriding the compiler version string at build time (e.g. -DOMSC_VERSION=1.2.3)
if(NOT DEFINED OMSC_VERSION)
    set(OMSC_VERSION "${PROJECT_VERSION}")
endif()
# Global define — safe before targets exist; propagates to all TUs.
add_compile_definitions(OMSC_VERSION="${OMSC_VERSION}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release build for production use.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Debug, Release, RelWithDebInfo)" FORCE)
endif()

# ── Options ───────────────────────────────────────────────────────────────────
option(LTO "Enable link-time optimization for the compiler build" OFF)
option(PGO_GENERATE "Instrument build for PGO profile generation" OFF)
set(PGO_PROFILE_DIR "" CACHE STRING "Directory for PGO raw profiles (PGO_GENERATE only)")
set(PGO_USE "" CACHE STRING "PGO profile path: directory of .gcda files (GCC) or merged .profdata file (Clang)")
option(NATIVE "Build with -march=native for host-specific optimizations" OFF)
option(COVERAGE "Enable code coverage" OFF)
set(SANITIZE "" CACHE STRING "Sanitizers (e.g. address, undefined, address,undefined)")
option(UNITY_BUILD "Unity build for faster full rebuilds" OFF)

# ── Global compile flags ─────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -pipe)
endif()

# ── LTO ───────────────────────────────────────────────────────────────────────
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

# ── PGO ───────────────────────────────────────────────────────────────────────
if(PGO_GENERATE AND PGO_USE)
    message(FATAL_ERROR "PGO_GENERATE and PGO_USE are mutually exclusive")
endif()
if(PGO_GENERATE)
    set(_pgo_gen_flags "-fprofile-generate")
    if(PGO_PROFILE_DIR)
        set(_pgo_gen_flags "${_pgo_gen_flags}=${PGO_PROFILE_DIR}")
    endif()
    add_compile_options(${_pgo_gen_flags})
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_pgo_gen_flags}")
    message(STATUS "PGO instrumentation enabled (profile dir: ${PGO_PROFILE_DIR})")
endif()
if(PGO_USE)
    if(NOT EXISTS "${PGO_USE}")
        message(FATAL_ERROR "PGO profile not found: ${PGO_USE}")
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_PGO_USE_COMPILE_FLAGS -fprofile-use=${PGO_USE})
        set(_PGO_USE_LINK_FLAGS -fprofile-use=${PGO_USE})
        message(STATUS "PGO profile-use enabled (Clang): ${PGO_USE}")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        set(_PGO_USE_COMPILE_FLAGS -fprofile-use=${PGO_USE} -fprofile-correction -Wno-missing-profile)
        set(_PGO_USE_LINK_FLAGS -fprofile-use=${PGO_USE})
        message(STATUS "PGO profile-use enabled (GCC): ${PGO_USE}")
    else()
        message(WARNING "PGO_USE requested but compiler does not support it")
    endif()
endif()

# ── Native tuning ─────────────────────────────────────────────────────────────
if(NATIVE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-march=native)
        message(STATUS "Native CPU targeting enabled (-march=native)")
    endif()
endif()

# ── Coverage ──────────────────────────────────────────────────────────────────
if(COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(--coverage -fno-inline)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    endif()
endif()

# ── Sanitizers ────────────────────────────────────────────────────────────────
if(SANITIZE)
    set(SANITIZE_FLAGS "-fsanitize=${SANITIZE} -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZE_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZE_FLAGS}")
endif()

# ── LLVM ──────────────────────────────────────────────────────────────────────
find_package(LLVM REQUIRED CONFIG)
if(LLVM_PACKAGE_VERSION VERSION_LESS "18")
    message(FATAL_ERROR "LLVM 18.0 or newer is required, but found LLVM ${LLVM_PACKAGE_VERSION}")
endif()
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# ── Polly detection ───────────────────────────────────────────────────────────
# Polly plugin loading is disabled on macOS because Homebrew LLVM does not
# export llvm::PassPlugin::Load from its static archives.
if(NOT APPLE)
    get_filename_component(_LLVM_BASE_DIR "${LLVM_DIR}" DIRECTORY)
    find_file(POLLY_LIB NAMES LLVMPolly.so
        HINTS "${_LLVM_BASE_DIR}/lib" "${LLVM_LIBRARY_DIR}"
        NO_DEFAULT_PATH)
    if(POLLY_LIB)
        message(STATUS "Found LLVM Polly: ${POLLY_LIB}")
    else()
        message(STATUS "LLVM Polly not found; polyhedral loop optimizations disabled")
    endif()
else()
    message(STATUS "Polly plugin disabled on macOS")
endif()

# ── Sources ───────────────────────────────────────────────────────────────────
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/runtime)

set(COMPILER_SOURCES
    src/lexer.cpp
    src/parser.cpp
    src/ast.cpp
    src/codegen.cpp
    src/codegen_builtins.cpp
    src/codegen_expr.cpp
    src/codegen_stmt.cpp
    src/codegen_opt.cpp
    src/egraph.cpp
    src/egraph_optimizer.cpp
    src/superoptimizer.cpp
    src/hardware_graph.cpp
    src/compiler.cpp
    src/main.cpp
)

set(RUNTIME_SOURCES
    runtime/value.cpp
    runtime/aot_profile.cpp
    runtime/jit_profiler.cpp
    runtime/deopt.cpp
)

# ── Main target ───────────────────────────────────────────────────────────────
add_executable(omsc ${COMPILER_SOURCES} ${RUNTIME_SOURCES})

# ── Release-mode performance flags (target-specific) ─────────────────────────
# These flags produce a significantly faster binary:
# - -fno-plt: avoid PLT indirection on shared library calls
# - -fomit-frame-pointer: free up a register (RBP on x86_64)
# - -fstrict-aliasing: enable type-based alias analysis
# - -finline-functions: inline small functions for zero-overhead abstractions
# - -ffunction-sections -fdata-sections: enable --gc-sections dead-code removal
# - -fno-semantic-interposition: big win for LLVM-style virtual dispatch
# - -fvisibility=hidden: reduce symbol table size and enable devirtualization
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(omsc PRIVATE
        $<$<CONFIG:Release>:
            -fno-plt
            -fomit-frame-pointer
            -fstrict-aliasing
            -finline-functions
            -ffunction-sections
            -fdata-sections
            -fno-semantic-interposition
            -fvisibility=hidden
        >
    )
endif()

# ── Release-mode link flags ──────────────────────────────────────────────────
# --gc-sections removes unreachable code/data sections that were isolated by
# -ffunction-sections/-fdata-sections, producing a smaller binary.
# -Wl,-O2 enables linker-level optimizations (relocation merging, etc.).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_link_options(omsc PRIVATE
        $<$<CONFIG:Release>:
            -Wl,-O2
            -Wl,--gc-sections
        >
    )
endif()

# ── Unity build (faster full rebuilds by merging TUs) ────────────────────────
if(UNITY_BUILD)
    set_target_properties(omsc PROPERTIES UNITY_BUILD ON)
endif()

# ── Precompiled headers (PCH) ────────────────────────────────────────────────
target_precompile_headers(omsc PRIVATE
    <llvm/IR/IRBuilder.h>
    <llvm/IR/Module.h>
    <llvm/IR/Instructions.h>
    <llvm/IR/LLVMContext.h>
    <llvm/IR/Value.h>
    <llvm/IR/DIBuilder.h>
    <llvm/IR/Function.h>
    <string>
    <vector>
    <memory>
    <unordered_map>
    <unordered_set>
)

# Apply PGO profile-use flags only to the compiler target (not tests).
if(_PGO_USE_COMPILE_FLAGS)
    target_compile_options(omsc PRIVATE ${_PGO_USE_COMPILE_FLAGS})
    target_link_options(omsc PRIVATE ${_PGO_USE_LINK_FLAGS})
endif()

# Pass Polly plugin path as a compile-time define.
if(POLLY_LIB)
    target_compile_definitions(omsc PRIVATE "POLLY_LIB_PATH=\"${POLLY_LIB}\"")
endif()

# ── LLVM linking ──────────────────────────────────────────────────────────────
set(LLVM_COMPONENTS
    core executionengine interpreter orcjit orctargetprocess
    native support passes target transformutils analysis
    asmparser codegen mc mcparser bitreader
)

# Conditionally add target-specific components based on available LLVM targets.
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

find_package(Threads REQUIRED)
target_link_libraries(omsc PRIVATE ${llvm_libs} Threads::Threads)

# ORC JIT requires host symbol export for in-process execution.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_link_options(omsc PRIVATE -Wl,--export-dynamic)
endif()

# ── Installation ──────────────────────────────────────────────────────────────
install(TARGETS omsc RUNTIME DESTINATION bin)

# ── GTest unit tests ──────────────────────────────────────────────────────────
find_package(GTest QUIET)
if(GTest_FOUND)
    enable_testing()

    # Static library of compiler+runtime sources (without main.cpp) for tests.
    set(LIB_SOURCES ${COMPILER_SOURCES} ${RUNTIME_SOURCES})
    list(FILTER LIB_SOURCES EXCLUDE REGEX "main\\.cpp$")
    add_library(omscript_lib STATIC ${LIB_SOURCES})
    target_link_libraries(omscript_lib PRIVATE ${llvm_libs})
    target_precompile_headers(omscript_lib REUSE_FROM omsc)
    if(POLLY_LIB)
        target_compile_definitions(omscript_lib PRIVATE "POLLY_LIB_PATH=\"${POLLY_LIB}\"")
    endif()
    if(_PGO_USE_COMPILE_FLAGS)
        target_compile_options(omscript_lib PRIVATE ${_PGO_USE_COMPILE_FLAGS})
    endif()

    file(GLOB TEST_SOURCES tests/*_test.cpp)
    foreach(test_source ${TEST_SOURCES})
        get_filename_component(test_name ${test_source} NAME_WE)
        add_executable(${test_name} ${test_source})
        target_link_libraries(${test_name} PRIVATE omscript_lib GTest::GTest GTest::Main ${llvm_libs})
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()

# ── CPack ─────────────────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "OmScript")
set(CPACK_PACKAGE_VENDOR "OmScript")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A low-level C-like programming language with LLVM AOT compilation")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "OmScript")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_SOURCE_IGNORE_FILES "^/build" "^/.git" "^/examples" "^/tests" "^/.github")
set(CPACK_BINARY_TGZ "ON")
include(CPack)
