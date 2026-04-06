#pragma once

#ifndef COMPILER_H
#define COMPILER_H

#include "codegen.h"
#include <string>

namespace omscript {

class Compiler {
  public:
    /// Maximum allowed length for source/output file paths.
    static constexpr size_t kMaxPathLength = 4096;

    /// Maximum allowed source file size in bytes (100 MB).
    static constexpr size_t kMaxFileSize = size_t{100} * 1024 * 1024;

    Compiler() = default;

    void compile(const std::string& sourceFile, const std::string& outputFile);
    void writeFile(const std::string& filename, const std::string& content);

    /// Control verbosity — when true, prints LLVM IR and progress messages.
    void setVerbose(bool v) {
        verbose_ = v;
    }
    [[nodiscard]] bool isVerbose() const noexcept {
        return verbose_;
    }

    /// Control quiet mode — when true, suppresses non-error output.
    void setQuiet(bool q) {
        quiet_ = q;
    }
    [[nodiscard]] bool isQuiet() const noexcept {
        return quiet_;
    }

    /// Set the LLVM optimization level used during code generation.
    void setOptimizationLevel(OptimizationLevel level) {
        optLevel_ = level;
    }
    [[nodiscard]] OptimizationLevel getOptimizationLevel() const noexcept {
        return optLevel_;
    }

    /// Target CPU architecture for instruction selection.
    /// Empty string or "native" triggers host auto-detection (default).
    void setMarch(const std::string& cpu) {
        march_ = cpu;
    }
    /// CPU model for scheduling tuning (empty = same as -march).
    void setMtune(const std::string& cpu) {
        mtune_ = cpu;
    }
    /// Enable or disable position-independent code generation (default: true).
    void setPIC(bool enable) {
        pic_ = enable;
    }
    /// Enable or disable unsafe floating-point optimizations (default: false).
    void setFastMath(bool enable) {
        fastMath_ = enable;
    }
    /// Enable or disable OPTMAX block optimization (default: true).
    void setOptMax(bool enable) {
        optMax_ = enable;
    }
    /// Enable or disable adaptive JIT runtime for hot function recompilation (default: true).
    void setJIT(bool enable) {
        jit_ = enable;
    }
    /// Enable or disable link-time optimization (default: false).
    void setLTO(bool enable) {
        lto_ = enable;
    }
    /// Enable or disable static linking (default: false).
    void setStaticLinking(bool enable) {
        staticLink_ = enable;
    }
    /// Enable or disable symbol stripping from output (default: false).
    void setStrip(bool enable) {
        strip_ = enable;
    }
    /// Enable or disable stack protector (default: false).
    void setStackProtector(bool enable) {
        stackProtector_ = enable;
    }
    /// Enable or disable SIMD vectorization hints (default: true).
    void setVectorize(bool enable) {
        vectorize_ = enable;
    }
    /// Enable or disable loop unrolling hints (default: true).
    void setUnrollLoops(bool enable) {
        unrollLoops_ = enable;
    }
    /// Enable or disable polyhedral-style loop optimizations (default: true).
    void setLoopOptimize(bool enable) {
        loopOptimize_ = enable;
    }
    /// Enable or disable automatic loop parallelization (default: true).
    void setParallelize(bool enable) {
        parallelize_ = enable;
    }
    /// Enable PGO instrumentation generation.
    /// The compiled binary will write a raw profile to @p path on exit.
    void setPGOGen(const std::string& path) {
        pgoGenPath_ = path;
    }
    /// Enable PGO profile-use optimizations from a previously-collected profile.
    void setPGOUse(const std::string& path) {
        pgoUsePath_ = path;
    }
    /// Enable or disable DWARF debug info generation (default: false).
    void setDebugMode(bool enable) {
        debug_ = enable;
    }
    /// Enable or disable e-graph equality saturation (default: true at O2+).
    void setEGraphOptimize(bool enable) {
        egraph_ = enable;
    }
    /// Enable or disable the superoptimizer pass (default: true at O2+).
    void setSuperoptimize(bool enable) {
        superopt_ = enable;
    }
    /// Set superoptimizer aggressiveness level 0-3 (default: 2).
    void setSuperoptLevel(int level) {
        superoptLevel_ = level;
    }
    /// Enable or disable hardware graph optimization engine (default: true).
    void setHardwareGraphOpt(bool enable) {
        hgoe_ = enable;
    }

  private:
    std::string readFile(const std::string& filename);
    bool verbose_ = false;
    bool quiet_ = false;
    OptimizationLevel optLevel_ = OptimizationLevel::O2;
    std::string march_;
    std::string mtune_;
    bool pic_ = true;
    bool fastMath_ = false;
    bool optMax_ = true;
    bool jit_ = true;
    bool lto_ = false;
    bool staticLink_ = false;
    bool strip_ = false;
    bool stackProtector_ = false;
    bool vectorize_ = true;
    bool unrollLoops_ = true;
    bool loopOptimize_ = true;
    bool parallelize_ = true;
    bool debug_ = false;
    bool egraph_ = true;
    bool superopt_ = true;
    int superoptLevel_ = 2;
    bool hgoe_ = true;
    std::string pgoGenPath_;
    std::string pgoUsePath_;
};

} // namespace omscript

#endif // COMPILER_H
