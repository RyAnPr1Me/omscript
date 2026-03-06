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
    auto& s = state_[funcName]; // single lookup for both count and deopt flag
    s.failures++;

    if (s.failures >= kDeoptThreshold) {
        if (__builtin_expect(s.deoptimized, 0))
            return; // Already deoptimized — nothing to do.
        // Revert to baseline: clear the hot-patch slot so the dispatch
        // prolog in the Tier-1 code falls through to the original body.
        if (fnPtrSlot) {
            *fnPtrSlot = nullptr;
        }
        s.deoptimized = true;
        std::cerr << "omsc: deoptimized '" << funcName << "' after " << s.failures << " guard failures\n";
    }
}

int64_t DeoptManager::failureCount(const std::string& funcName) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = state_.find(funcName);
    return it != state_.end() ? it->second.failures : 0;
}

bool DeoptManager::isDeoptimized(const std::string& funcName) const {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = state_.find(funcName);
    return it != state_.end() && it->second.deoptimized;
}

void DeoptManager::reset() {
    std::lock_guard<std::mutex> lk(mtx_);
    state_.clear();
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
