/// @file build_graph.cpp
/// @brief Import scanner, dependency graph traversal, and build cache.

#include "build_graph.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace omscript {

// ── FNV-1a 64-bit ───────────────────────────────────────────────────────────

uint64_t fnv1a64(const std::string& data) noexcept {
    constexpr uint64_t kOffset = 14695981039346656037ULL;
    constexpr uint64_t kPrime  = 1099511628211ULL;
    uint64_t h = kOffset;
    for (const unsigned char c : data) {
        h ^= static_cast<uint64_t>(c);
        h *= kPrime;
    }
    return h;
}

// ── Profile / manifest hashing ───────────────────────────────────────────────

uint64_t hashProfile(const BuildProfile& p) {
    std::ostringstream ss;
    ss << static_cast<int>(p.optLevel)
       << static_cast<int>(p.egraph)
       << static_cast<int>(p.superopt)
       << p.superoptLevel
       << static_cast<int>(p.debugInfo)
       << static_cast<int>(p.strip)
       << static_cast<int>(p.lto)
       << static_cast<int>(p.fastMath)
       << static_cast<int>(p.optMax)
       << static_cast<int>(p.vectorize)
       << static_cast<int>(p.unrollLoops)
       << static_cast<int>(p.loopOptimize)
       << static_cast<int>(p.parallelize)
       << static_cast<int>(p.hgoe)
       << static_cast<int>(p.wholeProgram)
       << p.outDir;
    return fnv1a64(ss.str());
}

uint64_t hashManifest(const OmsManifest& m) {
    std::ostringstream ss;
    ss << m.name << '\0' << m.version << '\0' << m.entry << '\0';
    for (const auto& [k, v] : m.dependencies) {
        ss << k << '\0' << v << '\0';
    }
    return fnv1a64(ss.str());
}

// ── Import scanner ───────────────────────────────────────────────────────────
// Extracts `import "filename"` paths from source text without running the
// full lexer.  This is intentionally conservative: it only looks for the
// literal sequence  import  followed by a double-quoted string, ensuring
// it does not pick up occurrences inside string literals or comments.

static std::vector<std::string> scanImports(const std::string& source) {
    std::vector<std::string> imports;
    const std::string needle = "import";
    const size_t kLen = needle.size();
    size_t pos = 0;

    while ((pos = source.find(needle, pos)) != std::string::npos) {
        // Guard: must be preceded by a word boundary (not alphanumeric / '_').
        if (pos > 0) {
            const unsigned char prev =
                static_cast<unsigned char>(source[pos - 1]);
            if (std::isalnum(prev) || prev == '_') {
                pos += kLen;
                continue;
            }
        }
        // Guard: must be followed by a word boundary.
        size_t after = pos + kLen;
        if (after < source.size()) {
            const unsigned char next =
                static_cast<unsigned char>(source[after]);
            if (std::isalnum(next) || next == '_') {
                pos = after;
                continue;
            }
        }

        // Skip whitespace after keyword.
        pos = after;
        while (pos < source.size() &&
               (source[pos] == ' ' || source[pos] == '\t')) {
            ++pos;
        }

        // Expect opening '"'.
        if (pos >= source.size() || source[pos] != '"') continue;
        ++pos;

        const auto end = source.find('"', pos);
        if (end == std::string::npos) continue;

        const std::string importPath = source.substr(pos, end - pos);
        if (!importPath.empty()) {
            imports.push_back(importPath);
        }
        pos = end + 1;
    }

    return imports;
}

// ── BuildGraph ───────────────────────────────────────────────────────────────

void BuildGraph::scanFile(const std::string& absPath,
                          const std::string& projectDir) {
    namespace fs = std::filesystem;

    // Guard against revisiting (handles circular imports safely).
    if (nodes_.count(absPath)) return;

    // Reserve the slot immediately to prevent infinite recursion on cycles.
    nodes_[absPath] = BuildNode{absPath, 0, {}};

    // Read file content.
    std::ifstream f(absPath);
    if (!f.is_open()) {
        // File missing — record a stub with a zero hash so the fingerprint
        // still changes when the file reappears.
        return;
    }
    const std::string content((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());

    BuildNode node;
    node.path        = absPath;
    node.contentHash = fnv1a64(content);

    // Resolve each import path to an absolute canonical path.
    std::error_code ec;
    const auto fileDir = fs::path(absPath).parent_path();

    for (const auto& imp : scanImports(content)) {
        // 1. Relative to the importing file's directory.
        auto candidate = fileDir / imp;
        if (fs::exists(candidate, ec)) {
            auto canon = fs::canonical(candidate, ec);
            if (!ec) {
                node.imports.push_back(canon.string());
                continue;
            }
        }
        // 2. Relative to the project root.
        candidate = fs::path(projectDir) / imp;
        if (fs::exists(candidate, ec)) {
            auto canon = fs::canonical(candidate, ec);
            if (!ec) {
                node.imports.push_back(canon.string());
            }
        }
    }

    nodes_[absPath] = std::move(node);

    // Recurse into discovered imports.
    for (const auto& imp : nodes_.at(absPath).imports) {
        scanFile(imp, projectDir);
    }
}

void BuildGraph::load(const std::string& entryPath,
                      const std::string& projectDir) {
    namespace fs = std::filesystem;
    nodes_.clear();
    entryPath_.clear();

    std::error_code ec;
    auto absEntry = fs::absolute(entryPath, ec);
    if (ec) return;

    // Try to canonicalise (resolves symlinks).  Fall back to absolute if
    // the file does not exist yet.
    auto canon = fs::weakly_canonical(absEntry, ec);
    entryPath_ = ec ? absEntry.string() : canon.string();

    scanFile(entryPath_, projectDir);
}

std::vector<std::string> BuildGraph::topologicalOrder() const {
    if (nodes_.empty()) return {};

    std::vector<std::string> order;
    order.reserve(nodes_.size());
    std::unordered_set<std::string> visited;
    std::unordered_set<std::string> onStack; // cycle guard

    // DFS: post-order gives dependencies before dependents.
    std::function<void(const std::string&)> dfs =
        [&](const std::string& path) {
            if (visited.count(path)) return;
            onStack.insert(path);
            const auto it = nodes_.find(path);
            if (it != nodes_.end()) {
                for (const auto& imp : it->second.imports) {
                    if (!onStack.count(imp)) dfs(imp);
                }
            }
            onStack.erase(path);
            visited.insert(path);
            order.push_back(path);
        };

    if (!entryPath_.empty()) dfs(entryPath_);

    // Catch any nodes not reachable from the entry (shouldn't happen
    // normally, but be safe).
    for (const auto& [path, _] : nodes_) {
        if (!visited.count(path)) dfs(path);
    }

    return order;
}

std::string BuildGraph::computeFingerprint(uint64_t manifestHash,
                                           uint64_t profileHash) const {
    // Seed with manifest and profile hashes.
    uint64_t combined = hashCombine(manifestHash, profileHash);

    // Mix in content hashes in sorted path order to ensure reproducibility
    // regardless of map iteration or discovery order.
    std::vector<std::string> paths;
    paths.reserve(nodes_.size());
    for (const auto& [p, _] : nodes_) paths.push_back(p);
    std::sort(paths.begin(), paths.end());

    for (const auto& p : paths) {
        combined = hashCombine(combined, fnv1a64(p));
        combined = hashCombine(combined, nodes_.at(p).contentHash);
    }

    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << combined;
    return ss.str();
}

// ── BuildCache ───────────────────────────────────────────────────────────────

BuildCache::BuildCache(std::string cacheDir)
    : cacheDir_(std::move(cacheDir)) {
    fingerprintFile_ = cacheDir_ + "/.fingerprint";
}

bool BuildCache::ensureDir() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(cacheDir_, ec);
    return !ec;
}

std::string BuildCache::loadFingerprint() const {
    std::ifstream f(fingerprintFile_);
    if (!f.is_open()) return "";
    std::string line;
    if (!std::getline(f, line)) return "";
    // Strip trailing whitespace.
    const auto last = line.find_last_not_of(" \t\r\n");
    return (last == std::string::npos) ? "" : line.substr(0, last + 1);
}

void BuildCache::saveFingerprint(const std::string& fp) const {
    if (!ensureDir()) return;
    std::ofstream f(fingerprintFile_);
    if (f) f << fp << "\n";
}

} // namespace omscript
