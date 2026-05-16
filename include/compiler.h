#pragma once

#ifndef COMPILER_H
#define COMPILER_H

#include "codegen.h"
#include "diagnostic.h"
#include <functional>
#include <string>
#include <unordered_map>

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

    /// Enable or disable E-graph equality-saturation (default: true at O2+).
    void setEGraphOptimize(bool enable) {
        egraph_ = enable;
    }

    /// Enable or disable the superoptimizer pass (default: true at O2+).
    void setSuperoptimize(bool enable) {
        superopt_ = enable;
    }

    /// Set superoptimizer aggressiveness 0–3 (default: 2).
    void setSuperoptLevel(unsigned level) {
        superoptLevel_ = level;
        superopt_ = (level > 0);
    }

    /// Enable or disable the Hardware Graph Optimization Engine (default: true).
    void setHardwareGraphOpt(bool enable) {
        hgoe_ = enable;
    }

    /// Enable or disable Speculative Devectorization & Revectorization (default: true).
    void setSDR(bool enable) {
        sdr_ = enable;
    }

    /// Enable or disable the Implicit Phase Ordering Fixer (default: true).
    void setIPOF(bool enable) {
        ipof_ = enable;
    }

    /// Disable all ownership/borrow safety checks (Ω spec §6.2: --no-ownership-checks).
    /// When true: borrow checker is skipped and codegen ownership validation is bypassed.
    void setNoOwnershipChecks(bool enable) {
        noOwnershipChecks_ = enable;
    }
    [[nodiscard]] bool isNoOwnershipChecks() const noexcept {
        return noOwnershipChecks_;
    }

    /// Enable compile-time path-sensitive memory-safety diagnostics (Ω spec §7: --mem-sanitize).
    void setMemSanitize(bool enable) {
        memSanitize_ = enable;
    }
    [[nodiscard]] bool isMemSanitize() const noexcept {
        return memSanitize_;
    }

    /// Inject an integer comptime define (equivalent to `comptime { const NAME: int = VALUE; }`).
    /// Used to implement the -D NAME[=VALUE] command-line flag.
    void addDefine(const std::string& name, long long value = 1) {
        defines_[name] = value;
    }

    /// Inject a string comptime define (equivalent to `comptime { const NAME: string = VALUE; }`).
    void addDefineString(const std::string& name, const std::string& value) {
        stringDefines_[name] = value;
    }

    [[nodiscard]] const std::unordered_map<std::string, long long>& defines() const noexcept {
        return defines_;
    }
    [[nodiscard]] const std::unordered_map<std::string, std::string>& stringDefines() const noexcept {
        return stringDefines_;
    }

    /// Register a callback invoked once for each codegen or parser warning produced
    /// during compile().  The callback receives the full Diagnostic object so that
    /// the caller can apply its own formatting, --Werror promotion, etc.
    using WarnCallback = std::function<void(const omscript::Diagnostic&)>;
    void setWarningCallback(WarnCallback cb) {
        warnCallback_ = std::move(cb);
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
    unsigned superoptLevel_ = 2;
    bool hgoe_ = true;
    bool sdr_ = true;
    bool ipof_ = true;
    bool noOwnershipChecks_ = false; ///< --no-ownership-checks (Ω spec §6.2)
    bool memSanitize_ = false;       ///< --mem-sanitize        (Ω spec §7)
    std::string pgoGenPath_;
    std::string pgoUsePath_;
    std::unordered_map<std::string, long long> defines_;      ///< -D NAME[=int] comptime flags
    std::unordered_map<std::string, std::string> stringDefines_; ///< -D NAME=str comptime flags
    WarnCallback warnCallback_;                                ///< optional per-warning callback
};

} // namespace omscript

#endif // COMPILER_H
