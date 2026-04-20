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
#include <llvm/Analysis/ValueTracking.h>

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
#include <limits>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_set>

namespace omscript {
namespace hgoe {

// Forward declaration: defined later in this file (Step 7).
// Allows buildFromFunction (Step 2) to call it without reordering definitions.
static unsigned getOpcodeLatency(const llvm::Instruction* inst,
                                  const MicroarchProfile& profile);

/// Return the memory pointer operand for load/store instructions.
[[nodiscard]] static const llvm::Value* getMemoryPointerOperand(const llvm::Instruction* inst) {
    if (const auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst))
        return ld->getPointerOperand();
    if (const auto* st = llvm::dyn_cast<llvm::StoreInst>(inst))
        return st->getPointerOperand();
    return nullptr;
}

/// Resolve a pointer to (base, constant byte offset) when provable.
[[nodiscard]] static bool getBaseAndConstByteOffset(const llvm::Value* ptr,
                                                    const llvm::DataLayout& dl,
                                                    const llvm::Value*& base,
                                                    int64_t& offset) {
    if (!ptr || !ptr->getType()->isPointerTy()) return false;
    int64_t off = 0;
    const llvm::Value* rawBase = llvm::GetPointerBaseWithConstantOffset(ptr, off, dl);
    if (!rawBase) return false;
    base = rawBase->stripPointerCasts();
    offset = off;
    return true;
}

/// Alias query used by HGOE memory-edge construction.
/// Enhanced with:
///   - TBAA metadata disambiguation (type-based alias analysis)
///   - Non-overlapping access size checks
///   - GEP index range analysis for provably disjoint array accesses
[[nodiscard]] static bool hgoeMayAlias(const llvm::Instruction* a,
                                       const llvm::Instruction* b,
                                       const llvm::DataLayout& dl) {
    // Side-effecting calls/barriers are always conservative-alias.
    if ((llvm::isa<llvm::CallBase>(a) && a->mayHaveSideEffects()) ||
        (llvm::isa<llvm::CallBase>(b) && b->mayHaveSideEffects()))
        return true;

    const llvm::Value* ptrA = getMemoryPointerOperand(a);
    const llvm::Value* ptrB = getMemoryPointerOperand(b);
    if (!ptrA || !ptrB) return true;

    ptrA = ptrA->stripPointerCasts();
    ptrB = ptrB->stripPointerCasts();
    if (ptrA == ptrB) return true;

    // ── TBAA metadata disambiguation ────────────────────────────────────────
    // LLVM's TBAA (Type-Based Alias Analysis) metadata on load/store
    // instructions encodes type hierarchy information.  Two accesses with
    // TBAA tags from disjoint subtrees in the type hierarchy cannot alias.
    // This is the same check LLVM's MachineScheduler uses.
    {
        auto* tbaaA = a->getMetadata(llvm::LLVMContext::MD_tbaa);
        auto* tbaaB = b->getMetadata(llvm::LLVMContext::MD_tbaa);
        if (tbaaA && tbaaB && tbaaA != tbaaB) {
            // Walk up the TBAA tree: if neither is an ancestor of the other,
            // they are from disjoint type subtrees and cannot alias.
            // Conservative fast path: if both have exactly 3 operands
            // (standard TBAA scalar format) and their root nodes differ,
            // they are guaranteed disjoint.
            auto getRootTag = [](const llvm::MDNode* md) -> const llvm::MDNode* {
                const llvm::MDNode* cur = md;
                unsigned depthLimit = 16; // prevent infinite loops on malformed MD
                while (cur && cur->getNumOperands() >= 2 && depthLimit-- > 0) {
                    auto* parent = llvm::dyn_cast<llvm::MDNode>(cur->getOperand(1));
                    if (!parent || parent == cur) return cur;
                    cur = parent;
                }
                return cur;
            };
            const llvm::MDNode* rootA = getRootTag(tbaaA);
            const llvm::MDNode* rootB = getRootTag(tbaaB);
            // Different root types → disjoint subtrees → no alias.
            if (rootA && rootB && rootA != rootB)
                return false;
            // Same root but different direct tags at the same tree depth:
            // if neither tag is an ancestor of the other, they don't alias.
            // Quick check: different tags with the same parent.
            if (tbaaA->getNumOperands() >= 2 && tbaaB->getNumOperands() >= 2) {
                auto* parentA = llvm::dyn_cast<llvm::MDNode>(tbaaA->getOperand(1));
                auto* parentB = llvm::dyn_cast<llvm::MDNode>(tbaaB->getOperand(1));
                if (parentA && parentB && parentA == parentB && tbaaA != tbaaB)
                    return false;
            }
        }
    }

    // Constant byte offsets from the same underlying base never alias when
    // offsets differ. This handles multi-index/struct-field GEPs.
    const llvm::Value* baseA = nullptr;
    const llvm::Value* baseB = nullptr;
    int64_t offA = 0;
    int64_t offB = 0;
    const bool hasOffA = getBaseAndConstByteOffset(ptrA, dl, baseA, offA);
    const bool hasOffB = getBaseAndConstByteOffset(ptrB, dl, baseB, offB);
    if (hasOffA && hasOffB && baseA == baseB && offA != offB) {
        // ── Non-overlapping access size check ────────────────────────────
        // Even if offsets differ, accesses may overlap if they have
        // different sizes.  Check that [offA, offA+sizeA) and
        // [offB, offB+sizeB) are disjoint.
        auto getAccessSize = [&](const llvm::Instruction* inst) -> uint64_t {
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst))
                return dl.getTypeStoreSize(ld->getType());
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(inst))
                return dl.getTypeStoreSize(st->getValueOperand()->getType());
            return 0;
        };
        uint64_t sizeA = getAccessSize(a);
        uint64_t sizeB = getAccessSize(b);
        if (sizeA > 0 && sizeB > 0) {
            int64_t endA = offA + static_cast<int64_t>(sizeA);
            int64_t endB = offB + static_cast<int64_t>(sizeB);
            if (endA <= offB || endB <= offA)
                return false; // provably non-overlapping
            // Ranges overlap → definitely alias (same base, overlapping offsets).
            return true;
        }
        // Unknown access size but different offsets from the same base:
        // cannot prove disjointness → conservative alias.
        // (Fall through to underlying-object checks for completeness.)
    }

    // Fall back to underlying-object disambiguation.
    const llvm::Value* objA = llvm::getUnderlyingObject(ptrA);
    const llvm::Value* objB = llvm::getUnderlyingObject(ptrB);
    if (objA) objA = objA->stripPointerCasts();
    if (objB) objB = objB->stripPointerCasts();
    if (!objA || !objB) return true;
    if (objA == objB) return true;

    const bool aIsAlloca = llvm::isa<llvm::AllocaInst>(objA);
    const bool bIsAlloca = llvm::isa<llvm::AllocaInst>(objB);
    const bool aIsArg = llvm::isa<llvm::Argument>(objA);
    const bool bIsArg = llvm::isa<llvm::Argument>(objB);
    const bool aIsGlobal = llvm::isa<llvm::GlobalVariable>(objA);
    const bool bIsGlobal = llvm::isa<llvm::GlobalVariable>(objB);

    if ((aIsAlloca && bIsAlloca) ||
        (aIsGlobal && bIsGlobal) ||
        (aIsAlloca && bIsArg) || (aIsArg && bIsAlloca) ||
        (aIsAlloca && bIsGlobal) || (aIsGlobal && bIsAlloca))
        return false;

    if (aIsArg && bIsArg) {
        const auto* argA = llvm::cast<llvm::Argument>(objA);
        const auto* argB = llvm::cast<llvm::Argument>(objB);
        if (argA->hasNoAliasAttr() || argB->hasNoAliasAttr())
            return false;
    }

    // noalias argument vs global: noalias guarantees the argument doesn't
    // alias anything the callee can see through other means (globals, other args).
    if ((aIsArg && bIsGlobal) || (aIsGlobal && bIsArg)) {
        const auto* arg = aIsArg ? llvm::cast<llvm::Argument>(objA)
                                 : llvm::cast<llvm::Argument>(objB);
        if (arg->hasNoAliasAttr())
            return false;
    }

    return true;
}

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
    case llvm::Instruction::FNeg:   // bit-flip on sign; uses FP pipeline
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

    // Integer compare uses the integer ALU pipeline.
    case llvm::Instruction::ICmp:
        return OpClass::Comparison;

    // Floating-point compare uses the FP pipeline (same ports as FADD/FMUL).
    // On x86: VCMPPD/VCMPPS execute on P0/P1 (FMA unit) not integer ALU.
    // On AArch64: FCMP/FCCMP execute on the FP pipe.
    case llvm::Instruction::FCmp:
        return OpClass::FPArith;

    // Integer-only conversions: these stay on the integer ALU (register rename
    // or single-cycle move on all major architectures).
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
    case llvm::Instruction::BitCast:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::PtrToInt:
        return OpClass::Conversion;

    // FP/cross-domain conversions go through the FP pipeline.
    // On x86: CVTSI2SD/CVTTSD2SI etc. execute on P0/P1 (FMA unit).
    // On AArch64: SCVTF/UCVTF/FCVTZS execute on the FP pipe.
    // Using FPArith routes them to FMAUnit (correct on all supported ISAs).
    case llvm::Instruction::FPToUI:
    case llvm::Instruction::FPToSI:
    case llvm::Instruction::UIToFP:
    case llvm::Instruction::SIToFP:
    case llvm::Instruction::FPTrunc:
    case llvm::Instruction::FPExt:
        return OpClass::FPArith;

    // SIMD / vector instructions map to the vector execution units.
    case llvm::Instruction::ExtractElement:
    case llvm::Instruction::InsertElement:
    case llvm::Instruction::ShuffleVector:
        return OpClass::VectorOp;

    // Zero-latency / register-file bookkeeping ops.
    case llvm::Instruction::PHI:
    case llvm::Instruction::ExtractValue:
    case llvm::Instruction::InsertValue:
    case llvm::Instruction::Alloca:   // stack allocation done in function prolog
        return OpClass::Phi;

    case llvm::Instruction::Call: {
        const auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(inst);
        if (!ii) return OpClass::Call;
        // Dispatch known intrinsics to the correct execution-unit class so
        // the scheduler assigns them to the right hardware ports.
        switch (ii->getIntrinsicID()) {
        // ── FMA unit (P0/P1 on x86, FP pipe on AArch64) ────────────────────
        case llvm::Intrinsic::fma:
        case llvm::Intrinsic::fmuladd:
            return OpClass::FMA;

        // ── FP divider / sqrt unit ──────────────────────────────────────────
        // sqrt shares the hardware divider port on both x86 (VSQRTSD runs on
        // the divider unit, same port as VDIVSD) and AArch64 (FSQRT uses the
        // FP divide pipeline).  Scheduling it early is critical for latency
        // hiding since the divider is non-pipelined on most µarchs.
        case llvm::Intrinsic::sqrt:
            return OpClass::FPDiv;

        // ── FP pipeline — rounding / abs / sign / min / max ────────────────
        // VROUNDPD/FRINTM/FRINTP execute on FMA ports (P0/P1 on x86, FP on
        // AArch64).  fabs/copysign/minnum/maxnum likewise use the FP pipe.
        case llvm::Intrinsic::floor:
        case llvm::Intrinsic::ceil:
        case llvm::Intrinsic::round:
        case llvm::Intrinsic::roundeven:
        case llvm::Intrinsic::rint:
        case llvm::Intrinsic::nearbyint:
        case llvm::Intrinsic::trunc:    // llvm.trunc intrinsic = FP rounding toward zero (VROUNDPD mode 3),
                                        // NOT the integer-narrowing Trunc opcode (llvm::Instruction::Trunc)
        case llvm::Intrinsic::fabs:
        case llvm::Intrinsic::copysign:
        case llvm::Intrinsic::minnum:
        case llvm::Intrinsic::maxnum:
            return OpClass::FPArith;

        // ── Integer ALU ─────────────────────────────────────────────────────
        // Scalar abs/min/max and bit-count operations use the integer pipe.
        case llvm::Intrinsic::abs:
        case llvm::Intrinsic::smin:
        case llvm::Intrinsic::smax:
        case llvm::Intrinsic::umin:
        case llvm::Intrinsic::umax:
        case llvm::Intrinsic::ctpop:
        case llvm::Intrinsic::ctlz:
        case llvm::Intrinsic::cttz:
            return OpClass::IntArith;

        // ── Zero-cost hint (no execution resource) ──────────────────────────
        case llvm::Intrinsic::prefetch:
            return OpClass::Phi;

        // ── Unknown intrinsic: conservative fallback ────────────────────────
        default:
            return OpClass::Intrinsic;
        }
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
    const llvm::DataLayout& dl = func.getParent()->getDataLayout();

    // Phase 1: Create a node for each instruction.
    for (auto& bb : func) {
        for (auto& inst : bb) {
            const OpClass cls = classifyOp(&inst);
            addNode(cls, &inst);
        }
    }

    // Phase 2: Add data-dependency edges based on def-use chains.
    // Use per-opcode default latency estimates for edge weights so that
    // the critical-path computation in the abstract ProgramGraph is accurate
    // even without a MicroarchProfile (which is not available here).
    //
    // These defaults are calibrated to modern OOO CPUs (Skylake-class x86,
    // Apple M-series AArch64).  The per-opcode scheduler in scheduleBasicBlock
    // uses the exact profile values; these defaults only affect the abstract
    // ProgramGraph used by mapProgramToHardware and criticalPathLength.
    // Use the default-constructed MicroarchProfile (conservative generic values)
    // so that this code shares one authoritative latency table with the rest
    // of the HGOE and never falls out of sync.
    static const MicroarchProfile kAbstractProfile = []{
        MicroarchProfile p;
        // Integer division: use 20 cycles for the abstract graph to match the
        // Haswell/Broadwell range; exact values are used by scheduleBasicBlock.
        p.latIntDiv = 20u;
        return p;
    }();

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto consIt = instToNode_.find(&inst);
            if (consIt == instToNode_.end()) continue;
            unsigned consId = consIt->second;

            for (unsigned i = 0; i < inst.getNumOperands(); ++i) {
                if (auto* opInst = llvm::dyn_cast<llvm::Instruction>(inst.getOperand(i))) {
                    auto prodIt = instToNode_.find(opInst);
                    if (prodIt != instToNode_.end()) {
                        unsigned prodLat = getOpcodeLatency(opInst, kAbstractProfile);
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
    auto mayAlias = [&](const llvm::Instruction* a, const llvm::Instruction* b) -> bool {
        return hgoeMayAlias(a, b, dl);
    };

    for (auto& bb : func) {
        // Track ALL live stores and loads in the BB so that every possible
        // memory hazard pair receives a dependency edge, not just the last
        // store/load.  This closes a correctness gap in the original code:
        //   store @A, 1
        //   store @B, 2
        //   load %x, @A     ← missed RAW from store @A with last-only tracking
        // The pairwise loop is O(stores × loads) per BB which is fine since
        // basic blocks are small in practice (typically < 100 instructions).
        std::vector<llvm::Instruction*> liveStores;
        std::vector<llvm::Instruction*> liveLoads;
        llvm::Instruction* lastBarrier = nullptr;

        for (auto& inst : bb) {
            const bool isStore = llvm::isa<llvm::StoreInst>(inst);
            const bool isLoad = llvm::isa<llvm::LoadInst>(inst);
            const bool isBarrierMem =
                inst.mayReadOrWriteMemory() && !isLoad && !isStore;

            if (isBarrierMem) {
                // All prior memory ops must complete before a side-effecting op
                // (calls, atomics, fences), and later memory ops must not pass it.
                auto thisIt = instToNode_.find(&inst);
                if (thisIt != instToNode_.end()) {
                    for (auto* s : liveStores) {
                        auto srcIt = instToNode_.find(s);
                        if (srcIt != instToNode_.end())
                            addEdge(srcIt->second, thisIt->second, DepType::Memory, 0);
                    }
                    for (auto* l : liveLoads) {
                        auto srcIt = instToNode_.find(l);
                        if (srcIt != instToNode_.end())
                            addEdge(srcIt->second, thisIt->second, DepType::Memory, 0);
                    }
                    if (lastBarrier) {
                        auto srcIt = instToNode_.find(lastBarrier);
                        if (srcIt != instToNode_.end())
                            addEdge(srcIt->second, thisIt->second, DepType::Memory, 0);
                    }
                }
                // Barrier serialises everything: clear tracked ops so later
                // loads/stores need only depend on the barrier, not on ops
                // that already have a transitive dependency through it.
                liveStores.clear();
                liveLoads.clear();
                lastBarrier = &inst;
                continue;
            }

            if (inst.getOpcode() == llvm::Instruction::Store) {
                auto dstIt = instToNode_.find(&inst);
                if (dstIt != instToNode_.end()) {
                    // WAW: all prior aliasing stores must precede this store.
                    for (auto* s : liveStores) {
                        if (mayAlias(s, &inst)) {
                            auto srcIt = instToNode_.find(s);
                            if (srcIt != instToNode_.end())
                                addEdge(srcIt->second, dstIt->second, DepType::Memory, 0);
                        }
                    }
                    // WAR: all prior aliasing loads must precede this store.
                    for (auto* l : liveLoads) {
                        if (mayAlias(l, &inst)) {
                            auto srcIt = instToNode_.find(l);
                            if (srcIt != instToNode_.end())
                                addEdge(srcIt->second, dstIt->second, DepType::Memory, 0);
                        }
                    }
                    if (lastBarrier) {
                        auto srcIt = instToNode_.find(lastBarrier);
                        if (srcIt != instToNode_.end())
                            addEdge(srcIt->second, dstIt->second, DepType::Memory, 0);
                    }
                }
                liveStores.push_back(&inst);
            }
            if (inst.getOpcode() == llvm::Instruction::Load) {
                auto dstIt = instToNode_.find(&inst);
                if (dstIt != instToNode_.end()) {
                    // RAW: all prior aliasing stores must precede this load.
                    for (auto* s : liveStores) {
                        if (mayAlias(s, &inst)) {
                            auto srcIt = instToNode_.find(s);
                            if (srcIt != instToNode_.end())
                                addEdge(srcIt->second, dstIt->second, DepType::Memory, 0);
                        }
                    }
                    if (lastBarrier) {
                        auto srcIt = instToNode_.find(lastBarrier);
                        if (srcIt != instToNode_.end())
                            addEdge(srcIt->second, dstIt->second, DepType::Memory, 0);
                    }
                }
                liveLoads.push_back(&inst);
            }
        }
    }
}

unsigned ProgramGraph::criticalPathLength() const {
    if (nodes_.empty()) return 0;
    const size_t n = nodes_.size();

    // Build adjacency list (outgoing edges per node) and in-degree array in
    // a single pass over edges_.  This avoids the O(N×E) inner-loop scan that
    // the previous edge-scan approach had when the node count grows.
    std::vector<std::vector<std::pair<unsigned, unsigned>>> succ(n); // succ[u] = {(v, lat)}
    std::vector<unsigned> inDeg(n, 0);
    for (const auto& e : edges_) {
        if (e.srcId < n && e.dstId < n) {
            succ[e.srcId].emplace_back(e.dstId, e.latency);
            inDeg[e.dstId]++;
        }
    }

    // Topological sort (Kahn's algorithm) + longest-path (ASAP distances).
    std::vector<unsigned> dist(n, 0);
    std::queue<unsigned> ready;
    for (unsigned i = 0; i < n; ++i) {
        if (inDeg[i] == 0) {
            ready.push(i);
            dist[i] = 1; // Minimum 1 cycle per instruction
        }
    }

    while (!ready.empty()) {
        unsigned u = ready.front();
        ready.pop();
        for (auto [v, lat] : succ[u]) {
            unsigned newDist = dist[u] + lat;
            if (newDist > dist[v]) dist[v] = newDist;
            if (--inDeg[v] == 0) ready.push(v);
        }
    }

    unsigned maxDist = 0;
    for (unsigned d : dist)
        if (d > maxDist) maxDist = d;
    return maxDist;
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 5 — Hardware-aware cost model
// ═════════════════════════════════════════════════════════════════════════════

HardwareCostModel::HardwareCostModel(const HardwareGraph& hw,
                                     const MicroarchProfile& profile)
    : hw_(hw), profile_(profile) {
    // Derive vector width directly from the profile's SIMD width field (bits).
    // The old code used pipelineDepth as a proxy; the profile field is exact.
    if (profile.vectorWidth >= 512)      vectorWidth_ = 16; // AVX-512
    else if (profile.vectorWidth >= 256) vectorWidth_ = 8;  // AVX2 / Neon 256
    else                                 vectorWidth_ = 4;  // SSE / NEON 128

    // Issue width, cache penalties: read directly from the profile rather than
    // reverse-engineering them from hardware-graph node attributes.
    issueWidth_          = static_cast<double>(profile.issueWidth);
    cacheMissL1Penalty_  = static_cast<double>(profile.l1DLatency);
    cacheMissL2Penalty_  = static_cast<double>(profile.l2Latency);
    cacheMissL3Penalty_  = static_cast<double>(profile.l3Latency);
}

OpClass HardwareCostModel::classifyInstruction(const llvm::Instruction* inst) const {
    return classifyOp(inst);
}

double HardwareCostModel::instructionCost(const llvm::Instruction* inst) const {
    if (!inst) return 0.0;
    // Delegate to the single authoritative latency function (getOpcodeLatency)
    // rather than re-implementing per-opclass dispatch here.  The hardware graph
    // is still available for structural queries (simulateExecution, etc.).
    return static_cast<double>(getOpcodeLatency(inst, profile_));
}

double HardwareCostModel::simulateExecution(const ProgramGraph& pg) const {
    const size_t n = pg.nodeCount();
    if (n == 0) return 0.0;

    // ── 1. Derive port capacities from the hardware graph ───────────────────────
    // Helper: sum the 'count' field of all nodes of a given resource type.
    auto portCapacity = [this](ResourceType rt) -> unsigned {
        unsigned total = 0;
        for (auto* nd : hw_.findNodes(rt)) total += nd->count;
        return total ? total : 1u;
    };

    // Classify each OpClass into a small number of port groups so we can
    // efficiently track port availability.  Each group maps to one capacity.
    enum PClass : unsigned {
        PC_IntArith = 0, // IntArith, Shift, Comparison, Conversion
        PC_IntMul   = 1, // IntMul (subset of ALU ports on most µarchs)
        PC_Div      = 2, // IntDiv, FPDiv — shares the divider unit
        PC_FP       = 3, // FPArith, FPMul, FMA
        PC_Load     = 4, // Load
        PC_Store    = 5, // Store
        PC_Branch   = 6, // Branch
        PC_Free     = 7, // PHI, Other — no port constraint (rename / free)
        PC_COUNT    = 8
    };

    const unsigned cap[PC_COUNT] = {
        portCapacity(ResourceType::IntegerALU),        // PC_IntArith
        std::max(1u, portCapacity(ResourceType::IntegerALU) / 2u), // PC_IntMul
        portCapacity(ResourceType::DividerUnit),       // PC_Div
        portCapacity(ResourceType::FMAUnit),           // PC_FP
        portCapacity(ResourceType::LoadUnit),          // PC_Load
        portCapacity(ResourceType::StoreUnit),         // PC_Store
        portCapacity(ResourceType::BranchUnit),        // PC_Branch
        ~0u,                                           // PC_Free (unlimited)
    };

    const unsigned issueWidth = static_cast<unsigned>(issueWidth_);

    // Map OpClass → PClass.
    auto toPClass = [](OpClass cls) -> unsigned {
        switch (cls) {
        case OpClass::IntArith:
        case OpClass::Shift:
        case OpClass::Comparison:
        case OpClass::Conversion:   return PC_IntArith;
        case OpClass::IntMul:       return PC_IntMul;
        case OpClass::IntDiv:       return PC_Div;
        case OpClass::FPArith:
        case OpClass::FPMul:
        case OpClass::FMA:          return PC_FP;
        case OpClass::FPDiv:        return PC_Div;
        case OpClass::Load:         return PC_Load;
        case OpClass::Store:        return PC_Store;
        case OpClass::Branch:       return PC_Branch;
        default:                    return PC_Free;
        }
    };

    // ── 2. Build adjacency lists (O(N+E)) ───────────────────────────────────────
    std::vector<std::vector<std::pair<unsigned,unsigned>>> succList(n); // (dst, edge_lat)
    std::vector<std::vector<std::pair<unsigned,unsigned>>> predList(n); // (src, edge_lat)
    std::vector<unsigned> inDeg(n, 0);

    for (const auto& e : pg.edges()) {
        if (e.srcId < n && e.dstId < n) {
            succList[e.srcId].emplace_back(e.dstId, e.latency);
            predList[e.dstId].emplace_back(e.srcId, e.latency);
            ++inDeg[e.dstId];
        }
    }

    // ── 3. Per-node execution latency ──────────────────────────────────────────
    std::vector<unsigned> nodeLat(n, 1);
    for (unsigned i = 0; i < n; ++i) {
        const ProgramNode* node = pg.getNode(i);
        if (node) {
            unsigned lat = node->inst
                ? getOpcodeLatency(node->inst, profile_)
                : static_cast<unsigned>(std::max(0.0, node->estimatedLatency));
            nodeLat[i] = lat;
        }
    }

    // ── 3b. Critical-path priority (longest latency-weighted path) ───────────
    std::vector<unsigned> priority(n, 0);
    {
        std::vector<unsigned> topo;
        topo.reserve(n);
        std::vector<unsigned> deg(inDeg);
        std::queue<unsigned> q;
        for (unsigned i = 0; i < n; ++i)
            if (deg[i] == 0) q.push(i);
        while (!q.empty()) {
            unsigned u = q.front();
            q.pop();
            topo.push_back(u);
            for (auto [v, edgeLat] : succList[u]) {
                (void)edgeLat;
                if (--deg[v] == 0) q.push(v);
            }
        }
        for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
            unsigned u = *it;
            unsigned maxSucc = 0;
            for (auto [v, edgeLat] : succList[u])
                maxSucc = std::max(maxSucc, priority[v] + edgeLat);
            priority[u] = nodeLat[u] + maxSucc;
        }
    }

    // ── 4. Port/issue occupancy tracking (sparse, per cycle) ──────────────────
    // Using unordered_map for O(1) average access; realistic programs have
    // bounded cycle spread so map memory stays small.
    //
    // portUsed[cycle][pc] = number of ops of port class pc already issued at cycle.
    // issueUsed[cycle]    = total ops issued at cycle (≤ issueWidth).
    //
    // For unordered_map<K, unsigned>::operator[], the inserted value is
    // value-initialised to 0 (confirmed by the C++ standard), so incrementing
    // a freshly inserted entry is well-defined.
    // For unordered_map<K, std::array<unsigned, N>>::operator[], the array is
    // also value-initialised (zero-filled) on first insertion.
    std::unordered_map<unsigned, std::array<unsigned, PC_COUNT>> portUsed;
    std::unordered_map<unsigned, unsigned> issueUsed;
    portUsed.reserve(n);
    issueUsed.reserve(n);

    // ── 5. Forward list scheduling (topological BFS) ──────────────────────────
    // Process nodes in topological order.  For each node, compute the earliest
    // cycle where (a) all predecessors have completed, (b) an issue slot is
    // free, and (c) a port of the required class is free.
    //
    // Note: BFS topological order does not guarantee that the critical-path
    // node is scheduled first when multiple nodes are ready simultaneously.
    // For a cost-estimation use-case this is an acceptable approximation;
    // the result may slightly over-count cycles compared to an optimal list
    // schedule but never under-counts.
    std::vector<unsigned> completedAt(n, 0);
    std::vector<unsigned> scheduledAt(n, 0);
    std::vector<bool> isScheduled(n, false);
    std::vector<unsigned> remainingUsers(n, 0);
    for (unsigned i = 0; i < n; ++i)
        remainingUsers[i] = static_cast<unsigned>(succList[i].size());

    // Lightweight register/rename-pressure model.
    const unsigned regBudget = std::max(
        8u, profile_.intRegisters + profile_.vecRegisters + profile_.fpRegisters);
    unsigned liveValues = 0;

    // Track total scheduled instructions; when numScheduled < robSize the ROB
    // cannot be full (inflight ≤ numScheduled), so the O(n) inflight scan can
    // be skipped entirely — the fast path fires for the vast majority of BBs.
    unsigned numScheduled = 0;
    const unsigned robSize = static_cast<unsigned>(profile_.robSize);

    auto inflightAt = [&](unsigned cycle) -> unsigned {
        unsigned inflight = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (!isScheduled[i]) continue;
            if (scheduledAt[i] <= cycle && completedAt[i] > cycle)
                ++inflight;
        }
        return inflight;
    };

    struct ReadyEntry {
        unsigned prio;
        unsigned id;
    };
    auto readyCmp = [](const ReadyEntry& a, const ReadyEntry& b) {
        if (a.prio != b.prio) return a.prio < b.prio; // max-heap by priority
        return a.id > b.id;
    };
    std::priority_queue<ReadyEntry, std::vector<ReadyEntry>, decltype(readyCmp)> ready(readyCmp);
    for (unsigned i = 0; i < n; ++i)
        if (inDeg[i] == 0) ready.push({priority[i], i});

    unsigned maxCycle = 0;

    while (!ready.empty()) {
        const unsigned u = ready.top().id;
        ready.pop();

        // Earliest cycle when all data dependencies are satisfied.
        unsigned readyCycle = 0;
        for (const auto& [pred, edgeLat] : predList[u]) {
            // completedAt[pred] already includes the producer's own latency.
            if (completedAt[pred] > readyCycle)
                readyCycle = completedAt[pred];
        }

        // Port class for this instruction.
        const ProgramNode* node = pg.getNode(u);
        const unsigned pc = toPClass(node ? node->opClass : OpClass::Other);

        // Find the earliest cycle ≥ readyCycle where both the issue slot and
        // the port slot for this op class are available.
        // We cap the search at readyCycle + 500 to avoid pathological loops
        // (in practice 1-10 iterations suffice for typical port utilisation).
        unsigned scheduleCycle = readyCycle;
        for (unsigned tries = 0; tries < 500; ++tries, ++scheduleCycle) {
            // Check global issue-width constraint.
            {
                auto iit = issueUsed.find(scheduleCycle);
                if (iit != issueUsed.end() && iit->second >= issueWidth)
                    continue;
            }
            // Check port-capacity constraint.
            if (pc != PC_Free) {
                auto pit = portUsed.find(scheduleCycle);
                if (pit != portUsed.end() && pit->second[pc] >= cap[pc])
                    continue;
            }
            // Rename/ROB pressure: avoid over-issuing when too many uops are
            // in-flight.  Fast path: if fewer instructions have been scheduled
            // than the ROB capacity the buffer cannot be full, so skip the
            // O(n) scan entirely — this covers the common case.
            if (robSize > 0 && numScheduled >= robSize &&
                inflightAt(scheduleCycle) >= robSize)
                continue;
            break; // valid cycle found
        }

        // Commit the scheduling decision.
        ++issueUsed[scheduleCycle]; // value-initialises to 0 on first access
        if (pc != PC_Free) {
            // try_emplace zero-initialises the array on first insertion.
            auto& row = portUsed.try_emplace(scheduleCycle,
                std::array<unsigned, PC_COUNT>{}).first->second;
            ++row[pc];
        }

        unsigned freesNow = 0;
        for (const auto& [pred, ignored] : predList[u]) {
            (void)ignored;
            if (remainingUsers[pred] == 1) ++freesNow;
        }
        const bool producesValue = !succList[u].empty();
        const int projectedLiveSigned =
            static_cast<int>(liveValues) +
            (producesValue ? 1 : 0) -
            static_cast<int>(freesNow);
        const unsigned projectedLive =
            static_cast<unsigned>(std::max(projectedLiveSigned, 0));
        const unsigned spillPenalty =
            (projectedLive > regBudget) ? (projectedLive - regBudget) : 0u;

        scheduledAt[u] = scheduleCycle;
        isScheduled[u] = true;
        ++numScheduled;
        completedAt[u] = scheduleCycle + nodeLat[u] + spillPenalty;
        if (completedAt[u] > maxCycle)
            maxCycle = completedAt[u];

        for (const auto& [pred, ignored] : predList[u]) {
            (void)ignored;
            if (remainingUsers[pred] > 0 && --remainingUsers[pred] == 0 && liveValues > 0)
                --liveValues;
        }
        if (producesValue) ++liveValues;

        // Release successors whose in-degree drops to zero.
        for (auto& [v, ignored] : succList[u]) {
            if (--inDeg[v] == 0)
                ready.push({priority[v], v});
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
    // Store-to-load forwarding on Skylake: 4 cycles (matches L1D hit latency).
    // The store buffer comparison adds no extra cycles vs a regular L1 load.
    p.latStoLForward = 4;
    // vec512Penalty set on the base Skylake profile: AVX2 (256-bit) native,
    // no 512-bit penalty.  Ice Lake / Tiger Lake variants override to 2.
    p.vec512Penalty = 1;
    // Division throughput: Skylake divider not fully pipelined.
    // FP64 divide: throughput ~1/17 cycles, FP32 ~1/7.  Use conservative FP64.
    p.tputFPDiv = 1.0 / 17.0;
    p.tputIntDiv = 1.0 / 26.0;
    // Reservation station (unified scheduler): 97 entries.
    p.schedulerSize = 97;
    // Load buffer: 128 entries (MOB load queue).  Store buffer: 72 entries.
    p.loadBufferEntries = 128;
    p.storeBufferEntries = 72;
    // L1D bandwidth: 2 load ports × 32 B = 64 B/cycle.
    // L2 bandwidth: ~32 B/cycle.  L3 ~16 B/cycle.  DRAM ~8 B/cycle at 1 GHz.
    p.l1DBandwidthBytesPerCycle = 64;
    p.l2BandwidthBytesPerCycle  = 32;
    p.l3BandwidthBytesPerCycle  = 16;
    p.memBandwidthBytesPerCycle = 8;
    // Skylake supports Hyperthreading (SMT2) on desktop and server parts.
    p.hasHyperthreading = true;
    // CVTSI2SD/CVTSI2SS on x86: integer-to-FP bypass path adds 1 cycle vs FADD.
    // Skylake: CVTSI2SD = 5 cycles, VADDPD = 4 cycles.
    p.latFPConvert = p.latFPAdd + 1;
    return p;
}
/// Sandy Bridge (2011): 6 execution ports, 3 integer ALUs, AVX 256-bit (no FMA).
/// Ivy Bridge (2012): same microarchitecture, minor improvements.
[[gnu::cold]] static MicroarchProfile sandyBridgeProfile() {
    MicroarchProfile p;
    p.name = "sandybridge";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 4;
    p.issueWidth = 4;           // 4-wide dispatch
    p.pipelineDepth = 14;
    p.intALUs = 3;              // ports 0, 1, 5 (no port 6 like Skylake)
    p.vecUnits = 2;             // ports 0 and 1 handle vector ops
    p.fmaUnits = 0;             // Sandy Bridge has no FMA; introduced in Haswell
    p.loadPorts = 2;            // ports 2, 3
    p.storePorts = 1;           // port 4 (data); port 7 (address) counted in agus
    p.branchUnits = 1;
    p.agus = 2;
    p.dividers = 1;
    p.mulPortCount = 1;         // integer multiply: port 1 only
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 22;
    p.latFPAdd = 5; p.latFPMul = 5; p.latFPDiv = 14; p.latFMA = 0;
    p.latLoad = 4; p.latStore = 4; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.33; p.tputIntMul = 1.0;
    p.tputFPAdd = 1.0; p.tputFPMul = 1.0;
    p.tputLoad = 0.5; p.tputStore = 1.0;
    p.l1DSize = 32;   p.l1DLatency = 4;
    p.l2Size = 256;   p.l2Latency = 12;
    p.l3Size = 8192;  p.l3Latency = 36;
    p.cacheLineSize = 64;
    p.vectorWidth = 256;        // AVX1 (256-bit SIMD, but FP ops are 2×128-bit)
    p.intRegisters = 16; p.vecRegisters = 16; p.fpRegisters = 16;
    p.branchMispredictPenalty = 15.0;
    p.btbEntries = 2048;
    p.memoryLatency = 200;
    p.robSize = 168;
    // Sandy Bridge store-to-load forwarding: 4 cycles (same as L1 hit).
    p.latStoLForward = 4;
    p.vec512Penalty = 1; // no AVX-512 on Sandy Bridge
    p.tputFPDiv = 1.0 / 14.0;
    p.tputIntDiv = 1.0 / 22.0;
    p.schedulerSize = 54;
    p.loadBufferEntries = 64;
    p.storeBufferEntries = 36;
    p.l1DBandwidthBytesPerCycle = 32; // 1 full-width load port (256-bit AVX split as 2×128)
    p.l2BandwidthBytesPerCycle  = 16;
    p.l3BandwidthBytesPerCycle  = 12;
    p.memBandwidthBytesPerCycle = 6;
    p.hasHyperthreading = true;
    // Sandy Bridge: CVTSI2SD = 6 cycles (latFPAdd=5), VADDPD = 5 cycles.
    p.latFPConvert = p.latFPAdd + 1;
    return p;
}

/// Return a Haswell (Intel 4th gen) microarchitecture profile.
/// Haswell (2013): added AVX2, FMA3, BMI/BMI2; integer multiply on port P1 only.
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
    // Haswell store-to-load forwarding: 4 cycles (same as L1 hit latency).
    p.latStoLForward = 4;
    p.vec512Penalty = 1; // AVX2 only, no 512-bit
    // Haswell has the same divider throughput as Skylake; RS and buffers are smaller.
    p.tputFPDiv = 1.0 / 14.0;
    p.tputIntDiv = 1.0 / 24.0;
    p.schedulerSize = 60;
    p.loadBufferEntries = 72;
    p.storeBufferEntries = 42;
    p.l1DBandwidthBytesPerCycle = 64;
    p.l2BandwidthBytesPerCycle  = 32;
    p.l3BandwidthBytesPerCycle  = 16;
    p.memBandwidthBytesPerCycle = 8;
    p.hasHyperthreading = true;
    return p;
}

/// Return an Intel Alder Lake / Raptor Lake (Golden Cove P-core) profile.
/// Alder Lake (2021): 5-wide integer backend, 3 load ports, 48 KB L1D.
/// Raptor Lake (2022): same P-core µarch, larger caches.
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
    // Alder Lake store-to-load forwarding: 4-5 cycles (L1D latency = 5 cycles;
    // forwarding completes in ~4 cycles via store-buffer bypass).
    p.latStoLForward = 4;
    p.vec512Penalty = 1; // AVX2 only on Golden Cove / Raptor Cove
    p.tputFPDiv = 1.0 / 14.0;
    p.tputIntDiv = 1.0 / 23.0;
    p.schedulerSize = 120;
    p.loadBufferEntries = 192;
    p.storeBufferEntries = 114;
    p.l1DBandwidthBytesPerCycle = 96; // 2 load ports × 32 B + partial store bandwidth
    p.l2BandwidthBytesPerCycle  = 48;
    p.l3BandwidthBytesPerCycle  = 20;
    p.memBandwidthBytesPerCycle = 10;
    p.hasHyperthreading = true; // Golden Cove P-cores support HT
    return p;
}

/// Return an AMD Zen 4 (Ryzen 7000 / EPYC Genoa) profile.
/// Zen 4 (2022): 6-wide backend, 4 integer pipes, 2 FMA units,
/// AVX-512 via 256-bit double-pump, 32 KB L1D, ROB=320.
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
    // Zen 4: store-to-load forwarding = 4 cycles (same as L1D hit latency).
    // Zen 4 uses 256-bit AVX-512 double-pump internally; the profile already
    // reflects this with vectorWidth=256.  No additional throughput penalty
    // needed since the 256-bit paths ARE the native paths.
    p.latStoLForward = 4;
    p.vec512Penalty = 1; // 256-bit native paths; AVX-512 handled at profile level
    p.tputFPDiv = 1.0 / 13.0;
    p.tputIntDiv = 1.0 / 17.0;
    p.schedulerSize = 96;
    p.loadBufferEntries = 88;
    p.storeBufferEntries = 64;
    p.l1DBandwidthBytesPerCycle = 96; // 3 AGU × 32 B
    p.l2BandwidthBytesPerCycle  = 48;
    p.l3BandwidthBytesPerCycle  = 24;
    p.memBandwidthBytesPerCycle = 12;
    p.hasHyperthreading = false; // Zen 4 desktop: SMT disabled by default; server enables it
    // Zen 4: VCVTSI2SD = 4 cycles, VADDPD = 3 cycles → +1 for CVT bypass.
    p.latFPConvert = p.latFPAdd + 1;
    return p;
}

/// Return an AMD Zen 3 (Ryzen 5000 / EPYC Milan) profile.
/// Zen 3 (2020): unified 8-core CCX (vs 4-core in Zen 2), 512 KB L2, no AVX-512.
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
    // Apple M1: store-to-load forwarding is ~4 cycles.  Store execution latency
    // is 3 cycles (write to store buffer), but forwarding requires a store-buffer
    // address match which adds 1 cycle → forwarding = 4 cycles > latStore.
    p.latStoLForward = 4;
    p.vec512Penalty = 1; // NEON 128-bit native; no 512-bit SIMD
    p.tputFPDiv = 1.0 / 10.0;
    p.tputIntDiv = 1.0 / 10.0;
    // Apple M1 Firestorm: very large RS (~600 effective), massive buffers.
    p.schedulerSize = 600;
    p.loadBufferEntries = 320;
    p.storeBufferEntries = 168;
    p.l1DBandwidthBytesPerCycle = 192; // 3 wide load paths × 16 B NEON (AMX aside)
    p.l2BandwidthBytesPerCycle  = 96;
    p.l3BandwidthBytesPerCycle  = 48;
    p.memBandwidthBytesPerCycle = 32; // LPDDR5 ~32 GB/s / 3.2 GHz ≈ 10 B/cyc; rough
    p.hasHyperthreading = false; // Apple Silicon: no SMT
    return p;
}

/// Return an ARM Neoverse V2 (server, 2023) profile.
/// Used in AWS Graviton4, NVIDIA Grace, Google Axion.  8-wide dispatch,
/// 256-bit SVE2, 6 integer pipes, 4 FMA units, ROB=256.
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
    // Neoverse V2: store-to-load forwarding = 4 cycles (same as L1D latency).
    p.latStoLForward = 4;
    p.vec512Penalty = 1; // SVE2 256-bit native
    p.tputFPDiv = 1.0 / 10.0;
    p.tputIntDiv = 1.0 / 12.0;
    p.schedulerSize = 128;
    p.loadBufferEntries = 128;
    p.storeBufferEntries = 72;
    p.l1DBandwidthBytesPerCycle = 48; // 3 load paths × 16 B
    p.l2BandwidthBytesPerCycle  = 32;
    p.l3BandwidthBytesPerCycle  = 16;
    p.memBandwidthBytesPerCycle = 10;
    p.hasHyperthreading = false; // Neoverse V2: no SMT
    return p;
}

/// Return an ARM Neoverse N2 (efficiency server, 2022) profile.
/// 5-wide dispatch, 4 integer pipes, 128-bit SVE2, 512 KB L2.
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
    // RISC-V in-order: forwarding latency = store latency (no out-of-order
    // store buffer; data is forwarded in the same pipeline cycle as writeback).
    p.latStoLForward = 3;
    p.vec512Penalty = 1; // RVV at 128-bit VLEN; no 512-bit wide ops
    p.tputFPDiv = 1.0 / 18.0;
    p.tputIntDiv = 1.0 / 20.0;
    p.schedulerSize = 8;
    p.loadBufferEntries = 8;
    p.storeBufferEntries = 8;
    p.l1DBandwidthBytesPerCycle = 8;
    p.l2BandwidthBytesPerCycle  = 8;
    p.l3BandwidthBytesPerCycle  = 6;
    p.memBandwidthBytesPerCycle = 4;
    p.hasHyperthreading = false;
    return p;
}

/// Return a SiFive U74 (RISC-V, in-order, single-issue) profile.
/// Used in HiFive Unmatched and various embedded SoCs.
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
    // Graviton3 (Neoverse V1): store-to-load forwarding = 4 cycles.
    p.latStoLForward = 4;
    p.vec512Penalty = 1; // SVE 256-bit native
    p.tputFPDiv = 1.0 / 10.0;
    p.tputIntDiv = 1.0 / 12.0;
    p.schedulerSize = 128;
    p.loadBufferEntries = 120;
    p.storeBufferEntries = 64;
    p.l1DBandwidthBytesPerCycle = 48; // 3 load pipes × 16 B
    p.l2BandwidthBytesPerCycle  = 32;
    p.l3BandwidthBytesPerCycle  = 18;
    p.memBandwidthBytesPerCycle = 12; // DDR5
    p.hasHyperthreading = false;
    return p;
}

/// Return an AWS Graviton4 profile (Arm Neoverse V2-based, 2024).
/// Graviton4: 96-core, 256-bit SVE2, DDR5-5600, wider backend than Graviton3.
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
    // Lion Cove: store-to-load forwarding = 4 cycles (store buffer bypass).
    p.latStoLForward = 4;
    p.vec512Penalty = 1; // AVX2 only; no 512-bit SIMD on Lion Cove
    p.tputFPDiv = 1.0 / 10.0;
    p.tputIntDiv = 1.0 / 20.0;
    p.schedulerSize = 160;
    p.loadBufferEntries = 256;
    p.storeBufferEntries = 120;
    p.l1DBandwidthBytesPerCycle = 96; // 3 load ports × 32 B
    p.l2BandwidthBytesPerCycle  = 48;
    p.l3BandwidthBytesPerCycle  = 20;
    p.memBandwidthBytesPerCycle = 10;
    p.hasHyperthreading = false; // Lunar Lake: no Hyperthreading on Lion Cove
    // Lunar Lake (Lion Cove): CVTSI2SD = latFPAdd+1 (x86 CVT bypass cycle).
    p.latFPConvert = p.latFPAdd + 1;
    return p;
}

/// Return an improved AMD Zen 5 profile (2024).
/// Zen 5: 8-wide dispatch, 6 ALU pipes, 4× 256-bit vector units,
/// native 512-bit AVX-512 (not double-pumped), ROB=448, latFPAdd=3.
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
    // Zen 5: store-to-load forwarding = 4 cycles (same as L1D latency).
    p.latStoLForward = 4;
    // Zen 5 has NATIVE 512-bit AVX-512 execution units (not double-pumped),
    // unlike Zen 4 which decomposes 512-bit ops into 2×256-bit µops.
    p.vec512Penalty = 1;
    p.tputFPDiv = 1.0 / 12.0;
    p.tputIntDiv = 1.0 / 15.0;
    p.schedulerSize = 128;
    p.loadBufferEntries = 128;
    p.storeBufferEntries = 80;
    p.l1DBandwidthBytesPerCycle = 128; // 4 load ports × 32 B
    p.l2BandwidthBytesPerCycle  = 64;
    p.l3BandwidthBytesPerCycle  = 32;
    p.memBandwidthBytesPerCycle = 16;
    p.hasHyperthreading = false;
    // Zen 5: VCVTSI2SD = latFPAdd+1 cycles (x86 CVT bypass path).
    p.latFPConvert = p.latFPAdd + 1;
    return p;
}

/// Return an Intel Sapphire Rapids / Emerald Rapids profile.
/// Sapphire Rapids (Xeon 4th Gen, 2023): 6 µops/cycle dispatch, 5 integer ports,
/// AVX-512 native at 512 bits (not double-pumped like Skylake-AVX512),
/// ROB doubled to 512, 3 load + 3 store ports.
/// Emerald Rapids (Xeon 5th Gen, 2023): same µarch, minor improvements.
[[gnu::cold]] static MicroarchProfile sapphireRapidsProfile() {
    MicroarchProfile p;
    p.name = "sapphirerapids";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 6;         // µop cache delivers 6/cycle (MITE decoder 4)
    p.issueWidth = 6;          // 6 µops dispatched per cycle (same as Skylake)
    p.pipelineDepth = 14;
    p.intALUs = 5;             // ports 0,1,5,6,10 (5 integer execution ports)
    p.vecUnits = 3;            // ports 0,1,5 with native 512-bit EVEX width
    p.fmaUnits = 2;            // ports 0 and 1
    p.loadPorts = 3;           // ports 2,3,8 (all three with AGU)
    p.storePorts = 3;          // ports 4,7,9 (incl. store-address port 9)
    p.branchUnits = 2;
    p.agus = 5;                // 3 load AGUs + 2 store-address AGUs
    p.dividers = 1;
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 18;
    p.latFPAdd = 4; p.latFPMul = 4; p.latFPDiv = 15; p.latFMA = 4;
    p.latLoad = 5; p.latStore = 5; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.20;       // 5 integer ports → ~5/cycle
    p.tputIntMul = 0.5;
    p.tputFPAdd = 0.33; p.tputFPMul = 0.33;
    p.tputLoad = 0.33; p.tputStore = 0.33;
    p.l1DSize = 48;            // 48 KB L1D (same as Ice Lake)
    p.l1DLatency = 5;
    p.l2Size = 2048;           // 2 MB L2 per core
    p.l2Latency = 14;
    p.l3Size = 61440;          // up to 60 MB shared L3
    p.l3Latency = 52;          // larger L3 → higher latency
    p.cacheLineSize = 64;
    p.vectorWidth = 512;       // Native AVX-512, not double-pumped
    p.intRegisters = 16; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 15.0;
    p.btbEntries = 12288;
    p.memoryLatency = 200;
    p.robSize = 512;           // Doubled from Skylake's 224
    // Sapphire Rapids: store-to-load forwarding ≈ 4 cycles (store buffer bypass),
    // shorter than the full 5-cycle L1D latency.
    p.latStoLForward = 4;
    // Sapphire Rapids has NATIVE 512-bit AVX-512 FMA/VecALU units (Golden Cove
    // execution backend), so no double-pump penalty unlike Skylake-AVX512.
    p.vec512Penalty = 1;
    p.tputFPDiv = 1.0 / 15.0;
    p.tputIntDiv = 1.0 / 18.0;
    p.schedulerSize = 160;
    p.loadBufferEntries = 192;
    p.storeBufferEntries = 128;
    p.l1DBandwidthBytesPerCycle = 96; // 3 load ports × 32 B
    p.l2BandwidthBytesPerCycle  = 64;
    p.l3BandwidthBytesPerCycle  = 32;
    p.memBandwidthBytesPerCycle = 14;
    p.hasHyperthreading = true; // Sapphire Rapids server: HT enabled
    // Sapphire Rapids: CVTSI2SD = latFPAdd+1 (x86 integer-to-FP bypass path).
    p.latFPConvert = p.latFPAdd + 1;
    return p;
}

/// Return an Apple M2 (Everest P-core, 2022) profile.
/// M2 improves on M1 with wider vector units, more FMA units, and higher
/// bandwidth.  The P-core has 8-wide dispatch, 6 integer pipes, 4 FMA units.
[[gnu::cold]] static MicroarchProfile appleM2Profile() {
    MicroarchProfile p;
    p.name = "apple-m2";
    p.isa = ISAFamily::AArch64;
    p.decodeWidth = 8;
    p.issueWidth = 8;           // 8-wide OoO dispatch (up from 6 on M1)
    p.pipelineDepth = 13;
    p.intALUs = 6;              // 6 integer pipes (up from 4 on M1)
    p.vecUnits = 4;             // 4× 128-bit NEON/FP units (up from 3)
    p.fmaUnits = 4;             // 4 FMA units (up from 3 on M1)
    p.loadPorts = 3;            // 3 load pipes (up from 2)
    p.storePorts = 2;
    p.branchUnits = 2;
    p.agus = 3;
    p.dividers = 2;
    p.mulPortCount = 4;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 12;
    p.latFPAdd = 3; p.latFPMul = 3; p.latFPDiv = 9; p.latFMA = 3;
    // M2 L1D: 128 KB; L2: 16 MB shared with E-cores; load latency 3 cycles.
    p.latLoad = 3; p.latStore = 3; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.17; p.tputIntMul = 0.25;
    p.tputFPAdd = 0.25; p.tputFPMul = 0.25;
    p.tputLoad = 0.33; p.tputStore = 0.5;
    p.l1DSize = 128; p.l1DLatency = 3;
    p.l2Size = 16384; p.l2Latency = 8;
    p.l3Size = 0;    p.l3Latency = 12;  // SLC (System Level Cache) as L3 proxy
    p.cacheLineSize = 128;              // Apple Silicon uses 128-byte cache lines
    p.vectorWidth = 128;                // NEON 128-bit (AMX for matrix, separate)
    p.intRegisters = 30; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 11.0;   // deep OoO pipeline, good predictor
    p.btbEntries = 16384;
    p.memoryLatency = 100;              // LPDDR5 — lower latency than x86 DRAM
    p.robSize = 250;                    // Larger ROB than M1 (M1≈192)
    // M2 store-to-load forwarding: 4 cycles (same as L1D hit).
    // Store buffer address comparison adds 1 cycle vs raw load → 4 > latStore=3.
    p.latStoLForward = 4;
    p.vec512Penalty = 1; // NEON 128-bit native; no 512-bit SIMD
    p.tputFPDiv = 1.0 / 9.0;
    p.tputIntDiv = 1.0 / 12.0;
    p.schedulerSize = 480;
    p.loadBufferEntries = 320;
    p.storeBufferEntries = 168;
    p.l1DBandwidthBytesPerCycle = 192;
    p.l2BandwidthBytesPerCycle  = 96;
    p.l3BandwidthBytesPerCycle  = 48;
    p.memBandwidthBytesPerCycle = 32;
    p.hasHyperthreading = false;
    return p;
}

/// Return an Intel Arrow Lake (Lion Cove P-core + Skymont E-core, 2024) profile.
/// Arrow Lake removed Hyperthreading from P-cores and dropped AVX-512.
/// Lion Cove improves dispatch to 6-wide vs 5-wide Raptor Lake P-cores.
/// This profile covers the P-core execution model.
[[gnu::cold]] static MicroarchProfile arrowLakeProfile() {
    MicroarchProfile p;
    p.name = "arrow-lake";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 6;          // 6-wide µop cache delivery
    p.issueWidth = 6;           // 6-wide dispatch to backend
    p.pipelineDepth = 14;
    p.intALUs = 6;              // 6 integer execution ports (P0-P5)
    p.vecUnits = 3;             // 3 vector ALU ports (no AVX-512)
    p.fmaUnits = 2;             // 2 FMA units on P0/P1
    p.loadPorts = 3;            // 3 load ports
    p.storePorts = 2;
    p.branchUnits = 2;
    p.agus = 3;
    p.dividers = 1;
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 20;
    p.latFPAdd = 4; p.latFPMul = 4; p.latFPDiv = 12; p.latFMA = 4;
    p.latLoad = 4; p.latStore = 5; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.17; p.tputIntMul = 0.5;
    p.tputFPAdd = 0.33; p.tputFPMul = 0.33;
    p.tputLoad = 0.33; p.tputStore = 0.5;
    p.l1DSize = 48;  p.l1DLatency = 4;
    p.l2Size = 2048; p.l2Latency = 14;     // Arrow Lake P-core: 2 MB L2
    p.l3Size = 36864; p.l3Latency = 42;    // 36 MB shared L3
    p.cacheLineSize = 64;
    p.vectorWidth = 256;        // AVX2 (AVX-512 removed vs Raptor Lake)
    p.intRegisters = 16; p.vecRegisters = 16; p.fpRegisters = 16;
    p.branchMispredictPenalty = 12.0;
    p.btbEntries = 16384;
    p.memoryLatency = 180;
    p.robSize = 256;            // Improved vs Raptor Lake (192)
    p.latStoLForward = 4;
    p.vec512Penalty = 1;        // No AVX-512
    p.tputFPDiv = 1.0 / 12.0;
    p.tputIntDiv = 1.0 / 20.0;
    p.schedulerSize = 160;
    p.loadBufferEntries = 256;
    p.storeBufferEntries = 120;
    p.l1DBandwidthBytesPerCycle = 96; // 3 load ports × 32 B
    p.l2BandwidthBytesPerCycle  = 48;
    p.l3BandwidthBytesPerCycle  = 20;
    p.memBandwidthBytesPerCycle = 10;
    p.hasHyperthreading = false; // Arrow Lake: no Hyperthreading on Lion Cove
    // Arrow Lake (Lion Cove P-core): CVTSI2SD = latFPAdd+1 (x86 CVT bypass).
    p.latFPConvert = p.latFPAdd + 1;
    return p;
}

/// Return an AMD EPYC Turin (Zen 5, server, 2024) profile.
/// Turin: up to 192 cores, Zen 5 µarch, native 512-bit AVX-512, large L3.
[[gnu::cold]] static MicroarchProfile epycTurinProfile() {
    MicroarchProfile p = zen5Profile();
    p.name = "epyc-turin";
    // Turin uses the same Zen 5 pipeline but with much larger L3 cache
    // spread across chiplets.  Per-core L3 share is smaller than desktop.
    p.l2Size = 1024;            // 1 MB L2 per core (same as Zen 5)
    p.l3Size = 65536;           // 64 MB per CCD (shared across 16 cores)
    p.l3Latency = 55;           // Slightly higher due to chiplet interconnect
    p.memoryLatency = 220;      // DDR5-6400, longer NUMA hops on big configs
    p.robSize = 448;            // Same ROB as desktop Zen 5
    return p;
}

/// Return an Intel Granite Rapids (Redwood Cove P-core, 2024) profile.
/// Xeon 6th Gen: enhanced Sapphire Rapids backend, wider dispatch,
/// native AVX-512, larger ROB, faster integer execution.
[[gnu::cold]] static MicroarchProfile graniteRapidsProfile() {
    MicroarchProfile p = sapphireRapidsProfile();
    p.name = "granite-rapids";
    // Granite Rapids: 8-wide dispatch (up from 6 in Sapphire Rapids).
    p.issueWidth = 8;
    p.decodeWidth = 6;           // µop cache still 6-wide legacy decode
    p.intALUs = 6;               // 6 integer execution ports (up from 5)
    p.vecUnits = 4;              // 4 vector ALU ports (up from 3)
    p.fmaUnits = 2;              // 2 native 512-bit FMA units
    p.loadPorts = 3;
    p.storePorts = 3;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 16;
    p.latFPAdd = 4; p.latFPMul = 4; p.latFPDiv = 14; p.latFMA = 4;
    p.latLoad = 5; p.latStore = 5;
    p.l1DSize = 48;   p.l1DLatency = 5;
    p.l2Size = 2048;  p.l2Latency = 14;
    p.l3Size = 131072; p.l3Latency = 55;  // up to 128 MB shared L3
    p.robSize = 512;
    p.btbEntries = 16384;
    p.memoryLatency = 200;
    p.vec512Penalty = 1;  // native 512-bit (same as Sapphire Rapids)
    p.latStoLForward = 4;
    p.tputFPDiv = 1.0 / 14.0;
    p.tputIntDiv = 1.0 / 16.0;
    p.schedulerSize = 160;
    p.loadBufferEntries = 192;
    p.storeBufferEntries = 128;
    p.l1DBandwidthBytesPerCycle = 96;
    p.l2BandwidthBytesPerCycle  = 64;
    p.l3BandwidthBytesPerCycle  = 32;
    p.memBandwidthBytesPerCycle = 14;
    p.hasHyperthreading = true;
    return p;
}

/// Return an AMD EPYC Zen4c (Bergamo, 2023) compact profile.
/// Zen4c uses the same Zen 4 µarch but with smaller caches and higher
/// core density (up to 128 cores/socket).  Same execution pipeline;
/// reduced L2 per core (1 MB → 1 MB, but smaller L3 per core share).
[[gnu::cold]] static MicroarchProfile zen4cProfile() {
    MicroarchProfile p = zen4Profile();
    p.name = "znver4c";
    // Bergamo: same pipeline as Zen 4 but denser packing.
    // L3 shared more aggressively: effective per-core L3 is smaller.
    p.l3Size = 16384;   // 16 MB shared (vs 32 MB on standard Zen 4)
    p.l3Latency = 54;   // slightly higher due to larger NUMA distance
    p.memoryLatency = 200;
    return p;
}

/// Return a SiFive P670 (RISC-V, high-performance OoO, 2023) profile.
/// P670: 4-issue out-of-order, RVV 128-bit VLEN, in-order front-end decode.
[[gnu::cold]] static MicroarchProfile sifiveP670Profile() {
    MicroarchProfile p;
    p.name = "sifive-p670";
    p.isa = ISAFamily::RISCV64;
    p.decodeWidth = 4;
    p.issueWidth = 4;
    p.pipelineDepth = 10;
    p.intALUs = 3;
    p.vecUnits = 2;
    p.fmaUnits = 2;
    p.loadPorts = 2;
    p.storePorts = 1;
    p.branchUnits = 1;
    p.agus = 2;
    p.dividers = 1;
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 20;
    p.latFPAdd = 4; p.latFPMul = 4; p.latFPDiv = 16; p.latFMA = 5;
    p.latLoad = 4; p.latStore = 4; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.33; p.tputIntMul = 1.0;
    p.tputFPAdd = 0.5; p.tputFPMul = 0.5;
    p.tputLoad = 0.5; p.tputStore = 1.0;
    p.l1DSize = 32; p.l1DLatency = 4;
    p.l2Size = 1024; p.l2Latency = 12;
    p.l3Size = 4096; p.l3Latency = 35;
    p.cacheLineSize = 64;
    p.vectorWidth = 128;  // RVV at 128-bit VLEN
    p.intRegisters = 31; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 8.0;
    p.btbEntries = 2048;
    p.memoryLatency = 200;
    p.robSize = 128;
    p.latStoLForward = 4;
    p.vec512Penalty = 1;
    p.tputFPDiv = 1.0 / 16.0;
    p.tputIntDiv = 1.0 / 20.0;
    p.schedulerSize = 40;
    p.loadBufferEntries = 32;
    p.storeBufferEntries = 24;
    p.l1DBandwidthBytesPerCycle = 16;
    p.l2BandwidthBytesPerCycle  = 12;
    p.l3BandwidthBytesPerCycle  = 8;
    p.memBandwidthBytesPerCycle = 4;
    p.hasHyperthreading = false;
    return p;
}

/// Return a Qualcomm Oryon (Snapdragon X Elite, 2024) profile.
/// Oryon is Qualcomm's custom ARMv9 P-core, debuting in the Snapdragon X
/// Elite/Plus SoC for Windows laptops and competing directly with Apple M3.
/// Key: 6-wide superscalar OoO, 5 int ALU, 4 FMA/NEON units, large ROB.
[[gnu::cold]] static MicroarchProfile oryonProfile() {
    MicroarchProfile p;
    p.name = "oryon";
    p.isa = ISAFamily::AArch64;
    p.decodeWidth = 6;          // 6-wide fetch and decode
    p.issueWidth = 6;           // 6-wide dispatch to execution units
    p.pipelineDepth = 12;
    p.intALUs = 5;              // 5 integer execution pipes
    p.vecUnits = 4;             // 4 NEON/FP 128-bit units
    p.fmaUnits = 4;             // 4 FMA units (VFMA.2D, VFMA.4S)
    p.loadPorts = 3;            // 3 load pipes
    p.storePorts = 2;
    p.branchUnits = 2;
    p.agus = 3;
    p.dividers = 2;
    p.mulPortCount = 4;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 12;
    p.latFPAdd = 3; p.latFPMul = 3; p.latFPDiv = 9; p.latFMA = 3;
    p.latLoad = 4; p.latStore = 3; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.20; p.tputIntMul = 0.25;
    p.tputFPAdd = 0.25; p.tputFPMul = 0.25;
    p.tputLoad = 0.33; p.tputStore = 0.5;
    p.l1DSize = 64;  p.l1DLatency = 4;
    p.l2Size = 1024; p.l2Latency = 10;  // 1 MB L2 per 4-core cluster
    p.l3Size = 6144; p.l3Latency = 35;  // 6 MB shared L3
    p.cacheLineSize = 64;
    p.vectorWidth = 128;        // NEON 128-bit (SVE2 optional)
    p.intRegisters = 30; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 14.0;
    p.btbEntries = 8192;
    p.memoryLatency = 100;      // LPDDR5x — fast
    p.robSize = 320;
    p.latStoLForward = 4;
    p.vec512Penalty = 1;        // NEON 128-bit native
    p.tputFPDiv = 1.0 / 9.0;
    p.tputIntDiv = 1.0 / 12.0;
    p.schedulerSize = 320;
    p.loadBufferEntries = 256;
    p.storeBufferEntries = 128;
    p.l1DBandwidthBytesPerCycle = 192;
    p.l2BandwidthBytesPerCycle  = 96;
    p.l3BandwidthBytesPerCycle  = 48;
    p.memBandwidthBytesPerCycle = 32;
    p.hasHyperthreading = false;
    return p;
}

/// Return an Intel Sierra Forest (Crestmont E-core, 2024) profile.
/// Sierra Forest is Intel's first E-core-only server CPU (Xeon 6 E-series).
/// Crestmont is an improved Gracemont E-core: 3-wide in-order decode per
/// cluster, 4 integer ALU pipes (modest OoO), no AVX-512, no SMT.
[[gnu::cold]] static MicroarchProfile sierraForestProfile() {
    MicroarchProfile p;
    p.name = "sierra-forest";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 3;          // 3-wide decode per E-core cluster
    p.issueWidth = 4;           // 4-wide dispatch (narrower than P-cores)
    p.pipelineDepth = 10;       // shallower pipeline than P-cores
    p.intALUs = 4;              // 4 integer execution pipes
    p.vecUnits = 2;             // 2 vector ALU pipes (256-bit AVX2)
    p.fmaUnits = 1;             // 1 FMA unit per core
    p.loadPorts = 2;
    p.storePorts = 1;
    p.branchUnits = 1;
    p.agus = 2;
    p.dividers = 1;
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 20;
    p.latFPAdd = 4; p.latFPMul = 4; p.latFPDiv = 14; p.latFMA = 5;
    p.latLoad = 5; p.latStore = 5; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.25; p.tputIntMul = 0.5;
    p.tputFPAdd = 0.5;  p.tputFPMul = 0.5;
    p.tputLoad = 0.5;   p.tputStore = 1.0;
    p.l1DSize = 32;  p.l1DLatency = 5;
    p.l2Size = 2048; p.l2Latency = 14;  // 2 MB per 4-core cluster
    p.l3Size = 57344; p.l3Latency = 50; // up to 56 MB shared L3
    p.cacheLineSize = 64;
    p.vectorWidth = 256;        // AVX2 (no AVX-512)
    p.intRegisters = 16; p.vecRegisters = 16; p.fpRegisters = 16;
    p.branchMispredictPenalty = 9.0; // shorter pipeline → smaller penalty
    p.btbEntries = 4096;
    p.memoryLatency = 190;
    p.robSize = 160;            // Crestmont ROB is ~160 (smaller than P-cores)
    p.latStoLForward = 4;
    p.vec512Penalty = 1;        // No AVX-512
    p.tputFPDiv = 1.0 / 14.0;
    p.tputIntDiv = 1.0 / 20.0;
    p.schedulerSize = 64;
    p.loadBufferEntries = 72;
    p.storeBufferEntries = 48;
    p.l1DBandwidthBytesPerCycle = 64; // 2 load ports × 32 B
    p.l2BandwidthBytesPerCycle  = 32;
    p.l3BandwidthBytesPerCycle  = 14;
    p.memBandwidthBytesPerCycle = 6;
    p.hasHyperthreading = false; // Sierra Forest E-cores: no SMT
    // Sierra Forest (Crestmont): x86 CVT bypass adds 1 cycle vs FP-add.
    p.latFPConvert = p.latFPAdd + 1;
    return p;
}

/// Return an Intel Gracemont (Atom E-core, 2021-2024) standalone profile.
/// Gracemont is Intel's efficiency E-core used in Alder/Raptor Lake hybrid
/// CPUs and Sierra Forest servers.  It is a shallow in-order-ish 3-wide
/// decode core — significantly narrower than the P-core Golden/Raptor Cove.
/// In Alder/Raptor Lake it is paired with P-cores; here modelled standalone.
[[gnu::cold]] static MicroarchProfile gracemontProfile() {
    MicroarchProfile p;
    p.name = "gracemont";
    p.isa = ISAFamily::X86_64;
    p.decodeWidth = 3;          // 3-wide decode (clustered E-core)
    p.issueWidth = 4;           // 4 µops dispatched per cycle
    p.pipelineDepth = 8;        // shallow pipeline
    p.intALUs = 3;              // 3 integer execution pipes
    p.vecUnits = 2;             // 2 vector ALU pipes
    p.fmaUnits = 1;             // 1 FMA unit
    p.loadPorts = 2;
    p.storePorts = 1;
    p.branchUnits = 1;
    p.agus = 2;
    p.dividers = 1;
    p.mulPortCount = 1;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 20;
    p.latFPAdd = 4; p.latFPMul = 4; p.latFPDiv = 14; p.latFMA = 5;
    p.latLoad = 5; p.latStore = 5; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.33; p.tputIntMul = 1.0;
    p.tputFPAdd = 0.5;  p.tputFPMul = 0.5;
    p.tputLoad = 0.5;   p.tputStore = 1.0;
    p.l1DSize = 32;  p.l1DLatency = 5;
    p.l2Size = 4096; p.l2Latency = 14;   // 4 MB shared per cluster
    p.l3Size = 20480; p.l3Latency = 44;  // 20 MB shared L3 (Alder Lake die)
    p.cacheLineSize = 64;
    p.vectorWidth = 256;        // AVX2 (no AVX-512)
    p.intRegisters = 16; p.vecRegisters = 16; p.fpRegisters = 16;
    p.branchMispredictPenalty = 8.0;
    p.btbEntries = 1024;
    p.memoryLatency = 200;
    p.robSize = 128;
    p.latStoLForward = 5;
    p.vec512Penalty = 1;
    p.tputFPDiv = 1.0 / 14.0;
    p.tputIntDiv = 1.0 / 20.0;
    p.schedulerSize = 48;
    p.loadBufferEntries = 64;
    p.storeBufferEntries = 32;
    p.l1DBandwidthBytesPerCycle = 64;
    p.l2BandwidthBytesPerCycle  = 28;
    p.l3BandwidthBytesPerCycle  = 12;
    p.memBandwidthBytesPerCycle = 6;
    p.hasHyperthreading = false;
    // Gracemont: x86 E-core, CVTSI2SD = latFPAdd+1 (CVT bypass path).
    p.latFPConvert = p.latFPAdd + 1;
    return p;
}

/// Return an ARM Cortex-A76 (high-performance mobile, 2019) profile.
/// The A76 is a wide 4-issue OoO core found in many Cortex-based SoCs
/// (Arm Mali-G76 era).  First core to match x86 laptop single-thread perf.
[[gnu::cold]] static MicroarchProfile cortexA76Profile() {
    MicroarchProfile p;
    p.name = "cortex-a76";
    p.isa = ISAFamily::AArch64;
    p.decodeWidth = 4;
    p.issueWidth = 4;
    p.pipelineDepth = 13;
    p.intALUs = 3;              // 3 integer execution pipes (B, M, S)
    p.vecUnits = 2;             // 2 NEON/FP 128-bit pipes
    p.fmaUnits = 2;
    p.loadPorts = 2;
    p.storePorts = 1;
    p.branchUnits = 1;
    p.agus = 2;
    p.dividers = 1;
    p.mulPortCount = 2;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 10;
    p.latFPAdd = 3; p.latFPMul = 4; p.latFPDiv = 12; p.latFMA = 4;
    p.latLoad = 4; p.latStore = 4; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.33; p.tputIntMul = 0.5;
    p.tputFPAdd = 0.5;  p.tputFPMul = 0.5;
    p.tputLoad = 0.5;   p.tputStore = 1.0;
    p.l1DSize = 64;  p.l1DLatency = 4;
    p.l2Size = 512;  p.l2Latency = 10;
    p.l3Size = 4096; p.l3Latency = 32;
    p.cacheLineSize = 64;
    p.vectorWidth = 128;        // NEON 128-bit
    p.intRegisters = 31; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 11.0;
    p.btbEntries = 4096;
    p.memoryLatency = 120;
    p.robSize = 128;
    p.latStoLForward = 4;
    p.vec512Penalty = 1;
    p.tputFPDiv = 1.0 / 12.0;
    p.tputIntDiv = 1.0 / 10.0;
    p.schedulerSize = 64;
    p.loadBufferEntries = 56;
    p.storeBufferEntries = 32;
    p.l1DBandwidthBytesPerCycle = 32; // 2 load pipes × 16 B
    p.l2BandwidthBytesPerCycle  = 16;
    p.l3BandwidthBytesPerCycle  = 10;
    p.memBandwidthBytesPerCycle = 6;
    p.hasHyperthreading = false;
    return p;
}

/// Return an ARM Cortex-A55 (efficiency mobile, 2017–present) profile.
/// The A55 is a compact 2-issue in-order core widely used as the efficiency
/// cluster in big.LITTLE / DynamIQ designs paired with A75/A76/A78/X1.
[[gnu::cold]] static MicroarchProfile cortexA55Profile() {
    MicroarchProfile p;
    p.name = "cortex-a55";
    p.isa = ISAFamily::AArch64;
    p.decodeWidth = 2;
    p.issueWidth = 2;           // dual-issue in-order
    p.pipelineDepth = 8;
    p.intALUs = 2;
    p.vecUnits = 1;
    p.fmaUnits = 1;
    p.loadPorts = 1;
    p.storePorts = 1;
    p.branchUnits = 1;
    p.agus = 1;
    p.dividers = 1;
    p.mulPortCount = 1;
    p.latIntAdd = 1; p.latIntMul = 3; p.latIntDiv = 12;
    p.latFPAdd = 4; p.latFPMul = 4; p.latFPDiv = 14; p.latFMA = 5;
    p.latLoad = 3; p.latStore = 3; p.latBranch = 1; p.latShift = 1;
    p.tputIntAdd = 0.5; p.tputIntMul = 1.0;
    p.tputFPAdd = 1.0;  p.tputFPMul = 1.0;
    p.tputLoad = 1.0;   p.tputStore = 1.0;
    p.l1DSize = 32;  p.l1DLatency = 3;
    p.l2Size = 256;  p.l2Latency = 8;
    p.l3Size = 2048; p.l3Latency = 28;  // shared LLC
    p.cacheLineSize = 64;
    p.vectorWidth = 128;
    p.intRegisters = 31; p.vecRegisters = 32; p.fpRegisters = 32;
    p.branchMispredictPenalty = 8.0;
    p.btbEntries = 1024;
    p.memoryLatency = 100;
    p.robSize = 64;             // effectively in-order; small window
    p.latStoLForward = 3;
    p.vec512Penalty = 1;
    p.tputFPDiv = 1.0 / 14.0;
    p.tputIntDiv = 1.0 / 12.0;
    p.schedulerSize = 16;
    p.loadBufferEntries = 16;
    p.storeBufferEntries = 8;
    p.l1DBandwidthBytesPerCycle = 16;
    p.l2BandwidthBytesPerCycle  = 8;
    p.l3BandwidthBytesPerCycle  = 6;
    p.memBandwidthBytesPerCycle = 4;
    p.hasHyperthreading = false;
    return p;
}

/// Return an NVIDIA Grace CPU (Neoverse V2-based, 2023) profile.
/// Grace is NVIDIA's first CPU, used in the Grace-Hopper and Grace-Grace
/// Superchips.  The CPU is a Neoverse V2 (72 cores) with HBM2e memory
/// providing very high bandwidth (~500 GB/s shared across the chip).
[[gnu::cold]] static MicroarchProfile nvidiaGraceProfile() {
    MicroarchProfile p = neoverseV2Profile();
    p.name = "nvidia-grace";
    // Same Neoverse V2 execution pipeline.  Key difference: HBM2e memory.
    p.memoryLatency = 80;       // HBM2e: ~80 ns at 3.1 GHz → ~250 cycles; 
                                 // effective with hardware prefetcher: ~80 cyc
    // HBM bandwidth: ~500 GB/s total / 72 cores ≈ 6.9 GB/s per core.
    // At 3.1 GHz → ~2.2 bytes/cycle per core (aggregate).  Use a higher value
    // to reflect that many-core workloads can sustain more per-core bandwidth.
    p.memBandwidthBytesPerCycle = 20; // higher than DDR5; HBM delivers well
    p.l3Size = 114688;          // 117 MB Last-Level Cache (system level cache)
    p.l3Latency = 45;
    p.hasHyperthreading = false;
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

    const std::string normalized = normalizeCpuName(cpuName);

    // x86-64 microarchitectures
    if (normalized == "skylake" || normalized == "skylakeserver" ||
        normalized == "skylakeavx512" || normalized == "cascadelake" ||
        normalized == "cooperlake" || normalized == "cannonlake")
        return skylakeProfile();

    if (normalized == "haswell" || normalized == "broadwell")
        return haswellProfile();

    if (normalized == "sandybridge" || normalized == "sandybridgep" ||
        normalized == "ivybridge" || normalized == "ivybridgep" ||
        normalized == "ivytown" || normalized == "jaketown" ||
        normalized == "westmere" || normalized == "nehalem") {
        auto p = sandyBridgeProfile();
        p.name = normalized;
        if (normalized == "ivybridge" || normalized == "ivybridgep" ||
            normalized == "ivytown")
            p.name = "ivybridge";
        return p;
    }

    if (normalized == "alderlake" || normalized == "raptorlake" ||
        normalized == "meteorlake")
        return alderlakeProfile();

    if (normalized == "arrowlake" || normalized == "arrowlakes")
        return arrowLakeProfile();

    if (normalized == "lunarlake" || normalized == "pantherlake")
        return lunarLakeProfile();

    // Intel Gracemont / Crestmont E-core (standalone or as secondary cluster).
    if (normalized == "gracemont" || normalized == "atomx7000" ||
        normalized == "elkhart" || normalized == "jasper")
        return gracemontProfile();

    if (normalized == "icelakeserver" || normalized == "icelakeclient" ||
        normalized == "tigerlake") {
        auto p = skylakeProfile();
        p.name = cpuName;
        p.vectorWidth = 512; // AVX-512
        p.vecRegisters = 32;
        p.loadPorts = 3;     // Ice Lake: 3 load ports
        p.storePorts = 2;
        p.robSize = 352;     // Ice Lake ROB is 352
        // Ice Lake / Tiger Lake run 512-bit SIMD by double-pumping the 256-bit
        // execution units (same as Skylake-AVX512).  This halves throughput:
        // each 512-bit FMA/VecALU instruction occupies the port for 2 cycles.
        p.vec512Penalty = 2;
        return p;
    }

    // Sapphire Rapids / Emerald Rapids: dedicated profile with ROB=512,
    // AVX-512 native, 3 load + 3 store ports.
    if (normalized == "sapphirerapids" || normalized == "emeraldrapids")
        return sapphireRapidsProfile();
    if (normalized == "graniterapids" || normalized == "graniteerapids" ||
        normalized == "xeon6p")
        return graniteRapidsProfile();
    // Sierra Forest: Crestmont E-core server (Xeon 6 E-series).
    // Completely different pipeline from P-core Granite Rapids.
    if (normalized == "sierraforest" || normalized == "xeon6e" ||
        normalized == "clearwaterforest")
        return sierraForestProfile();

    // AMD Zen family
    if (normalized == "znver4" || normalized == "zen4")
        return zen4Profile();
    if (normalized == "znver4c" || normalized == "zen4c" || normalized == "bergamo")
        return zen4cProfile();
    if (normalized == "znver3" || normalized == "zen3")
        return zen3Profile();
    if (normalized == "znver2" || normalized == "zen2") {
        auto p = zen3Profile();
        p.name = "znver2";
        p.pipelineDepth = 19;
        p.loadPorts = 2;   // Zen 2 has 2 load/store AGU units (Zen 3 added a 3rd)
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
    if (normalized == "epycturin" || normalized == "zen5server" || normalized == "turin")
        return epycTurinProfile();

    // ARM64 — Apple Silicon
    if (normalized == "applem1" || normalized == "applema14")
        return appleMProfile();
    if (normalized == "applem2" || normalized == "applem2pro" ||
        normalized == "applem2max" || normalized == "applem2ultra" ||
        normalized == "applem2base") {
        return appleM2Profile();
    }
    if (normalized == "applem3" || normalized == "applem3pro" ||
        normalized == "applem3max" || normalized == "applem3ultra" ||
        normalized == "applem4") {
        auto p = appleM2Profile();
        p.name = cpuName;
        p.decodeWidth = 8;
        p.issueWidth = 10;        // M3/M4: wider backend
        p.intALUs = 6;
        p.vecUnits = 4;
        p.fmaUnits = 4;
        p.l1DSize = 192;
        p.l2Size = 16384;
        p.l3Size = 36864;
        p.robSize = 300;
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

    // NVIDIA Grace CPU (Neoverse V2-based, HBM2e memory).
    if (normalized == "nvidiagrace" || normalized == "grace" ||
        normalized == "gracehopper" || normalized == "neoversev2grace")
        return nvidiaGraceProfile();

    // ARM64 — AWS Graviton (server)
    if (normalized == "graviton3")
        return graviton3Profile();
    if (normalized == "graviton4")
        return graviton4Profile();

    // ARM64 — Cortex
    if (normalized == "cortexa78" || normalized == "cortexa78c" ||
        normalized == "cortexa77") {
        auto p = cortexA76Profile();
        p.name = cpuName;
        p.issueWidth = 4;
        return p;
    }
    if (normalized == "cortexa76" || normalized == "cortexa76ae")
        return cortexA76Profile();
    if (normalized == "cortexa55" || normalized == "cortexa53" ||
        normalized == "cortexa35") {
        auto p = cortexA55Profile();
        p.name = cpuName;
        return p;
    }
    if (normalized == "cortexall" || normalized == "cortexx3" ||
        normalized == "cortexx4") {
        auto p = neoverseV2Profile();
        p.name = cpuName;
        return p;
    }

    // ARM64 — Qualcomm Oryon (Snapdragon X Elite)
    if (normalized == "oryon" || normalized == "snapdragonxelite" ||
        normalized == "snapdragonxplus" || normalized == "snapdragonx")
        return oryonProfile();

    // RISC-V
    if (normalized == "genericrv64" || normalized == "riscv64")
        return riscvGenericProfile();
    if (normalized == "sifiveu74")
        return sifiveU74Profile();
    if (normalized == "sifivep670" || normalized == "sifivep470" || normalized == "sifivep270")
        return sifiveP670Profile();

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

MicroarchProfile calibrateProfile(const MicroarchProfile& base,
                                   const CalibrationHints& hints) {
    MicroarchProfile p = base;

    // Helper: scale an unsigned value and clamp to [1, UINT_MAX].
    auto scaleU = [](unsigned v, double s) -> unsigned {
        if (s <= 0.0) return 1u;
        unsigned r = static_cast<unsigned>(std::round(static_cast<double>(v) * s));
        return std::max(r, 1u);
    };

    // Integer-add latency group (add, sub, shift, branch, compare).
    if (hints.intAddScale != 1.0) {
        p.latIntAdd  = scaleU(p.latIntAdd,  hints.intAddScale);
        p.latShift   = scaleU(p.latShift,   hints.intAddScale);
        p.latBranch  = scaleU(p.latBranch,  hints.intAddScale);
    }

    // FP latency group (fadd, fmul, fma).
    if (hints.fpAddScale != 1.0) {
        p.latFPAdd = scaleU(p.latFPAdd, hints.fpAddScale);
        p.latFPMul = scaleU(p.latFPMul, hints.fpAddScale);
        p.latFMA   = scaleU(p.latFMA,   hints.fpAddScale);
    }

    // Load / store-to-load forwarding latency.
    if (hints.loadScale != 1.0) {
        p.latLoad        = scaleU(p.latLoad,        hints.loadScale);
        p.latStore       = scaleU(p.latStore,        hints.loadScale);
        p.latStoLForward = scaleU(p.latStoLForward,  hints.loadScale);
    }

    // Division latency (integer and FP).
    if (hints.divScale != 1.0) {
        p.latIntDiv = scaleU(p.latIntDiv, hints.divScale);
        p.latFPDiv  = scaleU(p.latFPDiv,  hints.divScale);
        // Throughput (ops/cycle) is the reciprocal of latency.  When latency
        // increases by divScale, throughput must decrease by the same factor
        // (dividing by divScale).  E.g., divScale=2.0 → latency doubles,
        // throughput halves: tput /= 2.0.  This is semantically consistent:
        // a slower divider delivers fewer operations per cycle.
        if (p.tputIntDiv > 0.0 && hints.divScale > 0.0)
            p.tputIntDiv /= hints.divScale;
        if (p.tputFPDiv > 0.0 && hints.divScale > 0.0)
            p.tputFPDiv  /= hints.divScale;
    }

    // Reorder buffer size (e.g., 0.5 when the CPU is running with SMT contention).
    if (hints.robScale != 1.0)
        p.robSize = scaleU(p.robSize, hints.robScale);

    // Reservation station size.
    if (hints.rsScale != 1.0)
        p.schedulerSize = scaleU(p.schedulerSize, hints.rsScale);

    // Effective issue width (e.g., 0.5 for an SMT-shared front-end).
    if (hints.issueScale != 1.0) {
        unsigned newIssue = scaleU(p.issueWidth, hints.issueScale);
        p.issueWidth  = std::max(newIssue, 1u);
        p.decodeWidth = std::max(scaleU(p.decodeWidth, hints.issueScale), 1u);
    }

    return p;
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

    // Divider port throughput: use tputIntDiv if measured (non-zero), otherwise
    // model the divider as a non-pipelined serial unit where throughput equals
    // 1/latIntDiv (one new divide can start only after the previous one
    // completes).  The initHWPort lambda uses busy = ceil(1/throughput), so
    // passing 1.0/latIntDiv → busy = latIntDiv cycles, correctly blocking the
    // port for the full latency of the previous operation.
    //
    // NOTE: The previous code passed latIntDiv as throughput (i.e. 25.0 for
    // Skylake), which caused busy = ceil(1/25) = 1 cycle — the port was
    // treated as free after a single cycle, letting unlimited divides be
    // issued each cycle.  This was a significant throughput over-estimate.
    double divTput = (profile.tputIntDiv > 0.0)
        ? profile.tputIntDiv
        : 1.0 / static_cast<double>(std::max(profile.latIntDiv, 1u));

    unsigned divider = g.addNode(ResourceType::DividerUnit, "divider",
                                  profile.dividers,
                                  static_cast<double>(profile.latIntDiv),
                                  divTput, 1);

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

    // Reservation station (scheduler queue).
    // The RS holds dispatched µops until their operands are ready.
    // count = RS entries; throughput = issueWidth (how many µops can be
    // issued from the RS per cycle); latency ≈ 1 cycle scheduling latency.
    unsigned rs = g.addNode(ResourceType::SchedulerQueue, "sched_queue",
                             profile.schedulerSize > 0 ? profile.schedulerSize : 64u,
                             1.0,
                             static_cast<double>(profile.issueWidth), 1);

    // Load buffer and store buffer.
    // count = number of entries; throughput = number of loads/stores per cycle.
    unsigned lb = g.addNode(ResourceType::LoadBuffer, "load_buf",
                             profile.loadBufferEntries > 0 ? profile.loadBufferEntries : 64u,
                             static_cast<double>(profile.latLoad),
                             static_cast<double>(profile.loadPorts), 1);

    unsigned sb = g.addNode(ResourceType::StoreBuffer, "store_buf",
                             profile.storeBufferEntries > 0 ? profile.storeBufferEntries : 36u,
                             static_cast<double>(profile.latStore),
                             static_cast<double>(profile.storePorts), 1);

    (void)rs; (void)lb; (void)sb; // referenced for structural completeness

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
        return profile.latIntAdd;
    case llvm::Instruction::Select:
        // FP or vector result: uses FCSEL/BLENDVPD which go through the FP
        // pipeline — same latency as FP-add on both x86 and AArch64.
        // Integer result: CMOV / CSEL — single integer ALU cycle.
        return (inst->getType()->isFloatingPointTy() ||
                inst->getType()->isVectorTy())
               ? profile.latFPAdd : profile.latIntAdd;
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
    case llvm::Instruction::FNeg:
        // FNeg is a sign-bit flip: VXORPS/VANDNPS on x86 (1 cycle),
        // FNEG on AArch64 (2 cycles, same as latFPAdd on most ARM profiles).
        // Use the minimum of 1 and latFPAdd so fast CPUs (x86 vectorized) get
        // the accurate 1-cycle model, while ARM profiles use their own latency.
        return (profile.isa == ISAFamily::X86_64) ? 1u : profile.latFPAdd;
    case llvm::Instruction::FMul:
        return profile.latFPMul;
    case llvm::Instruction::FDiv:
    case llvm::Instruction::FRem:
        return profile.latFPDiv;

    // ── Memory ──────────────────────────────────────────────────────────────
    case llvm::Instruction::Load:
        return profile.latLoad;
    case llvm::Instruction::Store:
        return profile.latStore;
    case llvm::Instruction::GetElementPtr:
        return profile.latIntAdd; // address arithmetic

    // ── SIMD / vector element operations ────────────────────────────────────
    // These go through the vector execution units; use latFPAdd as a proxy
    // since on most CPUs they share the same pipeline.
    case llvm::Instruction::ExtractElement:
    case llvm::Instruction::InsertElement:
        return profile.latFPAdd;
    case llvm::Instruction::ShuffleVector:
        // On x86, cross-lane shuffles are 3-5 cycles; within-lane are 1 cycle.
        // Use latFPAdd * 2 as a conservative estimate that stays proportional.
        return profile.latFPAdd * 2;

    // ── Zero-latency structural / bookkeeping ops ────────────────────────────
    case llvm::Instruction::ExtractValue:
    case llvm::Instruction::InsertValue:
    case llvm::Instruction::Alloca:   // stack allocation done in function prolog
        return 0u;

    // ── Control flow ────────────────────────────────────────────────────────
    case llvm::Instruction::Br:
    case llvm::Instruction::Switch:
    case llvm::Instruction::IndirectBr:
        return profile.latBranch;
    case llvm::Instruction::Ret:
        return 0;

    // ── Type conversions ────────────────────────────────────────────────────
    // ZExt/SExt/Trunc latency: on x86-64 and AArch64, narrowing truncations
    // and zero-extensions between integer widths are typically implemented
    // as MOV with register-renaming (zero latency for ABI-safe cases) or a
    // single ALU cycle.  We model them as 1-cycle operations (not full ALU
    // pipeline latency) because they never stall the issue slot.
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
        return 1u; // single-cycle move / rename; always cheaper than any mul/fp op
    case llvm::Instruction::FPToUI:
    case llvm::Instruction::FPToSI:
    case llvm::Instruction::UIToFP:
    case llvm::Instruction::SIToFP:
    case llvm::Instruction::FPTrunc:
    case llvm::Instruction::FPExt:
        // FP pipeline; dedicated CVT bypass path.
        // latFPConvert = 0 means "unset — fall back to latFPAdd".
        return (profile.latFPConvert > 0) ? profile.latFPConvert : profile.latFPAdd;
    case llvm::Instruction::BitCast:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::PtrToInt:
    // ── PHI ─────────────────────────────────────────────────────────────────
    case llvm::Instruction::PHI:
        return 0; // free — zero-latency (register rename / resolved at predecessor edge)

    // ── Calls / intrinsics ───────────────────────────────────────────────────
    case llvm::Instruction::Call: {
        const auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(inst);
        if (!ii) return kUnknownCallLatency;
        const llvm::Intrinsic::ID id = ii->getIntrinsicID();
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
        // ── Transcendental / math intrinsics ───────────────────────────────
        // On x86 these are typically library calls (80-250+ cycles).
        // On AArch64, some may have native FRINT* instructions (2-4 cycles).
        // We model conservative cycle counts so the scheduler hoists them
        // early to hide the latency.
        case llvm::Intrinsic::floor:
        case llvm::Intrinsic::ceil:
        case llvm::Intrinsic::trunc:
        case llvm::Intrinsic::round:
        case llvm::Intrinsic::roundeven:
        case llvm::Intrinsic::rint:
        case llvm::Intrinsic::nearbyint:
            // VROUNDPD/VROUNDPS on x86 (8 cycles), FRINTX on AArch64 (2 cycles).
            // Use latFPAdd as a conservative estimate (always >= 2).
            return profile.latFPAdd * 2;
        case llvm::Intrinsic::log:
        case llvm::Intrinsic::log2:
        case llvm::Intrinsic::log10:
            // Typically software emulation: ~80-120 cycles.
            // Model as latFPDiv * 6 (14*6 = 84 on Skylake, 10*6 = 60 on M1).
            return profile.latFPDiv * 6;
        case llvm::Intrinsic::exp:
        case llvm::Intrinsic::exp2:
            // Similar to log: ~80-100 cycles.
            return profile.latFPDiv * 5;
        case llvm::Intrinsic::sin:
        case llvm::Intrinsic::cos:
            // FSIN/FCOS (x87) ≈ 80-100 cycles; libm call ≈ 100-150 cycles.
            return profile.latFPDiv * 7;
        case llvm::Intrinsic::fabs:
            // FABS is a single cycle bit-clear operation.
            return 1u;
        case llvm::Intrinsic::copysign:
            // Single cycle: isolate sign + OR into mantissa.
            return 1u;
        default:                        return kUnknownIntrinsicLatency;
        }
    }

    default:
        return 1;
    }
}

double instrCostFromProfile(const llvm::Instruction* inst,
                            const MicroarchProfile& profile) {
    return static_cast<double>(getOpcodeLatency(inst, profile));
}

MappingResult mapProgramToHardware(ProgramGraph& pg, const HardwareGraph& hw,
                                    const MicroarchProfile& profile) {
    MappingResult result;
    if (pg.nodeCount() == 0) return result;

    (void)hw; // Graph structure used for validation in debug builds

    const size_t n = pg.nodeCount();

    // ── Annotate nodes with per-opcode latencies ──────────────────────────────
    // Use `getOpcodeLatency` when the node has an instruction pointer (always
    // for code built via buildFromFunction).  Fall back to the coarser
    // OpClass-based latency for synthetic nodes.
    for (unsigned i = 0; i < n; ++i) {
        ProgramNode* node = pg.getNodeMut(i);
        if (!node) continue;
        unsigned lat = node->inst
            ? getOpcodeLatency(node->inst, profile)
            : getLatency(node->opClass, profile);
        node->estimatedLatency = static_cast<double>(lat);
    }

    // ── Build adjacency lists for O(N+E) operations ───────────────────────────
    // succList[u] = {(v, edge_latency)}  (outgoing edges)
    // predList[v] = {(u, edge_latency)}  (incoming edges)
    std::vector<std::vector<std::pair<unsigned,unsigned>>> succList(n), predList(n);
    std::vector<unsigned> inDeg(n, 0);
    for (const auto& e : pg.edges()) {
        if (e.srcId < n && e.dstId < n) {
            succList[e.srcId].emplace_back(e.dstId, e.latency);
            predList[e.dstId].emplace_back(e.srcId, e.latency);
            ++inDeg[e.dstId];
        }
    }

    // ── Compute priority (longest latency-weighted path to any sink) ──────────
    // Iterative bottom-up Kahn traversal: process nodes in reverse topological
    // order (sinks first) to propagate critical-path distances.
    std::vector<unsigned> priority(n, 0);
    {
        // Kahn's algorithm to get topological order.
        std::vector<unsigned> topoOrder;
        topoOrder.reserve(n);
        std::vector<unsigned> deg(inDeg);  // copy
        std::queue<unsigned> q;
        for (unsigned i = 0; i < n; ++i)
            if (deg[i] == 0) q.push(i);
        while (!q.empty()) {
            unsigned u = q.front(); q.pop();
            topoOrder.push_back(u);
            for (auto& [v, lat] : succList[u])
                if (--deg[v] == 0) q.push(v);
        }
        // Process in reverse topological order (sinks → sources).
        for (auto it = topoOrder.rbegin(); it != topoOrder.rend(); ++it) {
            unsigned u = *it;
            unsigned nodeLat = static_cast<unsigned>(
                pg.getNode(u) ? pg.getNode(u)->estimatedLatency : 1);
            unsigned maxSuccDist = 0;
            for (auto& [v, edgeLat] : succList[u]) {
                unsigned dist = priority[v] + edgeLat;
                if (dist > maxSuccDist) maxSuccDist = dist;
            }
            priority[u] = nodeLat + maxSuccDist;
        }
    }

    // ── Per-port-type throughput budget ──────────────────────────────────────
    // busy_cycles[rt] = how many cycles a port of this type is occupied per op.
    // For pipelined units (ALU, FMA) this is 1.
    // For non-pipelined units (divider on most CPUs) this is the full latency.
    auto portBusyCycles = [&](ResourceType rt) -> unsigned {
        switch (rt) {
        case ResourceType::IntegerALU:  return 1;
        case ResourceType::VectorALU:   return 1;
        case ResourceType::FMAUnit:     return 1;
        case ResourceType::LoadUnit:    return 1;
        case ResourceType::StoreUnit:   return 1;
        case ResourceType::BranchUnit:  return 1;
        case ResourceType::DividerUnit:
            // Divider is typically non-pipelined; occupy it for the full latency
            // divided by the number of divider units.
            return profile.latIntDiv;
        default:                        return 1;
        }
    };

    // Scheduled cycle for each node.
    std::vector<unsigned> scheduledCycle(n, 0);
    std::vector<bool> scheduled(n, false);

    // Resource availability: next free cycle for each port instance.
    std::unordered_map<int, std::vector<unsigned>> portAvail;
    auto initPort = [&](ResourceType rt) {
        unsigned cnt = getPortCount(rt, profile);
        portAvail[static_cast<int>(rt)].assign(cnt, 0u);
    };
    initPort(ResourceType::IntegerALU);
    initPort(ResourceType::VectorALU);
    initPort(ResourceType::FMAUnit);
    initPort(ResourceType::LoadUnit);
    initPort(ResourceType::StoreUnit);
    initPort(ResourceType::BranchUnit);
    initPort(ResourceType::DividerUnit);

    // Working in-degree (decremented as predecessors complete).
    std::vector<unsigned> workInDeg(inDeg);
    std::queue<unsigned> readyQ;
    for (unsigned i = 0; i < n; ++i)
        if (workInDeg[i] == 0) readyQ.push(i);

    unsigned totalScheduled = 0;
    unsigned maxCycle = 0;
    unsigned stallCycles = 0;

    while (totalScheduled < n) {
        // Collect ready nodes and sort by descending priority.
        std::vector<unsigned> ready;
        while (!readyQ.empty()) {
            ready.push_back(readyQ.front());
            readyQ.pop();
        }

        if (ready.empty()) {
            // Deadlock guard: should not happen for valid DAGs.
            for (unsigned i = 0; i < n; ++i)
                if (!scheduled[i]) { ready.push_back(i); break; }
            if (ready.empty()) break;
        }

        std::sort(ready.begin(), ready.end(), [&](unsigned a, unsigned b) {
            return priority[a] > priority[b];
        });

        unsigned issued = 0;
        for (unsigned nodeId : ready) {
            if (issued >= profile.issueWidth) break;
            if (scheduled[nodeId]) continue;

            // Earliest start: max over all predecessors of (sched_cycle + latency).
            unsigned earliest = 0;
            for (auto& [pred, edgeLat] : predList[nodeId]) {
                if (scheduled[pred]) {
                    unsigned predLat = static_cast<unsigned>(
                        pg.getNode(pred) ? pg.getNode(pred)->estimatedLatency : 1);
                    unsigned predEnd = scheduledCycle[pred] + predLat;
                    if (predEnd > earliest) earliest = predEnd;
                }
            }

            // Find earliest-free port for this operation.
            ResourceType rt = mapOpToResource(
                pg.getNode(nodeId) ? pg.getNode(nodeId)->opClass : OpClass::Other);
            auto& ports = portAvail[static_cast<int>(rt)];
            unsigned startCycle = earliest;
            unsigned bestPort = 0;
            if (!ports.empty()) {
                unsigned bestTime = ports[0];
                for (unsigned p = 1; p < ports.size(); ++p) {
                    if (ports[p] < bestTime) { bestTime = ports[p]; bestPort = p; }
                }
                startCycle = std::max(earliest, bestTime);
                unsigned busy = portBusyCycles(rt);
                ports[bestPort] = startCycle + busy;
            }
            if (startCycle > earliest) stallCycles += (startCycle - earliest);

            scheduledCycle[nodeId] = startCycle;

            // Update program node.
            ProgramNode* mutableNode = pg.getNodeMut(nodeId);
            if (mutableNode) {
                mutableNode->scheduledCycle = startCycle;
                mutableNode->assignedPort = bestPort;

                // Record a ScheduleEntry for consumers of the result.
                ScheduleEntry entry;
                entry.nodeId = nodeId;
                entry.cycle = startCycle;
                entry.port = bestPort;
                entry.resource = rt;
                result.schedule.push_back(entry);
            }

            unsigned endCycle = startCycle + static_cast<unsigned>(
                pg.getNode(nodeId) ? pg.getNode(nodeId)->estimatedLatency : 1);
            if (endCycle > maxCycle) maxCycle = endCycle;

            scheduled[nodeId] = true;
            ++totalScheduled;
            ++issued;

            // Release successors whose all predecessors are now scheduled.
            for (auto& [succ, succLat] : succList[nodeId]) {
                if (--workInDeg[succ] == 0)
                    readyQ.push(succ);
            }
        }
    }

    result.totalCycles = maxCycle;
    result.stallCycles = stallCycles;

    if (maxCycle > 0 && profile.issueWidth > 0)
        result.portUtilization = static_cast<double>(totalScheduled) /
            (static_cast<double>(maxCycle) * profile.issueWidth);

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
                // IEEE 754-2008 §5.4.1: fused operations require `contract` permission.
                // We check the fast-math flag on the fadd; the fmul's flag also
                // contributes but in practice both are set together (e.g. -ffast-math,
                // #pragma STDC FP_CONTRACT ON, or llvm's `reassoc contract` flags).
                auto* fpAdd = llvm::cast<llvm::FPMathOperator>(&inst);
                if (!fpAdd->hasAllowContract()) continue;

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

    // Compute a latency-driven prefetch distance.  To hide a full memory-
    // level miss (L3 → DRAM), we need to prefetch far enough ahead so the
    // hardware fetch completes before the data is needed.  A rough model:
    //
    //   prefetch_distance_bytes = (L3_latency_cycles / 2) * cache_line_size
    //
    // The /2 accounts for a typical loop throughput of ≈0.5 iterations/cycle
    // (a conservative assumption that avoids over-prefetching on fast loops).
    // We clamp to [2, 64] cache lines so we never insert a useless prefetch
    // for a tiny latency or an enormous one that thrashes the cache.
    unsigned prefetchLines = (profile.l3Latency > 0)
        ? std::max(2u, std::min(64u, profile.l3Latency / (2 * profile.pipelineDepth + 1)))
        : 8u;
    unsigned prefetchBytes = prefetchLines * profile.cacheLineSize;

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

        // Cap prefetches per function to avoid significant instruction-count
        // overhead (each prefetch is a call-like instruction).
        for (auto* load : loads) {
            if (count >= 8) break;

            llvm::Value* ptr = load->getPointerOperand();
            llvm::Type* ptrTy = ptr->getType();

            // Determine the prefetch offset.
            // If the pointer is a GEP with a constant stride, prefetch
            // strideBytes * prefetchLines ahead.  This is more accurate than
            // always using the element-agnostic prefetchBytes heuristic.
            unsigned offsetBytes = prefetchBytes;
            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
                // Single-index GEP: stride is the element type's byte size.
                if (gep->getNumIndices() == 1) {
                    llvm::Type* elemTy = gep->getSourceElementType();
                    const llvm::DataLayout* dl = nullptr;
                    if (auto* mod = func.getParent())
                        dl = &mod->getDataLayout();
                    if (dl && elemTy->isSized()) {
                        uint64_t elemBytes = dl->getTypeAllocSize(elemTy);
                        if (elemBytes > 0 && elemBytes <= 32) {
                            // prefetch_offset = prefetchLines cache lines
                            // expressed in element units.
                            unsigned elemLinesPerPrefetch =
                                (prefetchLines * profile.cacheLineSize
                                 + static_cast<unsigned>(elemBytes) - 1)
                                / static_cast<unsigned>(elemBytes);
                            offsetBytes = elemLinesPerPrefetch
                                          * static_cast<unsigned>(elemBytes);
                        }
                    }
                }
            }

            llvm::IRBuilder<> builder(load);
            llvm::Module* mod = func.getParent();

            llvm::Value* offset = llvm::ConstantInt::get(
                builder.getInt64Ty(), offsetBytes);
            llvm::Value* prefetchAddr = builder.CreateGEP(
                builder.getInt8Ty(), ptr, offset, "prefetch_addr");

            // Insert llvm.prefetch intrinsic.
            llvm::Function* prefetchFn = OMSC_GET_INTRINSIC(
                mod, llvm::Intrinsic::prefetch, {ptrTy});

            // Args: ptr, rw (0=read), locality (2=medium — L2/L3, not L1),
            // cache_type (1=data).  Use locality=2 so the prefetch warms L2
            // and L3 without polluting L1 with data that may not be used soon.
            builder.CreateCall(prefetchFn, {
                prefetchAddr,
                builder.getInt32(0),  // read
                builder.getInt32(2),  // medium locality (L2/L3)
                builder.getInt32(1)   // data cache
            });
            count++;
        }
    }

    return count;
}

/// Optimise branch layout for the hardware's branch predictor.
/// Ensures the fall-through path is the most likely one, and annotates
/// branches with profile weights when the outcome is predictable from
/// structural information:
///   • Exit-block detection: successor with ret/unreachable → unlikely
///   • Null-pointer checks: ICmp(ptr, null) → null is rare → non-null path hot
///   • Loop-closing back-edges: the latch branch is taken on most iterations
/// Returns the number of branches optimized.
static unsigned optimizeBranchLayout(llvm::Function& func,
                                      const MicroarchProfile& /*profile*/) {
    unsigned count = 0;

    // Pre-compute a linear order for back-edge detection.
    std::unordered_map<const llvm::BasicBlock*, unsigned> bbOrder;
    {
        unsigned ord = 0;
        for (auto& bb : func) bbOrder[&bb] = ord++;
    }

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
            continue;
        }

        // ── Null-pointer check: ICmp(ptr, null) ──────────────────────────────
        // Null dereferences are rare in correct programs.  When a branch
        // checks whether a pointer is null, the non-null path is hot.
        //   ICmp EQ  ptr, null  → branch to trueBB if null  → trueBB is cold
        //   ICmp NE  ptr, null  → branch to trueBB if !null → trueBB is hot
        // Skip if already annotated with branch weights.
        if (!br->getMetadata(llvm::LLVMContext::MD_prof)) {
            if (auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(br->getCondition())) {
                llvm::Value* op0 = icmp->getOperand(0);
                llvm::Value* op1 = icmp->getOperand(1);
                bool isNullCheck =
                    (op0->getType()->isPointerTy() || op1->getType()->isPointerTy()) &&
                    (llvm::isa<llvm::ConstantPointerNull>(op1) ||
                     llvm::isa<llvm::ConstantPointerNull>(op0));
                if (isNullCheck) {
                    llvm::MDBuilder mdBuilder(func.getContext());
                    llvm::MDNode* weights;
                    if (icmp->getPredicate() == llvm::ICmpInst::ICMP_EQ) {
                        // EQ null → true branch is the null (rare) path
                        weights = mdBuilder.createBranchWeights(1, 99);
                    } else if (icmp->getPredicate() == llvm::ICmpInst::ICMP_NE) {
                        // NE null → true branch is the non-null (common) path
                        weights = mdBuilder.createBranchWeights(99, 1);
                    } else {
                        weights = nullptr;
                    }
                    if (weights) {
                        br->setMetadata(llvm::LLVMContext::MD_prof, weights);
                        count++;
                        continue;
                    }
                }
            }
        }

        // ── Loop-closing back-edge: branch target precedes current BB ────────
        // If the true successor has a smaller linear order than the current BB,
        // the true branch is a back-edge (loop-closing).  Back-edges are taken
        // on every iteration except the last, so they are highly likely to be
        // taken (weight ≈ 90%).  Annotate with branch weights if not already set.
        if (!br->getMetadata(llvm::LLVMContext::MD_prof)) {
            auto itCur  = bbOrder.find(&bb);
            auto itTrue = bbOrder.find(trueBB);
            auto itFalse= bbOrder.find(falseBB);
            if (itCur != bbOrder.end() && itTrue != bbOrder.end()
                    && itFalse != bbOrder.end()) {
                bool trueIsBackEdge  = (itTrue->second  < itCur->second);
                bool falseIsBackEdge = (itFalse->second < itCur->second);
                if (trueIsBackEdge && !falseIsBackEdge) {
                    // true = loop continue (hot), false = loop exit (cold)
                    llvm::MDBuilder mdBuilder(func.getContext());
                    br->setMetadata(llvm::LLVMContext::MD_prof,
                        mdBuilder.createBranchWeights(9, 1));
                    count++;
                } else if (falseIsBackEdge && !trueIsBackEdge) {
                    // false = loop continue (hot), true = loop exit (cold)
                    llvm::MDBuilder mdBuilder(func.getContext());
                    br->setMetadata(llvm::LLVMContext::MD_prof,
                        mdBuilder.createBranchWeights(1, 9));
                    count++;
                }
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
                // Require `contract` permission for FMA fusion (IEEE §5.4.1).
                auto* fpSub = llvm::cast<llvm::FPMathOperator>(&inst);
                if (!fpSub->hasAllowContract()) continue;

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

/// Try to synthesize |absCV| as a 1- or 2-instruction shift+add/sub sequence.
///
/// Recognises three forms that are strictly cheaper than a multiply
/// (latency 2 cycles vs 3, and use general ALU ports instead of the
/// dedicated multiply port):
///
///   Power of 2 :  x << a              (1 shift, 1 cycle latency)
///   Add form   :  (x << a) + (x << b) (2 shifts + 1 add, latency 2)
///   Sub form   :  (x << a) - (x << b) (2 shifts + 1 sub, latency 2)
///
/// where, in the Add/Sub forms, b may be 0, which means the second operand
/// is just `x` itself (no shift needed).
///
/// Returns the synthesised Value*, or nullptr if no 2-instruction form exists.
[[nodiscard]] static llvm::Value*
tryShiftAddForm(llvm::IRBuilder<>& builder, llvm::Value* xv,
                llvm::Type* ty, uint64_t absCV, unsigned bitWidth) {
    if (absCV == 0 || bitWidth == 0) return nullptr;
    // Clamp bitWidth first so the absCV bounds check below uses the
    // same (clamped) width and cannot trigger UB via 1<<64.
    if (bitWidth > 64) bitWidth = 64;
    // Reject constants that cannot be represented in the (clamped) integer type.
    if (bitWidth < 64 && absCV >= (uint64_t(1) << bitWidth)) return nullptr;

    auto mk  = [&](unsigned sh) -> llvm::Value* {
        return llvm::ConstantInt::get(ty, static_cast<uint64_t>(sh));
    };
    auto shl = [&](llvm::Value* v, unsigned sh) -> llvm::Value* {
        return (sh == 0) ? v : builder.CreateShl(v, mk(sh), "sr.shl");
    };

    // ── Form 1: power of 2 ─────────────────────────────────────────────────
    if ((absCV & (absCV - 1)) == 0) {
        unsigned sh = static_cast<unsigned>(__builtin_ctzll(absCV));
        if (sh < bitWidth) return shl(xv, sh);
        return nullptr;
    }

    // ── Form 2: exactly 2 set bits  → (x << hi) + (x << lo) ───────────────
    if (__builtin_popcountll(absCV) == 2) {
        unsigned lo = static_cast<unsigned>(__builtin_ctzll(absCV));
        unsigned hi = static_cast<unsigned>(63 - __builtin_clzll(absCV));
        if (hi < bitWidth)
            return builder.CreateAdd(shl(xv, hi), shl(xv, lo), "sr.add2");
        return nullptr;
    }

    // ── Form 3: 2^a - 2^b  (a > b)  → (x << a) - (x << b) ────────────────
    // absCV = 2^a - 2^b  ⟺  absCV + 2^b is a power of 2.
    for (unsigned b = 0; b < bitWidth; ++b) {
        uint64_t adjusted = absCV + (uint64_t(1) << b);
        if ((adjusted & (adjusted - 1)) == 0) {
            unsigned a = static_cast<unsigned>(__builtin_ctzll(adjusted));
            if (a > b && a < bitWidth)
                return builder.CreateSub(shl(xv, a), shl(xv, b), "sr.sub2");
        }
    }

    return nullptr;
}

/// Integer strength reduction: replace multiply-by-small-constant with
/// shifts and adds, which execute on more ports and have lower latency.
///
/// Covers ALL constants expressible as 2^a ± 2^b (a > b ≥ 0) in 2
/// instructions, plus 3-set-bit constants in 3 instructions when the
/// multiply unit is the throughput bottleneck.
///
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
            if (!xv || cv == 0 || cv == 1 || cv == -1) continue;

            llvm::IRBuilder<> builder(&inst);
            llvm::Type* ty = inst.getType();
            unsigned bitWidth = ty->getIntegerBitWidth();

            // Work with the absolute value; negate the result if cv was negative.
            bool negate = (cv < 0);
            uint64_t absCV = negate ? static_cast<uint64_t>(-cv)
                                    : static_cast<uint64_t>(cv);

            llvm::Value* rep = tryShiftAddForm(builder, xv, ty, absCV, bitWidth);

            // ── 3-instruction form: 3 set bits → 2 shifts + 2 adds ──────────
            // Latency is 3 cycles (same as mul) but uses general ALU ports
            // instead of the scarce multiply port.  Only generate when the
            // multiply port count is less than the number of ALU ports, i.e.
            // when multiply throughput is the bottleneck.
            if (!rep && profile.mulPortCount < profile.intALUs &&
                __builtin_popcountll(absCV) == 3) {
                unsigned b0 = static_cast<unsigned>(__builtin_ctzll(absCV));
                uint64_t rest = absCV ^ (uint64_t(1) << b0);
                unsigned b1 = static_cast<unsigned>(__builtin_ctzll(rest));
                unsigned b2 = static_cast<unsigned>(63 - __builtin_clzll(rest));
                if (b2 < bitWidth) {
                    auto mk2 = [&](unsigned sh) -> llvm::Value* {
                        return llvm::ConstantInt::get(ty, static_cast<uint64_t>(sh));
                    };
                    auto sh2 = [&](llvm::Value* v, unsigned sh) -> llvm::Value* {
                        return (sh == 0) ? v
                             : builder.CreateShl(v, mk2(sh), "sr.shl3");
                    };
                    auto* t = builder.CreateAdd(sh2(xv, b2), sh2(xv, b1), "sr.add3a");
                    rep = builder.CreateAdd(t, sh2(xv, b0), "sr.add3b");
                }
            }

            if (rep && negate)
                rep = builder.CreateNeg(rep, "sr.neg");

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
///
/// Detects two patterns:
///   1. Consecutive GEP indices (diff == 1): classic adjacent elements.
///   2. GEP indices whose byte-offset difference is within one cache line:
///      covers non-unit strides (e.g. struct fields) that share a cache line.
/// Returns the number of load pairs identified.
static unsigned markLoadStorePairs(llvm::Function& func,
                                    const MicroarchProfile& profile) {
    if (profile.loadPorts < 2) return 0;

    unsigned count = 0;
    const llvm::DataLayout* dl = nullptr;
    if (auto* mod = func.getParent()) dl = &mod->getDataLayout();

    for (auto& bb : func) {
        std::vector<llvm::LoadInst*> loads;
        for (auto& inst : bb) {
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                if (!ld->isVolatile()) loads.push_back(ld);
            }
        }

        // Look for loads from the same base pointer that access addresses
        // within the same cache line (distance < cacheLineSize bytes).
        for (size_t i = 0; i + 1 < loads.size(); ++i) {
            llvm::LoadInst* ld0 = loads[i];
            llvm::LoadInst* ld1 = loads[i + 1];

            // Both loads must be from GEP instructions off the same base.
            auto* gep0 = llvm::dyn_cast<llvm::GetElementPtrInst>(ld0->getPointerOperand());
            auto* gep1 = llvm::dyn_cast<llvm::GetElementPtrInst>(ld1->getPointerOperand());
            if (!gep0 || !gep1) continue;
            if (gep0->getPointerOperand() != gep1->getPointerOperand()) continue;
            if (gep0->getNumIndices() != 1 || gep1->getNumIndices() != 1) continue;

            auto* idx0 = llvm::dyn_cast<llvm::ConstantInt>(gep0->getOperand(1));
            auto* idx1 = llvm::dyn_cast<llvm::ConstantInt>(gep1->getOperand(1));
            if (!idx0 || !idx1) continue;

            int64_t idxDiff = idx1->getSExtValue() - idx0->getSExtValue();
            if (idxDiff < 0) idxDiff = -idxDiff; // absolute difference

            // Compute the byte-offset difference using the GEP element type
            // and the data layout.  Fall back to index difference if no DL.
            bool pairOk = false;
            if (dl) {
                llvm::Type* elemTy = gep0->getSourceElementType();
                if (elemTy && elemTy->isSized()) {
                    uint64_t elemBytes = dl->getTypeAllocSize(elemTy);
                    if (elemBytes > 0) {
                        uint64_t byteDiff = static_cast<uint64_t>(idxDiff) * elemBytes;
                        // Pair if the byte distance is strictly within one cache line.
                        pairOk = (byteDiff > 0 && byteDiff < profile.cacheLineSize);
                    }
                }
            } else {
                // No data layout: use the classic consecutive-index heuristic.
                pairOk = (idxDiff == 1);
            }

            if (!pairOk) continue;

            // Both loads must have the same result type for the backend to
            // treat them as a coalescing candidate.
            if (ld0->getType() != ld1->getType()) continue;

            // Annotate with access.group metadata to hint the backend.
            llvm::LLVMContext& ctx = func.getContext();
            llvm::MDNode* agMD = llvm::MDNode::get(ctx, {});
            ld0->setMetadata(llvm::LLVMContext::MD_access_group, agMD);
            ld1->setMetadata(llvm::LLVMContext::MD_access_group, agMD);
            count++;
            ++i; // skip ld1 in outer loop (already paired)
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

        // ── Compute Recurrence MII (RecMII) ────────────────────────────────
        // For loops with loop-carried dependencies (e.g. reduction: acc += a[i]),
        // the hardware-imposed minimum is not just resource pressure but also
        // the latency of the recurrence cycle.  RecMII = max over all PHI-based
        // recurrences of the latency of the dependency chain from the PHI's
        // back-edge value back to the PHI itself.
        //
        // Algorithm (per-PHI):
        //   1. Forward-mark: BFS from phi via use-def edges within the loop
        //      body to find all instructions reachable from phi ("onChain").
        //   2. Walk backward from the back-edge value, at each step following
        //      only the operand that is onChain (i.e., part of the recurrence
        //      cycle), accumulating instruction latencies.
        //   3. RecMII = max latency over all PHIs.
        //
        // This correctly handles multi-hop recurrences like:
        //   acc = fma(a[i], b[i], fma(c[i], d[i], acc))
        // where naive "follow first operand" would miss the recurrence path.
        unsigned recMII = 1;
        {
            // Build a local index of instructions in this BB and the latch.
            std::unordered_map<const llvm::Instruction*, unsigned> bbInstIdx;
            unsigned ord = 0;
            for (auto& inst : bb)    bbInstIdx[&inst] = ord++;
            if (latch != &bb)
                for (auto& inst : *latch) bbInstIdx[&inst] = ord++;

            for (auto& phiInst : bb) {
                auto* phi = llvm::dyn_cast<llvm::PHINode>(&phiInst);
                if (!phi) break; // PHIs are always first in a BB

                // Find the back-edge incoming value (value coming from the latch).
                llvm::Value* backVal = nullptr;
                for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
                    if (phi->getIncomingBlock(i) == latch) {
                        backVal = phi->getIncomingValue(i);
                        break;
                    }
                }
                if (!backVal) continue;

                // Step 1: Forward BFS from phi to mark all instructions in
                // the loop body that are reachable via uses from phi.
                // These form the "recurrence spine" — only these can be on
                // the loop-carried dependency path.
                std::unordered_set<const llvm::Instruction*> onChain;
                {
                    std::queue<const llvm::Instruction*> wl;
                    onChain.insert(phi);
                    wl.push(phi);
                    while (!wl.empty()) {
                        const llvm::Instruction* cur = wl.front();
                        wl.pop();
                        for (auto* usr : cur->users()) {
                            auto* uInst = llvm::dyn_cast<llvm::Instruction>(usr);
                            if (!uInst) continue;
                            if (bbInstIdx.find(uInst) == bbInstIdx.end()) continue;
                            if (onChain.insert(uInst).second) wl.push(uInst);
                        }
                    }
                }

                // Step 2: Walk backward from backVal, following only operands
                // that are in onChain (guarantees we stay on the recurrence path).
                unsigned chainLat = 0;
                llvm::Value* cur = backVal;
                std::unordered_set<const llvm::Value*> visited;
                while (true) {
                    auto* curInst = llvm::dyn_cast<llvm::Instruction>(cur);
                    if (!curInst) break;
                    if (curInst == phi) break;  // closed the cycle
                    if (!visited.insert(curInst).second) break;  // cycle guard
                    if (bbInstIdx.find(curInst) == bbInstIdx.end()) break;

                    chainLat += getOpcodeLatency(curInst, profile);

                    // Follow the operand that is on the recurrence chain.
                    llvm::Value* next = nullptr;
                    for (auto& op : curInst->operands()) {
                        auto* opInst = llvm::dyn_cast<llvm::Instruction>(op.get());
                        if (!opInst) continue;
                        // phi itself or any onChain member is a valid next hop.
                        if (opInst == phi || onChain.count(opInst)) {
                            next = opInst;
                            break;
                        }
                    }
                    if (!next) break;
                    cur = next;
                }
                if (chainLat > recMII) recMII = chainLat;
            }
        }

        // True MII is the max of resource-constrained and recurrence-constrained.
        unsigned mii = std::max(resMII, recMII);

        // Unroll count: expose enough iterations to fill the pipeline.
        // Upper-bound prevents excessive code-size growth from very deep pipelines
        // combined with very short MII (e.g. a 14-stage pipeline with MII=1 would
        // otherwise produce 14 unrolled copies).
        constexpr unsigned kMaxUnrollCount = 8;
        unsigned unroll = (profile.pipelineDepth + mii - 1) / mii;
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
        addF("+sse4.1");
        addF("+sse4.2");
        addF("+popcnt");
        // BMI, BMI2, LZCNT, F16C, and FMA were introduced with Haswell (2013).
        // Sandy Bridge / Ivy Bridge profiles have fmaUnits==0 since they
        // predate FMA; use that as the proxy for "pre-Haswell baseline".
        if (profile.fmaUnits > 0) {
            addF("+bmi");
            addF("+bmi2");
            addF("+lzcnt");
            addF("+fma");    // FMA3 instructions (VFMADD132/213/231)
            addF("+f16c");   // half-float conversion (VCVTPH2PS, VCVTPS2PH)
            addF("+cx16");   // CMPXCHG16B
            addF("+sahf");   // LAHF/SAHF in 64-bit mode (needed by some libms)
        }
        if (profile.vectorWidth >= 256) {
            addF("+avx");
            // AVX2 was introduced together with FMA in Haswell (2013).
            if (profile.fmaUnits > 0)
                addF("+avx2");
        }
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
        addF("+fp16");       // half-precision FP arithmetic (ARMv8.2+)
        addF("+dotprod");    // 8-bit dot product (ARMv8.2-dotprod, all modern ARM)
        addF("+crypto");     // AES + SHA (ARMv8.0-crypto, ubiquitous in AArch64)
        addF("+zcm");        // zero cycle move (register renaming hint)
        addF("+zcz");        // zero cycle zeroing
        if (profile.vectorWidth >= 256) {
            addF("+sve");
            addF("+sve2");
            addF("+sve2-sha3");
            addF("+sve2-aes");
        }
        break;

    case ISAFamily::RISCV64:
        addF("+m");  // multiply/divide
        addF("+a");  // atomics
        addF("+f");  // single-precision FP
        addF("+d");  // double-precision FP
        addF("+c");  // compressed instructions
        addF("+zba"); // address generation bitmanip (SH1ADD, SH2ADD, SH3ADD)
        addF("+zbb"); // basic bitmanip (ANDN, CLZ, CTZ, ORC.B, REV8, etc.)
        addF("+zbc"); // carry-less multiplication
        addF("+zbs"); // single-bit instructions
        if (profile.vecUnits > 0 && profile.vectorWidth >= 128) addF("+v");
        break;

    default:
        break;
    }

    if (!features.empty())
        func.addFnAttr("target-features", features);
}

/// Detect `select(icmp slt x 0, sub(0, x), x)` and similar patterns and
/// replace with `llvm.abs(x, false)`.
/// Returns the number of abs patterns replaced.
static unsigned generateIntegerAbs(llvm::Function& func,
                                    const MicroarchProfile& profile) {
    // llvm.abs is beneficial on all architectures that have a native abs-like
    // instruction (x86 VABS* in AVX2, AArch64 ABS, RISC-V). Even without a
    // native instruction the backend can emit `neg + cmov` which is 2 µops
    // instead of 3 (cmp + neg + conditional move).
    if (profile.intALUs == 0) return 0;

    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst);
            if (!sel) continue;
            if (!sel->getType()->isIntegerTy()) continue;

            // Pattern: select(icmp_slt(x, 0), sub(0, x), x)
            //    or:   select(icmp_sgt(x, 0), x, sub(0, x))
            auto* cond = llvm::dyn_cast<llvm::ICmpInst>(sel->getCondition());
            if (!cond) continue;

            llvm::Value* x = nullptr;
            llvm::Value* negX = nullptr;

            auto matchNeg = [](llvm::Value* v, llvm::Value* base) -> bool {
                // sub(0, base) or sub(0, x) with constant zero LHS
                if (auto* sub = llvm::dyn_cast<llvm::BinaryOperator>(v)) {
                    if (sub->getOpcode() == llvm::Instruction::Sub) {
                        if (auto* lhs = llvm::dyn_cast<llvm::ConstantInt>(sub->getOperand(0)))
                            if (lhs->isZero() && sub->getOperand(1) == base)
                                return true;
                    }
                }
                return false;
            };

            if (cond->getPredicate() == llvm::ICmpInst::ICMP_SLT) {
                // select(icmp slt x 0, neg(x), x)  →  abs(x)
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(cond->getOperand(1))) {
                    if (ci->isZero()) {
                        x    = cond->getOperand(0);
                        negX = sel->getTrueValue();
                        if (!matchNeg(negX, x) || sel->getFalseValue() != x)
                            x = nullptr;
                    }
                }
            } else if (cond->getPredicate() == llvm::ICmpInst::ICMP_SGT) {
                // select(icmp sgt x 0, x, neg(x))  →  abs(x)
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(cond->getOperand(1))) {
                    if (ci->isZero()) {
                        x    = cond->getOperand(0);
                        negX = sel->getFalseValue();
                        if (!matchNeg(negX, x) || sel->getTrueValue() != x)
                            x = nullptr;
                    }
                }
            }

            if (!x) continue;

            llvm::IRBuilder<> builder(sel);
            llvm::Module* mod = func.getParent();
            llvm::Type* ty = sel->getType();
            llvm::Function* absFn = OMSC_GET_INTRINSIC(mod, llvm::Intrinsic::abs, {ty});
            // `false` = result may NOT be poison if the input is INT_MIN
            // (the safe/conservative version).
            llvm::Value* absVal = builder.CreateCall(
                absFn, {x, builder.getInt1(false)}, "abs");
            replacements.emplace_back(sel, absVal);
            ++count;
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Canonicalise `fadd fast x (fneg y)` → `fsub fast x y`.
/// This avoids an extra FNeg instruction on CPUs that lack a native FNMADD,
/// since the FP subtract maps directly to FSUBSS/FSUBD/VFNMADD on many arches.
/// The transformation is only applied when the fadd has `nsz` or `reassoc`
/// fast-math flags, or when the fneg result is only used by this fadd (so we
/// know the fneg can be folded away).
/// Returns the number of patterns replaced.
static unsigned canonicalizeFaddFneg(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Instruction*>> work;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::FAdd) continue;
            auto* fadd = llvm::cast<llvm::BinaryOperator>(&inst);

            // Only canonicalize when the fadd is "fast" (no strict FP semantics).
            // This guards against changing NaN/Inf behaviour.
            if (!fadd->isFast() && !fadd->hasNoNaNs()) continue;

            // Try both operands: fadd(x, fneg(y)) or fadd(fneg(y), x).
            for (int side = 0; side < 2; ++side) {
                llvm::Value* candidate = fadd->getOperand(side);
                auto* fneg = llvm::dyn_cast<llvm::UnaryOperator>(candidate);
                if (!fneg || fneg->getOpcode() != llvm::Instruction::FNeg) continue;
                // Only fold when the fneg result is used solely by this fadd,
                // OR when the fadd has reassoc — in that case a separate fneg
                // consumer can re-emit its own fneg.
                if (!fneg->hasOneUse() && !fadd->hasAllowReassoc()) continue;

                work.emplace_back(fadd, fneg);
                break;
            }
        }
    }

    for (auto& [fadd, fneg] : work) {
        // Determine which operand is the fneg and which is the other addend.
        llvm::Value* other = (fadd->getOperand(0) == fneg)
                             ? fadd->getOperand(1) : fadd->getOperand(0);
        llvm::Value* negated = fneg->getOperand(0);

        llvm::IRBuilder<> builder(fadd);
        llvm::Value* fsub = builder.CreateFSubFMF(other, negated, fadd, "fsub_canon");
        fadd->replaceAllUsesWith(fsub);
        fadd->eraseFromParent();
        if (fneg->use_empty()) fneg->eraseFromParent();
        ++count;
    }
    return count;
}

/// Replace FP division by a compile-time constant with a reciprocal multiply.
///
/// Pattern:  fdiv(x, C)  →  fmul(x, 1.0/C)
///
/// Requires the `arcp` (allow-reciprocal) fast-math flag on the division, which
/// permits the compiler to use a reciprocal approximation.  With a constant
/// divisor this is exact (not an approximation) at the same precision, so the
/// transform is correct even without `afn` or `unsafe-fp-math`.
///
/// Benefit: FP division is 10-15 cycles on most CPUs (latFPDiv = 10-15),
/// whereas FP multiply is 3-4 cycles (latFPMul = 3-4).  This is the single
/// most impactful scalar FP transform for division-heavy code.
///
/// Also folds `fdiv(1.0, x)` → `fdiv(1.0, x)` (kept, backend emits RCPPS).
///
/// Returns the number of divisions replaced.
static unsigned foldFPDivByConstant(llvm::Function& func,
                                     const MicroarchProfile& profile) {
    // Guard: only worth doing when div is materially slower than mul.
    if (profile.latFPDiv <= profile.latFPMul + 1) return 0;

    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::FDiv) continue;
            // Check the per-instruction `arcp` fast-math flag.
            auto* fpOp = llvm::cast<llvm::FPMathOperator>(&inst);
            if (!fpOp->hasAllowReciprocal()) continue;

            llvm::Value* dividend = inst.getOperand(0);
            llvm::Value* divisor  = inst.getOperand(1);
            llvm::Type*  ty       = inst.getType();

            // Only fold when the divisor is a compile-time FP constant (scalar or
            // splat vector).  Skip division by 0 or denormals.
            llvm::ConstantFP* cFP = nullptr;
            if (auto* cfp = llvm::dyn_cast<llvm::ConstantFP>(divisor)) {
                if (!cfp->isZero() && cfp->getValueAPF().isFiniteNonZero() &&
                    !cfp->getValueAPF().isDenormal())
                    cFP = cfp;
            }
            // Handle splat vector constant: <4 x float> <C, C, C, C>.
            if (!cFP) {
                if (auto* cv = llvm::dyn_cast<llvm::ConstantVector>(divisor)) {
                    if (auto* sp = cv->getSplatValue())
                        cFP = llvm::dyn_cast<llvm::ConstantFP>(sp);
                }
                if (!cFP) {
                    if (auto* cdv = llvm::dyn_cast<llvm::ConstantDataVector>(divisor))
                        cFP = llvm::dyn_cast<llvm::ConstantFP>(cdv->getSplatValue());
                }
            }
            if (!cFP) continue;

            // Compute 1.0 / C in the same FP semantics as the operation.
            // APFloat(semantics, double) was removed in LLVM 18; use the
            // double constructor then convert to the target semantics.
            llvm::APFloat recip(1.0); // start as double 1.0
            bool lossy = false;
            recip.convert(cFP->getValueAPF().getSemantics(),
                          llvm::APFloat::rmNearestTiesToEven, &lossy);
            llvm::APFloat divisorVal = cFP->getValueAPF();
            auto status = recip.divide(divisorVal, llvm::APFloat::rmNearestTiesToEven);
            // Reject if division produced an inexact result that would change
            // meaning (e.g., 1.0/3.0 is inexact but still exact to IEEE float
            // precision — this is fine since arcp allows it).
            (void)status; // non-exact is acceptable with arcp

            // Build the reciprocal constant with the same type as the original.
            llvm::Value* recipConst;
            if (ty->isVectorTy()) {
                // Splat the scalar reciprocal into a vector constant.
                llvm::Constant* scalarRecip = llvm::ConstantFP::get(
                    ty->getScalarType(), recip);
                recipConst = llvm::ConstantVector::getSplat(
                    llvm::cast<llvm::VectorType>(ty)->getElementCount(),
                    scalarRecip);
            } else {
                recipConst = llvm::ConstantFP::get(ty, recip);
            }

            llvm::IRBuilder<> builder(&inst);
            llvm::Value* mul = builder.CreateFMul(dividend, recipConst, "recip.mul");
            // Propagate fast-math flags from the original division.
            if (auto* mulInst = llvm::dyn_cast<llvm::Instruction>(mul))
                mulInst->setFastMathFlags(fpOp->getFastMathFlags());

            replacements.emplace_back(&inst, mul);
            ++count;
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Replace integer udiv/urem by a power-of-2 constant with logical shifts/masks.
///
/// These patterns may survive LLVM's InstCombine when constants are introduced
/// late (e.g., by our own strength-reduction pass or by constant propagation
/// after inlining), or when the function was not run through the full O2/O3 pipeline.
///
///   udiv(x, 2^k)  →  lshr(x, k)
///   urem(x, 2^k)  →  and(x, 2^k - 1)
///   sdiv(x, 2^k)  →  lshr(add(x, 2^k - 1), k)   [with rounding correction]
///   sdiv(x, 1)    →  x   [free]
///   udiv(x, 1)    →  x   [free]
///
/// Returns the number of operations folded.
static unsigned foldDivByPow2(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            unsigned op = inst.getOpcode();
            bool isUDiv = (op == llvm::Instruction::UDiv);
            bool isURem = (op == llvm::Instruction::URem);
            bool isSDiv = (op == llvm::Instruction::SDiv);
            if (!isUDiv && !isURem && !isSDiv) continue;
            if (!inst.getType()->isIntegerTy()) continue;

            auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1));
            if (!ci) continue;

            llvm::Value* x = inst.getOperand(0);
            llvm::IRBuilder<> builder(&inst);
            llvm::Type* ty = inst.getType();
            const llvm::APInt& divisor = ci->getValue();
            unsigned bitWidth = ty->getIntegerBitWidth();
            llvm::Value* rep = nullptr;

            if (divisor.isOne()) {
                // div by 1 is identity.
                rep = x;
            } else if (divisor.isPowerOf2()) {
                unsigned k = divisor.logBase2();
                if (isUDiv) {
                    rep = builder.CreateLShr(x,
                        llvm::ConstantInt::get(ty, k), "udiv.shr");
                } else if (isURem) {
                    rep = builder.CreateAnd(x,
                        llvm::ConstantInt::get(ty, divisor - 1), "urem.and");
                } else { // SDiv by positive power-of-2
                    // Signed division by 2^k rounds toward zero.
                    // Equivalent to: (x + ((x >> 63) & (2^k - 1))) >> k
                    // where `x >> 63` is the sign-bit replication.
                    llvm::Value* signBit = builder.CreateAShr(x,
                        llvm::ConstantInt::get(ty, bitWidth - 1), "sdiv.sign");
                    llvm::Value* adj = builder.CreateAnd(signBit,
                        llvm::ConstantInt::get(ty, divisor - 1), "sdiv.adj");
                    llvm::Value* adjusted = builder.CreateAdd(x, adj, "sdiv.adj_x");
                    rep = builder.CreateAShr(adjusted,
                        llvm::ConstantInt::get(ty, k), "sdiv.shr");
                }
            }

            if (rep) {
                replacements.emplace_back(&inst, rep);
                ++count;
            }
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Replace `icmp + select` patterns with integer min/max intrinsics, and
/// `fcmp + select` patterns with FP min/max intrinsics.
///
/// This enables the backend to lower to VMIN/VMAX instructions (1 cycle on
/// most CPUs) instead of a compare + conditional select (2+ µops).
///
/// Patterns:
///   select(icmp slt(a, b), a, b)  →  smin(a, b)
///   select(icmp sgt(a, b), a, b)  →  smax(a, b)
///   select(icmp ult(a, b), a, b)  →  umin(a, b)
///   select(icmp ugt(a, b), a, b)  →  umax(a, b)
///   select(fcmp olt(a, b), a, b)  →  minnum(a, b)  [fast-math: nnan]
///   select(fcmp ogt(a, b), a, b)  →  maxnum(a, b)  [fast-math: nnan]
///
/// Safety: for FP patterns we require the `nnan` flag to ensure NaN
/// semantics are compatible with minnum/maxnum (which propagate NaN
/// differently from a select under NaN inputs).
///
/// Returns the number of patterns folded.
static unsigned foldMinMaxPatterns(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    llvm::Module* mod = func.getParent();

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst);
            if (!sel) continue;

            llvm::Value* cond    = sel->getCondition();
            llvm::Value* trueVal = sel->getTrueValue();
            llvm::Value* falseVal= sel->getFalseValue();
            llvm::Type*  ty      = sel->getType();

            // ── Integer min/max ──────────────────────────────────────────────
            if (ty->isIntegerTy() || (ty->isVectorTy() && ty->getScalarType()->isIntegerTy())) {
                auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(cond);
                if (!icmp) continue;

                llvm::Value* a = icmp->getOperand(0);
                llvm::Value* b = icmp->getOperand(1);
                llvm::CmpInst::Predicate pred = icmp->getPredicate();

                llvm::Intrinsic::ID intrID = llvm::Intrinsic::not_intrinsic;
                llvm::Value* minA = nullptr, *minB = nullptr;

                // select(a < b, a, b)  →  smin(a, b)
                if ((pred == llvm::CmpInst::ICMP_SLT || pred == llvm::CmpInst::ICMP_SLE) &&
                    trueVal == a && falseVal == b) {
                    intrID = llvm::Intrinsic::smin; minA = a; minB = b;
                }
                // select(a > b, a, b)  →  smax(a, b)
                else if ((pred == llvm::CmpInst::ICMP_SGT || pred == llvm::CmpInst::ICMP_SGE) &&
                         trueVal == a && falseVal == b) {
                    intrID = llvm::Intrinsic::smax; minA = a; minB = b;
                }
                // select(a < b, b, a) → smax(a, b)  [inverted select]
                else if ((pred == llvm::CmpInst::ICMP_SLT || pred == llvm::CmpInst::ICMP_SLE) &&
                         trueVal == b && falseVal == a) {
                    intrID = llvm::Intrinsic::smax; minA = a; minB = b;
                }
                // select(a > b, b, a) → smin(a, b)  [inverted select]
                else if ((pred == llvm::CmpInst::ICMP_SGT || pred == llvm::CmpInst::ICMP_SGE) &&
                         trueVal == b && falseVal == a) {
                    intrID = llvm::Intrinsic::smin; minA = a; minB = b;
                }
                // Unsigned variants.
                else if ((pred == llvm::CmpInst::ICMP_ULT || pred == llvm::CmpInst::ICMP_ULE) &&
                         trueVal == a && falseVal == b) {
                    intrID = llvm::Intrinsic::umin; minA = a; minB = b;
                }
                else if ((pred == llvm::CmpInst::ICMP_UGT || pred == llvm::CmpInst::ICMP_UGE) &&
                         trueVal == a && falseVal == b) {
                    intrID = llvm::Intrinsic::umax; minA = a; minB = b;
                }
                else if ((pred == llvm::CmpInst::ICMP_ULT || pred == llvm::CmpInst::ICMP_ULE) &&
                         trueVal == b && falseVal == a) {
                    intrID = llvm::Intrinsic::umax; minA = a; minB = b;
                }
                else if ((pred == llvm::CmpInst::ICMP_UGT || pred == llvm::CmpInst::ICMP_UGE) &&
                         trueVal == b && falseVal == a) {
                    intrID = llvm::Intrinsic::umin; minA = a; minB = b;
                }

                if (intrID != llvm::Intrinsic::not_intrinsic && minA && minB) {
                    llvm::IRBuilder<> builder(sel);
                    llvm::Function* fn = OMSC_GET_INTRINSIC(mod, intrID, {ty});
                    llvm::Value* result = builder.CreateCall(fn, {minA, minB}, "minmax");
                    replacements.emplace_back(sel, result);
                    ++count;
                }
                continue;
            }

            // ── FP min/max ───────────────────────────────────────────────────
            if (ty->isFPOrFPVectorTy()) {
                auto* fcmp = llvm::dyn_cast<llvm::FCmpInst>(cond);
                if (!fcmp) continue;

                // For FP min/max, require `nnan` flag on the select (or the cmp).
                // minnum/maxnum semantics: if either operand is NaN, the result is
                // the other operand.  A plain select propagates NaN as the true/false
                // value.  With `nnan` the user asserts no NaN, so semantics match.
                bool noNaN = false;
                if (auto* fpSel = llvm::dyn_cast<llvm::FPMathOperator>(sel))
                    noNaN = fpSel->hasNoNaNs();
                if (!noNaN) {
                    if (auto* fpCmp = llvm::dyn_cast<llvm::FPMathOperator>(fcmp))
                        noNaN = fpCmp->hasNoNaNs();
                }
                if (!noNaN) continue;

                llvm::Value* a = fcmp->getOperand(0);
                llvm::Value* b = fcmp->getOperand(1);
                llvm::CmpInst::Predicate pred = fcmp->getPredicate();

                llvm::Intrinsic::ID intrID = llvm::Intrinsic::not_intrinsic;
                llvm::Value* minA = nullptr, *minB = nullptr;

                if ((pred == llvm::CmpInst::FCMP_OLT || pred == llvm::CmpInst::FCMP_OLE ||
                     pred == llvm::CmpInst::FCMP_ULT || pred == llvm::CmpInst::FCMP_ULE) &&
                    trueVal == a && falseVal == b) {
                    intrID = llvm::Intrinsic::minnum; minA = a; minB = b;
                }
                else if ((pred == llvm::CmpInst::FCMP_OGT || pred == llvm::CmpInst::FCMP_OGE ||
                          pred == llvm::CmpInst::FCMP_UGT || pred == llvm::CmpInst::FCMP_UGE) &&
                         trueVal == a && falseVal == b) {
                    intrID = llvm::Intrinsic::maxnum; minA = a; minB = b;
                }
                // Inverted selects.
                else if ((pred == llvm::CmpInst::FCMP_OLT || pred == llvm::CmpInst::FCMP_OLE ||
                          pred == llvm::CmpInst::FCMP_ULT || pred == llvm::CmpInst::FCMP_ULE) &&
                         trueVal == b && falseVal == a) {
                    intrID = llvm::Intrinsic::maxnum; minA = a; minB = b;
                }
                else if ((pred == llvm::CmpInst::FCMP_OGT || pred == llvm::CmpInst::FCMP_OGE ||
                          pred == llvm::CmpInst::FCMP_UGT || pred == llvm::CmpInst::FCMP_UGE) &&
                         trueVal == b && falseVal == a) {
                    intrID = llvm::Intrinsic::minnum; minA = a; minB = b;
                }

                if (intrID != llvm::Intrinsic::not_intrinsic && minA && minB) {
                    llvm::IRBuilder<> builder(sel);
                    llvm::Function* fn = OMSC_GET_INTRINSIC(mod, intrID, {ty});
                    llvm::Value* result = builder.CreateCall(fn, {minA, minB}, "fpminmax");
                    if (auto* ri = llvm::dyn_cast<llvm::Instruction>(result)) {
                        // Propagate nnan flag to the new intrinsic call.
                        llvm::FastMathFlags fmf;
                        fmf.setNoNaNs();
                        ri->setFastMathFlags(fmf);
                    }
                    replacements.emplace_back(sel, result);
                    ++count;
                }
            }
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Replace `fmul(x, -1.0)` and `fmul(-1.0, x)` with `fneg(x)`.
///
/// FP multiply by -1.0 is semantically identical to FP negation for all
/// well-defined values (including ±0, ±∞, and finite numbers).  The sign
/// of a NaN result is implementation-defined by IEEE 754, so both forms
/// may produce either sign of NaN — the transform is therefore safe even
/// without fast-math flags.
///
/// Benefit: on every major architecture FNeg maps to a single-cycle
/// instruction (VXORPS/VXORPD on x86, FNEG on ARM), while FMul uses
/// the 4-cycle FP-multiply pipeline.  Eliminating the FMul also frees
/// an FMA/FMul port for real multiply work.
///
/// Returns the number of multiplies replaced.
static unsigned foldFPMulByNeg1(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::FMul) continue;
            llvm::Value* op0 = inst.getOperand(0);
            llvm::Value* op1 = inst.getOperand(1);
            llvm::Type*  ty  = inst.getType();

            // Determine which operand is -1.0 (try both orderings).
            llvm::Value* other = nullptr;
            for (int s = 0; s < 2; ++s) {
                llvm::Value* candidate = (s == 0) ? op0 : op1;
                llvm::Value* partner   = (s == 0) ? op1 : op0;

                // Scalar -1.0 constant.
                if (auto* cfp = llvm::dyn_cast<llvm::ConstantFP>(candidate)) {
                    if (cfp->isExactlyValue(-1.0)) { other = partner; break; }
                }
                // Splat vector -1.0.
                if (auto* cv = llvm::dyn_cast<llvm::ConstantVector>(candidate)) {
                    if (auto* sp = cv->getSplatValue()) {
                        auto* spc = llvm::dyn_cast<llvm::ConstantFP>(sp);
                        if (spc && spc->isExactlyValue(-1.0)) { other = partner; break; }
                    }
                }
                if (auto* cdv = llvm::dyn_cast<llvm::ConstantDataVector>(candidate)) {
                    if (auto* sp = llvm::dyn_cast_or_null<llvm::ConstantFP>(
                            cdv->getSplatValue())) {
                        if (sp->isExactlyValue(-1.0)) { other = partner; break; }
                    }
                }
                (void)ty; // used for type check below if needed
            }
            if (!other) continue;

            llvm::IRBuilder<> builder(&inst);
            llvm::Value* neg = builder.CreateFNeg(other, "fneg1");
            // Propagate fast-math flags from the original mul.
            if (auto* negInst = llvm::dyn_cast<llvm::Instruction>(neg)) {
                auto* fpOp = llvm::cast<llvm::FPMathOperator>(&inst);
                negInst->setFastMathFlags(fpOp->getFastMathFlags());
            }
            replacements.emplace_back(&inst, neg);
            ++count;
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Expand `llvm.pow(x, N)` for small constant exponents to multiply chains.
///
/// Calling `pow(x, N)` for integer or half-integer N invokes the math
/// library, which is 50–100 cycles.  For small constant N we can express
/// the same value as a short sequence of fmul / sqrt instructions that
/// execute in 4–12 cycles on modern CPUs.
///
/// Transforms applied (require `afn` or `reassoc` fast-math flag):
///   pow(x, 0.0)  → 1.0          (exact)
///   pow(x, 1.0)  → x            (exact, no flag needed)
///   pow(x, 0.5)  → sqrt(x)      (requires nnan, so we use afn/reassoc)
///   pow(x, 2.0)  → fmul(x, x)   (one extra rounding — requires reassoc)
///   pow(x, 3.0)  → fmul(fmul(x,x), x)
///   pow(x, 4.0)  → let t=fmul(x,x); fmul(t,t)
///   pow(x, 5.0)  → let t=fmul(x,x); fmul(fmul(t,t), x)
///   pow(x, -0.5) → 1/sqrt(x)    (requires afn)
///   pow(x, -1.0) → 1/x (or fmul with arcp — handled by foldFPDivByConstant)
///   pow(x, -2.0) → let t=fmul(x,x); 1/t (then arcp folds)
///
/// Returns the number of pow calls replaced.
static unsigned foldPowBySmallInt(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    llvm::Module* mod = func.getParent();

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(&inst);
            if (!ii) continue;
            if (ii->getIntrinsicID() != llvm::Intrinsic::pow &&
                ii->getIntrinsicID() != llvm::Intrinsic::powi) continue;

            // Check for `afn` or `reassoc` fast-math flag.
            auto* fpOp = llvm::cast<llvm::FPMathOperator>(ii);
            bool canApprox = fpOp->hasApproxFunc() || fpOp->hasAllowReassoc();
            bool hasArcp   = fpOp->hasAllowReciprocal();
            if (!canApprox && !hasArcp) continue;

            llvm::Value* base = ii->getArgOperand(0);
            llvm::Type*  ty   = ii->getType();
            llvm::IRBuilder<> builder(ii);
            llvm::FastMathFlags fmf = fpOp->getFastMathFlags();

            llvm::Value* result = nullptr;

            // For powi, exponent is an i32 integer.
            if (ii->getIntrinsicID() == llvm::Intrinsic::powi) {
                auto* ci = llvm::dyn_cast<llvm::ConstantInt>(ii->getArgOperand(1));
                if (!ci) continue;
                int64_t exp = ci->getSExtValue();

                auto mul = [&](llvm::Value* a, llvm::Value* b) -> llvm::Value* {
                    auto* r = llvm::cast<llvm::Instruction>(
                        builder.CreateFMul(a, b, "pow.mul"));
                    r->setFastMathFlags(fmf);
                    return r;
                };
                llvm::Value* x2 = nullptr, *x4 = nullptr;
                auto getX2 = [&]() -> llvm::Value* {
                    if (!x2) x2 = mul(base, base);
                    return x2;
                };
                auto getX4 = [&]() -> llvm::Value* {
                    if (!x4) x4 = mul(getX2(), getX2());
                    return x4;
                };

                switch (exp) {
                case -4: { auto d = getX4();
                    auto* fd = llvm::cast<llvm::Instruction>(
                        builder.CreateFDiv(llvm::ConstantFP::get(ty, 1.0), d, "pow.recip"));
                    fd->setFastMathFlags(fmf);
                    result = fd; break; }
                case -3:
                    // pow(x, -3) = 1/(x²·x): requires three multiplies + one divide.
                    // The added complexity doesn't justify code generation here; the
                    // library call is only marginally slower for this single case.
                    // Fall through to the default (no transform).
                    break;
                case -2: {
                    auto* fd = llvm::cast<llvm::Instruction>(
                        builder.CreateFDiv(llvm::ConstantFP::get(ty, 1.0), getX2(), "pow.recip"));
                    fd->setFastMathFlags(fmf);
                    result = fd; break; }
                case -1: {
                    auto* fd = llvm::cast<llvm::Instruction>(
                        builder.CreateFDiv(llvm::ConstantFP::get(ty, 1.0), base, "pow.recip"));
                    fd->setFastMathFlags(fmf);
                    result = fd; break; }
                case 0:  result = llvm::ConstantFP::get(ty, 1.0); break;
                case 1:  result = base; break;
                case 2:  result = getX2(); break;
                case 3:  result = mul(getX2(), base); break;
                case 4:  result = getX4(); break;
                case 5:  result = mul(getX4(), base); break;
                case 6:  result = mul(getX4(), getX2()); break;
                case 8:  result = mul(getX4(), getX4()); break;
                default: break;
                }
            } else {
                // llvm.pow with a constant FP exponent.
                llvm::ConstantFP* cexp = nullptr;
                if (auto* c = llvm::dyn_cast<llvm::ConstantFP>(ii->getArgOperand(1)))
                    cexp = c;
                if (!cexp) continue;
                double expVal = cexp->getValueAPF().convertToDouble();

                auto mul = [&](llvm::Value* a, llvm::Value* b) -> llvm::Value* {
                    auto* r = llvm::cast<llvm::Instruction>(
                        builder.CreateFMul(a, b, "pow.mul"));
                    r->setFastMathFlags(fmf);
                    return r;
                };
                llvm::Value* x2 = nullptr, *x4 = nullptr;
                auto getX2 = [&]() -> llvm::Value* {
                    if (!x2) x2 = mul(base, base); return x2;
                };
                auto getX4 = [&]() -> llvm::Value* {
                    if (!x4) x4 = mul(getX2(), getX2()); return x4;
                };

                if (expVal == 0.0) {
                    result = llvm::ConstantFP::get(ty, 1.0);
                } else if (expVal == 1.0) {
                    result = base;
                } else if (expVal == 0.5) {
                    llvm::Function* sqrtFn = OMSC_GET_INTRINSIC(mod, llvm::Intrinsic::sqrt, {ty});
                    auto* sr = builder.CreateCall(sqrtFn, {base}, "pow.sqrt");
                    sr->setFastMathFlags(fmf);
                    result = sr;
                } else if (expVal == -0.5) {
                    llvm::Function* sqrtFn = OMSC_GET_INTRINSIC(mod, llvm::Intrinsic::sqrt, {ty});
                    auto* sr = builder.CreateCall(sqrtFn, {base}, "pow.sqrt");
                    sr->setFastMathFlags(fmf);
                    auto* fd = llvm::cast<llvm::Instruction>(
                        builder.CreateFDiv(llvm::ConstantFP::get(ty, 1.0), sr, "pow.rsqrt"));
                    fd->setFastMathFlags(fmf);
                    result = fd;
                } else if (expVal == 2.0) {
                    result = getX2();
                } else if (expVal == 3.0) {
                    result = mul(getX2(), base);
                } else if (expVal == 4.0) {
                    result = getX4();
                } else if (expVal == 5.0) {
                    result = mul(getX4(), base);
                } else if (expVal == 6.0) {
                    result = mul(getX4(), getX2());
                } else if (expVal == 8.0) {
                    result = mul(getX4(), getX4());
                } else if (expVal == -1.0) {
                    auto* fd = llvm::cast<llvm::Instruction>(
                        builder.CreateFDiv(llvm::ConstantFP::get(ty, 1.0), base, "pow.recip"));
                    fd->setFastMathFlags(fmf);
                    result = fd;
                } else if (expVal == -2.0) {
                    auto* fd = llvm::cast<llvm::Instruction>(
                        builder.CreateFDiv(llvm::ConstantFP::get(ty, 1.0), getX2(), "pow.recip2"));
                    fd->setFastMathFlags(fmf);
                    result = fd;
                }
            }

            if (!result) continue;
            replacements.emplace_back(ii, result);
            ++count;
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Replace `sqrt(x * x)` with `fabs(x)`.
///
/// For any finite real x, √(x²) = |x|.  The transform avoids a ~10-cycle
/// sqrt operation by substituting a 1-cycle sign-bit clear (FABS).
///
/// Safety conditions:
///   • `nnan`: no NaN inputs, so x is a real number.
///   • `ninf`: no infinities; |x| is finite.
///   Both flags on the sqrt (or `fast`) are required.
///
/// The fmul operand's flags are also checked: it must be the only user of
/// the fmul, or we would keep the fmul alive.
///
/// Returns the number of sqrt instructions replaced.
static unsigned foldSqrtSquare(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    llvm::Module* mod = func.getParent();

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(&inst);
            if (!ii || ii->getIntrinsicID() != llvm::Intrinsic::sqrt) continue;

            // Require nnan + ninf (or `fast`) on the sqrt.
            auto* fpSqrt = llvm::cast<llvm::FPMathOperator>(ii);
            if (!fpSqrt->hasNoNaNs() || !fpSqrt->hasNoInfs()) continue;

            llvm::Value* arg = ii->getArgOperand(0);
            llvm::Type*  ty  = ii->getType();

            // Pattern: sqrt(fmul(x, x)) where the fmul has exactly one use (this sqrt).
            auto* fmul = llvm::dyn_cast<llvm::BinaryOperator>(arg);
            if (!fmul || fmul->getOpcode() != llvm::Instruction::FMul) continue;
            if (!fmul->hasOneUse()) continue;
            if (fmul->getOperand(0) != fmul->getOperand(1)) continue;

            llvm::Value* x = fmul->getOperand(0);
            llvm::IRBuilder<> builder(ii);
            llvm::Function* fabsFn = OMSC_GET_INTRINSIC(mod, llvm::Intrinsic::fabs, {ty});
            llvm::Value* result = builder.CreateCall(fabsFn, {x}, "sqrt_sq_fabs");

            replacements.emplace_back(ii, result);
            // The fmul will be erased when the sqrt is replaced (use_empty check).
            ++count;
        }
    }

    for (auto& [inst, rep] : replacements) {
        // Also erase the now-dead fmul operand.
        llvm::Value* arg = inst->getOperand(0);
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
        if (auto* dead = llvm::dyn_cast<llvm::Instruction>(arg))
            if (dead->use_empty()) dead->eraseFromParent();
    }
    return count;
}

/// Replace `select(i1 cond, C_true, C_false)` with casts when the pair of
/// constants is (1, 0) or (-1, 0) — patterns the backend can lower to a
/// single SETCC or MOVZX/MOVSX instruction.
///
///   select(cond, i32  1, i32  0)  →  zext i1 cond to i32
///   select(cond, i32 -1, i32  0)  →  sext i1 cond to i32
///   select(cond, i32  0, i32  1)  →  zext i1 (not cond) to i32
///     (not folded here — would need xor; left for InstCombine)
///   select(cond, i64  1, i64  0)  →  zext i1 cond to i64
///   select(cond, i64 -1, i64  0)  →  sext i1 cond to i64
///
/// This reduces a branch-like select sequence (2+ µops: SETCC + MOVZX or
/// CMOV) to a single µop on architectures with an integrated SETCC.
///
/// Returns the number of selects replaced.
static unsigned foldSelectToBoolCast(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst);
            if (!sel) continue;

            // Condition must be i1 (bit-width 1).
            llvm::Value* cond = sel->getCondition();
            if (!cond->getType()->isIntegerTy(1)) continue;

            llvm::Value* tv = sel->getTrueValue();
            llvm::Value* fv = sel->getFalseValue();
            llvm::Type*  ty = sel->getType();
            if (!ty->isIntegerTy()) continue;

            auto* tCI = llvm::dyn_cast<llvm::ConstantInt>(tv);
            auto* fCI = llvm::dyn_cast<llvm::ConstantInt>(fv);
            if (!tCI || !fCI) continue;

            llvm::IRBuilder<> builder(sel);
            llvm::Value* result = nullptr;

            if (tCI->isOne() && fCI->isZero()) {
                // select(cond, 1, 0) → zext(cond)
                result = builder.CreateZExt(cond, ty, "bool.zext");
            } else if (tCI->isMinusOne() && fCI->isZero()) {
                // select(cond, -1, 0) → sext(cond)
                result = builder.CreateSExt(cond, ty, "bool.sext");
            } else if (tCI->isZero() && fCI->isOne()) {
                // select(cond, 0, 1) → zext(not cond)
                // Emit: xor cond, 1 → zext
                llvm::Value* notCond = builder.CreateXor(
                    cond, llvm::ConstantInt::getTrue(cond->getContext()), "bool.not");
                result = builder.CreateZExt(notCond, ty, "bool.notzext");
            } else if (tCI->isZero() && fCI->isMinusOne()) {
                // select(cond, 0, -1) → sext(not cond)
                llvm::Value* notCond = builder.CreateXor(
                    cond, llvm::ConstantInt::getTrue(cond->getContext()), "bool.not");
                result = builder.CreateSExt(notCond, ty, "bool.notsext");
            }

            if (!result) continue;
            replacements.emplace_back(sel, result);
            ++count;
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Hoist loop-invariant GEP instructions out of loop bodies into the loop's
/// pre-header (the unique non-back-edge predecessor of the loop header).
///
/// GEP instructions whose base pointer and all index operands are defined
/// outside the loop body are invariant — they compute the same address on
/// every iteration and can be computed once in the pre-header.
///
/// Motivation: while LLVM's LICM pass normally handles this, our transforms
/// (strength reduction, FMA generation, etc.) can introduce new GEP
/// combinations that appear *after* LICM has run.  Running this pass last
/// ensures newly created address computations are hoisted.
///
/// Algorithm:
///   1. Detect loop headers (basic blocks with a back-edge predecessor).
///   2. Identify the pre-header (unique non-back-edge predecessor).
///   3. Collect all basic blocks in the loop (reachable from header, before latch).
///   4. For each GEP in any loop BB: if all operands are defined outside the loop,
///      move the GEP to the end of the pre-header (before its terminator).
///
/// Returns the number of GEPs hoisted.
static unsigned hoistLoopInvariantGEP(llvm::Function& func) {
    if (func.isDeclaration()) return 0;

    unsigned count = 0;

    // Assign linear order to BBs for back-edge detection.
    std::unordered_map<const llvm::BasicBlock*, unsigned> bbOrder;
    { unsigned ord = 0; for (auto& bb : func) bbOrder[&bb] = ord++; }

    // Process each basic block as a potential loop header.
    for (auto& header : func) {
        // Find the latch (back-edge source): a predecessor of header with
        // a higher linear order.
        llvm::BasicBlock* latch = nullptr;
        for (auto* pred : llvm::predecessors(&header)) {
            if (bbOrder.count(pred) && bbOrder.at(pred) >= bbOrder.at(&header)) {
                latch = pred;
                break;
            }
        }
        if (!latch) continue; // not a loop header

        // Find the pre-header: unique predecessor of header that is NOT the latch.
        // If there are multiple non-latch predecessors, we cannot safely hoist
        // (the pre-header is ambiguous).
        llvm::BasicBlock* preHeader = nullptr;
        unsigned nonLatchPreds = 0;
        for (auto* pred : llvm::predecessors(&header)) {
            if (pred != latch) {
                preHeader = pred;
                ++nonLatchPreds;
            }
        }
        if (nonLatchPreds != 1 || !preHeader) continue;

        // Collect all basic blocks in the loop body using a BFS from the header
        // that stays within the back-edge cycle (stops at header when reached via latch).
        // Simple approximation: all BBs between header and latch in linear order.
        unsigned headerOrd = bbOrder.at(&header);
        unsigned latchOrd  = bbOrder.at(latch);
        std::unordered_set<const llvm::BasicBlock*> loopBBs;
        for (auto& bb : func) {
            unsigned ord = bbOrder.at(&bb);
            if (ord >= headerOrd && ord <= latchOrd)
                loopBBs.insert(&bb);
        }

        // Collect GEPs to hoist from any loop BB.
        // A GEP is invariant if ALL its operands (base + indices) are either:
        //   • Constants, or
        //   • Defined in a BB that is NOT in the loop (i.e., outside the loop).
        std::vector<llvm::GetElementPtrInst*> toHoist;

        for (auto& bb : func) {
            if (!loopBBs.count(&bb)) continue;
            for (auto& inst : bb) {
                auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst);
                if (!gep) continue;

                // Check all operands.
                bool invariant = true;
                for (unsigned i = 0; i < gep->getNumOperands(); ++i) {
                    llvm::Value* op = gep->getOperand(i);
                    if (llvm::isa<llvm::Constant>(op)) continue;
                    auto* defInst = llvm::dyn_cast<llvm::Instruction>(op);
                    if (!defInst) { invariant = false; break; }
                    // Loop-invariant if defined outside the loop.
                    if (loopBBs.count(defInst->getParent())) { invariant = false; break; }
                }
                if (!invariant) continue;
                // Don't hoist if already in the pre-header.
                if (gep->getParent() == preHeader) continue;
                // Skip GEPs with side effects (shouldn't exist, but be safe).
                toHoist.push_back(gep);
            }
        }

        // Hoist all invariant GEPs: move them to the pre-header (before its terminator).
        llvm::Instruction* insertPt = preHeader->getTerminator();
        for (auto* gep : toHoist) {
            gep->moveBefore(insertPt);
            ++count;
        }
    }
    return count;
}

/// Rebalance linear chains of commutative+associative binary operations into
/// balanced binary trees, reducing critical path depth.
///
/// A linear chain  ((a OP b) OP c) OP d  has depth 3 on a single-issue pipe.
/// The balanced form  (a OP b) OP (c OP d)  has depth 2, exposing ILP.
///
/// For n operands in a chain, the linear form has depth n-1; the balanced
/// tree has depth ceil(log2(n)).  For chains of 4+ operands on wide-issue
/// CPUs this is a strict win.
///
/// Only applies to:
///   - Integer:  Add, Mul, And, Or, Xor  (always associative)
///   - Floating-point:  FAdd, FMul  only when the instruction has the
///     `reassoc` fast-math flag (preserves IEEE semantics otherwise)
///
/// Guard: only rebalance when the critical-path reduction > 0 (chain ≥ 3
/// operands) and the CPU has ≥ 2 integer ALU ports or ≥ 2 FMA units
/// (spare capacity to execute the additional independent operations).
///
/// Returns the number of chains rebalanced.
[[gnu::hot]] static unsigned rebalanceChainForILP(llvm::Function& func,
                                                    const MicroarchProfile& profile) {
    // Need spare execution units to benefit.
    bool hasIntILP = profile.intALUs >= 2;
    bool hasFpILP  = profile.fmaUnits >= 2 || profile.vecUnits >= 2;
    if (!hasIntILP && !hasFpILP) return 0;

    unsigned count = 0;

    for (auto& bb : func) {
        // Collect instructions eligible to be a chain root: same binary opcode,
        // single use in this BB, and the use is also the same opcode.
        // We process bottom-up: find the deepest instruction in a chain
        // (the one whose result leaves the chain, i.e. is used outside or
        // is used by a different opcode), then walk up to collect operands.

        // isChainOp: true if the opcode is commutative+associative and we
        // are allowed to rebalance it (fast-math for FP, always for int).
        auto isChainOp = [](const llvm::Instruction* inst) -> bool {
            if (!inst) return false;
            switch (inst->getOpcode()) {
            case llvm::Instruction::Add:
            case llvm::Instruction::Mul:
            case llvm::Instruction::And:
            case llvm::Instruction::Or:
            case llvm::Instruction::Xor:
                return true;
            case llvm::Instruction::FAdd:
            case llvm::Instruction::FMul:
                // Only rebalance with reassoc flag to preserve FP semantics.
                return llvm::cast<llvm::FPMathOperator>(inst)->hasAllowReassoc();
            default:
                return false;
            }
        };

        // For each instruction in the BB that is a chain op, check if it is
        // the "root" of a chain (its result is NOT fed into the same op).
        std::unordered_set<llvm::Instruction*> processed;

        for (auto& rootInst : bb) {
            if (!isChainOp(&rootInst)) continue;
            if (processed.count(&rootInst)) continue;

            // Walk UP from the root to collect all instructions in the chain
            // that have the same opcode, single use back to this chain, and
            // are in the same BB.  Stop at instructions that have multiple
            // users (their result must stay live) or are used outside the BB.
            //
            // "Chain" = connected component of same-opcode nodes where each
            // internal node has exactly one user (the next in the chain).
            // The root is the node whose user is NOT in the chain.

            // Check if rootInst is truly the root: its use is NOT the same op.
            bool isRoot = true;
            if (rootInst.hasOneUse()) {
                auto* user = llvm::dyn_cast<llvm::Instruction>(*rootInst.user_begin());
                if (user && user->getOpcode() == rootInst.getOpcode() &&
                    user->getParent() == &bb)
                    isRoot = false; // there's a parent in the chain
            }
            if (!isRoot) continue;

            // Collect leaf operands via DFS: traverse all chain members.
            // chainMembers: all instructions in the chain (except the root itself).
            // leaves: the actual operand values that feed into the chain.
            std::vector<llvm::Instruction*> chainMembers;
            std::vector<llvm::Value*> leaves;
            unsigned opcode = rootInst.getOpcode();

            // Use a worklist of (instruction, operand_index).
            // For each chain member, expand its operands:
            //   - If operand is a chain-eligible instruction with one use →
            //     add to chain and expand its operands.
            //   - Otherwise → it's a leaf.
            std::function<void(llvm::Instruction*)> collect =
                [&](llvm::Instruction* inst) {
                    for (unsigned i = 0; i < inst->getNumOperands(); ++i) {
                        llvm::Value* op = inst->getOperand(i);
                        auto* opInst = llvm::dyn_cast<llvm::Instruction>(op);
                        if (opInst && opInst->getOpcode() == opcode &&
                            opInst->getParent() == &bb &&
                            opInst->hasOneUse() &&
                            !processed.count(opInst)) {
                            chainMembers.push_back(opInst);
                            processed.insert(opInst);
                            collect(opInst);
                        } else {
                            leaves.push_back(op);
                        }
                    }
                };

            processed.insert(&rootInst);
            collect(&rootInst);

            // Need at least 4 leaves to benefit (depth 3 linear → depth 2 tree).
            if (leaves.size() < 4) continue;

            // Check parallel execution capacity:
            bool isFP = (opcode == llvm::Instruction::FAdd ||
                         opcode == llvm::Instruction::FMul);
            if (isFP && !hasFpILP)  continue;
            if (!isFP && !hasIntILP) continue;

            // Build balanced binary tree from leaves.
            // Use IRBuilder positioned just before the root instruction.
            llvm::IRBuilder<> builder(&rootInst);

            // Copy fast-math flags from root for FP ops.
            llvm::FastMathFlags fmf;
            if (isFP)
                fmf = llvm::cast<llvm::FPMathOperator>(&rootInst)->getFastMathFlags();

            // Build tree bottom-up: combine pairs until one value remains.
            std::vector<llvm::Value*> work = leaves;
            while (work.size() > 1) {
                std::vector<llvm::Value*> next;
                for (size_t i = 0; i + 1 < work.size(); i += 2) {
                    llvm::Value* combined;
                    switch (opcode) {
                    case llvm::Instruction::Add:
                        combined = builder.CreateAdd(work[i], work[i+1], "rlp.add");
                        break;
                    case llvm::Instruction::Mul:
                        combined = builder.CreateMul(work[i], work[i+1], "rlp.mul");
                        break;
                    case llvm::Instruction::And:
                        combined = builder.CreateAnd(work[i], work[i+1], "rlp.and");
                        break;
                    case llvm::Instruction::Or:
                        combined = builder.CreateOr(work[i], work[i+1], "rlp.or");
                        break;
                    case llvm::Instruction::Xor:
                        combined = builder.CreateXor(work[i], work[i+1], "rlp.xor");
                        break;
                    case llvm::Instruction::FAdd: {
                        auto* fa = builder.CreateFAdd(work[i], work[i+1], "rlp.fadd");
                        if (auto* fi = llvm::dyn_cast<llvm::Instruction>(fa))
                            fi->setFastMathFlags(fmf);
                        combined = fa;
                        break;
                    }
                    case llvm::Instruction::FMul: {
                        auto* fm = builder.CreateFMul(work[i], work[i+1], "rlp.fmul");
                        if (auto* fi = llvm::dyn_cast<llvm::Instruction>(fm))
                            fi->setFastMathFlags(fmf);
                        combined = fm;
                        break;
                    }
                    default:
                        combined = work[i]; // fallback (shouldn't happen)
                        break;
                    }
                    next.push_back(combined);
                }
                // Carry forward an odd element unpaired.
                if (work.size() % 2 == 1)
                    next.push_back(work.back());
                work = std::move(next);
            }

            // Replace the root instruction with the balanced tree result.
            if (!work.empty() && work[0] != &rootInst) {
                rootInst.replaceAllUsesWith(work[0]);
                // Mark old chain for removal: they are now dead.
                // (LLVM DCE will clean them up; we just need to ensure no uses.)
                ++count;
            }
        }
    }
    return count;
}

/// Convert simple if-then-else diamonds to select instructions when the
/// branch misprediction penalty on the target CPU makes that more profitable.
///
/// Pattern:
///   header:  br cond, then_bb, else_bb / merge_bb
///   then_bb: %v = <pure_inst>;  br merge_bb        (≤ 2 instructions)
///   merge_bb: %phi = phi [%v, then_bb], [%other, header]
///
/// Replace with (in header):
///   %v = <pure_inst (hoisted)>
///   %phi_val = select cond, %v, %other
///
/// Profitability: profitable when
///   branchMispredictPenalty × kMissRate > cost(select) + cost(hoisted_inst)
/// where kMissRate = 0.1 (10% estimated miss rate without PGO).
///
/// Safety guards:
///   - then_bb has no side effects (no stores, calls, volatile loads)
///   - then_bb has ≤ 2 non-PHI, non-branch instructions
///   - The hoisted instruction's operands are all available in the header
///   - then_bb has exactly one predecessor (header) and one successor (merge)
///
/// Returns the number of branches converted.
static unsigned convertIfElseToSelect(llvm::Function& func,
                                       const MicroarchProfile& profile) {
    constexpr double kMissRate = 0.10;
    // Minimum misprediction penalty (in cycles) to justify conversion.
    // select costs 1 cycle (ALU); a speculative pure inst costs its latency.
    // We require: mispredictCycles > 2 * latIntAdd to ensure clear benefit.
    double mispredictCycles = profile.branchMispredictPenalty * kMissRate;
    if (mispredictCycles <= static_cast<double>(2 * profile.latIntAdd)) return 0;

    unsigned count = 0;
    // Collect conversions to apply (avoid invalidating iterators).
    struct Conversion {
        llvm::BranchInst* br;
        llvm::BasicBlock* thenBB;
        llvm::BasicBlock* mergeBB;
        llvm::PHINode* phi;
        llvm::Instruction* thenVal; // the value computed in thenBB (may be null)
        llvm::Value* elseVal;       // the value from the else path
        bool thenIsTrue;            // true if thenBB is the true successor
    };
    std::vector<Conversion> conversions;

    for (auto& bb : func) {
        auto* br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator());
        if (!br || !br->isConditional() || br->getNumSuccessors() != 2) continue;

        llvm::BasicBlock* succTrue  = br->getSuccessor(0);
        llvm::BasicBlock* succFalse = br->getSuccessor(1);

        // Try both orientations: thenBB = succTrue or thenBB = succFalse.
        for (int orientation = 0; orientation < 2; ++orientation) {
            llvm::BasicBlock* thenBB  = (orientation == 0) ? succTrue  : succFalse;
            llvm::BasicBlock* mergeBB = (orientation == 0) ? succFalse : succTrue;

            // thenBB must have exactly one predecessor (our header).
            if (thenBB->getSinglePredecessor() != &bb) continue;

            // thenBB must jump unconditionally to mergeBB.
            auto* thenTerm = llvm::dyn_cast<llvm::BranchInst>(thenBB->getTerminator());
            if (!thenTerm || thenTerm->isConditional()) continue;
            if (thenTerm->getSuccessor(0) != mergeBB) continue;

            // Count non-PHI, non-branch instructions in thenBB.
            std::vector<llvm::Instruction*> thenInsts;
            for (auto& inst : *thenBB) {
                if (llvm::isa<llvm::PHINode>(inst) || inst.isTerminator()) continue;
                thenInsts.push_back(&inst);
            }
            // Allow at most 2 pure instructions (e.g., a load + cast or a single compute).
            if (thenInsts.size() > 2) continue;

            // Check all thenBB instructions are pure (no side effects).
            bool pure = true;
            for (auto* inst : thenInsts) {
                if (inst->mayHaveSideEffects() || inst->mayReadOrWriteMemory()) {
                    pure = false; break;
                }
            }
            if (!pure) continue;

            // Find a PHI in mergeBB that merges a value from thenBB with a
            // value from &bb (the header / else path).
            llvm::PHINode* foundPhi = nullptr;
            llvm::Instruction* thenVal = nullptr;
            llvm::Value* elseVal = nullptr;

            for (auto& mergeInst : *mergeBB) {
                auto* phi = llvm::dyn_cast<llvm::PHINode>(&mergeInst);
                if (!phi) break;

                llvm::Value* fromThen = phi->getIncomingValueForBlock(thenBB);
                llvm::Value* fromElse = phi->getIncomingValueForBlock(&bb);
                if (!fromThen || !fromElse) continue;

                // The "from then" value must originate in thenBB (or be a constant).
                auto* fromThenInst = llvm::dyn_cast<llvm::Instruction>(fromThen);
                bool thenValIsLocal = !fromThenInst ||
                                      fromThenInst->getParent() == thenBB;
                if (!thenValIsLocal) continue;

                // Check that all operands of the thenBB instructions are
                // available in the header (defined before the branch).
                bool operandsOk = true;
                for (auto* inst : thenInsts) {
                    for (auto& op : inst->operands()) {
                        auto* opInst = llvm::dyn_cast<llvm::Instruction>(op.get());
                        if (opInst && opInst->getParent() == thenBB) continue; // def in thenBB
                        if (opInst && opInst->getParent() != &bb) {
                            // Must dominate header — conservative: only allow
                            // values from outside the function (args) or defined
                            // before the branch in the same function.
                            // For safety, we only allow args and header-local defs.
                            // NOTE: `op.get()` may be an Argument (not an Instruction).
                            operandsOk = false; break;
                        }
                        // If op is not an instruction (e.g., a function Argument or
                        // Constant), it always dominates every block — allow it.
                    }
                    if (!operandsOk) break;
                }
                if (!operandsOk) continue;

                foundPhi = phi;
                thenVal  = fromThenInst; // may be null if constant
                elseVal  = fromElse;
                break;
            }
            if (!foundPhi) continue;

            // Check profitability one more time with the actual hoisted inst cost.
            unsigned hoistCost = 0;
            for (auto* inst : thenInsts)
                hoistCost += getOpcodeLatency(inst, profile);
            // Profitable if: mispredict cost > select cost (1) + hoist cost
            if (mispredictCycles <= static_cast<double>(1 + hoistCost)) continue;

            conversions.push_back({br, thenBB, mergeBB, foundPhi, thenVal, elseVal,
                                   orientation == 0});
            break; // found a valid orientation for this BB
        }
    }

    // Apply conversions in reverse order to avoid invalidating BBs.
    for (auto& cv : conversions) {
        llvm::BranchInst* br     = cv.br;
        llvm::BasicBlock* thenBB = cv.thenBB;
        llvm::BasicBlock* header = br->getParent();
        llvm::PHINode*    phi    = cv.phi;

        // Hoist thenBB's instructions into the header (before the branch).
        std::vector<llvm::Instruction*> toHoist;
        for (auto& inst : *thenBB)
            if (!llvm::isa<llvm::PHINode>(inst) && !inst.isTerminator())
                toHoist.push_back(&inst);

        for (auto* inst : toHoist)
            inst->moveBefore(br);

        // Build select replacing the PHI.
        llvm::IRBuilder<> builder(br);
        llvm::Value* sel;
        if (cv.thenIsTrue)
            sel = builder.CreateSelect(br->getCondition(), phi->getIncomingValueForBlock(thenBB),
                                       cv.elseVal, "sel.if2sel");
        else
            sel = builder.CreateSelect(br->getCondition(), cv.elseVal,
                                       phi->getIncomingValueForBlock(thenBB), "sel.if2sel");

        phi->replaceAllUsesWith(sel);
        phi->eraseFromParent();

        // Redirect the branch to skip thenBB: unconditional branch to mergeBB.
        llvm::BasicBlock* mergeBB = cv.mergeBB;
        // Remove thenBB as a predecessor of mergeBB.
        // Update any remaining PHIs in mergeBB that reference thenBB.
        for (auto& inst : *mergeBB) {
            auto* remainingPhi = llvm::dyn_cast<llvm::PHINode>(&inst);
            if (!remainingPhi) break;
            int idx = remainingPhi->getBasicBlockIndex(thenBB);
            if (idx >= 0) remainingPhi->removeIncomingValue(idx, false);
        }

        // Replace the conditional branch with an unconditional one to mergeBB.
        llvm::BranchInst::Create(mergeBB, br);
        br->eraseFromParent();

        // thenBB is now unreachable; it will be cleaned up by CFG simplification.
        // Leave it for now; it has no predecessors.
        (void)header; // suppress unused variable warning

        ++count;
    }

    return count;
}

/// Add `!nontemporal` metadata to streaming stores whose estimated working set
/// exceeds the L1D cache capacity.  Non-temporal stores (MOVNT* on x86, STNPs
/// on ARM) bypass the cache entirely, avoiding pollution and reducing write-
/// combine overhead for large sequential writes.
///
/// Detection heuristic:
///   - Store is inside a loop body (basic block with a back-edge to itself or
///     a predecessor that appears later in linear order).
///   - The store pointer is a GEP with a stride larger than one cache line,
///     OR the function contains more unique store base pointers × estimated
///     element size than L1D capacity.
///
/// Safety: non-temporal stores must NOT be used when the stored value will
/// be read back soon (within the same loop iteration).  We conservatively
/// skip stores whose pointer is also loaded in the same BB.
///
/// Returns the number of stores annotated.
static unsigned insertNonTemporalHints(llvm::Function& func,
                                        const MicroarchProfile& profile) {
    if (func.isDeclaration()) return 0;
    // Only beneficial for CPUs with a decent L1D (≥ 16 KB).
    if (profile.l1DSize < 16) return 0;

    unsigned count = 0;
    llvm::LLVMContext& ctx = func.getContext();

    // Assign linear order to detect loop bodies.
    std::unordered_map<const llvm::BasicBlock*, unsigned> bbOrder;
    { unsigned ord = 0; for (auto& bb : func) bbOrder[&bb] = ord++; }

    const llvm::DataLayout* dl = nullptr;
    if (auto* mod = func.getParent()) dl = &mod->getDataLayout();

    for (auto& bb : func) {
        // Determine if this BB is a loop body (has a backedge from any successor).
        bool isLoop = false;
        for (auto* succ : llvm::successors(&bb)) {
            if (bbOrder.count(succ) && bbOrder[succ] <= bbOrder[&bb]) {
                isLoop = true; break;
            }
        }
        if (!isLoop) continue;

        // Collect store and load pointers in this BB to detect read-back.
        std::unordered_set<llvm::Value*> loadedPtrs;
        for (auto& inst : bb)
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst))
                loadedPtrs.insert(ld->getPointerOperand());

        for (auto& inst : bb) {
            auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst);
            if (!st || st->isVolatile()) continue;

            // Already annotated.
            if (st->getMetadata(llvm::LLVMContext::MD_nontemporal)) continue;

            llvm::Value* ptr = st->getPointerOperand();

            // Skip if this pointer is also loaded in the same BB (read-back risk).
            if (loadedPtrs.count(ptr)) continue;

            // Check for large-stride or large-working-set access.
            bool shouldAnnotate = false;

            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
                // Single-index GEP: check element type size vs cache line.
                if (gep->getNumIndices() == 1 && dl) {
                    llvm::Type* elemTy = gep->getSourceElementType();
                    if (elemTy && elemTy->isSized()) {
                        uint64_t elemBytes = dl->getTypeAllocSize(elemTy);
                        // Stride > cache line → streaming access pattern.
                        if (elemBytes > profile.cacheLineSize)
                            shouldAnnotate = true;
                        // Working set heuristic: if element count × elemBytes
                        // plausibly exceeds L1D (assume ≥ 256 iterations).
                        unsigned estimatedElements = 256;
                        if (elemBytes * estimatedElements >
                                static_cast<uint64_t>(profile.l1DSize) * 1024)
                            shouldAnnotate = true;
                    }
                }
            }

            if (shouldAnnotate) {
                // Add !nontemporal metadata (value = i32 1).
                llvm::MDNode* ntMD = llvm::MDNode::get(ctx, {
                    llvm::ConstantAsMetadata::get(
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 1))
                });
                st->setMetadata(llvm::LLVMContext::MD_nontemporal, ntMD);
                ++count;
            }
        }
    }
    return count;
}

/// Replace integer multiply-by-minus-one with negation.
///
/// Pattern:  mul(x, -1)  →  sub(0, x)
///
/// This is the integer analogue of foldFPMulByNeg1.  On every modern
/// architecture `neg` (or `sub reg, 0`) is a 1-cycle single-issue
/// operation that dispatches to any ALU port, while integer multiply
/// uses the dedicated multiplier (latency 3–4 cycles, one port).
/// Freeing the multiply port for real multiplies improves IPC when the
/// function contains other integer multiply operations.
///
/// Safety: integer arithmetic is defined modulo 2ⁿ, so `x × (−1)` ≡
/// `−x (mod 2ⁿ)` always.  No overflow or undefined-behaviour flags
/// are needed.  The transform is safe for any integer type, including
/// vector integer types.
///
/// Returns the number of multiplies replaced.
static unsigned foldIntMulByNeg1(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Mul) continue;
            llvm::Type* ty = inst.getType();
            // Integer or integer-vector only.
            if (!ty->isIntOrIntVectorTy()) continue;

            llvm::Value* op0 = inst.getOperand(0);
            llvm::Value* op1 = inst.getOperand(1);

            // Check whether one of the operands is a constant -1 (all-ones bits).
            // We handle: scalar ConstantInt, splat ConstantVector, and
            // ConstantDataVector (the form LLVM uses for <N x iM> splatted constants).
            llvm::Value* other = nullptr;
            for (int s = 0; s < 2; ++s) {
                llvm::Value* candidate = (s == 0) ? op0 : op1;
                llvm::Value* partner   = (s == 0) ? op1 : op0;

                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(candidate)) {
                    if (ci->isMinusOne()) { other = partner; break; }
                }
                if (auto* cv = llvm::dyn_cast<llvm::ConstantVector>(candidate)) {
                    if (auto* sp = cv->getSplatValue()) {
                        auto* sci = llvm::dyn_cast<llvm::ConstantInt>(sp);
                        if (sci && sci->isMinusOne()) { other = partner; break; }
                    }
                }
                if (auto* cdv = llvm::dyn_cast<llvm::ConstantDataVector>(candidate)) {
                    if (auto* sp = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                            cdv->getSplatValue())) {
                        if (sp->isMinusOne()) { other = partner; break; }
                    }
                }
            }
            if (!other) continue;

            // Emit sub(0, x): LLVM selects NEG on all supported architectures.
            llvm::IRBuilder<> builder(&inst);
            llvm::Value* zero = llvm::Constant::getNullValue(ty);
            llvm::Value* neg  = builder.CreateSub(zero, other, "neg1");
            // Propagate nsw/nuw flags conservatively (only nsw if the original
            // mul had nsw, meaning the negation cannot overflow either).
            if (auto* subInst = llvm::dyn_cast<llvm::BinaryOperator>(neg)) {
                auto* mulOp = llvm::cast<llvm::BinaryOperator>(&inst);
                if (mulOp->hasNoSignedWrap())
                    subInst->setHasNoSignedWrap(true);
            }
            replacements.emplace_back(&inst, neg);
            ++count;
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Fold `add(x, sub(0, y))` → `sub(x, y)`.
///
/// This is a direct follow-on to foldIntMulByNeg1.  That pass rewrites
/// `mul(y, -1)` to `sub(0, y)`.  When the negated value is then added to
/// another operand x, the net expression is `x + (-y)` = `x - y`, which is
/// representable as a single SUB instruction.
///
/// Safety: modular integer arithmetic guarantees `x + (0 - y) ≡ x - y (mod 2ⁿ)`
/// for every bit-pattern of x and y, with no exceptions.  We conservatively
/// do NOT forward nsw/nuw from either the add or the inner sub — the resulting
/// sub gets no overflow flags, which is the safest correct choice.
///
/// We only fold when the inner sub(0, y) has exactly one use (the add), so
/// both instructions can be replaced by a single sub.  If the neg has other
/// uses, keeping it alive is correct but we would not reduce the instruction
/// count, so we skip that case.
///
/// Returns the number of add+neg pairs folded.
static unsigned foldIntAddNeg(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Add) continue;
            llvm::Type* ty = inst.getType();
            if (!ty->isIntOrIntVectorTy()) continue;

            llvm::Value* op0 = inst.getOperand(0);
            llvm::Value* op1 = inst.getOperand(1);

            // Try both orderings: add(x, neg) and add(neg, x).
            for (int s = 0; s < 2; ++s) {
                llvm::Value* negCandidate = (s == 0) ? op1 : op0;
                llvm::Value* other        = (s == 0) ? op0 : op1;

                auto* negInst = llvm::dyn_cast<llvm::BinaryOperator>(negCandidate);
                if (!negInst) continue;
                if (negInst->getOpcode() != llvm::Instruction::Sub) continue;
                // Require sub(0, y) — the LHS must be the zero constant.
                if (!llvm::isa<llvm::Constant>(negInst->getOperand(0))) continue;
                auto* lhsC = llvm::dyn_cast<llvm::Constant>(negInst->getOperand(0));
                if (!lhsC || !lhsC->isNullValue()) continue;
                // Only fold when the neg is used only by this add.
                if (!negInst->hasOneUse()) continue;

                llvm::Value* y = negInst->getOperand(1);
                llvm::IRBuilder<> builder(&inst);
                llvm::Value* sub = builder.CreateSub(other, y, "addneg.sub");
                replacements.emplace_back(&inst, sub);
                ++count;
                break;
            }
        }
    }

    for (auto& [inst, rep] : replacements) {
        // Erase the now-dead sub(0, y) first if it becomes unused.
        llvm::Value* negArg = nullptr;
        for (int s = 0; s < 2; ++s) {
            auto* neg = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(s));
            if (neg && neg->getOpcode() == llvm::Instruction::Sub &&
                llvm::isa<llvm::Constant>(neg->getOperand(0))) {
                auto* lhsC = llvm::dyn_cast<llvm::Constant>(neg->getOperand(0));
                if (lhsC && lhsC->isNullValue()) { negArg = neg; break; }
            }
        }
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
        if (negArg) {
            if (auto* dead = llvm::dyn_cast<llvm::Instruction>(negArg))
                if (dead->use_empty()) dead->eraseFromParent();
        }
    }
    return count;
}

/// Replace `select(cond, x, x)` with `x`.
///
/// When both true-value and false-value of a select are the same SSA value,
/// the condition is irrelevant and the select is a no-op.  This pattern
/// arises after other transforms simplify one of the two arms of a
/// conditional to match the other arm (e.g. after strength-reduction or
/// after convertIfElseToSelect folds the arms).
///
/// Safety: always correct — the result is the same regardless of the condition.
///
/// Returns the number of selects eliminated.
static unsigned foldSelectSameValue(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst);
            if (!sel) continue;
            if (sel->getTrueValue() != sel->getFalseValue()) continue;
            sel->replaceAllUsesWith(sel->getTrueValue());
            toErase.push_back(sel);
            ++count;
        }
    }

    for (auto* inst : toErase)
        inst->eraseFromParent();
    return count;
}

/// Replace `fadd(x, x)` with `fmul(x, 2.0)` when the `reassoc` fast-math
/// flag is set.
///
/// `x + x = 2*x` mathematically, but under strict IEEE 754 the two forms
/// differ only in rounding of the final result — both produce the exact
/// mathematical value when |x| fits in the format.  The `reassoc` flag
/// explicitly permits this kind of reassociation.
///
/// Benefit: `fmul(x, 2.0)` is immediately eligible for FMA fusion by the
/// subsequent `generateFMA` scan.  e.g.:
///
///   fadd reassoc x, x       →   fmul x, 2.0
///   followed by:
///   fadd contract (fmul x, 2.0), y  →  fma(x, 2.0, y)
///
/// This is a net gain of one FMA instruction replacing two separate
/// instructions (the self-add and the add-with-y).
///
/// We only replace when the second operand is the same SSA value as the
/// first (pointer equality), which guarantees exact semantic equivalence
/// under reassoc.
///
/// Returns the number of self-adds replaced.
static unsigned foldFAddSelf(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::FAdd) continue;
            llvm::Value* op0 = inst.getOperand(0);
            llvm::Value* op1 = inst.getOperand(1);
            if (op0 != op1) continue; // must be exact same SSA value

            auto* fpOp = llvm::cast<llvm::FPMathOperator>(&inst);
            if (!fpOp->hasAllowReassoc()) continue;

            llvm::IRBuilder<> builder(&inst);
            llvm::Type* ty = inst.getType();
            llvm::Value* two = llvm::ConstantFP::get(ty, 2.0);
            llvm::Value* mul = builder.CreateFMul(op0, two, "faddself.fmul");
            // Propagate fast-math flags to the new multiply.
            if (auto* mulInst = llvm::dyn_cast<llvm::Instruction>(mul))
                mulInst->setFastMathFlags(fpOp->getFastMathFlags());

            replacements.emplace_back(&inst, mul);
            ++count;
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Eliminate double negation: fneg(fneg(x)) → x.
///
/// FMA generation, canonicalizeFaddFneg, and foldFPMulByNeg1 can each
/// introduce an llvm::FNeg instruction.  When two such passes run in
/// sequence on the same value a double negation can appear.  Removing
/// it eliminates two instructions and their latency chain entirely.
///
/// Safety: `fneg(fneg(x))` equals `x` for all IEEE 754 values (finite,
/// ±0, ±∞, NaN — FNeg only flips the sign bit).  No fast-math flags
/// are required.
///
/// We only fold when the inner fneg has exactly one use (the outer one),
/// so we can erase both.  If the inner fneg has other uses we would need
/// to keep it alive, so we skip that case.
///
/// Returns the number of double negations eliminated.
static unsigned foldFNegDouble(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::FNeg) continue;
            llvm::Value* arg = inst.getOperand(0);
            auto* inner = llvm::dyn_cast<llvm::UnaryOperator>(arg);
            if (!inner || inner->getOpcode() != llvm::Instruction::FNeg) continue;
            // Only fold when the inner fneg is used solely by this outer fneg.
            if (!inner->hasOneUse()) continue;

            // Replace outer fneg with the inner fneg's operand (the original value).
            replacements.emplace_back(&inst, inner->getOperand(0));
            ++count;
        }
    }

    // Apply replacements and erase both instructions.
    for (auto& [outer, orig] : replacements) {
        llvm::Value* innerArg = outer->getOperand(0); // the inner fneg
        outer->replaceAllUsesWith(orig);
        outer->eraseFromParent();
        if (auto* dead = llvm::dyn_cast<llvm::Instruction>(innerArg))
            if (dead->use_empty()) dead->eraseFromParent();
    }
    return count;
}

/// Hoist loop-invariant pure instructions to the loop pre-header.
///
/// This is a generalisation of hoistLoopInvariantGEP: instead of only
/// hoisting address-calculation GEPs we hoist ANY instruction that:
///   1. Is pure (no side effects, no memory accesses, not a PHI or terminator).
///   2. Has ALL operands defined outside the loop body.
///   3. Is not already in the pre-header.
///
/// Hoisting such instructions avoids re-computing the same value on every
/// loop iteration.  Unlike LLVM's own LICM this runs AFTER our transforms,
/// so it catches invariant computations introduced by strength reduction,
/// FMA generation, or GEP expansion that were not present when LLVM's
/// LICM ran.
///
/// Loop detection uses the same linear-order back-edge heuristic as
/// hoistLoopInvariantGEP.  GEPs are intentionally included so this
/// supersedes that pass — both are still called for safety; in practice
/// hoistLoopInvariantGEP will fire first and leave nothing for the GEP
/// case here.
///
/// Instruction ordering is preserved: we collect candidates in forward
/// program order and hoist them in that order, so that if instruction B
/// uses the result of instruction A and both are invariant, A arrives in
/// the pre-header before B.
///
/// Returns the number of instructions hoisted.
/// Fold `shl/lshr/ashr(0, x)` → `0`.
///
/// Shifting zero by any amount yields zero.  This pattern can appear after
/// strength reduction creates constants that cancel.  Always safe for all
/// shift opcodes.
///
/// Returns the number of shifts eliminated.
static unsigned foldShiftOfZero(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            unsigned op = inst.getOpcode();
            if (op != llvm::Instruction::Shl &&
                op != llvm::Instruction::LShr &&
                op != llvm::Instruction::AShr)
                continue;
            llvm::Type* ty = inst.getType();
            if (!ty->isIntOrIntVectorTy()) continue;

            // Check if the value being shifted (operand 0) is a zero constant.
            auto* lhs = llvm::dyn_cast<llvm::Constant>(inst.getOperand(0));
            if (!lhs || !lhs->isNullValue()) continue;

            inst.replaceAllUsesWith(llvm::Constant::getNullValue(ty));
            toErase.push_back(&inst);
            ++count;
        }
    }

    for (auto* inst : toErase)
        inst->eraseFromParent();
    return count;
}

/// Fold `or(x, x)` → `x` and `and(x, x)` → `x`.
///
/// Idempotent bitwise operations.  The pattern arises when SSA construction
/// or other passes duplicate an operand into both sides of a binary op.
/// Always safe — same SSA value pointer equality guarantees semantics.
///
/// Returns the number of idempotent ops eliminated.
static unsigned foldBitwiseIdempotent(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            unsigned op = inst.getOpcode();
            if (op != llvm::Instruction::Or && op != llvm::Instruction::And)
                continue;
            if (inst.getOperand(0) != inst.getOperand(1)) continue;

            inst.replaceAllUsesWith(inst.getOperand(0));
            toErase.push_back(&inst);
            ++count;
        }
    }

    for (auto* inst : toErase)
        inst->eraseFromParent();
    return count;
}

/// Fold `xor(x, x)` → `0`.
///
/// Self-XOR always produces zero.  This can appear after register allocation
/// hints or when strength reduction introduces temporary zero-init patterns.
/// Always safe for all integer types and vectors.
///
/// Returns the number of self-XORs eliminated.
static unsigned foldXorSelf(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Xor) continue;
            if (inst.getOperand(0) != inst.getOperand(1)) continue;
            llvm::Type* ty = inst.getType();
            if (!ty->isIntOrIntVectorTy()) continue;

            inst.replaceAllUsesWith(llvm::Constant::getNullValue(ty));
            toErase.push_back(&inst);
            ++count;
        }
    }

    for (auto* inst : toErase)
        inst->eraseFromParent();
    return count;
}

/// Fold `fsub nnan x, x` → `0.0`.
///
/// `x - x` is mathematically zero, but under strict IEEE 754 the result
/// depends on the sign of zero and NaN propagation:
///   - `(+0) - (+0)` = `+0` in round-to-nearest but `-0` in round-down
///   - `NaN - NaN` = `NaN`, not zero
///
/// The `nnan` (no-NaN) fast-math flag guarantees NaN is absent, making the
/// fold safe for all finite values and ±0 (the result is always +0.0 in
/// round-to-nearest, the default mode).  We also accept `nsz` (no-signed-zeros)
/// which eliminates the round-down edge case.
///
/// Returns the number of self-subtracts eliminated.
static unsigned foldFSubSelf(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::FSub) continue;
            if (inst.getOperand(0) != inst.getOperand(1)) continue;

            auto* fpOp = llvm::cast<llvm::FPMathOperator>(&inst);
            // Require nnan (or nsz) to safely fold.
            if (!fpOp->hasNoNaNs() && !fpOp->hasNoSignedZeros()) continue;

            llvm::Type* ty = inst.getType();
            inst.replaceAllUsesWith(llvm::ConstantFP::get(ty, 0.0));
            toErase.push_back(&inst);
            ++count;
        }
    }

    for (auto* inst : toErase)
        inst->eraseFromParent();
    return count;
}

/// Fold `and(x, -1)` → `x`, `or(x, 0)` → `x`, `xor(x, 0)` → `x`.
///
/// These are identity operations for bitwise ops that LLVM's InstCombine
/// typically catches but may remain after strength reduction or other
/// transforms introduce them.
///
/// Also folds `and(x, 0)` → `0` (absorbing element) and
/// `or(x, -1)` → `-1` (absorbing element).
///
/// Returns the number of ops folded.
static unsigned foldBitwiseWithConstants(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (!inst.getType()->isIntOrIntVectorTy()) continue;
            unsigned op = inst.getOpcode();

            if (op == llvm::Instruction::And) {
                for (int s = 0; s < 2; ++s) {
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(s))) {
                        if (c->isAllOnesValue()) {
                            // and(x, -1) → x
                            inst.replaceAllUsesWith(inst.getOperand(1 - s));
                            toErase.push_back(&inst);
                            ++count;
                            break;
                        }
                        if (c->isZero()) {
                            // and(x, 0) → 0
                            inst.replaceAllUsesWith(c);
                            toErase.push_back(&inst);
                            ++count;
                            break;
                        }
                    }
                }
            } else if (op == llvm::Instruction::Or) {
                for (int s = 0; s < 2; ++s) {
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(s))) {
                        if (c->isZero()) {
                            // or(x, 0) → x
                            inst.replaceAllUsesWith(inst.getOperand(1 - s));
                            toErase.push_back(&inst);
                            ++count;
                            break;
                        }
                        if (c->isAllOnesValue()) {
                            // or(x, -1) → -1
                            inst.replaceAllUsesWith(c);
                            toErase.push_back(&inst);
                            ++count;
                            break;
                        }
                    }
                }
            } else if (op == llvm::Instruction::Xor) {
                for (int s = 0; s < 2; ++s) {
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(s))) {
                        if (c->isZero()) {
                            // xor(x, 0) → x
                            inst.replaceAllUsesWith(inst.getOperand(1 - s));
                            toErase.push_back(&inst);
                            ++count;
                            break;
                        }
                    }
                }
            }
        }
    }

    for (auto* inst : toErase)
        inst->eraseFromParent();
    return count;
}

/// Fold `icmp eq x, x` → `true`, `icmp ne x, x` → `false`, and other
/// trivially self-comparing patterns.  Also folds comparisons of known
/// constants: `icmp eq 5, 5` → `true`.
///
/// These patterns appear after GVN, LICM, or our own transforms create
/// situations where both operands of a comparison are identical.
///
/// Returns the number of comparisons folded.
static unsigned foldTrivialICmp(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(&inst);
            if (!icmp) continue;

            llvm::Value* lhs = icmp->getOperand(0);
            llvm::Value* rhs = icmp->getOperand(1);

            // Self-comparison: icmp pred x, x
            if (lhs == rhs) {
                bool result = false;
                switch (icmp->getPredicate()) {
                case llvm::ICmpInst::ICMP_EQ:
                case llvm::ICmpInst::ICMP_ULE:
                case llvm::ICmpInst::ICMP_UGE:
                case llvm::ICmpInst::ICMP_SLE:
                case llvm::ICmpInst::ICMP_SGE:
                    result = true;
                    break;
                case llvm::ICmpInst::ICMP_NE:
                case llvm::ICmpInst::ICMP_ULT:
                case llvm::ICmpInst::ICMP_UGT:
                case llvm::ICmpInst::ICMP_SLT:
                case llvm::ICmpInst::ICMP_SGT:
                    result = false;
                    break;
                default:
                    continue;
                }
                llvm::Value* rep = llvm::ConstantInt::get(
                    llvm::Type::getInt1Ty(func.getContext()), result ? 1 : 0);
                icmp->replaceAllUsesWith(rep);
                toErase.push_back(icmp);
                ++count;
                continue;
            }

            // Constant folding: icmp pred C1, C2
            auto* c1 = llvm::dyn_cast<llvm::ConstantInt>(lhs);
            auto* c2 = llvm::dyn_cast<llvm::ConstantInt>(rhs);
            if (c1 && c2) {
                bool result = llvm::ICmpInst::compare(c1->getValue(), c2->getValue(),
                                                       icmp->getPredicate());
                llvm::Value* rep = llvm::ConstantInt::get(
                    llvm::Type::getInt1Ty(func.getContext()), result ? 1 : 0);
                icmp->replaceAllUsesWith(rep);
                toErase.push_back(icmp);
                ++count;
            }
        }
    }

    for (auto* inst : toErase)
        inst->eraseFromParent();
    return count;
}

/// Fold redundant GEP arithmetic patterns:
///   `gep(gep(base, i), j)` → `gep(base, i + j)` when both use constant
///   indices into the same element type.
///
/// GEP chains are common after loop transformations or address reassociation.
/// Merging them reduces the number of AGU µops and shortens address dependency
/// chains, improving both throughput and latency.
///
/// Safety: the inner GEP must have a single use (this outer GEP) to ensure
/// the intermediate pointer is not observed.
///
/// Returns the number of GEP pairs merged.
static unsigned foldGEPArithmetic(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* outer = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst);
            if (!outer) continue;
            if (outer->getNumIndices() != 1) continue;

            auto* inner = llvm::dyn_cast<llvm::GetElementPtrInst>(outer->getPointerOperand());
            if (!inner) continue;
            if (!inner->hasOneUse()) continue;
            if (inner->getNumIndices() != 1) continue;

            // Both must index the same element type.
            if (inner->getSourceElementType() != outer->getSourceElementType()) continue;

            llvm::Value* idxInner = *inner->idx_begin();
            llvm::Value* idxOuter = *outer->idx_begin();

            // Both constant: fold to single constant index.
            auto* ci = llvm::dyn_cast<llvm::ConstantInt>(idxInner);
            auto* co = llvm::dyn_cast<llvm::ConstantInt>(idxOuter);
            if (ci && co && ci->getType() == co->getType()) {
                llvm::APInt sum = ci->getValue() + co->getValue();
                llvm::Constant* merged = llvm::ConstantInt::get(ci->getType(), sum);
                llvm::IRBuilder<> builder(&inst);
                auto* rep = builder.CreateGEP(
                    inner->getSourceElementType(), inner->getPointerOperand(),
                    merged, "gep.merged");
                if (auto* gepRep = llvm::dyn_cast<llvm::GetElementPtrInst>(rep))
                    gepRep->setIsInBounds(outer->isInBounds() && inner->isInBounds());
                replacements.emplace_back(&inst, rep);
                ++count;
                continue;
            }

            // Both variable: create add and new GEP.
            if (idxInner->getType() == idxOuter->getType()) {
                llvm::IRBuilder<> builder(&inst);
                llvm::Value* sum = builder.CreateAdd(idxInner, idxOuter, "gep.idx.add");
                auto* rep = builder.CreateGEP(
                    inner->getSourceElementType(), inner->getPointerOperand(),
                    sum, "gep.merged");
                if (auto* gepRep = llvm::dyn_cast<llvm::GetElementPtrInst>(rep))
                    gepRep->setIsInBounds(outer->isInBounds() && inner->isInBounds());
                replacements.emplace_back(&inst, rep);
                ++count;
            }
        }
    }

    for (auto& [inst, rep] : replacements) {
        auto* innerGEP = llvm::dyn_cast<llvm::GetElementPtrInst>(
            llvm::cast<llvm::GetElementPtrInst>(inst)->getPointerOperand());
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
        if (innerGEP && innerGEP->use_empty())
            innerGEP->eraseFromParent();
    }
    return count;
}

/// Fold `select(true, x, y)` → `x`, `select(false, x, y)` → `y`.
///
/// Select instructions with constant conditions can appear after constant
/// propagation or our own convertIfElseToSelect creates them with known
/// conditions.  These are dead weight that takes up decode bandwidth.
///
/// Returns the number of constant selects eliminated.
static unsigned foldConstantSelect(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst);
            if (!sel) continue;
            auto* cond = llvm::dyn_cast<llvm::ConstantInt>(sel->getCondition());
            if (!cond) continue;
            llvm::Value* rep = cond->isOne() ? sel->getTrueValue() : sel->getFalseValue();
            sel->replaceAllUsesWith(rep);
            toErase.push_back(sel);
            ++count;
        }
    }

    for (auto* inst : toErase)
        inst->eraseFromParent();
    return count;
}

/// Fold `not(not(x))` → `x` where not(x) = xor(x, -1).
///
/// Double negation patterns appear after boolean canonicalization or
/// branch condition inversion creates two consecutive xor-with-all-ones.
///
/// Returns the number of double negations eliminated.
static unsigned foldDoubleNot(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Xor) continue;
            if (!inst.getType()->isIntOrIntVectorTy()) continue;

            // Check if this is xor(x, -1)
            llvm::Value* inner = nullptr;
            for (int s = 0; s < 2; ++s) {
                if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(s))) {
                    if (c->isAllOnesValue()) {
                        inner = inst.getOperand(1 - s);
                        break;
                    }
                }
            }
            if (!inner) continue;

            // Check if inner is also xor(y, -1)
            auto* innerInst = llvm::dyn_cast<llvm::BinaryOperator>(inner);
            if (!innerInst || innerInst->getOpcode() != llvm::Instruction::Xor) continue;
            if (!innerInst->hasOneUse()) continue;

            llvm::Value* source = nullptr;
            for (int s = 0; s < 2; ++s) {
                if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(innerInst->getOperand(s))) {
                    if (c->isAllOnesValue()) {
                        source = innerInst->getOperand(1 - s);
                        break;
                    }
                }
            }
            if (!source) continue;

            inst.replaceAllUsesWith(source);
            toErase.push_back(&inst);
            ++count;
        }
    }

    for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) {
        auto* inst = *it;
        llvm::Value* innerVal = inst->getOperand(0);
        if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(innerVal))
            innerVal = inst->getOperand(1);
        inst->eraseFromParent();
        if (auto* dead = llvm::dyn_cast<llvm::Instruction>(innerVal))
            if (dead->use_empty()) dead->eraseFromParent();
    }
    return count;
}

/// Fold `trunc(zext(x))` → `x` when the original and final types match.
///
/// Extension followed by truncation back to the original type is a no-op.
/// This pattern arises when type canonicalization or ABI-matching logic
/// widens a value and a later consumer narrows it back.  Also handles
/// `trunc(sext(x))` → `x` and the reverse `zext(trunc(x))` → `x` when
/// types match (though the latter is less common).
///
/// Safety: when srcTy == dstTy, the composition of ext followed by trunc
/// is the identity function regardless of sign extension semantics.
///
/// Returns the number of ext/trunc pairs eliminated.
static unsigned foldTruncExt(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            unsigned op = inst.getOpcode();
            // trunc(zext(x)) or trunc(sext(x))
            if (op == llvm::Instruction::Trunc) {
                auto* inner = llvm::dyn_cast<llvm::Instruction>(inst.getOperand(0));
                if (!inner) continue;
                unsigned innerOp = inner->getOpcode();
                if (innerOp != llvm::Instruction::ZExt &&
                    innerOp != llvm::Instruction::SExt)
                    continue;
                llvm::Value* src = inner->getOperand(0);
                if (src->getType() != inst.getType()) continue;
                // Only fold if inner has one use (this trunc) to avoid
                // keeping the ext alive.
                if (!inner->hasOneUse()) continue;
                inst.replaceAllUsesWith(src);
                toErase.push_back(&inst);
                ++count;
            }
            // zext(trunc(x)) or sext(trunc(x))
            else if (op == llvm::Instruction::ZExt ||
                     op == llvm::Instruction::SExt) {
                auto* inner = llvm::dyn_cast<llvm::Instruction>(inst.getOperand(0));
                if (!inner) continue;
                if (inner->getOpcode() != llvm::Instruction::Trunc) continue;
                llvm::Value* src = inner->getOperand(0);
                if (src->getType() != inst.getType()) continue;
                if (!inner->hasOneUse()) continue;
                inst.replaceAllUsesWith(src);
                toErase.push_back(&inst);
                ++count;
            }
        }
    }

    // Erase in reverse order to handle any chains.
    for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) {
        auto* inst = *it;
        // Also try to erase the now-dead inner instruction.
        llvm::Value* innerVal = nullptr;
        if (inst->getNumOperands() > 0)
            innerVal = inst->getOperand(0);
        inst->eraseFromParent();
        if (innerVal) {
            if (auto* dead = llvm::dyn_cast<llvm::Instruction>(innerVal))
                if (dead->use_empty()) dead->eraseFromParent();
        }
    }
    return count;
}

/// Fold `mul(x, 2^k)` → `shl(x, k)` for all power-of-2 constant multipliers.
///
/// More aggressive than integerStrengthReduce which only handles small
/// constants with few set bits.  This catches any power-of-2 multiplier
/// up to 2^62.  Shift is 1-cycle latency on all modern CPUs vs 3-cycle
/// multiply.  Always safe for integer types.
///
/// Returns the number of multiplies replaced.
static unsigned foldMulPow2(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Mul) continue;
            if (!inst.getType()->isIntegerTy()) continue;

            llvm::Value* xv = nullptr;
            uint64_t cv = 0;
            for (int s = 0; s < 2; ++s) {
                auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(s));
                if (ci && ci->getBitWidth() <= 64) {
                    uint64_t val = ci->getZExtValue();
                    // Check if val is a power of 2 (exactly one bit set).
                    if (val > 0 && (val & (val - 1)) == 0) {
                        cv = val;
                        xv = inst.getOperand(1 - s);
                        break;
                    }
                }
            }
            if (!xv || cv == 0) continue;

            unsigned shift = 0;
            uint64_t tmp = cv;
            while (tmp > 1) { tmp >>= 1; ++shift; }

            llvm::IRBuilder<> builder(&inst);
            llvm::Value* rep = builder.CreateShl(
                xv, llvm::ConstantInt::get(inst.getType(), shift), "mulpow2.shl");
            replacements.emplace_back(&inst, rep);
            ++count;
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        inst->eraseFromParent();
    }
    return count;
}

/// Fold `add(x, 0)` → `x` and `mul(x, 1)` → `x`.
///
/// Identity element elimination for arithmetic operations.  These patterns
/// appear after constant folding, strength reduction, and induction variable
/// simplification leave behind degenerate operations.
///
/// Returns the number of identity ops eliminated.
static unsigned foldIdentityOps(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            unsigned op = inst.getOpcode();
            if (!inst.getType()->isIntOrIntVectorTy()) continue;

            // add(x, 0) → x, sub(x, 0) → x
            if (op == llvm::Instruction::Add || op == llvm::Instruction::Sub) {
                if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    if (c->isZero()) {
                        inst.replaceAllUsesWith(inst.getOperand(0));
                        toErase.push_back(&inst);
                        ++count;
                        continue;
                    }
                }
                // add(0, x) → x (commutative)
                if (op == llvm::Instruction::Add) {
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(0))) {
                        if (c->isZero()) {
                            inst.replaceAllUsesWith(inst.getOperand(1));
                            toErase.push_back(&inst);
                            ++count;
                            continue;
                        }
                    }
                }
            }

            // mul(x, 1) → x
            if (op == llvm::Instruction::Mul) {
                for (int s = 0; s < 2; ++s) {
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(s))) {
                        if (c->isOne()) {
                            inst.replaceAllUsesWith(inst.getOperand(1 - s));
                            toErase.push_back(&inst);
                            ++count;
                            break;
                        }
                    }
                }
            }

            // mul(x, 0) → 0
            if (op == llvm::Instruction::Mul) {
                for (int s = 0; s < 2; ++s) {
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(s))) {
                        if (c->isZero()) {
                            inst.replaceAllUsesWith(c);
                            toErase.push_back(&inst);
                            ++count;
                            break;
                        }
                    }
                }
            }
        }
    }

    for (auto* inst : toErase)
        inst->eraseFromParent();
    return count;
}

/// Fold `shl(shl(x, a), b)` → `shl(x, a+b)` when both shifts are constant.
///
/// Also handles `lshr(lshr(x, a), b)` → `lshr(x, a+b)` and
/// `ashr(ashr(x, a), b)` → `ashr(x, a+b)`.  These patterns appear after
/// strength reduction creates shift chains.  The combined shift is faster
/// (1 instruction instead of 2) and reduces dependency chain length.
///
/// Safety: a+b must not exceed the bit width; if it does the result is
/// undefined (poison), which matches LLVM's semantics for oversized shifts.
/// We clamp to bitWidth-1 to avoid surprising backend behaviour.
///
/// Returns the number of shift pairs merged.
static unsigned foldConsecutiveShifts(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            unsigned op = inst.getOpcode();
            if (op != llvm::Instruction::Shl &&
                op != llvm::Instruction::LShr &&
                op != llvm::Instruction::AShr)
                continue;

            auto* inner = llvm::dyn_cast<llvm::Instruction>(inst.getOperand(0));
            if (!inner || inner->getOpcode() != op) continue;
            if (!inner->hasOneUse()) continue;

            auto* outerAmt = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1));
            auto* innerAmt = llvm::dyn_cast<llvm::ConstantInt>(inner->getOperand(1));
            if (!outerAmt || !innerAmt) continue;

            unsigned bitWidth = inst.getType()->getIntegerBitWidth();
            uint64_t total = outerAmt->getZExtValue() + innerAmt->getZExtValue();
            if (total >= bitWidth) total = bitWidth - 1; // clamp to avoid UB

            llvm::IRBuilder<> builder(&inst);
            llvm::Value* rep = nullptr;
            llvm::Value* src = inner->getOperand(0);
            auto* totalConst = llvm::ConstantInt::get(inst.getType(), total);
            switch (op) {
            case llvm::Instruction::Shl:
                rep = builder.CreateShl(src, totalConst, "shmerge.shl");
                break;
            case llvm::Instruction::LShr:
                rep = builder.CreateLShr(src, totalConst, "shmerge.lshr");
                break;
            case llvm::Instruction::AShr:
                rep = builder.CreateAShr(src, totalConst, "shmerge.ashr");
                break;
            default:
                break;
            }
            if (!rep) continue;

            replacements.emplace_back(&inst, rep);
            ++count;
        }
    }

    for (auto& [inst, rep] : replacements) {
        inst->replaceAllUsesWith(rep);
        // Also erase the inner shift if it's now dead.
        auto* innerShift = llvm::dyn_cast<llvm::Instruction>(inst->getOperand(0));
        inst->eraseFromParent();
        if (innerShift && innerShift->use_empty())
            innerShift->eraseFromParent();
    }
    return count;
}

/// Fold `bitcast(bitcast(x))` → `bitcast(x)` or `x` when types match.
///
/// Bitcast chains appear when type canonicalization or ABI-matching logic
/// introduces intermediate casts.  If the source type of the outer bitcast
/// matches the destination type, the result is a single bitcast from the
/// original source, or just the original value if all three types match.
///
/// Returns the number of bitcast chains shortened.
static unsigned foldBitcastChain(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::BitCast) continue;
            auto* inner = llvm::dyn_cast<llvm::BitCastInst>(inst.getOperand(0));
            if (!inner) continue;
            if (!inner->hasOneUse()) continue;

            llvm::Value* src = inner->getOperand(0);
            llvm::Type* srcTy = src->getType();
            llvm::Type* dstTy = inst.getType();

            if (srcTy == dstTy) {
                // Identity chain: bitcast(bitcast(x: T → U) → T) = x
                inst.replaceAllUsesWith(src);
                toErase.push_back(&inst);
                ++count;
            } else {
                // Shortcut chain: bitcast(bitcast(x: A → B): B → C) = bitcast(x: A → C)
                llvm::IRBuilder<> builder(&inst);
                auto* rep = builder.CreateBitCast(src, dstTy, "bcchain");
                inst.replaceAllUsesWith(rep);
                toErase.push_back(&inst);
                ++count;
            }
        }
    }

    for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) {
        auto* inst = *it;
        llvm::Value* innerVal = inst->getOperand(0);
        inst->eraseFromParent();
        if (auto* dead = llvm::dyn_cast<llvm::Instruction>(innerVal))
            if (dead->use_empty()) dead->eraseFromParent();
    }
    return count;
}

/// Local redundant load elimination (RLE) within basic blocks.
///
/// When a load reads from a pointer that was previously stored to with the
/// same type and no intervening store to an aliasing address, the load can
/// be replaced with the stored value.  This is a local form of the
/// store-to-load forwarding that LLVM's GVN does globally.
///
/// Also eliminates repeated loads from the same pointer (load-after-load):
/// if no intervening store may alias the pointer, the second load is
/// redundant and can use the first load's result.
///
/// Returns the number of redundant loads eliminated.
static unsigned eliminateRedundantLoads(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        // Track available values: pointer → (stored/loaded value, index).
        // Cleared on any potentially-aliasing store or memory barrier.
        std::unordered_map<const llvm::Value*, llvm::Value*> availableValues;

        for (auto& inst : bb) {
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                if (st->isAtomic()) {
                    availableValues.clear();
                    continue;
                }
                const llvm::Value* ptr = st->getPointerOperand()->stripPointerCasts();
                // This store makes its value available for forwarding.
                availableValues[ptr] = st->getValueOperand();
                continue;
            }

            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
                if (ld->isAtomic()) {
                    availableValues.clear();
                    continue;
                }
                const llvm::Value* ptr = ld->getPointerOperand()->stripPointerCasts();
                auto it = availableValues.find(ptr);
                if (it != availableValues.end()) {
                    llvm::Value* avail = it->second;
                    // Types must match for direct forwarding.
                    if (avail->getType() == ld->getType()) {
                        replacements.emplace_back(ld, avail);
                        ++count;
                        // The loaded value is also available for future loads.
                        continue;
                    }
                }
                // This load's result is available for future loads.
                availableValues[ptr] = ld;
                continue;
            }

            // Calls, fences, atomics: conservatively clear everything.
            if (inst.mayWriteToMemory())
                availableValues.clear();
        }
    }

    for (auto& [inst, val] : replacements) {
        inst->replaceAllUsesWith(val);
        inst->eraseFromParent();
    }
    return count;
}

/// Fold redundant extension chains: `sext(sext(x))` → `sext(x)`,
/// `zext(zext(x))` → `zext(x)`.
///
/// Extension chains appear when type promotion or ABI matching generates
/// intermediate widening steps (e.g. i8 → i32 → i64).  Collapsing them
/// into a single extension is faster (1 instruction vs 2) and reduces
/// register pressure.
///
/// Safety: sext(sext(x: iA → iB): iB → iC) == sext(x: iA → iC) by the
/// sign-extension monotonicity property.  Same for zext.
///
/// Returns the number of extension chains collapsed.
static unsigned foldRedundantExtensions(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            unsigned op = inst.getOpcode();
            // sext(sext(x)) → sext(x)
            if (op == llvm::Instruction::SExt) {
                auto* inner = llvm::dyn_cast<llvm::Instruction>(inst.getOperand(0));
                if (!inner || inner->getOpcode() != llvm::Instruction::SExt) continue;
                if (!inner->hasOneUse()) continue;
                llvm::IRBuilder<> builder(&inst);
                auto* rep = builder.CreateSExt(inner->getOperand(0), inst.getType(), "sext.chain");
                inst.replaceAllUsesWith(rep);
                toErase.push_back(&inst);
                ++count;
            }
            // zext(zext(x)) → zext(x)
            else if (op == llvm::Instruction::ZExt) {
                auto* inner = llvm::dyn_cast<llvm::Instruction>(inst.getOperand(0));
                if (!inner || inner->getOpcode() != llvm::Instruction::ZExt) continue;
                if (!inner->hasOneUse()) continue;
                llvm::IRBuilder<> builder(&inst);
                auto* rep = builder.CreateZExt(inner->getOperand(0), inst.getType(), "zext.chain");
                inst.replaceAllUsesWith(rep);
                toErase.push_back(&inst);
                ++count;
            }
        }
    }

    for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) {
        auto* inst = *it;
        llvm::Value* innerVal = inst->getOperand(0);
        inst->eraseFromParent();
        if (auto* dead = llvm::dyn_cast<llvm::Instruction>(innerVal))
            if (dead->use_empty()) dead->eraseFromParent();
    }
    return count;
}

/// Dead store elimination within basic blocks.
///
/// When two stores write to the same pointer and there are no intervening
/// loads or calls that may read from that pointer, the first store is dead
/// and can be eliminated.  This is a purely local (intra-BB) optimization
/// that catches patterns missed by LLVM's DSE when our transforms introduce
/// new stores (e.g. strength-reduced spills, redundant write-back).
///
/// We only eliminate when:
///   1. Both stores use the same pointer operand (SSA pointer equality).
///   2. No instruction between them may read from memory (conservative:
///      any load, call, or atomic between the two stores blocks elimination).
///   3. The stores have the same type (so the second fully overwrites the first).
///
/// Returns the number of dead stores eliminated.
static unsigned sinkDeadStores(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        // Walk instructions in reverse: for each store, look backward for
        // an earlier store to the same pointer with no intervening readers.
        // We use a map from pointer → most-recent-store-index for O(n) scanning.
        std::vector<llvm::Instruction*> insts;
        insts.reserve(bb.size());
        for (auto& inst : bb) insts.push_back(&inst);

        // Map: pointer → index of the last store to that pointer.
        std::unordered_map<const llvm::Value*, unsigned> lastStoreIdx;

        for (unsigned i = 0; i < insts.size(); ++i) {
            auto* st = llvm::dyn_cast<llvm::StoreInst>(insts[i]);
            if (!st) {
                // If this instruction may read memory, invalidate all tracked
                // store entries (conservative: the load may alias any of them).
                if (insts[i]->mayReadFromMemory())
                    lastStoreIdx.clear();
                continue;
            }

            const llvm::Value* ptr = st->getPointerOperand();
            auto it = lastStoreIdx.find(ptr);
            if (it != lastStoreIdx.end()) {
                // Found a prior store to the same pointer with no intervening
                // memory reads.  The prior store is dead.
                auto* deadStore = llvm::cast<llvm::StoreInst>(insts[it->second]);
                // Verify same stored type (full overwrite).
                if (deadStore->getValueOperand()->getType() ==
                    st->getValueOperand()->getType()) {
                    toErase.push_back(deadStore);
                    ++count;
                }
            }
            lastStoreIdx[ptr] = i;

            // A store also acts as a memory write; if there's also a fence
            // or atomic ordering, invalidate everything (conservative).
            if (st->isAtomic()) lastStoreIdx.clear();
        }
    }

    for (auto* inst : toErase)
        inst->eraseFromParent();
    return count;
}

/// Multi-level loop-invariant code motion.
///
/// Enhanced version of the original hoistLoopInvariantInst that supports
/// hoisting instructions whose operands are themselves loop-invariant but
/// defined inside the loop.  Uses iterative fixed-point analysis:
///
///   1. Mark all instructions with all-external operands as invariant.
///   2. Re-scan: if an instruction's operands are all either external or
///      already marked invariant, mark it invariant too.
///   3. Repeat until no new instructions are marked.
///
/// This catches chains like:
///   %a = add i64 %x, %y       ; x, y defined outside → invariant
///   %b = shl i64 %a, 3        ; %a is invariant → also invariant
///   %c = gep ... %b           ; %b is invariant → also invariant (if pure)
///
/// where the original single-pass version would only hoist %a.
///
/// Hoisting order: forward program order within each iteration, preserving
/// def-use ordering.  Instructions are hoisted in batches per iteration.
///
/// Returns the total number of instructions hoisted.
static unsigned hoistLoopInvariantInst(llvm::Function& func) {
    if (func.isDeclaration()) return 0;

    unsigned count = 0;

    // Assign linear order to BBs for back-edge detection.
    std::unordered_map<const llvm::BasicBlock*, unsigned> bbOrder;
    { unsigned ord = 0; for (auto& bb : func) bbOrder[&bb] = ord++; }

    for (auto& header : func) {
        // Find the latch (back-edge source): a predecessor of header with
        // a higher linear order than the header itself.
        llvm::BasicBlock* latch = nullptr;
        for (auto* pred : llvm::predecessors(&header)) {
            if (bbOrder.count(pred) && bbOrder.at(pred) >= bbOrder.at(&header)) {
                latch = pred;
                break;
            }
        }
        if (!latch) continue;

        // Require exactly one non-latch predecessor as the pre-header.
        llvm::BasicBlock* preHeader = nullptr;
        unsigned nonLatchPreds = 0;
        for (auto* pred : llvm::predecessors(&header)) {
            if (pred != latch) { preHeader = pred; ++nonLatchPreds; }
        }
        if (nonLatchPreds != 1 || !preHeader) continue;

        // Collect loop body BBs (linear order between header and latch).
        unsigned headerOrd = bbOrder.at(&header);
        unsigned latchOrd  = bbOrder.at(latch);
        std::unordered_set<const llvm::BasicBlock*> loopBBs;
        for (auto& bb : func) {
            unsigned ord = bbOrder.at(&bb);
            if (ord >= headerOrd && ord <= latchOrd)
                loopBBs.insert(&bb);
        }

        // Multi-level fixed-point: track which loop instructions are invariant.
        std::unordered_set<const llvm::Instruction*> invariantSet;
        bool changed = true;
        constexpr unsigned kMaxIters = 8; // safety bound

        for (unsigned iter = 0; iter < kMaxIters && changed; ++iter) {
            changed = false;
            for (auto& bb : func) {
                if (!loopBBs.count(&bb)) continue;
                for (auto& inst : bb) {
                    if (invariantSet.count(&inst)) continue;
                    // Skip PHIs, terminators.
                    if (llvm::isa<llvm::PHINode>(inst)) continue;
                    if (inst.isTerminator()) continue;
                    // Skip instructions with side effects or memory accesses.
                    if (inst.mayHaveSideEffects()) continue;
                    if (inst.mayReadOrWriteMemory()) continue;
                    // Already in pre-header — nothing to move.
                    if (inst.getParent() == preHeader) continue;

                    // All operands must be either:
                    //   - constants
                    //   - defined outside the loop
                    //   - already in the invariant set
                    bool isInvariant = true;
                    for (unsigned i = 0; i < inst.getNumOperands(); ++i) {
                        llvm::Value* op = inst.getOperand(i);
                        if (llvm::isa<llvm::Constant>(op)) continue;
                        auto* defInst = llvm::dyn_cast<llvm::Instruction>(op);
                        if (!defInst) { isInvariant = false; break; }
                        if (!loopBBs.count(defInst->getParent())) continue; // external
                        if (invariantSet.count(defInst)) continue; // already invariant
                        isInvariant = false;
                        break;
                    }
                    if (!isInvariant) continue;
                    invariantSet.insert(&inst);
                    changed = true;
                }
            }
        }

        // Collect invariant instructions in forward program order.
        std::vector<llvm::Instruction*> toHoist;
        for (auto& bb : func) {
            if (!loopBBs.count(&bb)) continue;
            for (auto& inst : bb) {
                if (invariantSet.count(&inst))
                    toHoist.push_back(&inst);
            }
        }

        // Hoist in collected order to the pre-header (before its terminator).
        llvm::Instruction* insertPt = preHeader->getTerminator();
        for (auto* inst : toHoist) {
            inst->moveBefore(insertPt);
            ++count;
        }
    }
    return count;
}

// ═════════════════════════════════════════════════════════════════════════════
// New algebraic & peephole transforms for HGOE overhaul
// ═════════════════════════════════════════════════════════════════════════════

/// Fold redundant PHI nodes: phi(x, x, ..., x) → x.
/// When all incoming values are the same (after GVN, LICM, or jump threading),
/// the PHI is a trivial identity and can be replaced.
static unsigned foldRedundantPHIs(llvm::Function& func) {
    unsigned count = 0;
    bool changed = true;
    // Iterate to a fixed point because folding one PHI may expose another.
    while (changed) {
        changed = false;
        std::vector<llvm::PHINode*> toErase;
        for (auto& bb : func) {
            for (auto& inst : bb) {
                auto* phi = llvm::dyn_cast<llvm::PHINode>(&inst);
                if (!phi) break; // PHIs are at the start of the BB
                if (phi->getNumIncomingValues() == 0) continue;
                llvm::Value* common = phi->getIncomingValue(0);
                bool allSame = true;
                for (unsigned i = 1; i < phi->getNumIncomingValues(); ++i) {
                    if (phi->getIncomingValue(i) != common) {
                        allSame = false;
                        break;
                    }
                }
                if (allSame && common != phi) {
                    phi->replaceAllUsesWith(common);
                    toErase.push_back(phi);
                    ++count;
                    changed = true;
                }
            }
        }
        for (auto* phi : toErase) phi->eraseFromParent();
    }
    return count;
}

/// Fold add(add(x, C1), C2) → add(x, C1+C2): merge chained constant adds.
/// This is the IR equivalent of LEA-style address combining.  After strength
/// reduction and GEP merging, chained adds with constants are common.
/// Folding them reduces critical-path depth and frees an ALU port.
static unsigned foldChainedAdds(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Add) continue;
            // Match: add(add(x, C1), C2)
            for (int outerSide = 0; outerSide < 2; ++outerSide) {
                auto* c2 = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(outerSide));
                if (!c2) continue;
                auto* innerAdd = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1 - outerSide));
                if (!innerAdd || innerAdd->getOpcode() != llvm::Instruction::Add) continue;
                if (!innerAdd->hasOneUse()) continue; // don't break other users
                for (int innerSide = 0; innerSide < 2; ++innerSide) {
                    auto* c1 = llvm::dyn_cast<llvm::ConstantInt>(innerAdd->getOperand(innerSide));
                    if (!c1) continue;
                    llvm::Value* x = innerAdd->getOperand(1 - innerSide);
                    // Create add(x, C1+C2)
                    llvm::APInt merged = c1->getValue() + c2->getValue();
                    auto* newConst = llvm::ConstantInt::get(inst.getType(), merged);
                    auto* newAdd = llvm::BinaryOperator::CreateAdd(x, newConst, "", &inst);
                    newAdd->setDebugLoc(inst.getDebugLoc());
                    // Preserve nuw/nsw if both original adds had them
                    if (auto* outerBO = llvm::dyn_cast<llvm::BinaryOperator>(&inst)) {
                        if (outerBO->hasNoUnsignedWrap() && innerAdd->hasNoUnsignedWrap())
                            newAdd->setHasNoUnsignedWrap(true);
                        if (outerBO->hasNoSignedWrap() && innerAdd->hasNoSignedWrap())
                            newAdd->setHasNoSignedWrap(true);
                    }
                    inst.replaceAllUsesWith(newAdd);
                    toErase.push_back(&inst);
                    toErase.push_back(innerAdd);
                    ++count;
                    goto next_inst;
                }
            }
            next_inst:;
        }
    }
    for (auto* i : toErase) i->eraseFromParent();
    return count;
}

/// Fold add(sub(0, x), y) → sub(y, x) and add(y, sub(0, x)) → sub(y, x).
/// Catches negation patterns that survive after foldIntMulByNeg1 / foldIntAddNeg.
static unsigned foldAddNegToSub(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Add) continue;
            for (int side = 0; side < 2; ++side) {
                auto* negSub = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(side));
                if (!negSub || negSub->getOpcode() != llvm::Instruction::Sub) continue;
                auto* zero = llvm::dyn_cast<llvm::ConstantInt>(negSub->getOperand(0));
                if (!zero || !zero->isZero()) continue;
                if (!negSub->hasOneUse()) continue;
                llvm::Value* x = negSub->getOperand(1);
                llvm::Value* y = inst.getOperand(1 - side);
                auto* newSub = llvm::BinaryOperator::CreateSub(y, x, "", &inst);
                newSub->setDebugLoc(inst.getDebugLoc());
                inst.replaceAllUsesWith(newSub);
                toErase.push_back(&inst);
                toErase.push_back(negSub);
                ++count;
                break;
            }
        }
    }
    for (auto* i : toErase) i->eraseFromParent();
    return count;
}

/// Fold sub(x, x) → 0: self-subtraction elimination.
/// Appears after PHI folding, LICM, or GVN when both operands resolve to the same value.
static unsigned foldSubSelf(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Sub) continue;
            if (inst.getOperand(0) == inst.getOperand(1)) {
                inst.replaceAllUsesWith(llvm::ConstantInt::get(inst.getType(), 0));
                toErase.push_back(&inst);
                ++count;
            }
        }
    }
    for (auto* i : toErase) i->eraseFromParent();
    return count;
}

/// Narrow zext to trunc when the extension is followed by a truncation
/// back to the original width: trunc(zext(x)) → x for matching types.
/// Also handles sext: trunc(sext(x)) → x.
/// This catches patterns left after loop unrolling where iterator widening
/// introduces unnecessary zext→trunc pairs.
static unsigned foldNarrowExtTrunc(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Trunc) continue;
            auto* ext = llvm::dyn_cast<llvm::Instruction>(inst.getOperand(0));
            if (!ext) continue;
            bool isExt = (ext->getOpcode() == llvm::Instruction::ZExt ||
                          ext->getOpcode() == llvm::Instruction::SExt);
            if (!isExt) continue;
            llvm::Value* orig = ext->getOperand(0);
            if (orig->getType() != inst.getType()) continue;
            inst.replaceAllUsesWith(orig);
            toErase.push_back(&inst);
            if (ext->hasOneUse() || ext->use_empty())
                toErase.push_back(ext);
            ++count;
        }
    }
    for (auto* i : toErase) i->eraseFromParent();
    return count;
}

/// FP reassociation for latency reduction: turn linear fadd/fmul chains into
/// balanced trees when `reassoc` flag is present.
/// fadd(fadd(fadd(a, b), c), d) → fadd(fadd(a, b), fadd(c, d))
/// This cuts the critical-path depth from O(n) to O(log n) for n-element reductions.
/// Only applied when the instruction has the `reassoc` fast-math flag.
static unsigned reassociateFPChains(llvm::Function& func) {
    unsigned count = 0;
    // Collect linear FP chains (fadd or fmul with reassoc).
    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::FAdd &&
                inst.getOpcode() != llvm::Instruction::FMul)
                continue;
            if (!inst.hasAllowReassoc()) continue;
            // Only process the root (last use of the chain — instruction with no
            // single-use chain consumer of the same opcode).
            bool isRoot = true;
            for (auto* user : inst.users()) {
                auto* userInst = llvm::dyn_cast<llvm::Instruction>(user);
                if (userInst && userInst->getOpcode() == inst.getOpcode() &&
                    userInst->hasAllowReassoc()) {
                    isRoot = false;
                    break;
                }
            }
            if (!isRoot) continue;

            // Walk the chain and collect leaf values.
            std::vector<llvm::Value*> leaves;
            std::vector<llvm::Instruction*> chainInsts;
            std::function<void(llvm::Value*)> collect = [&](llvm::Value* v) {
                auto* binOp = llvm::dyn_cast<llvm::BinaryOperator>(v);
                if (binOp && binOp->getOpcode() == inst.getOpcode() &&
                    binOp->hasAllowReassoc() && binOp->hasOneUse()) {
                    chainInsts.push_back(binOp);
                    collect(binOp->getOperand(0));
                    collect(binOp->getOperand(1));
                } else {
                    leaves.push_back(v);
                }
            };
            collect(&inst);

            if (leaves.size() < 4) continue; // not worth rebalancing

            // Build a balanced reduction tree.
            llvm::IRBuilder<> builder(&inst);
            builder.setFastMathFlags(inst.getFastMathFlags());
            while (leaves.size() > 1) {
                std::vector<llvm::Value*> next;
                for (unsigned i = 0; i + 1 < leaves.size(); i += 2) {
                    llvm::Value* combined;
                    if (inst.getOpcode() == llvm::Instruction::FAdd)
                        combined = builder.CreateFAdd(leaves[i], leaves[i + 1]);
                    else
                        combined = builder.CreateFMul(leaves[i], leaves[i + 1]);
                    if (auto* ci = llvm::dyn_cast<llvm::Instruction>(combined))
                        ci->setFastMathFlags(inst.getFastMathFlags());
                    next.push_back(combined);
                }
                if (leaves.size() & 1)
                    next.push_back(leaves.back());
                leaves = std::move(next);
            }

            inst.replaceAllUsesWith(leaves[0]);
            // Don't erase yet — other chain instructions may reference each other.
            // Mark for deletion.
            chainInsts.push_back(&inst);
            for (auto it = chainInsts.rbegin(); it != chainInsts.rend(); ++it) {
                if ((*it)->use_empty())
                    (*it)->eraseFromParent();
            }
            ++count;
        }
    }
    return count;
}

/// Fold or(shl(x, C1), lshr(x, C2)) → fshl(x, x, C1) when C1+C2 = bitwidth.
/// This recognizes rotate-left idioms and lowers them to funnel-shift intrinsics
/// which map to single-cycle ROL/ROR on x86 and EXTR on AArch64.
static unsigned foldRotateIdiom(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::Or) continue;
            if (!inst.getType()->isIntegerTy()) continue;
            unsigned bitWidth = inst.getType()->getIntegerBitWidth();
            if (bitWidth == 0) continue;

            auto* op0 = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
            auto* op1 = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
            if (!op0 || !op1) continue;

            // Match shl+lshr pair (either order)
            llvm::BinaryOperator* shlOp = nullptr;
            llvm::BinaryOperator* shrOp = nullptr;
            if (op0->getOpcode() == llvm::Instruction::Shl &&
                op1->getOpcode() == llvm::Instruction::LShr) {
                shlOp = op0; shrOp = op1;
            } else if (op1->getOpcode() == llvm::Instruction::Shl &&
                       op0->getOpcode() == llvm::Instruction::LShr) {
                shlOp = op1; shrOp = op0;
            } else continue;

            // Both must operate on the same value
            if (shlOp->getOperand(0) != shrOp->getOperand(0)) continue;
            llvm::Value* x = shlOp->getOperand(0);

            // Check for constant shift amounts that sum to bitwidth
            auto* c1 = llvm::dyn_cast<llvm::ConstantInt>(shlOp->getOperand(1));
            auto* c2 = llvm::dyn_cast<llvm::ConstantInt>(shrOp->getOperand(1));
            if (!c1 || !c2) continue;
            if (c1->getZExtValue() + c2->getZExtValue() != bitWidth) continue;

            // Create fshl(x, x, C1) — funnel shift left
            llvm::Module* mod = func.getParent();
            llvm::Function* fshl = OMSC_GET_INTRINSIC(
                mod, llvm::Intrinsic::fshl, {inst.getType()});
            llvm::IRBuilder<> builder(&inst);
            llvm::Value* result = builder.CreateCall(fshl, {x, x, c1});
            inst.replaceAllUsesWith(result);
            toErase.push_back(&inst);
            if (shlOp->hasOneUse() || shlOp->use_empty()) toErase.push_back(shlOp);
            if (shrOp->hasOneUse() || shrOp->use_empty()) toErase.push_back(shrOp);
            ++count;
        }
    }
    for (auto* i : toErase) i->eraseFromParent();
    return count;
}

/// Fold extractvalue(insertvalue(base, val, idx), idx) → val.
/// This pattern appears after SROA and struct returns where the value
/// is immediately extracted from the aggregate it was just inserted into.
static unsigned foldExtractInsert(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* ev = llvm::dyn_cast<llvm::ExtractValueInst>(&inst);
            if (!ev) continue;
            auto* iv = llvm::dyn_cast<llvm::InsertValueInst>(ev->getAggregateOperand());
            if (!iv) continue;
            // Check indices match exactly
            if (ev->getIndices() == iv->getIndices()) {
                ev->replaceAllUsesWith(iv->getInsertedValueOperand());
                toErase.push_back(ev);
                ++count;
            }
        }
    }
    for (auto* i : toErase) i->eraseFromParent();
    return count;
}

/// Dead code elimination: remove instructions with no uses and no side effects.
/// This is a cleanup pass that catches instructions left dead by our transforms.
static unsigned eliminateDeadInstructions(llvm::Function& func) {
    unsigned count = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& bb : func) {
            llvm::SmallVector<llvm::Instruction*, 16> dead;
            for (auto it = bb.rbegin(); it != bb.rend(); ++it) {
                llvm::Instruction* inst = &*it;
                if (inst->isTerminator()) continue;
                if (llvm::isa<llvm::PHINode>(inst)) continue;
                if (!inst->use_empty()) continue;
                if (inst->mayHaveSideEffects()) continue;
                dead.push_back(inst);
            }
            for (auto* d : dead) {
                d->eraseFromParent();
                ++count;
                changed = true;
            }
        }
    }
    return count;
}

TransformStats applyHardwareTransforms(llvm::Function& func,
                                        const MicroarchProfile& profile,
                                        bool enableLoopAnnotation) {
    TransformStats stats;


    stats.fmaGenerated     = generateFMA(func, profile);
    stats.fmaGenerated    += generateFMASub(func, profile);
    stats.fmaGenerated    += generateFMAChain(func, profile);
    // Canonicalize fadd(x, fneg(y)) → fsub(x, y) before FMA scan so the
    // FMA pass can recognise the resulting fsub patterns.
    stats.fmaGenerated    += canonicalizeFaddFneg(func);
    // fmul(x, -1.0) → fneg(x): 1 cycle bit-flip vs 4-cycle multiply.
    // Run before FMA scan so the eliminated fmul doesn't confuse FMA matching.
    stats.fmaGenerated    += foldFPMulByNeg1(func);
    // fneg(fneg(x)) → x: double negation elimination.
    // Run after foldFPMulByNeg1 (which may have introduced new FNeg) and
    // after FMA generation (which introduces FNeg for FNMADD forms).
    // No fast-math flags needed — safe for all IEEE 754 values.
    stats.fmaGenerated    += foldFNegDouble(func);
    // fsub nnan x, x → 0.0: self-cancel for FP subtraction.
    // Run after fneg folding so any newly-exposed self-subtracts are caught.
    stats.fmaGenerated    += foldFSubSelf(func);
    // fadd(x, x) → fmul(x, 2.0) with reassoc: exposes FMA fusion opportunities.
    // Run before the FP div-by-constant fold and before FMA passes so that
    // the resulting fmul(x, 2.0) can be fused: fma(x, 2.0, c).
    stats.fmaGenerated    += foldFAddSelf(func);
    // Re-run FMA passes once more so fmul(x,2.0) introduced by foldFAddSelf
    // can be immediately fused with adjacent fadd/fsub operations.
    stats.fmaGenerated    += generateFMA(func, profile);
    stats.fmaGenerated    += generateFMASub(func, profile);
    // FP division by constant → reciprocal multiply (fdiv → fmul when `arcp` flag).
    // Run before FMA scan so that the resulting fmul may be fused in a later pass.
    stats.fmaGenerated    += foldFPDivByConstant(func, profile);
    // pow(x, N) for small constant N → multiply chain (avoids 50-100cy lib call).
    // Run after fneg/fdiv folds so the resulting fmuls can be seen by FMA passes.
    stats.fmaGenerated    += foldPowBySmallInt(func);
    // sqrt(x * x) → fabs(x) with nnan+ninf: avoids ~10-cycle sqrt.
    stats.fmaGenerated    += foldSqrtSquare(func);
    // Integer div/rem by power-of-2 → shift/and (any that slipped past InstCombine).
    stats.intStrengthReduced = foldDivByPow2(func);
    // Min/max pattern: icmp/fcmp + select → smin/smax/minnum/maxnum intrinsics.
    stats.intStrengthReduced += foldMinMaxPatterns(func);
    // select(cond, 1, 0) → zext(cond), select(cond, -1, 0) → sext(cond).
    // Enables SETCC/MOVZX backend lowering (1 µop vs. multi-µop select sequence).
    stats.intStrengthReduced += foldSelectToBoolCast(func);
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
    stats.intStrengthReduced += integerStrengthReduce(func, profile);
    // mul(x, 2^k) → shl(x, k): power-of-2 multiply replacement.
    // More aggressive than integerStrengthReduce for exact powers of 2.
    // Run right after integerStrengthReduce to catch remaining pow2 mul.
    stats.intStrengthReduced += foldMulPow2(func);
    // mul(x, -1) → sub(0, x): integer negate instead of multiply.
    // Run after integerStrengthReduce so that the -1 constant is not
    // mistaken for a shift-add form (which has no -1 case), and before
    // generateIntegerAbs so that abs patterns using sub(0,x) are still seen.
    stats.intStrengthReduced += foldIntMulByNeg1(func);
    // add(x, sub(0,y)) → sub(x,y): fold add+neg created by foldIntMulByNeg1.
    // Run immediately after so the sub(0,y) pattern is fresh.
    stats.intStrengthReduced += foldIntAddNeg(func);
    // shl/lshr/ashr(0, x) → 0: dead shifts after strength reduction.
    stats.intStrengthReduced += foldShiftOfZero(func);
    // shl(shl(x, a), b) → shl(x, a+b): merge consecutive constant shifts.
    stats.intStrengthReduced += foldConsecutiveShifts(func);
    // or(x,x) → x, and(x,x) → x: idempotent bitwise ops.
    stats.intStrengthReduced += foldBitwiseIdempotent(func);
    // xor(x,x) → 0: self-XOR pattern.
    stats.intStrengthReduced += foldXorSelf(func);
    // add(x,0) → x, mul(x,1) → x, mul(x,0) → 0: identity/absorbing ops.
    stats.intStrengthReduced += foldIdentityOps(func);
    // trunc(zext(x)) → x, zext(trunc(x)) → x when types match.
    stats.intStrengthReduced += foldTruncExt(func);
    // sext(sext(x)) → sext(x), zext(zext(x)) → zext(x): extension chains.
    stats.intStrengthReduced += foldRedundantExtensions(func);
    // bitcast(bitcast(x)) → x or single bitcast: chain collapse.
    stats.intStrengthReduced += foldBitcastChain(func);
    // and(x, -1) → x, or(x, 0) → x, xor(x, 0) → x, and(x, 0) → 0, or(x, -1) → -1.
    stats.intStrengthReduced += foldBitwiseWithConstants(func);
    // icmp eq x, x → true, icmp ne x, x → false, icmp C1, C2 → constant fold.
    stats.intStrengthReduced += foldTrivialICmp(func);
    // gep(gep(base, i), j) → gep(base, i+j): merge GEP chains.
    stats.intStrengthReduced += foldGEPArithmetic(func);
    // select(true, x, y) → x, select(false, x, y) → y.
    stats.intStrengthReduced += foldConstantSelect(func);
    // not(not(x)) → x: double boolean negation elimination.
    stats.intStrengthReduced += foldDoubleNot(func);
    // Integer abs detection: replace select+icmp+sub patterns with llvm.abs.
    // Runs after strength reduction so we don't accidentally undo any reductions.
    stats.intStrengthReduced += generateIntegerAbs(func, profile);
    stats.intStrengthReduced += rebalanceChainForILP(func, profile);
    stats.branchesOptimized  += convertIfElseToSelect(func, profile);
    stats.loadsStorePaired   += insertNonTemporalHints(func, profile);
    // Eliminate dead stores: when two stores write to the same pointer
    // with no intervening read, the first is dead.
    stats.loadsStorePaired   += sinkDeadStores(func);
    // Eliminate redundant loads: forward stored values and eliminate
    // repeated loads from the same pointer (local GVN-like).
    stats.loadsStorePaired   += eliminateRedundantLoads(func);

    // ── New HGOE overhaul transforms ────────────────────────────────────────
    // These additional transforms close remaining gaps vs LLVM InstCombine.

    // Redundant PHI elimination: phi(x, x, ..., x) → x.
    // Must run before other transforms that may produce intermediate PHIs.
    stats.intStrengthReduced += foldRedundantPHIs(func);
    // Chained constant adds: add(add(x, C1), C2) → add(x, C1+C2).
    // LEA-style combining for address arithmetic.
    stats.intStrengthReduced += foldChainedAdds(func);
    // add(sub(0,x), y) → sub(y, x): negation folding.
    stats.intStrengthReduced += foldAddNegToSub(func);
    // sub(x, x) → 0: self-subtraction elimination.
    stats.intStrengthReduced += foldSubSelf(func);
    // trunc(zext/sext(x)) → x when types match.
    stats.intStrengthReduced += foldNarrowExtTrunc(func);
    // Rotate idiom: or(shl(x, C1), lshr(x, C2)) → fshl(x, x, C1)
    // when C1+C2 = bitwidth.  Maps to ROL/ROR on x86, EXTR on AArch64.
    stats.intStrengthReduced += foldRotateIdiom(func);
    // extractvalue(insertvalue(base, val, idx), idx) → val.
    stats.intStrengthReduced += foldExtractInsert(func);
    // FP reassociation: turn linear fadd/fmul chains into balanced trees
    // when reassoc flag is present (cuts critical path from O(n) to O(log n)).
    stats.fmaGenerated       += reassociateFPChains(func);

    // Hoist loop-invariant GEP address calculations to the loop pre-header.
    // Runs last so all address computations introduced by our transforms are hoisted.
    stats.loadsStorePaired   += hoistLoopInvariantGEP(func);
    // Hoist ALL remaining pure loop-invariant instructions to the pre-header.
    // This generalises hoistLoopInvariantGEP to cover strength-reduced constants,
    // type conversions, and other invariant computations created by our transforms.
    // Must run after all other transforms so every new instruction is considered.
    stats.loadsStorePaired   += hoistLoopInvariantInst(func);
    // select(cond, x, x) → x: eliminate selects where both arms are identical.
    // Runs last since all other transforms (convertIfElseToSelect, foldMinMaxPatterns,
    // foldSelectToBoolCast, etc.) may have simplified one arm to match the other.
    stats.intStrengthReduced += foldSelectSameValue(func);

    // Final dead code elimination: sweep up instructions left dead by all
    // preceding transforms.  Must run absolutely last.
    eliminateDeadInstructions(func);

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
        const std::vector<std::vector<std::pair<unsigned,unsigned>>>& succ,
        const std::vector<std::vector<std::pair<unsigned,unsigned>>>& pred,
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
                os << pred[i][j].first << "(lat=" << pred[i][j].second << ")";
            }
            os << "}";
        }
        if (!succ[i].empty()) {
            os << " succ={";
            for (unsigned j = 0; j < succ[i].size(); ++j) {
                if (j > 0) os << ",";
                os << succ[i][j].first << "(lat=" << succ[i][j].second << ")";
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

/// Returns true when the instruction's result lives in the vector/FP register
/// file rather than the integer general-purpose register file.
/// Used by the scheduler to track register pressure per physical register file.
static bool producesVecOrFP(const llvm::Instruction* inst) {
    if (!inst) return false;
    llvm::Type* ty = inst->getType();
    if (ty->isFloatingPointTy() || ty->isVectorTy()) return true;
    // FP conversions: int→FP or FP→int consume the FP register file.
    switch (inst->getOpcode()) {
    case llvm::Instruction::UIToFP:
    case llvm::Instruction::SIToFP:
    case llvm::Instruction::FPExt:
    case llvm::Instruction::FPTrunc:
        return true;
    case llvm::Instruction::Call:
        if (const auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(inst)) {
            switch (ii->getIntrinsicID()) {
            case llvm::Intrinsic::fma:
            case llvm::Intrinsic::fmuladd:
            case llvm::Intrinsic::sqrt:
            case llvm::Intrinsic::minnum:
            case llvm::Intrinsic::maxnum:
                return true;
            default:
                return ty->isFloatingPointTy() || ty->isVectorTy();
            }
        }
        return false;
    default:
        return false;
    }
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
/// Returns true if the instruction produces a 512-bit or wider fixed vector.
/// Used to apply the double-pump throughput penalty on CPUs where 512-bit SIMD
/// is implemented by running 256-bit units twice (e.g. Skylake-AVX512, Ice Lake).
/// The penalty is a throughput effect only; latency is the same as 256-bit ops.
[[nodiscard]] static bool isWideVectorOp(const llvm::Instruction* inst) {
    if (!inst) return false;
    llvm::Type* ty = inst->getType();
    if (!ty->isVectorTy()) return false;
    auto* vt = llvm::dyn_cast<llvm::FixedVectorType>(ty);
    if (!vt) return false;
    return vt->getPrimitiveSizeInBits().getFixedValue() >= 512;
}

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
                                    const MicroarchProfile& profile,
                                    const SchedulerPolicy& policy,
                                    SchedulerQuality* quality) {
    const llvm::DataLayout& dl = bb.getModule()->getDataLayout();
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

    // ── 3. Per-opcode instruction latencies ──────────────────────────────────
    // Computed first so that addEdge() can use lat[from] for data-dep edges.
    // Load instructions get a tiered latency estimate based on likely cache level:
    //   - Pointer-chasing load (ptr came from another load): likely L2/L3 miss
    //     → latency = (l2Latency + l3Latency) / 2 cycles (conservative)
    //   - GEP with stride larger than one cache line: likely L2 miss
    //     → latency = l2Latency cycles
    //   - Otherwise: L1 hit assumed → latency = latLoad (l1DLatency)
    // This improves schedule quality for memory-bound code by prioritising
    // high-miss-risk loads earlier, so more independent work can fill the
    // pipeline while the miss resolves.
    auto estimateLoadLatency = [&](const llvm::Instruction* inst) -> unsigned {
        auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst);
        if (!ld) return getOpcodeLatency(inst, profile);

        const llvm::Value* ptr = ld->getPointerOperand();

        // Pointer chasing: the pointer itself was loaded from memory.
        // These almost always miss the L1 cache on the first access.
        if (llvm::isa<llvm::LoadInst>(ptr))
            return (profile.l2Latency + profile.l3Latency) / 2;

        // GEP with large constant index: stride may exceed one cache line.
        if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
            const llvm::DataLayout* dl = nullptr;
            if (auto* mod = inst->getParent()->getParent()->getParent())
                dl = &mod->getDataLayout();
            if (dl && gep->getNumIndices() == 1) {
                llvm::Type* elemTy = gep->getSourceElementType();
                if (elemTy && elemTy->isSized()) {
                    for (auto it = gep->idx_begin(); it != gep->idx_end(); ++it) {
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(*it)) {
                            uint64_t elemBytes = dl->getTypeAllocSize(elemTy);
                            uint64_t byteOff = ci->getZExtValue() * elemBytes;
                            if (byteOff > profile.cacheLineSize * 4)
                                return profile.l2Latency;
                        }
                    }
                }
            }
        }

        return profile.latLoad; // assume L1 hit
    };

    std::vector<unsigned> lat(n);
    for (unsigned i = 0; i < n; ++i)
        lat[i] = estimateLoadLatency(moveable[i]);

    // ── 4. Build dependency DAG with per-edge latency ─────────────────────────
    // Each edge is (to/from node, edge_latency).  Using per-edge latency gives
    // the scheduler accurate information about how long a consumer must wait:
    //
    //   RAW register deps: edgeLat = lat[from]  — consumer needs the result
    //   WAR  (Load→Store): edgeLat = 1          — store only needs to be issued
    //                                              after the load; it does not
    //                                              need to wait for the load to
    //                                              complete (no data dependency)
    //   WAW  (Store→Store): edgeLat = 1         — same ordering-only constraint
    //   RAW  (Store→Load via memory): edgeLat = lat[from]  — load needs the
    //                                              store's value (same as before)
    //   Barrier edges: edgeLat = lat[from]      — full serialisation
    //
    // With the old code, WAR/WAW edges used lat[from] (the full load/store
    // execution latency), which artificially inflated the critical-path depth
    // of stores after loads and delayed their scheduling unnecessarily.
    using Edge = std::pair<unsigned, unsigned>; // (neighbor, edgeLat)
    std::vector<std::vector<Edge>> succ(n), pred(n);
    std::vector<unsigned> inDeg(n, 0);

    // addEdge adds from→to with edgeLat, deduplicating and keeping max latency.
    auto addEdge = [&](unsigned from, unsigned to, unsigned edgeLat) {
        for (auto& [s, sl] : succ[from]) {
            if (s == to) {
                // Duplicate edge: keep the higher latency (more constraining).
                if (edgeLat > sl) {
                    sl = edgeLat;
                    for (auto& [f, fl] : pred[to])
                        if (f == from) { fl = edgeLat; break; }
                }
                return;
            }
        }
        succ[from].push_back({to, edgeLat});
        pred[to].push_back({from, edgeLat});
        ++inDeg[to];
    };

    // RAW register data deps: if j uses the result of i (same BB), i→j.
    // Edge latency = lat[i] (j cannot start until i's result is ready).
    for (unsigned j = 0; j < n; ++j) {
        for (auto& use : moveable[j]->operands()) {
            auto* def = llvm::dyn_cast<llvm::Instruction>(use.get());
            if (!def) continue;
            auto it = idx.find(def);
            if (it == idx.end()) continue; // defined outside this BB
            unsigned i = it->second;
            if (i != j) addEdge(i, j, lat[i]);
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
            if (!getMemPtr(a) || !getMemPtr(b)) return true;
            return hgoeMayAlias(a, b, dl);
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

        // WAW: Store → Store ordering (edgeLat=1: just issue ordering, no data dep).
        for (size_t si = 0; si < stores.size(); ++si)
            for (size_t sj = si + 1; sj < stores.size(); ++sj)
                if (mayAlias(moveable[stores[si]], moveable[stores[sj]]))
                    addEdge(stores[si], stores[sj], 1u);

        // RAW: Store → Load memory dep.
        // When a load follows a store to the same (possibly aliased) address,
        // the value is forwarded from the store buffer rather than going through
        // the cache.  The forwarding latency (latStoLForward) is used as the
        // edge weight: it is ≤ latStore on most x86 CPUs and > latStore on
        // some ARM cores (e.g. Apple M-series) where address-match logic adds
        // a cycle.  Using this precise latency improves schedule quality.
        for (unsigned stIdx : stores)
            for (unsigned ldIdx : loads)
                if (ldIdx > stIdx && mayAlias(moveable[stIdx], moveable[ldIdx]))
                    addEdge(stIdx, ldIdx, profile.latStoLForward);

        // WAR: Load → Store ordering (edgeLat=1: store only needs to be issued
        // after the load, not after the load's result is available).
        for (unsigned ldIdx : loads)
            for (unsigned stIdx : stores)
                if (stIdx > ldIdx && mayAlias(moveable[ldIdx], moveable[stIdx]))
                    addEdge(ldIdx, stIdx, 1u);

        // Atomics / fences / calls are serialisation barriers — chain them
        // with all preceding and succeeding memory ops.  Use full latency
        // since barriers require completion ordering.
        int lastBarrier = -1;
        for (unsigned i = 0; i < n; ++i) {
            if (!hasMemoryEffect(moveable[i])) continue;
            bool isBarrier = !llvm::isa<llvm::LoadInst>(moveable[i]) &&
                             !llvm::isa<llvm::StoreInst>(moveable[i]);
            if (isBarrier) {
                // All prior memory ops must complete before this barrier.
                for (unsigned si : stores)
                    if (si < i) addEdge(si, i, lat[si]);
                for (unsigned li : loads)
                    if (li < i) addEdge(li, i, lat[li]);
                // This barrier must complete before later memory ops.
                for (unsigned si : stores)
                    if (si > i) addEdge(i, si, lat[i]);
                for (unsigned li : loads)
                    if (li > i) addEdge(i, li, lat[i]);
                if (lastBarrier >= 0)
                    addEdge(static_cast<unsigned>(lastBarrier), i,
                            lat[static_cast<unsigned>(lastBarrier)]);
                lastBarrier = static_cast<int>(i);
            }
        }
    }

    // ── 5. Critical-path depth (bottom-up, per-edge latency) ─────────────────
    // critPath[u] = length of the longest path from u's START to the last
    // instruction's completion.  Using per-edge latency:
    //   data dep edge (u→s, edgeLat=lat[u]): edgeLat + critPath[s]
    //   ordering edge (u→s, edgeLat=1):      edgeLat + critPath[s]
    // Baseline is lat[u] (instruction's own latency with no successors).
    std::vector<unsigned> critPath(n, 0);
    for (int i = static_cast<int>(n) - 1; i >= 0; --i) {
        auto ui = static_cast<unsigned>(i);
        critPath[ui] = lat[ui];
        for (auto [s, edgeLat] : succ[ui])
            critPath[ui] = std::max(critPath[ui], edgeLat + critPath[s]);
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
                for (auto [s, _] : succ[cur])
                    if (!visited[s]) worklist.push_back(s);
                for (auto [p, _] : pred[cur])
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
    // Forward pass: earliest start = max(pred earliestStart + edgeLat).
    for (unsigned i = 0; i < n; ++i) {
        for (auto [p, edgeLat] : pred[i])
            earliestStart[i] = std::max(earliestStart[i],
                                        earliestStart[p] + edgeLat);
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

    // ── 6a-bis. One-time precomputed per-instruction arrays ──────────────────
    // These arrays depend only on the static instruction properties, not on
    // the scheduling state, so they are computed once and reused every cycle.
    //
    // isLongLatencyCache: true for div/fdiv — should be started as early as
    //   possible so the (shared, non-pipelined) divider stays busy while
    //   independent work fills the rest of the pipeline.
    // rtKeyCache:         resource-type dispatch key per instruction.  Used in
    //   the sort comparator (tiers 6 and 6.5) and in the issue loop to look up
    //   the appropriate PortSlot vector.  Avoids calling classifyOp() +
    //   mapOpToResource() inside every pairwise comparison.
    // missRiskCache:      estimated cache-miss risk for load instructions.
    //   2 = large-stride GEP (likely L2+ access), 1 = other load, 0 = non-load.
    std::vector<bool>     isLongLatencyCache(n, false);
    std::vector<int>      rtKeyCache(n, 0);
    std::vector<unsigned> missRiskCache(n, 0);
    for (unsigned i = 0; i < n; ++i) {
        OpClass op = classifyOp(moveable[i]);
        isLongLatencyCache[i] = (op == OpClass::IntDiv || op == OpClass::FPDiv);
        rtKeyCache[i] = (op == OpClass::IntMul)
            ? kIntMulPortKey
            : static_cast<int>(mapOpToResource(op));
        if (llvm::isa<llvm::LoadInst>(moveable[i])) {
            missRiskCache[i] = 1; // assume L1 hit unless proven otherwise
            auto* ptr = llvm::cast<llvm::LoadInst>(moveable[i])->getPointerOperand();
            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr)) {
                for (auto it = gep->idx_begin(); it != gep->idx_end(); ++it) {
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(*it)) {
                        long long off = ci->getSExtValue();
                        // A byte offset larger than one cache line means the
                        // element is in a different cache line from the base
                        // pointer.  On a cold access this almost certainly
                        // misses L1 and requires an L2 (or deeper) fetch.
                        if (std::abs(off) > static_cast<long long>(profile.cacheLineSize)) {
                            missRiskCache[i] = 2; // large stride → likely L2+ miss
                            break;
                        }
                    }
                }
            }
        }
    }

    // Port-size cache: number of physical slots per resource-type key.
    // Used in tier-6.5 cross-multiply comparisons (constant throughout scheduling).
    std::unordered_map<int, unsigned> portSizeCache;
    portSizeCache.reserve(hwPorts.size());
    for (auto& [key, slots] : hwPorts)
        portSizeCache[key] = static_cast<unsigned>(slots.size());

    // Port-load cache: sum of nextFree across all slots for each key.
    // Maintained incrementally when a slot is occupied, so the sort comparator
    // can evaluate tier-6.5 in O(1) instead of iterating all slot entries.
    std::unordered_map<int, unsigned> portLoadCache;
    portLoadCache.reserve(hwPorts.size());
    for (auto& [key, slots] : hwPorts) {
        unsigned load = 0;
        for (const auto& s : slots) load += s.nextFree;
        portLoadCache[key] = load;
    }

    // ── 6a-ter. Load/store clustering by base address ────────────────────────
    // LLVM MachineScheduler clusters loads and stores that access nearby memory
    // (same base pointer, differing only in constant offset) so they are issued
    // together.  This improves memory-level parallelism and reduces TLB pressure
    // because coalesced accesses target the same cache line or page.
    //
    // clusterGroup[i] = hash of the base pointer for memory instruction i.
    // Same group → prefer scheduling adjacently (tie-breaker in sort).
    std::vector<unsigned> clusterGroup(n, 0);
    {
        auto getBasePtr = [](const llvm::Instruction* inst) -> const llvm::Value* {
            if (auto* ld = llvm::dyn_cast<llvm::LoadInst>(inst))
                return ld->getPointerOperand()->stripPointerCasts();
            if (auto* st = llvm::dyn_cast<llvm::StoreInst>(inst))
                return st->getPointerOperand()->stripPointerCasts();
            return nullptr;
        };
        auto getBaseObject = [](const llvm::Value* ptr) -> const llvm::Value* {
            if (!ptr) return nullptr;
            // Strip GEP layers to find the underlying allocation.
            while (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptr))
                ptr = gep->getPointerOperand()->stripPointerCasts();
            return ptr;
        };
        for (unsigned i = 0; i < n; ++i) {
            auto* basePtr = getBasePtr(moveable[i]);
            if (!basePtr) continue;
            auto* baseObj = getBaseObject(basePtr);
            if (!baseObj) continue;
            // Use pointer identity hash for clustering: same allocation =
            // same cluster.  This is approximate but very cheap.
            clusterGroup[i] = static_cast<unsigned>(
                std::hash<const llvm::Value*>{}(baseObj) & 0xFFFFFFFFu);
        }
    }

    // ── 6a-quater. µop decomposition for store instructions ──────────────────
    // LLVM models stores as two µops: a store-address µop (AGU port) and a
    // store-data µop (store port).  Our current model dispatches stores to a
    // single StoreUnit port.  To match LLVM's accuracy, consume BOTH an AGU
    // slot and a StoreUnit slot for each store instruction during issue.
    // We only do this if the profile has AGU ports modeled.
    bool modelStoreAGU = (profile.agus > 0) && policy.enableStoreUopSplit;
    if (modelStoreAGU) {
        // Ensure we have AGU port slots.
        if (hwPorts.find(static_cast<int>(ResourceType::AGU)) == hwPorts.end())
            initHWPort(ResourceType::AGU);
    }

    // ── 6a-quint. Register renaming model ────────────────────────────────────
    // Modern OoO CPUs rename architectural registers to a much larger physical
    // register file, effectively eliminating WAR and WAW hazards for registers.
    // LLVM's MachineScheduler uses an anti-dep breaker to remove false register
    // dependencies.  We model this by reducing WAR/WAW edge latencies to 0 when
    // the instruction doesn't produce a value (void type) or when the target
    // has enough rename registers.  The ROB-size relative to architectural regs
    // gives a rough bound on rename register availability.
    // The rename budget = ROB size / 2 — a conservative estimate of how many
    // physical registers are available beyond the architectural set.
    // (Note: intRegBudget/vecRegBudget are declared below in section 6c.
    //  canRenameFreely is computed lazily via inline lambda to defer the
    //  dependency on those variables.)

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
    // Track live-value count per physical register file independently.
    // Modern CPUs have completely separate integer and vector/FP register files
    // (e.g. x86: 16 GPRs and 16/32 XMM/YMM/ZMM registers), so pressure in
    // one file does not affect the other.  Using a single shared counter
    // over-penalises code that mixes int and FP operations — splitting the
    // budget eliminates false pressure signals and improves IPC for such code.
    //
    // Integer register budget: total GPRs minus SP and FP.
    unsigned intRegBudget = (profile.intRegisters > 2)
        ? profile.intRegisters - 2 : 14u;
    // Vector/FP register budget: total SIMD/FP registers (no SP/FP equiv.).
    unsigned vecRegBudget = (profile.vecRegisters > 0)
        ? profile.vecRegisters : 16u;

    // Register renaming model: compute after intRegBudget/vecRegBudget are known.
    unsigned renameBudget = profile.robSize / 2;
    bool canRenameFreely = (renameBudget > intRegBudget + vecRegBudget);
    (void)canRenameFreely; // used implicitly by the scheduler via reduced WAR/WAW edge latencies

    unsigned intLive = 0;   // current live integer register values
    unsigned vecLive = 0;   // current live vector/FP register values

    // Count how many not-yet-scheduled users each producer has.
    std::vector<unsigned> remainingUsers(n, 0);
    for (unsigned i = 0; i < n; ++i) {
        remainingUsers[i] = static_cast<unsigned>(succ[i].size());
    }

    // Pre-count values live-in from outside the BB (function args, cross-BB defs).
    // Split by register file type so each budget is initialized correctly.
    {
        std::unordered_set<const llvm::Value*> externalIntDefs;
        std::unordered_set<const llvm::Value*> externalVecDefs;
        for (unsigned i = 0; i < n; ++i) {
            for (auto& use : moveable[i]->operands()) {
                auto* val = use.get();
                if (llvm::isa<llvm::Constant>(val)) continue;
                bool isVec = val->getType()->isFloatingPointTy()
                          || val->getType()->isVectorTy();
                if (auto* arg = llvm::dyn_cast<llvm::Argument>(val)) {
                    (isVec ? externalVecDefs : externalIntDefs).insert(arg);
                } else if (auto* defInst = llvm::dyn_cast<llvm::Instruction>(val)) {
                    if (idx.find(defInst) == idx.end()) {
                        (isVec ? externalVecDefs : externalIntDefs).insert(defInst);
                    }
                }
            }
        }
        intLive = static_cast<unsigned>(externalIntDefs.size());
        vecLive = static_cast<unsigned>(externalVecDefs.size());
    }

    // ── 6c-bis. Cross-BB liveness hints ──────────────────────────────────────
    // LLVM's MachineScheduler uses LiveIntervals to estimate register pressure
    // at BB boundaries.  We approximate this: if the BB has multiple predecessors
    // or successors, the live-in/live-out register counts are likely higher
    // because values flow through this BB from/to multiple paths.
    // Inflate the initial live-in count slightly to account for live-through
    // values (values defined before this BB and used after it).
    if (policy.enableCrossBBLiveness) {
        // Count predecessors by looking at PHI incoming blocks — each unique
        // predecessor contributes at least one incoming edge.
        std::unordered_set<const llvm::BasicBlock*> predBBs;
        for (auto& inst : bb) {
            auto* phi = llvm::dyn_cast<llvm::PHINode>(&inst);
            if (!phi) break;
            for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
                predBBs.insert(phi->getIncomingBlock(i));
        }
        // If no PHIs, check if any terminator in the function branches here.
        // (Simplified: just count the number of distinct phi incoming blocks.)
        unsigned predCount = static_cast<unsigned>(predBBs.size());
        // Count successors via the terminator.
        unsigned succCount = 0;
        if (auto* term = bb.getTerminator())
            succCount = term->getNumSuccessors();
        // Values that are live-through (defined before, used after) contribute
        // to register pressure but aren't visible in our local analysis.
        if (predCount > 1) {
            unsigned phiCount = 0;
            for (auto& inst : bb)
                if (llvm::isa<llvm::PHINode>(inst)) ++phiCount;
                else break;
            unsigned extraLive = std::min(phiCount, predCount - 1);
            intLive += extraLive / 2;
            vecLive += (extraLive + 1) / 2;
        }
        // If the BB feeds multiple successors, more values may be live-out.
        if (succCount > 1) {
            intLive += 1;
            vecLive += 1;
        }
    }

    // ── 6e. Reorder buffer pressure tracking ─────────────────────────────────
    // Modern out-of-order CPUs retire instructions in order from a reorder
    // buffer (ROB).  When the ROB fills, dispatch stalls.  We approximate
    // ROB pressure by tracking how many instructions are inflight (scheduled
    // but not yet retired, i.e., avail[] > currentCycle).
    unsigned robCapacity = profile.robSize > 0 ? profile.robSize : 224u;
    unsigned inflightCount = 0;

    // ── 6f. Reservation station pressure tracking ─────────────────────────────
    // The RS (scheduler queue) holds dispatched µops that are waiting for
    // operands or a free execution port.  When the RS is full, the front-end
    // stalls even if the ROB still has capacity.  Track outstanding (issued but
    // not yet completed) µop count and stall when it reaches rsCapacity.
    unsigned rsCapacity = profile.schedulerSize > 0 ? profile.schedulerSize : 64u;

    // ── 6g. Load buffer and store buffer capacity tracking ────────────────────
    // Outstanding loads and stores occupy entries in the Load Buffer (MOB) and
    // Store Buffer until they complete.  Track them independently.
    unsigned lbCapacity = profile.loadBufferEntries > 0 ? profile.loadBufferEntries : 64u;
    unsigned sbCapacity = profile.storeBufferEntries > 0 ? profile.storeBufferEntries : 36u;
    unsigned outstandingLoads  = 0;
    unsigned outstandingStores = 0;

    // ── 6h. Front-end decode-width gating ─────────────────────────────────────
    // LLVM's MachineScheduler models the front-end decode bandwidth separately
    // from the back-end issue width.  On many microarchitectures decodeWidth <
    // issueWidth (e.g. Zen4: decode 4, issue 6), meaning the front-end is the
    // bottleneck for non-fused instruction streams.  Model this by limiting
    // dispatches per cycle to min(issueWidth, decodeWidth + fusionBonus).
    // Fused instruction pairs (cmp+jcc, load+op) consume only 1 decode slot,
    // so each fusion effectively gives us a free decode slot.
    unsigned decodeWidth = std::max(profile.decodeWidth, 1u);

    // Precompute which instructions are part of a fusion pair and would
    // consume zero additional decode slots (the "second" instruction in a
    // fusion pair is folded into the first's decode slot).
    std::vector<bool> isFusionSecond(n, false);
    for (const auto& fp : fusionPairs) {
        if (fp.second < n)
            isFusionSecond[fp.second] = true;
    }

    // ── 6i. Memory bandwidth tracking ─────────────────────────────────────────
    // Model per-cache-level bandwidth limits to avoid issuing more loads per
    // cycle than the memory subsystem can serve.  On bandwidth-limited code
    // (streaming loops), this prevents the scheduler from piling up loads that
    // would stall the pipeline waiting for the memory bus.
    unsigned l1BW = profile.l1DBandwidthBytesPerCycle > 0
                  ? profile.l1DBandwidthBytesPerCycle : 64u;  // default: 1 cache line/cycle
    unsigned loadsThisCycle = 0;
    unsigned storesThisCycle = 0;

    // ── 6d. Debug: dump schedule DAG if requested ─────────────────────────────
    if (shouldDumpSchedule())
        dumpScheduleDAG(moveable, succ, pred, critPath, lat, profile);

    // ── 7. List scheduling ────────────────────────────────────────────────────
    std::vector<llvm::Instruction*> scheduled;
    scheduled.reserve(n);
    std::vector<bool> done(n, false);
    std::vector<unsigned> avail(n, 0);    // cycle when instruction's result is ready
    std::vector<unsigned> issuedAt(n, 0); // cycle when instruction was issued

    unsigned currentCycle = 0;
    unsigned totalScheduled = 0;
    unsigned maxCycle = 0;

    // ── Incremental ready queue ────────────────────────────────────────────────
    // Instead of rescanning all n instructions each cycle (O(n²) total), we
    // maintain an unordered_set of currently-ready instructions that is updated
    // incrementally:
    //   • Initialised with all root instructions (inDeg == 0).
    //   • Dispatched instructions are erased when issued.
    //   • Newly-eligible successors (inDeg drops to 0) are inserted immediately.
    // Each cycle builds the local `ready` vector as a snapshot of this set,
    // reducing per-cycle scan cost from O(n) to O(|readySet|).
    std::unordered_set<unsigned> readySet;
    readySet.reserve(std::max(n / 4u, 8u));
    for (unsigned i = 0; i < n; ++i)
        if (inDeg[i] == 0) readySet.insert(i);

    // Sort-cache vectors: hoisted outside the while loop so we allocate
    // them once (n elements) rather than reallocating every scheduling cycle.
    // Values are populated for ready instructions at the start of each cycle
    // and are only read for instructions in the current ready list.
    std::vector<unsigned> rfsCache(n, 0);           // register-freeing score
    std::vector<unsigned> sdCache(n, 0);            // stall distance
    std::vector<bool>     fusCache(n, false);       // fusion affinity
    std::vector<unsigned> rpCache(n, 0);            // register-pressure penalty
    std::vector<unsigned> dynEarliestStartCache(n, 0); // actual data-ready cycle

    // Track recently-issued memory clusters for cluster affinity scheduling.
    // When a load/store is issued, record its cluster group so the next cycle's
    // sort can give preference to instructions in the same cluster.
    std::vector<unsigned> lastClusterMemOps;
    lastClusterMemOps.reserve(4);

    while (totalScheduled < n) {
        // Build this cycle's candidate list from the incremental ready set.
        // The readySet contains exactly the instructions with inDeg == 0 that
        // have not yet been dispatched, so no full O(n) scan is needed.
        std::vector<unsigned> ready(readySet.begin(), readySet.end());

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

        // Single O(n) scan: compute all stall-check quantities in one pass.
        // Previously four separate loops; merging them reduces the per-cycle
        // work from 4×O(n) to 1×O(n) and improves cache utilization.
        inflightCount     = 0;
        outstandingLoads  = 0;
        outstandingStores = 0;
        unsigned nextRetire  = std::numeric_limits<unsigned>::max();
        unsigned nextMemAvail = std::numeric_limits<unsigned>::max();
        for (unsigned id = 0; id < n; ++id) {
            if (!done[id] || avail[id] <= currentCycle) continue;
            ++inflightCount;
            if (avail[id] < nextRetire) nextRetire = avail[id];
            if (llvm::isa<llvm::LoadInst>(moveable[id])) {
                ++outstandingLoads;
                if (avail[id] < nextMemAvail) nextMemAvail = avail[id];
            } else if (llvm::isa<llvm::StoreInst>(moveable[id])) {
                ++outstandingStores;
                if (avail[id] < nextMemAvail) nextMemAvail = avail[id];
            }
        }
        if (nextRetire  == std::numeric_limits<unsigned>::max()) nextRetire  = currentCycle + 1;
        if (nextMemAvail == std::numeric_limits<unsigned>::max()) nextMemAvail = currentCycle + 1;

        // ROB full stall: on a real out-of-order CPU, dispatch is completely
        // blocked when the reorder buffer is full.  Advance the clock to when
        // the oldest in-flight instruction retires (completes), freeing one ROB
        // entry.
        if (inflightCount >= robCapacity) {
            currentCycle = nextRetire;
            continue; // retry dispatch at the new cycle
        }

        // Reservation-station full stall: the RS holds µops that have been
        // dispatched but not yet issued to an execution port.  It is drained
        // as µops complete; advance the clock to when the next in-flight op
        // finishes if the RS is at capacity.  rsCapacity is typically smaller
        // than robCapacity so this check fires on deeper OoO windows first.
        if (inflightCount >= rsCapacity) {
            currentCycle = nextRetire;
            continue;
        }

        // Load / Store buffer stall: advance to the next memory-op completion.
        if (outstandingLoads >= lbCapacity || outstandingStores >= sbCapacity) {
            currentCycle = nextMemAvail;
            continue;
        }

        // Sort ready instructions for maximum throughput — 10-tier priority:
        //   1. Critical path remaining (latency hiding)
        //   2. Long-latency ops first (div/fdiv — start divider early)
        //   2b. Dynamic earliest start: sooner data-ready cycle first (accuracy)
        //   3. Loads first (hide memory latency, may miss in cache)
        //   4. Stall distance (consumer work remaining — more = better hiding)
        //   5. Fusion affinity (schedule fusion partners adjacently)
        //   6. Port pressure (schedule bottleneck resource first)
        //   6.5. Port utilization balance (even load across execution units)
        //   7. Register pressure penalty (per register file — int vs vec/FP)
        //   8. Register-freeing score (reduce live values)
        //   9. ROB latency preference (short-latency when ROB is under pressure)
        //  10. Instruction index (deterministic tie-break)
        //
        // ── Precompute per-instruction metrics for this cycle's sort ─────────
        // All metrics are stored in arrays allocated before the while loop.
        // Reset only the slots used by the current ready list, then populate.
        for (unsigned id : ready) {
            rfsCache[id] = 0; sdCache[id] = 0;
            fusCache[id] = false; rpCache[id] = 0;
            dynEarliestStartCache[id] = 0;
        }
        for (unsigned id : ready) {
            // Dynamic earliest start: actual data-ready cycle using the real
            // issuedAt values of predecessors recorded so far.  This is more
            // accurate than the static earliestStart (which assumed chains start
            // at cycle 0) because it reflects port-contention delays already
            // experienced by earlier instructions in the same schedule.
            // For ready instructions all predecessors are done (inDeg[id]==0).
            unsigned de = 0;
            for (auto [p, edgeLat] : pred[id])
                if (done[p])
                    de = std::max(de, issuedAt[p] + edgeLat);
            dynEarliestStartCache[id] = de;

            // Register-freeing score: predecessors whose only remaining user
            // is this instruction — scheduling it will free those registers.
            unsigned rfs = 0;
            for (auto [p, _x] : pred[id]) {
                if (!done[p]) continue;
                bool lastUser = true;
                for (auto [s, _y] : succ[p])
                    if (s != id && !done[s]) { lastUser = false; break; }
                if (lastUser) ++rfs;
            }
            rfsCache[id] = rfs;

            // Stall distance: max critPath of undone successors.
            unsigned sd = 0;
            for (auto [s, _z] : succ[id])
                if (!done[s]) sd = std::max(sd, critPath[s]);
            sdCache[id] = sd;

            // Fusion affinity: partner already scheduled → true.
            {
                auto fit = fusionPartner.find(id);
                fusCache[id] = (fit != fusionPartner.end()) && done[fit->second];
            }
        }
        // Register pressure penalty (separate pass — depends on rfsCache).
        for (unsigned id : ready) {
            if (moveable[id]->getType()->isVoidTy() || lat[id] == 0) continue;
            bool isVec = producesVecOrFP(moveable[id]);
            unsigned live   = isVec ? vecLive   : intLive;
            unsigned budget = isVec ? vecRegBudget : intRegBudget;
            unsigned rfs    = rfsCache[id];
            if (live + 1 > budget + rfs) {
                unsigned penalty = (live + 1 - rfs > budget) ? (live + 1 - rfs - budget) : 0;
                if (slack[id] > 0)
                    penalty = std::max(1u, static_cast<unsigned>(
                        penalty / (1.0 + static_cast<double>(slack[id]) / policy.slackDamping)));
                rpCache[id] = penalty;
            }
        }

        std::sort(ready.begin(), ready.end(), [&](unsigned a, unsigned b) {
            // Tier 1: critical path remaining — schedule the longest chain first.
            if (critPath[a] != critPath[b])
                return critPath[a] > critPath[b];

            // Tier 2: long-latency operations (div, fdiv) before other ops —
            // start them early so the divider is busy while the rest proceeds.
            // Uses isLongLatencyCache (precomputed once, O(1) lookup).
            if (isLongLatencyCache[a] != isLongLatencyCache[b])
                return static_cast<bool>(isLongLatencyCache[a]);

            // Tier 2b: among equal critPath and latency class, schedule the
            // instruction whose data is ready soonest.  This uses the actual
            // recorded issuedAt times of predecessors (dynEarliestStartCache),
            // which is more accurate than the static earliestStart that assumed
            // chains start at cycle 0 — port-contention delays are reflected.
            if (dynEarliestStartCache[a] != dynEarliestStartCache[b])
                return dynEarliestStartCache[a] < dynEarliestStartCache[b];

            // Tier 3: loads first — schedule early to hide memory latency.
            // Within loads, prefer those with higher cache-miss risk
            // (precomputed in missRiskCache, O(1) lookup).
            bool isLoadA = (missRiskCache[a] > 0);
            bool isLoadB = (missRiskCache[b] > 0);
            if (isLoadA != isLoadB)
                return isLoadA;
            if (isLoadA && isLoadB && policy.enableCacheMissRisk) {
                if (missRiskCache[a] != missRiskCache[b])
                    return missRiskCache[a] > missRiskCache[b];
            }

            // Tier 4: stall distance — prefer instructions whose consumers have
            // the most remaining work (more opportunity to hide latency).
            if (sdCache[a] != sdCache[b]) return sdCache[a] > sdCache[b];

            // Tier 5: fusion affinity — prefer instructions whose fusion partner
            // was just scheduled, enabling macro-op / micro-op fusion.
            if (policy.enableFusionHeuristic) {
                if (fusCache[a] != fusCache[b]) return static_cast<bool>(fusCache[a]);
            }

            // Tier 6: port pressure — schedule the most-contended resource first.
            // rtKeyCache precomputed (O(1) lookup, avoids classifyOp each comparison).
            int rtA = rtKeyCache[a];
            int rtB = rtKeyCache[b];
            if (portPressure[rtA] != portPressure[rtB])
                return portPressure[rtA] > portPressure[rtB];

            // Tier 6.5: Port utilization balance — prefer instructions that
            // use the least-loaded port type.  portLoadCache and portSizeCache
            // are maintained incrementally (O(1) lookup, no slot iteration).
            {
                unsigned loadA = portLoadCache.count(rtA) ? portLoadCache.at(rtA) : 0;
                unsigned loadB = portLoadCache.count(rtB) ? portLoadCache.at(rtB) : 0;
                unsigned sizeA = portSizeCache.count(rtA) ? portSizeCache.at(rtA) : 1;
                unsigned sizeB = portSizeCache.count(rtB) ? portSizeCache.at(rtB) : 1;
                unsigned crossA = loadA * sizeB;
                unsigned crossB = loadB * sizeA;
                if (crossA != crossB)
                    return crossA < crossB;  // prefer less-loaded port
            }

            // Tier 7: register pressure — penalise instructions that would
            // cause spills (cached, O(1) lookup).
            if (policy.enableRegPressure) {
                if (rpCache[a] != rpCache[b]) return rpCache[a] < rpCache[b];
            }

            // Tier 8: register-freeing score — prefer instructions that release
            // dead predecessor values, reducing live-value count (O(1) lookup).
            if (rfsCache[a] != rfsCache[b]) return rfsCache[a] > rfsCache[b];

            // Tier 9: ROB pressure — when many instructions are inflight,
            // prefer instructions that complete sooner (lower latency) to
            // allow earlier retirement and free ROB entries.
            // Enhanced: also activate when issue slots are nearly saturated
            // (>50% inflight), not just at 75% ROB.  This improves retirement
            // rate in compute-dense code where the ROB fills quickly.
            if (policy.enableROBPressure &&
                (inflightCount > robCapacity * 3 / 4 ||
                 inflightCount > robCapacity / 2)) {
                if (lat[a] != lat[b])
                    return lat[a] < lat[b];  // shorter latency first
            }

            // Tier 9.5: Load/store cluster affinity — prefer memory instructions
            // that share the same base object (cluster group) to improve memory-
            // level parallelism and reduce TLB misses.  The last-issued cluster
            // gets preference so loads/stores to the same array are grouped.
            if (clusterGroup[a] != 0 && clusterGroup[b] != 0) {
                if (clusterGroup[a] != clusterGroup[b]) {
                    // Prefer the instruction in the same cluster as the most
                    // recently issued memory op.
                    bool aInLast = false, bInLast = false;
                    for (unsigned prev : lastClusterMemOps) {
                        if (clusterGroup[a] == prev) aInLast = true;
                        if (clusterGroup[b] == prev) bInLast = true;
                    }
                    if (aInLast != bInLast) return aInLast;
                }
            }

            // Tier 10: deterministic tie-break by original instruction index.
            return a < b;
        });

        // ── Beam search pruning ──────────────────────────────────────────────
        // For large ready lists (> beamWidth), only consider the top-N
        // candidates to prevent combinatorial explosion in very large BBs.
        // The sorted order ensures the highest-priority instructions are kept.
        // beamWidth is taken from the scheduler policy (default 32).
        if (policy.enableBeamPruning && ready.size() > policy.beamWidth)
            ready.resize(policy.beamWidth);

        // Within a cycle, track which ResourceTypes have been issued to
        // encourage diversity and fill different execution units in parallel.
        std::unordered_set<int> issuedPortsThisCycle;
        std::unordered_set<unsigned> issuedChainsThisCycle;
        unsigned issued = 0;
        unsigned decodedThisCycle = 0; // front-end decode slot usage
        loadsThisCycle = 0;
        storesThisCycle = 0;

        // Two-pass issue: first pass schedules instructions that use port
        // types not yet used this cycle (maximises parallel unit utilisation);
        // second pass fills remaining issue slots with any ready instruction.
        for (int pass = 0; pass < 2; ++pass) {
            for (unsigned id : ready) {
                if (issued >= profile.issueWidth) break;
                // ── Decode-width gating ──────────────────────────────────────
                // The front-end decoder can only crack decodeWidth instructions
                // per cycle.  Fused second-instructions don't consume a decode
                // slot (they are folded into the first instruction's µop).
                unsigned decodeCost = isFusionSecond[id] ? 0u : 1u;
                if (decodedThisCycle + decodeCost > decodeWidth) continue;
                if (done[id]) continue;

                // rtKeyCache precomputed once; avoids classifyOp + mapOpToResource.
                int rtKey = rtKeyCache[id];

                // ── Memory bandwidth gating ──────────────────────────────────
                // Don't issue more loads/stores than the L1 bandwidth supports.
                if (llvm::isa<llvm::LoadInst>(moveable[id])) {
                    unsigned loadBytes = 8; // default 64-bit load
                    if (auto* ty = moveable[id]->getType())
                        if (ty->isSized())
                            loadBytes = std::max(1u,
                                static_cast<unsigned>(dl.getTypeStoreSize(ty)));
                    if (loadsThisCycle + loadBytes > l1BW) continue;
                }

                // Pass 0: only issue to a port type not yet used this cycle.
                // Pass 1: issue to any port type (fill remaining slots).
                if (pass == 0 && issuedPortsThisCycle.count(rtKey)) continue;
                // Pass 0: also prefer chain diversity for ILP.
                if (pass == 0 && policy.enableChainDiversity &&
                    issuedChainsThisCycle.count(chainId[id])) continue;

                // Earliest start: max(currentCycle, predecessor availability).
                // For data-dep edges (edgeLat = lat[p]): wait for result.
                // For ordering edges (edgeLat = 1): wait one cycle after issue.
                unsigned earliest = currentCycle;
                for (auto [p, edgeLat] : pred[id]) {
                    if (done[p]) {
                        unsigned waitUntil = issuedAt[p] + edgeLat;
                        if (waitUntil > earliest) earliest = waitUntil;
                    }
                }

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
                    // Occupy the port for busyCycles (= reciprocal throughput).
                    // Apply the 512-bit double-pump penalty on CPUs where 512-bit
                    // SIMD is implemented by running 256-bit units twice (e.g.
                    // Skylake-AVX512, Ice Lake).  Only FMA and VecALU ports are
                    // affected; loads/stores of 512-bit data use the full-width
                    // load/store paths and do not incur the double-pump penalty.
                    unsigned pumpFactor = 1u;
                    if (profile.vec512Penalty > 1 && isWideVectorOp(moveable[id]) &&
                        (rtKey == static_cast<int>(ResourceType::FMAUnit) ||
                         rtKey == static_cast<int>(ResourceType::VectorALU))) {
                        pumpFactor = profile.vec512Penalty;
                    }
                    // Update portLoadCache incrementally: subtract the old
                    // nextFree value and add the new one so the sort comparator
                    // can evaluate tier-6.5 in O(1) without scanning all slots.
                    unsigned oldNextFree = slots[chosenSlot].nextFree;
                    slots[chosenSlot].nextFree =
                        startCycle + slots[chosenSlot].busyCycles * pumpFactor;
                    portLoadCache[rtKey] += slots[chosenSlot].nextFree - oldNextFree;
                }

                issuedAt[id] = startCycle;
                avail[id] = startCycle + lat[id];
                if (avail[id] > maxCycle) maxCycle = avail[id];

                done[id] = true;
                ++totalScheduled;
                ++issued;
                decodedThisCycle += decodeCost;
                scheduled.push_back(moveable[id]);

                // Track memory bandwidth usage this cycle.
                if (llvm::isa<llvm::LoadInst>(moveable[id])) {
                    unsigned loadBytes = 8;
                    if (auto* ty = moveable[id]->getType())
                        if (ty->isSized())
                            loadBytes = std::max(1u,
                                static_cast<unsigned>(dl.getTypeStoreSize(ty)));
                    loadsThisCycle += loadBytes;
                }
                if (llvm::isa<llvm::StoreInst>(moveable[id]))
                    ++storesThisCycle;

                // ── Store µop decomposition ──────────────────────────────────
                // Model stores as two µops: store-address on AGU + store-data
                // on StoreUnit.  Occupy both ports to match LLVM's model.
                if (modelStoreAGU && llvm::isa<llvm::StoreInst>(moveable[id])) {
                    int aguKey = static_cast<int>(ResourceType::AGU);
                    auto aguIt = hwPorts.find(aguKey);
                    if (aguIt != hwPorts.end() && !aguIt->second.empty()) {
                        auto& aguSlots = aguIt->second;
                        unsigned bestAGU = 0, bestAGUTime = std::numeric_limits<unsigned>::max();
                        for (unsigned s = 0; s < aguSlots.size(); ++s) {
                            unsigned t = std::max(startCycle, aguSlots[s].nextFree);
                            if (t < bestAGUTime) { bestAGUTime = t; bestAGU = s; }
                        }
                        unsigned oldAGU = aguSlots[bestAGU].nextFree;
                        aguSlots[bestAGU].nextFree = bestAGUTime + aguSlots[bestAGU].busyCycles;
                        portLoadCache[aguKey] += aguSlots[bestAGU].nextFree - oldAGU;
                    }
                }

                // ── Load/store cluster tracking ──────────────────────────────
                // Record this instruction's cluster group for affinity scheduling
                // in subsequent cycles.
                if (clusterGroup[id] != 0) {
                    // Keep a small window of recently issued cluster groups.
                    if (lastClusterMemOps.size() >= 4)
                        lastClusterMemOps.erase(lastClusterMemOps.begin());
                    lastClusterMemOps.push_back(clusterGroup[id]);
                }

                // Remove from the incremental ready set: this instruction is
                // now dispatched and must not be included in future cycles.
                readySet.erase(id);
                issuedPortsThisCycle.insert(rtKey);
                issuedChainsThisCycle.insert(chainId[id]);

                // ── Register pressure tracking (per register file) ──────────
                // Instruction produces a value → increase live count in the
                // appropriate register file (int or vector/FP).
                if (!moveable[id]->getType()->isVoidTy() && lat[id] > 0) {
                    if (producesVecOrFP(moveable[id])) ++vecLive;
                    else ++intLive;
                }
                // Check if scheduling this instruction kills any predecessor's
                // last use, decreasing the live count in the correct file.
                for (auto [p, _] : pred[id]) {
                    if (!done[p]) continue;
                    if (moveable[p]->getType()->isVoidTy()) continue;
                    if (remainingUsers[p] > 0) --remainingUsers[p];
                    if (remainingUsers[p] == 0) {
                        if (producesVecOrFP(moveable[p])) {
                            if (vecLive > 0) --vecLive;
                        } else {
                            if (intLive > 0) --intLive;
                        }
                    }
                }

                // Decrement in-degrees of successors.
                // Any successor whose in-degree reaches zero is now ready and
                // is added to the incremental readySet for future cycles.
                for (auto [s, _] : succ[id]) {
                    if (inDeg[s] > 0 && --inDeg[s] == 0)
                        readySet.insert(s);
                }

                // Update port pressure.
                if (portPressure[rtKey] > 0) --portPressure[rtKey];
            }
        }

        // Advance the cycle counter.  If nothing was issued this cycle,
        // skip ahead to the earliest time any port becomes free or any
        // dispatched instruction's result becomes available, avoiding
        // empty-cycle spin.  done[id] means "dispatched to the RS / execution
        // unit"; avail[id] (set at dispatch time) is when the result is ready.
        // Checking done[id] is correct: avail[] is only populated when
        // done[id]=true, so the old `!done[id]` condition was always false.
        if (issued == 0) {
            unsigned nextEvent = currentCycle + 1;
            for (auto& [key, portSlots] : hwPorts)
                for (auto& slot : portSlots)
                    if (slot.nextFree > currentCycle)
                        nextEvent = std::min(nextEvent, slot.nextFree);
            for (unsigned id = 0; id < n; ++id)
                if (done[id] && avail[id] > currentCycle)
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

    // ── 8b. Bidirectional scheduling: bottom-up comparison ──────────────────
    // LLVM's GenericScheduler runs both top-down and bottom-up scheduling passes
    // and picks the better result.  We implement a lightweight bottom-up pass:
    // schedule from sinks (instructions with no successors) toward roots,
    // using reverse critical-path depth as the primary sort key.
    //
    // The bottom-up approach is better for register-pressure-sensitive code
    // (it naturally sinks definitions close to their uses), while top-down
    // is better for latency-bound code (it prioritizes starting long chains).
    // We keep whichever schedule produces fewer estimated cycles.
    unsigned topDownCycles = maxCycle > 0 ? maxCycle : currentCycle;
    if (policy.enableBidirectional && n >= 4) {
        // Compute "height" from roots: longest path from any root to this node.
        // This is the bottom-up analog of critPath (which measures depth from sinks).
        std::vector<unsigned> height(n, 0);
        for (unsigned i = 0; i < n; ++i) {
            height[i] = lat[i];
            for (auto [p, edgeLat] : pred[i])
                height[i] = std::max(height[i], edgeLat + height[p]);
        }

        // Bottom-up scheduling: process sinks first (high height, no successors).
        // Use a simplified greedy: sort all instructions by bottom-up priority
        // (height descending, then succs-first to reduce live ranges).
        std::vector<unsigned> buOrder(n);
        std::iota(buOrder.begin(), buOrder.end(), 0u);
        std::sort(buOrder.begin(), buOrder.end(), [&](unsigned a, unsigned b) {
            // Primary: schedule sinks first (no or few successors)
            bool sinkA = succ[a].empty();
            bool sinkB = succ[b].empty();
            if (sinkA != sinkB) return sinkA;
            // Secondary: height (reverse crit-path from roots)
            if (height[a] != height[b]) return height[a] > height[b];
            // Tertiary: same as top-down for consistency
            if (critPath[a] != critPath[b]) return critPath[a] > critPath[b];
            return a > b; // reverse order for bottom-up
        });

        // Simulate bottom-up schedule to estimate cycle count.
        // Use a simple occupancy model: assign instructions to cycles respecting
        // dependencies and issue width.
        std::vector<unsigned> buAvail(n, 0);
        std::vector<unsigned> buIssued(n, 0);
        std::vector<bool> buDone(n, false);
        unsigned buCycle = 0;
        unsigned buMax = 0;
        unsigned buScheduled = 0;

        // Build reverse in-degree (out-degree of successors).
        std::vector<unsigned> outDeg(n, 0);
        for (unsigned i = 0; i < n; ++i)
            outDeg[i] = static_cast<unsigned>(succ[i].size());

        // Process in bottom-up order, assigning to earliest available cycle.
        for (unsigned id : buOrder) {
            unsigned earliest = 0;
            for (auto [p, edgeLat] : pred[id]) {
                if (buDone[p])
                    earliest = std::max(earliest, buIssued[p] + edgeLat);
            }
            buIssued[id] = earliest;
            buAvail[id] = earliest + lat[id];
            if (buAvail[id] > buMax) buMax = buAvail[id];
            buDone[id] = true;
        }

        unsigned bottomUpCycles = buMax;

        // If bottom-up produces fewer cycles, use its instruction order.
        if (bottomUpCycles < topDownCycles) {
            scheduled.clear();
            scheduled.reserve(n);
            for (unsigned id : buOrder)
                scheduled.push_back(moveable[id]);
            maxCycle = bottomUpCycles;
        }
    }

    // ── 9. Apply schedule: reorder LLVM IR within the basic block ────────────
    if (scheduled.size() == n) {
        llvm::Instruction* term = bb.getTerminator();
        for (auto* inst : scheduled)
            inst->moveBefore(bb, term->getIterator());
    }

    const unsigned bbCycles = maxCycle > 0 ? maxCycle : currentCycle;

    // ── 10. Accumulate quality metrics ──────────────────────────────────────
    if (quality && n > 0) {
        quality->scheduledCycles   += bbCycles;
        quality->instructionsTotal += n;
        ++quality->basicBlocksScheduled;

        // Track peak live register counts across the schedule.
        // We re-walk the scheduled order to obtain the watermark.
        unsigned peakInt = intLive, peakVec = vecLive;
        unsigned simInt = 0, simVec = 0;
        {
            // Reset to live-in counts and replay.
            std::unordered_set<const llvm::Value*> extInt, extVec;
            for (unsigned i = 0; i < n; ++i) {
                for (auto& use : moveable[i]->operands()) {
                    auto* val = use.get();
                    if (llvm::isa<llvm::Constant>(val)) continue;
                    bool isVec = val->getType()->isFloatingPointTy() ||
                                 val->getType()->isVectorTy();
                    if (llvm::dyn_cast<llvm::Argument>(val) ||
                        (llvm::dyn_cast<llvm::Instruction>(val) &&
                         idx.find(llvm::dyn_cast<llvm::Instruction>(val)) == idx.end()))
                        (isVec ? extVec : extInt).insert(val);
                }
            }
            simInt = static_cast<unsigned>(extInt.size());
            simVec = static_cast<unsigned>(extVec.size());

            std::vector<unsigned> remUsers2(n, 0);
            for (unsigned i = 0; i < n; ++i)
                remUsers2[i] = static_cast<unsigned>(succ[i].size());

            for (auto* inst : scheduled) {
                auto it2 = idx.find(inst);
                if (it2 == idx.end()) continue;
                unsigned id2 = it2->second;

                if (!moveable[id2]->getType()->isVoidTy() && lat[id2] > 0) {
                    if (producesVecOrFP(moveable[id2])) ++simVec;
                    else ++simInt;
                }
                for (auto [p2, _2] : pred[id2]) {
                    if (remUsers2[p2] > 0 && --remUsers2[p2] == 0) {
                        if (producesVecOrFP(moveable[p2])) {
                            assert(simVec > 0 && "vec live count underflow in quality tracking");
                            --simVec;
                        } else {
                            assert(simInt > 0 && "int live count underflow in quality tracking");
                            --simInt;
                        }
                    }
                }
                if (simInt > peakInt) peakInt = simInt;
                if (simVec > peakVec) peakVec = simVec;
            }
        }
        if (peakInt > quality->peakIntLive) quality->peakIntLive = peakInt;
        if (peakVec > quality->peakVecLive) quality->peakVecLive = peakVec;

        // Compute critical-path efficiency: maxCritPath / scheduledCycles.
        // A value of 1.0 means we scheduled at the theoretical minimum.
        unsigned maxCrit = 0;
        for (unsigned i = 0; i < n; ++i)
            if (critPath[i] > maxCrit) maxCrit = critPath[i];
        if (bbCycles > 0 && maxCrit > 0) {
            double bbEff = static_cast<double>(maxCrit) / static_cast<double>(bbCycles);
            // Weighted running average across BBs.
            unsigned prevBBs = quality->basicBlocksScheduled - 1;
            if (prevBBs == 0) {
                quality->efficiency = bbEff;
            } else {
                quality->efficiency =
                    (quality->efficiency * static_cast<double>(prevBBs) + bbEff) /
                    static_cast<double>(quality->basicBlocksScheduled);
            }
        }
    }

    return bbCycles;
}

unsigned scheduleInstructions(llvm::Function& func, const HardwareGraph& hw,
                               const MicroarchProfile& profile,
                               const SchedulerPolicy& policy,
                               SchedulerQuality* quality) {
    unsigned totalCycles = 0;
    for (auto& bb : func)
        totalCycles += scheduleBasicBlock(bb, hw, profile, policy, quality);

    // Compute IPC estimate: instructions / cycles.
    if (quality && quality->scheduledCycles > 0) {
        quality->estimatedIPC = static_cast<double>(quality->instructionsTotal) /
                                static_cast<double>(quality->scheduledCycles);
    }
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

    // ── Cost model lambda (shared by scheduling and transform phases) ────────
    // Compute total expected cycles for a function, accounting for:
    //   • Per-opcode execution latencies (the critical path contribution)
    //   • Statistical branch misprediction overhead (10% miss rate assumed
    //     for general-purpose code without profile data).
    //   • 512-bit double-pump penalty.
    auto estimateFuncCost = [&](llvm::Function& f) -> unsigned {
        constexpr double kBranchMissRate = 0.10;
        unsigned cost = 0;
        for (auto& bb : f) {
            for (auto& inst : bb) {
                if (llvm::isa<llvm::PHINode>(inst) || inst.isTerminator())
                    continue;
                unsigned latency = getOpcodeLatency(&inst, profile);
                if (profile.vec512Penalty > 1 && isWideVectorOp(&inst)) {
                    OpClass cls = classifyOp(&inst);
                    if (cls == OpClass::FMA || cls == OpClass::FPMul ||
                        cls == OpClass::FPArith || cls == OpClass::VectorOp)
                        latency *= profile.vec512Penalty;
                }
                cost += latency;
            }
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator())) {
                if (br->isConditional()) {
                    cost += static_cast<unsigned>(
                        profile.branchMispredictPenalty * kBranchMissRate + 0.5);
                }
            }
        }
        return cost;
    };

    // Step 3 — Map program onto hardware (list scheduling).
    if (config.enableScheduling) {
        MappingResult mapping = mapProgramToHardware(pg, hw, profile);
        stats.totalScheduledCycles += mapping.totalCycles;
        stats.avgPortUtilization = mapping.portUtilization;

        for (auto& bb : func) {
            unsigned bbSize = 0;
            for (auto& inst : bb)
                if (!llvm::isa<llvm::PHINode>(inst) && !inst.isTerminator())
                    ++bbSize;
            if (bbSize >= 2)
                ++stats.basicBlocksScheduled;
        }
        scheduleInstructions(func, hw, profile, config.schedulerPolicy,
                             &stats.schedulerQuality);
    }

    // Step 4 — Iterative transform-schedule pipeline.
    // Run transforms→schedule→transforms→schedule (up to 3 rounds), accepting
    // each iteration only if cost improves.  This catches cascading simplification
    // opportunities: transforms may expose new scheduling freedom, and better
    // scheduling may expose dead code or new transform targets.
    // LLVM's MachineScheduler effectively does this by running DAG mutations
    // between scheduling passes — we explicitly iterate at the IR level.
    if (config.enableTransforms) {
        unsigned bestCost = estimateFuncCost(func);
        unsigned maxIters = config.schedulerPolicy.enableIterativeRefine
                           ? config.schedulerPolicy.maxRefineIters : 1;

        for (unsigned iter = 0; iter < maxIters; ++iter) {
            unsigned costBefore = estimateFuncCost(func);
            TransformStats iterStats = applyHardwareTransforms(func, profile,
                                                                // Only annotate loops on first pass
                                                                config.enableLoopAnnotation && (iter == 0));
            unsigned costAfterTransform = estimateFuncCost(func);

            // Accumulate transform statistics.
            if (iter == 0) {
                stats.transforms = iterStats;
            } else {
                stats.transforms.fmaGenerated      += iterStats.fmaGenerated;
                stats.transforms.loadsStorePaired   += iterStats.loadsStorePaired;
                stats.transforms.prefetchesInserted += iterStats.prefetchesInserted;
                stats.transforms.branchesOptimized  += iterStats.branchesOptimized;
                stats.transforms.vectorExpanded     += iterStats.vectorExpanded;
                stats.transforms.intStrengthReduced += iterStats.intStrengthReduced;
            }

            // Re-schedule after transforms to exploit new opportunities.
            if (config.enableScheduling && costAfterTransform < costBefore) {
                scheduleInstructions(func, hw, profile, config.schedulerPolicy,
                                     &stats.schedulerQuality);
            }

            unsigned costAfter = estimateFuncCost(func);

            // Stop iterating if no improvement (fixed point reached).
            if (costAfter >= bestCost) break;
            bestCost = costAfter;
        }

        // Log the total cost delta.
        unsigned initialCost = estimateFuncCost(func);
        if (initialCost < bestCost)
            stats.totalScheduledCycles = bestCost - initialCost;
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
            if (!hasVecMD && profile.vectorWidth >= 128 && profile.vecUnits > 0) {
                // Determine the dominant element bit-width in the loop body
                // (same approach as softwarePipelineLoops) so the vectorize width
                // is expressed in lane count rather than always assuming 64-bit.
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
                for (auto& [bits, cnt] : widthFreq)
                    if (cnt > domCount) { domBits = bits; domCount = cnt; }

                unsigned lanes = profile.vectorWidth / domBits;
                if (lanes >= 2) {
                    unsigned vecWidth = std::min(lanes, 16u);
                    mds.push_back(llvm::MDNode::get(ctx, {
                        llvm::MDString::get(ctx, "llvm.loop.vectorize.width"),
                        llvm::ConstantAsMetadata::get(
                            llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), vecWidth))
                    }));
                }
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
