// ---------------------------------------------------------------------------
// Deoptimization — guard-based fallback implementation
// ---------------------------------------------------------------------------
//
// See deopt.h for the overall design.  This file implements the deoptimization
// manager and the C-linkage callback invoked from guard checks in
// specialized (Tier-2) code.
// ---------------------------------------------------------------------------

#include "deopt.h"

#include <iostream>

namespace omscript {

DeoptManager& DeoptManager::instance() {
    static DeoptManager inst;
    return inst;
}

void DeoptManager::onGuardFailure(const char* funcName, void** fnPtrSlot) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Fast path: if already deoptimized, nothing to do.
    auto deoptIt = deoptimized_.find(funcName);
    if (__builtin_expect(deoptIt != deoptimized_.end() && deoptIt->second, 0))
        return;

    auto& count = failures_[funcName];
    count++;

    if (count >= kDeoptThreshold) {
        // Revert to baseline: clear the hot-patch slot so the dispatch
        // prolog in the Tier-1 code falls through to the original body.
        if (fnPtrSlot) {
            *fnPtrSlot = nullptr;
        }
        deoptimized_[funcName] = true;
        std::cerr << "omsc: deoptimized '" << funcName << "' after " << count << " guard failures\n";
    }
}

int64_t DeoptManager::failureCount(const std::string& funcName) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = failures_.find(funcName);
    return it != failures_.end() ? it->second : 0;
}

bool DeoptManager::isDeoptimized(const std::string& funcName) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = deoptimized_.find(funcName);
    return it != deoptimized_.end() && it->second;
}

void DeoptManager::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    failures_.clear();
    deoptimized_.clear();
}

} // namespace omscript

// ---------------------------------------------------------------------------
// C-linkage callback — called from guard checks in specialized code
// ---------------------------------------------------------------------------
extern "C" {

void __omsc_deopt_guard_fail(const char* funcName, void** fnPtrSlot) {
    omscript::DeoptManager::instance().onGuardFailure(funcName, fnPtrSlot);
}

} // extern "C"
