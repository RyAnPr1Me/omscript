/// @file project_test.cpp
/// @brief Unit tests for oms.toml manifest parsing, build graph fingerprinting,
///        and the project initialisation helper.

#include "build_graph.h"
#include "project.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace fs = std::filesystem;

// ── Helpers ──────────────────────────────────────────────────────────────────

/// Write @p content to a temporary file.  Returns the absolute path.
static std::string writeTmp(const std::string& filename,
                             const std::string& content) {
    const auto path = fs::temp_directory_path() / filename;
    std::ofstream f(path.string());
    f << content;
    return path.string();
}

/// RAII temporary directory.
struct TmpDir {
    fs::path path;
    explicit TmpDir(const std::string& prefix = "omsc_test_") {
        path = fs::temp_directory_path() /
               (prefix + std::to_string(
                              std::hash<std::string>{}(
                                  std::to_string(
                                      std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()))));
        fs::create_directories(path);
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// ── parseOmsToml ─────────────────────────────────────────────────────────────

TEST(ParseOmsToml, MinimalManifest) {
    const std::string toml = R"([project]
name    = "hello"
version = "1.2.3"
entry   = "src/main.om"
)";
    const auto path = writeTmp("min_oms.toml", toml);
    const auto m = omscript::parseOmsToml(path);
    EXPECT_EQ(m.name,    "hello");
    EXPECT_EQ(m.version, "1.2.3");
    EXPECT_EQ(m.entry,   "src/main.om");
    std::remove(path.c_str());
}

TEST(ParseOmsToml, DefaultProfilesPresent) {
    const std::string toml = R"([project]
name = "x"
)";
    const auto path = writeTmp("def_profiles_oms.toml", toml);
    const auto m = omscript::parseOmsToml(path);
    EXPECT_TRUE(m.profiles.count("debug"));
    EXPECT_TRUE(m.profiles.count("release"));
    std::remove(path.c_str());
}

TEST(ParseOmsToml, DebugProfileOverrides) {
    const std::string toml = R"([project]
name = "x"

[profile.debug]
opt_level  = 1
egraph     = true
superopt   = true
debug_info = false
)";
    const auto path = writeTmp("debug_override_oms.toml", toml);
    const auto m = omscript::parseOmsToml(path);
    const auto& dbg = m.profiles.at("debug");
    EXPECT_EQ(dbg.optLevel, omscript::OptimizationLevel::O1);
    EXPECT_TRUE(dbg.egraph);
    EXPECT_TRUE(dbg.superopt);
    EXPECT_FALSE(dbg.debugInfo);
    std::remove(path.c_str());
}

TEST(ParseOmsToml, ReleaseProfileDefaults) {
    const std::string toml = R"([project]
name = "x"
)";
    const auto path = writeTmp("rel_defaults_oms.toml", toml);
    const auto m = omscript::parseOmsToml(path);
    const auto& rel = m.profiles.at("release");
    EXPECT_EQ(rel.optLevel, omscript::OptimizationLevel::O3);
    EXPECT_TRUE(rel.egraph);
    EXPECT_TRUE(rel.superopt);
    EXPECT_TRUE(rel.wholeProgram);
    EXPECT_TRUE(rel.strip);
    EXPECT_EQ(rel.outDir, "target/release");
    std::remove(path.c_str());
}

TEST(ParseOmsToml, CustomProfile) {
    const std::string toml = R"([project]
name = "x"

[profile.bench]
opt_level = 3
egraph    = false
)";
    const auto path = writeTmp("custom_profile_oms.toml", toml);
    const auto m = omscript::parseOmsToml(path);
    EXPECT_TRUE(m.profiles.count("bench"));
    const auto& bench = m.profiles.at("bench");
    EXPECT_EQ(bench.optLevel, omscript::OptimizationLevel::O3);
    EXPECT_FALSE(bench.egraph);
    EXPECT_EQ(bench.outDir, "target/bench");
    std::remove(path.c_str());
}

TEST(ParseOmsToml, Dependencies) {
    const std::string toml = R"([project]
name = "app"

[dependencies]
mylib = "../mylib"
)";
    const auto path = writeTmp("deps_oms.toml", toml);
    const auto m = omscript::parseOmsToml(path);
    ASSERT_TRUE(m.dependencies.count("mylib"));
    EXPECT_EQ(m.dependencies.at("mylib"), "../mylib");
    std::remove(path.c_str());
}

TEST(ParseOmsToml, CommentsAndWhitespace) {
    const std::string toml = R"(
# Top-level comment

[project]
name = "trimmed"  # inline comment
version = "0.0.1" # another comment
entry   = "src/lib.om"
)";
    const auto path = writeTmp("comments_oms.toml", toml);
    const auto m = omscript::parseOmsToml(path);
    EXPECT_EQ(m.name,  "trimmed");
    EXPECT_EQ(m.entry, "src/lib.om");
    std::remove(path.c_str());
}

TEST(ParseOmsToml, MissingFileThrows) {
    EXPECT_THROW(
        omscript::parseOmsToml("/nonexistent/path/oms.toml"),
        std::runtime_error);
}

TEST(ParseOmsToml, SuperoptLevelAndStaticLink) {
    const std::string toml = R"([project]
name = "x"

[profile.release]
opt_level      = 3
superopt_level = 3
static         = true
stack_protector = true
)";
    const auto path = writeTmp("superopt_oms.toml", toml);
    const auto m = omscript::parseOmsToml(path);
    const auto& rel = m.profiles.at("release");
    EXPECT_EQ(rel.superoptLevel, 3u);
    EXPECT_TRUE(rel.staticLink);
    EXPECT_TRUE(rel.stackProtector);
    std::remove(path.c_str());
}

// ── BuildProfile defaults ─────────────────────────────────────────────────────

TEST(BuildProfile, DebugDefaults) {
    const auto p = omscript::BuildProfile::makeDebug();
    EXPECT_EQ(p.optLevel, omscript::OptimizationLevel::O0);
    EXPECT_FALSE(p.egraph);
    EXPECT_FALSE(p.superopt);
    EXPECT_TRUE(p.debugInfo);
    EXPECT_FALSE(p.strip);
    EXPECT_FALSE(p.wholeProgram);
    EXPECT_EQ(p.outDir, "target/debug");
}

TEST(BuildProfile, ReleaseDefaults) {
    const auto p = omscript::BuildProfile::makeRelease();
    EXPECT_EQ(p.optLevel, omscript::OptimizationLevel::O3);
    EXPECT_TRUE(p.egraph);
    EXPECT_TRUE(p.superopt);
    EXPECT_EQ(p.superoptLevel, 2u);
    EXPECT_TRUE(p.strip);
    EXPECT_TRUE(p.wholeProgram);
    EXPECT_EQ(p.outDir, "target/release");
}

// ── FNV-1a hashing ───────────────────────────────────────────────────────────

TEST(Fnv1a64, EmptyStringIsOffset) {
    // Empty string FNV-1a offset basis.
    EXPECT_EQ(omscript::fnv1a64(""), 14695981039346656037ULL);
}

TEST(Fnv1a64, DifferentInputsDifferentHash) {
    EXPECT_NE(omscript::fnv1a64("hello"), omscript::fnv1a64("world"));
}

TEST(Fnv1a64, Deterministic) {
    EXPECT_EQ(omscript::fnv1a64("omscript"), omscript::fnv1a64("omscript"));
}

TEST(Fnv1a64, SingleByteChange) {
    EXPECT_NE(omscript::fnv1a64("aaa"), omscript::fnv1a64("aab"));
}

// ── hashProfile / hashManifest ───────────────────────────────────────────────

TEST(HashProfile, DebugAndReleaseAreDifferent) {
    EXPECT_NE(omscript::hashProfile(omscript::BuildProfile::makeDebug()),
              omscript::hashProfile(omscript::BuildProfile::makeRelease()));
}

TEST(HashProfile, Deterministic) {
    const auto p = omscript::BuildProfile::makeRelease();
    EXPECT_EQ(omscript::hashProfile(p), omscript::hashProfile(p));
}

TEST(HashManifest, SameManifestSameHash) {
    omscript::OmsManifest m;
    m.name    = "test";
    m.version = "1.0.0";
    m.entry   = "src/main.om";
    EXPECT_EQ(omscript::hashManifest(m), omscript::hashManifest(m));
}

TEST(HashManifest, DifferentNameDifferentHash) {
    omscript::OmsManifest a, b;
    a.name = "foo";
    b.name = "bar";
    EXPECT_NE(omscript::hashManifest(a), omscript::hashManifest(b));
}

// ── BuildGraph ───────────────────────────────────────────────────────────────

TEST(BuildGraph, EmptyWhenEntryMissing) {
    omscript::BuildGraph g;
    g.load("/nonexistent/main.om", "/nonexistent");
    // A stub node is recorded even for missing files.
    // The graph is not considered empty because the entry itself is recorded.
    // Fingerprint must still be computable without crashing.
    const auto fp = g.computeFingerprint(0, 0);
    EXPECT_FALSE(fp.empty());
}

TEST(BuildGraph, LoadSingleFile) {
    TmpDir tmp;
    const auto src = (tmp.path / "main.om").string();
    {
        std::ofstream f(src);
        f << "fn main() { return 0; }\n";
    }
    omscript::BuildGraph g;
    g.load(src, tmp.path.string());
    EXPECT_FALSE(g.empty());
    EXPECT_EQ(g.nodes().size(), 1u);
}

TEST(BuildGraph, FingerprintChangesWhenContentChanges) {
    TmpDir tmp;
    const auto src = (tmp.path / "main.om").string();

    {
        std::ofstream f(src);
        f << "fn main() { return 0; }\n";
    }
    omscript::BuildGraph g1, g2;
    g1.load(src, tmp.path.string());
    const auto fp1 = g1.computeFingerprint(0, 0);

    {
        std::ofstream f(src);
        f << "fn main() { return 42; }\n"; // changed content
    }
    g2.load(src, tmp.path.string());
    const auto fp2 = g2.computeFingerprint(0, 0);

    EXPECT_NE(fp1, fp2);
}

TEST(BuildGraph, FingerprintChangesWithProfile) {
    TmpDir tmp;
    const auto src = (tmp.path / "main.om").string();
    { std::ofstream f(src); f << "fn main() { return 0; }\n"; }

    omscript::BuildGraph g;
    g.load(src, tmp.path.string());

    const auto fpDebug   = g.computeFingerprint(0,
        omscript::hashProfile(omscript::BuildProfile::makeDebug()));
    const auto fpRelease = g.computeFingerprint(0,
        omscript::hashProfile(omscript::BuildProfile::makeRelease()));

    EXPECT_NE(fpDebug, fpRelease);
}

TEST(BuildGraph, FingerprintIsDeterministic) {
    TmpDir tmp;
    const auto src = (tmp.path / "main.om").string();
    { std::ofstream f(src); f << "fn main() { return 0; }\n"; }

    omscript::BuildGraph g;
    g.load(src, tmp.path.string());
    const uint64_t mh = omscript::hashManifest(omscript::OmsManifest{});
    const uint64_t ph = omscript::hashProfile(omscript::BuildProfile::makeDebug());
    EXPECT_EQ(g.computeFingerprint(mh, ph), g.computeFingerprint(mh, ph));
}

TEST(BuildGraph, TopologicalOrderSingleFile) {
    TmpDir tmp;
    const auto src = (tmp.path / "main.om").string();
    { std::ofstream f(src); f << "fn main() { return 0; }\n"; }

    omscript::BuildGraph g;
    g.load(src, tmp.path.string());
    const auto order = g.topologicalOrder();
    ASSERT_EQ(order.size(), 1u);
}

// ── BuildCache ───────────────────────────────────────────────────────────────

TEST(BuildCache, RoundTripFingerprint) {
    TmpDir tmp;
    const auto cacheDir = (tmp.path / "cache").string();
    omscript::BuildCache cache(cacheDir);

    EXPECT_EQ(cache.loadFingerprint(), "");
    cache.saveFingerprint("deadbeef01234567");
    EXPECT_EQ(cache.loadFingerprint(), "deadbeef01234567");
}

TEST(BuildCache, OverwriteFingerprint) {
    TmpDir tmp;
    omscript::BuildCache cache((tmp.path / "c").string());
    cache.saveFingerprint("aaaaaaaaaaaa0001");
    cache.saveFingerprint("bbbbbbbbbbbb0002");
    EXPECT_EQ(cache.loadFingerprint(), "bbbbbbbbbbbb0002");
}

// ── initProject ──────────────────────────────────────────────────────────────

TEST(InitProject, CreatesManifestAndSource) {
    TmpDir tmp;
    const auto projDir = (tmp.path / "myapp").string();
    EXPECT_TRUE(omscript::initProject(projDir, "myapp"));

    EXPECT_TRUE(fs::exists(projDir + "/oms.toml"));
    EXPECT_TRUE(fs::exists(projDir + "/src/main.om"));
}

TEST(InitProject, ManifestIsParseable) {
    TmpDir tmp;
    const auto projDir = (tmp.path / "parseable").string();
    ASSERT_TRUE(omscript::initProject(projDir, "parseable"));

    const auto m = omscript::parseOmsToml(projDir + "/oms.toml");
    EXPECT_EQ(m.name, "parseable");
    EXPECT_EQ(m.entry, "src/main.om");
    EXPECT_TRUE(m.profiles.count("debug"));
    EXPECT_TRUE(m.profiles.count("release"));
}

TEST(InitProject, FailsIfManifestExists) {
    TmpDir tmp;
    // Create oms.toml manually first.
    { std::ofstream f((tmp.path / "oms.toml").string()); f << "[project]\n"; }
    EXPECT_FALSE(omscript::initProject(tmp.path.string(), "x"));
}

// ── loadProjectContext ───────────────────────────────────────────────────────

TEST(LoadProjectContext, FindsManifestInCwd) {
    TmpDir tmp;
    const std::string toml =
        "[project]\nname = \"found\"\nversion = \"0.1.0\"\n";
    { std::ofstream f((tmp.path / "oms.toml").string()); f << toml; }

    const auto ctx = omscript::loadProjectContext(tmp.path.string());
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->manifest.name, "found");
    EXPECT_TRUE(ctx->isReal);
}

TEST(LoadProjectContext, FindsManifestInParent) {
    TmpDir tmp;
    const std::string toml =
        "[project]\nname = \"parent\"\nversion = \"0.1.0\"\n";
    { std::ofstream f((tmp.path / "oms.toml").string()); f << toml; }

    // Create a subdirectory and start search from there.
    const auto sub = tmp.path / "src" / "deep";
    fs::create_directories(sub);
    const auto ctx = omscript::loadProjectContext(sub.string());
    ASSERT_TRUE(ctx.has_value());
    EXPECT_EQ(ctx->manifest.name, "parent");
}

TEST(LoadProjectContext, ReturnsNulloptWhenNotFound) {
    // /tmp itself should not contain oms.toml (at least not generally).
    // Use a fresh empty directory to be safe.
    TmpDir tmp;
    const auto ctx = omscript::loadProjectContext(tmp.path.string());
    EXPECT_FALSE(ctx.has_value());
}

// ── makeEphemeralProject ─────────────────────────────────────────────────────

TEST(MakeEphemeralProject, WrapsSourceFile) {
    const std::string src = "/tmp/hello.om";
    const auto ctx = omscript::makeEphemeralProject(src);
    EXPECT_FALSE(ctx.isReal);
    // Entry should be the filename.
    EXPECT_EQ(ctx.manifest.entry, "hello.om");
    // Project name should match the stem.
    EXPECT_EQ(ctx.manifest.name, "hello");
}
