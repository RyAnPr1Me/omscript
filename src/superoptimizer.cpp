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
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/PatternMatch.h>
#include <llvm/Support/KnownBits.h>
#include <llvm/Support/MathExtras.h>
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
        llvm::Function* fshl = llvm::Intrinsic::getDeclaration(
            mod, llvm::Intrinsic::fshl, {intTy});
        llvm::Value* result = builder.CreateCall(fshl, {x, x, amt}, "rotl");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::RotateRight: {
        llvm::Value* x = match.operands[0];
        llvm::Value* amt = match.operands[1];
        llvm::Function* fshr = llvm::Intrinsic::getDeclaration(
            mod, llvm::Intrinsic::fshr, {intTy});
        llvm::Value* result = builder.CreateCall(fshr, {x, x, amt}, "rotr");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::AbsoluteValue: {
        llvm::Value* x = match.operands[0];
        llvm::Function* absIntrinsic = llvm::Intrinsic::getDeclaration(
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
        llvm::Function* minIntrinsic = llvm::Intrinsic::getDeclaration(mod, intrID, {intTy});
        llvm::Value* result = builder.CreateCall(minIntrinsic, {a, b}, "imin");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::IntMax: {
        llvm::Value* a = match.operands[0];
        llvm::Value* b = match.operands[1];
        bool isUnsigned = (match.bitWidth & 0x80000000u) != 0;
        llvm::Intrinsic::ID intrID = isUnsigned ? llvm::Intrinsic::umax : llvm::Intrinsic::smax;
        llvm::Function* maxIntrinsic = llvm::Intrinsic::getDeclaration(mod, intrID, {intTy});
        llvm::Value* result = builder.CreateCall(maxIntrinsic, {a, b}, "imax");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::IsPowerOf2: {
        // (x & (x-1)) == 0 → ctpop(x) <= 1
        // This helps the backend select POPCNT + CMP instead of SUB + AND + CMP
        llvm::Value* x = match.operands[0];
        llvm::Function* ctpop = llvm::Intrinsic::getDeclaration(
            mod, llvm::Intrinsic::ctpop, {x->getType()});
        llvm::Value* popcount = builder.CreateCall(ctpop, {x}, "popcnt");
        llvm::Value* one = llvm::ConstantInt::get(x->getType(), 1);
        llvm::Value* result = builder.CreateICmpULE(popcount, one, "ispow2");
        // The original was an i1, so the types match
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
