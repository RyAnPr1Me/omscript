cmake_minimum_required(VERSION 3.16)

# ─────────────────────────────────────────────────────────────────────────────
# OmScript Build System (Optimized for Compiler Performance)
# ─────────────────────────────────────────────────────────────────────────────

# ── Compiler cache ───────────────────────────────────────────────────────────
find_program(CCACHE_PROGRAM ccache)
find_program(SCCACHE_PROGRAM sccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
elseif(SCCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${SCCACHE_PROGRAM})
endif()

project(OmScript VERSION 3.5.0)

# ── Global settings ───────────────────────────────────────────────────────────
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── Options ───────────────────────────────────────────────────────────────────
option(LTO "Enable link-time optimization" OFF)
option(PGO_GENERATE "Enable PGO generation" OFF)
set(PGO_PROFILE_DIR "" CACHE STRING "PGO output dir")
set(PGO_USE "" CACHE STRING "PGO input profile")
option(NATIVE "Enable -march=native" OFF)
option(UNITY_BUILD "Enable unity builds" OFF)
set(SANITIZE "" CACHE STRING "Sanitizers")

# ── LLVM ──────────────────────────────────────────────────────────────────────
find_package(LLVM REQUIRED CONFIG)
if(LLVM_PACKAGE_VERSION VERSION_LESS "18")
    message(FATAL_ERROR "LLVM 18+ required")
endif()

include_directories(SYSTEM ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# ── Sources ───────────────────────────────────────────────────────────────────
set(SRC
    src/lexer.cpp src/parser.cpp src/ast.cpp src/codegen.cpp
    src/codegen_builtins.cpp src/codegen_expr.cpp src/codegen_stmt.cpp
    src/codegen_opt.cpp src/egraph.cpp src/egraph_optimizer.cpp
    src/superoptimizer.cpp src/hardware_graph.cpp src/compiler.cpp src/main.cpp
    runtime/value.cpp runtime/aot_profile.cpp runtime/jit_profiler.cpp runtime/deopt.cpp
)

add_executable(omsc ${SRC})

# ── Compile options (tuned for compiler workloads) ────────────────────────────
target_compile_options(omsc PRIVATE
    # Warnings
    -Wall -Wextra -Wpedantic

    # Release optimizations
    $<$<CONFIG:Release>:
        -O3
        -DNDEBUG

        # Performance-focused flags
        -fno-plt
        -fomit-frame-pointer
        -finline-functions
        -fstrict-aliasing

        # Better code layout / size
        -ffunction-sections
        -fdata-sections

        # LLVM-heavy workloads benefit
        -fno-semantic-interposition
    >

    # Native CPU tuning (optional)
    $<$<AND:$<CONFIG:Release>,$<BOOL:${NATIVE}>>:
        -march=native
    >
)

# ── Linker options ────────────────────────────────────────────────────────────
target_link_options(omsc PRIVATE
    $<$<CONFIG:Release>:
        -Wl,-O2
        -Wl,--gc-sections
    >
)

# Required for ORC JIT symbol visibility
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_link_options(omsc PRIVATE -Wl,--export-dynamic)
endif()

# ── LTO ───────────────────────────────────────────────────────────────────────
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
    set(flags "-fprofile-generate")
    if(PGO_PROFILE_DIR)
        set(flags "${flags}=${PGO_PROFILE_DIR}")
    endif()
    target_compile_options(omsc PRIVATE ${flags})
    target_link_options(omsc PRIVATE ${flags})
endif()

if(PGO_USE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(omsc PRIVATE -fprofile-use=${PGO_USE})
        target_link_options(omsc PRIVATE -fprofile-use=${PGO_USE})
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(omsc PRIVATE
            -fprofile-use=${PGO_USE}
            -fprofile-correction
            -Wno-missing-profile
        )
        target_link_options(omsc PRIVATE -fprofile-use=${PGO_USE})
    endif()
endif()

# ── Sanitizers ────────────────────────────────────────────────────────────────
if(SANITIZE)
    target_compile_options(omsc PRIVATE -fsanitize=${SANITIZE} -fno-omit-frame-pointer)
    target_link_options(omsc PRIVATE -fsanitize=${SANITIZE})
endif()

# ── Unity builds (faster compile times) ───────────────────────────────────────
if(UNITY_BUILD)
    set_target_properties(omsc PROPERTIES UNITY_BUILD ON)
endif()

# ── Threads ───────────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(omsc PRIVATE Threads::Threads)

# ── LLVM linking ──────────────────────────────────────────────────────────────
set(LLVM_COMPONENTS
    core executionengine interpreter orcjit orctargetprocess
    native support passes target transformutils analysis
    asmparser codegen mc mcparser bitreader
)

llvm_map_components_to_libnames(llvm_libs ${LLVM_COMPONENTS})

target_link_libraries(omsc PRIVATE ${llvm_libs})

# ── Precompiled headers (huge build speed win) ────────────────────────────────
target_precompile_headers(omsc PRIVATE
    <llvm/IR/IRBuilder.h>
    <llvm/IR/Module.h>
    <llvm/IR/Instructions.h>
    <llvm/IR/LLVMContext.h>
    <llvm/IR/Value.h>
)

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS omsc RUNTIME DESTINATION bin)
