// ---------------------------------------------------------------------------
// JIT Profiler — Runtime data collection implementation
// ---------------------------------------------------------------------------
//
// Pipeline:
//   Source Code → Compiler → Optimized IR (AOT) → Baseline Code
//       → Runtime Profiling → Hot Function Detected → Specialized IR
//       → Optimized Machine Code → Execution
//
// This file implements the runtime profiling callbacks and the profiler
// singleton.  The callbacks are invoked from compiler-injected LLVM IR
// during Tier-1 execution.
// ---------------------------------------------------------------------------

#include "jit_profiler.h"

#include <iostream>

namespace omscript {

// ---------------------------------------------------------------------------
// JITProfiler singleton
// ---------------------------------------------------------------------------
JITProfiler& JITProfiler::instance() {
    static JITProfiler inst;
    return inst;
}

void JITProfiler::recordBranch(const char* funcName, uint32_t branchId, bool taken) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& prof = profiles_[funcName];
    if (prof.name.empty())
        prof.name = funcName;
    // Grow branch vector if needed.
    if (branchId >= prof.branches.size())
        prof.branches.resize(branchId + 1);
    if (taken)
        prof.branches[branchId].takenCount++;
    else
        prof.branches[branchId].notTakenCount++;
}

void JITProfiler::recordArg(const char* funcName, uint32_t argIndex, ArgType type, int64_t value) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& prof = profiles_[funcName];
    if (prof.name.empty())
        prof.name = funcName;
    // Grow arg vector if needed.
    if (argIndex >= prof.args.size())
        prof.args.resize(argIndex + 1);
    prof.args[argIndex].record(type, value);
}

const FunctionProfile* JITProfiler::getProfile(const std::string& funcName) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = profiles_.find(funcName);
    if (it == profiles_.end())
        return nullptr;
    return &it->second;
}

FunctionProfile& JITProfiler::getOrCreateProfile(const std::string& funcName) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& prof = profiles_[funcName];
    if (prof.name.empty())
        prof.name = funcName;
    return prof;
}

void JITProfiler::dump() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::cerr << "=== JIT Profiler Data ===\n";
    for (const auto& kv : profiles_) {
        const auto& prof = kv.second;
        std::cerr << "  Function: " << prof.name << " (calls=" << prof.callCount
                  << ", promoted=" << (prof.promoted ? "yes" : "no") << ")\n";
        for (size_t i = 0; i < prof.branches.size(); i++) {
            const auto& bp = prof.branches[i];
            std::cerr << "    branch[" << i << "]: taken=" << bp.takenCount << " not_taken=" << bp.notTakenCount
                      << " (p_taken=" << bp.takenProbability() << ")\n";
        }
        for (size_t i = 0; i < prof.args.size(); i++) {
            const auto& ap = prof.args[i];
            std::cerr << "    arg[" << i << "]: dominant=" << argTypeName(ap.dominantType())
                      << " total=" << ap.totalCalls;
            if (ap.hasConstantSpecialization())
                std::cerr << " const_spec=" << ap.observedConstant;
            std::cerr << "\n";
        }
    }
    std::cerr << "=========================\n";
}

void JITProfiler::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    profiles_.clear();
}

} // namespace omscript

// ---------------------------------------------------------------------------
// C-linkage callbacks — called from injected LLVM IR
// ---------------------------------------------------------------------------
extern "C" {

void __omsc_profile_branch(const char* funcName, uint32_t branchId, int64_t taken) {
    omscript::JITProfiler::instance().recordBranch(funcName, branchId, taken != 0);
}

void __omsc_profile_arg(const char* funcName, uint32_t argIndex, uint8_t type, int64_t value) {
    // Clamp type to valid ArgType range to prevent out-of-bounds access.
    if (type > static_cast<uint8_t>(omscript::ArgType::None))
        type = static_cast<uint8_t>(omscript::ArgType::Unknown);
    omscript::JITProfiler::instance().recordArg(funcName, argIndex, static_cast<omscript::ArgType>(type), value);
}

} // extern "C"
