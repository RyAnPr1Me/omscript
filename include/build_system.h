#pragma once

#ifndef BUILD_SYSTEM_H
#define BUILD_SYSTEM_H

/// @file build_system.h
/// @brief High-level build orchestrator for OmScript projects.
///
/// BuildSystem is the primary driver for project-oriented compilation.  It:
///
///   1. Loads and validates the oms.toml manifest.
///   2. Builds the source-file dependency graph via BuildGraph.
///   3. Computes a combined build fingerprint (source hashes + profile hash).
///   4. Skips recompilation when the fingerprint matches the cached value and
///      the output binary already exists (incremental build).
///   5. Invokes the Compiler driver with profile-specific settings for a full
///      whole-program build when needed.
///   6. Persists the new fingerprint on success.
///
/// The compiler frontend (Preprocessor → Lexer → Parser) already merges all
/// `import "..."` files into a single program AST, so whole-program
/// optimization (E-graph, CF-CTRE, purity analysis, HGOE) runs over the
/// complete merged call graph automatically.

#include "build_graph.h"
#include "project.h"
#include <string>

namespace omscript {

/// I/O verbosity settings forwarded to the compiler and printed messages.
struct BuildIO {
    bool verbose = false; ///< Forward -V to the compiler; print pass details.
    bool quiet   = false; ///< Suppress non-error output.
    bool timing  = false; ///< Show timing breakdown after compilation.
};

/// Result returned by BuildSystem::build().
struct BuildResult {
    bool        success      = false; ///< True on successful compilation.
    bool        upToDate     = false; ///< True when build was skipped (cache hit).
    std::string outputPath;           ///< Absolute path to the produced binary.
    std::string errorMessage;         ///< Non-empty on failure.
};

/// Project-aware build orchestrator.
///
/// Typical usage:
/// ```cpp
/// BuildSystem bs(projectDir, "release");
/// if (!bs.prepare(io)) return 1;
/// auto result = bs.build(io);
/// ```
class BuildSystem {
public:
    /// @param projectDir   Absolute path to the project root.
    /// @param profileName  Profile name to activate ("debug" or "release").
    BuildSystem(std::string projectDir, std::string profileName);

    /// Load the manifest, select the profile, and initialise the build graph.
    /// Must be called before build() or clean().
    ///
    /// Returns false and prints an error message on failure.
    bool prepare(const BuildIO& io = {});

    /// Execute a build (potentially incremental).
    ///
    /// Returns a BuildResult with:
    ///   • success  = true on a successful full build or a cache hit.
    ///   • upToDate = true when compilation was skipped (cache hit).
    ///   • outputPath = absolute path to the binary on success.
    BuildResult build(const BuildIO& io = {});

    /// Remove the target/<profile>/ directory and everything inside it.
    bool clean(const BuildIO& io = {});

    // ── Accessors ────────────────────────────────────────────────────────

    /// Return the active BuildProfile (valid after prepare()).
    const BuildProfile& profile() const noexcept { return profile_; }

    /// Return the parsed manifest (valid after prepare()).
    const OmsManifest& manifest() const noexcept { return manifest_; }

    /// Return the output binary path computed during prepare().
    const std::string& outputPath() const noexcept { return outputPath_; }

    /// Return the project root directory.
    const std::string& projectDir() const noexcept { return projectDir_; }

private:
    /// Invoke Compiler::compile() with profile-derived settings.
    bool runCompiler(const std::string& entryPath,
                     const std::string& outPath,
                     const BuildIO& io);

    std::string  projectDir_;
    std::string  profileName_;
    OmsManifest  manifest_;
    BuildProfile profile_;
    BuildGraph   graph_;
    std::string  outputPath_; ///< <targetDir_>/<manifest_.name>
    std::string  targetDir_;  ///< <projectDir_>/target/<profileName_>
    std::string  cacheDir_;   ///< <targetDir_>/cache
    bool         prepared_ = false;
};

} // namespace omscript

#endif // BUILD_SYSTEM_H
