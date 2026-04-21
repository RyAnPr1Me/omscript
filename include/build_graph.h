#pragma once

#ifndef BUILD_GRAPH_H
#define BUILD_GRAPH_H

/// @file build_graph.h
/// @brief Dependency graph, fingerprinting, and persistent build cache.
///
/// The BuildGraph tracks every source file reachable from the project entry
/// point (following `import "..."` statements transitively) and computes a
/// stable 64-bit FNV-1a content hash for each file.  The combined
/// fingerprint — a hex string mixing all content hashes, the manifest hash,
/// and the profile hash — is persisted in `target/<profile>/cache/.fingerprint`
/// so that incremental rebuilds can skip recompilation when nothing changed.
///
/// Fingerprint semantics
/// =====================
/// A build is considered up-to-date when:
///   1. The fingerprint file exists and matches the current fingerprint, AND
///   2. The output binary exists.
/// Any change (source content, oms.toml, or profile settings) invalidates
/// the fingerprint and triggers a full recompile.  Future versions may track
/// individual file changes for finer-grained incremental builds.

#include "project.h"
#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

namespace omscript {

// ── Hashing primitives ───────────────────────────────────────────────────────

/// Compute a 64-bit FNV-1a hash of @p data.
uint64_t fnv1a64(const std::string& data) noexcept;

/// Combine two hash values deterministically (inspired by boost::hash_combine).
inline uint64_t hashCombine(uint64_t h1, uint64_t h2) noexcept {
    return h1 ^ (h2 * 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
}

/// Compute a hash capturing all build-relevant fields of @p profile.
uint64_t hashProfile(const BuildProfile& profile);

/// Compute a hash of the manifest fields that affect compilation.
uint64_t hashManifest(const OmsManifest& manifest);

// ── Build graph node ─────────────────────────────────────────────────────────

/// A single node in the build graph — one OmScript source file.
struct BuildNode {
    /// Absolute (canonical) path to the source file.
    std::string path;

    /// FNV-1a 64-bit hash of the file's byte content.
    uint64_t contentHash = 0;

    /// Absolute paths of files directly imported by this source file.
    std::vector<std::string> imports;
};

// ── BuildGraph ───────────────────────────────────────────────────────────────

/// Dependency and fingerprint graph for an OmScript project.
///
/// Call load() to walk the import graph from the entry point, then call
/// computeFingerprint() to obtain the reproducible build fingerprint.
class BuildGraph {
public:
    /// Walk the import graph rooted at @p entryPath, scanning each file's
    /// `import "..."` statements to discover dependencies transitively.
    ///
    /// @p projectDir  Project root used to resolve relative import paths.
    void load(const std::string& entryPath, const std::string& projectDir);

    /// Return all discovered source files in topological order
    /// (dependencies before the files that depend on them; entry last).
    std::vector<std::string> topologicalOrder() const;

    /// Compute the overall build fingerprint as a 16-hex-digit string.
    ///
    /// The fingerprint covers:
    ///   • Content hashes of all reachable source files (sorted for
    ///     determinism regardless of discovery order).
    ///   • @p manifestHash — result of hashManifest().
    ///   • @p profileHash  — result of hashProfile().
    std::string computeFingerprint(uint64_t manifestHash,
                                   uint64_t profileHash) const;

    /// Direct read access to the node map (path → BuildNode).
    const std::map<std::string, BuildNode>& nodes() const noexcept {
        return nodes_;
    }

    /// True when no source files were discovered (e.g. entry not found).
    bool empty() const noexcept { return nodes_.empty(); }

private:
    /// Recursively scan @p absPath, recording its content hash and imports.
    /// Already-visited paths are skipped to handle circular imports safely.
    void scanFile(const std::string& absPath, const std::string& projectDir);

    std::map<std::string, BuildNode> nodes_;
    std::string                      entryPath_;
};

// ── BuildCache ───────────────────────────────────────────────────────────────

/// Persistent build cache stored in a directory (typically
/// `<project>/target/<profile>/cache/`).
///
/// Currently caches only the build fingerprint.  Future versions may also
/// cache serialised AST facts and CF-CTRE memoisation tables.
class BuildCache {
public:
    explicit BuildCache(std::string cacheDir);

    /// Read and return the persisted fingerprint, or "" when absent.
    std::string loadFingerprint() const;

    /// Write @p fp to the fingerprint file (creates directories if needed).
    void saveFingerprint(const std::string& fp) const;

    /// Ensure the cache directory exists on disk.
    bool ensureDir() const;

    /// Return the cache directory path.
    const std::string& dir() const noexcept { return cacheDir_; }

private:
    std::string cacheDir_;
    std::string fingerprintFile_;
};

} // namespace omscript

#endif // BUILD_GRAPH_H
