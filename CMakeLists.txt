cmake_minimum_required(VERSION 3.16)

# ─────────────────────────────────────────────────────────────────────────────
# OmScript Compiler Build (Max Perf, LLVM-heavy, No Feature Loss)
# ─────────────────────────────────────────────────────────────────────────────

# ── Compiler cache ───────────────────────────────────────────────────────────
find_program(CCACHE_PROGRAM ccache)
find_program(SCCACHE_PROGRAM sccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
elseif(SCCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE_PROGRAM})
endif()

project(OmScript VERSION 3.7.0)

# Version define
if(NOT DEFINED OMSC_VERSION)
    set(OMSC_VERSION "${PROJECT_VERSION}")
endif()

# ── Global settings ───────────────────────────────────────────────────────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── Options ───────────────────────────────────────────────────────────────────
option(LTO "Enable LTO" OFF)
option(PGO_GENERATE "PGO generate" OFF)
set(PGO_PROFILE_DIR "" CACHE STRING "PGO dir")
set(PGO_USE "" CACHE STRING "PGO use")
option(NATIVE "-march=native" OFF)
option(COVERAGE "Coverage" OFF)
set(SANITIZE "" CACHE STRING "Sanitizers")
option(UNITY_BUILD "Unity build (faster compile)" OFF)
option(WITH_OPENMP "Enable OpenMP for parallel compilation passes" ON)
option(USE_MOLD "Use mold linker for faster link times" OFF)

# ── LLVM ──────────────────────────────────────────────────────────────────────
find_package(LLVM REQUIRED CONFIG)
if(LLVM_PACKAGE_VERSION VERSION_LESS "18")
    message(FATAL_ERROR "LLVM 18+ required")
endif()

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# ── Polly detection ───────────────────────────────────────────────────────────
if(NOT APPLE)
    get_filename_component(_LLVM_BASE_DIR "${LLVM_DIR}" DIRECTORY)
    find_file(POLLY_LIB NAMES LLVMPolly.so
        HINTS "${_LLVM_BASE_DIR}/lib" "${LLVM_LIBRARY_DIR}"
        NO_DEFAULT_PATH)
endif()

# ── Sources ───────────────────────────────────────────────────────────────────
set(SRC
    # Frontend / core
    src/lexer.cpp
    src/parser.cpp
    src/ast.cpp

    # Codegen pipeline
    src/codegen.cpp
    src/codegen_builtins.cpp
    src/codegen_expr.cpp
    src/codegen_stmt.cpp
    src/codegen_opt.cpp

    # Optimizers
    src/egraph.cpp
    src/egraph_optimizer.cpp
    src/superoptimizer.cpp
    src/hardware_graph.cpp

    # Driver
    src/preprocessor.cpp
    src/compiler.cpp
    src/main.cpp

    # Runtime
    runtime/value.cpp
)

add_executable(omsc ${SRC})

# ── Version define ────────────────────────────────────────────────────────────
target_compile_definitions(omsc PRIVATE
    OMSC_VERSION=\"${OMSC_VERSION}\"
)

# ── Include directories (explicit for headers) ────────────────────────────────
target_include_directories(omsc PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/runtime
)

# ── Compile options (maximum performance) ─────────────────────────────────────
target_compile_options(omsc PRIVATE
    -Wall -Wextra -Wpedantic

    $<$<CONFIG:Release>:
        -O3
        -DNDEBUG

        # Core perf
        -fno-plt
        -fomit-frame-pointer
        -fstrict-aliasing
        -finline-functions

        # Aggressive loop optimizations
        -funroll-loops
        -ftree-vectorize

        # Code layout / size
        -ffunction-sections
        -fdata-sections

        # Visibility (safe with export-dynamic)
        -fvisibility=hidden
    >

    $<$<CONFIG:RelWithDebInfo>:
        -O2
        -gline-tables-only
    >

    $<$<AND:$<CONFIG:Release>,$<BOOL:${NATIVE}>>:
        -march=native
    >

    # GCC-only loop and IPA optimizations (not supported by Clang/MSVC).
    # Excluded on Windows/MinGW: -fipa-pta and the graphite-adjacent loop
    # flags cause GCC to hang for hours on LLVM-header-heavy translation
    # units when targeting MinGW (LLVM 21 + GCC 15).
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU>,$<NOT:$<PLATFORM_ID:Windows>>>:
        -fprefetch-loop-arrays
        -fno-semantic-interposition
        -ftree-loop-distribution
        -ftree-loop-im
        -ftree-loop-ivcanon
        -fipa-pta

        # GCC block reordering: STC (software trace cache) produces good
        # code locality for large binaries with many branches.
        -freorder-blocks-algorithm=stc
        # Align hot function entries to cache line boundaries.
        -falign-functions=32
        # Enable computed goto lowering and switch table generation.
        -ftree-switch-conversion
    >
)

# ── Link options ──────────────────────────────────────────────────────────────
target_link_options(omsc PRIVATE
    # GNU ld-specific link optimizations and static C/C++ runtime linkage.
    # Not supported on macOS (ld64) or MSVC; applied for GCC (Linux + MinGW).
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:GNU>>:
        -Wl,-O3
        -Wl,--gc-sections
        # Static linkage for C/C++ runtimes: embed libgcc and libstdc++
        # into the binary so it doesn't depend on system-specific
        # versions.  Full -static is not used because LLVM depends on
        # dynamic-only system libraries (libffi, libz, etc.).
        -static-libgcc
        -static-libstdc++
        # Build a non-PIE (position-dependent) executable on Linux.
        # BOLT (Binary Optimization and Layout Tool) used in the release
        # pipeline cannot instrument PIE executables.  omsc is a developer
        # tool, not a user-facing daemon, so the ASLR trade-off is fine.
        -no-pie
        # Keep relocation records in the output binary so that BOLT can
        # correctly resolve indirect branches, PLT entries, and function
        # references when instrumenting or optimizing the binary.
        # Without this, BOLT instrumentation fails on mold-linked binaries.
        -Wl,--emit-relocs
    >
)

# ── Mold linker (optional — much faster link times for PGO rebuild) ───────────
if(USE_MOLD)
    find_program(MOLD_LINKER mold)
    if(MOLD_LINKER)
        target_link_options(omsc PRIVATE -fuse-ld=mold)
        message(STATUS "Using mold linker: ${MOLD_LINKER}")
    else()
        message(STATUS "mold not found — using default linker")
    endif()
endif()

# ── LTO (IPO) ─────────────────────────────────────────────────────────────────
if(LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ok)
    if(ok)
        set_property(TARGET omsc PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

# ── PGO ───────────────────────────────────────────────────────────────────────
if(PGO_GENERATE AND PGO_USE)
    message(FATAL_ERROR "PGO_GENERATE and PGO_USE are mutually exclusive")
endif()

if(PGO_GENERATE)
    set(_pgo_gen_flag "-fprofile-generate")
    if(PGO_PROFILE_DIR)
        set(_pgo_gen_flag "${_pgo_gen_flag}=${PGO_PROFILE_DIR}")
    endif()
    # Pass each PGO flag as its own target_compile_options call.
    # Using list(APPEND) + a single target_compile_options expansion can cause
    # CMake (particularly for the precompiled-header compilation step in GCC
    # builds) to concatenate the semicolon-separated list into one shell
    # argument ("-fprofile-generate -fprofile-update=atomic"), producing:
    #   g++: error: unrecognized command-line option '-fprofile-generate ...'
    # Separate calls guarantee each flag lands in its own argv slot.
    target_compile_options(omsc PRIVATE "${_pgo_gen_flag}")
    # Atomic profile updates: safe when the instrumented binary is invoked
    # concurrently (e.g. run_tests.sh compiling multiple .om files).
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(omsc PRIVATE -fprofile-update=atomic)
    endif()
    # Only -fprofile-generate[=dir] is needed at link time.
    target_link_options(omsc PRIVATE "${_pgo_gen_flag}")
endif()

if(PGO_USE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(omsc PRIVATE -fprofile-use=${PGO_USE})
        target_link_options(omsc PRIVATE -fprofile-use=${PGO_USE})
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        # When PGO_USE is a boolean true value (ON/1/TRUE), use -fprofile-use
        # without a directory: GCC then finds .gcda files in the same location
        # as the .gcno files produced during the generate phase (the build
        # tree).  Supplying an explicit absolute directory causes a path-
        # mangling mismatch — GCC #-encodes the absolute object-file path into
        # the .gcda filename, but the use phase looks up the source-file path,
        # so the two never match and all profile data is silently ignored.
        if(IS_ABSOLUTE "${PGO_USE}" OR PGO_USE MATCHES "^\\./")
            # Explicit path supplied (e.g. for Clang .profdata)
            set(_pgo_use_flag "-fprofile-use=${PGO_USE}")
        else()
            # Boolean/sentinel value — no-dir mode; gcda files are in the build tree
            set(_pgo_use_flag "-fprofile-use")
        endif()
        target_compile_options(omsc PRIVATE
            ${_pgo_use_flag}
            -fprofile-correction
            # Treat untrained code paths as cold.  Without this flag GCC
            # gives equal weight to trained and untrained paths, which
            # pollutes branch-probability estimates for functions that were
            # only partially covered during PGO training.
            -fprofile-partial-training
            -Wno-missing-profile
            # Suppress "control flow does not match profile data" errors that
            # arise when a function is modified after the training run (e.g.
            # estimateWorkingSet).  -fprofile-partial-training handles missing
            # arcs but not structural mismatches; this flag covers the rest.
            -Wno-coverage-mismatch
        )
        target_link_options(omsc PRIVATE ${_pgo_use_flag})
    endif()
endif()

# ── Sanitizers ────────────────────────────────────────────────────────────────
if(SANITIZE)
    target_compile_options(omsc PRIVATE -fsanitize=${SANITIZE} -fno-omit-frame-pointer)
    target_link_options(omsc PRIVATE -fsanitize=${SANITIZE})
endif()

# ── Coverage ──────────────────────────────────────────────────────────────────
if(COVERAGE)
    target_compile_options(omsc PRIVATE --coverage -fno-inline)
    target_link_options(omsc PRIVATE --coverage)
endif()

# ── Unity build (huge compile speed boost for large compilers) ────────────────
if(UNITY_BUILD)
    set_target_properties(omsc PROPERTIES UNITY_BUILD ON)
endif()

# ── Threads ───────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(omsc PRIVATE Threads::Threads)

# ── OpenMP (optional — accelerates parallel compiler passes) ──────────────────
# When available, superoptimizeModule dispatches function-level optimization
# passes to a thread pool using std::async; each function's IR is disjoint
# so concurrent modification is safe.  The SuperoptimizerConfig struct is
# copied (captured by value) into each async task to avoid dangling references.
if(WITH_OPENMP)
    find_package(OpenMP QUIET)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(omsc PRIVATE OpenMP::OpenMP_CXX)
        target_compile_definitions(omsc PRIVATE OMSC_HAS_OPENMP=1)
        message(STATUS "OpenMP ${OpenMP_CXX_VERSION} found — parallel compiler passes enabled")
    else()
        message(STATUS "OpenMP not found — compiler passes will run sequentially")
    endif()
endif()

# ── LLVM linking ──────────────────────────────────────────────────────────────
set(LLVM_COMPONENTS
    core native support passes target transformutils analysis
    asmparser codegen mc mcparser bitreader
)

llvm_map_components_to_libnames(llvm_libs ${LLVM_COMPONENTS})

target_link_libraries(omsc PRIVATE ${llvm_libs})

# ── Polly define ──────────────────────────────────────────────────────────────
if(POLLY_LIB)
    target_compile_definitions(omsc PRIVATE "POLLY_LIB_PATH=\"${POLLY_LIB}\"")
endif()

# ── Precompiled headers ───────────────────────────────────────────────────────
target_precompile_headers(omsc PRIVATE
    <llvm/IR/IRBuilder.h>
    <llvm/IR/Module.h>
    <llvm/IR/Instructions.h>
    <llvm/IR/LLVMContext.h>
    <llvm/IR/Value.h>
    <llvm/IR/Function.h>
    <string>
    <vector>
    <memory>
    <unordered_map>
    <unordered_set>
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS omsc RUNTIME DESTINATION bin)
