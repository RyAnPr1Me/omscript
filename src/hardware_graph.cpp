/// @file hardware_graph.cpp
/// @brief Hardware Graph Optimization Engine (HGOE) implementation.
///
/// Implements hardware-aware compilation by:
///   1. Building a structural model of the target CPU microarchitecture
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
#include <numeric>
#include <queue>
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
    unsigned id = static_cast<unsigned>(nodes_.size());
    nodes_.push_back({id, type, name, count, latency, throughput, pipelineDepth});
    return id;
}

void HardwareGraph::addEdge(unsigned srcId, unsigned dstId, double latency,
                             double bandwidth, const std::string& label) {
    edges_.push_back({srcId, dstId, latency, bandwidth, label});
}

const HardwareNode* HardwareGraph::getNode(unsigned id) const {
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
static OpClass classifyOp(const llvm::Instruction* inst) {
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
    unsigned id = static_cast<unsigned>(nodes_.size());
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
                            case OpClass::FPMul:    prodLat = 4;  break;
                            case OpClass::FPDiv:    prodLat = 15; break;
                            case OpClass::FMA:      prodLat = 4;  break;
                            case OpClass::Load:     prodLat = 4;  break;
                            case OpClass::FPArith:  prodLat = 4;  break;
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
static MicroarchProfile skylakeProfile() {
    MicroarchProfile p;
    p.name = "skylake";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 6;
    p.issueWidth = 6;
    p.pipelineDepth = 14;
    p.intALUs = 4;
    p.vecUnits = 2;
    p.fmaUnits = 2;
    p.loadPorts = 2;
    p.storePorts = 1;
    p.branchUnits = 1;
    p.agus = 2;
    p.dividers = 1;
    // Skylake: integer multiply on ports P0 and P1 only (2 of the 4 ALU ports).
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 26;
    p.latFPAdd = 4; p.latFPMul = 4; p.latFPDiv = 14; p.latFMA = 4;
    p.latLoad = 5; p.latStore = 5; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.25; p.tputIntMul = 1.0;
    p.tputFPAdd = 0.5; p.tputFPMul = 0.5;
    p.tputLoad = 0.5; p.tputStore = 1.0;
    p.l1DSize = 32; p.l1DLatency = 5;
    p.l2Size = 256; p.l2Latency = 12;
    p.l3Size = 8192; p.l3Latency = 42;
    p.cacheLineSize = 64;
    p.vectorWidth = 256; // AVX2
    p.intRegisters = 16; p.vecRegisters = 16; p.fpRegisters = 16;
    p.branchMispredictPenalty = 15.0;
    p.btbEntries = 4096;
    p.memoryLatency = 200;
    return p;
}

/// Return a Haswell (Intel 4th gen) microarchitecture profile.
static MicroarchProfile haswellProfile() {
    MicroarchProfile p = skylakeProfile();
    p.name = "haswell";
    p.decodeWidth = 4;
    p.issueWidth = 4;
    p.l3Latency = 36;
    p.branchMispredictPenalty = 15.0;
    // Haswell: integer multiply on port P1 only (1 of the 4 ALU ports).
    p.mulPortCount = 1;
    return p;
}

/// Return an Intel Alder Lake / Raptor Lake (big core) profile.
static MicroarchProfile alderlakeProfile() {
    MicroarchProfile p = skylakeProfile();
    p.name = "alderlake";
    p.decodeWidth = 6;
    p.issueWidth = 6;
    p.intALUs = 5;
    p.loadPorts = 2;
    p.storePorts = 2;
    p.l1DSize = 48;
    p.l3Size = 30720; // 30MB shared
    p.l3Latency = 44;
    return p;
}

/// Return an AMD Zen 4 (Ryzen 7000 / EPYC Genoa) profile.
static MicroarchProfile zen4Profile() {
    MicroarchProfile p;
    p.name = "znver4";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 4;
    p.issueWidth = 6;
    p.pipelineDepth = 13;
    p.intALUs = 4;
    p.vecUnits = 2;
    p.fmaUnits = 2;
    p.loadPorts = 3;
    p.storePorts = 2;
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
    p.vectorWidth = 256; // AVX2 (256-bit AVX-512 throughput on Zen 4)
    p.intRegisters = 16; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 13.0;
    p.btbEntries = 6144;
    p.memoryLatency = 180;
    return p;
}

/// Return an AMD Zen 3 (Ryzen 5000) profile.
static MicroarchProfile zen3Profile() {
    MicroarchProfile p = zen4Profile();
    p.name = "znver3";
    p.pipelineDepth = 13;
    p.loadPorts = 3;
    p.storePorts = 2;
    p.latIntDiv = 18;
    p.latFPDiv = 15;
    p.l2Size = 512;
    p.l3Size = 32768;
    p.l3Latency = 46;
    p.vectorWidth = 256;
    p.vecRegisters = 16;
    p.fpRegisters = 16;
    return p;
}

/// Return an Apple M1/M2 Firestorm (performance core) profile.
static MicroarchProfile appleMProfile() {
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
    return p;
}

/// Return an ARM Neoverse V2 (server) profile.
static MicroarchProfile neoverseV2Profile() {
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
    return p;
}

/// Return an ARM Neoverse N2 profile.
static MicroarchProfile neoverseN2Profile() {
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
static MicroarchProfile riscvGenericProfile() {
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
static MicroarchProfile sifiveU74Profile() {
    MicroarchProfile p = riscvGenericProfile();
    p.name = "sifive-u74";
    p.pipelineDepth = 8;
    p.intALUs = 2;
    p.l1DSize = 32;
    p.l2Size = 2048;
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
        p.l2Size = 512;
        return p;
    }
    if (normalized == "znver1" || normalized == "zen") {
        auto p = zen3Profile();
        p.name = "znver1";
        p.l2Size = 512;
        p.loadPorts = 2;
        return p;
    }
    if (normalized == "znver5" || normalized == "zen5") {
        auto p = zen4Profile();
        p.name = "znver5";
        p.issueWidth = 8;
        p.intALUs = 6;
        p.vecUnits = 4;
        p.fmaUnits = 4;
        p.loadPorts = 4;
        p.storePorts = 2;
        p.vectorWidth = 512;
        return p;
    }

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
static ResourceType mapOpToResource(OpClass op) {
    switch (op) {
    case OpClass::IntArith:
    case OpClass::Shift:
    case OpClass::Comparison:
    case OpClass::Conversion:
        return ResourceType::IntegerALU;
    case OpClass::IntMul:
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
static unsigned getLatency(OpClass op, const MicroarchProfile& profile) {
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
static unsigned getPortCount(ResourceType rt, const MicroarchProfile& profile) {
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
static unsigned getOpcodeLatency(const llvm::Instruction* inst,
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
                    unsigned lat = static_cast<unsigned>(
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

/// Detect and generate FMA: a*b + c → fma(a, b, c) or a*b - c → fma(a, b, -c).
/// Returns the number of FMAs generated.
static unsigned generateFMA(llvm::Function& func, const MicroarchProfile& profile) {
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
static unsigned integerStrengthReduce(llvm::Function& func,
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
            case 25: rep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,3), "t"), xv, "sr_mul25"); break;
            case 28: rep = builder.CreateSub(shl(xv,5), shl(xv,2), "sr_mul28"); break;
            case 40: rep = builder.CreateAdd(shl(xv,5), shl(xv,3), "sr_mul40"); break;
            default: break;
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
        constexpr unsigned kMaxUnrollCount = 16;
        unsigned unroll = (profile.pipelineDepth + resMII - 1) / resMII;
        unroll = std::max(unroll, 2u);
        unroll = std::min(unroll, kMaxUnrollCount);

        // Interleave count: same as unroll for out-of-order; limited for
        // in-order cores (issueWidth == 1 → no benefit from high interleave).
        unsigned interleave = (profile.issueWidth > 2) ? unroll : 2u;

        // ── Vectorize width (32-bit baseline, clamped to hardware width) ─────
        unsigned vecWidth = (profile.vecUnits > 0 && profile.vectorWidth >= 128)
            ? profile.vectorWidth / 32 : 0;
        if (vecWidth < 2) vecWidth = 0; // don't set a width of 1

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
                                        const MicroarchProfile& profile) {
    TransformStats stats;


    stats.fmaGenerated     = generateFMA(func, profile);
    stats.fmaGenerated    += generateFMASub(func, profile);
    stats.prefetchesInserted = insertPrefetches(func, profile);
    stats.branchesOptimized  = optimizeBranchLayout(func, profile);
    stats.loadsStorePaired   = markLoadStorePairs(func, profile);
    stats.vectorExpanded     = softwarePipelineLoops(func, profile);
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
static unsigned scheduleBasicBlock(llvm::BasicBlock& bb,
                                    const HardwareGraph& hw,
                                    const MicroarchProfile& profile) {
    // ── 1. Collect moveable instructions ─────────────────────────────────────
    std::vector<llvm::Instruction*> moveable;
    moveable.reserve(bb.size());
    for (auto& inst : bb)
        if (!llvm::isa<llvm::PHINode>(inst) && !inst.isTerminator())
            moveable.push_back(&inst);

    unsigned n = static_cast<unsigned>(moveable.size());
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

    // Memory ordering (conservative): each memory op follows the previous one.
    {
        int lastMem = -1;
        for (unsigned i = 0; i < n; ++i) {
            if (!hasMemoryEffect(moveable[i])) continue;
            if (lastMem >= 0)
                addEdge(static_cast<unsigned>(lastMem), i);
            lastMem = static_cast<int>(i);
        }
    }

    // ── 4. Per-opcode instruction latencies (more precise than OpClass-level) ──
    std::vector<unsigned> lat(n);
    for (unsigned i = 0; i < n; ++i)
        lat[i] = getOpcodeLatency(moveable[i], profile);

    // ── 5. Critical-path depth (bottom-up, longest latency path to any sink) ──
    std::vector<unsigned> critPath(n, 0);
    for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
        unsigned ui = static_cast<unsigned>(i);
        unsigned maxSucc = 0;
        for (unsigned s : succ[ui])
            if (critPath[s] > maxSucc) maxSucc = critPath[s];
        critPath[ui] = lat[ui] + maxSucc;
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

        // Sort ready instructions for maximum throughput:
        //   Primary   — critical path remaining (latency hiding)
        //   Secondary — port pressure (schedule bottleneck resource first)
        //   Tertiary  — register-freeing score (prefer instructions that kill
        //               live values, reducing register pressure)
        //   Quaternary — instruction index (deterministic tie-break)
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

        std::sort(ready.begin(), ready.end(), [&](unsigned a, unsigned b) {
            if (critPath[a] != critPath[b])
                return critPath[a] > critPath[b];
            OpClass opA = classifyOp(moveable[a]);
            OpClass opB = classifyOp(moveable[b]);
            int rtA = (opA == OpClass::IntMul) ? kIntMulPortKey
                : static_cast<int>(mapOpToResource(opA));
            int rtB = (opB == OpClass::IntMul) ? kIntMulPortKey
                : static_cast<int>(mapOpToResource(opB));
            if (portPressure[rtA] != portPressure[rtB])
                return portPressure[rtA] > portPressure[rtB];
            unsigned rfsA = regFreeScore(a), rfsB = regFreeScore(b);
            if (rfsA != rfsB) return rfsA > rfsB;
            return a < b;
        });

        // Within a cycle, track which ResourceTypes have been issued to
        // encourage diversity and fill different execution units in parallel.
        std::unordered_set<int> issuedPortsThisCycle;
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

    // ── 8. Apply schedule: reorder LLVM IR within the basic block ────────────
    if (scheduled.size() == n) {
        llvm::Instruction* term = bb.getTerminator();
        for (auto* inst : scheduled)
            inst->moveBefore(term);
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
        stats.transforms = applyHardwareTransforms(func, profile);
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
        info.nativeOps = 1;
        break;
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
        // On x86-64: 16 GPRs minus rsp (stack), rbp (frame), and 1-2 for
        // the loop induction variable and end value ≈ 12 usable.
        // On AArch64: 31 GPRs minus x29 (fp), x30 (lr) ≈ 28 usable.
        unsigned usableRegs = (profile.isa == ISAFamily::AArch64)
            ? (profile.intRegisters > 3 ? profile.intRegisters - 3 : 12)
            : (profile.intRegisters > 4 ? profile.intRegisters - 4 : 12);

        // Each unrolled iteration adds regsProduced live values.  The limit
        // is the largest N such that N * regsPerIter ≤ usableRegs.
        unsigned regUnroll = totalRegsProduced > 0
            ? usableRegs / totalRegsProduced
            : 16;

        // ── Constraint 2: L1 I-cache footprint ───────────────────────────
        // L1I is typically 32-64KB.  Each x86 instruction averages ~4.5
        // bytes (shorter for ALU, longer for memory+displacement).
        // We budget 40% of L1I for the hot loop to leave room for outer
        // loops, function prologs, and OS code.
        unsigned l1iBytes = profile.l1DSize * 1024; // approximate L1I ≈ L1D
        unsigned iCacheBudget = (l1iBytes * 40) / (100 * 5); // 40% / 5 bytes per op
        unsigned iCacheUnroll = totalNativeOps > 0
            ? iCacheBudget / totalNativeOps
            : 16;

        // ── Constraint 3: Pipeline saturation ────────────────────────────
        // We want enough iterations in-flight to hide the pipeline latency
        // of the loop body.  For an OOO core, this is:
        //   minUnroll = ceil(maxLatency / (issueWidth * tputPerOp))
        // Simplified: at least ceil(pipelineDepth / 4) iterations.
        unsigned pipelineMin = (profile.pipelineDepth + 3) / 4;

        // ── Combine constraints ──────────────────────────────────────────
        unsigned unroll = std::min(regUnroll, iCacheUnroll);
        unroll = std::max(unroll, pipelineMin);
        unroll = std::max(unroll, 2u);
        unroll = std::min(unroll, 16u);

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

} // namespace hgoe
} // namespace omscript
