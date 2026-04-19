#pragma once

#ifndef HARDWARE_GRAPH_H
#define HARDWARE_GRAPH_H

/// @file hardware_graph.h
/// @brief Hardware Graph Optimization Engine (HGOE) for hardware-aware compilation.
///
/// The HGOE activates when the user provides -march or -mtune flags (including
/// "native", which resolves to the host CPU) and models the target CPU
/// microarchitecture as a directed graph of execution resources.  The compiled
/// program is converted into a dependency graph and mapped onto the hardware
/// graph to minimise pipeline stalls, port contention, register pressure and
/// cache misses.
///
/// Integration:
///   The HGOE runs after the superoptimizer and before final code emission.
///   When -march / -mtune are not specified, the engine is bypassed entirely
///   and the normal compiler pipeline is used unchanged.

// Forward declarations — avoids pulling in heavy LLVM headers for translation
// units that only need the HGOE interface.  The full headers are included in
// the .cpp file where the implementations live.
namespace llvm {
class BasicBlock;
class Function;
class Instruction;
class Module;
} // namespace llvm

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>

namespace omscript {
namespace hgoe {

// ─────────────────────────────────────────────────────────────────────────────
// Floating-point precision levels for per-variable / per-operation control
// ─────────────────────────────────────────────────────────────────────────────

/// Fine-grained floating-point precision levels.
/// These levels allow per-variable and per-operation control over which FP
/// optimizations are permitted, as an alternative to the global -ffast-math flag.
enum class FPPrecision {
    Strict,  ///< Full IEEE-754 compliance: no reassociation, no NaN/Inf assumptions.
    Medium,  ///< Allow limited reassociation and some vectorization; preserve NaN/Inf.
    Fast,    ///< Equivalent to -ffast-math: reassociation, reciprocal transforms,
             ///< ignoring NaN/Inf, fused operations all permitted.
};

/// Return a human-readable name for an FPPrecision level.
inline const char* fpPrecisionName(FPPrecision p) {
    switch (p) {
    case FPPrecision::Strict: return "strict";
    case FPPrecision::Medium: return "medium";
    case FPPrecision::Fast:   return "fast";
    }
    return "unknown";
}

/// Resolve conflicting precision levels from two operands.
/// Uses a conservative meet: the stricter (lower) level wins so that
/// combining a strict operand with a fast operand yields strict semantics.
inline FPPrecision resolvePrecision(FPPrecision a, FPPrecision b) {
    // Strict < Medium < Fast  — lower enum value is stricter.
    return (a < b) ? a : b;
}

// ─────────────────────────────────────────────────────────────────────────────
// Cache model for cache-aware optimization
// ─────────────────────────────────────────────────────────────────────────────

/// Hardware cache model derived from the microarchitecture profile.
/// Used by the cache-aware optimization pass to choose tile sizes and
/// prefetch distances.
struct CacheModel {
    unsigned l1Size = 32;       ///< L1D size in KB
    unsigned l1Latency = 4;     ///< L1D hit latency in cycles
    unsigned l1LineSize = 64;   ///< L1 cache line size in bytes
    unsigned l2Size = 256;      ///< L2 size in KB
    unsigned l2Latency = 12;    ///< L2 hit latency in cycles
    unsigned l3Size = 8192;     ///< L3 size in KB
    unsigned l3Latency = 40;    ///< L3 hit latency in cycles
    unsigned memLatency = 200;  ///< Main memory latency in cycles
    double   memBandwidth = 40.0; ///< Memory bandwidth in GB/s (approximate)
};

/// Build a CacheModel from a MicroarchProfile (forward-declared; defined after
/// MicroarchProfile below).
struct MicroarchProfile;
CacheModel buildCacheModel(const MicroarchProfile& profile);

/// Memory access pattern classification.
enum class AccessPattern {
    Unknown,    ///< Cannot classify
    Sequential, ///< Unit-stride sequential access (best locality)
    Strided,    ///< Constant-stride access (predictable, may miss cache)
    Random,     ///< Irregular / data-dependent access (poor locality)
    Streaming,  ///< Write-once or read-once streaming (bypass-friendly)
};

/// Statistics from the cache-aware optimization pass.
struct CacheOptStats {
    unsigned loopsTiled = 0;         ///< Loops with tiling metadata added
    unsigned loopsInterchanged = 0;  ///< Loops reordered for locality
    unsigned prefetchesInserted = 0; ///< Software prefetch hints added
    unsigned layoutHints = 0;        ///< AoS→SoA suggestions emitted
};

// ─────────────────────────────────────────────────────────────────────────────
// Step 1 — Hardware execution graph types
// ─────────────────────────────────────────────────────────────────────────────

/// Types of hardware execution resources.
enum class ResourceType {
    IntegerALU,         ///< Integer arithmetic/logic unit
    VectorALU,          ///< SIMD/vector ALU
    FMAUnit,            ///< Fused multiply-add unit
    LoadUnit,           ///< Load port / load-store unit
    StoreUnit,          ///< Store port / store-data unit
    BranchUnit,         ///< Branch prediction / execution unit
    AGU,                ///< Address generation unit
    DividerUnit,        ///< Integer/FP divider (often shared)
    L1DCache,           ///< L1 data cache
    L1ICache,           ///< L1 instruction cache
    L2Cache,            ///< Unified L2 cache
    L3Cache,            ///< Unified L3 cache (LLC)
    MainMemory,         ///< Off-chip DRAM
    IntRegisterFile,    ///< Integer register file
    VecRegisterFile,    ///< Vector/SIMD register file
    FPRegisterFile,     ///< Floating-point register file
    Dispatch,           ///< Front-end dispatch/rename stage
    Retire,             ///< Retirement / writeback stage
    SchedulerQueue,     ///< Unified reservation station / scheduler queue
    LoadBuffer,         ///< Load queue / load buffer
    StoreBuffer,        ///< Store queue / store buffer
};

/// A node in the hardware execution graph.
struct HardwareNode {
    unsigned id = 0;
    ResourceType type;
    std::string name;
    unsigned count = 1;              ///< Number of instances (e.g. 4 ALU ports)
    double latency = 1.0;           ///< Latency in cycles for this resource
    double throughput = 1.0;         ///< Instructions per cycle throughput
    unsigned pipelineDepth = 1;      ///< Depth of the pipeline for this unit
};

/// An edge in the hardware execution graph (dispatch/forwarding path).
struct HardwareEdge {
    unsigned srcId = 0;              ///< Source hardware node ID
    unsigned dstId = 0;              ///< Destination hardware node ID
    double latency = 0.0;           ///< Forwarding latency in cycles
    double bandwidth = 1.0;         ///< Bandwidth (ops/cycle) on this path
    std::string label;               ///< Description of the path
};

/// The hardware execution graph — models a CPU microarchitecture.
class HardwareGraph {
public:
    HardwareGraph() = default;

    /// Add a hardware resource node. Returns its ID.
    unsigned addNode(ResourceType type, const std::string& name,
                     unsigned count = 1, double latency = 1.0,
                     double throughput = 1.0, unsigned pipelineDepth = 1);

    /// Add a directed edge between two hardware nodes.
    void addEdge(unsigned srcId, unsigned dstId, double latency = 0.0,
                 double bandwidth = 1.0, const std::string& label = "");

    /// Look up a node by ID.
    [[nodiscard]] const HardwareNode* getNode(unsigned id) const noexcept;

    /// Find nodes of a given resource type.
    [[nodiscard]] std::vector<const HardwareNode*> findNodes(ResourceType type) const;

    /// Get all edges originating from a node.
    [[nodiscard]] std::vector<const HardwareEdge*> getOutEdges(unsigned nodeId) const;

    /// Total number of nodes.
    [[nodiscard]] size_t nodeCount() const noexcept { return nodes_.size(); }

    /// Total number of edges.
    [[nodiscard]] size_t edgeCount() const noexcept { return edges_.size(); }

    /// Get all nodes (read-only).
    [[nodiscard]] const std::vector<HardwareNode>& nodes() const noexcept { return nodes_; }

    /// Get all edges (read-only).
    [[nodiscard]] const std::vector<HardwareEdge>& edges() const noexcept { return edges_; }

private:
    std::vector<HardwareNode> nodes_;
    std::vector<HardwareEdge> edges_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Step 2 — Program execution graph types
// ─────────────────────────────────────────────────────────────────────────────

/// Classification of program operations for mapping to hardware.
enum class OpClass {
    IntArith,       ///< Integer add, sub, logic
    IntMul,         ///< Integer multiply
    IntDiv,         ///< Integer divide/remainder
    FPArith,        ///< FP add, sub
    FPMul,          ///< FP multiply
    FPDiv,          ///< FP divide
    FMA,            ///< Fused multiply-add
    VectorOp,       ///< SIMD/vector operation
    Load,           ///< Memory load
    Store,          ///< Memory store
    Branch,         ///< Branch / conditional
    Shift,          ///< Shifts and rotations
    Comparison,     ///< Integer/FP comparisons
    Conversion,     ///< Type conversions (trunc, ext, cast)
    Intrinsic,      ///< Intrinsic function call
    Call,           ///< Generic function call
    Phi,            ///< PHI node (free in hardware)
    Other,          ///< Unclassified
};

/// A node in the program execution graph.
struct ProgramNode {
    unsigned id = 0;
    OpClass opClass = OpClass::Other;
    llvm::Instruction* inst = nullptr;  ///< The LLVM instruction (nullable for virtual nodes)
    double estimatedLatency = 1.0;      ///< Latency estimated from hardware profile
    double estimatedThroughput = 1.0;   ///< Throughput from hardware profile
    unsigned scheduledCycle = 0;        ///< Cycle assigned by scheduler
    unsigned assignedPort = 0;          ///< Hardware port assigned by mapper
    FPPrecision fpPrecision = FPPrecision::Medium; ///< FP precision level for this operation
};

/// Dependency type between program operations.
enum class DepType {
    Data,       ///< True data dependency (RAW)
    AntiDep,    ///< Anti-dependency (WAR)
    OutputDep,  ///< Output dependency (WAW)
    Memory,     ///< Memory ordering dependency
    Control,    ///< Control flow dependency
};

/// An edge in the program execution graph.
struct ProgramEdge {
    unsigned srcId = 0;          ///< Producer node
    unsigned dstId = 0;          ///< Consumer node
    DepType type = DepType::Data;
    unsigned latency = 1;        ///< Minimum latency between src and dst
};

/// The program execution graph for a single function.
class ProgramGraph {
public:
    ProgramGraph() = default;

    /// Build the program graph from an LLVM function.
    void buildFromFunction(llvm::Function& func);

    /// Add a program operation node. Returns its ID.
    unsigned addNode(OpClass opClass, llvm::Instruction* inst = nullptr);

    /// Add a dependency edge.
    void addEdge(unsigned srcId, unsigned dstId, DepType type = DepType::Data,
                 unsigned latency = 1);

    /// Get a node by ID.
    const ProgramNode* getNode(unsigned id) const;
    ProgramNode* getNodeMut(unsigned id);

    /// Get predecessors of a node.
    std::vector<unsigned> getPredecessors(unsigned nodeId) const;

    /// Get successors of a node.
    std::vector<unsigned> getSuccessors(unsigned nodeId) const;

    /// Total number of nodes.
    size_t nodeCount() const { return nodes_.size(); }

    /// Compute the critical path length through the graph.
    unsigned criticalPathLength() const;

    /// Get all nodes (read-only).
    const std::vector<ProgramNode>& nodes() const { return nodes_; }

    /// Get all edges (read-only).
    const std::vector<ProgramEdge>& edges() const { return edges_; }

private:
    std::vector<ProgramNode> nodes_;
    std::vector<ProgramEdge> edges_;
    std::unordered_map<llvm::Instruction*, unsigned> instToNode_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Step 5 — Hardware-aware cost model
// ─────────────────────────────────────────────────────────────────────────────

// Forward declaration — MicroarchProfile is defined below (Step 7).
// Required so that HardwareCostModel can store a reference to it.
struct MicroarchProfile;

/// Hardware-aware cost model that replaces the normal cost model when HGOE
/// is active.  Costs are derived directly from the microarchitecture profile
/// via getOpcodeLatency — the single authoritative latency lookup function.
class HardwareCostModel {
public:
    /// Construct from both the hardware graph (for structural simulation)
    /// and the microarchitecture profile (for accurate per-opcode latencies).
    HardwareCostModel(const HardwareGraph& hw, const MicroarchProfile& profile);

    /// Cost of a single instruction on the target hardware.
    /// Delegates to getOpcodeLatency(inst, profile_).
    double instructionCost(const llvm::Instruction* inst) const;

    /// Cost of executing the program graph on the hardware graph.
    /// Simulates execution and returns estimated total cycles.
    double simulateExecution(const ProgramGraph& pg) const;

    /// Estimate port contention for the program graph.
    double portContentionPenalty(const ProgramGraph& pg) const;

    /// Get preferred vector width for the hardware.
    unsigned preferredVectorWidth() const { return vectorWidth_; }

private:
    const HardwareGraph& hw_;
    const MicroarchProfile& profile_; ///< Single authoritative source for latencies
    unsigned vectorWidth_ = 4;
    double issueWidth_ = 4.0;
    double cacheMissL1Penalty_ = 4.0;
    double cacheMissL2Penalty_ = 12.0;
    double cacheMissL3Penalty_ = 40.0;

    OpClass classifyInstruction(const llvm::Instruction* inst) const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Step 7 — Hardware database (microarchitecture profiles)
// ─────────────────────────────────────────────────────────────────────────────

/// ISA family of the target.
enum class ISAFamily {
    X86_64,
    AArch64,
    RISCV64,
    Unknown,
};

/// A microarchitecture profile describes the execution characteristics of a
/// specific CPU microarchitecture.
struct MicroarchProfile {
    std::string name;               ///< e.g. "skylake", "znver4", "neoverse-v2"
    ISAFamily isa = ISAFamily::Unknown;

    // Front-end
    unsigned decodeWidth = 4;       ///< Instructions decoded per cycle
    unsigned issueWidth = 4;        ///< Micro-ops issued per cycle
    unsigned pipelineDepth = 14;    ///< Total pipeline stages

    // Execution units
    unsigned intALUs = 4;           ///< Number of integer ALU ports
    unsigned vecUnits = 2;          ///< Number of SIMD/vector units
    unsigned fmaUnits = 2;          ///< Number of FMA units
    unsigned loadPorts = 2;         ///< Number of load ports
    unsigned storePorts = 1;        ///< Number of store ports
    unsigned branchUnits = 1;       ///< Branch execution units
    unsigned agus = 2;              ///< Address generation units
    unsigned dividers = 1;          ///< Integer/FP dividers

    // Instruction latencies (cycles)
    unsigned latIntAdd = 1;
    unsigned latIntMul = 3;
    unsigned latIntDiv = 25;
    unsigned latFPAdd = 4;
    unsigned latFPMul = 4;
    unsigned latFPDiv = 14;
    unsigned latFMA = 4;
    unsigned latLoad = 4;           ///< L1 hit latency
    unsigned latStore = 4;
    unsigned latBranch = 1;
    unsigned latShift = 1;
    /// Latency for integer↔FP conversion instructions (CVTSI2SD, SCVTF, etc.).
    /// On x86 these use a bypass path from the integer file to the FP file and
    /// typically take one cycle more than a plain FP-add (e.g., Skylake
    /// CVTSI2SD = 5 cycles while VADDPD = 4 cycles).
    /// On AArch64 SCVTF/UCVTF/FCVTZS have the same latency as FADD.
    /// 0 = unset: the scheduler automatically falls back to latFPAdd.
    unsigned latFPConvert = 0;      ///< Int↔FP conversion latency (0 = use latFPAdd)

    // Throughput (reciprocal: cycles per instruction)
    double tputIntAdd = 0.25;       ///< 4 per cycle → 0.25
    double tputIntMul = 1.0;
    double tputFPAdd = 0.5;
    double tputFPMul = 0.5;
    double tputLoad = 0.5;
    double tputStore = 1.0;

    // Cache hierarchy
    unsigned l1DSize = 32;          ///< L1D cache size in KB
    unsigned l1DLatency = 4;        ///< L1D latency in cycles
    unsigned l2Size = 256;          ///< L2 cache size in KB
    unsigned l2Latency = 12;        ///< L2 latency in cycles
    unsigned l3Size = 8192;         ///< L3 cache size in KB
    unsigned l3Latency = 40;        ///< L3 latency in cycles
    unsigned cacheLineSize = 64;    ///< Cache line size in bytes

    // Vector capabilities
    unsigned vectorWidth = 256;     ///< SIMD width in bits (128=SSE, 256=AVX2, 512=AVX-512)

    // Register counts
    unsigned intRegisters = 16;
    unsigned vecRegisters = 16;
    unsigned fpRegisters = 16;

    // Branch prediction
    double branchMispredictPenalty = 15.0;  ///< Cycles lost on mispredict
    unsigned btbEntries = 4096;             ///< Branch target buffer entries

    // Memory
    unsigned memoryLatency = 200;   ///< Main memory latency in cycles

    // Reorder buffer
    unsigned robSize = 224;         ///< Reorder buffer entries (224 for Skylake)

    // Port constraints for integer multiply.
    // Integer multiply can only use a subset of the total integer ALU ports on
    // most microarchitectures (e.g. only 2 of 4 ports on Skylake, port 1 only
    // on Haswell).  The scheduler uses this to avoid overcommitting ALU capacity.
    unsigned mulPortCount = 1;      ///< ALU port instances capable of integer multiply

    // Store-to-load forwarding latency.
    // When a load immediately follows a store to the same address, the value
    // can be forwarded from the store buffer without going through the cache.
    // This latency is used as the Store→Load RAW dependency edge weight in the
    // scheduler, replacing the less accurate `latStore` value.
    // Typically equals latLoad (the L1 hit latency) on most architectures.
    // On some ARM cores (e.g. Apple M-series) forwarding is slower than the
    // store execution latency because the store buffer comparison adds cycles.
    unsigned latStoLForward = 4;    ///< Store-to-load forwarding latency (cycles)

    // 512-bit SIMD throughput penalty.
    // On CPUs that implement 512-bit SIMD by double-pumping 256-bit execution
    // units (e.g. Skylake-AVX512, Ice Lake), issuing a 512-bit instruction
    // occupies the FMA/VecALU port for 2 cycles instead of 1.
    // The latency is unchanged; only throughput (port occupancy) is halved.
    //   1 = native 512-bit unit (no penalty): Sapphire Rapids, Zen 5, etc.
    //   2 = double-pumped (2× port occupancy): Skylake-AVX512, Ice Lake, etc.
    unsigned vec512Penalty = 1;     ///< FMA/VecALU throughput multiplier for 512-bit ops

    // Division throughput (reciprocal cycles per instruction).
    // Division units are rarely fully pipelined: a new divide cannot start
    // every cycle.  These fields capture the *throughput* constraint separately
    // from the latency fields above.  When zero the latency is used as a
    // conservative fallback.
    double tputFPDiv = 0.0;         ///< FP divide throughput (0 = use latFPDiv as reciprocal)
    double tputIntDiv = 0.0;        ///< Int divide throughput (0 = use latIntDiv as reciprocal)

    // Reservation station (scheduler queue) size.
    // Instructions are held in the RS from dispatch until their operands are
    // ready and a port is available.  When the RS is full, the front-end stalls
    // regardless of remaining ROB capacity.
    unsigned schedulerSize = 64;    ///< Unified RS / scheduler queue entries

    // Load and store buffer sizes.
    // Out-of-order CPUs track outstanding loads and stores in dedicated
    // hardware buffers.  When a buffer fills, dispatch of new loads / stores
    // is stalled until an entry retires.
    unsigned loadBufferEntries = 64;   ///< Load buffer / MOB load queue entries
    unsigned storeBufferEntries = 36;  ///< Store buffer / MOB store queue entries

    // Memory subsystem bandwidth (bytes per cycle, single-core peak read).
    // Used to bound throughput for streaming / vectorised loops.
    // 0 means "not modelled" (no bandwidth penalty applied).
    unsigned l1DBandwidthBytesPerCycle = 0;  ///< L1D peak load bandwidth
    unsigned l2BandwidthBytesPerCycle  = 0;  ///< L2 peak bandwidth
    unsigned l3BandwidthBytesPerCycle  = 0;  ///< L3 peak bandwidth
    unsigned memBandwidthBytesPerCycle = 0;  ///< DRAM peak bandwidth (single-core)

    // SMT / Hyperthreading support.
    // When enabled, effective per-thread resource budgets (RS, ROB, LB, SB)
    // are roughly halved during competition from the sibling thread.
    bool hasHyperthreading = false;  ///< CPU supports simultaneous multi-threading
};

/// Look up a microarchitecture profile by CPU name.
/// Returns std::nullopt for unknown architectures (fallback mode).
std::optional<MicroarchProfile> lookupMicroarch(const std::string& cpuName);

/// Build a HardwareGraph from a microarchitecture profile.
HardwareGraph buildHardwareGraph(const MicroarchProfile& profile);

/// Compute instruction latency cost directly from a microarchitecture profile.
/// Returns latency in cycles.  This is the single authoritative latency lookup:
/// HardwareCostModel::instructionCost() and ProgramGraph::buildFromFunction()
/// both delegate to this function, ensuring all cost estimates stay consistent.
double instrCostFromProfile(const llvm::Instruction* inst, const MicroarchProfile& profile);

// ─────────────────────────────────────────────────────────────────────────────
// Step 3 & 4 — Graph mapping optimizer and transformations
// ─────────────────────────────────────────────────────────────────────────────

/// Schedule entry for the mapping result.
struct ScheduleEntry {
    unsigned nodeId = 0;             ///< Program graph node
    unsigned cycle = 0;              ///< Scheduled cycle
    unsigned port = 0;               ///< Assigned hardware port
    ResourceType resource = ResourceType::IntegerALU;
};

/// Result of the graph mapping optimisation.
struct MappingResult {
    std::vector<ScheduleEntry> schedule;
    unsigned totalCycles = 0;        ///< Simulated total execution cycles
    double portUtilization = 0.0;    ///< Average port utilization (0.0–1.0)
    unsigned stallCycles = 0;        ///< Pipeline stall cycles
};

/// Map a program graph onto a hardware graph using list scheduling.
MappingResult mapProgramToHardware(ProgramGraph& pg, const HardwareGraph& hw,
                                    const MicroarchProfile& profile);

// ─────────────────────────────────────────────────────────────────────────────
// Step 4 — Hardware-aware transformations
// ─────────────────────────────────────────────────────────────────────────────

/// Statistics from hardware-aware transformations.
struct TransformStats {
    unsigned fmaGenerated = 0;        ///< FMA instructions generated
    unsigned loadsStorePaired = 0;    ///< Load/store pairs fused
    unsigned prefetchesInserted = 0;  ///< Prefetch instructions inserted
    unsigned branchesOptimized = 0;   ///< Branch layout improvements
    unsigned vectorExpanded = 0;      ///< Vector width expansions
    unsigned intStrengthReduced = 0;  ///< Integer multiplies replaced by shift+add
    CacheOptStats cacheOpt;           ///< Cache-aware optimization stats
};

/// Apply hardware-aware transformations to a function.
TransformStats applyHardwareTransforms(llvm::Function& func,
                                        const MicroarchProfile& profile,
                                        bool enableLoopAnnotation = true);

// ─────────────────────────────────────────────────────────────────────────────
// Step 6 & 10 — Top-level HGOE API
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// SchedulerPolicy — independently tunable components of the list scheduler
// ─────────────────────────────────────────────────────────────────────────────

/// Per-subsystem feature flags and heuristic weights for the HGOE list
/// scheduler.  Splitting the scheduler into independently tunable components
/// lets each heuristic be validated, benchmarked, and calibrated on its own
/// without touching the others.
///
/// Priority tiers (1 = highest, 9 = lowest tie-break):
///   1. Critical-path remaining — latency hiding
///   2. Long-latency first (div/fdiv) — start slow ops early
///   3. Loads first — hide memory latency
///   4. Stall distance — consumer work remaining
///   5. Fusion affinity — keep macro-op / micro-op fusion pairs adjacent
///   6. Port pressure — schedule bottleneck resource first
///   7. Register pressure penalty — avoid spills
///   8. Register-freeing score — reduce live-value count
///   9. ROB latency preference — short-latency ops when ROB is under pressure
struct SchedulerPolicy {
    // ── Feature flags ──────────────────────────────────────────────────────────
    /// Enable macro-op / micro-op fusion affinity heuristic (tier 5).
    bool enableFusionHeuristic  = true;
    /// Enable per-register-file pressure penalty (tier 7).
    bool enableRegPressure      = true;
    /// Enable ROB occupancy short-latency preference (tier 9).
    bool enableROBPressure      = true;
    /// Enable chain-diversity enforcement in the two-pass issue window.
    bool enableChainDiversity   = true;
    /// Enable cache-miss-risk heuristic for load scheduling (tier 3 sub-sort).
    bool enableCacheMissRisk    = true;
    /// Enable slack-aware dampening of the register-pressure penalty.
    bool enableSlackAware       = true;
    /// Enable beam-search pruning of large ready lists.
    bool enableBeamPruning      = true;

    // ── Tuning parameters ─────────────────────────────────────────────────────
    /// Maximum candidates considered per cycle (beam width).  Reducing this
    /// lowers compile time for very large basic blocks; increasing it gives the
    /// sorter more candidates to choose from and may improve IPC.
    unsigned beamWidth          = 32;
    /// Slack damping divisor for the register-pressure penalty.
    /// penalty = penalty / (1 + slack / slackDamping).
    /// Higher values reduce the damping effect; 1 disables it entirely.
    unsigned slackDamping       = 4;
};

// ─────────────────────────────────────────────────────────────────────────────
// SchedulerQuality — per-function scheduling efficiency metrics
// ─────────────────────────────────────────────────────────────────────────────

/// Scheduling-efficiency metrics accumulated by scheduleInstructions().
/// Used to measure the quality of a schedule relative to the theoretical
/// optimum (critical path) and to compare HGOE against baseline schedulers.
struct SchedulerQuality {
    unsigned scheduledCycles    = 0;   ///< Total cycles across all BBs
    unsigned instructionsTotal  = 0;   ///< Total moveable instructions scheduled
    unsigned peakIntLive        = 0;   ///< Peak integer register live-value count
    unsigned peakVecLive        = 0;   ///< Peak vector/FP register live-value count
    unsigned basicBlocksScheduled = 0; ///< BBs with ≥2 moveable instructions
    /// Ratio criticalPath / scheduledCycles (1.0 = perfect, <1.0 = suboptimal).
    /// A value of 1.0 means we achieved the theoretical minimum (scheduled in
    /// exactly as many cycles as the longest dependency chain requires).
    /// Values below 1.0 indicate the schedule required more cycles than the
    /// critical path, e.g. due to resource contention or suboptimal ordering.
    /// Set to 0.0 when no scheduling was performed or the function is empty.
    double   efficiency         = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// CalibrationHints — per-architecture measurement-driven profile adjustment
// ─────────────────────────────────────────────────────────────────────────────

/// Per-architecture correction factors derived from hardware measurements.
/// Pass to calibrateProfile() to produce an adjusted MicroarchProfile.
/// Scale factors <1.0 reduce the base value; >1.0 increase it.
/// 1.0 (default) is a no-op — the base profile is returned unchanged.
struct CalibrationHints {
    double intAddScale   = 1.0; ///< Multiply latIntAdd / latShift / latBranch
    double fpAddScale    = 1.0; ///< Multiply latFPAdd / latFPMul
    double loadScale     = 1.0; ///< Multiply latLoad and latStoLForward
    double divScale      = 1.0; ///< Multiply latIntDiv and latFPDiv
    double robScale      = 1.0; ///< Scale robSize (e.g., 0.5 for SMT/HT contention)
    double rsScale       = 1.0; ///< Scale schedulerSize
    double issueScale    = 1.0; ///< Scale issueWidth (e.g., 0.5 for SMT)
};

/// Produce an adjusted MicroarchProfile by applying CalibrationHints.
/// Useful for tuning profile parameters based on measured performance counters
/// without editing the base profile tables.  All resulting values are clamped
/// to sane minima so the scheduler never sees zero-capacity resources.
MicroarchProfile calibrateProfile(const MicroarchProfile& base,
                                   const CalibrationHints& hints);

/// Configuration for the Hardware Graph Optimization Engine.
struct HGOEConfig {
    std::string marchCpu;            ///< -march value (empty = not specified)
    std::string mtuneCpu;            ///< -mtune value (empty = not specified)
    bool enableScheduling = true;    ///< Graph-based instruction scheduling
    bool enableTransforms = true;    ///< Hardware-aware transformations
    bool enableCostModel = true;     ///< Hardware-aware cost model
    bool enableLoopAnnotation = true; ///< Add loop unroll/vectorize metadata.
                                      ///< Disable when LTO is active — the LTO
                                      ///< linker has its own loop optimizer and
                                      ///< forced hints cause excessive compile
                                      ///< time or hangs.
    /// Per-subsystem scheduler knobs.  The default policy matches the behaviour
    /// prior to the introduction of this field (all features enabled, beam=32).
    SchedulerPolicy schedulerPolicy;
};

/// Apply hardware-graph-driven instruction scheduling to all basic blocks in a
/// function.  Reorders instructions within each basic block (phi nodes and
/// terminators stay fixed) to maximise throughput for the specific hardware.
/// Uses the actual HardwareGraph port model — each execution-unit node carries
/// a throughput value (instructions/cycle) that drives cycle-accurate port
/// assignment and port-diversity scheduling.
///
/// @param func     Function to schedule.
/// @param hw       Hardware graph built from the target microarchitecture.
/// @param profile  Microarchitecture profile (latencies, port counts, etc.).
/// @param policy   Scheduler heuristic knobs (default: all features enabled).
/// @param quality  Optional output — scheduling-efficiency metrics.
/// @returns Estimated total cycle count across all scheduled basic blocks.
unsigned scheduleInstructions(llvm::Function& func, const HardwareGraph& hw,
                               const MicroarchProfile& profile,
                               const SchedulerPolicy& policy = SchedulerPolicy{},
                               SchedulerQuality* quality = nullptr);

/// Statistics from the HGOE pipeline.
struct HGOEStats {
    bool activated = false;           ///< Whether HGOE was activated
    std::string resolvedArch;         ///< Resolved microarchitecture name
    unsigned functionsOptimized = 0;
    unsigned totalScheduledCycles = 0;
    double avgPortUtilization = 0.0;
    unsigned basicBlocksScheduled = 0; ///< Basic blocks with instructions reordered
    unsigned loopsUnrolled = 0;        ///< Loops annotated for software pipelining
    TransformStats transforms;
    /// Scheduling-quality metrics from the last scheduleInstructions() call.
    SchedulerQuality schedulerQuality;
};

/// Check whether HGOE should activate for the given config.
/// Returns true when -march or -mtune is specified (including "native").
bool shouldActivate(const HGOEConfig& config);

/// Run the Hardware Graph Optimization Engine on a module.
/// If -march/-mtune are not specified, this is a no-op (returns stats with
/// activated=false).
HGOEStats optimizeModule(llvm::Module& module, const HGOEConfig& config);

/// Run the HGOE on a single function.
HGOEStats optimizeFunction(llvm::Function& func, const HGOEConfig& config);

/// Pre-pipeline loop annotation: set llvm.loop metadata (unroll count,
/// interleave count, vector width) on loops in the module based on the
/// target CPU's characteristics.  This must run BEFORE the LLVM
/// optimization pipeline so that the unroller and vectorizer respect the
/// hardware-optimal parameters instead of using generic heuristics.
/// Returns the number of loops annotated.
unsigned annotateLoopsForTarget(llvm::Module& module, const HGOEConfig& config);

// ─────────────────────────────────────────────────────────────────────────────
// Cache-aware optimization pass
// ─────────────────────────────────────────────────────────────────────────────

/// Run the cache-aware optimization pass on a single function.
/// Analyses memory access patterns and inserts prefetch hints tuned to the
/// target cache hierarchy.  Uses precision metadata on instructions to
/// control aggressiveness: fast regions get longer prefetch distances and
/// more aggressive tiling, while strict regions are left untouched to
/// preserve numerical stability.
CacheOptStats optimizeCacheLocality(llvm::Function& func,
                                     const MicroarchProfile& profile,
                                     const CacheModel& cache);

// ─────────────────────────────────────────────────────────────────────────────
// Precision metadata helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Read the FP precision level attached to an LLVM instruction via metadata.
/// Returns Medium if no metadata is present.
FPPrecision getInstructionPrecision(const llvm::Instruction* inst);

/// Attach FP precision metadata to an LLVM instruction.
void setInstructionPrecision(llvm::Instruction* inst, FPPrecision prec);

/// Propagate precision metadata through a function: instructions without
/// explicit precision inherit from their operands using the conservative
/// meet (strictest wins).
void propagatePrecision(llvm::Function& func);

} // namespace hgoe
} // namespace omscript

#endif // HARDWARE_GRAPH_H
