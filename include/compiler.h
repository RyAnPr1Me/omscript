#ifndef COMPILER_H
#define COMPILER_H

#include "codegen.h"
#include <string>

namespace omscript {

class Compiler {
  public:
    Compiler();

    void compile(const std::string& sourceFile, const std::string& outputFile);
    void writeFile(const std::string& filename, const std::string& content);

    /// Control verbosity — when true, prints LLVM IR and progress messages.
    void setVerbose(bool v) {
        verbose_ = v;
    }
    bool isVerbose() const {
        return verbose_;
    }

    /// Set the LLVM optimization level used during code generation.
    void setOptimizationLevel(OptimizationLevel level) {
        optLevel_ = level;
    }
    OptimizationLevel getOptimizationLevel() const {
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
    /// Enable or disable hybrid bytecode/JIT compilation for untyped functions (default: true).
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

  private:
    std::string readFile(const std::string& filename);
    bool verbose_ = false;
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
};

} // namespace omscript

#endif // COMPILER_H
