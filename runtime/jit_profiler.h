// ---------------------------------------------------------------------------
// JIT Profiler — Runtime data collection for tiered optimizing JIT
// ---------------------------------------------------------------------------
//
// Pipeline diagram:
//
//   Source Code → Compiler → Optimized IR (AOT) → Baseline Code
//       → Runtime Profiling → Hot Function Detected → Specialized IR
//       → Optimized Machine Code → Execution
//
// This module collects runtime profile data during Tier-1 execution:
//
//   1. Call frequency  — tracked by dispatch prolog counters (aot_profile.h)
//   2. Branch probabilities — tracked via branch-site counters
//   3. Argument types  — observed operand types (int/float/string/array)
//   4. Observed constants — frequently-passed constant values
//
// When a function is promoted to Tier-2, the profiler data feeds into
// the IR specialization pass:
//   - Branch weights become LLVM metadata for layout optimization
//   - Dominant argument types enable type-specialized code paths
//   - Observed constants enable constant folding in hot paths
//   - Deoptimization guards protect speculative assumptions
//
// ---------------------------------------------------------------------------

#ifndef JIT_PROFILER_H
#define JIT_PROFILER_H

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace omscript {

// ---------------------------------------------------------------------------
// Observed argument type tags.
// These classify the runtime types of values passed to functions.
// ---------------------------------------------------------------------------
enum class ArgType : uint8_t {
    Unknown = 0,
    Integer = 1,
    Float = 2,
    String = 3,
    Array = 4,
    None = 5,
};

/// Return a human-readable name for an ArgType value.
inline const char* argTypeName(ArgType t) {
    switch (t) {
    case ArgType::Unknown:
        return "unknown";
    case ArgType::Integer:
        return "int";
    case ArgType::Float:
        return "float";
    case ArgType::String:
        return "string";
    case ArgType::Array:
        return "array";
    case ArgType::None:
        return "none";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// BranchProfile — per-branch-site taken/not-taken counts
// ---------------------------------------------------------------------------
struct BranchProfile {
    uint64_t takenCount = 0;
    uint64_t notTakenCount = 0;

    /// Return the probability (0.0–1.0) that the branch is taken.
    [[nodiscard]] double takenProbability() const {
        uint64_t total = takenCount + notTakenCount;
        if (total == 0)
            return 0.5; // No data — assume 50/50
        return static_cast<double>(takenCount) / static_cast<double>(total);
    }
};

// ---------------------------------------------------------------------------
// ArgProfile — per-parameter type and constant statistics
// ---------------------------------------------------------------------------
struct ArgProfile {
    /// Number of distinct ArgType values — must match the ArgType enum.
    static constexpr size_t kNumArgTypes = 6;

    /// Minimum fraction of calls where a single constant must dominate to
    /// trigger constant-value specialization.
    static constexpr double kConstantSpecThreshold = 0.8;

    /// Minimum fraction of integer samples that must be in-range for
    /// range-based specialization to be applied.
    static constexpr double kRangeValidityThreshold = 0.9;

    /// Maximum range width (max - min) considered "tight" for range
    /// specialization.  Values spanning more than this are not specialized.
    static constexpr int64_t kTightRangeLimit = 1024;

    /// Minimum fraction of calls where the top value must appear to be
    /// considered "dominant" for multi-value specialization.
    static constexpr double kDominantValueThreshold = 0.6;

    /// Counts of each observed type for this parameter position.
    uint64_t typeCounts[kNumArgTypes] = {}; // indexed by ArgType

    /// Track the most commonly observed integer constant.
    /// observedConstant is valid when constantCount > 0.
    int64_t observedConstant = 0;
    uint64_t constantCount = 0;
    uint64_t totalCalls = 0;

    /// Observed value range (min/max) for integer arguments.
    /// Valid when rangeCount > 0.
    int64_t minObserved = INT64_MAX;
    int64_t maxObserved = INT64_MIN;
    uint64_t rangeCount = 0;

    // -----------------------------------------------------------------------
    // Top-K value frequency tracking (space-saving / misra-gries style).
    // Tracks up to kTopKSize most frequent integer values with approximate
    // counts.  Used for multi-value specialization at Tier-4+.
    // -----------------------------------------------------------------------
    static constexpr size_t kTopKSize = 4;
    struct ValueSlot {
        int64_t value = 0;
        uint64_t count = 0;
    };
    ValueSlot topK[kTopKSize] = {};

    /// Record an observed argument value.
    void record(ArgType type, int64_t value) {
        auto idx = static_cast<uint8_t>(type);
        if (idx < kNumArgTypes)
            typeCounts[idx]++;
        totalCalls++;
        if (type == ArgType::Integer) {
            if (constantCount == 0 || value == observedConstant) {
                observedConstant = value;
                constantCount++;
            }
            // Track value range for range-based optimizations.
            if (value < minObserved)
                minObserved = value;
            if (value > maxObserved)
                maxObserved = value;
            rangeCount++;

            // Top-K tracking: increment existing slot or replace minimum.
            recordTopK(value);
        }
    }

    /// Return the dominant argument type (most frequently observed).
    [[nodiscard]] ArgType dominantType() const {
        ArgType best = ArgType::Unknown;
        uint64_t bestCount = 0;
        for (size_t i = 0; i < kNumArgTypes; i++) {
            if (typeCounts[i] > bestCount) {
                bestCount = typeCounts[i];
                best = static_cast<ArgType>(i);
            }
        }
        return best;
    }

    /// Return true if a single constant dominates (>80% of calls).
    [[nodiscard]] bool hasConstantSpecialization() const {
        return totalCalls > 0 && constantCount > 0 &&
               (static_cast<double>(constantCount) / static_cast<double>(totalCalls)) > kConstantSpecThreshold;
    }

    /// Return true if observed integer values fall within a tight range.
    /// A "tight" range is when all observed values fit within [min, max] where
    /// max - min <= 1024.  This enables range-based llvm.assume optimizations
    /// that help LLVM eliminate bounds checks and narrow integer widths.
    [[nodiscard]] bool hasRangeSpecialization() const {
        if (rangeCount == 0 || rangeCount < totalCalls * kRangeValidityThreshold)
            return false;
        // Avoid overflow: if minObserved > maxObserved (shouldn't happen), bail.
        if (minObserved > maxObserved)
            return false;
        // Use unsigned subtraction to safely check range width.
        auto range = static_cast<uint64_t>(maxObserved) - static_cast<uint64_t>(minObserved);
        return range <= static_cast<uint64_t>(kTightRangeLimit);
    }

    /// Return true if the top-K data shows a small number of distinct values
    /// dominating (top value > 60% of calls).  Useful for multi-version
    /// specialization at Tier-4+.
    [[nodiscard]] bool hasDominantValue() const {
        if (totalCalls == 0)
            return false;
        uint64_t bestCount = 0;
        for (size_t i = 0; i < kTopKSize; i++) {
            if (topK[i].count > bestCount)
                bestCount = topK[i].count;
        }
        return bestCount > 0 &&
               (static_cast<double>(bestCount) / static_cast<double>(totalCalls)) > kDominantValueThreshold;
    }

    /// Return the most frequent value from the top-K tracker.
    [[nodiscard]] int64_t dominantValue() const {
        uint64_t bestCount = 0;
        int64_t bestVal = 0;
        for (size_t i = 0; i < kTopKSize; i++) {
            if (topK[i].count > bestCount) {
                bestCount = topK[i].count;
                bestVal = topK[i].value;
            }
        }
        return bestVal;
    }

  private:
    void recordTopK(int64_t value) {
        // Check if value is already tracked.
        for (size_t i = 0; i < kTopKSize; i++) {
            if (topK[i].count > 0 && topK[i].value == value) {
                topK[i].count++;
                return;
            }
        }
        // Find an empty slot.
        for (size_t i = 0; i < kTopKSize; i++) {
            if (topK[i].count == 0) {
                topK[i].value = value;
                topK[i].count = 1;
                return;
            }
        }
        // All slots occupied — replace the minimum-count slot (Misra-Gries).
        size_t minIdx = 0;
        uint64_t minCount = topK[0].count;
        for (size_t i = 1; i < kTopKSize; i++) {
            if (topK[i].count < minCount) {
                minCount = topK[i].count;
                minIdx = i;
            }
        }
        // Decrement all counts by the minimum; if any reach 0, evict.
        for (size_t i = 0; i < kTopKSize; i++) {
            topK[i].count -= minCount;
        }
        topK[minIdx].value = value;
        topK[minIdx].count = 1;
    }
};

// ---------------------------------------------------------------------------
// LoopProfile — per-loop trip count statistics with stride detection
// ---------------------------------------------------------------------------
struct LoopProfile {
    uint64_t totalIterations = 0; ///< Sum of all observed trip counts.
    uint64_t executionCount = 0;  ///< Number of times the loop was entered.

    /// Track whether all observed trip counts are exactly the same value.
    /// When true, the loop has a constant trip count that can be used for
    /// full unrolling or exact vectorisation width selection.
    int64_t constantTripCount = -1; ///< -1 = unknown, >=0 = observed constant.
    bool tripCountIsConstant = true; ///< false once a different trip count is seen.

    /// Min/max observed trip counts for range-based unroll decisions.
    uint64_t minTripCount = UINT64_MAX;
    uint64_t maxTripCount = 0;

    /// Record an observed trip count.
    void record(uint64_t tripCount) {
        totalIterations += tripCount;
        executionCount++;
        if (tripCount < minTripCount)
            minTripCount = tripCount;
        if (tripCount > maxTripCount)
            maxTripCount = tripCount;
        // Track constant trip count.
        if (executionCount == 1) {
            constantTripCount = static_cast<int64_t>(tripCount);
        } else if (tripCountIsConstant &&
                   static_cast<int64_t>(tripCount) != constantTripCount) {
            tripCountIsConstant = false;
        }
    }

    /// Return the average trip count (0 if never executed).
    [[nodiscard]] uint64_t averageTripCount() const {
        return executionCount > 0 ? totalIterations / executionCount : 0;
    }

    /// Return true if the loop always executes the same number of iterations.
    [[nodiscard]] bool hasConstantTripCount() const {
        return executionCount > 0 && tripCountIsConstant && constantTripCount >= 0;
    }

    /// Return true if trip counts fall in a narrow range (max - min <= 4).
    /// Such loops benefit from aggressive unrolling.
    [[nodiscard]] bool hasNarrowTripRange() const {
        return executionCount > 0 && maxTripCount >= minTripCount &&
               (maxTripCount - minTripCount) <= 4;
    }
};

// ---------------------------------------------------------------------------
// CallSiteProfile — per-callee call frequency from a specific caller
// ---------------------------------------------------------------------------
struct CallSiteProfile {
    std::string calleeName;
    uint64_t count = 0;
};

// ---------------------------------------------------------------------------
// FunctionProfile — aggregated profile data for one function
// ---------------------------------------------------------------------------
struct FunctionProfile {
    std::string name;

    /// Call count (mirrored from the atomic counter in the dispatch prolog).
    uint64_t callCount = 0;

    /// Branch profiles keyed by branch-site ID (sequential per function).
    std::vector<BranchProfile> branches;

    /// Argument profiles indexed by parameter position.
    std::vector<ArgProfile> args;

    /// Loop profiles indexed by loop-site ID (sequential per function).
    std::vector<LoopProfile> loops;

    /// Call-site profiles: callee frequency from this function.
    std::unordered_map<std::string, uint64_t> callSites;

    /// Whether this function has been promoted to Tier-2.
    bool promoted = false;
};

// ---------------------------------------------------------------------------
// JITProfiler — singleton runtime profiler
// ---------------------------------------------------------------------------
// Thread-safe: all mutations are guarded by a mutex.
// The profiler is accessed from:
//   - JIT-compiled code (via C-linkage callbacks)
//   - The Tier-2 recompiler (to read profile data)
// ---------------------------------------------------------------------------
class JITProfiler {
  public:
    /// Return the process-wide singleton instance.
    static JITProfiler& instance();

    /// Record a branch observation at (funcName, branchId).
    /// @p taken is true if the branch was taken, false otherwise.
    void recordBranch(const char* funcName, uint32_t branchId, bool taken);

    /// Record an argument observation for parameter @p argIndex
    /// of function @p funcName.
    void recordArg(const char* funcName, uint32_t argIndex, ArgType type, int64_t value);

    /// Record a loop trip count for loop @p loopId of function @p funcName.
    void recordLoopTripCount(const char* funcName, uint32_t loopId, uint64_t tripCount);

    /// Record a call-site observation: @p callerName called @p calleeName.
    void recordCallSite(const char* callerName, const char* calleeName);

    /// Retrieve the profile for a function.  Returns nullptr if no data.
    const FunctionProfile* getProfile(const std::string& funcName) const;

    /// Retrieve a mutable profile, creating it if needed.
    FunctionProfile& getOrCreateProfile(const std::string& funcName);

    /// Dump all profiles to stderr (for diagnostics).
    void dump() const;

    /// Reset all collected data.
    void reset();

  private:
    JITProfiler() = default;
    mutable std::mutex mtx_;
    std::unordered_map<std::string, FunctionProfile> profiles_;
};

} // namespace omscript

// ---------------------------------------------------------------------------
// C-linkage callbacks — called from compiler-injected LLVM IR
// ---------------------------------------------------------------------------
extern "C" {

/// Record a branch observation.  Injected at if/while/for branch sites.
void __omsc_profile_branch(const char* funcName, uint32_t branchId, int64_t taken);

/// Record an argument type/value observation.
void __omsc_profile_arg(const char* funcName, uint32_t argIndex, uint8_t type, int64_t value);

/// Record a loop trip count observation.
void __omsc_profile_loop(const char* funcName, uint32_t loopId, uint64_t tripCount);

/// Record a call-site observation (caller → callee).
void __omsc_profile_call_site(const char* callerName, const char* calleeName);

} // extern "C"

#endif // JIT_PROFILER_H
