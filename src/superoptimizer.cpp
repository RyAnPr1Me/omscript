/// @file superoptimizer.cpp
/// @brief Superoptimizer implementation for LLVM IR.
///
/// Finds globally optimal instruction sequences by combining:
///   - Idiom recognition (popcount, bswap, rotate, min/max, abs, etc.)
///   - Algebraic identity simplification on LLVM IR
///   - Branch-to-select conversion for simple diamond CFGs
///   - Enumerative synthesis with concrete verification
///
/// The superoptimizer operates as a late-stage pass after LLVM's standard
/// pipeline, targeting patterns that individual passes miss or that require
/// cross-instruction analysis.

#include "superoptimizer.h"
#include <llvm/Config/llvm-config.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Support/KnownBits.h>
#include <llvm/Support/MathExtras.h>

// LLVM 19 introduced getOrInsertDeclaration; older versions only have getDeclaration.
#if LLVM_VERSION_MAJOR >= 19
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getOrInsertDeclaration
#else
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getDeclaration
#endif

#include <algorithm>
#include <cstdint>
#include <random>
#include <unordered_set>

namespace omscript {
namespace superopt {

using namespace llvm::PatternMatch;

// ─────────────────────────────────────────────────────────────────────────────
// Cost model
// ─────────────────────────────────────────────────────────────────────────────

double instructionCost(const llvm::Instruction* inst) {
    if (!inst) return 0.0;

    switch (inst->getOpcode()) {
    // Near-free: these are typically eliminated or folded by the backend
    case llvm::Instruction::BitCast:
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::PtrToInt:
    case llvm::Instruction::AddrSpaceCast:
        return 0.0;

    // 1-cycle ALU ops on modern x86
    case llvm::Instruction::Add:
    case llvm::Instruction::Sub:
    case llvm::Instruction::And:
    case llvm::Instruction::Or:
    case llvm::Instruction::Xor:
    case llvm::Instruction::ICmp:
    case llvm::Instruction::Trunc:
    case llvm::Instruction::ZExt:
    case llvm::Instruction::SExt:
        return 1.0;

    // Shifts: 1 cycle
    case llvm::Instruction::Shl:
    case llvm::Instruction::LShr:
    case llvm::Instruction::AShr:
        return 1.0;

    // Select (cmov): 1 cycle but adds dependency
    case llvm::Instruction::Select:
        return 1.5;

    // Multiply: 3 cycles latency
    case llvm::Instruction::Mul:
        return 3.0;

    // Division: 20-40 cycles
    case llvm::Instruction::SDiv:
    case llvm::Instruction::UDiv:
    case llvm::Instruction::SRem:
    case llvm::Instruction::URem:
        return 25.0;

    // Floating point
    case llvm::Instruction::FAdd:
    case llvm::Instruction::FSub:
        return 4.0;
    case llvm::Instruction::FMul:
        return 4.0;
    case llvm::Instruction::FDiv:
        return 15.0;
    case llvm::Instruction::FCmp:
        return 3.0;

    // Memory
    case llvm::Instruction::Load:
        return 4.0; // L1 cache hit
    case llvm::Instruction::Store:
        return 4.0;
    case llvm::Instruction::GetElementPtr:
        return 0.5; // Usually folded into addressing mode

    // Branches
    case llvm::Instruction::Br:
        return 1.0;
    case llvm::Instruction::Switch:
        return 3.0;

    // Function calls
    case llvm::Instruction::Call: {
        auto* call = llvm::cast<llvm::CallInst>(inst);
        if (auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call)) {
            switch (intrinsic->getIntrinsicID()) {
            case llvm::Intrinsic::ctpop:
                return 1.0; // POPCNT instruction
            case llvm::Intrinsic::ctlz:
            case llvm::Intrinsic::cttz:
                return 1.0; // BSR/BSF or LZCNT/TZCNT
            case llvm::Intrinsic::bswap:
                return 1.0; // BSWAP instruction
            case llvm::Intrinsic::fshl:
            case llvm::Intrinsic::fshr:
                return 1.0; // ROL/ROR instructions
            case llvm::Intrinsic::abs:
                return 2.0;
            case llvm::Intrinsic::smin:
            case llvm::Intrinsic::smax:
            case llvm::Intrinsic::umin:
            case llvm::Intrinsic::umax:
                return 1.5;
            case llvm::Intrinsic::sadd_sat:
            case llvm::Intrinsic::uadd_sat:
            case llvm::Intrinsic::ssub_sat:
            case llvm::Intrinsic::usub_sat:
                return 2.0;
            default:
                break;
            }
        }
        return 10.0; // Generic function call
    }

    // PHI nodes: free (resolved at register allocation)
    case llvm::Instruction::PHI:
        return 0.0;

    default:
        return 3.0;
    }
}

double blockCost(const llvm::BasicBlock* bb) {
    double cost = 0.0;
    for (const auto& inst : *bb) {
        cost += instructionCost(&inst);
    }
    return cost;
}

// ─────────────────────────────────────────────────────────────────────────────
// Concrete evaluator
// ─────────────────────────────────────────────────────────────────────────────

std::optional<uint64_t> evaluateInst(const llvm::Instruction* inst,
                                      const std::vector<uint64_t>& argValues) {
    if (!inst) return std::nullopt;

    // Helper: get the concrete value of an operand
    auto getVal = [&](llvm::Value* v) -> std::optional<uint64_t> {
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
            return ci->getZExtValue();
        }
        // If it's an argument, look up in argValues
        if (auto* arg = llvm::dyn_cast<llvm::Argument>(v)) {
            unsigned idx = arg->getArgNo();
            if (idx < argValues.size()) return argValues[idx];
        }
        return std::nullopt;
    };

    switch (inst->getOpcode()) {
    case llvm::Instruction::Add: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs) return *lhs + *rhs;
        break;
    }
    case llvm::Instruction::Sub: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs) return *lhs - *rhs;
        break;
    }
    case llvm::Instruction::Mul: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs) return *lhs * *rhs;
        break;
    }
    case llvm::Instruction::And: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs) return *lhs & *rhs;
        break;
    }
    case llvm::Instruction::Or: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs) return *lhs | *rhs;
        break;
    }
    case llvm::Instruction::Xor: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs) return *lhs ^ *rhs;
        break;
    }
    case llvm::Instruction::Shl: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs && *rhs < 64) return *lhs << *rhs;
        break;
    }
    case llvm::Instruction::LShr: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs && *rhs < 64) return *lhs >> *rhs;
        break;
    }
    case llvm::Instruction::AShr: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs && *rhs < 64) {
            // Arithmetic shift right (sign-extending)
            int64_t signed_lhs = static_cast<int64_t>(*lhs);
            return static_cast<uint64_t>(signed_lhs >> *rhs);
        }
        break;
    }
    default:
        break;
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection helpers
// ─────────────────────────────────────────────────────────────────────────────

/// Check if a value has exactly one use.
static bool hasOneUse(llvm::Value* v) {
    return v->hasOneUse();
}

/// Check if value is a constant integer with a specific value.
static bool isConstInt(llvm::Value* v, uint64_t val) {
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        return ci->getZExtValue() == val;
    }
    return false;
}

/// Check if value is a constant integer and return its value.
static std::optional<int64_t> getConstIntValue(llvm::Value* v) {
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        return ci->getSExtValue();
    }
    return std::nullopt;
}

/// Recursively check if a value is known to be non-negative (sign bit = 0).
/// This goes beyond computeKnownBits by tracking through nuw-flagged arithmetic,
/// XOR of non-negative values, and loop induction variable PHI nodes that
/// start from 0 and increment by a positive step.
static bool isValueNonNegative(llvm::Value* v, const llvm::DataLayout& DL, unsigned depth = 0) {
    if (depth > 6) return false;  // prevent infinite recursion

    // Non-negative constant
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v))
        return ci->getSExtValue() >= 0;

    // computeKnownBits check (handles many cases including zext, and)
    llvm::KnownBits KB = llvm::computeKnownBits(v, DL, /*Depth=*/0);
    if (KB.isNonNegative()) return true;

    auto* inst = llvm::dyn_cast<llvm::Instruction>(v);
    if (!inst) return false;

    unsigned op = inst->getOpcode();

    // add nuw: if both operands are non-negative, result is non-negative
    // (nuw guarantees no unsigned wrap, so the sum stays within [0, 2^64))
    if (op == llvm::Instruction::Add) {
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
            if (bo->hasNoUnsignedWrap()) {
                return isValueNonNegative(bo->getOperand(0), DL, depth + 1) &&
                       isValueNonNegative(bo->getOperand(1), DL, depth + 1);
            }
        }
    }

    // xor: if both operands are non-negative (sign bit = 0), result has sign bit = 0
    if (op == llvm::Instruction::Xor) {
        return isValueNonNegative(inst->getOperand(0), DL, depth + 1) &&
               isValueNonNegative(inst->getOperand(1), DL, depth + 1);
    }

    // and: if either operand is non-negative, result is non-negative
    if (op == llvm::Instruction::And) {
        return isValueNonNegative(inst->getOperand(0), DL, depth + 1) ||
               isValueNonNegative(inst->getOperand(1), DL, depth + 1);
    }

    // or: if both operands are non-negative, result is non-negative
    if (op == llvm::Instruction::Or) {
        return isValueNonNegative(inst->getOperand(0), DL, depth + 1) &&
               isValueNonNegative(inst->getOperand(1), DL, depth + 1);
    }

    // lshr (logical shift right): always non-negative (fills with 0s)
    if (op == llvm::Instruction::LShr) return true;

    // zext: always non-negative
    if (op == llvm::Instruction::ZExt) return true;

    // mul nsw nuw: if both non-negative, result is non-negative
    if (op == llvm::Instruction::Mul) {
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
            if (bo->hasNoSignedWrap() || bo->hasNoUnsignedWrap()) {
                return isValueNonNegative(bo->getOperand(0), DL, depth + 1) &&
                       isValueNonNegative(bo->getOperand(1), DL, depth + 1);
            }
        }
    }

    // shl nuw: if operand is non-negative, result is non-negative
    if (op == llvm::Instruction::Shl) {
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
            if (bo->hasNoUnsignedWrap()) {
                return isValueNonNegative(bo->getOperand(0), DL, depth + 1);
            }
        }
    }

    // PHI node: check if it's a simple loop induction variable
    // Pattern: phi [0, preheader], [inc, latch] where inc = phi + positive
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(inst)) {
        // All incoming values must be non-negative
        bool allNonNeg = true;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
            llvm::Value* incoming = phi->getIncomingValue(i);
            if (incoming == phi) continue;  // self-reference (back edge)
            // Check if the incoming value is a non-negative constant or
            // an add nuw/nsw of the phi itself (loop increment)
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(incoming)) {
                if (ci->getSExtValue() < 0) { allNonNeg = false; break; }
            } else if (auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(incoming)) {
                // Increment pattern: phi + positive_constant (with nsw or nuw)
                if (addInst->getOpcode() == llvm::Instruction::Add &&
                    (addInst->hasNoSignedWrap() || addInst->hasNoUnsignedWrap())) {
                    bool usesPhi = (addInst->getOperand(0) == phi || addInst->getOperand(1) == phi);
                    llvm::Value* step = (addInst->getOperand(0) == phi) ?
                                         addInst->getOperand(1) : addInst->getOperand(0);
                    if (usesPhi) {
                        if (auto* stepCI = llvm::dyn_cast<llvm::ConstantInt>(step)) {
                            if (stepCI->getSExtValue() <= 0) { allNonNeg = false; break; }
                        } else {
                            allNonNeg = false; break;
                        }
                    } else {
                        allNonNeg = false; break;
                    }
                } else if (addInst->getOpcode() == llvm::Instruction::Or) {
                    // After unrolling, LLVM uses `or disjoint` instead of add
                    bool usesPhi = false;
                    for (unsigned j = 0; j < addInst->getNumOperands(); j++) {
                        if (addInst->getOperand(j) == phi) { usesPhi = true; break; }
                    }
                    if (!usesPhi) { allNonNeg = false; break; }
                    // or disjoint with a positive constant is fine
                } else {
                    allNonNeg = false; break;
                }
            } else {
                allNonNeg = false; break;
            }
        }
        if (allNonNeg && phi->getNumIncomingValues() > 0) return true;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — bit rotation
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: (x << c) | (x >> (bitwidth - c))  →  rotate left by c
/// Also:   (x >> c) | (x << (bitwidth - c))  →  rotate right by c
static std::optional<IdiomMatch> detectRotate(llvm::Instruction* inst) {
    if (inst->getOpcode() != llvm::Instruction::Or) return std::nullopt;

    llvm::Value* op0 = inst->getOperand(0);
    llvm::Value* op1 = inst->getOperand(1);

    auto* shl = llvm::dyn_cast<llvm::BinaryOperator>(op0);
    auto* lshr = llvm::dyn_cast<llvm::BinaryOperator>(op1);

    // Try both orderings
    if (!shl || shl->getOpcode() != llvm::Instruction::Shl) {
        std::swap(shl, lshr);
        std::swap(op0, op1);
    }
    if (!shl || shl->getOpcode() != llvm::Instruction::Shl) return std::nullopt;
    if (!lshr || lshr->getOpcode() != llvm::Instruction::LShr) return std::nullopt;

    // Both must operate on the same value
    if (shl->getOperand(0) != lshr->getOperand(0)) return std::nullopt;

    unsigned bitWidth = inst->getType()->getIntegerBitWidth();

    auto shlAmt = getConstIntValue(shl->getOperand(1));
    auto lshrAmt = getConstIntValue(lshr->getOperand(1));

    if (!shlAmt || !lshrAmt) return std::nullopt;
    if (*shlAmt + *lshrAmt != static_cast<int64_t>(bitWidth)) return std::nullopt;

    IdiomMatch match;
    match.idiom = Idiom::RotateLeft;
    match.rootInst = inst;
    match.operands = {shl->getOperand(0), shl->getOperand(1)};
    match.bitWidth = bitWidth;
    return match;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — absolute value
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: select(x < 0, -x, x)  →  abs(x)
/// Also:   (x ^ (x >> 31)) - (x >> 31)  →  abs(x) for i32
static std::optional<IdiomMatch> detectAbsoluteValue(llvm::Instruction* inst) {
    // Pattern 1: select(icmp slt x, 0, sub 0, x, x)
    if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(inst)) {
        auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(sel->getCondition());
        if (!cmp) return std::nullopt;

        if (cmp->getPredicate() == llvm::ICmpInst::ICMP_SLT &&
            isConstInt(cmp->getOperand(1), 0)) {
            llvm::Value* x = cmp->getOperand(0);
            // Check: trueVal = -x, falseVal = x (or vice versa)
            auto* negInst = llvm::dyn_cast<llvm::BinaryOperator>(sel->getTrueValue());
            if (negInst && negInst->getOpcode() == llvm::Instruction::Sub &&
                isConstInt(negInst->getOperand(0), 0) &&
                negInst->getOperand(1) == x &&
                sel->getFalseValue() == x) {
                IdiomMatch match;
                match.idiom = Idiom::AbsoluteValue;
                match.rootInst = inst;
                match.operands = {x};
                match.bitWidth = inst->getType()->getIntegerBitWidth();
                return match;
            }
        }
    }

    // Pattern 2: XOR-subtract trick for i32
    // (x ^ (x >> 31)) - (x >> 31)
    if (inst->getOpcode() == llvm::Instruction::Sub) {
        auto* xorInst = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(0));
        auto* ashr = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(1));

        if (xorInst && ashr &&
            xorInst->getOpcode() == llvm::Instruction::Xor &&
            ashr->getOpcode() == llvm::Instruction::AShr) {
            llvm::Value* x = xorInst->getOperand(0);
            unsigned bitWidth = inst->getType()->getIntegerBitWidth();

            if (xorInst->getOperand(1) == ashr &&
                ashr->getOperand(0) == x &&
                isConstInt(ashr->getOperand(1), bitWidth - 1)) {
                IdiomMatch match;
                match.idiom = Idiom::AbsoluteValue;
                match.rootInst = inst;
                match.operands = {x};
                match.bitWidth = bitWidth;
                return match;
            }
        }
    }

    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — min/max
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: select(icmp slt a, b, a, b) → smin(a, b)
/// And all variants (sgt → smax, ult → umin, ugt → umax)
static std::optional<IdiomMatch> detectMinMax(llvm::Instruction* inst) {
    auto* sel = llvm::dyn_cast<llvm::SelectInst>(inst);
    if (!sel) return std::nullopt;

    auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(sel->getCondition());
    if (!cmp) return std::nullopt;

    llvm::Value* a = cmp->getOperand(0);
    llvm::Value* b = cmp->getOperand(1);
    llvm::Value* trueVal = sel->getTrueValue();
    llvm::Value* falseVal = sel->getFalseValue();

    // select(a < b, a, b) → min(a, b)
    // select(a > b, a, b) → max(a, b)
    Idiom idiom = Idiom::None;

    if (trueVal == a && falseVal == b) {
        switch (cmp->getPredicate()) {
        case llvm::ICmpInst::ICMP_SLT:
        case llvm::ICmpInst::ICMP_SLE:
            idiom = Idiom::IntMin;
            break;
        case llvm::ICmpInst::ICMP_SGT:
        case llvm::ICmpInst::ICMP_SGE:
            idiom = Idiom::IntMax;
            break;
        case llvm::ICmpInst::ICMP_ULT:
        case llvm::ICmpInst::ICMP_ULE:
            idiom = Idiom::IntMin;
            break;
        case llvm::ICmpInst::ICMP_UGT:
        case llvm::ICmpInst::ICMP_UGE:
            idiom = Idiom::IntMax;
            break;
        default:
            break;
        }
    } else if (trueVal == b && falseVal == a) {
        switch (cmp->getPredicate()) {
        case llvm::ICmpInst::ICMP_SLT:
        case llvm::ICmpInst::ICMP_SLE:
            idiom = Idiom::IntMax;
            break;
        case llvm::ICmpInst::ICMP_SGT:
        case llvm::ICmpInst::ICMP_SGE:
            idiom = Idiom::IntMin;
            break;
        case llvm::ICmpInst::ICMP_ULT:
        case llvm::ICmpInst::ICMP_ULE:
            idiom = Idiom::IntMax;
            break;
        case llvm::ICmpInst::ICMP_UGT:
        case llvm::ICmpInst::ICMP_UGE:
            idiom = Idiom::IntMin;
            break;
        default:
            break;
        }
    }

    if (idiom == Idiom::None) return std::nullopt;

    bool isSigned = cmp->isSigned();

    IdiomMatch match;
    match.idiom = idiom;
    match.rootInst = inst;
    match.operands = {a, b};
    match.bitWidth = inst->getType()->getIntegerBitWidth();

    // Store signedness in bitWidth high bit as a signal
    // (the caller uses the predicate to determine signed vs unsigned)
    if (!isSigned) {
        match.bitWidth |= 0x80000000u; // Mark as unsigned
    }
    return match;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — power-of-2 test
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: (x & (x - 1)) == 0  →  ctpop(x) <= 1
static std::optional<IdiomMatch> detectPowerOf2Test(llvm::Instruction* inst) {
    auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(inst);
    if (!cmp || cmp->getPredicate() != llvm::ICmpInst::ICMP_EQ)
        return std::nullopt;
    if (!isConstInt(cmp->getOperand(1), 0))
        return std::nullopt;

    auto* andInst = llvm::dyn_cast<llvm::BinaryOperator>(cmp->getOperand(0));
    if (!andInst || andInst->getOpcode() != llvm::Instruction::And)
        return std::nullopt;

    llvm::Value* x = andInst->getOperand(0);
    auto* subInst = llvm::dyn_cast<llvm::BinaryOperator>(andInst->getOperand(1));
    if (!subInst || subInst->getOpcode() != llvm::Instruction::Sub)
        return std::nullopt;
    if (subInst->getOperand(0) != x || !isConstInt(subInst->getOperand(1), 1))
        return std::nullopt;

    IdiomMatch match;
    match.idiom = Idiom::IsPowerOf2;
    match.rootInst = inst;
    match.operands = {x};
    match.bitWidth = x->getType()->getIntegerBitWidth();
    return match;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — bit field extract
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: (x >> shift) & mask  where mask = (1 << width) - 1
/// → bitfield extract from bit position `shift`, width `width`
static std::optional<IdiomMatch> detectBitFieldExtract(llvm::Instruction* inst) {
    if (inst->getOpcode() != llvm::Instruction::And) return std::nullopt;

    auto* shift = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(0));
    auto mask = getConstIntValue(inst->getOperand(1));

    if (!shift || !mask) return std::nullopt;
    if (shift->getOpcode() != llvm::Instruction::LShr &&
        shift->getOpcode() != llvm::Instruction::AShr)
        return std::nullopt;

    auto shiftAmt = getConstIntValue(shift->getOperand(1));
    if (!shiftAmt) return std::nullopt;

    // Check if mask is (1 << width) - 1
    int64_t m = *mask;
    if (m <= 0) return std::nullopt;
    // m+1 must be a power of 2
    if ((m & (m + 1)) != 0) return std::nullopt;

    IdiomMatch match;
    match.idiom = Idiom::BitFieldExtract;
    match.rootInst = inst;
    match.operands = {shift->getOperand(0), shift->getOperand(1), inst->getOperand(1)};
    match.bitWidth = inst->getType()->getIntegerBitWidth();
    return match;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — conditional negation
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: select(cond, sub(0, x), x)  →  conditional negation
/// Also:   (x ^ mask) - mask  where mask = ashr(x, bitwidth-1)
static std::optional<IdiomMatch> detectConditionalNeg(llvm::Instruction* inst) {
    // Pattern: select(cond, -x, x)
    if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(inst)) {
        llvm::Value* trueVal = sel->getTrueValue();
        llvm::Value* falseVal = sel->getFalseValue();

        // Check if trueVal = sub(0, falseVal)  i.e. trueVal = -falseVal
        if (auto* sub = llvm::dyn_cast<llvm::BinaryOperator>(trueVal)) {
            if (sub->getOpcode() == llvm::Instruction::Sub &&
                isConstInt(sub->getOperand(0), 0) &&
                sub->getOperand(1) == falseVal) {
                // This is select(cond, -x, x) but NOT abs (abs checks x < 0)
                // Only match if condition is NOT "x < 0" (abs is already handled)
                auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(sel->getCondition());
                if (cmp && cmp->getPredicate() == llvm::ICmpInst::ICMP_SLT &&
                    isConstInt(cmp->getOperand(1), 0) &&
                    cmp->getOperand(0) == falseVal) {
                    return std::nullopt; // This is abs, handled elsewhere
                }

                IdiomMatch match;
                match.idiom = Idiom::ConditionalNeg;
                match.rootInst = inst;
                match.operands = {sel->getCondition(), falseVal};
                match.bitWidth = inst->getType()->getIntegerBitWidth();
                return match;
            }
        }
        // Check reverse: select(cond, x, -x)
        if (auto* sub = llvm::dyn_cast<llvm::BinaryOperator>(falseVal)) {
            if (sub->getOpcode() == llvm::Instruction::Sub &&
                isConstInt(sub->getOperand(0), 0) &&
                sub->getOperand(1) == trueVal) {
                IdiomMatch match;
                match.idiom = Idiom::ConditionalNeg;
                match.rootInst = inst;
                // Invert: select(!cond, -x, x)
                match.operands = {sel->getCondition(), trueVal};
                match.bitWidth = inst->getType()->getIntegerBitWidth();
                return match;
            }
        }
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — count trailing zeros via isolate lowest bit
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: x & (-x) → isolate lowest set bit (related to CTZ)
/// Pattern: and(x, sub(0, x))
static std::optional<IdiomMatch> detectIsolateLowestBit(llvm::Instruction* inst) {
    if (inst->getOpcode() != llvm::Instruction::And) return std::nullopt;

    llvm::Value* op0 = inst->getOperand(0);
    llvm::Value* op1 = inst->getOperand(1);

    // Try both orderings: and(x, sub(0, x)) or and(sub(0, x), x)
    auto tryMatch = [](llvm::Value* x, llvm::Value* negCandidate) -> llvm::Value* {
        auto* sub = llvm::dyn_cast<llvm::BinaryOperator>(negCandidate);
        if (sub && sub->getOpcode() == llvm::Instruction::Sub &&
            isConstInt(sub->getOperand(0), 0) &&
            sub->getOperand(1) == x) {
            return x;
        }
        return nullptr;
    };

    llvm::Value* x = tryMatch(op0, op1);
    if (!x) x = tryMatch(op1, op0);
    if (!x) return std::nullopt;

    IdiomMatch match;
    match.idiom = Idiom::CountTrailingZeros;
    match.rootInst = inst;
    match.operands = {x};
    match.bitWidth = inst->getType()->getIntegerBitWidth();
    return match;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — byte swap
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24)
/// This is a 32-bit byte swap pattern.  Also detects the simpler 16-bit form:
/// ((x >> 8) & 0xFF) | (x << 8)
static std::optional<IdiomMatch> detectByteSwap(llvm::Instruction* inst) {
    if (inst->getOpcode() != llvm::Instruction::Or) return std::nullopt;

    // 16-bit byte swap: ((x >> 8) & 0xFF) | ((x & 0xFF) << 8)
    // or: (x >> 8) | (x << 8) for i16
    unsigned bitWidth = inst->getType()->getIntegerBitWidth();
    if (bitWidth == 16) {
        auto* op0 = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(0));
        auto* op1 = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(1));
        if (!op0 || !op1) return std::nullopt;

        // Try both orderings
        for (int swap = 0; swap < 2; ++swap) {
            if (swap) std::swap(op0, op1);
            if (op0->getOpcode() == llvm::Instruction::LShr &&
                op1->getOpcode() == llvm::Instruction::Shl &&
                isConstInt(op0->getOperand(1), 8) &&
                isConstInt(op1->getOperand(1), 8) &&
                op0->getOperand(0) == op1->getOperand(0)) {
                IdiomMatch match;
                match.idiom = Idiom::ByteSwap;
                match.rootInst = inst;
                match.operands = {op0->getOperand(0)};
                match.bitWidth = bitWidth;
                return match;
            }
        }
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — population count (Hamming weight)
// ─────────────────────────────────────────────────────────────────────────────

/// Detect the "subtract and mask" population count pattern:
/// x = x - ((x >> 1) & 0x5555555555555555)
/// This is the first step of the standard Brian Kernighan or divide-and-conquer
/// popcount algorithm.  We detect the pattern and replace with llvm.ctpop.
static std::optional<IdiomMatch> detectPopCount(llvm::Instruction* inst) {
    if (inst->getOpcode() != llvm::Instruction::Sub) return std::nullopt;

    // Look for: sub(x, and(lshr(x, 1), 0x5555...))
    llvm::Value* x = inst->getOperand(0);
    auto* andInst = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(1));
    if (!andInst || andInst->getOpcode() != llvm::Instruction::And)
        return std::nullopt;

    auto* shr = llvm::dyn_cast<llvm::BinaryOperator>(andInst->getOperand(0));
    auto maskVal = getConstIntValue(andInst->getOperand(1));
    if (!shr || !maskVal) return std::nullopt;
    if (shr->getOpcode() != llvm::Instruction::LShr) return std::nullopt;
    if (!isConstInt(shr->getOperand(1), 1)) return std::nullopt;
    if (shr->getOperand(0) != x) return std::nullopt;

    // Verify mask is 0x5555... for the bit width
    unsigned bitWidth = inst->getType()->getIntegerBitWidth();
    int64_t expected = 0;
    if (bitWidth == 32) expected = 0x55555555LL;
    else if (bitWidth == 64) expected = 0x5555555555555555LL;
    else return std::nullopt;

    if (*maskVal != expected) return std::nullopt;

    IdiomMatch match;
    match.idiom = Idiom::PopCount;
    match.rootInst = inst;
    match.operands = {x};
    match.bitWidth = bitWidth;
    return match;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main idiom detection
// ─────────────────────────────────────────────────────────────────────────────

std::vector<IdiomMatch> detectIdioms(llvm::BasicBlock& bb) {
    std::vector<IdiomMatch> results;

    for (auto& inst : bb) {
        // Try each idiom detector
        if (auto m = detectRotate(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectAbsoluteValue(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectMinMax(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectPowerOf2Test(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectBitFieldExtract(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectConditionalNeg(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectIsolateLowestBit(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectByteSwap(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectPopCount(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
    }

    return results;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom replacement — emit optimal intrinsics
// ─────────────────────────────────────────────────────────────────────────────

/// Replace a detected idiom with an optimal instruction sequence.
/// Returns true if the replacement was made.
static bool replaceIdiom(IdiomMatch& match) {
    llvm::IRBuilder<> builder(match.rootInst);
    llvm::Module* mod = match.rootInst->getModule();
    llvm::LLVMContext& ctx = mod->getContext();
    llvm::Type* intTy = match.rootInst->getType();

    switch (match.idiom) {
    case Idiom::RotateLeft: {
        // Replace with llvm.fshl(x, x, amount) — funnel shift left = rotate
        llvm::Value* x = match.operands[0];
        llvm::Value* amt = match.operands[1];
        llvm::Function* fshl = OMSC_GET_INTRINSIC(
            mod, llvm::Intrinsic::fshl, {intTy});
        llvm::Value* result = builder.CreateCall(fshl, {x, x, amt}, "rotl");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::RotateRight: {
        llvm::Value* x = match.operands[0];
        llvm::Value* amt = match.operands[1];
        llvm::Function* fshr = OMSC_GET_INTRINSIC(
            mod, llvm::Intrinsic::fshr, {intTy});
        llvm::Value* result = builder.CreateCall(fshr, {x, x, amt}, "rotr");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::AbsoluteValue: {
        llvm::Value* x = match.operands[0];
        llvm::Function* absIntrinsic = OMSC_GET_INTRINSIC(
            mod, llvm::Intrinsic::abs, {intTy});
        llvm::Value* falseConst = llvm::ConstantInt::getFalse(ctx);
        llvm::Value* result = builder.CreateCall(absIntrinsic, {x, falseConst}, "abs");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::IntMin: {
        llvm::Value* a = match.operands[0];
        llvm::Value* b = match.operands[1];
        bool isUnsigned = (match.bitWidth & 0x80000000u) != 0;
        llvm::Intrinsic::ID intrID = isUnsigned ? llvm::Intrinsic::umin : llvm::Intrinsic::smin;
        llvm::Function* minIntrinsic = OMSC_GET_INTRINSIC(mod, intrID, {intTy});
        llvm::Value* result = builder.CreateCall(minIntrinsic, {a, b}, "imin");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::IntMax: {
        llvm::Value* a = match.operands[0];
        llvm::Value* b = match.operands[1];
        bool isUnsigned = (match.bitWidth & 0x80000000u) != 0;
        llvm::Intrinsic::ID intrID = isUnsigned ? llvm::Intrinsic::umax : llvm::Intrinsic::smax;
        llvm::Function* maxIntrinsic = OMSC_GET_INTRINSIC(mod, intrID, {intTy});
        llvm::Value* result = builder.CreateCall(maxIntrinsic, {a, b}, "imax");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::IsPowerOf2: {
        // (x & (x-1)) == 0 → ctpop(x) <= 1
        // This helps the backend select POPCNT + CMP instead of SUB + AND + CMP
        llvm::Value* x = match.operands[0];
        llvm::Function* ctpop = OMSC_GET_INTRINSIC(
            mod, llvm::Intrinsic::ctpop, {x->getType()});
        llvm::Value* popcount = builder.CreateCall(ctpop, {x}, "popcnt");
        llvm::Value* one = llvm::ConstantInt::get(x->getType(), 1);
        llvm::Value* result = builder.CreateICmpULE(popcount, one, "ispow2");
        // The original was an i1, so the types match
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::ByteSwap: {
        // Replace byte-swapping OR chains with llvm.bswap intrinsic
        llvm::Value* x = match.operands[0];
        llvm::Function* bswap = OMSC_GET_INTRINSIC(
            mod, llvm::Intrinsic::bswap, {intTy});
        llvm::Value* result = builder.CreateCall(bswap, {x}, "bswap");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::PopCount: {
        // Replace population count patterns with llvm.ctpop intrinsic
        llvm::Value* x = match.operands[0];
        llvm::Function* ctpop = OMSC_GET_INTRINSIC(
            mod, llvm::Intrinsic::ctpop, {intTy});
        llvm::Value* result = builder.CreateCall(ctpop, {x}, "popcount");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Algebraic simplification on LLVM IR
// ─────────────────────────────────────────────────────────────────────────────

/// Apply algebraic identity simplifications that LLVM's instcombine may miss
/// when instructions are in different basic blocks or have multiple uses.
static unsigned applyAlgebraicSimplifications(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            llvm::Value* simplified = nullptr;

            // Pattern: (x * c1) * c2 → x * (c1*c2) when constants in different BBs
            if (inst.getOpcode() == llvm::Instruction::Mul) {
                auto* lhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (lhs && c2 && lhs->getOpcode() == llvm::Instruction::Mul) {
                    auto c1 = getConstIntValue(lhs->getOperand(1));
                    if (c1 && hasOneUse(lhs)) {
                        llvm::IRBuilder<> builder(&inst);
                        llvm::Value* combined = llvm::ConstantInt::get(
                            inst.getType(), *c1 * *c2);
                        simplified = builder.CreateMul(lhs->getOperand(0), combined, "mulcombine");
                    }
                }
            }

            // Pattern: (x + c1) + c2 → x + (c1+c2)
            if (inst.getOpcode() == llvm::Instruction::Add) {
                auto* lhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (lhs && c2 && lhs->getOpcode() == llvm::Instruction::Add) {
                    auto c1 = getConstIntValue(lhs->getOperand(1));
                    if (c1 && hasOneUse(lhs)) {
                        llvm::IRBuilder<> builder(&inst);
                        llvm::Value* combined = llvm::ConstantInt::get(
                            inst.getType(), *c1 + *c2);
                        simplified = builder.CreateAdd(lhs->getOperand(0), combined, "addcombine");
                    }
                }
            }

            // Pattern: (x << c) >> c → x & ((1 << (bitwidth - c)) - 1)  [zero extension]
            if (inst.getOpcode() == llvm::Instruction::LShr) {
                auto* shl = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto shrAmt = getConstIntValue(inst.getOperand(1));
                if (shl && shrAmt && shl->getOpcode() == llvm::Instruction::Shl) {
                    auto shlAmt = getConstIntValue(shl->getOperand(1));
                    if (shlAmt && *shlAmt == *shrAmt && hasOneUse(shl)) {
                        unsigned bitWidth = inst.getType()->getIntegerBitWidth();
                        if (*shrAmt < static_cast<int64_t>(bitWidth)) {
                            llvm::IRBuilder<> builder(&inst);
                            uint64_t mask = (1ULL << (bitWidth - *shrAmt)) - 1;
                            simplified = builder.CreateAnd(
                                shl->getOperand(0),
                                llvm::ConstantInt::get(inst.getType(), mask),
                                "zext_mask");
                        }
                    }
                }
            }

            // Pattern: x - x → 0
            if (inst.getOpcode() == llvm::Instruction::Sub) {
                if (inst.getOperand(0) == inst.getOperand(1)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                }
            }

            // Pattern: x ^ x → 0
            if (inst.getOpcode() == llvm::Instruction::Xor) {
                if (inst.getOperand(0) == inst.getOperand(1)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                }
            }

            // Pattern: x & x → x
            if (inst.getOpcode() == llvm::Instruction::And) {
                if (inst.getOperand(0) == inst.getOperand(1)) {
                    simplified = inst.getOperand(0);
                }
            }

            // Pattern: x | x → x
            if (inst.getOpcode() == llvm::Instruction::Or) {
                if (inst.getOperand(0) == inst.getOperand(1)) {
                    simplified = inst.getOperand(0);
                }
            }

            // Pattern: x + 0 → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = inst.getOperand(0);
                } else if (isConstInt(inst.getOperand(0), 0)) {
                    simplified = inst.getOperand(1);
                }
            }

            // Pattern: x * 1 → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                if (isConstInt(inst.getOperand(1), 1)) {
                    simplified = inst.getOperand(0);
                } else if (isConstInt(inst.getOperand(0), 1)) {
                    simplified = inst.getOperand(1);
                }
            }

            // Pattern: x * 0 → 0 (integer only)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                } else if (isConstInt(inst.getOperand(0), 0)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                }
            }

            // Pattern: x & 0 → 0
            if (!simplified && inst.getOpcode() == llvm::Instruction::And) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                } else if (isConstInt(inst.getOperand(0), 0)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                }
            }

            // Pattern: x | 0 → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Or) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = inst.getOperand(0);
                } else if (isConstInt(inst.getOperand(0), 0)) {
                    simplified = inst.getOperand(1);
                }
            }

            // Pattern: x & -1 → x (AND with all-ones)
            if (!simplified && inst.getOpcode() == llvm::Instruction::And) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    if (ci->isMinusOne()) {
                        simplified = inst.getOperand(0);
                    }
                } else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(0))) {
                    if (ci->isMinusOne()) {
                        simplified = inst.getOperand(1);
                    }
                }
            }

            // Pattern: x | -1 → -1 (OR with all-ones)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Or) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    if (ci->isMinusOne()) {
                        simplified = llvm::ConstantInt::get(inst.getType(), -1);
                    }
                } else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(0))) {
                    if (ci->isMinusOne()) {
                        simplified = llvm::ConstantInt::get(inst.getType(), -1);
                    }
                }
            }

            // Pattern: x * 2 → x << 1 (strength reduction)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                if (isConstInt(inst.getOperand(1), 2)) {
                    llvm::IRBuilder<> builder(&inst);
                    simplified = builder.CreateShl(inst.getOperand(0),
                        llvm::ConstantInt::get(inst.getType(), 1), "mul2_shl");
                } else if (isConstInt(inst.getOperand(0), 2)) {
                    llvm::IRBuilder<> builder(&inst);
                    simplified = builder.CreateShl(inst.getOperand(1),
                        llvm::ConstantInt::get(inst.getType(), 1), "mul2_shl");
                }
            }

            // Pattern: x << 0 → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Shl) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = inst.getOperand(0);
                }
            }

            // Pattern: x >> 0 → x (logical or arithmetic)
            if (!simplified && (inst.getOpcode() == llvm::Instruction::LShr ||
                                inst.getOpcode() == llvm::Instruction::AShr)) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = inst.getOperand(0);
                }
            }

            // Pattern: (x + y) - y → x  or  (y + x) - y → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                if (auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                    if (addInst->getOpcode() == llvm::Instruction::Add && hasOneUse(addInst)) {
                        if (addInst->getOperand(1) == inst.getOperand(1)) {
                            simplified = addInst->getOperand(0);
                        } else if (addInst->getOperand(0) == inst.getOperand(1)) {
                            simplified = addInst->getOperand(1);
                        }
                    }
                }
            }

            // Pattern: (x - y) + y → x  or  y + (x - y) → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                // Case 1: (x - y) + y
                if (auto* subInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                    if (subInst->getOpcode() == llvm::Instruction::Sub &&
                        subInst->getOperand(1) == inst.getOperand(1) &&
                        hasOneUse(subInst)) {
                        simplified = subInst->getOperand(0);
                    }
                }
                // Case 2: y + (x - y)
                if (!simplified) {
                    if (auto* subInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1))) {
                        if (subInst->getOpcode() == llvm::Instruction::Sub &&
                            subInst->getOperand(1) == inst.getOperand(0) &&
                            hasOneUse(subInst)) {
                            simplified = subInst->getOperand(0);
                        }
                    }
                }
            }

            // Pattern: ~~x → x  (xor(xor(x, -1), -1) → x)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Xor) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    if (ci->isMinusOne()) {
                        if (auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                            if (inner->getOpcode() == llvm::Instruction::Xor) {
                                if (auto* ci2 = llvm::dyn_cast<llvm::ConstantInt>(inner->getOperand(1))) {
                                    if (ci2->isMinusOne() && hasOneUse(inner)) {
                                        simplified = inner->getOperand(0);
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Pattern: (x << c1) << c2 → x << (c1 + c2)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Shl) {
                if (auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                    if (inner->getOpcode() == llvm::Instruction::Shl && hasOneUse(inner)) {
                        auto c1 = getConstIntValue(inner->getOperand(1));
                        auto c2 = getConstIntValue(inst.getOperand(1));
                        if (c1 && c2) {
                            unsigned bitWidth = inst.getType()->getIntegerBitWidth();
                            int64_t total = *c1 + *c2;
                            if (total >= 0 && total < static_cast<int64_t>(bitWidth)) {
                                llvm::IRBuilder<> builder(&inst);
                                simplified = builder.CreateShl(
                                    inner->getOperand(0),
                                    llvm::ConstantInt::get(inst.getType(), total),
                                    "shl_combine");
                            }
                        }
                    }
                }
            }

            // Pattern: (x >> c1) >> c2 → x >> (c1 + c2) (logical shift)
            if (!simplified && inst.getOpcode() == llvm::Instruction::LShr) {
                if (auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                    if (inner->getOpcode() == llvm::Instruction::LShr && hasOneUse(inner)) {
                        auto c1 = getConstIntValue(inner->getOperand(1));
                        auto c2 = getConstIntValue(inst.getOperand(1));
                        if (c1 && c2) {
                            unsigned bitWidth = inst.getType()->getIntegerBitWidth();
                            int64_t total = *c1 + *c2;
                            if (total >= 0 && total < static_cast<int64_t>(bitWidth)) {
                                llvm::IRBuilder<> builder(&inst);
                                simplified = builder.CreateLShr(
                                    inner->getOperand(0),
                                    llvm::ConstantInt::get(inst.getType(), total),
                                    "lshr_combine");
                            }
                        }
                    }
                }
            }

            // Pattern: select(cond, x, x) → x
            if (!simplified && llvm::isa<llvm::SelectInst>(inst)) {
                auto* sel = llvm::cast<llvm::SelectInst>(&inst);
                if (sel->getTrueValue() == sel->getFalseValue()) {
                    simplified = sel->getTrueValue();
                }
            }

            // Pattern: (x >> c) << c → x & ~((1 << c) - 1)  [mask off low bits]
            if (!simplified && inst.getOpcode() == llvm::Instruction::Shl) {
                auto* lshr = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto shlAmt = getConstIntValue(inst.getOperand(1));
                if (lshr && shlAmt && lshr->getOpcode() == llvm::Instruction::LShr) {
                    auto lshrAmt = getConstIntValue(lshr->getOperand(1));
                    if (lshrAmt && *lshrAmt == *shlAmt && hasOneUse(lshr)) {
                        unsigned bitWidth = inst.getType()->getIntegerBitWidth();
                        if (*shlAmt > 0 && *shlAmt < static_cast<int64_t>(bitWidth)) {
                            llvm::IRBuilder<> builder(&inst);
                            uint64_t mask = ~((1ULL << *shlAmt) - 1);
                            if (bitWidth < 64) {
                                mask &= (1ULL << bitWidth) - 1;
                            }
                            simplified = builder.CreateAnd(
                                lshr->getOperand(0),
                                llvm::ConstantInt::get(inst.getType(), mask),
                                "round_down_mask");
                        }
                    }
                }
            }

            // Pattern: x * c where c is power of 2 → x << log2(c)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                auto c = getConstIntValue(inst.getOperand(1));
                llvm::Value* var = inst.getOperand(0);
                if (!c) {
                    c = getConstIntValue(inst.getOperand(0));
                    var = inst.getOperand(1);
                }
                if (c && *c > 2 && (*c & (*c - 1)) == 0) {
                    // It's a power of 2 > 2 (mul by 2 already handled above)
                    unsigned shift = llvm::Log2_64(static_cast<uint64_t>(*c));
                    llvm::IRBuilder<> builder(&inst);
                    simplified = builder.CreateShl(var,
                        llvm::ConstantInt::get(inst.getType(), shift), "mulpow2_shl");
                }
            }

            // Pattern: udiv x, 1 → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::UDiv) {
                if (isConstInt(inst.getOperand(1), 1)) {
                    simplified = inst.getOperand(0);
                }
            }

            // Pattern: sdiv x, 1 → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::SDiv) {
                if (isConstInt(inst.getOperand(1), 1)) {
                    simplified = inst.getOperand(0);
                }
            }

            // Pattern: urem x, 1 → 0
            if (!simplified && inst.getOpcode() == llvm::Instruction::URem) {
                if (isConstInt(inst.getOperand(1), 1)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                }
            }

            // Pattern: srem x, 1 → 0
            if (!simplified && inst.getOpcode() == llvm::Instruction::SRem) {
                if (isConstInt(inst.getOperand(1), 1)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                }
            }

            // Pattern: srem x, C → urem x, C  when x is known non-negative
            // and C is a positive constant.  Unsigned remainder avoids the
            // sign-correction fixup that srem requires (saves ~6 instructions
            // in the lowered code for non-power-of-2 divisors like 37).
            // This is a key advantage the superoptimizer provides over
            // standard LLVM passes that may not prove non-negativity across
            // complex expressions involving loop induction variables.
            if (!simplified && inst.getOpcode() == llvm::Instruction::SRem) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    if (ci->getSExtValue() > 0) {
                        const llvm::DataLayout& DL = inst.getModule()->getDataLayout();
                        if (isValueNonNegative(inst.getOperand(0), DL)) {
                            llvm::IRBuilder<> builder(&inst);
                            simplified = builder.CreateURem(
                                inst.getOperand(0), inst.getOperand(1),
                                "srem_to_urem");
                        }
                    }
                }
            }

            // Pattern: sdiv x, C → udiv x, C  when x is known non-negative
            // and C is a positive constant.  Same rationale as srem→urem.
            if (!simplified && inst.getOpcode() == llvm::Instruction::SDiv) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    if (ci->getSExtValue() > 0) {
                        const llvm::DataLayout& DL = inst.getModule()->getDataLayout();
                        if (isValueNonNegative(inst.getOperand(0), DL)) {
                            llvm::IRBuilder<> builder(&inst);
                            simplified = builder.CreateUDiv(
                                inst.getOperand(0), inst.getOperand(1),
                                "sdiv_to_udiv");
                        }
                    }
                }
            }

            // Pattern: sub 0, sub(0, x) → x  (double negation)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                if (isConstInt(inst.getOperand(0), 0)) {
                    if (auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1))) {
                        if (inner->getOpcode() == llvm::Instruction::Sub &&
                            isConstInt(inner->getOperand(0), 0) && hasOneUse(inner)) {
                            simplified = inner->getOperand(1);
                        }
                    }
                }
            }

            // Pattern: (x | y) & x → x  (absorption)
            if (!simplified && inst.getOpcode() == llvm::Instruction::And) {
                auto* orInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                if (orInst && orInst->getOpcode() == llvm::Instruction::Or) {
                    if (orInst->getOperand(0) == inst.getOperand(1) ||
                        orInst->getOperand(1) == inst.getOperand(1)) {
                        simplified = inst.getOperand(1);
                    }
                }
                // Also check: x & (x | y) → x
                if (!simplified) {
                    orInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                    if (orInst && orInst->getOpcode() == llvm::Instruction::Or) {
                        if (orInst->getOperand(0) == inst.getOperand(0) ||
                            orInst->getOperand(1) == inst.getOperand(0)) {
                            simplified = inst.getOperand(0);
                        }
                    }
                }
            }

            // Pattern: (x & y) | x → x  (absorption)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Or) {
                auto* andInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                if (andInst && andInst->getOpcode() == llvm::Instruction::And) {
                    if (andInst->getOperand(0) == inst.getOperand(1) ||
                        andInst->getOperand(1) == inst.getOperand(1)) {
                        simplified = inst.getOperand(1);
                    }
                }
                if (!simplified) {
                    andInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                    if (andInst && andInst->getOpcode() == llvm::Instruction::And) {
                        if (andInst->getOperand(0) == inst.getOperand(0) ||
                            andInst->getOperand(1) == inst.getOperand(0)) {
                            simplified = inst.getOperand(0);
                        }
                    }
                }
            }

            // Pattern: and(x, -1) → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::And) {
                if (isConstInt(inst.getOperand(1), -1)) {
                    simplified = inst.getOperand(0);
                } else if (isConstInt(inst.getOperand(0), -1)) {
                    simplified = inst.getOperand(1);
                }
            }

            // Pattern: or(x, 0) → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Or) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = inst.getOperand(0);
                } else if (isConstInt(inst.getOperand(0), 0)) {
                    simplified = inst.getOperand(1);
                }
            }

            // Pattern: xor(x, 0) → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Xor) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = inst.getOperand(0);
                } else if (isConstInt(inst.getOperand(0), 0)) {
                    simplified = inst.getOperand(1);
                }
            }

            // Pattern: add(x, 0) → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = inst.getOperand(0);
                } else if (isConstInt(inst.getOperand(0), 0)) {
                    simplified = inst.getOperand(1);
                }
            }

            // Pattern: sub(x, 0) → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = inst.getOperand(0);
                }
            }

            // Pattern: mul(x, 0) → 0  (integer only)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                if (isConstInt(inst.getOperand(1), 0)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                } else if (isConstInt(inst.getOperand(0), 0)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                }
            }

            // Pattern: mul(x, 1) → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                if (isConstInt(inst.getOperand(1), 1)) {
                    simplified = inst.getOperand(0);
                } else if (isConstInt(inst.getOperand(0), 1)) {
                    simplified = inst.getOperand(1);
                }
            }

            // Pattern: (x >> c1) >> c2 → x >> (c1 + c2) (arithmetic shift)
            if (!simplified && inst.getOpcode() == llvm::Instruction::AShr) {
                if (auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                    if (inner->getOpcode() == llvm::Instruction::AShr && hasOneUse(inner)) {
                        auto c1 = getConstIntValue(inner->getOperand(1));
                        auto c2 = getConstIntValue(inst.getOperand(1));
                        if (c1 && c2) {
                            unsigned bitWidth = inst.getType()->getIntegerBitWidth();
                            int64_t total = *c1 + *c2;
                            if (total >= 0 && total < static_cast<int64_t>(bitWidth)) {
                                llvm::IRBuilder<> builder(&inst);
                                simplified = builder.CreateAShr(
                                    inner->getOperand(0),
                                    llvm::ConstantInt::get(inst.getType(), total),
                                    "ashr_combine");
                            }
                        }
                    }
                }
            }

            // Pattern: icmp eq x, x → true (i1 1)
            if (!simplified && inst.getOpcode() == llvm::Instruction::ICmp) {
                auto* cmp = llvm::cast<llvm::ICmpInst>(&inst);
                if (cmp->getOperand(0) == cmp->getOperand(1)) {
                    switch (cmp->getPredicate()) {
                    case llvm::CmpInst::ICMP_EQ:
                    case llvm::CmpInst::ICMP_ULE:
                    case llvm::CmpInst::ICMP_UGE:
                    case llvm::CmpInst::ICMP_SLE:
                    case llvm::CmpInst::ICMP_SGE:
                        simplified = llvm::ConstantInt::getTrue(inst.getType());
                        break;
                    case llvm::CmpInst::ICMP_NE:
                    case llvm::CmpInst::ICMP_ULT:
                    case llvm::CmpInst::ICMP_UGT:
                    case llvm::CmpInst::ICMP_SLT:
                    case llvm::CmpInst::ICMP_SGT:
                        simplified = llvm::ConstantInt::getFalse(inst.getType());
                        break;
                    default:
                        break;
                    }
                }
            }

            // Pattern: (a + b) ^ (a & b) → (a | b)
            // Proof: a+b = (a^b) + 2*(a&b), but (a+b)^(a&b) = a|b is only valid
            // when there's no carry.  For general values: (a^b) + (a&b) = a|b is NOT
            // true.  So skip this — it's not a correct identity for all inputs.

            // Pattern: (x << c) | (x >> (bitwidth - c)) → rotate left
            // These are already handled by the idiom recognizer, so skip here.

            // Pattern: zext(trunc(x)) where no bits lost → x
            // Only valid when upper bits are known zero.
            // Leave to LLVM's instcombine as it has proper known-bits analysis.

            // Pattern: select(cond, x, undef) → x
            if (!simplified && llvm::isa<llvm::SelectInst>(inst)) {
                auto* sel = llvm::cast<llvm::SelectInst>(&inst);
                if (llvm::isa<llvm::UndefValue>(sel->getFalseValue())) {
                    simplified = sel->getTrueValue();
                } else if (llvm::isa<llvm::UndefValue>(sel->getTrueValue())) {
                    simplified = sel->getFalseValue();
                }
            }

            // Pattern: select(true, a, b) → a
            if (!simplified && llvm::isa<llvm::SelectInst>(inst)) {
                auto* sel = llvm::cast<llvm::SelectInst>(&inst);
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(sel->getCondition())) {
                    simplified = ci->isOne() ? sel->getTrueValue() : sel->getFalseValue();
                }
            }

            // Pattern: (x ^ -1) → ~x   (canonical form for LLVM)
            // Already in canonical form. No action needed.

            // Pattern: -(-x) → x  (sub 0, sub 0, x → x)  (already handled above)

            // Pattern: x + (-y) → x - y
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                // Check if RHS is sub(0, y)
                if (auto* rhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1))) {
                    if (rhs->getOpcode() == llvm::Instruction::Sub &&
                        isConstInt(rhs->getOperand(0), 0) && hasOneUse(rhs)) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateSub(inst.getOperand(0),
                            rhs->getOperand(1), "add_neg_to_sub");
                    }
                }
                // Check if LHS is sub(0, x)
                if (!simplified) {
                    if (auto* lhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                        if (lhs->getOpcode() == llvm::Instruction::Sub &&
                            isConstInt(lhs->getOperand(0), 0) && hasOneUse(lhs)) {
                            llvm::IRBuilder<> builder(&inst);
                            simplified = builder.CreateSub(inst.getOperand(1),
                                lhs->getOperand(1), "neg_add_to_sub");
                        }
                    }
                }
            }

            // Pattern: (x * c1) + (x * c2) → x * (c1 + c2)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                auto* lhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* rhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (lhs && rhs &&
                    lhs->getOpcode() == llvm::Instruction::Mul &&
                    rhs->getOpcode() == llvm::Instruction::Mul) {
                    auto c1 = getConstIntValue(lhs->getOperand(1));
                    auto c2 = getConstIntValue(rhs->getOperand(1));
                    if (c1 && c2 &&
                        lhs->getOperand(0) == rhs->getOperand(0) &&
                        hasOneUse(lhs) && hasOneUse(rhs)) {
                        llvm::IRBuilder<> builder(&inst);
                        llvm::Value* combined = llvm::ConstantInt::get(
                            inst.getType(), *c1 + *c2);
                        simplified = builder.CreateMul(lhs->getOperand(0), combined,
                            "factor_mul");
                    }
                }
            }

            // Pattern: (x * c1) - (x * c2) → x * (c1 - c2)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                auto* lhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* rhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (lhs && rhs &&
                    lhs->getOpcode() == llvm::Instruction::Mul &&
                    rhs->getOpcode() == llvm::Instruction::Mul) {
                    auto c1 = getConstIntValue(lhs->getOperand(1));
                    auto c2 = getConstIntValue(rhs->getOperand(1));
                    if (c1 && c2 &&
                        lhs->getOperand(0) == rhs->getOperand(0) &&
                        hasOneUse(lhs) && hasOneUse(rhs)) {
                        llvm::IRBuilder<> builder(&inst);
                        llvm::Value* combined = llvm::ConstantInt::get(
                            inst.getType(), *c1 - *c2);
                        simplified = builder.CreateMul(lhs->getOperand(0), combined,
                            "factor_sub_mul");
                    }
                }
            }

            // ── Unsigned div/rem by power-of-2 strength reduction ────────────
            // udiv x, 2^n → x >> n  (safe ONLY for unsigned)
            if (!simplified && inst.getOpcode() == llvm::Instruction::UDiv) {
                auto c = getConstIntValue(inst.getOperand(1));
                if (c && *c > 1 && (*c & (*c - 1)) == 0) {
                    unsigned shift = llvm::Log2_64(static_cast<uint64_t>(*c));
                    llvm::IRBuilder<> builder(&inst);
                    simplified = builder.CreateLShr(inst.getOperand(0),
                        llvm::ConstantInt::get(inst.getType(), shift), "udiv_pow2");
                }
            }
            // urem x, 2^n → x & (2^n - 1)  (safe ONLY for unsigned)
            if (!simplified && inst.getOpcode() == llvm::Instruction::URem) {
                auto c = getConstIntValue(inst.getOperand(1));
                if (c && *c > 1 && (*c & (*c - 1)) == 0) {
                    llvm::IRBuilder<> builder(&inst);
                    simplified = builder.CreateAnd(inst.getOperand(0),
                        llvm::ConstantInt::get(inst.getType(), *c - 1), "urem_pow2");
                }
            }
            // sdiv x, -1 → 0 - x  (negate via nsw sub from zero)
            // Guard: INT_MIN / -1 is undefined behaviour (overflows).  The
            // LLVM `sub nsw` carries the same UB semantics as C negation, so
            // this transformation is correct under LLVM's poison / UB rules.
            if (!simplified && inst.getOpcode() == llvm::Instruction::SDiv) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    if (ci->isMinusOne()) {
                        llvm::IRBuilder<> builder(&inst);
                        // Use CreateNSWNeg so overflow (INT_MIN ÷ -1) remains
                        // poison rather than silently wrapping.
                        simplified = builder.CreateNSWNeg(inst.getOperand(0), "sdiv_neg1");
                    }
                }
            }

            // ── Multiply strength reduction: x * small_const → shifts+adds ──
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                auto cv = getConstIntValue(inst.getOperand(1));
                llvm::Value* xv = inst.getOperand(0);
                if (!cv) {
                    cv = getConstIntValue(inst.getOperand(0));
                    xv = inst.getOperand(1);
                }
                if (cv) {
                    llvm::IRBuilder<> builder(&inst);
                    llvm::Type* ty = inst.getType();
                    auto mk = [&](int64_t v) { return llvm::ConstantInt::get(ty, v); };
                    auto shl = [&](llvm::Value* v, int64_t s) {
                        return builder.CreateShl(v, mk(s));
                    };
                    switch (*cv) {
                    case 3:  simplified = builder.CreateAdd(shl(xv,1), xv, "mul3"); break;
                    case 5:  simplified = builder.CreateAdd(shl(xv,2), xv, "mul5"); break;
                    case 6:  simplified = builder.CreateAdd(shl(xv,2), shl(xv,1), "mul6"); break;
                    case 7:  simplified = builder.CreateSub(shl(xv,3), xv, "mul7"); break;
                    case 9:  simplified = builder.CreateAdd(shl(xv,3), xv, "mul9"); break;
                    case 10: simplified = builder.CreateAdd(shl(xv,3), shl(xv,1), "mul10"); break;
                    case 11: simplified = builder.CreateAdd(builder.CreateAdd(shl(xv,3), shl(xv,1)), xv, "mul11"); break;
                    case 12: simplified = builder.CreateAdd(shl(xv,3), shl(xv,2), "mul12"); break;
                    case 13: simplified = builder.CreateAdd(builder.CreateAdd(shl(xv,3), shl(xv,2)), xv, "mul13"); break;
                    case 14: simplified = builder.CreateSub(shl(xv,4), shl(xv,1), "mul14"); break;
                    case 15: simplified = builder.CreateSub(shl(xv,4), xv, "mul15"); break;
                    case 17: simplified = builder.CreateAdd(shl(xv,4), xv, "mul17"); break;
                    case 18: simplified = builder.CreateAdd(shl(xv,4), shl(xv,1), "mul18"); break;
                    case 19: simplified = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,1)), xv, "mul19"); break;
                    case 20: simplified = builder.CreateAdd(shl(xv,4), shl(xv,2), "mul20"); break;
                    case 21: simplified = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,2)), xv, "mul21"); break;
                    case 24: simplified = builder.CreateAdd(shl(xv,4), shl(xv,3), "mul24"); break;
                    case 25: simplified = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,3)), xv, "mul25"); break;
                    case 28: simplified = builder.CreateSub(shl(xv,5), shl(xv,2), "mul28"); break;
                    case 30: simplified = builder.CreateSub(shl(xv,5), shl(xv,1), "mul30"); break;
                    case 31: simplified = builder.CreateSub(shl(xv,5), xv, "mul31"); break;
                    case 33: simplified = builder.CreateAdd(shl(xv,5), xv, "mul33"); break;
                    case 34: simplified = builder.CreateAdd(shl(xv,5), shl(xv,1), "mul34"); break;
                    case 36: simplified = builder.CreateAdd(shl(xv,5), shl(xv,2), "mul36"); break;
                    case 40: simplified = builder.CreateAdd(shl(xv,5), shl(xv,3), "mul40"); break;
                    case 48: simplified = builder.CreateAdd(shl(xv,5), shl(xv,4), "mul48"); break;
                    case 56: simplified = builder.CreateSub(shl(xv,6), shl(xv,3), "mul56"); break;
                    case 60: simplified = builder.CreateSub(shl(xv,6), shl(xv,2), "mul60"); break;
                    case 63: simplified = builder.CreateSub(shl(xv,6), xv, "mul63"); break;
                    case 65: simplified = builder.CreateAdd(shl(xv,6), xv, "mul65"); break;
                    case 72: simplified = builder.CreateAdd(shl(xv,6), shl(xv,3), "mul72"); break;
                    case 80: simplified = builder.CreateAdd(shl(xv,6), shl(xv,4), "mul80"); break;
                    case 96: simplified = builder.CreateAdd(shl(xv,6), shl(xv,5), "mul96"); break;
                    case 127: simplified = builder.CreateSub(shl(xv,7), xv, "mul127"); break;
                    case 129: simplified = builder.CreateAdd(shl(xv,7), xv, "mul129"); break;
                    case 192: simplified = builder.CreateAdd(shl(xv,7), shl(xv,6), "mul192"); break;
                    case 255: simplified = builder.CreateSub(shl(xv,8), xv, "mul255"); break;
                    case 257: simplified = builder.CreateAdd(shl(xv,8), xv, "mul257"); break;
                    case 511: simplified = builder.CreateSub(shl(xv,9), xv, "mul511"); break;
                    case 513: simplified = builder.CreateAdd(shl(xv,9), xv, "mul513"); break;
                    case 1000: simplified = builder.CreateSub(builder.CreateSub(shl(xv,10), shl(xv,4)), shl(xv,3), "mul1000"); break;
                    case 1023: simplified = builder.CreateSub(shl(xv,10), xv, "mul1023"); break;
                    case 1025: simplified = builder.CreateAdd(shl(xv,10), xv, "mul1025"); break;
                    default: break;
                    }
                }
            }

            // ── Constant arithmetic folding: (x op c1) op c2 → x op (c1 op c2) ─
            // (x - c1) - c2 → x - (c1+c2)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (inner && c2 && inner->getOpcode() == llvm::Instruction::Sub && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateSub(inner->getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), *c1 + *c2), "sub_const_fold");
                    }
                }
            }
            // (x + c1) - c2 → x + (c1-c2)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (inner && c2 && inner->getOpcode() == llvm::Instruction::Add && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1) {
                        llvm::IRBuilder<> builder(&inst);
                        int64_t diff = *c1 - *c2;
                        if (diff == 0) {
                            simplified = inner->getOperand(0);
                        } else if (diff > 0) {
                            simplified = builder.CreateAdd(inner->getOperand(0),
                                llvm::ConstantInt::get(inst.getType(), diff), "add_sub_fold");
                        } else {
                            simplified = builder.CreateSub(inner->getOperand(0),
                                llvm::ConstantInt::get(inst.getType(), -diff), "add_sub_fold2");
                        }
                    }
                }
            }
            // (x - c1) + c2 → x + (c2-c1)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (inner && c2 && inner->getOpcode() == llvm::Instruction::Sub && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1) {
                        llvm::IRBuilder<> builder(&inst);
                        int64_t diff = *c2 - *c1;
                        if (diff == 0) {
                            simplified = inner->getOperand(0);
                        } else if (diff > 0) {
                            simplified = builder.CreateAdd(inner->getOperand(0),
                                llvm::ConstantInt::get(inst.getType(), diff), "sub_add_fold");
                        } else {
                            simplified = builder.CreateSub(inner->getOperand(0),
                                llvm::ConstantInt::get(inst.getType(), -diff), "sub_add_fold2");
                        }
                    }
                }
            }
            // (x * c1) + x → x * (c1+1)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                llvm::Value* rhs = inst.getOperand(1);
                if (inner && inner->getOpcode() == llvm::Instruction::Mul && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1 && inner->getOperand(0) == rhs) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateMul(rhs,
                            llvm::ConstantInt::get(inst.getType(), *c1 + 1), "mul_plus_x");
                    }
                }
                if (!simplified) {
                    inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                    llvm::Value* lhs = inst.getOperand(0);
                    if (inner && inner->getOpcode() == llvm::Instruction::Mul && hasOneUse(inner)) {
                        auto c1 = getConstIntValue(inner->getOperand(1));
                        if (c1 && inner->getOperand(0) == lhs) {
                            llvm::IRBuilder<> builder(&inst);
                            simplified = builder.CreateMul(lhs,
                                llvm::ConstantInt::get(inst.getType(), *c1 + 1), "mul_plus_x2");
                        }
                    }
                }
            }
            // (x * c1) - x → x * (c1-1)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                llvm::Value* rhs = inst.getOperand(1);
                if (inner && inner->getOpcode() == llvm::Instruction::Mul && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1 && inner->getOperand(0) == rhs) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateMul(rhs,
                            llvm::ConstantInt::get(inst.getType(), *c1 - 1), "mul_minus_x");
                    }
                }
            }
            // x - (x * c) → x * (1-c)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                llvm::Value* lhs = inst.getOperand(0);
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (inner && inner->getOpcode() == llvm::Instruction::Mul && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1 && inner->getOperand(0) == lhs) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateMul(lhs,
                            llvm::ConstantInt::get(inst.getType(), 1 - *c1), "x_minus_mul");
                    }
                }
            }

            // ── Nested AND/OR/XOR with constants ─────────────────────────────
            // (x & c1) & c2 → x & (c1&c2)
            if (!simplified && inst.getOpcode() == llvm::Instruction::And) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (inner && c2 && inner->getOpcode() == llvm::Instruction::And && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateAnd(inner->getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), *c1 & *c2), "and_const_fold");
                    }
                }
            }
            // (x | c1) | c2 → x | (c1|c2)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Or) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (inner && c2 && inner->getOpcode() == llvm::Instruction::Or && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateOr(inner->getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), *c1 | *c2), "or_const_fold");
                    }
                }
            }
            // (x ^ c1) ^ c2 → x ^ (c1^c2)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Xor) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (inner && c2 && inner->getOpcode() == llvm::Instruction::Xor && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateXor(inner->getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), *c1 ^ *c2), "xor_const_fold");
                    }
                }
            }
            // (x & c1) | (x & c2) → x & (c1|c2)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Or) {
                auto* lhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* rhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (lhs && rhs &&
                    lhs->getOpcode() == llvm::Instruction::And &&
                    rhs->getOpcode() == llvm::Instruction::And &&
                    lhs->getOperand(0) == rhs->getOperand(0) &&
                    hasOneUse(lhs) && hasOneUse(rhs)) {
                    auto c1 = getConstIntValue(lhs->getOperand(1));
                    auto c2 = getConstIntValue(rhs->getOperand(1));
                    if (c1 && c2) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateAnd(lhs->getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), *c1 | *c2), "and_masks_or");
                    }
                }
            }
            // (x | c1) & (x | c2) → x | (c1&c2)
            if (!simplified && inst.getOpcode() == llvm::Instruction::And) {
                auto* lhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* rhs = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (lhs && rhs &&
                    lhs->getOpcode() == llvm::Instruction::Or &&
                    rhs->getOpcode() == llvm::Instruction::Or &&
                    lhs->getOperand(0) == rhs->getOperand(0) &&
                    hasOneUse(lhs) && hasOneUse(rhs)) {
                    auto c1 = getConstIntValue(lhs->getOperand(1));
                    auto c2 = getConstIntValue(rhs->getOperand(1));
                    if (c1 && c2) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateOr(lhs->getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), *c1 & *c2), "or_masks_and");
                    }
                }
            }

            // ── XOR cancellation chains ────────────────────────────────────
            // (x ^ y) ^ y → x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Xor) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                llvm::Value* rhs = inst.getOperand(1);
                if (inner && inner->getOpcode() == llvm::Instruction::Xor && hasOneUse(inner)) {
                    if (inner->getOperand(1) == rhs) {
                        simplified = inner->getOperand(0);
                    } else if (inner->getOperand(0) == rhs) {
                        simplified = inner->getOperand(1);
                    }
                }
            }
            // x ^ (x ^ y) → y
            if (!simplified && inst.getOpcode() == llvm::Instruction::Xor) {
                llvm::Value* lhs = inst.getOperand(0);
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (inner && inner->getOpcode() == llvm::Instruction::Xor && hasOneUse(inner)) {
                    if (inner->getOperand(0) == lhs) {
                        simplified = inner->getOperand(1);
                    } else if (inner->getOperand(1) == lhs) {
                        simplified = inner->getOperand(0);
                    }
                }
            }

            // ── Shifts: shl/lshr/ashr by >= bitwidth → 0 or sign bit ────────
            if (!simplified && (inst.getOpcode() == llvm::Instruction::Shl ||
                                 inst.getOpcode() == llvm::Instruction::LShr)) {
                auto c = getConstIntValue(inst.getOperand(1));
                if (c && inst.getType()->isIntegerTy()) {
                    unsigned bw = inst.getType()->getIntegerBitWidth();
                    if (*c >= static_cast<int64_t>(bw)) {
                        simplified = llvm::ConstantInt::get(inst.getType(), 0);
                    }
                }
            }
            if (!simplified && inst.getOpcode() == llvm::Instruction::AShr) {
                auto c = getConstIntValue(inst.getOperand(1));
                if (c && inst.getType()->isIntegerTy()) {
                    unsigned bw = inst.getType()->getIntegerBitWidth();
                    if (*c >= static_cast<int64_t>(bw)) {
                        // AShr by >= bitwidth → sign extension = ashr x, (bw-1)
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateAShr(inst.getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), bw - 1), "ashr_clamp");
                    }
                }
            }

            // ── (shl x, c) + x → x * (1 + 2^c)  [for c <= 30] ──────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                auto* shlInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                llvm::Value* xv = inst.getOperand(1);
                if (shlInst && shlInst->getOpcode() == llvm::Instruction::Shl &&
                    shlInst->getOperand(0) == xv && hasOneUse(shlInst)) {
                    auto c = getConstIntValue(shlInst->getOperand(1));
                    if (c && *c > 0 && *c <= 30) {
                        llvm::IRBuilder<> builder(&inst);
                        int64_t factor = (1LL << *c) + 1;
                        simplified = builder.CreateMul(xv,
                            llvm::ConstantInt::get(inst.getType(), factor), "shl_add_mul");
                    }
                }
                if (!simplified) {
                    shlInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                    xv = inst.getOperand(0);
                    if (shlInst && shlInst->getOpcode() == llvm::Instruction::Shl &&
                        shlInst->getOperand(0) == xv && hasOneUse(shlInst)) {
                        auto c = getConstIntValue(shlInst->getOperand(1));
                        if (c && *c > 0 && *c <= 30) {
                            llvm::IRBuilder<> builder(&inst);
                            int64_t factor = (1LL << *c) + 1;
                            simplified = builder.CreateMul(xv,
                                llvm::ConstantInt::get(inst.getType(), factor), "shl_add_mul2");
                        }
                    }
                }
            }
            // ── (shl x, c) - x → x * (2^c - 1) ─────────────────────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                auto* shlInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                llvm::Value* xv = inst.getOperand(1);
                if (shlInst && shlInst->getOpcode() == llvm::Instruction::Shl &&
                    shlInst->getOperand(0) == xv && hasOneUse(shlInst)) {
                    auto c = getConstIntValue(shlInst->getOperand(1));
                    if (c && *c > 0 && *c <= 30) {
                        llvm::IRBuilder<> builder(&inst);
                        int64_t factor = (1LL << *c) - 1;
                        simplified = builder.CreateMul(xv,
                            llvm::ConstantInt::get(inst.getType(), factor), "shl_sub_mul");
                    }
                }
            }

            // ── (shl x, c1) + (shl x, c2) → x * (2^c1 + 2^c2) ─────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                auto* shl1 = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* shl2 = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (shl1 && shl2 &&
                    shl1->getOpcode() == llvm::Instruction::Shl &&
                    shl2->getOpcode() == llvm::Instruction::Shl &&
                    shl1->getOperand(0) == shl2->getOperand(0) &&
                    hasOneUse(shl1) && hasOneUse(shl2)) {
                    auto c1 = getConstIntValue(shl1->getOperand(1));
                    auto c2 = getConstIntValue(shl2->getOperand(1));
                    if (c1 && c2 && *c1 >= 0 && *c2 >= 0 && *c1 <= 30 && *c2 <= 30) {
                        llvm::IRBuilder<> builder(&inst);
                        int64_t factor = (1LL << *c1) + (1LL << *c2);
                        simplified = builder.CreateMul(shl1->getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), factor), "shl_shl_add_mul");
                    }
                }
            }
            // ── (shl x, c1) - (shl x, c2) → x * (2^c1 - 2^c2) ─────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                auto* shl1 = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* shl2 = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (shl1 && shl2 &&
                    shl1->getOpcode() == llvm::Instruction::Shl &&
                    shl2->getOpcode() == llvm::Instruction::Shl &&
                    shl1->getOperand(0) == shl2->getOperand(0) &&
                    hasOneUse(shl1) && hasOneUse(shl2)) {
                    auto c1 = getConstIntValue(shl1->getOperand(1));
                    auto c2 = getConstIntValue(shl2->getOperand(1));
                    if (c1 && c2 && *c1 >= 0 && *c2 >= 0 && *c1 <= 30 && *c2 <= 30 && *c1 > *c2) {
                        llvm::IRBuilder<> builder(&inst);
                        int64_t factor = (1LL << *c1) - (1LL << *c2);
                        simplified = builder.CreateMul(shl1->getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), factor), "shl_shl_sub_mul");
                    }
                }
            }

            // ── mul(add(x,c1), c2) → add(mul(x,c2), c1*c2) ─────────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (inner && c2 && inner->getOpcode() == llvm::Instruction::Add && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1) {
                        llvm::IRBuilder<> builder(&inst);
                        llvm::Value* xc2 = builder.CreateMul(inner->getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), *c2), "mul_dist");
                        simplified = builder.CreateAdd(xc2,
                            llvm::ConstantInt::get(inst.getType(), *c1 * *c2), "mul_dist_add");
                    }
                }
            }
            // ── mul(sub(x,c1), c2) → sub(mul(x,c2), c1*c2) ─────────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (inner && c2 && inner->getOpcode() == llvm::Instruction::Sub && hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1) {
                        llvm::IRBuilder<> builder(&inst);
                        llvm::Value* xc2 = builder.CreateMul(inner->getOperand(0),
                            llvm::ConstantInt::get(inst.getType(), *c2), "mul_dist_sub");
                        simplified = builder.CreateSub(xc2,
                            llvm::ConstantInt::get(inst.getType(), *c1 * *c2), "mul_dist_sub2");
                    }
                }
            }

            // ── add(mul(x,c1), mul(y,c1)) → mul(add(x,y), c1) ──────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                auto* lhsi = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* rhsi = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (lhsi && rhsi &&
                    lhsi->getOpcode() == llvm::Instruction::Mul &&
                    rhsi->getOpcode() == llvm::Instruction::Mul &&
                    hasOneUse(lhsi) && hasOneUse(rhsi)) {
                    auto c1 = getConstIntValue(lhsi->getOperand(1));
                    auto c2 = getConstIntValue(rhsi->getOperand(1));
                    if (c1 && c2 && *c1 == *c2) {
                        llvm::IRBuilder<> builder(&inst);
                        llvm::Value* sum = builder.CreateAdd(lhsi->getOperand(0), rhsi->getOperand(0), "factored_add");
                        simplified = builder.CreateMul(sum,
                            llvm::ConstantInt::get(inst.getType(), *c1), "factored_mul");
                    }
                }
            }
            // ── sub(mul(x,c1), mul(y,c1)) → mul(sub(x,y), c1) ──────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                auto* lhsi = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* rhsi = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (lhsi && rhsi &&
                    lhsi->getOpcode() == llvm::Instruction::Mul &&
                    rhsi->getOpcode() == llvm::Instruction::Mul &&
                    hasOneUse(lhsi) && hasOneUse(rhsi)) {
                    auto c1 = getConstIntValue(lhsi->getOperand(1));
                    auto c2 = getConstIntValue(rhsi->getOperand(1));
                    if (c1 && c2 && *c1 == *c2) {
                        llvm::IRBuilder<> builder(&inst);
                        llvm::Value* diff = builder.CreateSub(lhsi->getOperand(0), rhsi->getOperand(0), "factored_sub");
                        simplified = builder.CreateMul(diff,
                            llvm::ConstantInt::get(inst.getType(), *c1), "factored_mul2");
                    }
                }
            }

            // ── icmp: fold comparisons with offset ───────────────────────────
            // icmp eq (add x, c), 0 → icmp eq x, -c
            if (!simplified && inst.getOpcode() == llvm::Instruction::ICmp) {
                auto* cmp = llvm::cast<llvm::ICmpInst>(&inst);
                if (cmp->getPredicate() == llvm::ICmpInst::ICMP_EQ ||
                    cmp->getPredicate() == llvm::ICmpInst::ICMP_NE) {
                    auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(cmp->getOperand(0));
                    if (addInst && addInst->getOpcode() == llvm::Instruction::Add &&
                        isConstInt(cmp->getOperand(1), 0)) {
                        auto c = getConstIntValue(addInst->getOperand(1));
                        if (c) {
                            llvm::IRBuilder<> builder(&inst);
                            llvm::Value* newCmp = builder.CreateICmp(cmp->getPredicate(),
                                addInst->getOperand(0),
                                llvm::ConstantInt::get(addInst->getType(), -*c), "cmp_add_fold");
                            simplified = newCmp;
                        }
                    }
                }
            }
            // icmp eq (sub x, c), 0 → icmp eq x, c
            if (!simplified && inst.getOpcode() == llvm::Instruction::ICmp) {
                auto* cmp = llvm::cast<llvm::ICmpInst>(&inst);
                if (cmp->getPredicate() == llvm::ICmpInst::ICMP_EQ ||
                    cmp->getPredicate() == llvm::ICmpInst::ICMP_NE) {
                    auto* subInst = llvm::dyn_cast<llvm::BinaryOperator>(cmp->getOperand(0));
                    if (subInst && subInst->getOpcode() == llvm::Instruction::Sub &&
                        isConstInt(cmp->getOperand(1), 0)) {
                        auto c = getConstIntValue(subInst->getOperand(1));
                        if (c) {
                            llvm::IRBuilder<> builder(&inst);
                            llvm::Value* newCmp = builder.CreateICmp(cmp->getPredicate(),
                                subInst->getOperand(0),
                                llvm::ConstantInt::get(subInst->getType(), *c), "cmp_sub_fold");
                            simplified = newCmp;
                        }
                    }
                }
            }
            // icmp ult x, 1 → icmp eq x, 0
            if (!simplified && inst.getOpcode() == llvm::Instruction::ICmp) {
                auto* cmp = llvm::cast<llvm::ICmpInst>(&inst);
                if (cmp->getPredicate() == llvm::ICmpInst::ICMP_ULT &&
                    isConstInt(cmp->getOperand(1), 1)) {
                    llvm::IRBuilder<> builder(&inst);
                    simplified = builder.CreateICmpEQ(cmp->getOperand(0),
                        llvm::ConstantInt::get(cmp->getOperand(0)->getType(), 0), "ult1_eq0");
                }
            }
            // icmp ugt x, 0 → icmp ne x, 0
            if (!simplified && inst.getOpcode() == llvm::Instruction::ICmp) {
                auto* cmp = llvm::cast<llvm::ICmpInst>(&inst);
                if (cmp->getPredicate() == llvm::ICmpInst::ICMP_UGT &&
                    isConstInt(cmp->getOperand(1), 0)) {
                    llvm::IRBuilder<> builder(&inst);
                    simplified = builder.CreateICmpNE(cmp->getOperand(0),
                        llvm::ConstantInt::get(cmp->getOperand(0)->getType(), 0), "ugt0_ne0");
                }
            }

            // ── ~x + 1 → -x  (two's complement negate pattern) ───────────────
            // In LLVM IR: xor(x,-1) + 1 = -x
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                if (isConstInt(inst.getOperand(1), 1)) {
                    auto* xorInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                    if (xorInst && xorInst->getOpcode() == llvm::Instruction::Xor &&
                        hasOneUse(xorInst)) {
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(xorInst->getOperand(1))) {
                            if (ci->isMinusOne()) {
                                llvm::IRBuilder<> builder(&inst);
                                simplified = builder.CreateNeg(xorInst->getOperand(0), "bitnot_plus1_neg");
                            }
                        }
                    }
                }
            }

            // ── mul(x, -1) → 0 - x  (negate) ────────────────────────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    if (ci->isMinusOne()) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateNeg(inst.getOperand(0), "mul_negone");
                    }
                } else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(0))) {
                    if (ci->isMinusOne()) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateNeg(inst.getOperand(1), "mul_negone2");
                    }
                }
            }

            // ── and(x, -1) → x  (already covered but add variant) ────────────
            // ── or(x, -1) → -1  (already covered) ─────────────────────────────

            // ── (x + y) - x → y  and  (y + x) - x → y ───────────────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                llvm::Value* xv = inst.getOperand(1);
                if (addInst && addInst->getOpcode() == llvm::Instruction::Add) {
                    if (addInst->getOperand(0) == xv) {
                        simplified = addInst->getOperand(1);
                    } else if (addInst->getOperand(1) == xv) {
                        simplified = addInst->getOperand(0);
                    }
                }
            }

            // ── x - (y - x) → 2*x - y → (x << 1) - y ───────────────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                llvm::Value* xv = inst.getOperand(0);
                auto* subInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (subInst && subInst->getOpcode() == llvm::Instruction::Sub &&
                    subInst->getOperand(1) == xv && hasOneUse(subInst)) {
                    llvm::IRBuilder<> builder(&inst);
                    llvm::Value* x2 = builder.CreateShl(xv,
                        llvm::ConstantInt::get(inst.getType(), 1), "x2");
                    simplified = builder.CreateSub(x2, subInst->getOperand(0), "x_minus_y_minus_x");
                }
            }

            // ── a - (a - b) → b ───────────────────────────────────────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                llvm::Value* av = inst.getOperand(0);
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (inner && inner->getOpcode() == llvm::Instruction::Sub &&
                    inner->getOperand(0) == av && hasOneUse(inner)) {
                    simplified = inner->getOperand(1);
                }
            }

            // ── (a - b) + b → a ───────────────────────────────────────────────
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                llvm::Value* bv = inst.getOperand(1);
                if (inner && inner->getOpcode() == llvm::Instruction::Sub &&
                    inner->getOperand(1) == bv && hasOneUse(inner)) {
                    simplified = inner->getOperand(0);
                }
                if (!simplified) {
                    inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                    bv = inst.getOperand(0);
                    if (inner && inner->getOpcode() == llvm::Instruction::Sub &&
                        inner->getOperand(1) == bv && hasOneUse(inner)) {
                        simplified = inner->getOperand(0);
                    }
                }
            }

            // ── (a | b) - (a & b) → a ^ b ────────────────────────────────────
            // Proof: OR = XOR + AND (in terms of bit-counting).
            //   a|b = a^b + a&b  →  (a|b) - (a&b) = a^b  ✓
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                auto* orInst  = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* andInst = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (orInst  && orInst->getOpcode()  == llvm::Instruction::Or  &&
                    andInst && andInst->getOpcode() == llvm::Instruction::And &&
                    hasOneUse(orInst) && hasOneUse(andInst)) {
                    // Operands of OR and AND must be the same pair (in any order)
                    auto* orA  = orInst->getOperand(0);  auto* orB  = orInst->getOperand(1);
                    auto* andA = andInst->getOperand(0); auto* andB = andInst->getOperand(1);
                    if ((orA == andA && orB == andB) || (orA == andB && orB == andA)) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateXor(orA, orB, "or_sub_and_xor");
                    }
                }
            }

            // ── sdiv x, 2^n with nsw → arithmetic-shift + round-up correction ──
            // NOTE: sdiv x, 2^n is NOT simply ashr(x, n) for signed values.  The
            // correct sequence is: ashr(x + ((x >> 63) & (2^n - 1)), n).  LLVM's
            // InstCombine already emits this sequence, so we leave signed power-of-2
            // divisions entirely to LLVM's existing pass rather than risk introducing
            // an unsound transformation here.

            // ── select(cond, x, x) → x ──────────────────────────────────────
            // Identical true/false arms in a select are dead control flow.
            if (!simplified) {
                if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst)) {
                    if (sel->getTrueValue() == sel->getFalseValue()) {
                        simplified = sel->getTrueValue();
                    }
                }
            }

            // ── select(cond, true, false) → zext(cond) ──────────────────────
            if (!simplified) {
                if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst)) {
                    if (isConstInt(sel->getTrueValue(), 1) && isConstInt(sel->getFalseValue(), 0)) {
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateZExt(sel->getCondition(), inst.getType(), "sel_zext");
                    } else if (isConstInt(sel->getTrueValue(), 0) && isConstInt(sel->getFalseValue(), 1)) {
                        // select(cond, 0, 1) → zext(!cond)
                        llvm::IRBuilder<> builder(&inst);
                        llvm::Value* notCond = builder.CreateNot(sel->getCondition(), "sel_not");
                        simplified = builder.CreateZExt(notCond, inst.getType(), "sel_zext_not");
                    }
                }
            }

            // ── or(and(x, mask), and(y, ~mask)) → select ────────────────────
            // Bit-select pattern: pick bits from x where mask=1, y where mask=0
            // This can be lowered to a single blend instruction on x86 (vpblendvb)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Or) {
                auto* and1 = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* and2 = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (and1 && and2 &&
                    and1->getOpcode() == llvm::Instruction::And &&
                    and2->getOpcode() == llvm::Instruction::And &&
                    hasOneUse(and1) && hasOneUse(and2)) {
                    auto c1 = getConstIntValue(and1->getOperand(1));
                    auto c2 = getConstIntValue(and2->getOperand(1));
                    if (c1 && c2 && (*c1 ^ *c2) == -1LL) {
                        // c1 and c2 are complementary masks
                        llvm::IRBuilder<> builder(&inst);
                        // (x & mask) | (y & ~mask) → select on a per-bit basis
                        // For constants, just fold: (x & c1) | (y & c2)
                        // This is already optimal, but establish the equivalence
                        // for further optimization passes to exploit.
                    }
                }
            }

            if (simplified) {
                inst.replaceAllUsesWith(simplified);
                toErase.push_back(&inst);
                count++;
            }
        }
    }

    for (auto* inst : toErase) {
        if (inst->use_empty()) {
            inst->eraseFromParent();
        }
    }

    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Branch simplification
// ─────────────────────────────────────────────────────────────────────────────

/// Convert simple diamond-shaped branch patterns to select instructions.
/// Pattern:
///   if (cond) { x = a; } else { x = b; }
/// Becomes:
///   x = select(cond, a, b)
///
/// This is profitable when the branches are small (1-2 instructions each)
/// and the values are cheap to compute unconditionally.
static unsigned simplifyBranches(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::BranchInst*> toProcess;

    // Collect conditional branches for processing
    for (auto& bb : func) {
        auto* br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator());
        if (br && br->isConditional()) {
            toProcess.push_back(br);
        }
    }

    for (auto* br : toProcess) {
        llvm::BasicBlock* trueBB = br->getSuccessor(0);
        llvm::BasicBlock* falseBB = br->getSuccessor(1);

        // Both branches must have exactly one successor (the merge block)
        auto* trueTerm = llvm::dyn_cast<llvm::BranchInst>(trueBB->getTerminator());
        auto* falseTerm = llvm::dyn_cast<llvm::BranchInst>(falseBB->getTerminator());
        if (!trueTerm || !falseTerm) continue;
        if (!trueTerm->isUnconditional() || !falseTerm->isUnconditional()) continue;
        if (trueTerm->getSuccessor(0) != falseTerm->getSuccessor(0)) continue;

        llvm::BasicBlock* mergeBB = trueTerm->getSuccessor(0);

        // Each branch should have at most 2 non-terminator instructions
        unsigned trueInstCount = 0, falseInstCount = 0;
        for (auto& inst : *trueBB) {
            if (!inst.isTerminator()) trueInstCount++;
        }
        for (auto& inst : *falseBB) {
            if (!inst.isTerminator()) falseInstCount++;
        }
        if (trueInstCount > 2 || falseInstCount > 2) continue;

        // Check that the branches don't have side effects
        bool hasSideEffects = false;
        for (auto& inst : *trueBB) {
            if (inst.isTerminator()) continue;
            if (inst.mayHaveSideEffects()) { hasSideEffects = true; break; }
        }
        for (auto& inst : *falseBB) {
            if (inst.isTerminator()) continue;
            if (inst.mayHaveSideEffects()) { hasSideEffects = true; break; }
        }
        if (hasSideEffects) continue;

        // Look for PHI nodes in the merge block that select between true/false values
        for (auto& phi : mergeBB->phis()) {
            llvm::Value* trueVal = phi.getIncomingValueForBlock(trueBB);
            llvm::Value* falseVal = phi.getIncomingValueForBlock(falseBB);

            if (!trueVal || !falseVal) continue;

            // Create select in the original block
            llvm::IRBuilder<> builder(br);
            llvm::Value* sel = builder.CreateSelect(br->getCondition(), trueVal, falseVal,
                                                     phi.getName() + ".sel");
            phi.replaceAllUsesWith(sel);
            count++;
        }
    }

    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Synthesis engine
// ─────────────────────────────────────────────────────────────────────────────

/// Simple enumerative synthesis: try to find a cheaper equivalent for
/// a single instruction using a small library of templates.
bool synthesizeReplacement(llvm::Instruction* inst, const SynthesisConfig& config) {
    if (!inst->getType()->isIntegerTy()) return false;

    llvm::IRBuilder<> builder(inst);
    llvm::Value* replacement = nullptr;
    double oldCost = instructionCost(inst);

    // Template 1: Expensive multiply by constant → shift+add/sub sequence
    if (inst->getOpcode() == llvm::Instruction::Mul) {
        auto cval = getConstIntValue(inst->getOperand(1));
        llvm::Value* var = inst->getOperand(0);
        if (!cval) {
            cval = getConstIntValue(inst->getOperand(0));
            var = inst->getOperand(1);
        }
        if (!cval) return false;

        int64_t c = *cval;
        if (c <= 0) return false;

        // x * 3 → (x << 1) + x  (cost: 2 vs 3)
        if (c == 3) {
            llvm::Value* shl = builder.CreateShl(var, 1);
            replacement = builder.CreateAdd(shl, var, "mul3");
        }
        // x * 5 → (x << 2) + x  (cost: 2 vs 3)
        else if (c == 5) {
            llvm::Value* shl = builder.CreateShl(var, 2);
            replacement = builder.CreateAdd(shl, var, "mul5");
        }
        // x * 7 → (x << 3) - x  (cost: 2 vs 3)
        else if (c == 7) {
            llvm::Value* shl = builder.CreateShl(var, 3);
            replacement = builder.CreateSub(shl, var, "mul7");
        }
        // x * 9 → (x << 3) + x  (cost: 2 vs 3)
        else if (c == 9) {
            llvm::Value* shl = builder.CreateShl(var, 3);
            replacement = builder.CreateAdd(shl, var, "mul9");
        }
        // x * 15 → (x << 4) - x  (cost: 2 vs 3)
        else if (c == 15) {
            llvm::Value* shl = builder.CreateShl(var, 4);
            replacement = builder.CreateSub(shl, var, "mul15");
        }
        // x * 17 → (x << 4) + x  (cost: 2 vs 3)
        else if (c == 17) {
            llvm::Value* shl = builder.CreateShl(var, 4);
            replacement = builder.CreateAdd(shl, var, "mul17");
        }
        // x * 31 → (x << 5) - x  (cost: 2 vs 3)
        else if (c == 31) {
            llvm::Value* shl = builder.CreateShl(var, 5);
            replacement = builder.CreateSub(shl, var, "mul31");
        }
        // x * 33 → (x << 5) + x  (cost: 2 vs 3)
        else if (c == 33) {
            llvm::Value* shl = builder.CreateShl(var, 5);
            replacement = builder.CreateAdd(shl, var, "mul33");
        }
        // x * 63 → (x << 6) - x  (cost: 2 vs 3)
        else if (c == 63) {
            llvm::Value* shl = builder.CreateShl(var, 6);
            replacement = builder.CreateSub(shl, var, "mul63");
        }
        // x * 65 → (x << 6) + x  (cost: 2 vs 3)
        else if (c == 65) {
            llvm::Value* shl = builder.CreateShl(var, 6);
            replacement = builder.CreateAdd(shl, var, "mul65");
        }
        // x * 127 → (x << 7) - x  (cost: 2 vs 3)
        else if (c == 127) {
            llvm::Value* shl = builder.CreateShl(var, 7);
            replacement = builder.CreateSub(shl, var, "mul127");
        }
        // x * 255 → (x << 8) - x  (cost: 2 vs 3)
        else if (c == 255) {
            llvm::Value* shl = builder.CreateShl(var, 8);
            replacement = builder.CreateSub(shl, var, "mul255");
        }
        // x * 129 → (x << 7) + x  (cost: 2 vs 3)
        else if (c == 129) {
            llvm::Value* shl = builder.CreateShl(var, 7);
            replacement = builder.CreateAdd(shl, var, "mul129");
        }
        // x * 257 → (x << 8) + x  (cost: 2 vs 3)
        else if (c == 257) {
            llvm::Value* shl = builder.CreateShl(var, 8);
            replacement = builder.CreateAdd(shl, var, "mul257");
        }
        // x * 511 → (x << 9) - x  (cost: 2 vs 3)
        else if (c == 511) {
            llvm::Value* shl = builder.CreateShl(var, 9);
            replacement = builder.CreateSub(shl, var, "mul511");
        }
        // x * 513 → (x << 9) + x  (cost: 2 vs 3)
        else if (c == 513) {
            llvm::Value* shl = builder.CreateShl(var, 9);
            replacement = builder.CreateAdd(shl, var, "mul513");
        }
        // x * 1023 → (x << 10) - x  (cost: 2 vs 3)
        else if (c == 1023) {
            llvm::Value* shl = builder.CreateShl(var, 10);
            replacement = builder.CreateSub(shl, var, "mul1023");
        }
        // x * 1025 → (x << 10) + x  (cost: 2 vs 3)
        else if (c == 1025) {
            llvm::Value* shl = builder.CreateShl(var, 10);
            replacement = builder.CreateAdd(shl, var, "mul1025");
        }
        // General: x * (2^n + 1) → (x << n) + x  for any n
        // General: x * (2^n - 1) → (x << n) - x  for any n
        // These are always 2 instructions (cost 2.0) vs mul (cost 3.0)
        else {
            // Check if c = 2^n + 1
            int64_t cm1 = c - 1;
            if (cm1 > 0 && (cm1 & (cm1 - 1)) == 0) {
                unsigned n = llvm::Log2_64(static_cast<uint64_t>(cm1));
                llvm::Value* shl = builder.CreateShl(var, n);
                replacement = builder.CreateAdd(shl, var,
                    "mul" + std::to_string(c));
            }
            // Check if c = 2^n - 1
            else {
                int64_t cp1 = c + 1;
                if (cp1 > 0 && (cp1 & (cp1 - 1)) == 0) {
                    unsigned n = llvm::Log2_64(static_cast<uint64_t>(cp1));
                    llvm::Value* shl = builder.CreateShl(var, n);
                    replacement = builder.CreateSub(shl, var,
                        "mul" + std::to_string(c));
                }
            }
        }
    }

    // Template 2: Division by power-of-2 constant → shift
    if (inst->getOpcode() == llvm::Instruction::UDiv) {
        auto cval = getConstIntValue(inst->getOperand(1));
        if (cval && *cval > 0 && (*cval & (*cval - 1)) == 0) {
            // It's a power of 2
            unsigned shift = 0;
            int64_t v = *cval;
            while (v > 1) { v >>= 1; shift++; }
            replacement = builder.CreateLShr(inst->getOperand(0),
                llvm::ConstantInt::get(inst->getType(), shift), "udiv_shr");
        }
    }

    // Template 3: Unsigned remainder by power-of-2 → AND mask
    if (inst->getOpcode() == llvm::Instruction::URem) {
        auto cval = getConstIntValue(inst->getOperand(1));
        if (cval && *cval > 0 && (*cval & (*cval - 1)) == 0) {
            replacement = builder.CreateAnd(inst->getOperand(0),
                llvm::ConstantInt::get(inst->getType(), *cval - 1), "urem_and");
        }
    }

    if (replacement) {
        double newCost = 0.0;
        // Estimate cost of replacement (rough: count the instructions we created)
        if (auto* ri = llvm::dyn_cast<llvm::Instruction>(replacement)) {
            newCost = instructionCost(ri);
            // Add costs of any intermediate instructions
            for (unsigned i = 0; i < ri->getNumOperands(); ++i) {
                if (auto* opInst = llvm::dyn_cast<llvm::Instruction>(ri->getOperand(i))) {
                    if (opInst->getParent() == ri->getParent() &&
                        opInst != inst) {
                        newCost += instructionCost(opInst);
                    }
                }
            }
        }

        if (newCost < oldCost * config.costThreshold) {
            inst->replaceAllUsesWith(replacement);
            return true;
        }
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Dead code elimination
// ─────────────────────────────────────────────────────────────────────────────

/// Remove dead (unused) instructions that were left behind by other
/// superoptimizer phases.  Returns the number of instructions removed.
static unsigned eliminateDeadCode(llvm::Function& func) {
    unsigned count = 0;
    bool changed = true;

    // Iterate until convergence — removing one instruction may make others dead.
    while (changed) {
        changed = false;
        std::vector<llvm::Instruction*> toErase;

        for (auto& bb : func) {
            for (auto& inst : bb) {
                if (inst.isTerminator()) continue;
                if (inst.use_empty() && !inst.mayHaveSideEffects()) {
                    toErase.push_back(&inst);
                }
            }
        }

        for (auto* inst : toErase) {
            inst->eraseFromParent();
            count++;
            changed = true;
        }
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main superoptimizer entry points
// ─────────────────────────────────────────────────────────────────────────────

SuperoptimizerStats superoptimizeFunction(llvm::Function& func,
                                           const SuperoptimizerConfig& config) {
    SuperoptimizerStats stats;
    if (func.isDeclaration()) return stats;

    // Phase 1: Idiom recognition and replacement
    if (config.enableIdiomRecognition) {
        for (auto& bb : func) {
            auto idioms = detectIdioms(bb);
            for (auto& match : idioms) {
                if (replaceIdiom(match)) {
                    stats.idiomsReplaced++;
                }
            }
        }
    }

    // Phase 2: Algebraic simplification
    if (config.enableAlgebraic) {
        stats.algebraicSimplified = applyAlgebraicSimplifications(func);
    }

    // Phase 3: Branch simplification (branch-to-select)
    if (config.enableBranchOpt) {
        stats.branchesSimplified = simplifyBranches(func);
    }

    // Phase 4: Enumerative synthesis on remaining expensive instructions
    if (config.enableSynthesis) {
        std::vector<llvm::Instruction*> candidates;
        for (auto& bb : func) {
            for (auto& inst : bb) {
                if (instructionCost(&inst) >= 3.0) {
                    candidates.push_back(&inst);
                }
            }
        }
        for (auto* inst : candidates) {
            if (synthesizeReplacement(inst, config.synthesis)) {
                stats.synthReplacements++;
            }
        }
    }

    // Phase 5: Dead code elimination — clean up instructions made dead by
    // previous phases (idiom replacement, algebraic simplification, synthesis).
    if (config.enableDeadCodeElim) {
        stats.deadCodeEliminated = eliminateDeadCode(func);
    }

    return stats;
}

SuperoptimizerStats superoptimizeModule(llvm::Module& module,
                                         const SuperoptimizerConfig& config) {
    SuperoptimizerStats total;
    for (auto& func : module) {
        auto stats = superoptimizeFunction(func, config);
        total.idiomsReplaced += stats.idiomsReplaced;
        total.synthReplacements += stats.synthReplacements;
        total.branchesSimplified += stats.branchesSimplified;
        total.algebraicSimplified += stats.algebraicSimplified;
        total.deadCodeEliminated += stats.deadCodeEliminated;
    }
    return total;
}

} // namespace superopt
} // namespace omscript
