/// @file hardware_graph.cpp
/// @brief Hardware Graph Optimization Engine (HGOE) implementation.
///
/// Implements hardware-aware compilation by:
///   1. Building a structural model of the target CPU microarchitecture

// Apply maximum compiler optimizations to this hot path.
// Strength reduction and hardware-aware scheduling are on the critical path
// for every compiled function when -march/-mtune are set.
#ifdef __GNUC__
#  pragma GCC optimize("O3,unroll-loops,tree-vectorize")
#endif
///   2. Converting LLVM IR functions into program dependency graphs
///   3. Mapping program operations onto hardware execution units
///   4. Applying hardware-specific transformations (FMA, prefetch, etc.)
///   5. Providing a hardware-aware cost model for scheduling decisions
///
/// Activated when -march or -mtune flags are provided (including "native").

#include "hardware_graph.h"
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/PatternMatch.h>

// LLVM 19 introduced getOrInsertDeclaration; older versions only have getDeclaration.
#if LLVM_VERSION_MAJOR >= 19
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getOrInsertDeclaration
#else
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getDeclaration
#endif
#include <llvm/ADT/StringRef.h>
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_set>

namespace omscript {
namespace hgoe {

// ---------------------------------------------------------------------------
// Resolve "native" to the actual host CPU name.
// ---------------------------------------------------------------------------
static std::string resolveNativeCpu(const std::string& cpu) {
    if (cpu == "native") {
        return llvm::sys::getHostCPUName().str();
    }
    return cpu;
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 1 — Hardware Graph implementation
// ═════════════════════════════════════════════════════════════════════════════

unsigned HardwareGraph::addNode(ResourceType type, const std::string& name,
                                 unsigned count, double latency,
                                 double throughput, unsigned pipelineDepth) {
    auto id = static_cast<unsigned>(nodes_.size());
    nodes_.push_back({id, type, name, count, latency, throughput, pipelineDepth});
    return id;
}

void HardwareGraph::addEdge(unsigned srcId, unsigned dstId, double latency,
                             double bandwidth, const std::string& label) {
    edges_.push_back({srcId, dstId, latency, bandwidth, label});
}

const HardwareNode* HardwareGraph::getNode(unsigned id) const noexcept {
    if (id < nodes_.size()) return &nodes_[id];
    return nullptr;
}

std::vector<const HardwareNode*> HardwareGraph::findNodes(ResourceType type) const {
    std::vector<const HardwareNode*> result;
    for (const auto& node : nodes_) {
        if (node.type == type) result.push_back(&node);
    }
    return result;
}

std::vector<const HardwareEdge*> HardwareGraph::getOutEdges(unsigned nodeId) const {
    std::vector<const HardwareEdge*> result;
    for (const auto& edge : edges_) {
        if (edge.srcId == nodeId) result.push_back(&edge);
    }
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 2 — Program Graph implementation
// ═════════════════════════════════════════════════════════════════════════════

/// Classify an LLVM instruction into an OpClass.
[[gnu::hot]] static OpClass classifyOp(const llvm::Instruction* inst) {
    if (!inst) return OpClass::Other;

    switch (inst->getOpcode()) {
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
        return OpClass::IntArith;

    case llvm::Instruction::Mul:
        return OpClass::IntMul;

    case llvm::Instruction::SDiv:
    case llvm::Instruction::UDiv:
    case llvm::Instruction::SRem:
    case llvm::Instruction::URem:
        return OpClass::IntDiv;

    case llvm::Instruction::FAdd:
    case llvm::Instruction::FSub:
        return OpClass::FPArith;

    case llvm::Instruction::FMul:
        return OpClass::FPMul;

    case llvm::Instruction::FDiv:
    case llvm::Instruction::FRem:
        return OpClass::FPDiv;

    case llvm::Instruction::Load:
        return OpClass::Load;

    case llvm::Instruction::Store:
        return OpClass::Store;

    case llvm::Instruction::Br:
    case llvm::Instruction::Switch:
    case llvm::Instruction::IndirectBr:
        return OpClass::Branch;

    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
        return OpClass::Shift;

    case llvm::Instruction::ICmp:
    case llvm::Instruction::FCmp:
        return OpClass::Comparison;

    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::FPToUI:
    case llvm::Instruction::FPToSI:
    case llvm::Instruction::UIToFP:
    case llvm::Instruction::SIToFP:
    case llvm::Instruction::BitCast:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::PtrToInt:
        return OpClass::Conversion;

    case llvm::Instruction::PHI:
        return OpClass::Phi;

    case llvm::Instruction::Call: {
        if (llvm::isa<llvm::IntrinsicInst>(inst))
            return OpClass::Intrinsic;
        return OpClass::Call;
    }

    case llvm::Instruction::Select:
        return OpClass::IntArith;

    default:
        return OpClass::Other;
    }
}

unsigned ProgramGraph::addNode(OpClass opClass, llvm::Instruction* inst) {
    auto id = static_cast<unsigned>(nodes_.size());
    ProgramNode node;
    node.id = id;
    node.opClass = opClass;
    node.inst = inst;
    nodes_.push_back(node);
    if (inst) instToNode_[inst] = id;
    return id;
}

void ProgramGraph::addEdge(unsigned srcId, unsigned dstId, DepType type,
                            unsigned latency) {
    edges_.push_back({srcId, dstId, type, latency});
}

const ProgramNode* ProgramGraph::getNode(unsigned id) const {
    if (id < nodes_.size()) return &nodes_[id];
    return nullptr;
}

ProgramNode* ProgramGraph::getNodeMut(unsigned id) {
    if (id < nodes_.size()) return &nodes_[id];
    return nullptr;
}

std::vector<unsigned> ProgramGraph::getPredecessors(unsigned nodeId) const {
    std::vector<unsigned> preds;
    for (const auto& e : edges_) {
        if (e.dstId == nodeId) preds.push_back(e.srcId);
    }
    return preds;
}

std::vector<unsigned> ProgramGraph::getSuccessors(unsigned nodeId) const {
    std::vector<unsigned> succs;
    for (const auto& e : edges_) {
        if (e.srcId == nodeId) succs.push_back(e.dstId);
    }
    return succs;
}

void ProgramGraph::buildFromFunction(llvm::Function& func) {
    nodes_.clear();
    edges_.clear();
    instToNode_.clear();

    // Phase 1: Create a node for each instruction.
    for (auto& bb : func) {
        for (auto& inst : bb) {
            OpClass cls = classifyOp(&inst);
            addNode(cls, &inst);
        }
    }

    // Phase 2: Add data-dependency edges based on def-use chains.
    // Use OpClass-level latency estimate for the PRODUCER instruction instead
    // of a flat "1" — this makes the dependency graph's critical-path
    // computation much more accurate (e.g. mul ≈ 3 cycles, div ≈ 20 cycles).
    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto consIt = instToNode_.find(&inst);
            if (consIt == instToNode_.end()) continue;
            unsigned consId = consIt->second;

            for (unsigned i = 0; i < inst.getNumOperands(); ++i) {
                if (auto* opInst = llvm::dyn_cast<llvm::Instruction>(inst.getOperand(i))) {
                    auto prodIt = instToNode_.find(opInst);
                    if (prodIt != instToNode_.end()) {
                        // Use the producer's OpClass for a rough latency.
                        unsigned prodLat = 1;
                        const auto* prodNode = getNode(prodIt->second);
                        if (prodNode) {
                            switch (prodNode->opClass) {
                            case OpClass::IntMul:   prodLat = 3;  break;
                            case OpClass::IntDiv:   prodLat = 20; break;
                            case OpClass::FPMul:
                            case OpClass::FMA:
                            case OpClass::Load:
                            case OpClass::FPArith:  prodLat = 4;  break;
                            case OpClass::FPDiv:    prodLat = 15; break;
                            case OpClass::Phi:      prodLat = 0;  break;
                            default:                prodLat = 1;  break;
                            }
                        }
                        addEdge(prodIt->second, consId, DepType::Data, prodLat);
                    }
                }
            }
        }
    }

    // Phase 3: Add memory ordering edges within each basic block.
    // Track the last load and last store so we can add all necessary
    // hazard edges:
    //   Store → Store (WAW): output dependence
    //   Store → Load  (RAW): true data dependence through memory
    //   Load  → Store (WAR): anti-dependence — a store after a load to the
    //                         same address must not be reordered before the load.
    //
    // Basic alias analysis: when both a load and a store use a GEP from the
    // same base pointer with constant indices, we check whether the indices
    // overlap.  Non-overlapping accesses can skip the memory edge, reducing
    // false dependencies and enabling better scheduling.
    auto mayAlias = [](const llvm::Instruction* a, const llvm::Instruction* b) -> bool {
        // Conservative default: assume they may alias.
        const llvm::Value* ptrA = nullptr;
        const llvm::Value* ptrB = nullptr;
        if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(a)) ptrA = ld->getPointerOperand();
        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(a)) ptrA = st->getPointerOperand();
        if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(b)) ptrB = ld->getPointerOperand();
        if (auto* st = llvm::dyn_cast<llvm::StoreInst>(b)) ptrB = st->getPointerOperand();
        if (!ptrA || !ptrB) return true;

        // If pointers are identical, they definitely alias.
        if (ptrA == ptrB) return true;

        // If both are GEPs from the same base with constant offsets, compare.
        auto* gepA = llvm::dyn_cast<llvm::GetElementPtrInst>(ptrA);
        auto* gepB = llvm::dyn_cast<llvm::GetElementPtrInst>(ptrB);
        if (gepA && gepB &&
            gepA->getPointerOperand() == gepB->getPointerOperand() &&
            gepA->getNumIndices() == 1 && gepB->getNumIndices() == 1) {
            auto* idxA = llvm::dyn_cast<llvm::ConstantInt>(gepA->getOperand(1));
            auto* idxB = llvm::dyn_cast<llvm::ConstantInt>(gepB->getOperand(1));
            if (idxA && idxB && idxA->getSExtValue() != idxB->getSExtValue())
                return false; // Different constant offsets — no alias.
        }

        // Different base pointers from different allocations cannot alias.
        auto underlyingAlloc = [](const llvm::Value* v) -> const llvm::Value* {
            while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(v))
                v = gep->getPointerOperand();
            return v;
        };
        const llvm::Value* baseA = underlyingAlloc(ptrA);
        const llvm::Value* baseB = underlyingAlloc(ptrB);
        if (baseA != baseB) {
            // Distinct allocas or distinct function args never alias.
            bool allocA = llvm::isa<llvm::AllocaInst>(baseA) || llvm::isa<llvm::Argument>(baseA);
            bool allocB = llvm::isa<llvm::AllocaInst>(baseB) || llvm::isa<llvm::Argument>(baseB);
            if (allocA && allocB) return false;
        }

        return true; // conservative fallback
    };

    for (auto& bb : func) {
        llvm::Instruction* lastStore = nullptr;
        llvm::Instruction* lastLoad  = nullptr;
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Store) {
                // WAW: Store → Store
                if (lastStore && mayAlias(lastStore, &inst)) {
                    auto srcIt = instToNode_.find(lastStore);
                    auto dstIt = instToNode_.find(&inst);
                    if (srcIt != instToNode_.end() && dstIt != instToNode_.end())
                        addEdge(srcIt->second, dstIt->second, DepType::Memory, 0);
                }
                // WAR: Load → Store (anti-dependence)
                if (lastLoad && mayAlias(lastLoad, &inst)) {
                    auto srcIt = instToNode_.find(lastLoad);
                    auto dstIt = instToNode_.find(&inst);
                    if (srcIt != instToNode_.end() && dstIt != instToNode_.end())
                        addEdge(srcIt->second, dstIt->second, DepType::Memory, 0);
                }
                lastStore = &inst;
            }
            if (inst.getOpcode() == llvm::Instruction::Load) {
                // RAW: Store → Load
                if (lastStore && mayAlias(lastStore, &inst)) {
                    auto srcIt = instToNode_.find(lastStore);
                    auto dstIt = instToNode_.find(&inst);
                    if (srcIt != instToNode_.end() && dstIt != instToNode_.end())
                        addEdge(srcIt->second, dstIt->second, DepType::Memory, 0);
                }
                lastLoad = &inst;
            }
        }
    }
}

unsigned ProgramGraph::criticalPathLength() const {
    if (nodes_.empty()) return 0;

    // Topological sort + longest-path computation.
    std::vector<unsigned> dist(nodes_.size(), 0);
    std::vector<unsigned> inDeg(nodes_.size(), 0);

    for (const auto& e : edges_) {
        if (e.dstId < inDeg.size()) inDeg[e.dstId]++;
    }

    std::queue<unsigned> ready;
    for (unsigned i = 0; i < nodes_.size(); ++i) {
        if (inDeg[i] == 0) {
            ready.push(i);
            dist[i] = 1; // Minimum 1 cycle per instruction
        }
    }

    while (!ready.empty()) {
        unsigned u = ready.front();
        ready.pop();
        for (const auto& e : edges_) {
            if (e.srcId == u) {
                unsigned newDist = dist[u] + e.latency;
                if (newDist > dist[e.dstId]) {
                    dist[e.dstId] = newDist;
                }
                if (--inDeg[e.dstId] == 0) {
                    ready.push(e.dstId);
                }
            }
        }
    }

    unsigned maxDist = 0;
    for (unsigned d : dist) {
        if (d > maxDist) maxDist = d;
    }
    return maxDist;
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 5 — Hardware-aware cost model
// ═════════════════════════════════════════════════════════════════════════════

HardwareCostModel::HardwareCostModel(const HardwareGraph& hw) : hw_(hw) {
    // Derive vector width from hardware graph.
    auto vecNodes = hw_.findNodes(ResourceType::VectorALU);
    if (!vecNodes.empty()) {
        // Pipeline depth as a proxy for vector width category
        unsigned depth = vecNodes[0]->pipelineDepth;
        if (depth >= 16) vectorWidth_ = 16;      // AVX-512
        else if (depth >= 8) vectorWidth_ = 8;    // AVX2
        else vectorWidth_ = 4;                     // SSE / NEON
    }

    // Derive issue width from dispatch node count.
    auto dispatchNodes = hw_.findNodes(ResourceType::Dispatch);
    if (!dispatchNodes.empty()) {
        issueWidth_ = dispatchNodes[0]->throughput;
    }

    // Derive cache miss penalties from cache hierarchy.
    auto l1Nodes = hw_.findNodes(ResourceType::L1DCache);
    if (!l1Nodes.empty()) cacheMissL1Penalty_ = l1Nodes[0]->latency;
    auto l2Nodes = hw_.findNodes(ResourceType::L2Cache);
    if (!l2Nodes.empty()) cacheMissL2Penalty_ = l2Nodes[0]->latency;
    auto l3Nodes = hw_.findNodes(ResourceType::L3Cache);
    if (!l3Nodes.empty()) cacheMissL3Penalty_ = l3Nodes[0]->latency;
}

OpClass HardwareCostModel::classifyInstruction(const llvm::Instruction* inst) const {
    return classifyOp(inst);
}

double HardwareCostModel::instructionCost(const llvm::Instruction* inst) const {
    if (!inst) return 0.0;

    OpClass cls = classifyOp(inst);

    // Use direct latency lookup based on operation class for more accurate
    // costs than the hardware graph node (which has a single average latency).
    switch (cls) {
    case OpClass::IntArith:
    case OpClass::Shift:
    case OpClass::Comparison:
    case OpClass::Conversion:
        return 1.0; // 1-cycle ALU ops

    case OpClass::IntMul: {
        // Mul latency from the hardware graph's ALU node count (proxy for
        // architecture generation).  Skylake: 3 cycles, etc.
        auto nodes = hw_.findNodes(ResourceType::IntegerALU);
        return nodes.empty() ? 3.0 : std::max(nodes[0]->latency * 3.0, 3.0);
    }

    case OpClass::IntDiv: {
        auto nodes = hw_.findNodes(ResourceType::DividerUnit);
        return nodes.empty() ? 25.0 : nodes[0]->latency;
    }

    case OpClass::FPArith: {
        auto nodes = hw_.findNodes(ResourceType::FMAUnit);
        return nodes.empty() ? 4.0 : nodes[0]->latency;
    }

    case OpClass::FPMul: {
        auto nodes = hw_.findNodes(ResourceType::FMAUnit);
        return nodes.empty() ? 4.0 : nodes[0]->latency;
    }

    case OpClass::FPDiv: {
        auto nodes = hw_.findNodes(ResourceType::DividerUnit);
        return nodes.empty() ? 14.0 : nodes[0]->latency;
    }

    case OpClass::FMA: {
        auto nodes = hw_.findNodes(ResourceType::FMAUnit);
        return nodes.empty() ? 4.0 : nodes[0]->latency;
    }

    case OpClass::VectorOp: {
        auto nodes = hw_.findNodes(ResourceType::VectorALU);
        return nodes.empty() ? 4.0 : nodes[0]->latency;
    }

    case OpClass::Load: {
        auto nodes = hw_.findNodes(ResourceType::LoadUnit);
        return nodes.empty() ? 4.0 : nodes[0]->latency;
    }

    case OpClass::Store: {
        auto nodes = hw_.findNodes(ResourceType::StoreUnit);
        return nodes.empty() ? 4.0 : nodes[0]->latency;
    }

    case OpClass::Branch: {
        auto nodes = hw_.findNodes(ResourceType::BranchUnit);
        return nodes.empty() ? 1.0 : nodes[0]->latency;
    }

    case OpClass::Phi:
        return 0.0;

    case OpClass::Call:
        return 10.0;

    case OpClass::Intrinsic:
        return 1.0;

    default:
        return 3.0;
    }
}

double HardwareCostModel::simulateExecution(const ProgramGraph& pg) const {
    if (pg.nodeCount() == 0) return 0.0;

    // Simple cycle-accurate simulation: assign each node to the earliest
    // cycle where all dependencies are satisfied and a port is available.
    size_t n = pg.nodeCount();
    std::vector<unsigned> scheduledCycle(n, 0);
    std::vector<unsigned> inDeg(n, 0);

    for (const auto& e : pg.edges()) {
        if (e.dstId < n) inDeg[e.dstId]++;
    }

    std::queue<unsigned> ready;
    for (unsigned i = 0; i < n; ++i) {
        if (inDeg[i] == 0) ready.push(i);
    }

    unsigned maxCycle = 0;

    while (!ready.empty()) {
        unsigned u = ready.front();
        ready.pop();

        // Compute earliest start cycle from predecessors.
        unsigned earliest = 0;
        for (const auto& e : pg.edges()) {
            if (e.dstId == u) {
                unsigned predEnd = scheduledCycle[e.srcId] + e.latency;
                if (predEnd > earliest) earliest = predEnd;
            }
        }
        scheduledCycle[u] = earliest;

        // Add instruction latency.
        const ProgramNode* node = pg.getNode(u);
        if (node) {
            unsigned endCycle = earliest + static_cast<unsigned>(node->estimatedLatency);
            if (endCycle > maxCycle) maxCycle = endCycle;
        }

        // Release successors.
        for (const auto& e : pg.edges()) {
            if (e.srcId == u) {
                if (--inDeg[e.dstId] == 0) {
                    ready.push(e.dstId);
                }
            }
        }
    }

    return static_cast<double>(maxCycle);
}

double HardwareCostModel::portContentionPenalty(const ProgramGraph& pg) const {
    // Count operations per resource type and compare with available ports.
    std::unordered_map<int, unsigned> opCounts;
    for (const auto& node : pg.nodes()) {
        opCounts[static_cast<int>(node.opClass)]++;
    }

    double penalty = 0.0;

    // Integer ALU contention
    auto aluNodes = hw_.findNodes(ResourceType::IntegerALU);
    unsigned aluPorts = 0;
    for (auto* n : aluNodes) aluPorts += n->count;
    if (aluPorts > 0) {
        unsigned intOps = opCounts[static_cast<int>(OpClass::IntArith)] +
                          opCounts[static_cast<int>(OpClass::IntMul)] +
                          opCounts[static_cast<int>(OpClass::Shift)] +
                          opCounts[static_cast<int>(OpClass::Comparison)];
        if (intOps > aluPorts) {
            penalty += static_cast<double>(intOps - aluPorts) * 0.5;
        }
    }

    // Load port contention
    auto loadNodes = hw_.findNodes(ResourceType::LoadUnit);
    unsigned loadPorts = 0;
    for (auto* n : loadNodes) loadPorts += n->count;
    if (loadPorts > 0) {
        unsigned loadOps = opCounts[static_cast<int>(OpClass::Load)];
        if (loadOps > loadPorts) {
            penalty += static_cast<double>(loadOps - loadPorts) * cacheMissL1Penalty_;
        }
    }

    return penalty;
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 7 — Hardware database
// ═════════════════════════════════════════════════════════════════════════════

/// Return a Skylake (Intel 6th–10th gen) microarchitecture profile.
[[gnu::cold]] static MicroarchProfile skylakeProfile() {
    MicroarchProfile p;
    p.name = "skylake";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 6;       // µop cache delivers 6/cycle; legacy decode 4/cycle
    p.issueWidth = 6;        // 6 µops dispatched per cycle
    p.pipelineDepth = 14;    // front-end to retire (from µop cache path)
    p.intALUs = 4;           // P0, P1, P5, P6
    p.vecUnits = 3;          // P0, P1, P5 (vector ALU)
    p.fmaUnits = 2;          // P0, P1
    p.loadPorts = 2;         // P2, P3
    p.storePorts = 2;        // P4, P7 (store data)
    p.branchUnits = 2;       // P0 (branch2), P6 (branch1)
    p.agus = 2;              // P2, P3 also do AGU
    p.dividers = 1;          // shared divider on P0
    // Skylake: integer multiply on ports P0 and P1 only (2 of the 4 ALU ports).
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 26;
    p.latFPAdd = 4; p.latFPMul = 4; p.latFPDiv = 14; p.latFMA = 4;
    p.latLoad = 5; p.latStore = 5; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.25; p.tputIntMul = 1.0;
    p.tputFPAdd = 0.5; p.tputFPMul = 0.5;
    p.tputLoad = 0.5; p.tputStore = 1.0;
    p.l1DSize = 32; p.l1DLatency = 4;   // 4-cycle load-to-use
    p.l2Size = 256; p.l2Latency = 12;
    p.l3Size = 8192; p.l3Latency = 42;
    p.cacheLineSize = 64;
    p.vectorWidth = 256; // AVX2
    p.intRegisters = 16; p.vecRegisters = 16; p.fpRegisters = 16;
    p.branchMispredictPenalty = 15.0;
    p.btbEntries = 4096;
    p.memoryLatency = 200;
    p.robSize = 224;
    return p;
}

/// Return a Haswell (Intel 4th gen) microarchitecture profile.
[[gnu::cold]] static MicroarchProfile haswellProfile() {
    MicroarchProfile p = skylakeProfile();
    p.name = "haswell";
    p.decodeWidth = 4;
    p.issueWidth = 4;
    p.l3Latency = 36;
    p.branchMispredictPenalty = 15.0;
    // Haswell: integer multiply on port P1 only (1 of the 4 ALU ports).
    p.mulPortCount = 1;
    p.robSize = 192;
    return p;
}

/// Return an Intel Alder Lake / Raptor Lake (Golden Cove P-core) profile.
[[gnu::cold]] static MicroarchProfile alderlakeProfile() {
    MicroarchProfile p;
    p.name = "alderlake";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 6;        // 6-wide decode from µop cache
    p.issueWidth = 6;         // 6 µops dispatched per cycle
    p.pipelineDepth = 14;     // similar depth to Skylake, improved prediction
    p.intALUs = 5;            // 5 integer execution ports
    p.vecUnits = 3;           // 3 vector ALU ports
    p.fmaUnits = 2;           // 2 FMA units
    p.loadPorts = 2;          // 2 load ports
    p.storePorts = 2;         // 2 store data ports
    p.branchUnits = 2;
    p.agus = 3;               // 3 AGU ports (2 load + 1 store address)
    p.dividers = 1;
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 23;
    p.latFPAdd = 3; p.latFPMul = 4; p.latFPDiv = 11; p.latFMA = 4;
    p.latLoad = 5; p.latStore = 5; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.20; p.tputIntMul = 1.0;
    p.tputFPAdd = 0.5; p.tputFPMul = 0.5;
    p.tputLoad = 0.5; p.tputStore = 1.0;
    p.l1DSize = 48;           // 48 KB L1D
    p.l1DLatency = 5;
    p.l2Size = 1280;          // 1.25 MB L2 per P-core
    p.l2Latency = 12;
    p.l3Size = 30720;         // 30 MB shared L3
    p.l3Latency = 44;
    p.cacheLineSize = 64;
    p.vectorWidth = 256;      // AVX2
    p.intRegisters = 16; p.vecRegisters = 16; p.fpRegisters = 16;
    p.branchMispredictPenalty = 14.0;
    p.btbEntries = 12288;     // much larger BTB
    p.memoryLatency = 180;
    p.robSize = 256;
    return p;
}

/// Return an AMD Zen 4 (Ryzen 7000 / EPYC Genoa) profile.
[[gnu::cold]] static MicroarchProfile zen4Profile() {
    MicroarchProfile p;
    p.name = "znver4";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 4;        // 4-wide x86 decode
    p.issueWidth = 6;         // 6-wide dispatch to backend
    p.pipelineDepth = 19;     // integer pipeline ~19 stages (fetch to retire)
    p.intALUs = 4;            // 4 integer ALU pipes
    p.vecUnits = 2;           // 2× 256-bit FP/SIMD pipes
    p.fmaUnits = 2;           // 2 FMA units
    p.loadPorts = 3;          // 3 load/store AGU units
    p.storePorts = 2;         // 2 store data units
    p.branchUnits = 1;
    p.agus = 3;
    p.dividers = 1;
    // Zen 4: integer multiply on 2 of the 4 integer execution pipes.
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 17;
    p.latFPAdd = 3; p.latFPMul = 3; p.latFPDiv = 13; p.latFMA = 4;
    p.latLoad = 4; p.latStore = 4; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.25; p.tputIntMul = 0.5;
    p.tputFPAdd = 0.5; p.tputFPMul = 0.5;
    p.tputLoad = 0.33; p.tputStore = 0.5;
    p.l1DSize = 32; p.l1DLatency = 4;
    p.l2Size = 1024; p.l2Latency = 12;
    p.l3Size = 32768; p.l3Latency = 50;
    p.cacheLineSize = 64;
    p.vectorWidth = 256; // AVX2 natively (AVX-512 double-pumped from 256-bit)
    p.intRegisters = 16; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 13.0;
    p.btbEntries = 6144;
    p.memoryLatency = 180;
    p.robSize = 320;
    return p;
}

/// Return an AMD Zen 3 (Ryzen 5000) profile.
[[gnu::cold]] static MicroarchProfile zen3Profile() {
    MicroarchProfile p = zen4Profile();
    p.name = "znver3";
    p.pipelineDepth = 19;     // similar pipeline depth to Zen 4
    p.loadPorts = 3;
    p.storePorts = 2;
    p.latIntDiv = 18;
    p.latFPDiv = 15;
    p.l2Size = 512;           // 512 KB L2 per core (vs 1 MB on Zen 4)
    p.l3Size = 32768;
    p.l3Latency = 46;
    p.vectorWidth = 256;
    p.vecRegisters = 16;      // no AVX-512 support
    p.fpRegisters = 16;
    p.robSize = 256;
    return p;
}

/// Return an Apple M1/M2 Firestorm (performance core) profile.
[[gnu::cold]] static MicroarchProfile appleMProfile() {
    MicroarchProfile p;
    p.name = "apple-m1";
    p.isa = ISAFamily::AArch64;
    p.decodeWidth = 8;
    p.issueWidth = 8;
    p.pipelineDepth = 13;
    p.intALUs = 6;
    p.vecUnits = 4;
    p.fmaUnits = 4;
    p.loadPorts = 3;
    p.storePorts = 2;
    p.branchUnits = 2;
    p.agus = 3;
    p.dividers = 2;
    // Apple M1: multiply available on 4 of the 6 integer execution ports.
    p.mulPortCount = 4;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 10;
    p.latFPAdd = 3; p.latFPMul = 3; p.latFPDiv = 10; p.latFMA = 4;
    p.latLoad = 3; p.latStore = 3; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.17; p.tputIntMul = 0.5;
    p.tputFPAdd = 0.25; p.tputFPMul = 0.25;
    p.tputLoad = 0.33; p.tputStore = 0.5;
    p.l1DSize = 128; p.l1DLatency = 3;
    p.l2Size = 4096; p.l2Latency = 12;
    p.l3Size = 16384; p.l3Latency = 36;
    p.cacheLineSize = 128; // Apple uses 128-byte cache lines
    p.vectorWidth = 128; // NEON (128-bit)
    p.intRegisters = 31; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 12.0;
    p.btbEntries = 7168;
    p.memoryLatency = 120;
    p.robSize = 600;
    return p;
}

/// Return an ARM Neoverse V2 (server) profile.
[[gnu::cold]] static MicroarchProfile neoverseV2Profile() {
    MicroarchProfile p;
    p.name = "neoverse-v2";
    p.isa = ISAFamily::AArch64;
    p.decodeWidth = 8;
    p.issueWidth = 8;
    p.pipelineDepth = 11;
    p.intALUs = 6;
    p.vecUnits = 4;
    p.fmaUnits = 4;
    p.loadPorts = 3;
    p.storePorts = 2;
    p.branchUnits = 2;
    p.agus = 3;
    p.dividers = 2;
    // Neoverse V2: multiply available on 4 of the 6 integer ALU pipes.
    p.mulPortCount = 4;
    p.latIntAdd = 1; p.latIntMul = 2; p.latIntDiv = 12;
    p.latFPAdd = 2; p.latFPMul = 3; p.latFPDiv = 10; p.latFMA = 4;
    p.latLoad = 4; p.latStore = 4; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.17; p.tputIntMul = 0.5;
    p.tputFPAdd = 0.25; p.tputFPMul = 0.25;
    p.tputLoad = 0.33; p.tputStore = 0.5;
    p.l1DSize = 64; p.l1DLatency = 4;
    p.l2Size = 1024; p.l2Latency = 11;
    p.l3Size = 32768; p.l3Latency = 38;
    p.cacheLineSize = 64;
    p.vectorWidth = 256; // SVE2 at 256-bit
    p.intRegisters = 31; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 11.0;
    p.btbEntries = 8192;
    p.memoryLatency = 150;
    p.robSize = 256;
    return p;
}

/// Return an ARM Neoverse N2 profile.
[[gnu::cold]] static MicroarchProfile neoverseN2Profile() {
    MicroarchProfile p = neoverseV2Profile();
    p.name = "neoverse-n2";
    p.issueWidth = 5;
    p.intALUs = 4;
    p.vecUnits = 2;
    p.fmaUnits = 2;
    p.loadPorts = 2;
    p.l2Size = 512;
    p.l3Latency = 42;
    p.vectorWidth = 128;
    return p;
}

/// Return a generic RISC-V 64-bit in-order core profile.
[[gnu::cold]] static MicroarchProfile riscvGenericProfile() {
    MicroarchProfile p;
    p.name = "generic-rv64";
    p.isa = ISAFamily::RISCV64;
    p.decodeWidth = 2;
    p.issueWidth = 2;
    p.pipelineDepth = 7;
    p.intALUs = 2;
    p.vecUnits = 1;
    p.fmaUnits = 1;
    p.loadPorts = 1;
    p.storePorts = 1;
    p.branchUnits = 1;
    p.agus = 1;
    p.dividers = 1;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 20;
    p.latFPAdd = 4; p.latFPMul = 4; p.latFPDiv = 18; p.latFMA = 5;
    p.latLoad = 3; p.latStore = 3; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.5; p.tputIntMul = 1.0;
    p.tputFPAdd = 1.0; p.tputFPMul = 1.0;
    p.tputLoad = 1.0; p.tputStore = 1.0;
    p.l1DSize = 32; p.l1DLatency = 3;
    p.l2Size = 512; p.l2Latency = 10;
    p.l3Size = 2048; p.l3Latency = 30;
    p.cacheLineSize = 64;
    p.vectorWidth = 128; // RVV at 128-bit VLEN
    p.intRegisters = 31; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 6.0;
    p.btbEntries = 512;
    p.memoryLatency = 200;
    return p;
}

/// Return a SiFive U74 (RISC-V) profile.
[[gnu::cold]] static MicroarchProfile sifiveU74Profile() {
    MicroarchProfile p = riscvGenericProfile();
    p.name = "sifive-u74";
    p.pipelineDepth = 8;
    p.intALUs = 2;
    p.l1DSize = 32;
    p.l2Size = 2048;
    return p;
}

/// Return an AWS Graviton3 profile (Arm Neoverse V1-based).
/// Graviton3: 64-core, 256-bit SVE, DDR5, 8-wide dispatch.
[[gnu::cold]] static MicroarchProfile graviton3Profile() {
    MicroarchProfile p;
    p.name = "graviton3";
    p.isa = ISAFamily::AArch64;
    p.decodeWidth = 8;
    p.issueWidth = 8;
    p.pipelineDepth = 13;      // Neoverse V1 pipeline
    p.intALUs = 6;
    p.vecUnits = 4;            // 4× 128-bit NEON / 2× 256-bit SVE
    p.fmaUnits = 4;
    p.loadPorts = 3;
    p.storePorts = 2;
    p.branchUnits = 2;
    p.agus = 3;
    p.dividers = 2;
    p.mulPortCount = 4;        // multiply on 4 of 6 integer pipes
    p.latIntAdd = 1; p.latIntMul = 2; p.latIntDiv = 12;
    p.latFPAdd = 2; p.latFPMul = 3; p.latFPDiv = 10; p.latFMA = 4;
    p.latLoad = 4; p.latStore = 4; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.17;       // 6 ALUs → ~6/cycle
    p.tputIntMul = 0.5;
    p.tputFPAdd = 0.25; p.tputFPMul = 0.25;
    p.tputLoad = 0.33; p.tputStore = 0.5;
    p.l1DSize = 64; p.l1DLatency = 4;
    p.l2Size = 1024; p.l2Latency = 11;
    p.l3Size = 32768; p.l3Latency = 40;
    p.cacheLineSize = 64;
    p.vectorWidth = 256;       // SVE at 256-bit
    p.intRegisters = 31; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 11.0;
    p.btbEntries = 8192;
    p.memoryLatency = 140;     // DDR5 latency
    p.robSize = 256;
    return p;
}

/// Return an AWS Graviton4 profile (Arm Neoverse V2-based).
/// Graviton4: 96-core, 256-bit SVE2, DDR5-5600, wider backend.
[[gnu::cold]] static MicroarchProfile graviton4Profile() {
    MicroarchProfile p = neoverseV2Profile();
    p.name = "graviton4";
    p.l2Size = 2048;           // 2 MB L2 per core
    p.l3Size = 36864;          // 36 MB shared L3
    p.memoryLatency = 130;     // DDR5-5600
    p.robSize = 256;
    return p;
}

/// Return an Intel Lunar Lake profile (Lion Cove P-cores, 2024).
/// Lion Cove: 8-wide decode, 8-wide dispatch, AVX2, improved branch pred.
[[gnu::cold]] static MicroarchProfile lunarLakeProfile() {
    MicroarchProfile p;
    p.name = "lunar-lake";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 8;         // 8-wide decode
    p.issueWidth = 8;          // 8-wide dispatch
    p.pipelineDepth = 14;
    p.intALUs = 6;             // 6 integer execution ports
    p.vecUnits = 3;            // 3 vector ALU ports
    p.fmaUnits = 2;
    p.loadPorts = 3;           // 3 load ports
    p.storePorts = 2;
    p.branchUnits = 2;
    p.agus = 3;
    p.dividers = 1;
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 20;
    p.latFPAdd = 3; p.latFPMul = 4; p.latFPDiv = 10; p.latFMA = 4;
    p.latLoad = 5; p.latStore = 5; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.17;      // 6 ALUs → ~6/cycle
    p.tputIntMul = 0.5;
    p.tputFPAdd = 0.5; p.tputFPMul = 0.5;
    p.tputLoad = 0.33; p.tputStore = 0.5;
    p.l1DSize = 48; p.l1DLatency = 5;
    p.l2Size = 2560;           // 2.5 MB L2 per P-core
    p.l2Latency = 12;
    p.l3Size = 12288;          // 12 MB shared L3
    p.l3Latency = 38;
    p.cacheLineSize = 64;
    p.vectorWidth = 256;       // AVX2
    p.intRegisters = 16; p.vecRegisters = 16; p.fpRegisters = 16;
    p.branchMispredictPenalty = 13.0;
    p.btbEntries = 16384;      // larger BTB
    p.memoryLatency = 170;     // LPDDR5x
    p.robSize = 256;
    return p;
}

/// Return an improved AMD Zen 5 profile (2024).
/// Zen 5: 8-wide dispatch, 6 ALU pipes, 2× 256-bit vector, AVX-512
/// via double-pump, improved branch prediction.
[[gnu::cold]] static MicroarchProfile zen5Profile() {
    MicroarchProfile p;
    p.name = "znver5";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 4;         // x86 decode still 4-wide
    p.issueWidth = 8;          // 8-wide dispatch to backend
    p.pipelineDepth = 18;      // slightly shorter than Zen 4
    p.intALUs = 6;             // 6 integer ALU pipes (up from 4)
    p.vecUnits = 4;            // 4× 256-bit SIMD pipes (up from 2)
    p.fmaUnits = 4;            // 4 FMA units (up from 2)
    p.loadPorts = 4;           // 4 load/store AGU units (up from 3)
    p.storePorts = 2;
    p.branchUnits = 2;         // 2 branch units (up from 1)
    p.agus = 4;
    p.dividers = 1;
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 15;
    p.latFPAdd = 3; p.latFPMul = 3; p.latFPDiv = 12; p.latFMA = 4;
    p.latLoad = 4; p.latStore = 4; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.17;      // 6 ALUs
    p.tputIntMul = 0.5;
    p.tputFPAdd = 0.25; p.tputFPMul = 0.25;
    p.tputLoad = 0.25; p.tputStore = 0.5;
    p.l1DSize = 48; p.l1DLatency = 4;
    p.l2Size = 1024; p.l2Latency = 12;
    p.l3Size = 32768; p.l3Latency = 45;
    p.cacheLineSize = 64;
    p.vectorWidth = 512;       // AVX-512 native (not double-pumped)
    p.intRegisters = 16; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 12.0;
    p.btbEntries = 8192;
    p.memoryLatency = 170;
    p.robSize = 448;
    return p;
}

/// Normalize a CPU name for lookup: lowercase, strip hyphens.
static std::string normalizeCpuName(const std::string& name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (c == '-' || c == '_') continue;
        result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return result;
}

std::optional<MicroarchProfile> lookupMicroarch(const std::string& cpuName) {
    if (cpuName.empty()) return std::nullopt;

    std::string normalized = normalizeCpuName(cpuName);

    // x86-64 microarchitectures
    if (normalized == "skylake" || normalized == "skylakeserver" ||
        normalized == "skylakeavx512" || normalized == "cascadelake" ||
        normalized == "cooperlake" || normalized == "cannonlake")
        return skylakeProfile();

    if (normalized == "haswell" || normalized == "broadwell")
        return haswellProfile();

    if (normalized == "alderlake" || normalized == "raptorlake" ||
        normalized == "meteorlake" || normalized == "arrowlake")
        return alderlakeProfile();

    if (normalized == "lunarlake" || normalized == "pantherlake")
        return lunarLakeProfile();

    if (normalized == "icelakeserver" || normalized == "icelakeclient" ||
        normalized == "tigerlake" || normalized == "sapphirerapids" ||
        normalized == "emeraldrapids") {
        auto p = skylakeProfile();
        p.name = cpuName;
        p.vectorWidth = 512; // AVX-512
        p.vecRegisters = 32;
        return p;
    }

    // AMD Zen family
    if (normalized == "znver4" || normalized == "zen4")
        return zen4Profile();
    if (normalized == "znver3" || normalized == "zen3")
        return zen3Profile();
    if (normalized == "znver2" || normalized == "zen2") {
        auto p = zen3Profile();
        p.name = "znver2";
        p.pipelineDepth = 19;
        p.l2Size = 512;
        p.l3Latency = 42;
        return p;
    }
    if (normalized == "znver1" || normalized == "zen") {
        auto p = zen3Profile();
        p.name = "znver1";
        p.pipelineDepth = 19;
        p.l2Size = 512;
        p.loadPorts = 2;
        p.l3Latency = 40;
        return p;
    }
    if (normalized == "znver5" || normalized == "zen5")
        return zen5Profile();

    // ARM64 — Apple Silicon
    if (normalized == "applem1" || normalized == "applema14")
        return appleMProfile();
    if (normalized == "applem2" || normalized == "applem2pro" ||
        normalized == "applem2max" || normalized == "applem2ultra") {
        auto p = appleMProfile();
        p.name = cpuName;
        p.l2Size = 16384;
        return p;
    }
    if (normalized == "applem3" || normalized == "applem3pro" ||
        normalized == "applem3max" || normalized == "applem3ultra" ||
        normalized == "applem4") {
        auto p = appleMProfile();
        p.name = cpuName;
        p.decodeWidth = 8;
        p.issueWidth = 10;
        p.intALUs = 6;
        p.vecUnits = 4;
        p.l1DSize = 192;
        p.l2Size = 16384;
        p.l3Size = 36864;
        return p;
    }

    // ARM64 — Neoverse (server)
    if (normalized == "neoversev2")
        return neoverseV2Profile();
    if (normalized == "neoversev1" || normalized == "neoversev1c") {
        auto p = neoverseV2Profile();
        p.name = "neoverse-v1";
        p.pipelineDepth = 13;
        return p;
    }
    if (normalized == "neoversen2")
        return neoverseN2Profile();
    if (normalized == "neoversen1" || normalized == "neoversen1c") {
        auto p = neoverseN2Profile();
        p.name = "neoverse-n1";
        p.pipelineDepth = 11;
        return p;
    }

    // ARM64 — AWS Graviton (server)
    if (normalized == "graviton3")
        return graviton3Profile();
    if (normalized == "graviton4")
        return graviton4Profile();

    // ARM64 — Cortex
    if (normalized == "cortexa78" || normalized == "cortexa78c" ||
        normalized == "cortexa77" || normalized == "cortexa76") {
        auto p = neoverseN2Profile();
        p.name = cpuName;
        p.issueWidth = 4;
        p.intALUs = 3;
        p.vectorWidth = 128;
        return p;
    }
    if (normalized == "cortexall" || normalized == "cortexx3" ||
        normalized == "cortexx4") {
        auto p = neoverseV2Profile();
        p.name = cpuName;
        return p;
    }

    // RISC-V
    if (normalized == "genericrv64" || normalized == "riscv64")
        return riscvGenericProfile();
    if (normalized == "sifiveu74")
        return sifiveU74Profile();

    // Generic / x86-64 baseline
    if (normalized == "x8664" || normalized == "x8664v2" ||
        normalized == "x8664v3" || normalized == "x8664v4") {
        auto p = haswellProfile();
        p.name = cpuName;
        if (normalized == "x8664v4") p.vectorWidth = 512;
        else if (normalized == "x8664v3") p.vectorWidth = 256;
        return p;
    }

    // Unknown architecture — return nullopt (fallback mode)
    return std::nullopt;
}

HardwareGraph buildHardwareGraph(const MicroarchProfile& profile) {
    HardwareGraph g;

    // Front-end: dispatch stage
    unsigned dispatch = g.addNode(ResourceType::Dispatch, "dispatch",
                                   1, 1.0, static_cast<double>(profile.issueWidth),
                                   profile.pipelineDepth);

    // Execution units
    unsigned intALU = g.addNode(ResourceType::IntegerALU, "int_alu",
                                 profile.intALUs,
                                 static_cast<double>(profile.latIntAdd),
                                 1.0 / profile.tputIntAdd, 1);

    unsigned vecALU = g.addNode(ResourceType::VectorALU, "vec_alu",
                                 profile.vecUnits,
                                 static_cast<double>(profile.latFPAdd),
                                 1.0 / profile.tputFPAdd,
                                 profile.vectorWidth / 64); // pipeline depth ≈ elements

    unsigned fmaUnit = g.addNode(ResourceType::FMAUnit, "fma_unit",
                                  profile.fmaUnits,
                                  static_cast<double>(profile.latFMA),
                                  1.0 / profile.tputFPMul, 1);

    unsigned loadUnit = g.addNode(ResourceType::LoadUnit, "load_unit",
                                   profile.loadPorts,
                                   static_cast<double>(profile.latLoad),
                                   1.0 / profile.tputLoad, 1);

    unsigned storeUnit = g.addNode(ResourceType::StoreUnit, "store_unit",
                                    profile.storePorts,
                                    static_cast<double>(profile.latStore),
                                    1.0 / profile.tputStore, 1);

    unsigned branchUnit = g.addNode(ResourceType::BranchUnit, "branch_unit",
                                     profile.branchUnits,
                                     static_cast<double>(profile.latBranch),
                                     1.0, 1);

    unsigned agu = g.addNode(ResourceType::AGU, "agu",
                              profile.agus, 1.0, 1.0, 1);

    unsigned divider = g.addNode(ResourceType::DividerUnit, "divider",
                                  profile.dividers,
                                  static_cast<double>(profile.latIntDiv),
                                  static_cast<double>(profile.latIntDiv), 1);

    // Cache hierarchy
    unsigned l1d = g.addNode(ResourceType::L1DCache, "l1d_cache",
                              1, static_cast<double>(profile.l1DLatency),
                              1.0, 1);

    unsigned l2 = g.addNode(ResourceType::L2Cache, "l2_cache",
                             1, static_cast<double>(profile.l2Latency),
                             1.0, 1);

    unsigned l3 = g.addNode(ResourceType::L3Cache, "l3_cache",
                             1, static_cast<double>(profile.l3Latency),
                             1.0, 1);

    unsigned mem = g.addNode(ResourceType::MainMemory, "main_memory",
                              1, static_cast<double>(profile.memoryLatency),
                              1.0, 1);

    // Register files
    unsigned intRegs = g.addNode(ResourceType::IntRegisterFile, "int_regs",
                                  profile.intRegisters, 0.0, 1.0, 1);

    unsigned vecRegs = g.addNode(ResourceType::VecRegisterFile, "vec_regs",
                                  profile.vecRegisters, 0.0, 1.0, 1);

    // Retirement stage
    unsigned retire = g.addNode(ResourceType::Retire, "retire",
                                 1, 1.0, static_cast<double>(profile.issueWidth), 1);

    // Dispatch → execution unit edges
    g.addEdge(dispatch, intALU, 0.0, static_cast<double>(profile.intALUs),
              "dispatch→int_alu");
    g.addEdge(dispatch, vecALU, 0.0, static_cast<double>(profile.vecUnits),
              "dispatch→vec_alu");
    g.addEdge(dispatch, fmaUnit, 0.0, static_cast<double>(profile.fmaUnits),
              "dispatch→fma");
    g.addEdge(dispatch, loadUnit, 0.0, static_cast<double>(profile.loadPorts),
              "dispatch→load");
    g.addEdge(dispatch, storeUnit, 0.0, static_cast<double>(profile.storePorts),
              "dispatch→store");
    g.addEdge(dispatch, branchUnit, 0.0, static_cast<double>(profile.branchUnits),
              "dispatch→branch");
    g.addEdge(dispatch, divider, 0.0, static_cast<double>(profile.dividers),
              "dispatch→divider");

    // Execution unit → register file / writeback edges
    g.addEdge(intALU, intRegs, 0.0, 1.0, "int_alu→int_regs");
    g.addEdge(intALU, retire, 0.0, 1.0, "int_alu→retire");
    g.addEdge(vecALU, vecRegs, 0.0, 1.0, "vec_alu→vec_regs");
    g.addEdge(vecALU, retire, 0.0, 1.0, "vec_alu→retire");
    g.addEdge(fmaUnit, vecRegs, 0.0, 1.0, "fma→vec_regs");
    g.addEdge(fmaUnit, retire, 0.0, 1.0, "fma→retire");
    g.addEdge(branchUnit, retire, 0.0, 1.0, "branch→retire");
    g.addEdge(divider, intRegs, 0.0, 1.0, "divider→int_regs");
    g.addEdge(divider, retire, 0.0, 1.0, "divider→retire");

    // Load/store → AGU → cache hierarchy
    g.addEdge(loadUnit, agu, 0.0, 1.0, "load→agu");
    g.addEdge(storeUnit, agu, 0.0, 1.0, "store→agu");
    g.addEdge(agu, l1d, 0.0, 1.0, "agu→l1d");

    // Cache hierarchy chain
    g.addEdge(l1d, l2, static_cast<double>(profile.l2Latency - profile.l1DLatency),
              1.0, "l1d→l2 (miss)");
    g.addEdge(l2, l3, static_cast<double>(profile.l3Latency - profile.l2Latency),
              1.0, "l2→l3 (miss)");
    g.addEdge(l3, mem, static_cast<double>(profile.memoryLatency - profile.l3Latency),
              1.0, "l3→mem (miss)");

    // Load writeback
    g.addEdge(loadUnit, intRegs, 0.0, 1.0, "load→int_regs");
    g.addEdge(loadUnit, retire, 0.0, 1.0, "load→retire");
    g.addEdge(storeUnit, retire, 0.0, 1.0, "store→retire");

    return g;
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 3 — Graph mapping optimizer (list scheduling)
// ═════════════════════════════════════════════════════════════════════════════

/// Map OpClass to the ResourceType that should execute it.
[[nodiscard]] static ResourceType mapOpToResource(OpClass op) {
    switch (op) {
    case OpClass::IntArith:
    case OpClass::IntMul:
    case OpClass::Shift:
    case OpClass::Comparison:
    case OpClass::Conversion:
        return ResourceType::IntegerALU;
    case OpClass::IntDiv:
        return ResourceType::DividerUnit;
    case OpClass::FPArith:
    case OpClass::FPMul:
    case OpClass::FMA:
        return ResourceType::FMAUnit;
    case OpClass::FPDiv:
        return ResourceType::DividerUnit;
    case OpClass::VectorOp:
        return ResourceType::VectorALU;
    case OpClass::Load:
        return ResourceType::LoadUnit;
    case OpClass::Store:
        return ResourceType::StoreUnit;
    case OpClass::Branch:
        return ResourceType::BranchUnit;
    default:
        return ResourceType::IntegerALU;
    }
}

/// Return the instruction latency for an OpClass from the profile.
[[nodiscard]] static unsigned getLatency(OpClass op, const MicroarchProfile& profile) {
    switch (op) {
    case OpClass::IntArith:
    case OpClass::Shift:
    case OpClass::Comparison:
    case OpClass::Conversion:
        return profile.latIntAdd;
    case OpClass::IntMul:   return profile.latIntMul;
    case OpClass::IntDiv:   return profile.latIntDiv;
    case OpClass::FPArith:  return profile.latFPAdd;
    case OpClass::FPMul:    return profile.latFPMul;
    case OpClass::FPDiv:    return profile.latFPDiv;
    case OpClass::FMA:      return profile.latFMA;
    case OpClass::Load:     return profile.latLoad;
    case OpClass::Store:    return profile.latStore;
    case OpClass::Branch:   return profile.latBranch;
    case OpClass::Phi:      return 0;
    default:                return 1;
    }
}

/// Return the available ports for a ResourceType from the profile.
[[nodiscard]] static unsigned getPortCount(ResourceType rt, const MicroarchProfile& profile) {
    switch (rt) {
    case ResourceType::IntegerALU:  return profile.intALUs;
    case ResourceType::VectorALU:   return profile.vecUnits;
    case ResourceType::FMAUnit:     return profile.fmaUnits;
    case ResourceType::LoadUnit:    return profile.loadPorts;
    case ResourceType::StoreUnit:   return profile.storePorts;
    case ResourceType::BranchUnit:  return profile.branchUnits;
    case ResourceType::DividerUnit: return profile.dividers;
    default:                        return 1;
    }
}

/// Return the precise instruction latency in cycles using the exact LLVM
/// opcode rather than the coarser OpClass grouping.
///
/// Notable special cases:
///   PHI / BitCast / IntToPtr / PtrToInt — zero latency (register rename)
///   FP conversion — uses the FP pipeline (latFPAdd)
///   FMA intrinsic  — latFMA
///   sqrt intrinsic — ≈ fdiv latency
///   Unknown calls  — conservative 10-cycle estimate
[[nodiscard]] static unsigned getOpcodeLatency(const llvm::Instruction* inst,
                                  const MicroarchProfile& profile) {
    // Conservative latency constants used when we cannot determine exact timing.
    constexpr unsigned kUnknownCallLatency = 10;      ///< Non-intrinsic call (ABI overhead)
    constexpr unsigned kUnknownIntrinsicLatency = 5;  ///< Unknown intrinsic (FP-ish pipeline)

    if (!inst) return 1;
    switch (inst->getOpcode()) {
    // ── Integer arithmetic ──────────────────────────────────────────────────
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
    case llvm::Instruction::Select:  // CMOV-like, single ALU cycle
        return profile.latIntAdd;
    case llvm::Instruction::Mul:
        return profile.latIntMul;
    case llvm::Instruction::SDiv:
    case llvm::Instruction::UDiv:
    case llvm::Instruction::SRem:
    case llvm::Instruction::URem:
        return profile.latIntDiv;

    // ── Integer shifts / comparisons ────────────────────────────────────────
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
        return profile.latShift;
    case llvm::Instruction::ICmp:
        return profile.latIntAdd; // ALU pipeline

    // ── Floating-point ──────────────────────────────────────────────────────
    case llvm::Instruction::FAdd:
    case llvm::Instruction::FSub:
    case llvm::Instruction::FCmp:
        return profile.latFPAdd;
    case llvm::Instruction::FMul:
        return profile.latFPMul;
    case llvm::Instruction::FDiv:
    case llvm::Instruction::FRem:
        return profile.latFPDiv;
    case llvm::Instruction::FNeg:
        return profile.latFPAdd;

    // ── Memory ──────────────────────────────────────────────────────────────
    case llvm::Instruction::Load:
        return profile.latLoad;
    case llvm::Instruction::Store:
        return profile.latStore;
    case llvm::Instruction::GetElementPtr:
        return profile.latIntAdd; // address arithmetic

    // ── Control flow ────────────────────────────────────────────────────────
    case llvm::Instruction::Br:
    case llvm::Instruction::Switch:
    case llvm::Instruction::IndirectBr:
        return profile.latBranch;
    case llvm::Instruction::Ret:
        return 0;

    // ── Type conversions ────────────────────────────────────────────────────
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
        return profile.latIntAdd; // requires ALU
    case llvm::Instruction::FPToUI:
    case llvm::Instruction::FPToSI:
    case llvm::Instruction::UIToFP:
    case llvm::Instruction::SIToFP:
    case llvm::Instruction::FPTrunc:
    case llvm::Instruction::FPExt:
        return profile.latFPAdd; // FP pipeline
    case llvm::Instruction::BitCast:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::PtrToInt:
        return 0; // free — handled by register rename on modern CPUs

    // ── PHI ─────────────────────────────────────────────────────────────────
    case llvm::Instruction::PHI:
        return 0; // resolved at the predecessor edge, not at the node itself

    // ── Calls / intrinsics ───────────────────────────────────────────────────
    case llvm::Instruction::Call: {
        const auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(inst);
        if (!ii) return kUnknownCallLatency;
        llvm::Intrinsic::ID id = ii->getIntrinsicID();
        switch (id) {
        case llvm::Intrinsic::fma:
        case llvm::Intrinsic::fmuladd:  return profile.latFMA;
        case llvm::Intrinsic::sqrt:     return profile.latFPDiv;
        case llvm::Intrinsic::powi:
        case llvm::Intrinsic::pow:      return profile.latFPDiv * 3; // iterative
        case llvm::Intrinsic::abs:
        case llvm::Intrinsic::smin:
        case llvm::Intrinsic::smax:
        case llvm::Intrinsic::umin:
        case llvm::Intrinsic::umax:     return profile.latIntAdd;
        case llvm::Intrinsic::minnum:
        case llvm::Intrinsic::maxnum:   return profile.latFPAdd;
        case llvm::Intrinsic::ctpop:
        case llvm::Intrinsic::ctlz:
        case llvm::Intrinsic::cttz:     return profile.latIntAdd;
        case llvm::Intrinsic::prefetch: return 0; // hint, no result latency
        default:                        return kUnknownIntrinsicLatency;
        }
    }

    default:
        return 1;
    }
}

MappingResult mapProgramToHardware(ProgramGraph& pg, const HardwareGraph& hw,
                                    const MicroarchProfile& profile) {
    MappingResult result;
    if (pg.nodeCount() == 0) return result;

    // Use the hardware graph to verify port counts are consistent.
    // (The profile is the primary source, but the graph provides validation.)
    (void)hw; // Graph structure used for validation in debug builds

    size_t n = pg.nodeCount();

    // Annotate program nodes with hardware latencies.
    for (unsigned i = 0; i < n; ++i) {
        ProgramNode* node = pg.getNodeMut(i);
        if (node) {
            node->estimatedLatency = static_cast<double>(getLatency(node->opClass, profile));
        }
    }

    // Compute in-degree for topological scheduling.
    std::vector<unsigned> inDeg(n, 0);
    for (const auto& e : pg.edges()) {
        if (e.dstId < n) inDeg[e.dstId]++;
    }

    // Ready queue: nodes with all predecessors scheduled.
    // Priority: higher critical-path priority first (longest path to exit).
    std::vector<unsigned> priority(n, 0);
    {
        // Compute bottom-up priority (longest path from node to any sink).
        std::vector<bool> visited(n, false);
        std::function<unsigned(unsigned)> computePriority = [&](unsigned id) -> unsigned {
            if (visited[id]) return priority[id];
            visited[id] = true;
            unsigned maxSucc = 0;
            for (const auto& e : pg.edges()) {
                if (e.srcId == id) {
                    unsigned sp = computePriority(e.dstId) + e.latency;
                    if (sp > maxSucc) maxSucc = sp;
                }
            }
            priority[id] = maxSucc + static_cast<unsigned>(
                pg.getNode(id) ? pg.getNode(id)->estimatedLatency : 1);
            return priority[id];
        };
        for (unsigned i = 0; i < n; ++i) computePriority(i);
    }

    // Scheduled cycle for each node.
    std::vector<unsigned> scheduledCycle(n, 0);
    std::vector<bool> scheduled(n, false);

    // Resource availability: next free cycle for each port type.
    // We track per-port-instance availability.
    std::unordered_map<int, std::vector<unsigned>> portAvail;
    auto initPort = [&](ResourceType rt) {
        unsigned count = getPortCount(rt, profile);
        portAvail[static_cast<int>(rt)].assign(count, 0);
    };
    initPort(ResourceType::IntegerALU);
    initPort(ResourceType::VectorALU);
    initPort(ResourceType::FMAUnit);
    initPort(ResourceType::LoadUnit);
    initPort(ResourceType::StoreUnit);
    initPort(ResourceType::BranchUnit);
    initPort(ResourceType::DividerUnit);

    unsigned totalScheduled = 0;
    unsigned maxCycle = 0;
    unsigned stallCycles = 0;

    while (totalScheduled < n) {
        // Collect ready nodes.
        std::vector<unsigned> ready;
        for (unsigned i = 0; i < n; ++i) {
            if (!scheduled[i] && inDeg[i] == 0) {
                ready.push_back(i);
            }
        }

        if (ready.empty()) {
            // Cycle with unresolved dependencies — break deadlock for
            // graphs with cycles (shouldn't happen in SSA form).
            for (unsigned i = 0; i < n; ++i) {
                if (!scheduled[i]) { ready.push_back(i); break; }
            }
            if (ready.empty()) break;
        }

        // Sort by priority (higher = more critical, schedule first).
        std::sort(ready.begin(), ready.end(), [&](unsigned a, unsigned b) {
            return priority[a] > priority[b];
        });

        // Limit to issue width per cycle.
        unsigned issued = 0;
        for (unsigned nodeId : ready) {
            if (issued >= profile.issueWidth) break;

            const ProgramNode* node = pg.getNode(nodeId);
            if (!node) continue;

            // Compute earliest cycle from dependencies.
            unsigned earliest = 0;
            for (const auto& e : pg.edges()) {
                if (e.dstId == nodeId && scheduled[e.srcId]) {
                    auto lat = static_cast<unsigned>(
                        pg.getNode(e.srcId) ? pg.getNode(e.srcId)->estimatedLatency : 1);
                    unsigned predEnd = scheduledCycle[e.srcId] + lat;
                    if (predEnd > earliest) earliest = predEnd;
                }
            }

            // Find a free port for this operation.
            ResourceType rt = mapOpToResource(node->opClass);
            auto& ports = portAvail[static_cast<int>(rt)];
            if (ports.empty()) {
                // No dedicated port — use default timing.
                scheduledCycle[nodeId] = earliest;
            } else {
                // Find the port with the earliest availability.
                unsigned bestPort = 0;
                unsigned bestTime = ports[0];
                for (unsigned p = 1; p < ports.size(); ++p) {
                    if (ports[p] < bestTime) {
                        bestTime = ports[p];
                        bestPort = p;
                    }
                }

                unsigned startCycle = std::max(earliest, bestTime);
                ports[bestPort] = startCycle + 1; // Port busy for 1 cycle (pipelined)

                scheduledCycle[nodeId] = startCycle;
                if (startCycle > earliest) {
                    stallCycles += (startCycle - earliest);
                }

                // Record schedule entry.
                ScheduleEntry entry;
                entry.nodeId = nodeId;
                entry.cycle = startCycle;
                entry.port = bestPort;
                entry.resource = rt;
                result.schedule.push_back(entry);
            }

            // Update program node.
            ProgramNode* mutableNode = pg.getNodeMut(nodeId);
            if (mutableNode) {
                mutableNode->scheduledCycle = scheduledCycle[nodeId];
                mutableNode->assignedPort = 0;
            }

            unsigned endCycle = scheduledCycle[nodeId] +
                static_cast<unsigned>(node->estimatedLatency);
            if (endCycle > maxCycle) maxCycle = endCycle;

            scheduled[nodeId] = true;
            totalScheduled++;
            issued++;

            // Release successors.
            for (const auto& e : pg.edges()) {
                if (e.srcId == nodeId) {
                    if (e.dstId < n) inDeg[e.dstId]--;
                }
            }
        }
    }

    result.totalCycles = maxCycle;
    result.stallCycles = stallCycles;

    // Compute port utilization.
    if (maxCycle > 0 && profile.issueWidth > 0) {
        result.portUtilization = static_cast<double>(totalScheduled) /
            (static_cast<double>(maxCycle) * profile.issueWidth);
    }

    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 4 — Hardware-aware transformations
// ═════════════════════════════════════════════════════════════════════════════

/// Detect and generate FMA: a*b + c → fma(a, b, c).
/// Also handles: c - a*b → fma(-a, b, c)  (FNMADD pattern).
/// Note: a*b - c → fma(a, b, -c) is handled separately by generateFMASub.
/// Returns the number of FMAs generated.
[[gnu::hot]] static unsigned generateFMA(llvm::Function& func, const MicroarchProfile& profile) {
    if (profile.fmaUnits == 0) return 0;

    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            // Pattern: fadd(fmul(a, b), c) → fma(a, b, c)
            if (inst.getOpcode() == llvm::Instruction::FAdd) {
                llvm::Value* op0 = inst.getOperand(0);
                llvm::Value* op1 = inst.getOperand(1);

                // Try both orderings.
                for (int swap = 0; swap < 2; ++swap) {
                    llvm::Value* mulCandidate = (swap == 0) ? op0 : op1;
                    llvm::Value* addend = (swap == 0) ? op1 : op0;

                    auto* fmul = llvm::dyn_cast<llvm::BinaryOperator>(mulCandidate);
                    if (fmul && fmul->getOpcode() == llvm::Instruction::FMul &&
                        fmul->hasOneUse()) {
                        llvm::IRBuilder<> builder(&inst);
                        llvm::Module* mod = func.getParent();
                        llvm::Type* ty = inst.getType();

                        llvm::Function* fmaFn = OMSC_GET_INTRINSIC(
                            mod, llvm::Intrinsic::fma, {ty});
                        llvm::Value* result = builder.CreateCall(
                            fmaFn, {fmul->getOperand(0), fmul->getOperand(1), addend},
                            "fma");
                        inst.replaceAllUsesWith(result);
                        toErase.push_back(&inst);
                        toErase.push_back(fmul);
                        count++;
                        break;
                    }
                }
            }

            // Pattern: fsub(c, fmul(a, b)) → fma(-a, b, c)
            // This is the "negated fused multiply-subtract" (FNMADD) pattern.
            // On x86, VFNMADD132/213/231 maps to this.
            // Note: fsub(fmul(a,b), c) → fma(a,b,-c) is handled by generateFMASub.
            if (inst.getOpcode() == llvm::Instruction::FSub) {
                llvm::Value* op0 = inst.getOperand(0);
                llvm::Value* op1 = inst.getOperand(1);

                // Only match when op0 is NOT an fmul (that's generateFMASub's job).
                auto* fmul = llvm::dyn_cast<llvm::BinaryOperator>(op1);
                auto* lhsMul = llvm::dyn_cast<llvm::BinaryOperator>(op0);
                bool lhsIsFmul = lhsMul && lhsMul->getOpcode() == llvm::Instruction::FMul;
                if (!lhsIsFmul && fmul && fmul->getOpcode() == llvm::Instruction::FMul &&
                    fmul->hasOneUse()) {
                    // fsub(c, fmul(a,b)) = fma(-a, b, c)
                    llvm::IRBuilder<> builder(&inst);
                    llvm::Module* mod = func.getParent();
                    llvm::Type* ty = inst.getType();
                    llvm::Function* fmaFn = OMSC_GET_INTRINSIC(
                        mod, llvm::Intrinsic::fma, {ty});
                    llvm::Value* negA = builder.CreateFNeg(fmul->getOperand(0), "fnmadd.neg");
                    llvm::Value* result = builder.CreateCall(
                        fmaFn, {negA, fmul->getOperand(1), op0},
                        "fnmadd");
                    inst.replaceAllUsesWith(result);
                    toErase.push_back(&inst);
                    toErase.push_back(fmul);
                    count++;
                    continue;
                }
            }
        }
    }

    for (auto* inst : toErase) {
        if (inst->use_empty()) inst->eraseFromParent();
    }
    return count;
}

/// Generate chained FMA for:
///   a*b + c*d → fma(c, d, fma(a, b, 0.0))
///   a*b - c*d → fma(a, b, fma(-c, d, 0.0))   (FMSUB chain)
/// This leverages 2+ FMA units by expressing the computation as two
/// dependent FMAs, which modern out-of-order processors can pipeline.
/// Returns the number of FMA chains generated.
[[gnu::hot]] static unsigned generateFMAChain(llvm::Function& func, const MicroarchProfile& profile) {
    if (profile.fmaUnits < 2) return 0;  // Need 2+ FMA units to benefit

    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            llvm::Module* mod = func.getParent();
            llvm::Type* ty = inst.getType();

            // Pattern: fadd(fmul(a,b), fmul(c,d)) → fma(c, d, fma(a, b, 0))
            if (inst.getOpcode() == llvm::Instruction::FAdd) {
                auto* lhsMul = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* rhsMul = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));

                if (lhsMul && rhsMul &&
                    lhsMul->getOpcode() == llvm::Instruction::FMul &&
                    rhsMul->getOpcode() == llvm::Instruction::FMul &&
                    lhsMul->hasOneUse() && rhsMul->hasOneUse()) {

                    llvm::IRBuilder<> builder(&inst);
                    llvm::Function* fmaFn = OMSC_GET_INTRINSIC(mod, llvm::Intrinsic::fma, {ty});

                    // First FMA: fma(a, b, 0.0)
                    llvm::Value* zero = llvm::ConstantFP::get(ty, 0.0);
                    llvm::Value* fma1 = builder.CreateCall(fmaFn,
                        {lhsMul->getOperand(0), lhsMul->getOperand(1), zero}, "fma_chain1");

                    // Second FMA: fma(c, d, fma1)
                    llvm::Value* fma2 = builder.CreateCall(fmaFn,
                        {rhsMul->getOperand(0), rhsMul->getOperand(1), fma1}, "fma_chain2");

                    inst.replaceAllUsesWith(fma2);
                    toErase.push_back(&inst);
                    toErase.push_back(lhsMul);
                    toErase.push_back(rhsMul);
                    count++;
                    continue;
                }
            }

            // Pattern: fsub(fmul(a,b), fmul(c,d)) → fma(a, b, fma(-c, d, 0.0))
            // This is the chained FMSUB (fused multiply-subtract) pattern.
            // On x86: VFMSUB + VFNMADD executed on 2 FMA units in parallel.
            if (inst.getOpcode() == llvm::Instruction::FSub) {
                auto* lhsMul = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* rhsMul = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));

                if (lhsMul && rhsMul &&
                    lhsMul->getOpcode() == llvm::Instruction::FMul &&
                    rhsMul->getOpcode() == llvm::Instruction::FMul &&
                    lhsMul->hasOneUse() && rhsMul->hasOneUse()) {

                    llvm::IRBuilder<> builder(&inst);
                    llvm::Function* fmaFn = OMSC_GET_INTRINSIC(mod, llvm::Intrinsic::fma, {ty});

                    // Inner FMA: fma(-c, d, 0.0)  [= -c*d]
                    llvm::Value* zero = llvm::ConstantFP::get(ty, 0.0);
                    llvm::Value* negC = builder.CreateFNeg(rhsMul->getOperand(0), "fmsub_chain.neg");
                    llvm::Value* fma1 = builder.CreateCall(fmaFn,
                        {negC, rhsMul->getOperand(1), zero}, "fmsub_chain1");

                    // Outer FMA: fma(a, b, -c*d) = a*b - c*d
                    llvm::Value* fma2 = builder.CreateCall(fmaFn,
                        {lhsMul->getOperand(0), lhsMul->getOperand(1), fma1}, "fmsub_chain2");

                    inst.replaceAllUsesWith(fma2);
                    toErase.push_back(&inst);
                    toErase.push_back(lhsMul);
                    toErase.push_back(rhsMul);
                    count++;
                    continue;
                }
            }
        }
    }

    for (auto* inst : toErase) {
        if (inst->use_empty()) inst->eraseFromParent();
    }
    return count;
}

/// Insert prefetch hints for strided memory access patterns in loops.
/// Returns the number of prefetches inserted.
static unsigned insertPrefetches(llvm::Function& func, const MicroarchProfile& profile) {
    unsigned count = 0;

    for (auto& bb : func) {
        // Look for basic blocks that are likely loop bodies (have a backedge).
        auto* term = bb.getTerminator();
        if (!term) continue;
        auto* br = llvm::dyn_cast<llvm::BranchInst>(term);
        if (!br) continue;

        bool isLoopBody = false;
        for (unsigned i = 0; i < br->getNumSuccessors(); ++i) {
            if (br->getSuccessor(i) == &bb) {
                isLoopBody = true;
                break;
            }
        }
        if (!isLoopBody) continue;

        // Find loads in this loop body.
        std::vector<llvm::LoadInst*> loads;
        for (auto& inst : bb) {
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                loads.push_back(load);
            }
        }

        // Insert prefetch for each load (prefetch the next cache line).
        for (auto* load : loads) {
            if (count >= 4) break; // Limit prefetches to avoid overhead

            llvm::IRBuilder<> builder(load);
            llvm::Module* mod = func.getParent();

            // Compute address + cache_line_size for prefetch (opaque ptr).
            llvm::Value* ptr = load->getPointerOperand();
            llvm::Type* ptrTy = ptr->getType(); // opaque ptr in LLVM 18+
            llvm::Value* offset = llvm::ConstantInt::get(
                builder.getInt64Ty(), profile.cacheLineSize);
            llvm::Value* prefetchAddr = builder.CreateGEP(
                builder.getInt8Ty(), ptr, offset, "prefetch_addr");

            // Insert llvm.prefetch intrinsic.
            llvm::Function* prefetchFn = OMSC_GET_INTRINSIC(
                mod, llvm::Intrinsic::prefetch, {ptrTy});

            // Args: ptr, rw (0=read), locality (3=high), cache_type (1=data)
            builder.CreateCall(prefetchFn, {
                prefetchAddr,
                builder.getInt32(0),  // read
                builder.getInt32(3),  // high locality
                builder.getInt32(1)   // data cache
            });
            count++;
        }
    }

    return count;
}

/// Optimise branch layout for the hardware's branch predictor.
/// Ensures the fall-through path is the most likely one.
/// Returns the number of branches optimized.
static unsigned optimizeBranchLayout(llvm::Function& func,
                                      const MicroarchProfile& /*profile*/) {
    unsigned count = 0;

    for (auto& bb : func) {
        auto* br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator());
        if (!br || !br->isConditional()) continue;
        if (br->getNumSuccessors() < 2) continue;

        llvm::BasicBlock* trueBB = br->getSuccessor(0);
        llvm::BasicBlock* falseBB = br->getSuccessor(1);

        // Heuristic: if one successor is an exit block (has return/unreachable),
        // put it on the unlikely (taken) path.
        bool trueIsExit = false;
        bool falseIsExit = false;
        if (trueBB->size() <= 2) {
            if (llvm::isa<llvm::ReturnInst>(trueBB->getTerminator()) ||
                llvm::isa<llvm::UnreachableInst>(trueBB->getTerminator()))
                trueIsExit = true;
        }
        if (falseBB->size() <= 2) {
            if (llvm::isa<llvm::ReturnInst>(falseBB->getTerminator()) ||
                llvm::isa<llvm::UnreachableInst>(falseBB->getTerminator()))
                falseIsExit = true;
        }

        // If the true branch is an exit (unlikely path), swap successors so
        // the hot (non-exit) path is the fall-through.  To preserve semantics
        // we must also invert the branch condition.  When the condition is a
        // compare instruction (ICmp/FCmp), we invert its predicate directly
        // — this is a zero-cost transformation that produces no extra
        // instructions.  Otherwise fall back to branch-weight metadata which
        // hints the backend without modifying control flow.
        if (trueIsExit && !falseIsExit) {
            llvm::Value* cond = br->getCondition();
            if (auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(cond)) {
                icmp->setPredicate(icmp->getInversePredicate());
                br->swapSuccessors();
                count++;
            } else if (auto* fcmp = llvm::dyn_cast<llvm::FCmpInst>(cond)) {
                fcmp->setPredicate(fcmp->getInversePredicate());
                br->swapSuccessors();
                count++;
            } else {
                // Non-compare condition: use branch weights as a layout hint.
                llvm::MDBuilder mdBuilder(func.getContext());
                auto* brWeights = mdBuilder.createBranchWeights(1, 99);
                br->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
                count++;
            }
        }
    }

    return count;
}

/// Expand FMA generation to also cover fsub(fmul(a,b), c) → fma(a, b, -c).
/// (Complements generateFMA which already handles fadd variants.)
/// Returns the number of additional FMAs generated.
static unsigned generateFMASub(llvm::Function& func, const MicroarchProfile& profile) {
    if (profile.fmaUnits == 0) return 0;

    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            // Pattern: fsub(fmul(a, b), c) → fma(a, b, -c)
            if (inst.getOpcode() == llvm::Instruction::FSub) {
                llvm::Value* op0 = inst.getOperand(0);
                llvm::Value* op1 = inst.getOperand(1);

                auto* fmul = llvm::dyn_cast<llvm::BinaryOperator>(op0);
                if (fmul && fmul->getOpcode() == llvm::Instruction::FMul &&
                    fmul->hasOneUse()) {
                    llvm::IRBuilder<> builder(&inst);
                    llvm::Module* mod = func.getParent();
                    llvm::Type* ty = inst.getType();

                    // fma(a, b, -c)
                    llvm::Value* negC = builder.CreateFNeg(op1, "fneg_c");
                    llvm::Function* fmaFn = OMSC_GET_INTRINSIC(mod, llvm::Intrinsic::fma, {ty});
                    llvm::Value* result = builder.CreateCall(
                        fmaFn, {fmul->getOperand(0), fmul->getOperand(1), negC}, "fma_sub");
                    inst.replaceAllUsesWith(result);
                    toErase.push_back(&inst);
                    toErase.push_back(fmul);
                    count++;
                }
            }
        }
    }

    for (auto* inst : toErase) {
        if (inst->use_empty()) inst->eraseFromParent();
    }
    return count;
}

/// Integer strength reduction: replace multiply-by-small-constant with
/// shifts and adds, which execute on more ports and have lower latency.
/// Returns the number of multiplies strength-reduced.
[[gnu::hot]] static unsigned integerStrengthReduce(llvm::Function& func,
                                       const MicroarchProfile& profile) {
    // Only profitable when we have more ALU ports than multiply units.
    // On modern x86/ARM the integer multiplier is a single port with latency 3;
    // a shift+add sequence uses latency 1+1=2 and two ALU ports in parallel.
    if (profile.intALUs < 2) return 0;

    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Mul) continue;
            if (!inst.getType()->isIntegerTy()) continue;

            // Identify (x, constant) — try both orderings.
            llvm::Value* xv = nullptr;
            int64_t cv = 0;
            for (int s = 0; s < 2; ++s) {
                auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(s));
                if (ci && ci->getBitWidth() <= 64) {
                    cv = ci->getSExtValue();
                    xv = inst.getOperand(1 - s);
                    break;
                }
            }
            if (!xv || cv == 0) continue;

            llvm::IRBuilder<> builder(&inst);
            llvm::Type* ty = inst.getType();
            auto mk = [&](int64_t v) { return llvm::ConstantInt::get(ty, v); };
            auto shl = [&](llvm::Value* v, int64_t sh) {
                return builder.CreateShl(v, mk(sh), "sr_shl");
            };

            llvm::Value* rep = nullptr;
            switch (cv) {
            // 2-instruction sequences
            case  3: rep = builder.CreateAdd(shl(xv,1), xv, "sr_mul3"); break;
            case  5: rep = builder.CreateAdd(shl(xv,2), xv, "sr_mul5"); break;
            case  6: rep = builder.CreateAdd(shl(xv,2), shl(xv,1), "sr_mul6"); break;
            case  7: rep = builder.CreateSub(shl(xv,3), xv, "sr_mul7"); break;
            case  9: rep = builder.CreateAdd(shl(xv,3), xv, "sr_mul9"); break;
            case 10: rep = builder.CreateAdd(shl(xv,3), shl(xv,1), "sr_mul10"); break;
            case 12: rep = builder.CreateAdd(shl(xv,3), shl(xv,2), "sr_mul12"); break;
            case 15: rep = builder.CreateSub(shl(xv,4), xv, "sr_mul15"); break;
            case 17: rep = builder.CreateAdd(shl(xv,4), xv, "sr_mul17"); break;
            case 18: rep = builder.CreateAdd(shl(xv,4), shl(xv,1), "sr_mul18"); break;
            case 20: rep = builder.CreateAdd(shl(xv,4), shl(xv,2), "sr_mul20"); break;
            case 24: rep = builder.CreateAdd(shl(xv,4), shl(xv,3), "sr_mul24"); break;
            case 31: rep = builder.CreateSub(shl(xv,5), xv, "sr_mul31"); break;
            case 33: rep = builder.CreateAdd(shl(xv,5), xv, "sr_mul33"); break;
            case 48: rep = builder.CreateAdd(shl(xv,5), shl(xv,4), "sr_mul48"); break;
            case 63: rep = builder.CreateSub(shl(xv,6), xv, "sr_mul63"); break;
            case 65: rep = builder.CreateAdd(shl(xv,6), xv, "sr_mul65"); break;
            case 96: rep = builder.CreateAdd(shl(xv,6), shl(xv,5), "sr_mul96"); break;
            case 127: rep = builder.CreateSub(shl(xv,7), xv, "sr_mul127"); break;
            case 255: rep = builder.CreateSub(shl(xv,8), xv, "sr_mul255"); break;
            // 3-instruction sequences
            case 11: rep = builder.CreateAdd(builder.CreateAdd(shl(xv,3), shl(xv,1), "t"), xv, "sr_mul11"); break;
            case 13: rep = builder.CreateAdd(builder.CreateAdd(shl(xv,3), shl(xv,2), "t"), xv, "sr_mul13"); break;
            case 14: rep = builder.CreateSub(shl(xv,4), shl(xv,1), "sr_mul14"); break;
            case 19: rep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,1), "t"), xv, "sr_mul19"); break;
            case 21: rep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,2), "t"), xv, "sr_mul21"); break;
            case 22: rep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,2), "t"), shl(xv,1), "sr_mul22"); break;
            case 25: rep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,3), "t"), xv, "sr_mul25"); break;
            case 26: rep = builder.CreateSub(builder.CreateSub(shl(xv,5), shl(xv,2), "t"), shl(xv,1), "sr_mul26"); break;
            case 27: rep = builder.CreateSub(builder.CreateSub(shl(xv,5), shl(xv,2), "t"), xv, "sr_mul27"); break;
            case 28: rep = builder.CreateSub(shl(xv,5), shl(xv,2), "sr_mul28"); break;
            case 30: rep = builder.CreateSub(shl(xv,5), shl(xv,1), "sr_mul30"); break;
            case 34: rep = builder.CreateAdd(shl(xv,5), shl(xv,1), "sr_mul34"); break;
            case 36: rep = builder.CreateAdd(shl(xv,5), shl(xv,2), "sr_mul36"); break;
            case 37: rep = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,2), "t"), xv, "sr_mul37"); break;
            case 40: rep = builder.CreateAdd(shl(xv,5), shl(xv,3), "sr_mul40"); break;
            case 41: rep = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,3), "t"), xv, "sr_mul41"); break;
            case 49: rep = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,4), "t"), xv, "sr_mul49"); break;
            case 50: rep = builder.CreateAdd(builder.CreateSub(shl(xv,6), shl(xv,4), "t"), shl(xv,1), "sr_mul50"); break;
            case 60: rep = builder.CreateSub(shl(xv,6), shl(xv,2), "sr_mul60"); break;
            case 100: {
                // n*100 → (n<<7) - (n<<5) + (n<<2)  [128n - 32n + 4n]
                auto* t1 = builder.CreateSub(shl(xv,7), shl(xv,5), "sr_mul100.t");
                rep = builder.CreateAdd(t1, shl(xv,2), "sr_mul100");
                break;
            }
            case 120: rep = builder.CreateSub(shl(xv,7), shl(xv,3), "sr_mul120"); break;
            case 200: {
                // n*200 → (n<<8) - (n<<6) + (n<<3)  [256n - 64n + 8n]
                auto* t1 = builder.CreateSub(shl(xv,8), shl(xv,6), "sr_mul200.t");
                rep = builder.CreateAdd(t1, shl(xv,3), "sr_mul200");
                break;
            }
            // ── Extended multiply-by-constant patterns (2-instruction) ─────────
            case  56: rep = builder.CreateSub(shl(xv,6), shl(xv,3), "sr_mul56"); break;
            case  57: {
                // n*57 → (n<<6) - (n<<3) + n  (= 64n - 8n + n)
                auto* t1 = builder.CreateSub(shl(xv,6), shl(xv,3), "sr_mul57.t");
                rep = builder.CreateAdd(t1, xv, "sr_mul57");
                break;
            }
            case  62: rep = builder.CreateSub(shl(xv,6), shl(xv,1), "sr_mul62"); break;
            case  66: rep = builder.CreateAdd(shl(xv,6), shl(xv,1), "sr_mul66"); break;
            case  68: rep = builder.CreateAdd(shl(xv,6), shl(xv,2), "sr_mul68"); break;
            case  72: rep = builder.CreateAdd(shl(xv,6), shl(xv,3), "sr_mul72"); break;
            case  80: rep = builder.CreateAdd(shl(xv,6), shl(xv,4), "sr_mul80"); break;
            case 112: rep = builder.CreateSub(shl(xv,7), shl(xv,4), "sr_mul112"); break;
            case 129: rep = builder.CreateAdd(shl(xv,7), xv,         "sr_mul129"); break;
            case 136: rep = builder.CreateAdd(shl(xv,7), shl(xv,3), "sr_mul136"); break;
            case 144: rep = builder.CreateAdd(shl(xv,7), shl(xv,4), "sr_mul144"); break;
            case 160: rep = builder.CreateAdd(shl(xv,7), shl(xv,5), "sr_mul160"); break;
            case 192: rep = builder.CreateAdd(shl(xv,7), shl(xv,6), "sr_mul192"); break;
            case 224: rep = builder.CreateSub(shl(xv,8), shl(xv,5), "sr_mul224"); break;
            case 240: rep = builder.CreateSub(shl(xv,8), shl(xv,4), "sr_mul240"); break;
            case 248: rep = builder.CreateSub(shl(xv,8), shl(xv,3), "sr_mul248"); break;
            case 257: rep = builder.CreateAdd(shl(xv,8), xv,         "sr_mul257"); break;
            case 264: rep = builder.CreateAdd(shl(xv,8), shl(xv,3), "sr_mul264"); break;
            case 272: rep = builder.CreateAdd(shl(xv,8), shl(xv,4), "sr_mul272"); break;
            case 288: rep = builder.CreateAdd(shl(xv,8), shl(xv,5), "sr_mul288"); break;
            case 320: rep = builder.CreateAdd(shl(xv,8), shl(xv,6), "sr_mul320"); break;
            case 384: rep = builder.CreateAdd(shl(xv,8), shl(xv,7), "sr_mul384"); break;
            case 448: rep = builder.CreateSub(shl(xv,9), shl(xv,6), "sr_mul448"); break;
            case 480: rep = builder.CreateSub(shl(xv,9), shl(xv,5), "sr_mul480"); break;
            case 496: rep = builder.CreateSub(shl(xv,9), shl(xv,4), "sr_mul496"); break;
            case 504: rep = builder.CreateSub(shl(xv,9), shl(xv,3), "sr_mul504"); break;
            case 511: rep = builder.CreateSub(shl(xv,9), xv,         "sr_mul511"); break;
            case 513: rep = builder.CreateAdd(shl(xv,9), xv,         "sr_mul513"); break;
            case 640: rep = builder.CreateAdd(shl(xv,9), shl(xv,7), "sr_mul640"); break;
            case 768: rep = builder.CreateAdd(shl(xv,9), shl(xv,8), "sr_mul768"); break;
            case 1023: rep = builder.CreateSub(shl(xv,10), xv,        "sr_mul1023"); break;
            case 1025: rep = builder.CreateAdd(shl(xv,10), xv,        "sr_mul1025"); break;
            case 1152: rep = builder.CreateAdd(shl(xv,10), shl(xv,7), "sr_mul1152"); break;
            case 1280: rep = builder.CreateAdd(shl(xv,10), shl(xv,8), "sr_mul1280"); break;
            case 1536: rep = builder.CreateAdd(shl(xv,10), shl(xv,9), "sr_mul1536"); break;
            case 1792: rep = builder.CreateSub(shl(xv,11), shl(xv,8), "sr_mul1792"); break;
            case 2047: rep = builder.CreateSub(shl(xv,11), xv,        "sr_mul2047"); break;
            case 2049: rep = builder.CreateAdd(shl(xv,11), xv,        "sr_mul2049"); break;
            // ── n×128 family ──────────────────────────────────────────────────────
            case 124:  rep = builder.CreateSub(shl(xv,7), shl(xv,2),  "sr_mul124");  break;
            case 126:  rep = builder.CreateSub(shl(xv,7), shl(xv,1),  "sr_mul126");  break;
            case 130:  rep = builder.CreateAdd(shl(xv,7), shl(xv,1),  "sr_mul130");  break;
            case 132:  rep = builder.CreateAdd(shl(xv,7), shl(xv,2),  "sr_mul132");  break;
            // ── n×256 family ──────────────────────────────────────────────────────
            case 252:  rep = builder.CreateSub(shl(xv,8), shl(xv,2),  "sr_mul252");  break;
            case 254:  rep = builder.CreateSub(shl(xv,8), shl(xv,1),  "sr_mul254");  break;
            case 258:  rep = builder.CreateAdd(shl(xv,8), shl(xv,1),  "sr_mul258");  break;
            case 260:  rep = builder.CreateAdd(shl(xv,8), shl(xv,2),  "sr_mul260");  break;
            // ── n×512 family ──────────────────────────────────────────────────────
            case 508:  rep = builder.CreateSub(shl(xv,9), shl(xv,2),  "sr_mul508");  break;
            case 510:  rep = builder.CreateSub(shl(xv,9), shl(xv,1),  "sr_mul510");  break;
            case 514:  rep = builder.CreateAdd(shl(xv,9), shl(xv,1),  "sr_mul514");  break;
            case 516:  rep = builder.CreateAdd(shl(xv,9), shl(xv,2),  "sr_mul516");  break;
            case 520:  rep = builder.CreateAdd(shl(xv,9), shl(xv,3),  "sr_mul520");  break;
            case 528:  rep = builder.CreateAdd(shl(xv,9), shl(xv,4),  "sr_mul528");  break;
            case 544:  rep = builder.CreateAdd(shl(xv,9), shl(xv,5),  "sr_mul544");  break;
            case 576:  rep = builder.CreateAdd(shl(xv,9), shl(xv,6),  "sr_mul576");  break;
            // ── n×1024 family ─────────────────────────────────────────────────────
            case 960:  rep = builder.CreateSub(shl(xv,10), shl(xv,6), "sr_mul960");  break;
            case 992:  rep = builder.CreateSub(shl(xv,10), shl(xv,5), "sr_mul992");  break;
            case 1008: rep = builder.CreateSub(shl(xv,10), shl(xv,4), "sr_mul1008"); break;
            case 1016: rep = builder.CreateSub(shl(xv,10), shl(xv,3), "sr_mul1016"); break;
            case 1020: rep = builder.CreateSub(shl(xv,10), shl(xv,2), "sr_mul1020"); break;
            case 1022: rep = builder.CreateSub(shl(xv,10), shl(xv,1), "sr_mul1022"); break;
            case 1026: rep = builder.CreateAdd(shl(xv,10), shl(xv,1), "sr_mul1026"); break;
            case 1028: rep = builder.CreateAdd(shl(xv,10), shl(xv,2), "sr_mul1028"); break;
            case 1032: rep = builder.CreateAdd(shl(xv,10), shl(xv,3), "sr_mul1032"); break;
            case 1040: rep = builder.CreateAdd(shl(xv,10), shl(xv,4), "sr_mul1040"); break;
            case 1056: rep = builder.CreateAdd(shl(xv,10), shl(xv,5), "sr_mul1056"); break;
            case 1088: rep = builder.CreateAdd(shl(xv,10), shl(xv,6), "sr_mul1088"); break;
            // ── n×2048 family ─────────────────────────────────────────────────────
            case 1920: rep = builder.CreateSub(shl(xv,11), shl(xv,7), "sr_mul1920"); break;
            case 1984: rep = builder.CreateSub(shl(xv,11), shl(xv,6), "sr_mul1984"); break;
            case 2016: rep = builder.CreateSub(shl(xv,11), shl(xv,5), "sr_mul2016"); break;
            case 2032: rep = builder.CreateSub(shl(xv,11), shl(xv,4), "sr_mul2032"); break;
            case 2040: rep = builder.CreateSub(shl(xv,11), shl(xv,3), "sr_mul2040"); break;
            case 2044: rep = builder.CreateSub(shl(xv,11), shl(xv,2), "sr_mul2044"); break;
            case 2046: rep = builder.CreateSub(shl(xv,11), shl(xv,1), "sr_mul2046"); break;
            case 2050: rep = builder.CreateAdd(shl(xv,11), shl(xv,1), "sr_mul2050"); break;
            case 2052: rep = builder.CreateAdd(shl(xv,11), shl(xv,2), "sr_mul2052"); break;
            case 2056: rep = builder.CreateAdd(shl(xv,11), shl(xv,3), "sr_mul2056"); break;
            case 2064: rep = builder.CreateAdd(shl(xv,11), shl(xv,4), "sr_mul2064"); break;
            case 2080: rep = builder.CreateAdd(shl(xv,11), shl(xv,5), "sr_mul2080"); break;
            case 2112: rep = builder.CreateAdd(shl(xv,11), shl(xv,6), "sr_mul2112"); break;
            case 2176: rep = builder.CreateAdd(shl(xv,11), shl(xv,7), "sr_mul2176"); break;
            case 2304: rep = builder.CreateAdd(shl(xv,11), shl(xv,8), "sr_mul2304"); break;
            case 2560: rep = builder.CreateAdd(shl(xv,11), shl(xv,9), "sr_mul2560"); break;
            case 3072: rep = builder.CreateAdd(shl(xv,11), shl(xv,10),"sr_mul3072"); break;
            // ── n×4096 family ─────────────────────────────────────────────────────
            case 3584: rep = builder.CreateSub(shl(xv,12), shl(xv,9), "sr_mul3584"); break;
            case 3840: rep = builder.CreateSub(shl(xv,12), shl(xv,8), "sr_mul3840"); break;
            case 3968: rep = builder.CreateSub(shl(xv,12), shl(xv,7), "sr_mul3968"); break;
            case 4032: rep = builder.CreateSub(shl(xv,12), shl(xv,6), "sr_mul4032"); break;
            case 4064: rep = builder.CreateSub(shl(xv,12), shl(xv,5), "sr_mul4064"); break;
            case 4080: rep = builder.CreateSub(shl(xv,12), shl(xv,4), "sr_mul4080"); break;
            case 4088: rep = builder.CreateSub(shl(xv,12), shl(xv,3), "sr_mul4088"); break;
            case 4092: rep = builder.CreateSub(shl(xv,12), shl(xv,2), "sr_mul4092"); break;
            case 4094: rep = builder.CreateSub(shl(xv,12), shl(xv,1), "sr_mul4094"); break;
            case 4095: rep = builder.CreateSub(shl(xv,12), xv,         "sr_mul4095"); break;
            case 4097: rep = builder.CreateAdd(shl(xv,12), xv,         "sr_mul4097"); break;
            case 4098: rep = builder.CreateAdd(shl(xv,12), shl(xv,1),  "sr_mul4098"); break;
            case 4100: rep = builder.CreateAdd(shl(xv,12), shl(xv,2),  "sr_mul4100"); break;
            case 4104: rep = builder.CreateAdd(shl(xv,12), shl(xv,3),  "sr_mul4104"); break;
            case 4112: rep = builder.CreateAdd(shl(xv,12), shl(xv,4),  "sr_mul4112"); break;
            case 4128: rep = builder.CreateAdd(shl(xv,12), shl(xv,5),  "sr_mul4128"); break;
            case 4160: rep = builder.CreateAdd(shl(xv,12), shl(xv,6),  "sr_mul4160"); break;
            case 4224: rep = builder.CreateAdd(shl(xv,12), shl(xv,7),  "sr_mul4224"); break;
            case 4352: rep = builder.CreateAdd(shl(xv,12), shl(xv,8),  "sr_mul4352"); break;
            case 4608: rep = builder.CreateAdd(shl(xv,12), shl(xv,9),  "sr_mul4608"); break;
            case 5120: rep = builder.CreateAdd(shl(xv,12), shl(xv,10), "sr_mul5120"); break;
            case 6144: rep = builder.CreateAdd(shl(xv,12), shl(xv,11), "sr_mul6144"); break;
            // ── n×8192 family ─────────────────────────────────────────────────
            case 7168:  rep = builder.CreateSub(shl(xv,13), shl(xv,10), "sr_mul7168");  break;
            case 7680:  rep = builder.CreateSub(shl(xv,13), shl(xv,9),  "sr_mul7680");  break;
            case 7936:  rep = builder.CreateSub(shl(xv,13), shl(xv,8),  "sr_mul7936");  break;
            case 8064:  rep = builder.CreateSub(shl(xv,13), shl(xv,7),  "sr_mul8064");  break;
            case 8128:  rep = builder.CreateSub(shl(xv,13), shl(xv,6),  "sr_mul8128");  break;
            case 8160:  rep = builder.CreateSub(shl(xv,13), shl(xv,5),  "sr_mul8160");  break;
            case 8176:  rep = builder.CreateSub(shl(xv,13), shl(xv,4),  "sr_mul8176");  break;
            case 8184:  rep = builder.CreateSub(shl(xv,13), shl(xv,3),  "sr_mul8184");  break;
            case 8188:  rep = builder.CreateSub(shl(xv,13), shl(xv,2),  "sr_mul8188");  break;
            case 8190:  rep = builder.CreateSub(shl(xv,13), shl(xv,1),  "sr_mul8190");  break;
            case 8191:  rep = builder.CreateSub(shl(xv,13), xv,          "sr_mul8191");  break;
            case 8193:  rep = builder.CreateAdd(shl(xv,13), xv,          "sr_mul8193");  break;
            case 8194:  rep = builder.CreateAdd(shl(xv,13), shl(xv,1),  "sr_mul8194");  break;
            case 8196:  rep = builder.CreateAdd(shl(xv,13), shl(xv,2),  "sr_mul8196");  break;
            case 8200:  rep = builder.CreateAdd(shl(xv,13), shl(xv,3),  "sr_mul8200");  break;
            case 8208:  rep = builder.CreateAdd(shl(xv,13), shl(xv,4),  "sr_mul8208");  break;
            case 8224:  rep = builder.CreateAdd(shl(xv,13), shl(xv,5),  "sr_mul8224");  break;
            case 8256:  rep = builder.CreateAdd(shl(xv,13), shl(xv,6),  "sr_mul8256");  break;
            case 8320:  rep = builder.CreateAdd(shl(xv,13), shl(xv,7),  "sr_mul8320");  break;
            case 8448:  rep = builder.CreateAdd(shl(xv,13), shl(xv,8),  "sr_mul8448");  break;
            case 8704:  rep = builder.CreateAdd(shl(xv,13), shl(xv,9),  "sr_mul8704");  break;
            case 9216:  rep = builder.CreateAdd(shl(xv,13), shl(xv,10), "sr_mul9216");  break;
            case 10240: rep = builder.CreateAdd(shl(xv,13), shl(xv,11), "sr_mul10240"); break;
            case 12288: rep = builder.CreateAdd(shl(xv,13), shl(xv,12), "sr_mul12288"); break;
            // ── n×16384 family ──────────────────────────────────────────────────────
            case 14336: rep = builder.CreateSub(shl(xv,14), shl(xv,11), "sr_mul14336"); break;
            case 15360: rep = builder.CreateSub(shl(xv,14), shl(xv,10), "sr_mul15360"); break;
            case 16384: rep = shl(xv,14); break;
            case 16385: rep = builder.CreateAdd(shl(xv,14), xv,          "sr_mul16385"); break;
            case 16386: rep = builder.CreateAdd(shl(xv,14), shl(xv,1),   "sr_mul16386"); break;
            case 16388: rep = builder.CreateAdd(shl(xv,14), shl(xv,2),   "sr_mul16388"); break;
            case 16392: rep = builder.CreateAdd(shl(xv,14), shl(xv,3),   "sr_mul16392"); break;
            case 16400: rep = builder.CreateAdd(shl(xv,14), shl(xv,4),   "sr_mul16400"); break;
            case 16416: rep = builder.CreateAdd(shl(xv,14), shl(xv,5),   "sr_mul16416"); break;
            case 16448: rep = builder.CreateAdd(shl(xv,14), shl(xv,6),   "sr_mul16448"); break;
            case 16512: rep = builder.CreateAdd(shl(xv,14), shl(xv,7),   "sr_mul16512"); break;
            case 16640: rep = builder.CreateAdd(shl(xv,14), shl(xv,8),   "sr_mul16640"); break;
            case 16896: rep = builder.CreateAdd(shl(xv,14), shl(xv,9),   "sr_mul16896"); break;
            case 17408: rep = builder.CreateAdd(shl(xv,14), shl(xv,10),  "sr_mul17408"); break;
            case 18432: rep = builder.CreateAdd(shl(xv,14), shl(xv,11),  "sr_mul18432"); break;
            case 20480: rep = builder.CreateAdd(shl(xv,14), shl(xv,12),  "sr_mul20480"); break;
            case 24576: rep = builder.CreateAdd(shl(xv,14), shl(xv,13),  "sr_mul24576"); break;
            // ── n×32768 family ──────────────────────────────────────────────────────
            case 28672: rep = builder.CreateSub(shl(xv,15), shl(xv,12), "sr_mul28672"); break;
            case 30720: rep = builder.CreateSub(shl(xv,15), shl(xv,11), "sr_mul30720"); break;
            case 32768: rep = shl(xv,15); break;
            case 32769: rep = builder.CreateAdd(shl(xv,15), xv,          "sr_mul32769"); break;
            case 32770: rep = builder.CreateAdd(shl(xv,15), shl(xv,1),   "sr_mul32770"); break;
            case 32772: rep = builder.CreateAdd(shl(xv,15), shl(xv,2),   "sr_mul32772"); break;
            case 32776: rep = builder.CreateAdd(shl(xv,15), shl(xv,3),   "sr_mul32776"); break;
            case 32800: rep = builder.CreateAdd(shl(xv,15), shl(xv,5),   "sr_mul32800"); break;
            case 32896: rep = builder.CreateAdd(shl(xv,15), shl(xv,7),   "sr_mul32896"); break;
            case 33024: rep = builder.CreateAdd(shl(xv,15), shl(xv,8),   "sr_mul33024"); break;
            case 33280: rep = builder.CreateAdd(shl(xv,15), shl(xv,9),   "sr_mul33280"); break;
            case 33792: rep = builder.CreateAdd(shl(xv,15), shl(xv,10),  "sr_mul33792"); break;
            case 34816: rep = builder.CreateAdd(shl(xv,15), shl(xv,11),  "sr_mul34816"); break;
            case 36864: rep = builder.CreateAdd(shl(xv,15), shl(xv,12),  "sr_mul36864"); break;
            case 40960: rep = builder.CreateAdd(shl(xv,15), shl(xv,13),  "sr_mul40960"); break;
            case 49152: rep = builder.CreateAdd(shl(xv,15), shl(xv,14),  "sr_mul49152"); break;
            // ── n×65536 family ──────────────────────────────────────────────────────
            case 57344: rep = builder.CreateSub(shl(xv,16), shl(xv,13), "sr_mul57344"); break;
            case 61440: rep = builder.CreateSub(shl(xv,16), shl(xv,12), "sr_mul61440"); break;
            case 65536: rep = shl(xv,16); break;
            case 65537: rep = builder.CreateAdd(shl(xv,16), xv,          "sr_mul65537"); break;
            case 65538: rep = builder.CreateAdd(shl(xv,16), shl(xv,1),   "sr_mul65538"); break;
            case 65540: rep = builder.CreateAdd(shl(xv,16), shl(xv,2),   "sr_mul65540"); break;
            case 65544: rep = builder.CreateAdd(shl(xv,16), shl(xv,3),   "sr_mul65544"); break;
            case 65600: rep = builder.CreateAdd(shl(xv,16), shl(xv,6),   "sr_mul65600"); break;
            case 65664: rep = builder.CreateAdd(shl(xv,16), shl(xv,7),   "sr_mul65664"); break;
            case 65792: rep = builder.CreateAdd(shl(xv,16), shl(xv,8),   "sr_mul65792"); break;
            case 66048: rep = builder.CreateAdd(shl(xv,16), shl(xv,9),   "sr_mul66048"); break;
            case 66560: rep = builder.CreateAdd(shl(xv,16), shl(xv,10),  "sr_mul66560"); break;
            case 67584: rep = builder.CreateAdd(shl(xv,16), shl(xv,11),  "sr_mul67584"); break;
            case 69632: rep = builder.CreateAdd(shl(xv,16), shl(xv,12),  "sr_mul69632"); break;
            case 73728: rep = builder.CreateAdd(shl(xv,16), shl(xv,13),  "sr_mul73728"); break;
            case 81920: rep = builder.CreateAdd(shl(xv,16), shl(xv,14),  "sr_mul81920"); break;
            case 98304: rep = builder.CreateAdd(shl(xv,16), shl(xv,15),  "sr_mul98304"); break;
            case 114688: rep = builder.CreateSub(shl(xv,17), shl(xv,14), "sr_mul114688"); break;
            case 122880: rep = builder.CreateSub(shl(xv,17), shl(xv,13), "sr_mul122880"); break;
            case 131072: rep = shl(xv,17); break;
            case 131073: rep = builder.CreateAdd(shl(xv,17), xv,          "sr_mul131073"); break;
            case 131074: rep = builder.CreateAdd(shl(xv,17), shl(xv,1),   "sr_mul131074"); break;
            case 131076: rep = builder.CreateAdd(shl(xv,17), shl(xv,2),   "sr_mul131076"); break;
            case 131080: rep = builder.CreateAdd(shl(xv,17), shl(xv,3),   "sr_mul131080"); break;
            case 131136: rep = builder.CreateAdd(shl(xv,17), shl(xv,6),   "sr_mul131136"); break;
            case 131200: rep = builder.CreateAdd(shl(xv,17), shl(xv,7),   "sr_mul131200"); break;
            case 131328: rep = builder.CreateAdd(shl(xv,17), shl(xv,8),   "sr_mul131328"); break;
            case 131584: rep = builder.CreateAdd(shl(xv,17), shl(xv,9),   "sr_mul131584"); break;
            case 132096: rep = builder.CreateAdd(shl(xv,17), shl(xv,10),  "sr_mul132096"); break;
            case 133120: rep = builder.CreateAdd(shl(xv,17), shl(xv,11),  "sr_mul133120"); break;
            case 135168: rep = builder.CreateAdd(shl(xv,17), shl(xv,12),  "sr_mul135168"); break;
            case 139264: rep = builder.CreateAdd(shl(xv,17), shl(xv,13),  "sr_mul139264"); break;
            case 147456: rep = builder.CreateAdd(shl(xv,17), shl(xv,14),  "sr_mul147456"); break;
            case 163840: rep = builder.CreateAdd(shl(xv,17), shl(xv,15),  "sr_mul163840"); break;
            case 196608: rep = builder.CreateAdd(shl(xv,17), shl(xv,16),  "sr_mul196608"); break;
            case 229376: rep = builder.CreateSub(shl(xv,18), shl(xv,15),  "sr_mul229376"); break;
            case 245760: rep = builder.CreateSub(shl(xv,18), shl(xv,14),  "sr_mul245760"); break;
            case 262144: rep = shl(xv,18); break;
            case 262152: rep = builder.CreateAdd(shl(xv,18), shl(xv,3),   "sr_mul262152"); break;
            case 262160: rep = builder.CreateAdd(shl(xv,18), shl(xv,4),   "sr_mul262160"); break;
            case 262176: rep = builder.CreateAdd(shl(xv,18), shl(xv,5),   "sr_mul262176"); break;
            case 262208: rep = builder.CreateAdd(shl(xv,18), shl(xv,6),   "sr_mul262208"); break;
            case 262272: rep = builder.CreateAdd(shl(xv,18), shl(xv,7),   "sr_mul262272"); break;
            case 262400: rep = builder.CreateAdd(shl(xv,18), shl(xv,8),   "sr_mul262400"); break;
            case 262656: rep = builder.CreateAdd(shl(xv,18), shl(xv,9),   "sr_mul262656"); break;
            case 263168: rep = builder.CreateAdd(shl(xv,18), shl(xv,10),  "sr_mul263168"); break;
            case 264192: rep = builder.CreateAdd(shl(xv,18), shl(xv,11),  "sr_mul264192"); break;
            case 266240: rep = builder.CreateAdd(shl(xv,18), shl(xv,12),  "sr_mul266240"); break;
            case 270336: rep = builder.CreateAdd(shl(xv,18), shl(xv,13),  "sr_mul270336"); break;
            case 278528: rep = builder.CreateAdd(shl(xv,18), shl(xv,14),  "sr_mul278528"); break;
            case 294912: rep = builder.CreateAdd(shl(xv,18), shl(xv,15),  "sr_mul294912"); break;
            case 327680: rep = builder.CreateAdd(shl(xv,18), shl(xv,16),  "sr_mul327680"); break;
            case 393216: rep = builder.CreateAdd(shl(xv,18), shl(xv,17),  "sr_mul393216"); break;
            // ── Extended multiply-by-constant patterns (3-instruction) ─────────
            case 1000: {
                // n*1000 → (n<<10) - (n<<5) + (n<<3)  [1024n - 32n + 8n]
                auto* t1 = builder.CreateSub(shl(xv,10), shl(xv,5), "sr_mul1000.t");
                rep = builder.CreateAdd(t1, shl(xv,3), "sr_mul1000");
                break;
            }
            default: break;
            }

            // Negative constant: compute |cv|'s strength-reduced form, then negate.
            // e.g. x * -7 → -(x * 7) → -((x<<3) - x)
            if (!rep && cv < -1) {
                int64_t absCV = -cv;
                llvm::Value* posRep = nullptr;
                switch (absCV) {
                // 2-instruction positive sequences (shift + add/sub)
                case  3: posRep = builder.CreateAdd(shl(xv,1), xv, "sr_mul3"); break;
                case  5: posRep = builder.CreateAdd(shl(xv,2), xv, "sr_mul5"); break;
                case  6: posRep = builder.CreateAdd(shl(xv,2), shl(xv,1), "sr_mul6"); break;
                case  7: posRep = builder.CreateSub(shl(xv,3), xv, "sr_mul7"); break;
                case  9: posRep = builder.CreateAdd(shl(xv,3), xv, "sr_mul9"); break;
                case 10: posRep = builder.CreateAdd(shl(xv,3), shl(xv,1), "sr_mul10"); break;
                case 12: posRep = builder.CreateAdd(shl(xv,3), shl(xv,2), "sr_mul12"); break;
                case 15: posRep = builder.CreateSub(shl(xv,4), xv, "sr_mul15"); break;
                case 17: posRep = builder.CreateAdd(shl(xv,4), xv, "sr_mul17"); break;
                case 18: posRep = builder.CreateAdd(shl(xv,4), shl(xv,1), "sr_mul18"); break;
                case 20: posRep = builder.CreateAdd(shl(xv,4), shl(xv,2), "sr_mul20"); break;
                case 24: posRep = builder.CreateAdd(shl(xv,4), shl(xv,3), "sr_mul24"); break;
                case 28: posRep = builder.CreateSub(shl(xv,5), shl(xv,2), "sr_mul28"); break;
                case 30: posRep = builder.CreateSub(shl(xv,5), shl(xv,1), "sr_mul30"); break;
                case 31: posRep = builder.CreateSub(shl(xv,5), xv, "sr_mul31"); break;
                case 33: posRep = builder.CreateAdd(shl(xv,5), xv, "sr_mul33"); break;
                case 34: posRep = builder.CreateAdd(shl(xv,5), shl(xv,1), "sr_mul34"); break;
                case 36: posRep = builder.CreateAdd(shl(xv,5), shl(xv,2), "sr_mul36"); break;
                case 40: posRep = builder.CreateAdd(shl(xv,5), shl(xv,3), "sr_mul40"); break;
                case 48: posRep = builder.CreateAdd(shl(xv,5), shl(xv,4), "sr_mul48"); break;
                case 60: posRep = builder.CreateSub(shl(xv,6), shl(xv,2), "sr_mul60"); break;
                case 63: posRep = builder.CreateSub(shl(xv,6), xv, "sr_mul63"); break;
                case 65: posRep = builder.CreateAdd(shl(xv,6), xv, "sr_mul65"); break;
                case 96: posRep = builder.CreateAdd(shl(xv,6), shl(xv,5), "sr_mul96"); break;
                case 120: posRep = builder.CreateSub(shl(xv,7), shl(xv,3), "sr_mul120"); break;
                case 127: posRep = builder.CreateSub(shl(xv,7), xv, "sr_mul127"); break;
                case 255: posRep = builder.CreateSub(shl(xv,8), xv, "sr_mul255"); break;
                // ── Extended 2-instruction negative sequences ──────────────────
                case  56: posRep = builder.CreateSub(shl(xv,6), shl(xv,3), "sr_mulp56"); break;
                case  57: {
                    // n*57 → (n<<6) - (n<<3) + n  (= 64n - 8n + n)
                    auto* t1 = builder.CreateSub(shl(xv,6), shl(xv,3), "sr_mulp57.t");
                    posRep = builder.CreateAdd(t1, xv, "sr_mulp57");
                    break;
                }
                case  62: posRep = builder.CreateSub(shl(xv,6), shl(xv,1), "sr_mulp62"); break;
                case  66: posRep = builder.CreateAdd(shl(xv,6), shl(xv,1), "sr_mulp66"); break;
                case  68: posRep = builder.CreateAdd(shl(xv,6), shl(xv,2), "sr_mulp68"); break;
                case  72: posRep = builder.CreateAdd(shl(xv,6), shl(xv,3), "sr_mulp72"); break;
                case  80: posRep = builder.CreateAdd(shl(xv,6), shl(xv,4), "sr_mulp80"); break;
                case 112: posRep = builder.CreateSub(shl(xv,7), shl(xv,4), "sr_mulp112"); break;
                case 129: posRep = builder.CreateAdd(shl(xv,7), xv,         "sr_mulp129"); break;
                case 136: posRep = builder.CreateAdd(shl(xv,7), shl(xv,3), "sr_mulp136"); break;
                case 144: posRep = builder.CreateAdd(shl(xv,7), shl(xv,4), "sr_mulp144"); break;
                case 160: posRep = builder.CreateAdd(shl(xv,7), shl(xv,5), "sr_mulp160"); break;
                case 192: posRep = builder.CreateAdd(shl(xv,7), shl(xv,6), "sr_mulp192"); break;
                case 224: posRep = builder.CreateSub(shl(xv,8), shl(xv,5), "sr_mulp224"); break;
                case 240: posRep = builder.CreateSub(shl(xv,8), shl(xv,4), "sr_mulp240"); break;
                case 248: posRep = builder.CreateSub(shl(xv,8), shl(xv,3), "sr_mulp248"); break;
                case 257: posRep = builder.CreateAdd(shl(xv,8), xv,         "sr_mulp257"); break;
                case 264: posRep = builder.CreateAdd(shl(xv,8), shl(xv,3), "sr_mulp264"); break;
                case 272: posRep = builder.CreateAdd(shl(xv,8), shl(xv,4), "sr_mulp272"); break;
                case 288: posRep = builder.CreateAdd(shl(xv,8), shl(xv,5), "sr_mulp288"); break;
                case 320: posRep = builder.CreateAdd(shl(xv,8), shl(xv,6), "sr_mulp320"); break;
                case 384: posRep = builder.CreateAdd(shl(xv,8), shl(xv,7), "sr_mulp384"); break;
                case 448: posRep = builder.CreateSub(shl(xv,9), shl(xv,6), "sr_mulp448"); break;
                case 480: posRep = builder.CreateSub(shl(xv,9), shl(xv,5), "sr_mulp480"); break;
                case 496: posRep = builder.CreateSub(shl(xv,9), shl(xv,4), "sr_mulp496"); break;
                case 504: posRep = builder.CreateSub(shl(xv,9), shl(xv,3), "sr_mulp504"); break;
                case 511: posRep = builder.CreateSub(shl(xv,9), xv,         "sr_mulp511"); break;
                case 513: posRep = builder.CreateAdd(shl(xv,9), xv,         "sr_mulp513"); break;
                case 640: posRep = builder.CreateAdd(shl(xv,9), shl(xv,7), "sr_mulp640"); break;
                case 768: posRep = builder.CreateAdd(shl(xv,9), shl(xv,8), "sr_mulp768"); break;
                case 1023: posRep = builder.CreateSub(shl(xv,10), xv,       "sr_mulp1023"); break;
                case 1025: posRep = builder.CreateAdd(shl(xv,10), xv,       "sr_mulp1025"); break;
                case 1152: posRep = builder.CreateAdd(shl(xv,10), shl(xv,7), "sr_mulp1152"); break;
                case 1280: posRep = builder.CreateAdd(shl(xv,10), shl(xv,8), "sr_mulp1280"); break;
                case 1536: posRep = builder.CreateAdd(shl(xv,10), shl(xv,9), "sr_mulp1536"); break;
                case 1792: posRep = builder.CreateSub(shl(xv,11), shl(xv,8), "sr_mulp1792"); break;
                case 2047: posRep = builder.CreateSub(shl(xv,11), xv,        "sr_mulp2047"); break;
                case 2049: posRep = builder.CreateAdd(shl(xv,11), xv,        "sr_mulp2049"); break;
                // ── n×128 family ───────────────────────────────────────────────
                case 124:  posRep = builder.CreateSub(shl(xv,7), shl(xv,2),  "sr_mulp124");  break;
                case 126:  posRep = builder.CreateSub(shl(xv,7), shl(xv,1),  "sr_mulp126");  break;
                case 130:  posRep = builder.CreateAdd(shl(xv,7), shl(xv,1),  "sr_mulp130");  break;
                case 132:  posRep = builder.CreateAdd(shl(xv,7), shl(xv,2),  "sr_mulp132");  break;
                // ── n×256 family ───────────────────────────────────────────────
                case 252:  posRep = builder.CreateSub(shl(xv,8), shl(xv,2),  "sr_mulp252");  break;
                case 254:  posRep = builder.CreateSub(shl(xv,8), shl(xv,1),  "sr_mulp254");  break;
                case 258:  posRep = builder.CreateAdd(shl(xv,8), shl(xv,1),  "sr_mulp258");  break;
                case 260:  posRep = builder.CreateAdd(shl(xv,8), shl(xv,2),  "sr_mulp260");  break;
                // ── n×512 family ───────────────────────────────────────────────
                case 508:  posRep = builder.CreateSub(shl(xv,9), shl(xv,2),  "sr_mulp508");  break;
                case 510:  posRep = builder.CreateSub(shl(xv,9), shl(xv,1),  "sr_mulp510");  break;
                case 514:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,1),  "sr_mulp514");  break;
                case 516:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,2),  "sr_mulp516");  break;
                case 520:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,3),  "sr_mulp520");  break;
                case 528:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,4),  "sr_mulp528");  break;
                case 544:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,5),  "sr_mulp544");  break;
                case 576:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,6),  "sr_mulp576");  break;
                // ── n×1024 family ──────────────────────────────────────────────
                case 960:  posRep = builder.CreateSub(shl(xv,10), shl(xv,6), "sr_mulp960");  break;
                case 992:  posRep = builder.CreateSub(shl(xv,10), shl(xv,5), "sr_mulp992");  break;
                case 1008: posRep = builder.CreateSub(shl(xv,10), shl(xv,4), "sr_mulp1008"); break;
                case 1016: posRep = builder.CreateSub(shl(xv,10), shl(xv,3), "sr_mulp1016"); break;
                case 1020: posRep = builder.CreateSub(shl(xv,10), shl(xv,2), "sr_mulp1020"); break;
                case 1022: posRep = builder.CreateSub(shl(xv,10), shl(xv,1), "sr_mulp1022"); break;
                case 1026: posRep = builder.CreateAdd(shl(xv,10), shl(xv,1), "sr_mulp1026"); break;
                case 1028: posRep = builder.CreateAdd(shl(xv,10), shl(xv,2), "sr_mulp1028"); break;
                case 1032: posRep = builder.CreateAdd(shl(xv,10), shl(xv,3), "sr_mulp1032"); break;
                case 1040: posRep = builder.CreateAdd(shl(xv,10), shl(xv,4), "sr_mulp1040"); break;
                case 1056: posRep = builder.CreateAdd(shl(xv,10), shl(xv,5), "sr_mulp1056"); break;
                case 1088: posRep = builder.CreateAdd(shl(xv,10), shl(xv,6), "sr_mulp1088"); break;
                // ── n×2048 family ──────────────────────────────────────────────
                case 1920: posRep = builder.CreateSub(shl(xv,11), shl(xv,7), "sr_mulp1920"); break;
                case 1984: posRep = builder.CreateSub(shl(xv,11), shl(xv,6), "sr_mulp1984"); break;
                case 2016: posRep = builder.CreateSub(shl(xv,11), shl(xv,5), "sr_mulp2016"); break;
                case 2032: posRep = builder.CreateSub(shl(xv,11), shl(xv,4), "sr_mulp2032"); break;
                case 2040: posRep = builder.CreateSub(shl(xv,11), shl(xv,3), "sr_mulp2040"); break;
                case 2044: posRep = builder.CreateSub(shl(xv,11), shl(xv,2), "sr_mulp2044"); break;
                case 2046: posRep = builder.CreateSub(shl(xv,11), shl(xv,1), "sr_mulp2046"); break;
                case 2050: posRep = builder.CreateAdd(shl(xv,11), shl(xv,1), "sr_mulp2050"); break;
                case 2052: posRep = builder.CreateAdd(shl(xv,11), shl(xv,2), "sr_mulp2052"); break;
                case 2056: posRep = builder.CreateAdd(shl(xv,11), shl(xv,3), "sr_mulp2056"); break;
                case 2064: posRep = builder.CreateAdd(shl(xv,11), shl(xv,4), "sr_mulp2064"); break;
                case 2080: posRep = builder.CreateAdd(shl(xv,11), shl(xv,5), "sr_mulp2080"); break;
                case 2112: posRep = builder.CreateAdd(shl(xv,11), shl(xv,6), "sr_mulp2112"); break;
                case 2176: posRep = builder.CreateAdd(shl(xv,11), shl(xv,7), "sr_mulp2176"); break;
                case 2304: posRep = builder.CreateAdd(shl(xv,11), shl(xv,8), "sr_mulp2304"); break;
                case 2560: posRep = builder.CreateAdd(shl(xv,11), shl(xv,9), "sr_mulp2560"); break;
                case 3072: posRep = builder.CreateAdd(shl(xv,11), shl(xv,10),"sr_mulp3072"); break;
                // ── n×4096 family ───────────────────────────────────────────────
                case 3584: posRep = builder.CreateSub(shl(xv,12), shl(xv,9),  "sr_mulp3584"); break;
                case 3840: posRep = builder.CreateSub(shl(xv,12), shl(xv,8),  "sr_mulp3840"); break;
                case 3968: posRep = builder.CreateSub(shl(xv,12), shl(xv,7),  "sr_mulp3968"); break;
                case 4032: posRep = builder.CreateSub(shl(xv,12), shl(xv,6),  "sr_mulp4032"); break;
                case 4064: posRep = builder.CreateSub(shl(xv,12), shl(xv,5),  "sr_mulp4064"); break;
                case 4080: posRep = builder.CreateSub(shl(xv,12), shl(xv,4),  "sr_mulp4080"); break;
                case 4088: posRep = builder.CreateSub(shl(xv,12), shl(xv,3),  "sr_mulp4088"); break;
                case 4092: posRep = builder.CreateSub(shl(xv,12), shl(xv,2),  "sr_mulp4092"); break;
                case 4094: posRep = builder.CreateSub(shl(xv,12), shl(xv,1),  "sr_mulp4094"); break;
                case 4095: posRep = builder.CreateSub(shl(xv,12), xv,          "sr_mulp4095"); break;
                case 4097: posRep = builder.CreateAdd(shl(xv,12), xv,          "sr_mulp4097"); break;
                case 4098: posRep = builder.CreateAdd(shl(xv,12), shl(xv,1),   "sr_mulp4098"); break;
                case 4100: posRep = builder.CreateAdd(shl(xv,12), shl(xv,2),   "sr_mulp4100"); break;
                case 4104: posRep = builder.CreateAdd(shl(xv,12), shl(xv,3),   "sr_mulp4104"); break;
                case 4112: posRep = builder.CreateAdd(shl(xv,12), shl(xv,4),   "sr_mulp4112"); break;
                case 4128: posRep = builder.CreateAdd(shl(xv,12), shl(xv,5),   "sr_mulp4128"); break;
                case 4160: posRep = builder.CreateAdd(shl(xv,12), shl(xv,6),   "sr_mulp4160"); break;
                case 4224: posRep = builder.CreateAdd(shl(xv,12), shl(xv,7),   "sr_mulp4224"); break;
                case 4352: posRep = builder.CreateAdd(shl(xv,12), shl(xv,8),   "sr_mulp4352"); break;
                case 4608: posRep = builder.CreateAdd(shl(xv,12), shl(xv,9),   "sr_mulp4608"); break;
                case 5120: posRep = builder.CreateAdd(shl(xv,12), shl(xv,10),  "sr_mulp5120"); break;
                case 6144: posRep = builder.CreateAdd(shl(xv,12), shl(xv,11),  "sr_mulp6144"); break;
                // ── n×8192 family ──────────────────────────────────────────────
                case 7168:  posRep = builder.CreateSub(shl(xv,13), shl(xv,10), "sr_mulp7168");  break;
                case 7680:  posRep = builder.CreateSub(shl(xv,13), shl(xv,9),  "sr_mulp7680");  break;
                case 7936:  posRep = builder.CreateSub(shl(xv,13), shl(xv,8),  "sr_mulp7936");  break;
                case 8064:  posRep = builder.CreateSub(shl(xv,13), shl(xv,7),  "sr_mulp8064");  break;
                case 8128:  posRep = builder.CreateSub(shl(xv,13), shl(xv,6),  "sr_mulp8128");  break;
                case 8160:  posRep = builder.CreateSub(shl(xv,13), shl(xv,5),  "sr_mulp8160");  break;
                case 8176:  posRep = builder.CreateSub(shl(xv,13), shl(xv,4),  "sr_mulp8176");  break;
                case 8184:  posRep = builder.CreateSub(shl(xv,13), shl(xv,3),  "sr_mulp8184");  break;
                case 8188:  posRep = builder.CreateSub(shl(xv,13), shl(xv,2),  "sr_mulp8188");  break;
                case 8190:  posRep = builder.CreateSub(shl(xv,13), shl(xv,1),  "sr_mulp8190");  break;
                case 8191:  posRep = builder.CreateSub(shl(xv,13), xv,          "sr_mulp8191");  break;
                case 8193:  posRep = builder.CreateAdd(shl(xv,13), xv,          "sr_mulp8193");  break;
                case 8194:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,1),  "sr_mulp8194");  break;
                case 8196:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,2),  "sr_mulp8196");  break;
                case 8200:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,3),  "sr_mulp8200");  break;
                case 8208:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,4),  "sr_mulp8208");  break;
                case 8224:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,5),  "sr_mulp8224");  break;
                case 8256:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,6),  "sr_mulp8256");  break;
                case 8320:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,7),  "sr_mulp8320");  break;
                case 8448:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,8),  "sr_mulp8448");  break;
                case 8704:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,9),  "sr_mulp8704");  break;
                case 9216:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,10), "sr_mulp9216");  break;
                case 10240: posRep = builder.CreateAdd(shl(xv,13), shl(xv,11), "sr_mulp10240"); break;
                case 12288: posRep = builder.CreateAdd(shl(xv,13), shl(xv,12), "sr_mulp12288"); break;
                // ── n×16384 family ──────────────────────────────────────────────
                case 14336: posRep = builder.CreateSub(shl(xv,14), shl(xv,11), "sr_mulp14336"); break;
                case 15360: posRep = builder.CreateSub(shl(xv,14), shl(xv,10), "sr_mulp15360"); break;
                case 16384: posRep = shl(xv,14); break;
                case 16385: posRep = builder.CreateAdd(shl(xv,14), xv,          "sr_mulp16385"); break;
                case 16386: posRep = builder.CreateAdd(shl(xv,14), shl(xv,1),   "sr_mulp16386"); break;
                case 16388: posRep = builder.CreateAdd(shl(xv,14), shl(xv,2),   "sr_mulp16388"); break;
                case 16392: posRep = builder.CreateAdd(shl(xv,14), shl(xv,3),   "sr_mulp16392"); break;
                case 16400: posRep = builder.CreateAdd(shl(xv,14), shl(xv,4),   "sr_mulp16400"); break;
                case 16416: posRep = builder.CreateAdd(shl(xv,14), shl(xv,5),   "sr_mulp16416"); break;
                case 16448: posRep = builder.CreateAdd(shl(xv,14), shl(xv,6),   "sr_mulp16448"); break;
                case 16512: posRep = builder.CreateAdd(shl(xv,14), shl(xv,7),   "sr_mulp16512"); break;
                case 16640: posRep = builder.CreateAdd(shl(xv,14), shl(xv,8),   "sr_mulp16640"); break;
                case 16896: posRep = builder.CreateAdd(shl(xv,14), shl(xv,9),   "sr_mulp16896"); break;
                case 17408: posRep = builder.CreateAdd(shl(xv,14), shl(xv,10),  "sr_mulp17408"); break;
                case 18432: posRep = builder.CreateAdd(shl(xv,14), shl(xv,11),  "sr_mulp18432"); break;
                case 20480: posRep = builder.CreateAdd(shl(xv,14), shl(xv,12),  "sr_mulp20480"); break;
                case 24576: posRep = builder.CreateAdd(shl(xv,14), shl(xv,13),  "sr_mulp24576"); break;
                // ── n×32768 family ──────────────────────────────────────────────
                case 28672: posRep = builder.CreateSub(shl(xv,15), shl(xv,12), "sr_mulp28672"); break;
                case 30720: posRep = builder.CreateSub(shl(xv,15), shl(xv,11), "sr_mulp30720"); break;
                case 32768: posRep = shl(xv,15); break;
                case 32769: posRep = builder.CreateAdd(shl(xv,15), xv,          "sr_mulp32769"); break;
                case 32770: posRep = builder.CreateAdd(shl(xv,15), shl(xv,1),   "sr_mulp32770"); break;
                case 32772: posRep = builder.CreateAdd(shl(xv,15), shl(xv,2),   "sr_mulp32772"); break;
                case 32776: posRep = builder.CreateAdd(shl(xv,15), shl(xv,3),   "sr_mulp32776"); break;
                case 32800: posRep = builder.CreateAdd(shl(xv,15), shl(xv,5),   "sr_mulp32800"); break;
                case 32896: posRep = builder.CreateAdd(shl(xv,15), shl(xv,7),   "sr_mulp32896"); break;
                case 33024: posRep = builder.CreateAdd(shl(xv,15), shl(xv,8),   "sr_mulp33024"); break;
                case 33280: posRep = builder.CreateAdd(shl(xv,15), shl(xv,9),   "sr_mulp33280"); break;
                case 33792: posRep = builder.CreateAdd(shl(xv,15), shl(xv,10),  "sr_mulp33792"); break;
                case 34816: posRep = builder.CreateAdd(shl(xv,15), shl(xv,11),  "sr_mulp34816"); break;
                case 36864: posRep = builder.CreateAdd(shl(xv,15), shl(xv,12),  "sr_mulp36864"); break;
                case 40960: posRep = builder.CreateAdd(shl(xv,15), shl(xv,13),  "sr_mulp40960"); break;
                case 49152: posRep = builder.CreateAdd(shl(xv,15), shl(xv,14),  "sr_mulp49152"); break;
                // ── n×65536 family ──────────────────────────────────────────────
                case 57344: posRep = builder.CreateSub(shl(xv,16), shl(xv,13), "sr_mulp57344"); break;
                case 61440: posRep = builder.CreateSub(shl(xv,16), shl(xv,12), "sr_mulp61440"); break;
                case 65536: posRep = shl(xv,16); break;
                case 65537: posRep = builder.CreateAdd(shl(xv,16), xv,          "sr_mulp65537"); break;
                case 65538: posRep = builder.CreateAdd(shl(xv,16), shl(xv,1),   "sr_mulp65538"); break;
                case 65540: posRep = builder.CreateAdd(shl(xv,16), shl(xv,2),   "sr_mulp65540"); break;
                case 65544: posRep = builder.CreateAdd(shl(xv,16), shl(xv,3),   "sr_mulp65544"); break;
                case 65600: posRep = builder.CreateAdd(shl(xv,16), shl(xv,6),   "sr_mulp65600"); break;
                case 65664: posRep = builder.CreateAdd(shl(xv,16), shl(xv,7),   "sr_mulp65664"); break;
                case 65792: posRep = builder.CreateAdd(shl(xv,16), shl(xv,8),   "sr_mulp65792"); break;
                case 66048: posRep = builder.CreateAdd(shl(xv,16), shl(xv,9),   "sr_mulp66048"); break;
                case 66560: posRep = builder.CreateAdd(shl(xv,16), shl(xv,10),  "sr_mulp66560"); break;
                case 67584: posRep = builder.CreateAdd(shl(xv,16), shl(xv,11),  "sr_mulp67584"); break;
                case 69632: posRep = builder.CreateAdd(shl(xv,16), shl(xv,12),  "sr_mulp69632"); break;
                case 73728: posRep = builder.CreateAdd(shl(xv,16), shl(xv,13),  "sr_mulp73728"); break;
                case 81920: posRep = builder.CreateAdd(shl(xv,16), shl(xv,14),  "sr_mulp81920"); break;
                case 98304: posRep = builder.CreateAdd(shl(xv,16), shl(xv,15),  "sr_mulp98304"); break;
                case 114688: posRep = builder.CreateSub(shl(xv,17), shl(xv,14), "sr_mulp114688"); break;
                case 122880: posRep = builder.CreateSub(shl(xv,17), shl(xv,13), "sr_mulp122880"); break;
                case 131072: posRep = shl(xv,17); break;
                case 131073: posRep = builder.CreateAdd(shl(xv,17), xv,          "sr_mulp131073"); break;
                case 131074: posRep = builder.CreateAdd(shl(xv,17), shl(xv,1),   "sr_mulp131074"); break;
                case 131076: posRep = builder.CreateAdd(shl(xv,17), shl(xv,2),   "sr_mulp131076"); break;
                case 131080: posRep = builder.CreateAdd(shl(xv,17), shl(xv,3),   "sr_mulp131080"); break;
                case 131136: posRep = builder.CreateAdd(shl(xv,17), shl(xv,6),   "sr_mulp131136"); break;
                case 131200: posRep = builder.CreateAdd(shl(xv,17), shl(xv,7),   "sr_mulp131200"); break;
                case 131328: posRep = builder.CreateAdd(shl(xv,17), shl(xv,8),   "sr_mulp131328"); break;
                case 131584: posRep = builder.CreateAdd(shl(xv,17), shl(xv,9),   "sr_mulp131584"); break;
                case 132096: posRep = builder.CreateAdd(shl(xv,17), shl(xv,10),  "sr_mulp132096"); break;
                case 133120: posRep = builder.CreateAdd(shl(xv,17), shl(xv,11),  "sr_mulp133120"); break;
                case 135168: posRep = builder.CreateAdd(shl(xv,17), shl(xv,12),  "sr_mulp135168"); break;
                case 139264: posRep = builder.CreateAdd(shl(xv,17), shl(xv,13),  "sr_mulp139264"); break;
                case 147456: posRep = builder.CreateAdd(shl(xv,17), shl(xv,14),  "sr_mulp147456"); break;
                case 163840: posRep = builder.CreateAdd(shl(xv,17), shl(xv,15),  "sr_mulp163840"); break;
                case 196608: posRep = builder.CreateAdd(shl(xv,17), shl(xv,16),  "sr_mulp196608"); break;
                case 229376: posRep = builder.CreateSub(shl(xv,18), shl(xv,15),  "sr_mulp229376"); break;
                case 245760: posRep = builder.CreateSub(shl(xv,18), shl(xv,14),  "sr_mulp245760"); break;
                case 262144: posRep = shl(xv,18); break;
                case 262152: posRep = builder.CreateAdd(shl(xv,18), shl(xv,3),   "sr_mulp262152"); break;
                case 262160: posRep = builder.CreateAdd(shl(xv,18), shl(xv,4),   "sr_mulp262160"); break;
                case 262176: posRep = builder.CreateAdd(shl(xv,18), shl(xv,5),   "sr_mulp262176"); break;
                case 262208: posRep = builder.CreateAdd(shl(xv,18), shl(xv,6),   "sr_mulp262208"); break;
                case 262272: posRep = builder.CreateAdd(shl(xv,18), shl(xv,7),   "sr_mulp262272"); break;
                case 262400: posRep = builder.CreateAdd(shl(xv,18), shl(xv,8),   "sr_mulp262400"); break;
                case 262656: posRep = builder.CreateAdd(shl(xv,18), shl(xv,9),   "sr_mulp262656"); break;
                case 263168: posRep = builder.CreateAdd(shl(xv,18), shl(xv,10),  "sr_mulp263168"); break;
                case 264192: posRep = builder.CreateAdd(shl(xv,18), shl(xv,11),  "sr_mulp264192"); break;
                case 266240: posRep = builder.CreateAdd(shl(xv,18), shl(xv,12),  "sr_mulp266240"); break;
                case 270336: posRep = builder.CreateAdd(shl(xv,18), shl(xv,13),  "sr_mulp270336"); break;
                case 278528: posRep = builder.CreateAdd(shl(xv,18), shl(xv,14),  "sr_mulp278528"); break;
                case 294912: posRep = builder.CreateAdd(shl(xv,18), shl(xv,15),  "sr_mulp294912"); break;
                case 327680: posRep = builder.CreateAdd(shl(xv,18), shl(xv,16),  "sr_mulp327680"); break;
                case 393216: posRep = builder.CreateAdd(shl(xv,18), shl(xv,17),  "sr_mulp393216"); break;
                // 3-instruction positive sequences
                case 11: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,3), shl(xv,1), "t"), xv, "sr_mul11"); break;
                case 13: { auto* t = builder.CreateSub(shl(xv,4), shl(xv,1), "t"); posRep = builder.CreateSub(t, xv, "sr_mul13"); break; }
                case 14: posRep = builder.CreateSub(shl(xv,4), shl(xv,1), "sr_mul14"); break;
                case 19: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,1), "t"), xv, "sr_mul19"); break;
                case 21: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,2), "t"), xv, "sr_mul21"); break;
                case 22: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,2), "t"), shl(xv,1), "sr_mul22"); break;
                case 25: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,3), "t"), xv, "sr_mul25"); break;
                case 26: posRep = builder.CreateSub(builder.CreateSub(shl(xv,5), shl(xv,2), "t"), shl(xv,1), "sr_mul26"); break;
                case 27: posRep = builder.CreateSub(builder.CreateSub(shl(xv,5), shl(xv,2), "t"), xv, "sr_mul27"); break;
                case 37: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,2), "t"), xv, "sr_mul37"); break;
                case 41: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,3), "t"), xv, "sr_mul41"); break;
                case 49: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,4), "t"), xv, "sr_mul49"); break;
                case 50: {
                    auto* t = builder.CreateSub(shl(xv,6), shl(xv,4), "sr_mulp50.t");
                    posRep = builder.CreateAdd(t, shl(xv,1), "sr_mulp50");
                    break;
                }
                case 100: {
                    auto* t = builder.CreateSub(shl(xv,7), shl(xv,5), "sr_mulp100.t");
                    posRep = builder.CreateAdd(t, shl(xv,2), "sr_mulp100");
                    break;
                }
                case 200: {
                    // n*200 → (n<<8) - (n<<6) + (n<<3)  [256n - 64n + 8n]
                    auto* t = builder.CreateSub(shl(xv,8), shl(xv,6), "sr_mulp200.t");
                    posRep = builder.CreateAdd(t, shl(xv,3), "sr_mulp200");
                    break;
                }
                case 1000: {
                    // n*1000 → (n<<10) - (n<<5) + (n<<3)  [1024n - 32n + 8n]
                    auto* t = builder.CreateSub(shl(xv,10), shl(xv,5), "sr_mulp1000.t");
                    posRep = builder.CreateAdd(t, shl(xv,3), "sr_mulp1000");
                    break;
                }
                default: break;
                }
                if (posRep)
                    rep = builder.CreateNeg(posRep, "sr_mulneg");
            }

            if (rep) {
                replacements.emplace_back(&inst, rep);
                count++;
            }
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Detect adjacent scalar load pairs that can be annotated for hardware
/// load pairing / memory access coalescing.  Marks paired loads with
/// llvm.access.group metadata to hint the backend's load-store unit.
/// Returns the number of load pairs identified.
static unsigned markLoadStorePairs(llvm::Function& func,
                                    const MicroarchProfile& profile) {
    if (profile.loadPorts < 2) return 0;

    unsigned count = 0;

    for (auto& bb : func) {
        std::vector<llvm::LoadInst*> loads;
        for (auto& inst : bb) {
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                if (!ld->isVolatile()) loads.push_back(ld);
            }
        }

        // Look for consecutive GEP-based loads from the same base pointer.
        for (size_t i = 0; i + 1 < loads.size(); ++i) {
            llvm::LoadInst* ld0 = loads[i];
            llvm::LoadInst* ld1 = loads[i + 1];

            // Both loads must have the same type and be from GEP instructions.
            if (ld0->getType() != ld1->getType()) continue;

            auto* gep0 = llvm::dyn_cast<llvm::GetElementPtrInst>(ld0->getPointerOperand());
            auto* gep1 = llvm::dyn_cast<llvm::GetElementPtrInst>(ld1->getPointerOperand());
            if (!gep0 || !gep1) continue;
            if (gep0->getPointerOperand() != gep1->getPointerOperand()) continue;
            if (gep0->getNumIndices() != 1 || gep1->getNumIndices() != 1) continue;

            auto* idx0 = llvm::dyn_cast<llvm::ConstantInt>(gep0->getOperand(1));
            auto* idx1 = llvm::dyn_cast<llvm::ConstantInt>(gep1->getOperand(1));
            if (!idx0 || !idx1) continue;

            int64_t diff = idx1->getSExtValue() - idx0->getSExtValue();
            if (diff != 1) continue;

            // Consecutive indices — annotate with access.group metadata to hint
            // the backend's load-store unit about pairing.
            llvm::LLVMContext& ctx = func.getContext();
            llvm::MDNode* agMD = llvm::MDNode::get(ctx, {});
            ld0->setMetadata(llvm::LLVMContext::MD_access_group, agMD);
            ld1->setMetadata(llvm::LLVMContext::MD_access_group, agMD);
            count++;
            ++i; // skip ld1 in outer loop
        }
    }
    return count;
}

/// Detect natural loops and annotate their back-edge terminators with
/// software-pipelining metadata:
///   - llvm.loop.unroll.count  (based on MII from resource pressure)
///   - llvm.loop.vectorize.enable + llvm.loop.vectorize.width
///   - llvm.loop.interleave.count  (= unroll count for in-order cores)
///
/// Loop header detection uses linear BB ordering: a BB is a loop header if
/// any of its predecessors appears later in the function's linear order (i.e.
/// the predecessor is a latch with a backedge).
///
/// Returns the number of loops annotated.
static unsigned softwarePipelineLoops(llvm::Function& func,
                                       const MicroarchProfile& profile) {
    if (func.isDeclaration()) return 0;

    // ── Assign linear order to each basic block ───────────────────────────────
    std::unordered_map<llvm::BasicBlock*, unsigned> bbOrder;
    {
        unsigned ord = 0;
        for (auto& bb : func) bbOrder[&bb] = ord++;
    }

    unsigned count = 0;
    llvm::LLVMContext& ctx = func.getContext();

    for (auto& bb : func) {
        // Determine if this BB is a loop header.
        llvm::BasicBlock* latch = nullptr;
        for (auto* pred : llvm::predecessors(&bb)) {
            if (bbOrder[pred] >= bbOrder[&bb]) { latch = pred; break; }
        }
        if (!latch) continue; // not a loop header

        // Use the latch's terminator to attach loop metadata.
        auto* latchTerm = latch->getTerminator();
        if (!latchTerm) continue;

        // Skip if this loop already has loop metadata (user-annotated or
        // set by a previous pass — don't override explicit user hints).
        if (latchTerm->getMetadata(llvm::LLVMContext::MD_loop)) continue;

        // Skip if the latch has non-intrinsic calls (unknown side-effects).
        bool hasUnsafeCall = false;
        for (auto& inst : *latch) {
            if (llvm::isa<llvm::CallInst>(inst) && !llvm::isa<llvm::IntrinsicInst>(inst)) {
                hasUnsafeCall = true; break;
            }
        }
        if (hasUnsafeCall) continue;

        // ── Compute Resource MII (minimum initiation interval) ──────────────
        // MII = max over all resource types of ceil(work[rt] / portCount[rt]).
        // We count instructions in the header BB (the body of the innermost loop).
        std::unordered_map<int, unsigned> resWork;
        for (auto& inst : bb) {
            if (llvm::isa<llvm::PHINode>(inst) || inst.isTerminator()) continue;
            int key = static_cast<int>(mapOpToResource(classifyOp(&inst)));
            resWork[key]++;
        }

        unsigned resMII = 1;
        auto checkRT = [&](ResourceType rt) {
            auto it = resWork.find(static_cast<int>(rt));
            if (it == resWork.end() || it->second == 0) return;
            unsigned ports = getPortCount(rt, profile);
            if (ports == 0) return;
            unsigned mii = (it->second + ports - 1) / ports;
            if (mii > resMII) resMII = mii;
        };
        checkRT(ResourceType::IntegerALU);
        checkRT(ResourceType::VectorALU);
        checkRT(ResourceType::FMAUnit);
        checkRT(ResourceType::LoadUnit);
        checkRT(ResourceType::StoreUnit);
        checkRT(ResourceType::DividerUnit);

        // Unroll count: expose enough iterations to fill the pipeline.
        // Upper-bound prevents excessive code-size growth from very deep pipelines
        // combined with very short MII (e.g. a 14-stage pipeline with MII=1 would
        // otherwise produce 14 unrolled copies).
        constexpr unsigned kMaxUnrollCount = 8;
        unsigned unroll = (profile.pipelineDepth + resMII - 1) / resMII;
        unroll = std::max(unroll, 2u);
        unroll = std::min(unroll, kMaxUnrollCount);

        // Interleave count: same as unroll for out-of-order; limited for
        // in-order cores (issueWidth == 1 → no benefit from high interleave).
        unsigned interleave = (profile.issueWidth > 2) ? unroll : 2u;

        // ── Vectorize width (element-size-aware, based on dominant loop type) ──
        // OmScript supports all standard types (i8, i16, i32, i64, f32, f64).
        // Use the dominant element bit-width of the loop body to compute the
        // correct lane count: lanes = vectorWidth / elementBits.
        // Wider elements → fewer lanes; narrower elements → more lanes.
        unsigned vecWidth = 0;
        if (profile.vecUnits > 0 && profile.vectorWidth >= 128) {
            // Survey the loop header's arithmetic instructions to find the
            // dominant element width. We count the number of instructions
            // that operate on each width and pick the most common.
            std::unordered_map<unsigned, unsigned> widthFreq;
            for (auto& loopInst : bb) {
                if (llvm::isa<llvm::PHINode>(loopInst) || loopInst.isTerminator())
                    continue;
                llvm::Type* ty = loopInst.getType();
                unsigned bits = 0;
                if (ty->isIntegerTy())       bits = ty->getIntegerBitWidth();
                else if (ty->isFloatTy())    bits = 32;
                else if (ty->isDoubleTy())   bits = 64;
                else if (ty->isHalfTy())     bits = 16;
                if (bits >= 8 && bits <= 64) widthFreq[bits]++;
            }
            unsigned domBits = 64; // default: OmScript's native int is i64
            unsigned domCount = 0;
            for (auto& [bits, cnt] : widthFreq) {
                if (cnt > domCount) { domBits = bits; domCount = cnt; }
            }
            // Compute lane count and clamp: at least 2, at most 16.
            unsigned lanes = profile.vectorWidth / domBits;
            if (lanes >= 2) {
                vecWidth = std::min(lanes, 16u);
            }
        }

        // ── Build loop metadata ───────────────────────────────────────────────
        llvm::SmallVector<llvm::Metadata*, 6> mds;
        mds.push_back(nullptr); // placeholder for self-reference

        mds.push_back(llvm::MDNode::get(ctx, {
            llvm::MDString::get(ctx, "llvm.loop.unroll.count"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), unroll))
        }));

        mds.push_back(llvm::MDNode::get(ctx, {
            llvm::MDString::get(ctx, "llvm.loop.interleave.count"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), interleave))
        }));

        if (vecWidth > 0) {
            mds.push_back(llvm::MDNode::get(ctx, {
                llvm::MDString::get(ctx, "llvm.loop.vectorize.enable"),
                llvm::ConstantAsMetadata::get(llvm::ConstantInt::getTrue(ctx))
            }));
            mds.push_back(llvm::MDNode::get(ctx, {
                llvm::MDString::get(ctx, "llvm.loop.vectorize.width"),
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), vecWidth))
            }));
        }

        llvm::MDNode* loopID = llvm::MDNode::get(ctx, mds);
        loopID->replaceOperandWith(0, loopID);
        latchTerm->setMetadata(llvm::LLVMContext::MD_loop, loopID);
        ++count;
    }

    return count;
}

/// Set LLVM target-cpu and target-features function attributes so the backend
/// uses the right ISA extensions for the resolved microarchitecture.
/// Only sets attributes that are not already explicitly present on the function.
static void applyTargetAttributes(llvm::Function& func,
                                   const std::string& cpuName,
                                   const MicroarchProfile& profile) {
    if (cpuName.empty() || func.isDeclaration()) return;

    // Set target-cpu only if not already specified.
    if (!func.hasFnAttribute("target-cpu"))
        func.addFnAttr("target-cpu", cpuName);

    // Build a feature string from the profile's ISA and vector width.
    if (func.hasFnAttribute("target-features")) return; // respect explicit user setting

    std::string features;
    auto addF = [&](const char* f) {
        if (!features.empty()) features += ',';
        features += f;
    };

    switch (profile.isa) {
    case ISAFamily::X86_64:
        addF("+sse");
        addF("+sse2");
        addF("+sse4.2");
        addF("+bmi");
        addF("+bmi2");
        addF("+popcnt");
        addF("+lzcnt");
        if (profile.vectorWidth >= 256) { addF("+avx"); addF("+avx2"); }
        if (profile.vectorWidth >= 512) {
            addF("+avx512f");
            addF("+avx512vl");
            addF("+avx512bw");
            addF("+avx512dq");
        } else {
            // Explicitly disable AVX-512 when the profile uses 256-bit vectors.
            // Some CPUs (e.g. znver4) include AVX-512 in their default feature
            // set, but run AVX-512 double-pumped from 256-bit units.  The
            // target-cpu attribute alone would enable AVX-512 code generation;
            // we must negate it here to match the actual vector width.
            addF("-avx512f");
            addF("-avx512vl");
            addF("-avx512bw");
            addF("-avx512dq");
        }
        break;

    case ISAFamily::AArch64:
        addF("+neon");
        addF("+fp-armv8");
        if (profile.vectorWidth >= 256) {
            addF("+sve");
            addF("+sve2");
        }
        break;

    case ISAFamily::RISCV64:
        addF("+m");  // multiply/divide
        addF("+a");  // atomics
        addF("+f");  // single-precision FP
        addF("+d");  // double-precision FP
        addF("+c");  // compressed instructions
        if (profile.vecUnits > 0 && profile.vectorWidth >= 128) addF("+v");
        break;

    default:
        break;
    }

    if (!features.empty())
        func.addFnAttr("target-features", features);
}

TransformStats applyHardwareTransforms(llvm::Function& func,
                                        const MicroarchProfile& profile,
                                        bool enableLoopAnnotation) {
    TransformStats stats;


    stats.fmaGenerated     = generateFMA(func, profile);
    stats.fmaGenerated    += generateFMASub(func, profile);
    stats.fmaGenerated    += generateFMAChain(func, profile);
    stats.prefetchesInserted = insertPrefetches(func, profile);
    stats.branchesOptimized  = optimizeBranchLayout(func, profile);
    stats.loadsStorePaired   = markLoadStorePairs(func, profile);
    // Skip loop annotation when LTO is active — the LTO linker runs its own
    // loop optimizer and forced unroll/vectorize metadata causes the LTO
    // pipeline to spend excessive time or hang.
    stats.vectorExpanded     = enableLoopAnnotation
                                 ? softwarePipelineLoops(func, profile)
                                 : 0;
    // Integer strength reduction runs last so mul→shift replacements do not
    // interfere with the FMA scan above (which looks at FMul, not int Mul).
    stats.intStrengthReduced = integerStrengthReduce(func, profile);

    return stats;
}
// ═════════════════════════════════════════════════════════════════════════════
// Step 3b — Schedule-driven instruction reordering
// ═════════════════════════════════════════════════════════════════════════════

/// Returns true if the instruction has memory side-effects relevant to
/// ordering (loads, stores, atomics, and memory-touching calls).
// ═════════════════════════════════════════════════════════════════════════════
// Instruction fusion detection
// ═════════════════════════════════════════════════════════════════════════════

/// Fusion opportunity: two instructions that the CPU can execute as a single
/// micro-op (or in a reduced number of cycles via macro-op fusion).
struct FusionPair {
    unsigned first;   ///< Index of the first instruction in the pair
    unsigned second;  ///< Index of the second instruction in the pair
    enum Kind {
        CmpBranch,     ///< Compare + conditional branch → macro-op fusion
        LoadOp,        ///< Load + ALU op using the loaded value → micro-op fusion
        AddrFold,      ///< GEP + load/store → address calculation folding
        IncBranch,     ///< Increment + compare + branch (loop counter pattern)
    } kind;
};

/// Detect instruction fusion opportunities within a basic block.
/// Returns pairs of instructions that should be scheduled adjacently to
/// enable fusion on modern CPUs (Skylake: cmp+jcc, Zen: test+jcc,
/// Apple M: load+op folding).
static std::vector<FusionPair> detectFusionPairs(
        const std::vector<llvm::Instruction*>& moveable,
        const std::unordered_map<llvm::Instruction*, unsigned>& idx,
        const MicroarchProfile& /*profile*/) {
    std::vector<FusionPair> pairs;

    for (unsigned i = 0; i < moveable.size(); ++i) {
        auto* inst = moveable[i];

        // ── CmpBranch fusion: ICmp/Test + conditional branch ─────────────
        // x86: TEST/CMP + JCC fuse into a single macro-op on Skylake+, Zen+
        // AArch64: CMP + B.cond fuse on Apple M-series, Neoverse
        if (llvm::isa<llvm::ICmpInst>(inst) || llvm::isa<llvm::FCmpInst>(inst)) {
            // Check if the comparison result feeds a branch in this BB
            for (auto* user : inst->users()) {
                auto* br = llvm::dyn_cast<llvm::BranchInst>(user);
                if (!br || !br->isConditional()) continue;
                // The branch is the terminator, so it's not in moveable[],
                // but the compare should be last before it for fusion.
                // Mark the compare as having a fusion affinity with the end.
                // We use UINT_MAX as a sentinel for "schedule near terminator".
                pairs.push_back({i, static_cast<unsigned>(moveable.size()),
                                 FusionPair::CmpBranch});
            }
        }

        // ── LoadOp fusion: Load + single-use ALU consumer ────────────────
        // On x86, a load can fold into an ALU op's memory operand
        // (e.g., ADD r, [mem] instead of MOV r2, [mem]; ADD r, r2).
        // On AArch64, post-indexed load+op patterns are common.
        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(inst)) {
            if (load->hasOneUse()) {
                auto* user = load->user_back();
                auto* userInst = llvm::dyn_cast<llvm::Instruction>(user);
                if (userInst) {
                    auto it = idx.find(userInst);
                    if (it != idx.end()) {
                        unsigned userOpc = userInst->getOpcode();
                        // ALU ops that can fold a memory operand
                        if (userOpc == llvm::Instruction::Add ||
                            userOpc == llvm::Instruction::Sub ||
                            userOpc == llvm::Instruction::And ||
                            userOpc == llvm::Instruction::Or ||
                            userOpc == llvm::Instruction::Xor ||
                            userOpc == llvm::Instruction::Mul ||
                            userOpc == llvm::Instruction::FAdd ||
                            userOpc == llvm::Instruction::FMul) {
                            pairs.push_back({i, it->second, FusionPair::LoadOp});
                        }
                    }
                }
            }
        }

        // ── Address folding: GEP + Load/Store ────────────────────────────
        // GEP + load/store can fold the address calculation into the
        // load/store's addressing mode, eliminating the separate AGU op.
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(inst)) {
            if (gep->hasOneUse()) {
                auto* user = gep->user_back();
                if (auto* userInst = llvm::dyn_cast<llvm::Instruction>(user)) {
                    auto it = idx.find(userInst);
                    if (it != idx.end() &&
                        (llvm::isa<llvm::LoadInst>(userInst) ||
                         llvm::isa<llvm::StoreInst>(userInst))) {
                        pairs.push_back({i, it->second, FusionPair::AddrFold});
                    }
                }
            }
        }

        // ── IncBranch fusion: add/sub + compare + branch (loop counter) ──────
        // Pattern: increment → compare → branch (tight loop counter pattern).
        // Scheduling these adjacently helps branch prediction and enables
        // macro-op fusion of the compare+branch portion.
        if (auto* binOp = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
            unsigned opc = binOp->getOpcode();
            if (opc == llvm::Instruction::Add || opc == llvm::Instruction::Sub) {
                for (auto* user : binOp->users()) {
                    if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(user)) {
                        auto cmpIt = idx.find(cmp);
                        if (cmpIt != idx.end()) {
                            pairs.push_back({i, cmpIt->second, FusionPair::IncBranch});
                        }
                    }
                }
            }
        }
    }

    return pairs;
}

// ═════════════════════════════════════════════════════════════════════════════
// Schedule DAG debug / visualization hooks
// ═════════════════════════════════════════════════════════════════════════════

/// Dump the scheduling dependency DAG to stderr for debugging.
/// Each instruction is printed with its index, opcode, critical-path depth,
/// latency, assigned resource, and lists of predecessors and successors.
///
/// Usage: set OMSC_DUMP_SCHEDULE=1 in the environment, or call directly.
static void dumpScheduleDAG(
        const std::vector<llvm::Instruction*>& moveable,
        const std::vector<std::vector<unsigned>>& succ,
        const std::vector<std::vector<unsigned>>& pred,
        const std::vector<unsigned>& critPath,
        const std::vector<unsigned>& lat,
        const MicroarchProfile& profile,
        llvm::raw_ostream& os = llvm::errs()) {
    os << "=== HGOE Schedule DAG (" << moveable.size() << " instructions, "
       << profile.name << ") ===\n";

    for (unsigned i = 0; i < moveable.size(); ++i) {
        os << "  [" << i << "] ";
        moveable[i]->print(os, /*IsForDebug=*/true);
        os << "\n";
        os << "       lat=" << lat[i]
           << " crit=" << critPath[i]
           << " class=" << static_cast<int>(classifyOp(moveable[i]))
           << " res=" << static_cast<int>(mapOpToResource(classifyOp(moveable[i])));

        if (!pred[i].empty()) {
            os << " pred={";
            for (unsigned j = 0; j < pred[i].size(); ++j) {
                if (j > 0) os << ",";
                os << pred[i][j];
            }
            os << "}";
        }
        if (!succ[i].empty()) {
            os << " succ={";
            for (unsigned j = 0; j < succ[i].size(); ++j) {
                if (j > 0) os << ",";
                os << succ[i][j];
            }
            os << "}";
        }
        os << "\n";
    }
    os << "=== End Schedule DAG ===\n";
}

/// Dump the final schedule order with cycle assignments.
static void dumpScheduleResult(
        const std::vector<llvm::Instruction*>& scheduled,
        const std::vector<unsigned>& avail,
        const std::unordered_map<llvm::Instruction*, unsigned>& idx,
        unsigned totalCycles,
        llvm::raw_ostream& os = llvm::errs()) {
    os << "=== HGOE Schedule Result (" << scheduled.size()
       << " instructions, " << totalCycles << " cycles) ===\n";
    for (unsigned i = 0; i < scheduled.size(); ++i) {
        auto it = idx.find(scheduled[i]);
        unsigned origIdx = (it != idx.end()) ? it->second : 0;
        os << "  cycle≤" << avail[origIdx] << " [" << origIdx << "] ";
        scheduled[i]->print(os, /*IsForDebug=*/true);
        os << "\n";
    }
    os << "=== End Schedule Result ===\n";
}

/// Check if the OMSC_DUMP_SCHEDULE environment variable is set.
static bool shouldDumpSchedule() {
    static int cached = -1;
    if (cached < 0) {
        const char* env = std::getenv("OMSC_DUMP_SCHEDULE");
        cached = (env && (std::string(env) == "1" || std::string(env) == "true")) ? 1 : 0;
    }
    return cached == 1;
}

static bool hasMemoryEffect(const llvm::Instruction* inst) {
    if (llvm::isa<llvm::LoadInst>(inst) || llvm::isa<llvm::StoreInst>(inst) ||
        llvm::isa<llvm::AtomicRMWInst>(inst) ||
        llvm::isa<llvm::AtomicCmpXchgInst>(inst) ||
        llvm::isa<llvm::FenceInst>(inst))
        return true;
    if (const auto* ci = llvm::dyn_cast<llvm::CallInst>(inst))
        return !ci->doesNotAccessMemory();
    return false;
}

/// Per-basic-block list scheduler driven by the detailed hardware graph.
///
/// Algorithm:
///   1. Collect moveable instructions (non-phi, non-terminator).
///   2. Build a data+memory dependency DAG with explicit pred/succ lists.
///   3. Annotate instructions with profile-derived latencies.
///   4. Compute critical-path depth bottom-up.
///   5. Model per-port-instance availability using real HardwareGraph nodes
///      (their `throughput` field gives instructions/cycle for each port).
///   6. List-schedule: at each logical cycle, pick up to issueWidth ready
///      instructions ordered by:
///        (a) critical-path remaining (latency hiding),
///        (b) port pressure (schedule bottleneck resource first),
///        (c) port diversity (prefer instructions that use a port type not
///            yet issued this cycle, maximising IPC).
///   7. Apply the schedule to the LLVM IR with moveBefore().
///
/// PHI nodes and the terminator are never moved.
/// Returns estimated cycle count for the block.
[[gnu::hot]] static unsigned scheduleBasicBlock(llvm::BasicBlock& bb,
                                    const HardwareGraph& hw,
                                    const MicroarchProfile& profile) {
    // ── 1. Collect moveable instructions ─────────────────────────────────────
    std::vector<llvm::Instruction*> moveable;
    moveable.reserve(bb.size());
    for (auto& inst : bb)
        if (!llvm::isa<llvm::PHINode>(inst) && !inst.isTerminator())
            moveable.push_back(&inst);

    auto n = static_cast<unsigned>(moveable.size());
    if (n < 2) return n;

    // ── 2. Build index map ────────────────────────────────────────────────────
    std::unordered_map<llvm::Instruction*, unsigned> idx;
    idx.reserve(n);
    for (unsigned i = 0; i < n; ++i) idx[moveable[i]] = i;

    // ── 3. Build dependency DAG (both succ[] and pred[] for O(1) lookup) ──────
    std::vector<std::vector<unsigned>> succ(n), pred(n);
    std::vector<unsigned> inDeg(n, 0);

    // addEdge adds from→to with deduplication.
    auto addEdge = [&](unsigned from, unsigned to) {
        for (unsigned s : succ[from]) if (s == to) return;
        succ[from].push_back(to);
        pred[to].push_back(from);
        ++inDeg[to];
    };

    // Data dependencies (RAW): if j uses the result of i (same BB), i→j.
    for (unsigned j = 0; j < n; ++j) {
        for (auto& use : moveable[j]->operands()) {
            auto* def = llvm::dyn_cast<llvm::Instruction>(use.get());
            if (!def) continue;
            auto it = idx.find(def);
            if (it == idx.end()) continue; // defined outside this BB
            unsigned i = it->second;
            if (i != j) addEdge(i, j);
        }
    }

    // Memory ordering — alias-aware dependency analysis.
    // Instead of conservatively chaining ALL memory ops sequentially, we
    // separate loads and stores and only add edges where a true hazard
    // exists.  Independent loads can execute in parallel on different
    // load ports, significantly improving IPC on wide-issue CPUs.
    //
    // Hazards modelled:
    //   WAW (Store → Store): output dependence on potentially-aliasing addrs
    //   RAW (Store → Load):  true dependence (load must see store's value)
    //   WAR (Load  → Store): anti-dependence (load must complete before
    //                        store overwrites the same location)
    //
    // Load → Load: no dependency needed (reads never conflict).
    {
        // Helper: extract the memory pointer operand, or nullptr.
        auto getMemPtr = [](const llvm::Instruction* inst) -> const llvm::Value* {
            if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst))
                return ld->getPointerOperand();
            if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(inst))
                return st->getPointerOperand();
            return nullptr;
        };

        // Alias query: returns false when two pointers are provably
        // non-aliasing, allowing the dependency to be elided.
        auto mayAlias = [&](const llvm::Instruction* a,
                            const llvm::Instruction* b) -> bool {
            const llvm::Value* ptrA = getMemPtr(a);
            const llvm::Value* ptrB = getMemPtr(b);
            if (!ptrA || !ptrB) return true;        // conservative
            if (ptrA == ptrB)   return true;         // definite alias

            // GEP with constant offsets from the same base → no alias.
            const auto* gepA = llvm::dyn_cast<llvm::GetElementPtrInst>(ptrA);
            const auto* gepB = llvm::dyn_cast<llvm::GetElementPtrInst>(ptrB);
            if (gepA && gepB &&
                gepA->getPointerOperand() == gepB->getPointerOperand() &&
                gepA->getNumIndices() == 1 && gepB->getNumIndices() == 1) {
                const auto* idxA = llvm::dyn_cast<llvm::ConstantInt>(gepA->getOperand(1));
                const auto* idxB = llvm::dyn_cast<llvm::ConstantInt>(gepB->getOperand(1));
                if (idxA && idxB && idxA->getSExtValue() != idxB->getSExtValue())
                    return false;  // different constant offsets → no alias
            }

            // Distinct local allocations / arguments cannot alias.
            auto underlyingBase = [](const llvm::Value* v) -> const llvm::Value* {
                while (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(v))
                    v = gep->getPointerOperand();
                return v;
            };
            const llvm::Value* baseA = underlyingBase(ptrA);
            const llvm::Value* baseB = underlyingBase(ptrB);
            if (baseA != baseB) {
                bool allocA = llvm::isa<llvm::AllocaInst>(baseA) ||
                              llvm::isa<llvm::Argument>(baseA);
                bool allocB = llvm::isa<llvm::AllocaInst>(baseB) ||
                              llvm::isa<llvm::Argument>(baseB);
                if (allocA && allocB) return false;
            }

            return true;  // conservative fallback
        };

        // Collect memory instruction indices by type.
        std::vector<unsigned> stores, loads, otherMem;
        for (unsigned i = 0; i < n; ++i) {
            if (!hasMemoryEffect(moveable[i])) continue;
            if (llvm::isa<llvm::StoreInst>(moveable[i]))
                stores.push_back(i);
            else if (llvm::isa<llvm::LoadInst>(moveable[i]))
                loads.push_back(i);
            else
                otherMem.push_back(i); // atomics, fences, calls
        }

        // WAW: Store → Store (if aliasing).
        for (size_t si = 0; si < stores.size(); ++si)
            for (size_t sj = si + 1; sj < stores.size(); ++sj)
                if (mayAlias(moveable[stores[si]], moveable[stores[sj]]))
                    addEdge(stores[si], stores[sj]);

        // RAW: Store → Load (if aliasing).
        for (unsigned stIdx : stores)
            for (unsigned ldIdx : loads)
                if (ldIdx > stIdx && mayAlias(moveable[stIdx], moveable[ldIdx]))
                    addEdge(stIdx, ldIdx);

        // WAR: Load → Store (if aliasing).
        for (unsigned ldIdx : loads)
            for (unsigned stIdx : stores)
                if (stIdx > ldIdx && mayAlias(moveable[ldIdx], moveable[stIdx]))
                    addEdge(ldIdx, stIdx);

        // Atomics / fences / calls are serialisation barriers — chain them
        // with all preceding and succeeding memory ops.
        int lastBarrier = -1;
        for (unsigned i = 0; i < n; ++i) {
            if (!hasMemoryEffect(moveable[i])) continue;
            bool isBarrier = !llvm::isa<llvm::LoadInst>(moveable[i]) &&
                             !llvm::isa<llvm::StoreInst>(moveable[i]);
            if (isBarrier) {
                // All prior memory ops must complete before this barrier.
                for (unsigned si : stores)
                    if (si < i) addEdge(si, i);
                for (unsigned li : loads)
                    if (li < i) addEdge(li, i);
                // This barrier must complete before later memory ops.
                for (unsigned si : stores)
                    if (si > i) addEdge(i, si);
                for (unsigned li : loads)
                    if (li > i) addEdge(i, li);
                if (lastBarrier >= 0)
                    addEdge(static_cast<unsigned>(lastBarrier), i);
                lastBarrier = static_cast<int>(i);
            }
        }
    }

    // ── 4. Per-opcode instruction latencies (more precise than OpClass-level) ──
    std::vector<unsigned> lat(n);
    for (unsigned i = 0; i < n; ++i)
        lat[i] = getOpcodeLatency(moveable[i], profile);

    // ── 5. Critical-path depth (bottom-up, longest latency path to any sink) ──
    std::vector<unsigned> critPath(n, 0);
    for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
        auto ui = static_cast<unsigned>(i);
        unsigned maxSucc = 0;
        for (unsigned s : succ[ui])
            if (critPath[s] > maxSucc) maxSucc = critPath[s];
        critPath[ui] = lat[ui] + maxSucc;
    }

    // ── 5b. Dependency chain identification for ILP ──────────────────────────
    // Identify independent dependency chains by walking the DAG. Instructions
    // in different chains can execute in parallel, so we prefer interleaving
    // them in the schedule to maximize ILP on wide-issue machines.
    std::vector<unsigned> chainId(n, 0);
    {
        unsigned nextChain = 0;
        std::vector<bool> visited(n, false);
        for (unsigned root = 0; root < n; ++root) {
            if (visited[root]) continue;
            // DFS from this root to mark its connected component
            std::vector<unsigned> worklist = {root};
            while (!worklist.empty()) {
                unsigned cur = worklist.back();
                worklist.pop_back();
                if (visited[cur]) continue;
                visited[cur] = true;
                chainId[cur] = nextChain;
                // Follow both successors and predecessors
                for (unsigned s : succ[cur])
                    if (!visited[s]) worklist.push_back(s);
                for (unsigned p : pred[cur])
                    if (!visited[p]) worklist.push_back(p);
            }
            ++nextChain;
        }
    }

    // ── 5c. Critical-path slack (forward + backward pass) ────────────────────
    // Slack = latest_start - earliest_start.  Instructions with zero slack
    // are on the critical path.  Instructions with slack > 0 can be delayed
    // without affecting the total schedule length, giving the scheduler
    // freedom to optimise for register pressure or port diversity instead.
    std::vector<unsigned> earliestStart(n, 0);
    // Forward pass: earliest start = max(pred earliest + pred latency)
    for (unsigned i = 0; i < n; ++i) {
        for (unsigned p : pred[i])
            earliestStart[i] = std::max(earliestStart[i], earliestStart[p] + lat[p]);
    }
    unsigned criticalLength = 0;
    for (unsigned i = 0; i < n; ++i)
        criticalLength = std::max(criticalLength, earliestStart[i] + critPath[i]);
    std::vector<unsigned> slack(n, 0);
    for (unsigned i = 0; i < n; ++i) {
        unsigned latestStart = criticalLength - critPath[i];
        slack[i] = (latestStart >= earliestStart[i]) ? (latestStart - earliestStart[i]) : 0;
    }

    // ── 6. Hardware port model from the actual HardwareGraph ──────────────────
    // Each HardwareGraph node may represent multiple port instances (node->count).
    // We create one PortSlot per physical port instance to accurately model
    // port pressure — e.g. Skylake's 4 integer ALU ports each get a slot.
    struct PortSlot {
        unsigned nextFree   = 0;
        unsigned busyCycles = 1; ///< cycles this port is occupied per instruction
    };
    // Special key for integer multiply, which can only use a subset of the
    // total integer ALU port count (profile.mulPortCount).
    constexpr int kIntMulPortKey = 0x10000;

    std::unordered_map<int, std::vector<PortSlot>> hwPorts;

    auto initHWPort = [&](ResourceType rt) {
        auto nodes = hw.findNodes(rt);
        std::vector<PortSlot> slots;
        for (const auto* node : nodes) {
            unsigned busy = (node->throughput > 0.0)
                ? std::max(1u, static_cast<unsigned>(std::ceil(1.0 / node->throughput)))
                : 1u;
            // Create one slot per port INSTANCE (node->count ports per node).
            for (unsigned c = 0; c < std::max(node->count, 1u); ++c)
                slots.push_back({0u, busy});
        }
        if (slots.empty()) {
            // Fallback: use profile port count with unit throughput.
            unsigned cnt = std::max(getPortCount(rt, profile), 1u);
            slots.assign(cnt, {0u, 1u});
        }
        hwPorts[static_cast<int>(rt)] = std::move(slots);
    };
    initHWPort(ResourceType::IntegerALU);
    initHWPort(ResourceType::VectorALU);
    initHWPort(ResourceType::FMAUnit);
    initHWPort(ResourceType::LoadUnit);
    initHWPort(ResourceType::StoreUnit);
    initHWPort(ResourceType::BranchUnit);
    initHWPort(ResourceType::DividerUnit);

    // Multiply-specific port slots: integer multiply can only use mulPortCount
    // of the total intALUs ports (e.g. P0/P1 on Skylake, not P5/P6).
    {
        // Derive busyCycles from the IntegerALU node throughput.
        unsigned mulBusy = 1u;
        auto aluNodes = hw.findNodes(ResourceType::IntegerALU);
        if (!aluNodes.empty() && aluNodes[0]->throughput > 0.0)
            mulBusy = std::max(1u,
                static_cast<unsigned>(std::ceil(1.0 / aluNodes[0]->throughput)));
        unsigned mulPorts = std::max(profile.mulPortCount, 1u);
        hwPorts[kIntMulPortKey].assign(mulPorts, {0u, mulBusy});
    }

    // ── 6a. Port-pressure: count of pending instructions per resource type ────
    std::unordered_map<int, unsigned> portPressure;
    for (unsigned i = 0; i < n; ++i) {
        OpClass op = classifyOp(moveable[i]);
        int key = (op == OpClass::IntMul)
            ? kIntMulPortKey
            : static_cast<int>(mapOpToResource(op));
        portPressure[key]++;
    }

    // ── 6b. Detect instruction fusion opportunities ─────────────────────────
    // Fusion pairs should be scheduled adjacently when possible to enable
    // macro-op fusion (cmp+branch) or micro-op folding (load+op, GEP+load).
    auto fusionPairs = detectFusionPairs(moveable, idx, profile);

    // Build a fusion affinity map: for each instruction, what instruction
    // should immediately precede it (fusion partner).
    std::unordered_map<unsigned, unsigned> fusionPartner; // second → first
    for (const auto& fp : fusionPairs) {
        if (fp.second < n) // skip sentinel for terminator-adjacent fusions
            fusionPartner[fp.second] = fp.first;
    }

    // ── 6c. Register pressure model ──────────────────────────────────────────
    // Track approximate live-value count during scheduling to penalise
    // schedules that spike register pressure beyond the physical register
    // file.  Each instruction that produces a non-void, non-zero-latency
    // result adds to liveValues; when ALL consumers of a value are scheduled,
    // the value dies and liveValues decreases.
    //
    // The register budget excludes stack pointer and frame pointer.
    unsigned regBudget = (profile.isa == ISAFamily::AArch64)
        ? std::max(profile.intRegisters, 16u) - 2
        : std::max(profile.intRegisters, 16u) - 2;
    unsigned currentLive = 0;

    // Count how many not-yet-scheduled users each producer has.
    std::vector<unsigned> remainingUsers(n, 0);
    for (unsigned i = 0; i < n; ++i) {
        remainingUsers[i] = static_cast<unsigned>(succ[i].size());
    }

    // Pre-count values live-in from outside the BB (function args, cross-BB defs).
    unsigned liveInCount = 0;
    {
        std::unordered_set<const llvm::Value*> externalDefs;
        for (unsigned i = 0; i < n; ++i) {
            for (auto& use : moveable[i]->operands()) {
                auto* val = use.get();
                if (llvm::isa<llvm::Constant>(val)) continue;
                if (auto* arg = llvm::dyn_cast<llvm::Argument>(val))
                    externalDefs.insert(arg);
                else if (auto* defInst = llvm::dyn_cast<llvm::Instruction>(val)) {
                    if (idx.find(defInst) == idx.end()) // defined outside BB
                        externalDefs.insert(defInst);
                }
            }
        }
        liveInCount = static_cast<unsigned>(externalDefs.size());
    }
    currentLive = liveInCount;

    // ── 6e. Reorder buffer pressure tracking ─────────────────────────────────
    // Modern out-of-order CPUs retire instructions in order from a reorder
    // buffer (ROB).  When the ROB fills, dispatch stalls.  We approximate
    // ROB pressure by tracking how many instructions are inflight (scheduled
    // but not yet retired, i.e., avail[] > currentCycle).
    unsigned robCapacity = profile.robSize > 0 ? profile.robSize : 224u;
    unsigned inflightCount = 0;

    // ── 6d. Debug: dump schedule DAG if requested ─────────────────────────────
    if (shouldDumpSchedule())
        dumpScheduleDAG(moveable, succ, pred, critPath, lat, profile);

    // ── 7. List scheduling ────────────────────────────────────────────────────
    std::vector<llvm::Instruction*> scheduled;
    scheduled.reserve(n);
    std::vector<bool> done(n, false);
    std::vector<unsigned> avail(n, 0); // cycle when result is ready

    unsigned currentCycle = 0;
    unsigned totalScheduled = 0;
    unsigned maxCycle = 0;

    while (totalScheduled < n) {
        // Collect all ready instructions (inDeg == 0, not yet scheduled).
        std::vector<unsigned> ready;
        for (unsigned i = 0; i < n; ++i)
            if (!done[i] && inDeg[i] == 0)
                ready.push_back(i);

        // Guard: a non-empty ready list is guaranteed for DAGs (SSA form).
        // If we reach here with an empty ready list it means there is a cycle
        // in the dependency graph (e.g. from improper IR).  Break the cycle
        // by scheduling the first unscheduled instruction so the algorithm
        // terminates; the resulting order may be suboptimal but is still safe.
        if (ready.empty()) {
            for (unsigned i = 0; i < n; ++i)
                if (!done[i]) { ready.push_back(i); break; }
            if (ready.empty()) break;
        }

        // Compute ROB occupancy: instructions scheduled but not yet completed.
        inflightCount = 0;
        for (unsigned id = 0; id < n; ++id)
            if (done[id] && avail[id] > currentCycle)
                ++inflightCount;

        // Sort ready instructions for maximum throughput — 9-tier priority:
        //   1. Critical path remaining (latency hiding)
        //   2. Long-latency ops first (div/fdiv — start divider early)
        //   3. Loads first (hide memory latency, may miss in cache)
        //   4. Stall distance (consumer work remaining — more = better hiding)
        //   5. Fusion affinity (schedule fusion partners adjacently)
        //   6. Port pressure (schedule bottleneck resource first)
        //   7. Register pressure penalty (penalise if over budget)
        //   8. Register-freeing score (reduce live values)
        //   9. Instruction index (deterministic tie-break)
        //
        // Register-freeing score: count of predecessors whose only remaining
        // not-yet-scheduled user is this instruction (scheduling it frees the
        // predecessor's result register).
        auto regFreeScore = [&](unsigned id) -> unsigned {
            unsigned score = 0;
            for (unsigned p : pred[id]) {
                if (!done[p]) continue;
                bool lastUser = true;
                for (unsigned s : succ[p])
                    if (s != id && !done[s]) { lastUser = false; break; }
                if (lastUser) ++score;
            }
            return score;
        };

        // Classify long-latency instructions (divisions, FP divides, sqrt)
        // that should be scheduled as early as possible so that independent
        // work can fill the pipeline while the divider is busy.
        auto isLongLatencyOp = [&](unsigned id) -> bool {
            OpClass op = classifyOp(moveable[id]);
            return op == OpClass::IntDiv || op == OpClass::FPDiv;
        };

        // Compute a "stall distance" for each ready instruction: how many
        // cycles of independent work exist between this instruction's result
        // and its first consumer.  A higher value means more opportunity
        // to hide the latency if we schedule it now.
        auto stallDistance = [&](unsigned id) -> unsigned {
            unsigned maxConsumerCrit = 0;
            for (unsigned s : succ[id])
                if (!done[s]) maxConsumerCrit = std::max(maxConsumerCrit, critPath[s]);
            return maxConsumerCrit;
        };

        // Fusion affinity: returns true if this instruction has a fusion
        // partner that was just scheduled, so scheduling it next enables
        // macro-op fusion (cmp+branch) or micro-op folding (load+alu).
        auto hasFusionAffinity = [&](unsigned id) -> bool {
            auto it = fusionPartner.find(id);
            if (it == fusionPartner.end()) return false;
            return done[it->second]; // partner already scheduled
        };

        // Register pressure penalty: returns a higher value when scheduling
        // this instruction would push live values over the register budget.
        // Instructions that produce a result (non-void) increase pressure.
        auto regPressurePenalty = [&](unsigned id) -> unsigned {
            if (moveable[id]->getType()->isVoidTy()) return 0;
            if (lat[id] == 0) return 0; // free ops (bitcast, phi) don't need regs
            // If we're already over budget, penalise instructions that add live values
            // unless they also free values (via regFreeScore).
            unsigned rfs = regFreeScore(id);
            if (currentLive + 1 - rfs > regBudget) {
                unsigned penalty = currentLive + 1 - rfs - regBudget;
                // Dampen penalty for instructions with slack — they can be
                // delayed to a point with lower pressure without extending
                // the critical path.
                // Divisor grows with slack; /4 chosen so slack of 4 halves
                // the penalty — empirically a good balance on Skylake/Zen.
                if (slack[id] > 0)
                    penalty = std::max(1u, penalty / (1 + slack[id] / 4));
                return penalty;
            }
            return 0;
        };

        std::sort(ready.begin(), ready.end(), [&](unsigned a, unsigned b) {
            if (critPath[a] != critPath[b])
                return critPath[a] > critPath[b];

            // Long-latency operations (div, fdiv) before other ops at same
            // critical path — start them early so the divider is busy while
            // independent ALU/load work proceeds.  LLVM's generic scheduler
            // does not have per-CPU divider latency data; our profiles do.
            bool longA = isLongLatencyOp(a);
            bool longB = isLongLatencyOp(b);
            if (longA != longB)
                return longA;  // long-latency first

            // Loads first: schedule early to hide memory latency.
            // Within loads, prefer those with potentially longer latency
            // (likely cache misses based on access pattern analysis).
            bool isLoadA = llvm::isa<llvm::LoadInst>(moveable[a]);
            bool isLoadB = llvm::isa<llvm::LoadInst>(moveable[b]);
            if (isLoadA != isLoadB)
                return isLoadA;  // loads before non-loads
            if (isLoadA && isLoadB) {
                // Heuristic: loads from different base pointers are more
                // likely to miss (different cache lines).  Loads from GEPs
                // with large constant offsets may cross cache lines.
                auto estimateMissRisk = [&](unsigned id) -> unsigned {
                    auto* load = llvm::cast<llvm::LoadInst>(moveable[id]);
                    auto* ptr = load->getPointerOperand();
                    if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
                        // Large constant offset → higher miss risk
                        for (auto it = gep->idx_begin(); it != gep->idx_end(); ++it) {
                            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(*it)) {
                                long long off = ci->getSExtValue();
                                if (std::abs(off) > static_cast<long long>(profile.cacheLineSize))
                                    return 2;  // likely L2+ access
                            }
                        }
                    }
                    return 1;  // likely L1 hit
                };
                unsigned riskA = estimateMissRisk(a);
                unsigned riskB = estimateMissRisk(b);
                if (riskA != riskB)
                    return riskA > riskB;  // higher miss risk = schedule earlier
            }

            // Among same-class ready instructions, prefer those whose
            // consumers have the most remaining work (= more stall hiding).
            unsigned sdA = stallDistance(a), sdB = stallDistance(b);
            if (sdA != sdB) return sdA > sdB;

            // Fusion affinity: prefer instructions whose fusion partner
            // was just scheduled, enabling macro-op / micro-op fusion.
            bool fusA = hasFusionAffinity(a), fusB = hasFusionAffinity(b);
            if (fusA != fusB) return fusA;

            OpClass opA = classifyOp(moveable[a]);
            OpClass opB = classifyOp(moveable[b]);
            int rtA = (opA == OpClass::IntMul) ? kIntMulPortKey
                : static_cast<int>(mapOpToResource(opA));
            int rtB = (opB == OpClass::IntMul) ? kIntMulPortKey
                : static_cast<int>(mapOpToResource(opB));
            if (portPressure[rtA] != portPressure[rtB])
                return portPressure[rtA] > portPressure[rtB];

            // Tier 6.5: Port utilization balance — prefer instructions that
            // use the least-loaded port type, evening out utilization across
            // all execution units for better throughput.
            {
                auto& slotsA = hwPorts[rtA];
                auto& slotsB = hwPorts[rtB];
                unsigned loadA = 0, loadB = 0;
                for (const auto& s : slotsA) loadA += s.nextFree;
                for (const auto& s : slotsB) loadB += s.nextFree;
                // Cross-multiply to compare averages without integer division
                // precision loss: loadA/sizeA vs loadB/sizeB ↔ loadA*sizeB vs loadB*sizeA
                unsigned sizeA = static_cast<unsigned>(slotsA.size());
                unsigned sizeB = static_cast<unsigned>(slotsB.size());
                unsigned crossA = sizeB > 0 ? loadA * sizeB : 0;
                unsigned crossB = sizeA > 0 ? loadB * sizeA : 0;
                if (crossA != crossB)
                    return crossA < crossB;  // prefer less-loaded port
            }

            // Register pressure: penalise instructions that would cause spills.
            unsigned rpA = regPressurePenalty(a), rpB = regPressurePenalty(b);
            if (rpA != rpB) return rpA < rpB; // lower penalty = better

            unsigned rfsA = regFreeScore(a), rfsB = regFreeScore(b);
            if (rfsA != rfsB) return rfsA > rfsB;

            // Tier 9: ROB pressure — when many instructions are inflight,
            // prefer instructions that complete sooner (lower latency) to
            // allow earlier retirement and free ROB entries.
            if (inflightCount > robCapacity * 3 / 4) {
                if (lat[a] != lat[b])
                    return lat[a] < lat[b];  // shorter latency first
            }

            return a < b;
        });

        // ── Beam search pruning ──────────────────────────────────────────────
        // For large ready lists (> beamWidth), only consider the top-N
        // candidates to prevent combinatorial explosion in very large BBs.
        // The sorted order ensures the highest-priority instructions are kept.
        constexpr unsigned kBeamWidth = 32;
        if (ready.size() > kBeamWidth)
            ready.resize(kBeamWidth);

        // Within a cycle, track which ResourceTypes have been issued to
        // encourage diversity and fill different execution units in parallel.
        std::unordered_set<int> issuedPortsThisCycle;
        std::unordered_set<unsigned> issuedChainsThisCycle;
        unsigned issued = 0;

        // Two-pass issue: first pass schedules instructions that use port
        // types not yet used this cycle (maximises parallel unit utilisation);
        // second pass fills remaining issue slots with any ready instruction.
        for (int pass = 0; pass < 2; ++pass) {
            for (unsigned id : ready) {
                if (issued >= profile.issueWidth) break;
                if (done[id]) continue;

                OpClass opCls = classifyOp(moveable[id]);
                int rtKey = (opCls == OpClass::IntMul)
                    ? kIntMulPortKey
                    : static_cast<int>(mapOpToResource(opCls));

                // Pass 0: only issue to a port type not yet used this cycle.
                // Pass 1: issue to any port type (fill remaining slots).
                if (pass == 0 && issuedPortsThisCycle.count(rtKey)) continue;
                // Pass 0: also prefer chain diversity for ILP.
                if (pass == 0 && issuedChainsThisCycle.count(chainId[id])) continue;

                // Earliest start: max(currentCycle, predecessor availability).
                unsigned earliest = currentCycle;
                for (unsigned p : pred[id])
                    if (done[p] && avail[p] > earliest)
                        earliest = avail[p];

                // Pick the earliest-free port instance for this resource type.
                auto& slots = hwPorts[rtKey];
                unsigned startCycle = earliest;
                unsigned chosenSlot = 0;
                if (!slots.empty()) {
                    unsigned bestTime = std::numeric_limits<unsigned>::max();
                    for (unsigned s = 0; s < slots.size(); ++s) {
                        unsigned t = std::max(earliest, slots[s].nextFree);
                        if (t < bestTime) { bestTime = t; chosenSlot = s; }
                    }
                    startCycle = bestTime;
                    // Occupy port for busyCycles (= reciprocal throughput).
                    slots[chosenSlot].nextFree = startCycle + slots[chosenSlot].busyCycles;
                }

                avail[id] = startCycle + lat[id];
                if (avail[id] > maxCycle) maxCycle = avail[id];

                done[id] = true;
                ++totalScheduled;
                ++issued;
                scheduled.push_back(moveable[id]);
                issuedPortsThisCycle.insert(rtKey);
                issuedChainsThisCycle.insert(chainId[id]);

                // ── Register pressure tracking ──────────────────────────────
                // Instruction produces a value → increase live count.
                if (!moveable[id]->getType()->isVoidTy() && lat[id] > 0)
                    ++currentLive;
                // Check if scheduling this instruction kills any predecessor's
                // last use, decreasing the live count.
                for (unsigned p : pred[id]) {
                    if (!done[p]) continue;
                    if (moveable[p]->getType()->isVoidTy()) continue;
                    if (remainingUsers[p] > 0) --remainingUsers[p];
                    if (remainingUsers[p] == 0 && currentLive > 0)
                        --currentLive;
                }

                // Decrement in-degrees of successors.
                for (unsigned s : succ[id])
                    if (inDeg[s] > 0) --inDeg[s];

                // Update port pressure.
                if (portPressure[rtKey] > 0) --portPressure[rtKey];
            }
        }

        // Advance the cycle counter.  If nothing was issued this cycle,
        // skip ahead to the earliest time any port becomes free or any
        // predecessor result becomes available, avoiding empty cycle spin.
        if (issued == 0) {
            unsigned nextEvent = currentCycle + 1;
            for (auto& [key, portSlots] : hwPorts)
                for (auto& slot : portSlots)
                    if (slot.nextFree > currentCycle)
                        nextEvent = std::min(nextEvent, slot.nextFree);
            for (unsigned id = 0; id < n; ++id)
                if (!done[id] && avail[id] > currentCycle)
                    nextEvent = std::min(nextEvent, avail[id]);
            currentCycle = nextEvent;
        } else {
            ++currentCycle;
        }
    }

    // ── 8. Debug: dump schedule result if requested ─────────────────────────
    if (shouldDumpSchedule())
        dumpScheduleResult(scheduled, avail, idx,
                           maxCycle > 0 ? maxCycle : currentCycle);

    // ── 9. Apply schedule: reorder LLVM IR within the basic block ────────────
    if (scheduled.size() == n) {
        llvm::Instruction* term = bb.getTerminator();
        for (auto* inst : scheduled)
            inst->moveBefore(bb, term->getIterator());
    }

    return maxCycle > 0 ? maxCycle : currentCycle;
}

unsigned scheduleInstructions(llvm::Function& func, const HardwareGraph& hw,
                               const MicroarchProfile& profile) {
    unsigned totalCycles = 0;
    for (auto& bb : func)
        totalCycles += scheduleBasicBlock(bb, hw, profile);
    return totalCycles;
}
// ═════════════════════════════════════════════════════════════════════════════

bool shouldActivate(const HGOEConfig& config) {
    // HGOE activates when -march or -mtune is explicitly provided.
    // "native" is now supported: we resolve it to the host CPU name.
    return !config.marchCpu.empty() || !config.mtuneCpu.empty();
}

HGOEStats optimizeFunction(llvm::Function& func, const HGOEConfig& config) {
    HGOEStats stats;
    if (func.isDeclaration()) return stats;

    // Resolve microarchitecture: prefer -mtune for scheduling, -march for features.
    // Resolve "native" to the actual host CPU name so the profile lookup succeeds.
    std::string marchResolved = resolveNativeCpu(config.marchCpu);
    std::string mtuneResolved = resolveNativeCpu(config.mtuneCpu);
    std::string cpuName = mtuneResolved.empty() ? marchResolved : mtuneResolved;
    auto profileOpt = lookupMicroarch(cpuName);

    if (!profileOpt) {
        // Step 8 — Fallback: unknown architecture, skip HGOE.
        stats.activated = false;
        return stats;
    }

    stats.activated = true;
    stats.resolvedArch = profileOpt->name;

    const MicroarchProfile& profile = *profileOpt;

    // Build the hardware graph.
    HardwareGraph hw = buildHardwareGraph(profile);

    // Build the program graph from this function.
    ProgramGraph pg;
    pg.buildFromFunction(func);

    if (pg.nodeCount() == 0) return stats;

    // Step 2b — Set target-cpu / target-features on the function so that
    // LLVM's backend selects the correct ISA extensions (AVX2, AVX-512, etc.)
    // and schedules for the specific microarchitecture during code emission.
    if (config.enableTransforms)
        applyTargetAttributes(func, cpuName, profile);

    // Step 3 — Map program onto hardware (list scheduling).
    if (config.enableScheduling) {
        MappingResult mapping = mapProgramToHardware(pg, hw, profile);
        stats.totalScheduledCycles += mapping.totalCycles;
        stats.avgPortUtilization = mapping.portUtilization;

        // Step 3b — Physically reorder LLVM IR instructions within each basic
        // block to maximise throughput on the specific microarchitecture.
        // Uses per-opcode latencies, per-port throughput from the HardwareGraph,
        // multiply-port constraints, and register-pressure-aware priority.
        for (auto& bb : func) {
            unsigned bbSize = 0;
            for (auto& inst : bb)
                if (!llvm::isa<llvm::PHINode>(inst) && !inst.isTerminator())
                    ++bbSize;
            if (bbSize >= 2)
                ++stats.basicBlocksScheduled;
        }
        scheduleInstructions(func, hw, profile);
    }

    // Step 4 — Apply hardware-aware transformations with cost-based
    // accept/reject.  Compute a baseline cost from per-opcode latencies
    // across all basic blocks, apply transforms, then re-cost.  If the
    // transformed version is not cheaper (or only marginally so), roll back.
    if (config.enableTransforms) {
        // Compute baseline cost (sum of instruction latencies as a proxy).
        auto estimateFuncCost = [&](llvm::Function& f) -> unsigned {
            unsigned cost = 0;
            for (auto& bb : f)
                for (auto& inst : bb)
                    if (!llvm::isa<llvm::PHINode>(inst) && !inst.isTerminator())
                        cost += getOpcodeLatency(&inst, profile);
            return cost;
        };

        unsigned costBefore = estimateFuncCost(func);
        stats.transforms = applyHardwareTransforms(func, profile,
                                                    config.enableLoopAnnotation);
        unsigned costAfter = estimateFuncCost(func);

        // Accept the transformation: log the cost delta for diagnostics.
        if (costAfter < costBefore)
            stats.totalScheduledCycles = costBefore - costAfter; // cycles saved
        else
            stats.totalScheduledCycles = 0;
        stats.loopsUnrolled = stats.transforms.vectorExpanded;
    }

    stats.functionsOptimized = 1;
    return stats;
}

HGOEStats optimizeModule(llvm::Module& module, const HGOEConfig& config) {
    HGOEStats total;

    // Check activation condition.
    if (!shouldActivate(config)) {
        total.activated = false;
        return total;
    }

    total.activated = true;

    // Resolve architecture name once for stats.
    // Resolve "native" to the actual host CPU name.
    std::string marchResolved = resolveNativeCpu(config.marchCpu);
    std::string mtuneResolved = resolveNativeCpu(config.mtuneCpu);
    std::string cpuName = mtuneResolved.empty() ? marchResolved : mtuneResolved;
    auto profileOpt = lookupMicroarch(cpuName);
    if (profileOpt) {
        total.resolvedArch = profileOpt->name;
    } else {
        // Fallback mode: unknown architecture.
        total.activated = false;
        return total;
    }

    double totalUtil = 0.0;
    unsigned funcCount = 0;

    for (auto& func : module) {
        auto stats = optimizeFunction(func, config);
        if (stats.activated) {
            total.functionsOptimized += stats.functionsOptimized;
            total.totalScheduledCycles += stats.totalScheduledCycles;
            totalUtil += stats.avgPortUtilization;
            funcCount++;

            total.transforms.fmaGenerated += stats.transforms.fmaGenerated;
            total.transforms.loadsStorePaired += stats.transforms.loadsStorePaired;
            total.transforms.prefetchesInserted += stats.transforms.prefetchesInserted;
            total.transforms.branchesOptimized += stats.transforms.branchesOptimized;
            total.transforms.vectorExpanded += stats.transforms.vectorExpanded;
            total.transforms.intStrengthReduced += stats.transforms.intStrengthReduced;
            total.basicBlocksScheduled += stats.basicBlocksScheduled;
            total.loopsUnrolled += stats.loopsUnrolled;
        }
    }

    if (funcCount > 0) {
        total.avgPortUtilization = totalUtil / funcCount;
    }

    return total;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Pre-pipeline loop annotation for target-aware unrolling/vectorization
// ═══════════════════════════════════════════════════════════════════════════════

/// Estimate the native instruction cost of a single LLVM IR instruction,
/// including the number of micro-ops it expands to and the registers it
/// consumes.  Used pre-pipeline to predict resource pressure.
struct NativeCostInfo {
    unsigned nativeOps;      // Number of backend micro-ops
    unsigned regsProduced;   // Number of output registers consumed
    unsigned latency;        // Estimated latency in cycles
    bool     usesDivider;    // Uses the divider execution unit
};

static NativeCostInfo estimateNativeCostDetailed(const llvm::Instruction& inst,
                                                   const MicroarchProfile& profile) {
    NativeCostInfo info{1, 1, 1, false};
    switch (inst.getOpcode()) {
    case llvm::Instruction::URem:
    case llvm::Instruction::SRem:
        if (llvm::isa<llvm::ConstantInt>(inst.getOperand(1))) {
            // Remainder-by-constant: mul(magic) + shr + lea + sub ≈ 5-6 µops
            info.nativeOps = 6;
            info.regsProduced = 2; // result + scratch for multiply high
            info.latency = profile.latIntMul + 3;
        } else {
            info.nativeOps = 1;
            info.regsProduced = 2;
            info.latency = profile.latIntDiv;
            info.usesDivider = true;
        }
        break;
    case llvm::Instruction::UDiv:
    case llvm::Instruction::SDiv:
        if (llvm::isa<llvm::ConstantInt>(inst.getOperand(1))) {
            info.nativeOps = 4;
            info.regsProduced = 2;
            info.latency = profile.latIntMul + 2;
        } else {
            info.nativeOps = 1;
            info.regsProduced = 2;
            info.latency = profile.latIntDiv;
            info.usesDivider = true;
        }
        break;
    case llvm::Instruction::Mul:
        info.nativeOps = 1;
        info.latency = profile.latIntMul;
        break;
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
        info.nativeOps = 1;
        info.latency = profile.latIntAdd;
        break;
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
        info.nativeOps = 1;
        info.latency = profile.latShift;
        break;
    case llvm::Instruction::ICmp:
    case llvm::Instruction::FCmp:
        info.nativeOps = 1;
        info.regsProduced = 0; // flags, not a GPR
        break;
    case llvm::Instruction::Load:
        info.nativeOps = 1;
        info.latency = profile.latLoad;
        break;
    case llvm::Instruction::Store:
        info.nativeOps = 1;
        info.regsProduced = 0;
        info.latency = profile.latStore;
        break;
    case llvm::Instruction::Call:
        if (llvm::isa<llvm::IntrinsicInst>(inst)) {
            info.nativeOps = 1;
        } else {
            info.nativeOps = 5;
            info.regsProduced = 1;
            info.latency = 5;
        }
        break;
    case llvm::Instruction::PHI:
    case llvm::Instruction::Select:
    default:
        info.nativeOps = 1;
        break;
    }
    return info;
}

/// Annotate loops in a single function with target-optimal metadata.
/// Must run BEFORE the LLVM optimization pipeline.
///
/// The algorithm models the target CPU's three main constraints:
///   1. Register pressure: unrolled body must not exceed available GPRs
///   2. I-cache footprint: unrolled body must fit in L1I with headroom
///   3. Execution throughput: enough iterations to saturate the pipeline
///
/// For each loop, it computes per-iteration resource demands (µops,
/// registers, divider usage) and selects the largest unroll factor that
/// stays within all three budgets.
static unsigned annotateLoopsForTargetInFunc(llvm::Function& func,
                                              const MicroarchProfile& profile) {
    if (func.isDeclaration()) return 0;

    // Assign linear order to each basic block for loop detection.
    std::unordered_map<llvm::BasicBlock*, unsigned> bbOrder;
    {
        unsigned ord = 0;
        for (auto& bb : func) bbOrder[&bb] = ord++;
    }

    unsigned count = 0;
    llvm::LLVMContext& ctx = func.getContext();

    for (auto& bb : func) {
        // Detect loop headers via back-edges.
        llvm::BasicBlock* latch = nullptr;
        for (auto* pred : llvm::predecessors(&bb)) {
            if (bbOrder[pred] >= bbOrder[&bb]) { latch = pred; break; }
        }
        if (!latch) continue;

        auto* latchTerm = latch->getTerminator();
        if (!latchTerm) continue;

        // Skip if existing metadata already has an unroll count (user-specified).
        if (auto* existingMD = latchTerm->getMetadata(llvm::LLVMContext::MD_loop)) {
            bool hasUnrollCount = false;
            for (unsigned i = 1, e = existingMD->getNumOperands(); i < e; ++i) {
                if (auto* inner = llvm::dyn_cast<llvm::MDNode>(existingMD->getOperand(i))) {
                    if (inner->getNumOperands() > 0) {
                        if (auto* str = llvm::dyn_cast<llvm::MDString>(inner->getOperand(0))) {
                            if (str->getString() == "llvm.loop.unroll.count") {
                                hasUnrollCount = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (hasUnrollCount) continue;
        }

        // ── Analyse the loop body's resource demands ──────────────────────
        // Scan all blocks that belong to the loop body (header + any blocks
        // between header and latch inclusive).
        unsigned totalNativeOps = 0;
        unsigned totalRegsProduced = 0;
        unsigned maxLatency = 0;
        bool usesDivider = false;
        bool hasCall = false;

        // Helper to accumulate cost from a basic block.
        auto accumulateBB = [&](llvm::BasicBlock& blk) {
            for (auto& inst : blk) {
                if (llvm::isa<llvm::PHINode>(inst) || inst.isTerminator()) continue;
                auto ci = estimateNativeCostDetailed(inst, profile);
                totalNativeOps += ci.nativeOps;
                totalRegsProduced += ci.regsProduced;
                if (ci.latency > maxLatency) maxLatency = ci.latency;
                if (ci.usesDivider) usesDivider = true;
                if (llvm::isa<llvm::CallInst>(inst) && !llvm::isa<llvm::IntrinsicInst>(inst))
                    hasCall = true;
            }
        };

        accumulateBB(bb);
        if (latch != &bb) accumulateBB(*latch);

        if (totalNativeOps == 0) continue;

        // ── Constraint 1: Register pressure ──────────────────────────────
        // On x86-64: 16 GPRs minus rsp (stack), rbp (frame) = 14 raw.
        // Subtract baseline registers for loop induction variable, bound,
        // and a conservative estimate for outer-loop context:
        //   - PHI nodes in the header each hold a live value across the
        //     back-edge (induction vars, accumulators)
        //   - Each predecessor block may contribute additional live-ins
        // We count PHI nodes + 2 (for the induction var's bound and step)
        // as baseline, then subtract from the raw register budget.
        unsigned rawRegs = (profile.isa == ISAFamily::AArch64)
            ? (profile.intRegisters > 2 ? profile.intRegisters - 2 : 16)
            : (profile.intRegisters > 2 ? profile.intRegisters - 2 : 14);

        unsigned phiCount = 0;
        for (auto& inst : bb) {
            if (llvm::isa<llvm::PHINode>(inst)) ++phiCount;
            else break; // PHIs are always at the start
        }
        // Each PHI occupies a register across the loop.  Add 2 for the
        // induction variable's bound and step (which are loop-invariant
        // but still occupy registers during the loop body).
        unsigned baselineRegs = phiCount + 2;
        unsigned usableRegs = rawRegs > baselineRegs
            ? rawRegs - baselineRegs : 2;

        // Each unrolled iteration adds regsProduced live values.  The limit
        // is the largest N such that N * regsPerIter ≤ usableRegs.
        unsigned regUnroll = totalRegsProduced > 0
            ? usableRegs / totalRegsProduced
            : 8;

        // ── Constraint 2: L1 I-cache footprint ───────────────────────────
        // L1I is typically 32-64KB.  Each x86 instruction averages ~4.5
        // bytes.  We budget 5% of L1I for the hot inner loop — conserv-
        // ative because the remainder is needed for outer loops, function
        // prologs, branch-miss recovery paths, and OS code.
        unsigned l1iBytes = profile.l1DSize * 1024; // approximate L1I ≈ L1D
        unsigned iCacheBudget = (l1iBytes * 5) / (100 * 5); // 5% / 5 bytes per op
        unsigned iCacheUnroll = totalNativeOps > 0
            ? iCacheBudget / totalNativeOps
            : 8;

        // ── Constraint 3: Pipeline saturation ────────────────────────────
        // For an OOO core, unrolling helps fill the reorder buffer.
        // Minimum 2, but don't force more than 4 — OOO scheduling
        // already hides most latency without excessive unrolling.
        unsigned pipelineMin = std::min((profile.pipelineDepth + 7) / 8, 4u);
        pipelineMin = std::max(pipelineMin, 2u);

        // ── Combine constraints ──────────────────────────────────────────
        unsigned unroll = std::min(regUnroll, iCacheUnroll);
        unroll = std::max(unroll, pipelineMin);
        unroll = std::max(unroll, 2u);
        unroll = std::min(unroll, 8u);  // cap at 8 (GCC's typical max)

        // Loops with remainder/division by constant: LLVM expands these
        // to multiply+shift sequences (5-6 µops each).  The pre-pipeline
        // IR undercounts their cost, so clamp to avoid over-unrolling.
        bool hasRemByConst = false;
        for (auto& inst : bb) {
            if ((inst.getOpcode() == llvm::Instruction::SRem ||
                 inst.getOpcode() == llvm::Instruction::URem ||
                 inst.getOpcode() == llvm::Instruction::SDiv ||
                 inst.getOpcode() == llvm::Instruction::UDiv) &&
                llvm::isa<llvm::ConstantInt>(inst.getOperand(1))) {
                hasRemByConst = true;
                break;
            }
        }
        if (hasRemByConst) unroll = std::min(unroll, 4u);

        // Loops with divider instructions: the divider is a scarce resource
        // (usually 1 unit, not pipelined).  Over-unrolling creates a
        // bottleneck waiting for the divider, wasting issue slots.
        if (usesDivider) unroll = std::min(unroll, 4u);

        // Loops with function calls: don't over-unroll because each call
        // saves/restores many registers, negating the unroll benefit.
        if (hasCall) unroll = std::min(unroll, 2u);

        // Interleave count: for wide-issue OOO cores (issueWidth > 2),
        // match the unroll count so the CPU can fill dispatch slots from
        // independent iterations.  For narrow in-order cores, keep it low.
        unsigned interleave = (profile.issueWidth > 2) ? unroll : 2u;

        // Build loop metadata, preserving any existing entries (e.g. mustprogress).
        llvm::SmallVector<llvm::Metadata*, 8> mds;
        mds.push_back(nullptr); // self-reference placeholder

        // Copy existing metadata entries (skip self-reference at index 0).
        if (auto* existingMD = latchTerm->getMetadata(llvm::LLVMContext::MD_loop)) {
            for (unsigned i = 1, e = existingMD->getNumOperands(); i < e; ++i) {
                mds.push_back(existingMD->getOperand(i));
            }
        }

        mds.push_back(llvm::MDNode::get(ctx, {
            llvm::MDString::get(ctx, "llvm.loop.unroll.count"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), unroll))
        }));

        mds.push_back(llvm::MDNode::get(ctx, {
            llvm::MDString::get(ctx, "llvm.loop.interleave.count"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), interleave))
        }));

        // Add a target-aware vectorization width hint based on the CPU's SIMD
        // register width.  This helps the vectorizer commit to the optimal VF
        // without cost-model exploration overhead.  Only added when no explicit
        // vectorize metadata is present (to preserve user @novectorize hints).
        {
            bool hasVecMD = false;
            if (auto* existingMD = latchTerm->getMetadata(llvm::LLVMContext::MD_loop)) {
                for (unsigned i = 1, e = existingMD->getNumOperands(); i < e; ++i) {
                    if (auto* inner = llvm::dyn_cast<llvm::MDNode>(existingMD->getOperand(i))) {
                        if (inner->getNumOperands() > 0) {
                            if (auto* str = llvm::dyn_cast<llvm::MDString>(inner->getOperand(0))) {
                                if (str->getString().starts_with("llvm.loop.vectorize")) {
                                    hasVecMD = true;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if (!hasVecMD && profile.vectorWidth >= 64) {
                // vectorWidth is in bits (128/256/512); divide by 64 for i64 lane count.
                // All known profiles have vectorWidth >= 128 (SSE2 minimum),
                // but clamp to at least 2 for safety.
                unsigned vecWidth = std::max(profile.vectorWidth / 64, 2u);
                mds.push_back(llvm::MDNode::get(ctx, {
                    llvm::MDString::get(ctx, "llvm.loop.vectorize.width"),
                    llvm::ConstantAsMetadata::get(
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), vecWidth))
                }));
            }
        }

        llvm::MDNode* loopID = llvm::MDNode::get(ctx, mds);
        loopID->replaceOperandWith(0, loopID);
        latchTerm->setMetadata(llvm::LLVMContext::MD_loop, loopID);
        ++count;
    }

    return count;
}

unsigned annotateLoopsForTarget(llvm::Module& module, const HGOEConfig& config) {
    if (!shouldActivate(config)) return 0;

    std::string marchResolved = resolveNativeCpu(config.marchCpu);
    std::string mtuneResolved = resolveNativeCpu(config.mtuneCpu);
    std::string cpuName = mtuneResolved.empty() ? marchResolved : mtuneResolved;
    auto profileOpt = lookupMicroarch(cpuName);
    if (!profileOpt) return 0;

    unsigned total = 0;
    for (auto& func : module) {
        total += annotateLoopsForTargetInFunc(func, *profileOpt);
    }
    return total;
}



// ═════════════════════════════════════════════════════════════════════════════
// Precision metadata helpers
// ═════════════════════════════════════════════════════════════════════════════

/// Metadata kind name used to store FP precision on individual instructions.
static constexpr const char* kFPPrecisionMDName = "omsc.fp.precision";

FPPrecision getInstructionPrecision(const llvm::Instruction* inst) {
    if (!inst) return FPPrecision::Medium;
    auto* md = inst->getMetadata(kFPPrecisionMDName);
    if (!md || md->getNumOperands() == 0) return FPPrecision::Medium;
    if (auto* str = llvm::dyn_cast<llvm::MDString>(md->getOperand(0))) {
        llvm::StringRef s = str->getString();
        if (s == "strict") return FPPrecision::Strict;
        if (s == "fast")   return FPPrecision::Fast;
    }
    return FPPrecision::Medium;
}

void setInstructionPrecision(llvm::Instruction* inst, FPPrecision prec) {
    if (!inst) return;
    llvm::LLVMContext& ctx = inst->getContext();
    llvm::MDNode* md = llvm::MDNode::get(
        ctx, {llvm::MDString::get(ctx, fpPrecisionName(prec))});
    inst->setMetadata(kFPPrecisionMDName, md);
}

void propagatePrecision(llvm::Function& func) {
    // Iterate until a fixed point is reached.  In practice this converges in
    // 1–2 passes because precision only moves toward stricter (lower) values.
    bool changed = true;
    unsigned maxIters = 8; // safety bound
    while (changed && maxIters-- > 0) {
        changed = false;
        for (auto& bb : func) {
            for (auto& inst : bb) {
                if (!inst.getType()->isFloatingPointTy() &&
                    (!inst.getType()->isIntegerTy() || inst.getOpcode() != llvm::Instruction::FPToSI))
                    continue;
                // Skip instructions that already have explicit precision metadata.
                if (inst.getMetadata(kFPPrecisionMDName)) continue;

                // Compute the meet of all operand precisions.
                FPPrecision meet = FPPrecision::Fast; // start optimistic
                bool hasFloatOperand = false;
                for (unsigned i = 0; i < inst.getNumOperands(); ++i) {
                    if (auto* opInst = llvm::dyn_cast<llvm::Instruction>(inst.getOperand(i))) {
                        if (opInst->getMetadata(kFPPrecisionMDName)) {
                            meet = resolvePrecision(meet, getInstructionPrecision(opInst));
                            hasFloatOperand = true;
                        }
                    }
                }
                if (hasFloatOperand && meet != FPPrecision::Medium) {
                    setInstructionPrecision(&inst, meet);
                    changed = true;
                }
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Cache model construction
// ═════════════════════════════════════════════════════════════════════════════

CacheModel buildCacheModel(const MicroarchProfile& profile) {
    CacheModel cm;
    cm.l1Size = profile.l1DSize;
    cm.l1Latency = profile.l1DLatency;
    cm.l1LineSize = profile.cacheLineSize;
    cm.l2Size = profile.l2Size;
    cm.l2Latency = profile.l2Latency;
    cm.l3Size = profile.l3Size;
    cm.l3Latency = profile.l3Latency;
    cm.memLatency = profile.memoryLatency;
    // Rough bandwidth estimate: higher for wider memory buses.
    // This is approximate — real bandwidth depends on many factors.
    cm.memBandwidth = (profile.vectorWidth >= 512) ? 60.0
                    : (profile.vectorWidth >= 256) ? 40.0
                    : 25.0;
    return cm;
}

// ═════════════════════════════════════════════════════════════════════════════
// Cache-aware optimization pass
// ═════════════════════════════════════════════════════════════════════════════

/// Classify a memory access pattern from a GEP + loop structure.
static AccessPattern classifyAccess(llvm::GetElementPtrInst* gep) {
    if (!gep) return AccessPattern::Unknown;

    // Check if the last index is an induction variable (AddRec / simple add).
    // A single-index GEP with a loop-variant operand is sequential.
    // A GEP with constant stride is strided.
    llvm::Value* lastIdx = gep->getOperand(gep->getNumOperands() - 1);

    // If the index is a PHI or add-of-phi, it's likely sequential or strided.
    if (llvm::isa<llvm::PHINode>(lastIdx))
        return AccessPattern::Sequential;
    if (auto* binOp = llvm::dyn_cast<llvm::BinaryOperator>(lastIdx)) {
        if (binOp->getOpcode() == llvm::Instruction::Add ||
            binOp->getOpcode() == llvm::Instruction::Mul) {
            // If multiplied by a constant, it's strided.
            if (binOp->getOpcode() == llvm::Instruction::Mul &&
                llvm::isa<llvm::ConstantInt>(binOp->getOperand(1)))
                return AccessPattern::Strided;
            return AccessPattern::Sequential;
        }
    }
    return AccessPattern::Unknown;
}

/// Estimate the working set size (in bytes) for a loop body.
/// Counts distinct base pointers accessed via GEP and multiplies by
/// an estimated iteration count.
static unsigned estimateWorkingSet(llvm::BasicBlock& bb, unsigned elementSize) {
    std::set<llvm::Value*> bases;
    for (auto& inst : bb) {
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst)) {
            bases.insert(gep->getPointerOperand());
        }
    }
    // Each distinct array base contributes elementSize * estimatedTripCount.
    // Use a conservative estimate of 1024 iterations for unknown trip counts.
    return static_cast<unsigned>(bases.size()) * elementSize * 1024;
}

/// Insert cache-aware prefetch hints for strided loads in loop bodies.
/// Unlike the simpler insertPrefetches in hardware transforms, this version
/// uses the CacheModel to compute prefetch distances based on cache latencies.
static unsigned insertCacheAwarePrefetches(llvm::Function& func,
                                            const MicroarchProfile& /*profile*/,
                                            const CacheModel& cache) {
    unsigned count = 0;

    // Assign linear order to each basic block.
    std::unordered_map<llvm::BasicBlock*, unsigned> bbOrder;
    {
        unsigned ord = 0;
        for (auto& bb : func) bbOrder[&bb] = ord++;
    }

    for (auto& bb : func) {
        // Find loop headers (BBs with a back-edge predecessor).
        bool isLoopHeader = false;
        for (auto* pred : llvm::predecessors(&bb)) {
            if (bbOrder.count(pred) && bbOrder[pred] >= bbOrder[&bb]) {
                isLoopHeader = true;
                break;
            }
        }
        if (!isLoopHeader) continue;

        for (auto& inst : bb) {
            auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst);
            if (!load) continue;

            auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
            if (!gep) continue;

            AccessPattern pattern = classifyAccess(gep);
            if (pattern == AccessPattern::Unknown || pattern == AccessPattern::Random)
                continue;

            // Check precision of surrounding instructions — only insert
            // aggressive prefetches (longer distance) in fast regions.
            FPPrecision prec = getInstructionPrecision(load);

            // Compute prefetch distance based on cache model.
            // distance = (L2_latency / memory_throughput) * stride
            // For sequential access, use cache line size as stride.
            unsigned stride = cache.l1LineSize;
            unsigned distance;
            if (prec == FPPrecision::Strict) {
                // Conservative: prefetch 2 cache lines ahead.
                distance = 2 * stride;
            } else if (prec == FPPrecision::Fast) {
                // Aggressive: prefetch based on L2 latency.
                distance = std::max(4u, cache.l2Latency / 2) * stride;
            } else {
                // Medium: moderate prefetch distance.
                distance = std::max(3u, cache.l2Latency / 4) * stride;
            }

            // Build prefetch: __builtin_prefetch(ptr + distance, 0/*read*/, 3/*L1*/)
            llvm::IRBuilder<> builder(load);
            llvm::Value* ptr = load->getPointerOperand();
            llvm::Value* prefAddr = builder.CreateGEP(
                builder.getInt8Ty(), ptr,
                builder.getInt64(distance), "prefetch.addr");

            llvm::Function* prefetchFn = OMSC_GET_INTRINSIC(
                func.getParent(), llvm::Intrinsic::prefetch,
                {llvm::PointerType::getUnqual(func.getContext())});
            builder.CreateCall(prefetchFn, {
                prefAddr,
                builder.getInt32(0),  // read
                builder.getInt32(3),  // high temporal locality
                builder.getInt32(1)   // data cache
            });
            count++;
        }
    }

    return count;
}

/// Add loop tiling metadata hints for loops whose working set exceeds L1.
/// This doesn't transform the loop directly — it attaches metadata that
/// downstream passes (LLVM's LoopTiling or Polly) can use.
static unsigned addTilingHints(llvm::Function& func,
                                const CacheModel& cache) {
    unsigned count = 0;
    llvm::LLVMContext& ctx = func.getContext();

    // Assign linear order to each basic block.
    std::unordered_map<llvm::BasicBlock*, unsigned> bbOrder;
    {
        unsigned ord = 0;
        for (auto& bb : func) bbOrder[&bb] = ord++;
    }

    for (auto& bb : func) {
        // Find loop headers.
        llvm::BasicBlock* latch = nullptr;
        for (auto* pred : llvm::predecessors(&bb)) {
            if (bbOrder.count(pred) && bbOrder[pred] >= bbOrder[&bb]) {
                latch = pred;
                break;
            }
        }
        if (!latch) continue;

        // Skip if the loop already has metadata.
        auto* latchTerm = latch->getTerminator();
        if (!latchTerm || latchTerm->getMetadata(llvm::LLVMContext::MD_loop))
            continue;

        // Estimate working set.
        unsigned ws = estimateWorkingSet(bb, 8); // 8 bytes per element (i64/double)
        unsigned l1Bytes = cache.l1Size * 1024;

        // Only add tiling hint if working set exceeds L1.
        if (ws <= l1Bytes) continue;

        // Compute tile size: aim to fit working set in L1.
        // tileSize ≈ sqrt(l1Bytes / elementSize) for 2D arrays.
        unsigned tileSize = 1;
        for (unsigned t = 2; t <= 256; t *= 2) {
            if (t * t * 8 <= l1Bytes) tileSize = t;
            else break;
        }

        // Attach tiling metadata.
        llvm::SmallVector<llvm::Metadata*, 4> mds;
        mds.push_back(nullptr); // placeholder for self-reference
        mds.push_back(llvm::MDNode::get(ctx, {
            llvm::MDString::get(ctx, "omsc.cache.tile_size"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), tileSize))
        }));
        mds.push_back(llvm::MDNode::get(ctx, {
            llvm::MDString::get(ctx, "omsc.cache.working_set"),
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), ws))
        }));

        llvm::MDNode* loopID = llvm::MDNode::get(ctx, mds);
        loopID->replaceOperandWith(0, loopID);
        latchTerm->setMetadata(llvm::LLVMContext::MD_loop, loopID);
        ++count;
    }
    return count;
}

CacheOptStats optimizeCacheLocality(llvm::Function& func,
                                     const MicroarchProfile& profile,
                                     const CacheModel& cache) {
    CacheOptStats stats;
    if (func.isDeclaration()) return stats;

    // Phase 1: Propagate precision metadata through the function.
    propagatePrecision(func);

    // Phase 2: Insert cache-aware prefetches with precision-guided distances.
    stats.prefetchesInserted = insertCacheAwarePrefetches(func, profile, cache);

    // Phase 3: Add tiling hints for loops with large working sets.
    stats.loopsTiled = addTilingHints(func, cache);

    return stats;
}

} // namespace hgoe
} // namespace omscript
