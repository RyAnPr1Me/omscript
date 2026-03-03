// ---------------------------------------------------------------------------
// Deoptimization — guard-based fallback from specialized to baseline code
// ---------------------------------------------------------------------------
//
// Pipeline:
//   Source Code → Compiler → Optimized IR (AOT) → Baseline Code
//       → Runtime Profiling → Hot Function Detected → Specialized IR
//       → Optimized Machine Code → Execution
//                                    ↓ (guard failure)
//                              Deoptimization → Baseline Code
//
// When a function is specialized based on profiling data (e.g. "argument 0
// is always an integer"), the specializer inserts guard checks at the top
// of the function:
//
//   if (typeof(arg0) != INTEGER) goto deopt;
//
// If the guard fails at runtime, the deoptimization callback increments a
// failure counter.  Once the failure count exceeds kDeoptThreshold, the
// function's hot-patch slot is reset to nullptr — forcing future calls to
// take the baseline Tier-1 code path through the dispatch prolog.
//
// This ensures that speculative optimizations are safe: if the assumptions
// are violated, the program correctly falls back to the general-purpose
// baseline code without any incorrect behavior.
// ---------------------------------------------------------------------------

#ifndef DEOPT_H
#define DEOPT_H

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace omscript {

// ---------------------------------------------------------------------------
// DeoptManager — tracks guard failures and manages fallback to baseline
// ---------------------------------------------------------------------------
class DeoptManager {
  public:
    /// Maximum guard failures before reverting to baseline code.
    static constexpr int64_t kDeoptThreshold = 10;

    /// Return the process-wide singleton instance.
    static DeoptManager& instance();

    /// Record a guard failure for @p funcName.
    /// @p fnPtrSlot points to the @__omsc_fn_<name> global in the JIT binary.
    /// When failures exceed kDeoptThreshold, *fnPtrSlot is reset to nullptr
    /// so the dispatch prolog falls back to the baseline Tier-1 code.
    void onGuardFailure(const char* funcName, void** fnPtrSlot);

    /// Query the number of guard failures for a function.
    int64_t failureCount(const std::string& funcName) const;

    /// Check whether a function has been deoptimized.
    bool isDeoptimized(const std::string& funcName) const;

    /// Reset all deoptimization state.
    void reset();

  private:
    DeoptManager() = default;
    mutable std::mutex mtx_;
    std::unordered_map<std::string, int64_t> failures_;
    std::unordered_map<std::string, bool> deoptimized_;
};

} // namespace omscript

// ---------------------------------------------------------------------------
// C-linkage callback — called from guard checks in specialized code
// ---------------------------------------------------------------------------
extern "C" {

/// Called when a speculative guard fails at runtime.
/// Increments the failure count and reverts to baseline code if threshold
/// is exceeded.
void __omsc_deopt_guard_fail(const char* funcName, void** fnPtrSlot);

} // extern "C"

#endif // DEOPT_H
