#pragma once

#ifndef PROJECT_H
#define PROJECT_H

/// @file project.h
/// @brief OmScript project manifest (oms.toml) data model.
///
/// This module defines the build profile and project manifest structures used
/// by the build system. An oms.toml file is the canonical description of a
/// project: its entry point, named build profiles, and local dependencies.
///
/// # Manifest example
/// ```toml
/// [project]
/// name    = "hello"
/// version = "0.1.0"
/// entry   = "src/main.om"
///
/// [profile.debug]
/// opt_level        = 0
/// egraph           = false
/// superopt         = false
/// debug_info       = true
/// whole_program    = false
///
/// [profile.release]
/// opt_level        = 3
/// egraph           = true
/// superopt         = true
/// superopt_level   = 2
/// whole_program    = true
/// strip            = true
///
/// [dependencies]
/// my_lib = "../my_lib"
/// ```

#include "codegen.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace omscript {

/// Per-profile compiler settings.  Controls every optimization and codegen
/// flag that the build system passes to the underlying Compiler driver.
struct BuildProfile {
    /// LLVM optimization level (0–3).
    OptimizationLevel optLevel    = OptimizationLevel::O2;

    /// Enable E-graph equality-saturation (O2+ default: on).
    bool egraph                   = true;

    /// Enable the superoptimizer pass (O2+ default: on).
    bool superopt                 = true;

    /// Superoptimizer aggressiveness level (0 = off, 1–3 = increasing).
    unsigned superoptLevel        = 2;

    /// Emit DWARF debug info into the output binary.
    bool debugInfo                = false;

    /// Strip symbols from the output binary.
    bool strip                    = false;

    /// Enable full link-time optimization.
    bool lto                      = false;

    /// Enable unsafe floating-point math optimizations.
    bool fastMath                 = false;

    /// Enable OPTMAX block specialization.
    bool optMax                   = true;

    /// Emit SIMD vectorization hints.
    bool vectorize                = true;

    /// Emit loop-unrolling hints.
    bool unrollLoops              = true;

    /// Enable polyhedral-style loop optimizations.
    bool loopOptimize             = true;

    /// Enable automatic loop parallelization.
    bool parallelize              = true;

    /// Enable the Hardware Graph Optimization Engine.
    bool hgoe                     = true;

    /// Run the optimizer over the merged whole-program AST rather than
    /// per-file.  Enables cross-file inlining, DCE, and global constant
    /// propagation.  Implied by opt_level >= 2 in the release profile.
    bool wholeProgram             = false;

    /// Output directory for this profile relative to the project root,
    /// e.g. "target/debug" or "target/release".
    std::string outDir;

    // ── Factory helpers ──────────────────────────────────────────────────

    /// Default debug profile: O0, no heavy optimization, debug info on.
    static BuildProfile makeDebug();

    /// Default release profile: O3, whole-program, strip.
    static BuildProfile makeRelease();
};

/// Parsed representation of an oms.toml project manifest.
struct OmsManifest {
    /// Project name — also used as the output binary name.
    std::string name    = "project";

    /// Semantic version string (informational).
    std::string version = "0.1.0";

    /// Entry-point file path relative to the project root.
    std::string entry   = "src/main.om";

    /// Named build profiles; always contains at least "debug" and "release".
    std::map<std::string, BuildProfile> profiles;

    /// Local-path dependencies: logical name → directory path relative to
    /// the project root.  Remote/registry deps are reserved for future use.
    std::map<std::string, std::string> dependencies;

    /// Construct with default debug + release profiles.
    OmsManifest();
};

/// Parse an oms.toml file at @p path.
/// Throws std::runtime_error on I/O errors or fatal parse errors.
/// Unknown keys are silently ignored for forward compatibility.
OmsManifest parseOmsToml(const std::string& path);

// ── Project discovery ────────────────────────────────────────────────────────

/// Result of locating and loading an oms.toml project.
struct ProjectContext {
    /// Absolute path to the project root directory (contains oms.toml).
    std::string rootDir;

    /// Parsed manifest.
    OmsManifest manifest;

    /// True when loaded from a real oms.toml file; false for ephemeral
    /// (single-file) projects.
    bool isReal = false;
};

/// Walk up the directory tree starting at @p startDir searching for
/// oms.toml.  Returns the first found project context, or an empty
/// optional if none is found.
std::optional<ProjectContext> loadProjectContext(const std::string& startDir);

/// Wrap a single source file as an ephemeral (unnamed) project so that the
/// build system can treat single-file and project-mode uniformly.
ProjectContext makeEphemeralProject(const std::string& sourceFile,
                                    const std::string& profileName = "debug");

// ── Project initialisation ───────────────────────────────────────────────────

/// Create a new OmScript project in @p dir with the given @p name.
///
/// Creates:
///   <dir>/oms.toml        — project manifest with default debug/release profiles
///   <dir>/src/main.om     — minimal "Hello, <name>!" program
///
/// Returns false (after printing an error) if oms.toml already exists.
bool initProject(const std::string& dir, const std::string& name);

} // namespace omscript

#endif // PROJECT_H
