/// @file build_system.cpp
/// @brief BuildSystem: project-aware, incremental whole-program compiler driver.

#include "build_system.h"
#include "compiler.h"
#include <chrono>
#include <filesystem>
#include <iostream>

namespace omscript {

BuildSystem::BuildSystem(std::string projectDir, std::string profileName)
    : projectDir_(std::move(projectDir))
    , profileName_(std::move(profileName)) {}

// ── prepare() ────────────────────────────────────────────────────────────────

bool BuildSystem::prepare(const BuildIO& io) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Load manifest (oms.toml), falling back to defaults when absent.
    const auto manifestPath =
        (fs::path(projectDir_) / "oms.toml").string();
    if (fs::exists(manifestPath, ec) && !ec) {
        try {
            manifest_ = parseOmsToml(manifestPath);
        } catch (const std::exception& ex) {
            std::cerr << "Error: failed to parse oms.toml: "
                      << ex.what() << "\n";
            return false;
        }
    }
    // If oms.toml is absent manifest_ retains its default-constructed values.

    // Select profile.
    const auto profIt = manifest_.profiles.find(profileName_);
    if (profIt == manifest_.profiles.end()) {
        std::cerr << "Error: unknown build profile '" << profileName_ << "'\n";
        std::cerr << "Available profiles:";
        for (const auto& [n, _] : manifest_.profiles) std::cerr << " " << n;
        std::cerr << "\n";
        return false;
    }
    profile_ = profIt->second;

    // Derive output paths.
    targetDir_  = (fs::path(projectDir_) / profile_.outDir).string();
    cacheDir_   = (fs::path(targetDir_) / "cache").string();
    outputPath_ = (fs::path(targetDir_) / manifest_.name).string();

    // Build the import-dependency graph rooted at the entry file.
    const auto entryPath =
        (fs::path(projectDir_) / manifest_.entry).string();
    graph_.load(entryPath, projectDir_);

    if (graph_.empty() && !io.quiet) {
        std::cerr << "Warning: entry file '" << manifest_.entry
                  << "' not found relative to project root '"
                  << projectDir_ << "'.\n";
    }

    prepared_ = true;
    return true;
}

// ── build() ──────────────────────────────────────────────────────────────────

BuildResult BuildSystem::build(const BuildIO& io) {
    BuildResult result;

    if (!prepared_ && !prepare(io)) {
        result.errorMessage = "Build system preparation failed";
        return result;
    }

    namespace fs = std::filesystem;
    std::error_code ec;

    // Compute the combined build fingerprint.
    const auto mHash = hashManifest(manifest_);
    const auto pHash = hashProfile(profile_);
    const auto fp    = graph_.computeFingerprint(mHash, pHash);

    BuildCache cache(cacheDir_);
    const std::string cachedFp  = cache.loadFingerprint();
    const bool        binExists = fs::exists(outputPath_, ec) && !ec;

    if (cachedFp == fp && binExists) {
        // Nothing changed — report up-to-date without recompiling.
        if (!io.quiet) {
            std::cout << "    Finished `" << profileName_ << "` profile"
                         " [already up-to-date]\n";
        }
        result.success    = true;
        result.upToDate   = true;
        result.outputPath = outputPath_;
        return result;
    }

    // Ensure the target directory exists.
    fs::create_directories(targetDir_, ec);
    if (ec) {
        result.errorMessage =
            "Cannot create target directory '" + targetDir_ +
            "': " + ec.message();
        return result;
    }

    const auto entryPath =
        (fs::path(projectDir_) / manifest_.entry).string();

    if (!io.quiet) {
        std::cout << "   Compiling " << manifest_.name
                  << " v" << manifest_.version
                  << " (" << profileName_ << ")\n";
    }

    const auto t0 = std::chrono::steady_clock::now();
    if (!runCompiler(entryPath, outputPath_, io)) {
        result.errorMessage = "Compilation failed";
        return result;
    }
    const double ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();

    // Persist the new fingerprint so the next build can detect up-to-date.
    cache.saveFingerprint(fp);

    if (!io.quiet) {
        std::cout << "    Finished `" << profileName_
                  << "` profile [optimized] target(s) in "
                  << static_cast<int>(ms / 1000.0) << "."
                  << static_cast<int>(ms) % 1000 << "s\n";
    }

    result.success    = true;
    result.outputPath = outputPath_;
    return result;
}

// ── clean() ──────────────────────────────────────────────────────────────────

bool BuildSystem::clean(const BuildIO& io) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // Resolve targetDir_ even if prepare() was never called.
    std::string dir = targetDir_;
    if (dir.empty()) {
        const auto profIt = manifest_.profiles.find(profileName_);
        if (profIt != manifest_.profiles.end() &&
            !profIt->second.outDir.empty()) {
            dir = (fs::path(projectDir_) / profIt->second.outDir).string();
        } else {
            // Fall back to conventional target/<profile> path.
            dir = (fs::path(projectDir_) / "target" / profileName_).string();
        }
    }

    if (!fs::exists(dir, ec) || ec) {
        if (!io.quiet) std::cout << "Nothing to clean.\n";
        return true;
    }

    fs::remove_all(dir, ec);
    if (ec) {
        std::cerr << "Error: cannot remove '" << dir << "': "
                  << ec.message() << "\n";
        return false;
    }
    if (!io.quiet) std::cout << "  Removed " << dir << "\n";
    return true;
}

// ── runCompiler() ────────────────────────────────────────────────────────────

bool BuildSystem::runCompiler(const std::string& entryPath,
                               const std::string& outPath,
                               const BuildIO& io) {
    Compiler compiler;
    compiler.setVerbose(io.verbose);
    compiler.setQuiet(io.quiet);
    compiler.setOptimizationLevel(profile_.optLevel);
    compiler.setPIC(true);
    compiler.setFastMath(profile_.fastMath);
    compiler.setOptMax(profile_.optMax);
    compiler.setLTO(profile_.lto);
    compiler.setStrip(profile_.strip);
    compiler.setVectorize(profile_.vectorize);
    compiler.setUnrollLoops(profile_.unrollLoops);
    compiler.setLoopOptimize(profile_.loopOptimize);
    compiler.setParallelize(profile_.parallelize);
    compiler.setDebugMode(profile_.debugInfo);
    compiler.setEGraphOptimize(profile_.egraph);
    compiler.setSuperoptimize(profile_.superopt);
    compiler.setSuperoptLevel(profile_.superoptLevel);
    compiler.setHardwareGraphOpt(profile_.hgoe);
    compiler.setStaticLinking(profile_.staticLink);
    compiler.setStackProtector(profile_.stackProtector);
    if (!marchCpu_.empty())   compiler.setMarch(marchCpu_);
    if (!mtuneCpu_.empty())   compiler.setMtune(mtuneCpu_);
    if (!pgoGenPath_.empty()) compiler.setPGOGen(pgoGenPath_);
    if (!pgoUsePath_.empty()) compiler.setPGOUse(pgoUsePath_);

    try {
        compiler.compile(entryPath, outPath);
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return false;
    }
}

} // namespace omscript
