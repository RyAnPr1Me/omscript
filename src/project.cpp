/// @file project.cpp
/// @brief OmsManifest construction, minimal TOML parser, and project init.

#include "project.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace omscript {

// ── BuildProfile defaults ────────────────────────────────────────────────────

BuildProfile BuildProfile::makeDebug() {
    BuildProfile p;
    p.optLevel      = OptimizationLevel::O0;
    p.egraph        = false;
    p.superopt      = false;
    p.superoptLevel = 0;
    p.debugInfo     = true;
    p.strip         = false;
    p.lto           = false;
    p.fastMath      = false;
    p.optMax        = true;
    p.vectorize     = false;
    p.unrollLoops   = false;
    p.loopOptimize  = false;
    p.parallelize   = false;
    p.hgoe          = false;
    p.wholeProgram  = false;
    p.outDir        = "target/debug";
    return p;
}

BuildProfile BuildProfile::makeRelease() {
    BuildProfile p;
    p.optLevel      = OptimizationLevel::O3;
    p.egraph        = true;
    p.superopt      = true;
    p.superoptLevel = 2;
    p.debugInfo     = false;
    p.strip         = true;
    p.lto           = false;
    p.fastMath      = false;
    p.optMax        = true;
    p.vectorize     = true;
    p.unrollLoops   = true;
    p.loopOptimize  = true;
    p.parallelize   = true;
    p.hgoe          = true;
    p.wholeProgram  = true;
    p.outDir        = "target/release";
    return p;
}

OmsManifest::OmsManifest() {
    profiles["debug"]   = BuildProfile::makeDebug();
    profiles["release"] = BuildProfile::makeRelease();
}

// ── Minimal TOML parser ──────────────────────────────────────────────────────
//
// Handles the subset of TOML needed for oms.toml:
//   • Comments:            # ...
//   • Section headers:     [section]   [profile.name]
//   • String values:       key = "value"
//   • Integer values:      key = 42
//   • Boolean values:      key = true  /  key = false
// Unknown keys and sections are silently ignored for forward compatibility.

namespace {

// Trim ASCII whitespace from both ends of @p s.
static std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Remove an inline comment from a value string.
// Handles the simple case: anything after a bare '#' outside a quoted string.
static std::string stripInlineComment(const std::string& s) {
    bool inStr = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' && (i == 0 || s[i - 1] != '\\')) inStr = !inStr;
        if (!inStr && c == '#') return s.substr(0, i);
    }
    return s;
}

// Parse a quoted string "value", setting @p out to the unquoted text.
// Returns false if the trimmed input is not a valid quoted string.
static bool parseTomlString(const std::string& val, std::string& out) {
    const auto s = trim(val);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        out = s.substr(1, s.size() - 2);
        return true;
    }
    return false;
}

// Parse a decimal integer, returning false on failure.
static bool parseTomlInt(const std::string& val, int64_t& out) {
    const auto s = trim(val);
    if (s.empty()) return false;
    try {
        size_t pos;
        out = std::stoll(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

// Parse "true" or "false", returning false on failure.
static bool parseTomlBool(const std::string& val, bool& out) {
    const auto s = trim(val);
    if (s == "true")  { out = true;  return true; }
    if (s == "false") { out = false; return true; }
    return false;
}

// Map a numeric opt_level (0–3) to OptimizationLevel.
static OptimizationLevel intToOptLevel(int64_t v) {
    switch (v) {
        case 0:  return OptimizationLevel::O0;
        case 1:  return OptimizationLevel::O1;
        case 2:  return OptimizationLevel::O2;
        default: return OptimizationLevel::O3;
    }
}

// Apply a (key, raw-value) pair to a BuildProfile.
static void applyProfileKey(BuildProfile& p,
                             const std::string& key,
                             const std::string& rawVal) {
    const std::string val = trim(stripInlineComment(rawVal));
    int64_t n  = 0;
    bool    b  = false;

    if (key == "opt_level") {
        if (parseTomlInt(val, n)) p.optLevel = intToOptLevel(n);
    } else if (key == "egraph") {
        if (parseTomlBool(val, b)) p.egraph = b;
    } else if (key == "superopt") {
        if (parseTomlBool(val, b)) p.superopt = b;
    } else if (key == "superopt_level") {
        if (parseTomlInt(val, n)) p.superoptLevel = static_cast<unsigned>(n);
    } else if (key == "debug_info" || key == "debug") {
        if (parseTomlBool(val, b)) p.debugInfo = b;
    } else if (key == "strip") {
        if (parseTomlBool(val, b)) p.strip = b;
    } else if (key == "lto") {
        if (parseTomlBool(val, b)) p.lto = b;
    } else if (key == "fast_math") {
        if (parseTomlBool(val, b)) p.fastMath = b;
    } else if (key == "optmax") {
        if (parseTomlBool(val, b)) p.optMax = b;
    } else if (key == "vectorize") {
        if (parseTomlBool(val, b)) p.vectorize = b;
    } else if (key == "unroll_loops") {
        if (parseTomlBool(val, b)) p.unrollLoops = b;
    } else if (key == "loop_optimize") {
        if (parseTomlBool(val, b)) p.loopOptimize = b;
    } else if (key == "parallelize") {
        if (parseTomlBool(val, b)) p.parallelize = b;
    } else if (key == "hgoe") {
        if (parseTomlBool(val, b)) p.hgoe = b;
    } else if (key == "whole_program") {
        if (parseTomlBool(val, b)) p.wholeProgram = b;
    }
    // Unknown keys are silently ignored for forward compatibility.
}

} // anonymous namespace

OmsManifest parseOmsToml(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open manifest file: " + path);
    }

    OmsManifest manifest;
    std::string line;

    // Active section: "" = top-level, "project", "dependencies",
    // "profile.debug", "profile.release", etc.
    std::string section;
    std::string currentProfile; // non-empty when section == "profile.*"

    while (std::getline(f, line)) {
        const std::string t = trim(line);

        // Skip blank lines and full-line comments.
        if (t.empty() || t[0] == '#') continue;

        // ── Section header ───────────────────────────────────────────────
        if (t.front() == '[' && t.back() == ']') {
            section = trim(t.substr(1, t.size() - 2));
            if (section.rfind("profile.", 0) == 0) {
                currentProfile = section.substr(8); // skip "profile."
                // Ensure the profile exists; initialise unknown profiles
                // from release defaults so authors only need to override
                // the values they care about.
                if (!manifest.profiles.count(currentProfile)) {
                    manifest.profiles[currentProfile] =
                        BuildProfile::makeRelease();
                    manifest.profiles[currentProfile].outDir =
                        "target/" + currentProfile;
                }
            } else {
                currentProfile.clear();
            }
            continue;
        }

        // ── Key = value pair ─────────────────────────────────────────────
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = trim(t.substr(0, eq));
        const std::string val = trim(stripInlineComment(t.substr(eq + 1)));

        if (section == "project") {
            std::string sv;
            if (key == "name"    && parseTomlString(val, sv)) manifest.name    = sv;
            if (key == "version" && parseTomlString(val, sv)) manifest.version = sv;
            if (key == "entry"   && parseTomlString(val, sv)) manifest.entry   = sv;

        } else if (section == "dependencies") {
            // Simple form:  name = "relative/path"
            std::string sv;
            if (parseTomlString(val, sv)) {
                manifest.dependencies[key] = sv;
            }

        } else if (!currentProfile.empty()) {
            applyProfileKey(manifest.profiles[currentProfile], key, val);
        }
        // Top-level keys outside a known section are silently ignored.
    }

    // Ensure every named profile has a correct outDir.
    for (auto& [pname, prof] : manifest.profiles) {
        if (prof.outDir.empty()) {
            prof.outDir = "target/" + pname;
        }
    }

    return manifest;
}

// ── Project discovery ────────────────────────────────────────────────────────

std::optional<ProjectContext> loadProjectContext(const std::string& startDir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto current = fs::absolute(startDir, ec);
    if (ec) return std::nullopt;

    while (true) {
        const auto candidate = current / "oms.toml";
        if (fs::exists(candidate, ec) && !ec) {
            try {
                ProjectContext ctx;
                ctx.rootDir  = current.string();
                ctx.manifest = parseOmsToml(candidate.string());
                ctx.isReal   = true;
                return ctx;
            } catch (const std::exception& ex) {
                std::cerr << "Warning: found oms.toml but failed to parse: "
                          << ex.what() << "\n";
                return std::nullopt;
            }
        }
        const auto parent = current.parent_path();
        if (parent == current) break; // reached filesystem root
        current = parent;
    }
    return std::nullopt;
}

ProjectContext makeEphemeralProject(const std::string& sourceFile,
                                    const std::string& /*profileName*/) {
    namespace fs = std::filesystem;
    ProjectContext ctx;
    std::error_code ec;
    const auto absPath = fs::absolute(sourceFile, ec);
    if (!ec) {
        ctx.rootDir           = absPath.parent_path().string();
        ctx.manifest.name     = absPath.stem().string();
        ctx.manifest.entry    = absPath.filename().string();
    } else {
        ctx.rootDir           = ".";
        ctx.manifest.name     = "ephemeral";
        ctx.manifest.entry    = sourceFile;
    }
    ctx.isReal = false;
    return ctx;
}

// ── Project initialisation ───────────────────────────────────────────────────

bool initProject(const std::string& dir, const std::string& name) {
    namespace fs = std::filesystem;
    std::error_code ec;

    const auto absDir = fs::absolute(dir, ec);
    if (ec) {
        std::cerr << "Error: cannot resolve directory '" << dir << "': "
                  << ec.message() << "\n";
        return false;
    }

    const auto manifestPath = absDir / "oms.toml";
    if (fs::exists(manifestPath, ec)) {
        std::cerr << "Error: oms.toml already exists in "
                  << absDir.string() << "\n";
        return false;
    }

    // Create project root and src/ subdirectory.
    fs::create_directories(absDir / "src", ec);
    if (ec) {
        std::cerr << "Error: cannot create '"
                  << (absDir / "src").string()
                  << "': " << ec.message() << "\n";
        return false;
    }

    // Write oms.toml.
    {
        std::ofstream toml(manifestPath.string());
        if (!toml) {
            std::cerr << "Error: cannot write " << manifestPath.string() << "\n";
            return false;
        }
        toml << "[project]\n"
             << "name    = \"" << name << "\"\n"
             << "version = \"0.1.0\"\n"
             << "entry   = \"src/main.om\"\n"
             << "\n"
             << "[profile.debug]\n"
             << "opt_level     = 0\n"
             << "egraph        = false\n"
             << "superopt      = false\n"
             << "debug_info    = true\n"
             << "whole_program = false\n"
             << "\n"
             << "[profile.release]\n"
             << "opt_level     = 3\n"
             << "egraph        = true\n"
             << "superopt      = true\n"
             << "superopt_level = 2\n"
             << "whole_program = true\n"
             << "strip         = true\n"
             << "\n"
             << "# [dependencies]\n"
             << "# my_lib = \"../my_lib\"\n";
    }

    // Write src/main.om — only if it does not already exist.
    const auto mainPath = absDir / "src" / "main.om";
    if (!fs::exists(mainPath, ec)) {
        std::ofstream main(mainPath.string());
        if (!main) {
            std::cerr << "Error: cannot write " << mainPath.string() << "\n";
            return false;
        }
        main << "fn main() {\n"
             << "    println(\"Hello, " << name << "!\");\n"
             << "    return 0;\n"
             << "}\n";
    }

    std::cout << "Created project '" << name << "' at "
              << absDir.string() << "\n"
              << "  oms.toml\n"
              << "  src/main.om\n"
              << "\nRun 'omsc build' inside the project directory to compile.\n";
    return true;
}

} // namespace omscript
