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
    // Use try_lock to avoid blocking on the hot path — dropping a few samples
    // has negligible impact on profile accuracy since profiling is statistical.
    std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
    if (__builtin_expect(!lk.owns_lock(), 0))
        return;
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
    std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
    if (__builtin_expect(!lk.owns_lock(), 0))
        return;
    auto& prof = profiles_[funcName];
    if (prof.name.empty())
        prof.name = funcName;
    // Grow arg vector if needed.
    if (argIndex >= prof.args.size())
        prof.args.resize(argIndex + 1);
    prof.args[argIndex].record(type, value);
}

void JITProfiler::recordLoopTripCount(const char* funcName, uint32_t loopId, uint64_t tripCount) {
    std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
    if (__builtin_expect(!lk.owns_lock(), 0))
        return;
    auto& prof = profiles_[funcName];
    if (prof.name.empty())
        prof.name = funcName;
    if (loopId >= prof.loops.size())
        prof.loops.resize(loopId + 1);
    prof.loops[loopId].totalIterations += tripCount;
    prof.loops[loopId].executionCount++;
}

void JITProfiler::recordCallSite(const char* callerName, const char* calleeName) {
    std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
    if (__builtin_expect(!lk.owns_lock(), 0))
        return;
    auto& prof = profiles_[callerName];
    if (prof.name.empty())
        prof.name = callerName;
    prof.callSites[calleeName]++;
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
            if (ap.hasRangeSpecialization())
                std::cerr << " range=[" << ap.minObserved << "," << ap.maxObserved << "]";
            std::cerr << "\n";
        }
        for (size_t i = 0; i < prof.loops.size(); i++) {
            const auto& lp = prof.loops[i];
            std::cerr << "    loop[" << i << "]: avg_trip=" << lp.averageTripCount()
                      << " executions=" << lp.executionCount << " total_iters=" << lp.totalIterations << "\n";
        }
        for (const auto& cs : prof.callSites) {
            std::cerr << "    call_site: " << cs.first << " count=" << cs.second << "\n";
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

void __omsc_profile_loop(const char* funcName, uint32_t loopId, uint64_t tripCount) {
    omscript::JITProfiler::instance().recordLoopTripCount(funcName, loopId, tripCount);
}

void __omsc_profile_call_site(const char* callerName, const char* calleeName) {
    omscript::JITProfiler::instance().recordCallSite(callerName, calleeName);
}

} // extern "C"
