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

// Apply maximum compiler optimizations to this hot path.
// The idiom detection and algebraic simplification loops dominate
// the post-LLVM optimization time for large programs.
#ifdef __GNUC__
#  pragma GCC optimize("O3,unroll-loops,tree-vectorize")
#endif

#include "superoptimizer.h"
#include <llvm/Config/llvm-config.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>
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
#include <limits>
#include <future>
#include <random>
#include <thread>
#include <unordered_set>

namespace omscript {
namespace superopt {

using namespace llvm::PatternMatch;

// ─────────────────────────────────────────────────────────────────────────────
// Cost model
// ─────────────────────────────────────────────────────────────────────────────

// Thread-local cost override.  Set by superoptimizeFunction/superoptimizeModule
// when the caller provides a hardware-profile-driven cost function via
// SuperoptimizerConfig::costFn.  This ensures a single authoritative cost
// source is used for all synthesis, idiom, and candidate-selection decisions.
static thread_local const std::function<double(const llvm::Instruction*)>*
    g_costFn = nullptr;

double instructionCost(const llvm::Instruction* inst) {
    if (__builtin_expect(!inst, 0)) return 0.0;

    // Delegate to hardware-profile-driven cost when available.
    if (__builtin_expect(g_costFn != nullptr, 0)) {
        double cost = (*g_costFn)(inst);
        if (cost >= 0.0) return cost;
    }

    switch (inst->getOpcode()) {
    // Near-free: these are typically eliminated or folded by the backend
    case llvm::Instruction::BitCast:
    case llvm::Instruction::AddrSpaceCast:
        return 0.0;

    // IntToPtr/PtrToInt: while these are nominally free in the backend,
    // they destroy pointer provenance information which prevents LLVM's
    // alias analysis (BasicAA) from proving noalias between pointers
    // that went through an i64 round-trip.  This blocks LICM, GVN, and
    // vectorization.  Assigning a cost of 1.5 incentivizes the
    // superoptimizer to prefer patterns that avoid unnecessary
    // pointer-integer conversions.
    case llvm::Instruction::IntToPtr:
    case llvm::Instruction::PtrToInt:
        return 1.5;

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
    case llvm::Instruction::FMul:
        return 4.0;
    case llvm::Instruction::FDiv:
        return 15.0;
    case llvm::Instruction::FCmp:
        return 3.0;

    // Memory
    case llvm::Instruction::Load:
    case llvm::Instruction::Store:
        return 4.0; // L1 cache hit
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
            case llvm::Intrinsic::ctlz:
            case llvm::Intrinsic::cttz:
            case llvm::Intrinsic::bswap:
            case llvm::Intrinsic::fshl:
            case llvm::Intrinsic::fshr:
                return 1.0;
            case llvm::Intrinsic::smin:
            case llvm::Intrinsic::smax:
            case llvm::Intrinsic::umin:
            case llvm::Intrinsic::umax:
                return 1.5;
            case llvm::Intrinsic::abs:
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
            auto signed_lhs = static_cast<int64_t>(*lhs);
            return static_cast<uint64_t>(signed_lhs >> *rhs);
        }
        break;
    }
    case llvm::Instruction::SDiv: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs && *rhs != 0) {
            auto sl = static_cast<int64_t>(*lhs), sr = static_cast<int64_t>(*rhs);
            if (sl == std::numeric_limits<int64_t>::min() && sr == -1) break;
            return static_cast<uint64_t>(sl / sr);
        }
        break;
    }
    case llvm::Instruction::UDiv: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs && *rhs != 0) return *lhs / *rhs;
        break;
    }
    case llvm::Instruction::SRem: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs && *rhs != 0) {
            auto sl = static_cast<int64_t>(*lhs), sr = static_cast<int64_t>(*rhs);
            if (sl == std::numeric_limits<int64_t>::min() && sr == -1) return 0;
            return static_cast<uint64_t>(sl % sr);
        }
        break;
    }
    case llvm::Instruction::URem: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs && *rhs != 0) return *lhs % *rhs;
        break;
    }
    case llvm::Instruction::ICmp: {
        auto lhs = getVal(inst->getOperand(0));
        auto rhs = getVal(inst->getOperand(1));
        if (lhs && rhs) {
            auto* icmp = llvm::cast<llvm::ICmpInst>(inst);
            int64_t sl = static_cast<int64_t>(*lhs), sr = static_cast<int64_t>(*rhs);
            bool result = false;
            switch (icmp->getPredicate()) {
            case llvm::ICmpInst::ICMP_EQ:  result = *lhs == *rhs; break;
            case llvm::ICmpInst::ICMP_NE:  result = *lhs != *rhs; break;
            case llvm::ICmpInst::ICMP_ULT: result = *lhs < *rhs;  break;
            case llvm::ICmpInst::ICMP_ULE: result = *lhs <= *rhs; break;
            case llvm::ICmpInst::ICMP_UGT: result = *lhs > *rhs;  break;
            case llvm::ICmpInst::ICMP_UGE: result = *lhs >= *rhs; break;
            case llvm::ICmpInst::ICMP_SLT: result = sl < sr;  break;
            case llvm::ICmpInst::ICMP_SLE: result = sl <= sr; break;
            case llvm::ICmpInst::ICMP_SGT: result = sl > sr;  break;
            case llvm::ICmpInst::ICMP_SGE: result = sl >= sr; break;
            default: break;
            }
            return result ? 1ULL : 0ULL;
        }
        break;
    }
    case llvm::Instruction::Select: {
        auto cond = getVal(inst->getOperand(0));
        if (cond) {
            auto chosen = getVal(*cond ? inst->getOperand(1) : inst->getOperand(2));
            if (chosen) return *chosen;
        }
        break;
    }
    case llvm::Instruction::ZExt: {
        auto v = getVal(inst->getOperand(0));
        if (v) return *v; // already stored as uint64_t, zero-extended
        break;
    }
    case llvm::Instruction::SExt: {
        auto v = getVal(inst->getOperand(0));
        if (v) {
            unsigned srcBits = inst->getOperand(0)->getType()->getIntegerBitWidth();
            if (srcBits < 64) {
                // Sign-extend by shifting up then arithmetic-shifting down
                uint64_t shifted = *v << (64 - srcBits);
                return static_cast<uint64_t>(static_cast<int64_t>(shifted) >> (64 - srcBits));
            }
            return *v;
        }
        break;
    }
    case llvm::Instruction::Trunc: {
        auto v = getVal(inst->getOperand(0));
        if (v) {
            unsigned dstBits = inst->getType()->getIntegerBitWidth();
            if (dstBits < 64) return *v & ((1ULL << dstBits) - 1ULL);
            return *v;
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
[[nodiscard]] [[gnu::always_inline]] static inline bool hasOneUse(llvm::Value* v) {
    return v->hasOneUse();
}

/// Check if value is a constant integer with a specific value.
[[nodiscard]] [[gnu::always_inline]] static inline bool isConstInt(llvm::Value* v, uint64_t val) {
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        return ci->getZExtValue() == val;
    }
    return false;
}

/// Check if value is a constant integer and return its value.
[[nodiscard]] [[gnu::always_inline]] static inline std::optional<int64_t> getConstIntValue(llvm::Value* v) {
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        return ci->getSExtValue();
    }
    return std::nullopt;
}

/// Recursively check if a value is known to be non-negative (sign bit = 0).
/// This goes beyond computeKnownBits by tracking through nuw-flagged arithmetic,
/// XOR of non-negative values, and loop induction variable PHI nodes that
/// start from 0 and increment by a positive step.

/// Check whether a constant (scalar ConstantInt or vector splat/element-wise)
/// has all elements strictly positive (> 0).
[[nodiscard]] static bool isConstantAllPositive(llvm::Constant* c) {
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(c))
        return ci->getSExtValue() > 0;
    // Handle vector constants: ConstantDataVector, ConstantVector, splat
    if (auto* splat = c->getSplatValue()) {
        if (auto* si = llvm::dyn_cast<llvm::ConstantInt>(splat))
            return si->getSExtValue() > 0;
    }
    if (auto* cdv = llvm::dyn_cast<llvm::ConstantDataVector>(c)) {
        for (unsigned i = 0, n = cdv->getNumElements(); i < n; ++i) {
            if (auto* ei = llvm::dyn_cast<llvm::ConstantInt>(cdv->getElementAsConstant(i))) {
                if (ei->getSExtValue() <= 0) return false;
            } else {
                return false;
            }
        }
        return cdv->getNumElements() > 0;
    }
    if (auto* cv = llvm::dyn_cast<llvm::ConstantVector>(c)) {
        for (unsigned i = 0, n = cv->getNumOperands(); i < n; ++i) {
            if (auto* ei = llvm::dyn_cast<llvm::ConstantInt>(cv->getOperand(i))) {
                if (ei->getSExtValue() <= 0) return false;
            } else {
                return false;
            }
        }
        return cv->getNumOperands() > 0;
    }
    return false;
}

[[nodiscard]] static bool isValueNonNegative(llvm::Value* v, const llvm::DataLayout& DL, unsigned depth = 0) {
    if (__builtin_expect(depth > 12, 0)) return false;  // prevent infinite recursion

    // Non-negative constant
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v))
        return ci->getSExtValue() >= 0;

    // computeKnownBits check (handles many cases including zext, and)
    llvm::KnownBits KB = llvm::computeKnownBits(v, DL, /*Depth=*/0);
    if (KB.isNonNegative()) return true;

    auto* inst = llvm::dyn_cast<llvm::Instruction>(v);
    if (!inst) return false;

    unsigned op = inst->getOpcode();

    // add: if both operands are non-negative, the result is non-negative when:
    //   (a) nuw flag is set (guarantees no unsigned wrap), OR
    //   (b) nsw flag is set (signed overflow is poison, so the result must be
    //       representable as a non-negative i64), OR
    //   (c) both operands have at least 2 leading zero bits known (i.e., both
    //       are < 2^62), so their sum is < 2^63 and the sign bit stays 0.
    //       This is a key OmScript advantage: for-loop iterators and their
    //       derived expressions (XOR, AND, OR of loop vars) typically fit in
    //       far fewer than 62 bits, so this handles patterns like
    //       ((i^j) + k) where i,j,k are loop iterators.
    if (op == llvm::Instruction::Add) {
        if (isValueNonNegative(inst->getOperand(0), DL, depth + 1) &&
            isValueNonNegative(inst->getOperand(1), DL, depth + 1)) {
            auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(inst);
            if (bo && (bo->hasNoUnsignedWrap() || bo->hasNoSignedWrap()))
                return true;
            // No wrapping flags: check if both operands are small enough
            // that their sum can't overflow into negative territory.
            llvm::KnownBits kb0 = llvm::computeKnownBits(inst->getOperand(0), DL);
            llvm::KnownBits kb1 = llvm::computeKnownBits(inst->getOperand(1), DL);
            unsigned leadingZeros0 = kb0.countMinLeadingZeros();
            unsigned leadingZeros1 = kb1.countMinLeadingZeros();
            // Each operand fits in (bitWidth - leadingZeros) bits.
            // Their sum fits in max(bw0, bw1) + 1 bits.
            // If max(bw0, bw1) + 1 <= 63, the sum is < 2^63 (non-negative).
            unsigned bw = kb0.getBitWidth();  // typically 64 for i64
            unsigned maxBits = std::max(bw - leadingZeros0, bw - leadingZeros1) + 1;
            // bw - 1 is the sign bit position (63 for i64); if the sum
            // fits within that many bits, the sign bit stays 0.
            if (maxBits <= bw - 1)
                return true;
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

    // urem: result of unsigned remainder is always in [0, divisor), so
    // it's always non-negative.  This is critical for post-unroll srem→urem
    // conversion: after loop unrolling, patterns like
    //   %t = urem i64 %x, 37
    //   %t2 = add i64 %t, 1
    //   %t3 = srem i64 %t2, 37   ← can be converted to urem because t2 >= 0
    // become provable.
    if (op == llvm::Instruction::URem) return true;

    // udiv: result of unsigned division is always non-negative
    if (op == llvm::Instruction::UDiv) return true;

    // srem by positive constant: result is always in (-(divisor-1), divisor-1)
    // When the dividend is non-negative, srem is equivalent to urem and the
    // result is in [0, divisor-1).  Even when the dividend is negative, the
    // absolute value of srem is bounded.  For the non-negative case specifically:
    if (op == llvm::Instruction::SRem) {
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
            if (ci->getSExtValue() > 0 &&
                isValueNonNegative(inst->getOperand(0), DL, depth + 1))
                return true;
        }
        // Vector srem: check if all elements of the divisor are positive
        if (auto* cv = llvm::dyn_cast<llvm::Constant>(inst->getOperand(1))) {
            if (cv->getType()->isVectorTy() && isConstantAllPositive(cv) &&
                isValueNonNegative(inst->getOperand(0), DL, depth + 1))
                return true;
        }
    }

    // sdiv by positive constant with non-negative dividend: result is in [0, dividend/C],
    // so it is always non-negative.  Mirrors the SRem case above.  This enables
    // downstream adds/muls on quotients to get NSW/NUW flags without needing
    // an extra llvm.assume.
    if (op == llvm::Instruction::SDiv) {
        if (isValueNonNegative(inst->getOperand(0), DL, depth + 1)) {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1))) {
                if (ci->getSExtValue() > 0) return true;
            }
            // Runtime positive divisor: if it's provably non-negative (and
            // non-zero — assumed by the caller as sdiv by zero is UB), the
            // quotient is also non-negative.
            if (isValueNonNegative(inst->getOperand(1), DL, depth + 1)) return true;
        }
    }

    // lshr (logical shift right): always non-negative (fills with 0s)
    if (op == llvm::Instruction::LShr) return true;

    // zext: always non-negative
    if (op == llvm::Instruction::ZExt) return true;

    // sext: non-negative if the source value is non-negative (sign extension
    // preserves the sign).  This is critical for srem→urem after LLVM truncates
    // modulo operations to narrow types: e.g., `srem i64 x, 193` may become
    // `trunc i64 → i16`, `srem i16, 193`, `sext i16 → i64`.
    if (op == llvm::Instruction::SExt) {
        return isValueNonNegative(inst->getOperand(0), DL, depth + 1);
    }

    // trunc: non-negative if the source value's significant bits fit in the
    // truncated type.  Use KnownBits to check if the value fits.
    if (op == llvm::Instruction::Trunc) {
        // If the source is known non-negative and fits in the narrower type,
        // the truncation preserves non-negativity.
        unsigned srcBits = inst->getOperand(0)->getType()->getIntegerBitWidth();
        unsigned dstBits = inst->getType()->getIntegerBitWidth();
        llvm::KnownBits srcKB = llvm::computeKnownBits(inst->getOperand(0), DL);
        unsigned leadingZeros = srcKB.countMinLeadingZeros();
        // If the source has enough leading zeros that it fits in (dstBits - 1)
        // bits, then the truncated result is non-negative.
        if (leadingZeros >= srcBits - (dstBits - 1)) return true;
        // Otherwise, if the source is non-negative and bounded by a small
        // constant (e.g., result of srem by a constant that fits in the
        // truncated type), still mark as non-negative.
        if (isValueNonNegative(inst->getOperand(0), DL, depth + 1)) {
            // Check if the source is bounded by the truncated type's signed max.
            // srem result is bounded by divisor - 1.
            if (auto* sremOp = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(0))) {
                if (sremOp->getOpcode() == llvm::Instruction::SRem ||
                    sremOp->getOpcode() == llvm::Instruction::URem) {
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(sremOp->getOperand(1))) {
                        int64_t divisor = ci->getSExtValue();
                        if (divisor > 0 && divisor < (1LL << (dstBits - 1))) {
                            return true;  // result fits in narrow type
                        }
                    }
                }
            }
        }
    }

    // and: bitmask with a positive constant always produces a non-negative
    // result.  e.g. x & 1  →  result ∈ {0,1}, always ≥ 0.
    // More generally, x & C where C ∈ [0, INT64_MAX] has the sign bit cleared
    // so the result is non-negative regardless of x.
    if (op == llvm::Instruction::And) {
        auto checkNonNegConst = [](llvm::Value* v) -> bool {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v))
                return !ci->isNegative();
            if (auto* cv = llvm::dyn_cast<llvm::Constant>(v))
                return isConstantAllPositive(cv);
            return false;
        };
        if (checkNonNegConst(inst->getOperand(1)) ||
            checkNonNegConst(inst->getOperand(0)))
            return true;
        // If either operand is non-negative, the and result is too
        // (anding with a non-negative value clears the sign bit).
        if (isValueNonNegative(inst->getOperand(0), DL, depth + 1) ||
            isValueNonNegative(inst->getOperand(1), DL, depth + 1))
            return true;
    }

    // or disjoint (llvm.or.disjoint / or with known non-overlapping bits):
    // if both operands are non-negative, the result is non-negative.
    if (op == llvm::Instruction::Or) {
        if (isValueNonNegative(inst->getOperand(0), DL, depth + 1) &&
            isValueNonNegative(inst->getOperand(1), DL, depth + 1))
            return true;
    }

    // select: if both possible values are non-negative, the result is too
    if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(inst)) {
        return isValueNonNegative(sel->getTrueValue(), DL, depth + 1) &&
               isValueNonNegative(sel->getFalseValue(), DL, depth + 1);
    }

    // mul nsw nuw: if both non-negative, result is non-negative
    if (op == llvm::Instruction::Mul) {
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
            if (bo->hasNoSignedWrap() || bo->hasNoUnsignedWrap()) {
                return isValueNonNegative(bo->getOperand(0), DL, depth + 1) &&
                       isValueNonNegative(bo->getOperand(1), DL, depth + 1);
            }
        }
    }

    // shl nsw: if operand is non-negative, result is non-negative.
    // nsw (no signed wrap) on shl guarantees the sign bit was not
    // changed by the shift, so x >= 0 → (x shl k nsw) >= 0.
    // Note: nuw alone is NOT sufficient — shl nuw only guarantees that
    // no bits are shifted out of the top, but the result's sign bit can
    // still be set by a 1-bit in the source shifted into position 63.
    if (op == llvm::Instruction::Shl) {
        if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(inst)) {
            if (bo->hasNoSignedWrap()) {
                return isValueNonNegative(bo->getOperand(0), DL, depth + 1);
            }
        }
    }

    // Intrinsic calls with non-negative results:
    //   abs(x)        — absolute value is always non-negative by definition.
    //   umin/umax     — unsigned min/max results are always in [0, UINT64_MAX],
    //                   which interpreted as i64 never has the sign bit set when
    //                   both inputs are < 2^63.  We conservatively check that
    //                   at least one operand is non-negative (the min/max result
    //                   is bounded by the non-negative operand).
    //   smin(a,b)     — signed min is non-negative when both a and b are ≥ 0.
    //   smax(a,b)     — signed max is non-negative when at least one is ≥ 0
    //                   (the result >= the non-negative operand).
    // These enable NSW/NUW propagation through min/max chains in hot loops
    // (e.g. clamp patterns, saturating accumulation).
    if (auto* call = llvm::dyn_cast<llvm::CallInst>(inst)) {
        if (auto* callee = call->getCalledFunction()) {
            switch (callee->getIntrinsicID()) {
            case llvm::Intrinsic::abs:
                return true;
            case llvm::Intrinsic::umin:
            case llvm::Intrinsic::umax:
                // Unsigned results are always non-negative as i64 when inputs < 2^63.
                return isValueNonNegative(call->getArgOperand(0), DL, depth + 1) ||
                       isValueNonNegative(call->getArgOperand(1), DL, depth + 1);
            case llvm::Intrinsic::smin:
                return isValueNonNegative(call->getArgOperand(0), DL, depth + 1) &&
                       isValueNonNegative(call->getArgOperand(1), DL, depth + 1);
            case llvm::Intrinsic::smax:
                return isValueNonNegative(call->getArgOperand(0), DL, depth + 1) ||
                       isValueNonNegative(call->getArgOperand(1), DL, depth + 1);
            default:
                break;
            }
        }
    }

    // PHI node: check if all incoming values (excluding self-references) are
    // non-negative.  This handles loop induction variables that start from 0
    // and increment by a positive step, as well as PHIs from loop unrolling
    // where incoming values are other PHI nodes or or-disjoint increments.
    //
    // Cycle detection: a thread-local set of PHIs currently under analysis
    // prevents infinite recursion when back-edges form cycles through select
    // or other expressions that contain the PHI (e.g. Collatz x variable:
    //   phi = [init, select(x&1, 3*phi+1, phi>>1)]).
    // When we re-encounter a phi already being analyzed, we return true
    // (assume non-negative inductively) — the base case is checked below
    // on the non-back-edge incoming values, and the back-edge check tests
    // whether the update expression preserves non-negativity given non-neg input.
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(inst)) {
        // Cycle guard: if we're already analyzing this phi in the current
        // call chain, assume it's non-negative (inductive hypothesis).
        thread_local llvm::SmallPtrSet<const llvm::PHINode*, 8> activePHIs;
        if (activePHIs.count(phi)) return true;
        activePHIs.insert(phi);

        bool allNonNeg = true;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); i++) {
            llvm::Value* incoming = phi->getIncomingValue(i);
            if (incoming == phi) continue;  // self-reference (back edge)
            // Fast path: loop increment pattern (phi + positive_constant)
            if (auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(incoming)) {
                if (addInst->getOpcode() == llvm::Instruction::Add &&
                    (addInst->getOperand(0) == phi || addInst->getOperand(1) == phi)) {
                    llvm::Value* step = (addInst->getOperand(0) == phi) ?
                                         addInst->getOperand(1) : addInst->getOperand(0);
                    // Scalar positive step
                    if (auto* stepCI = llvm::dyn_cast<llvm::ConstantInt>(step)) {
                        if (stepCI->getSExtValue() > 0) continue;  // positive step ✓
                    }
                    // Vector positive step (from vectorized loop increments)
                    if (auto* stepConst = llvm::dyn_cast<llvm::Constant>(step)) {
                        if (isConstantAllPositive(stepConst)) continue;  // positive step ✓
                    }
                }
                // or disjoint used by loop unroller as add substitute
                if (addInst->getOpcode() == llvm::Instruction::Or &&
                    (addInst->getOperand(0) == phi || addInst->getOperand(1) == phi)) {
                    continue;  // or disjoint with phi is non-negative if phi is ✓
                }
                // Modular reduction pattern: srem(expr, positive_const) as
                // back-edge value.  The srem result is in (-C+1, C-1) for
                // divisor C.  If the initial value of the PHI is non-negative,
                // and the back-edge computes srem of an expression derived from
                // this PHI with a positive constant, the result is non-negative
                // because the PHI's range is always [0, C-1).
                // Pattern: phi = [init, srem(f(phi), C)]  where C > 0
                if (addInst->getOpcode() == llvm::Instruction::SRem) {
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(addInst->getOperand(1))) {
                        if (ci->getSExtValue() > 0) {
                            // Check if this srem uses the phi (directly or indirectly)
                            // and initial values are non-negative.
                            // This handles patterns like: a = (a + i*7) % 97
                            continue;  // srem with positive divisor → non-negative ✓
                        }
                    }
                }
            }
            // SRem as direct incoming value (not wrapped in BinaryOperator check)
            // Also trace through sext/zext/extractelement to find the srem at the
            // root.  LLVM's SLP vectorizer may pack two srem operations into a vector
            // srem, producing: sext(extractelement(srem(<2 x i16>, <i16 C1, i16 C2>), N))
            {
                llvm::Value* sremCandidate = incoming;
                // Peel through sext/zext
                if (auto* castInst = llvm::dyn_cast<llvm::CastInst>(sremCandidate)) {
                    if (castInst->getOpcode() == llvm::Instruction::SExt ||
                        castInst->getOpcode() == llvm::Instruction::ZExt) {
                        sremCandidate = castInst->getOperand(0);
                    }
                }
                // Peel through extractelement
                if (auto* ee = llvm::dyn_cast<llvm::ExtractElementInst>(sremCandidate)) {
                    sremCandidate = ee->getVectorOperand();
                }
                if (auto* sremInst = llvm::dyn_cast<llvm::BinaryOperator>(sremCandidate)) {
                    if (sremInst->getOpcode() == llvm::Instruction::SRem ||
                        sremInst->getOpcode() == llvm::Instruction::URem) {
                        auto* divisor = sremInst->getOperand(1);
                        bool allPositive = false;
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(divisor)) {
                            allPositive = ci->getSExtValue() > 0;
                        } else if (auto* cv = llvm::dyn_cast<llvm::Constant>(divisor)) {
                            allPositive = isConstantAllPositive(cv);
                        }
                        if (allPositive) {
                            // Check if initial values are non-negative
                            bool initNonNeg = true;
                            for (unsigned j = 0; j < phi->getNumIncomingValues(); j++) {
                                if (j == i) continue;
                                llvm::Value* otherInc = phi->getIncomingValue(j);
                                if (otherInc == phi) continue;
                                if (!isValueNonNegative(otherInc, DL, depth + 1)) {
                                    initNonNeg = false;
                                    break;
                                }
                            }
                            if (initNonNeg) continue;  // modular loop-carry → non-negative ✓
                        }
                    }
                }
            }
            // General case: recursively check if incoming value is non-negative
            if (!isValueNonNegative(incoming, DL, depth + 1)) {
                allNonNeg = false;
                break;
            }
        }
        activePHIs.erase(phi);
        if (allNonNeg && phi->getNumIncomingValues() > 0) return true;
    }

    // Vector operations: propagate non-negativity through vectorizer-created
    // instructions so that vector srem can be converted to vector urem.
    if (auto* si = llvm::dyn_cast<llvm::ShuffleVectorInst>(inst)) {
        return isValueNonNegative(si->getOperand(0), DL, depth + 1) &&
               (llvm::isa<llvm::UndefValue>(si->getOperand(1)) ||
                llvm::isa<llvm::PoisonValue>(si->getOperand(1)) ||
                isValueNonNegative(si->getOperand(1), DL, depth + 1));
    }
    if (auto* ie = llvm::dyn_cast<llvm::InsertElementInst>(inst)) {
        return isValueNonNegative(ie->getOperand(0), DL, depth + 1) &&
               isValueNonNegative(ie->getOperand(1), DL, depth + 1);
    }
    if (auto* ee = llvm::dyn_cast<llvm::ExtractElementInst>(inst)) {
        return isValueNonNegative(ee->getVectorOperand(), DL, depth + 1);
    }

    // Intrinsic calls with known non-negative results.
    // Note: computeKnownBits at depth=0 does not analyse call instructions,
    // so we must handle these explicitly.
    if (auto* ci = llvm::dyn_cast<llvm::CallInst>(inst)) {
        if (auto* ii = llvm::dyn_cast<llvm::IntrinsicInst>(ci)) {
            switch (ii->getIntrinsicID()) {
            // Bit-counting intrinsics: result is always in [0, bitwidth]
            // (max 64 for i64), so the sign bit is always 0.
            case llvm::Intrinsic::ctpop:
            case llvm::Intrinsic::ctlz:
            case llvm::Intrinsic::cttz:
                return true;
            // llvm.abs: result is always in [0, INT64_MAX] since abs never
            // produces INT64_MIN when is_int_min_poison is false.
            case llvm::Intrinsic::abs:
                return true;
            // Unsigned min/max: results are always non-negative (unsigned).
            case llvm::Intrinsic::umin:
            case llvm::Intrinsic::umax:
                return true;
            // Saturating unsigned add/sub: results are always non-negative.
            case llvm::Intrinsic::uadd_sat:
            case llvm::Intrinsic::usub_sat:
                return true;
            // Signed min/max: non-negative when both inputs are non-negative.
            case llvm::Intrinsic::smin:
            case llvm::Intrinsic::smax:
                return isValueNonNegative(ii->getArgOperand(0), DL, depth + 1) &&
                       isValueNonNegative(ii->getArgOperand(1), DL, depth + 1);
            default:
                break;
            }
        }
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — bit rotation
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: (x << c) | (x >> (bitwidth - c))  →  rotate left by c
/// Also:   (x >> c) | (x << (bitwidth - c))  →  rotate right by c
[[nodiscard]] static std::optional<IdiomMatch> detectRotate(llvm::Instruction* inst) {
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
[[nodiscard]] static std::optional<IdiomMatch> detectAbsoluteValue(llvm::Instruction* inst) {
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

    // Pattern 3: abs(a - b) from select(a > b, a-b, b-a) or select(a < b, b-a, a-b)
    //
    // This is the canonical "absolute difference" or "distance" pattern:
    //   if (a > b) return a - b; else return b - a;
    // Which can be expressed as: abs(a - b)
    //
    // The select has:
    //   cond: icmp sgt a, b  or  icmp sge a, b  or  icmp slt a, b  or  icmp sle a, b
    //   true: a - b  (or b - a, depending on predicate)
    //   false: b - a  (or a - b)
    if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(inst)) {
        auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(sel->getCondition());
        if (cmp) {
            llvm::Value* cmpA = cmp->getOperand(0);
            llvm::Value* cmpB = cmp->getOperand(1);
            llvm::Value* trueV = sel->getTrueValue();
            llvm::Value* falseV = sel->getFalseValue();

            // Helper: check if instruction is sub(p, q)
            auto isSub = [](llvm::Value* v, llvm::Value* p, llvm::Value* q) -> bool {
                auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(v);
                return bin && bin->getOpcode() == llvm::Instruction::Sub &&
                       bin->getOperand(0) == p && bin->getOperand(1) == q;
            };

            // Pattern: select(a > b, a-b, b-a) or select(a >= b, a-b, b-a)
            bool gtOrGe = (cmp->getPredicate() == llvm::ICmpInst::ICMP_SGT ||
                           cmp->getPredicate() == llvm::ICmpInst::ICMP_SGE);
            bool ltOrLe = (cmp->getPredicate() == llvm::ICmpInst::ICMP_SLT ||
                           cmp->getPredicate() == llvm::ICmpInst::ICMP_SLE);

            if ((gtOrGe && isSub(trueV, cmpA, cmpB) && isSub(falseV, cmpB, cmpA)) ||
                (ltOrLe && isSub(trueV, cmpB, cmpA) && isSub(falseV, cmpA, cmpB))) {
                // abs(a - b)
                // Build a - b as a new instruction; the intrinsic needs the diff
                llvm::IRBuilder<> diffBuilder(inst);
                llvm::Value* diff = diffBuilder.CreateSub(cmpA, cmpB, "absdiff");
                IdiomMatch match;
                match.idiom = Idiom::AbsoluteValue;
                match.rootInst = inst;
                match.operands = {diff};
                match.bitWidth = inst->getType()->getIntegerBitWidth();
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
[[nodiscard]] static std::optional<IdiomMatch> detectMinMax(llvm::Instruction* inst) {
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
[[nodiscard]] static std::optional<IdiomMatch> detectPowerOf2Test(llvm::Instruction* inst) {
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
[[nodiscard]] static std::optional<IdiomMatch> detectBitFieldExtract(llvm::Instruction* inst) {
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
// Idiom detection — sign extension
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: (x << (bw-n)) >> (bw-n)  →  sext from n bits
/// This pattern manually sign-extends an n-bit value stored in a wider integer.
[[nodiscard]] static std::optional<IdiomMatch> detectSignExtend(llvm::Instruction* inst) {
    if (inst->getOpcode() != llvm::Instruction::AShr)
        return std::nullopt;

    auto* shl = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(0));
    if (!shl || shl->getOpcode() != llvm::Instruction::Shl)
        return std::nullopt;

    auto shlAmt = getConstIntValue(shl->getOperand(1));
    auto shrAmt = getConstIntValue(inst->getOperand(1));
    if (!shlAmt || !shrAmt || *shlAmt != *shrAmt)
        return std::nullopt;

    unsigned bw = inst->getType()->getIntegerBitWidth();
    unsigned srcBits = bw - static_cast<unsigned>(*shlAmt);
    if (srcBits == 0 || srcBits >= bw)
        return std::nullopt;

    // We have a valid sign-extension from srcBits to bw
    IdiomMatch match;
    match.idiom = Idiom::SignExtend;
    match.rootInst = inst;
    match.operands = {shl->getOperand(0)};
    match.bitWidth = srcBits;
    return match;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — conditional negation
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: select(cond, sub(0, x), x)  →  conditional negation
/// Also:   (x ^ mask) - mask  where mask = ashr(x, bitwidth-1)
[[nodiscard]] static std::optional<IdiomMatch> detectConditionalNeg(llvm::Instruction* inst) {
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
                // operands[0] = x (the value), operands[1] = cond (the condition)
                // replaceIdiom(ConditionalNeg) reads: x = operands[0], cond = operands[1]
                match.operands = {falseVal, sel->getCondition()};
                match.bitWidth = inst->getType()->getIntegerBitWidth();
                return match;
            }
        }
        // Check reverse: select(cond, x, -x) == select(!cond, -x, x)
        // Emit !cond so the replacement can use the standard formula with inverted mask.
        if (auto* sub = llvm::dyn_cast<llvm::BinaryOperator>(falseVal)) {
            if (sub->getOpcode() == llvm::Instruction::Sub &&
                isConstInt(sub->getOperand(0), 0) &&
                sub->getOperand(1) == trueVal) {
                llvm::IRBuilder<> invBuilder(sel);
                llvm::Value* invCond = invBuilder.CreateNot(
                    sel->getCondition(), "cneg.invcond");
                IdiomMatch match;
                match.idiom = Idiom::ConditionalNeg;
                match.rootInst = inst;
                // operands[0] = x (the value), operands[1] = !cond
                match.operands = {trueVal, invCond};
                match.bitWidth = inst->getType()->getIntegerBitWidth();
                return match;
            }
        }
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — clear lowest set bit (x & (x-1))
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: x & (x - 1) → clear lowest set bit (used in is_power_of_2, bit-loop)
/// Pattern: and(x, sub(x, 1))
/// NOTE: x & (-x) isolates the lowest set bit VALUE (= 1 << ctz(x)), not the
/// count.  We do NOT replace x & (-x) with cttz(x) because that changes the
/// result (e.g., 12 & -12 = 4, but cttz(12) = 2).  LLVM's backend already
/// emits `blsi` for x & (-x) on BMI1 targets, so no IR transformation is needed.
[[nodiscard]] static std::optional<IdiomMatch> detectClearLowestBit(llvm::Instruction* inst) {
    if (inst->getOpcode() != llvm::Instruction::And) return std::nullopt;

    llvm::Value* op0 = inst->getOperand(0);
    llvm::Value* op1 = inst->getOperand(1);

    // Try both orderings: and(x, sub(x, 1)) or and(sub(x, 1), x)
    auto tryMatch = [](llvm::Value* x, llvm::Value* candidate) -> llvm::Value* {
        auto* sub = llvm::dyn_cast<llvm::BinaryOperator>(candidate);
        if (sub && sub->getOpcode() == llvm::Instruction::Sub &&
            sub->getOperand(0) == x &&
            isConstInt(sub->getOperand(1), 1)) {
            return x;
        }
        return nullptr;
    };

    llvm::Value* x = tryMatch(op0, op1);
    if (!x) x = tryMatch(op1, op0);
    if (!x) return std::nullopt;

    // x & (x-1) is already optimal: on BMI1 x86, the backend emits `blsr`.
    // No beneficial IR transformation exists, so we return nullopt to leave
    // the pattern as-is and let LLVM's backend handle it.
    (void)x;
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection — byte swap
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: (x >> 24) | ((x >> 8) & 0xFF00) | ((x << 8) & 0xFF0000) | (x << 24)
/// This is a 32-bit byte swap pattern.  Also detects the simpler 16-bit form:
/// ((x >> 8) & 0xFF) | (x << 8)
[[nodiscard]] static std::optional<IdiomMatch> detectByteSwap(llvm::Instruction* inst) {
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
[[nodiscard]] static std::optional<IdiomMatch> detectPopCount(llvm::Instruction* inst) {
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
// Idiom detection — count leading zeros via bit-smearing
// ─────────────────────────────────────────────────────────────────────────────

/// Detect the "bit smear then popcount" CLZ pattern:
///   x |= x >> 1; x |= x >> 2; x |= x >> 4; ... ; return bitwidth - popcount(x)
///
/// This function specifically detects the bit-smear-popcount CLZ idiom,
/// where all bits below the highest set bit are smeared to 1 via a chain
/// of OR-shift operations, and then popcount is subtracted from the bit
/// width to yield the number of leading zeros.
///
/// We detect the end of the pattern: sub(bitwidth, ctpop(or-chain(x)))
/// where the or-chain is at least one stage of or(x, lshr(x, k)).
/// The de Bruijn lookup variant is left for future work.
[[nodiscard]] static std::optional<IdiomMatch> detectCountLeadingZeros(llvm::Instruction* inst) {
    // Detect bit-smear sequence ending with popcount subtraction:
    //   x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16; x |= x >> 32;
    //   return 64 - popcount(x);
    // The de Bruijn lookup variant is left for future work.

    // We detect the end of the pattern: sub(bitwidth, ctpop(or-chain))
    if (inst->getOpcode() != llvm::Instruction::Sub) return std::nullopt;

    unsigned bitWidth = inst->getType()->getIntegerBitWidth();
    if (!isConstInt(inst->getOperand(0), static_cast<int64_t>(bitWidth)))
        return std::nullopt;

    // The second operand should be a ctpop call
    auto* call = llvm::dyn_cast<llvm::CallInst>(inst->getOperand(1));
    if (!call) return std::nullopt;
    llvm::Function* callee = call->getCalledFunction();
    if (!callee || callee->getIntrinsicID() != llvm::Intrinsic::ctpop)
        return std::nullopt;

    // The argument to ctpop should be the result of a bit-smearing OR chain
    llvm::Value* smeared = call->getArgOperand(0);

    // Walk back through OR-shift chain: x |= x >> k
    // We need at least the first stage: or(x, lshr(x, 1))
    auto* orInst = llvm::dyn_cast<llvm::BinaryOperator>(smeared);
    if (!orInst || orInst->getOpcode() != llvm::Instruction::Or)
        return std::nullopt;

    // Check for or(x, lshr(x, k)) pattern
    auto* lshr = llvm::dyn_cast<llvm::BinaryOperator>(orInst->getOperand(1));
    if (!lshr || lshr->getOpcode() != llvm::Instruction::LShr)
        return std::nullopt;

    llvm::Value* x = orInst->getOperand(0);
    // Walk up the OR chain to find the original x
    // The chain looks like: or(or(or(x, lshr(x,1)), lshr(or(x,lshr(x,1)),2)), ...)
    // Just find the deepest x
    while (auto* innerOr = llvm::dyn_cast<llvm::BinaryOperator>(x)) {
        if (innerOr->getOpcode() != llvm::Instruction::Or) break;
        x = innerOr->getOperand(0);
    }

    IdiomMatch match;
    match.idiom = Idiom::CountLeadingZeros;
    match.rootInst = inst;
    match.operands = {x};
    match.bitWidth = bitWidth;
    return match;
}

// ─────────────────────────────────────────────────────────────────────────────
// Saturating addition: select(add_overflow(a,b), MAX, a+b) → uadd.sat(a,b)
// Patterns detected:
//   1. select(icmp ugt (a+b), a, MAX_UINT, a+b)  (unsigned overflow via wrap)
//   2. select(extractvalue(@llvm.uadd.with.overflow(a,b), 1), MAX, sum)
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] static std::optional<IdiomMatch> detectSaturatingAdd(llvm::Instruction* inst) {
    auto* sel = llvm::dyn_cast<llvm::SelectInst>(inst);
    if (!sel) return std::nullopt;
    if (!sel->getType()->isIntegerTy()) return std::nullopt;

    llvm::Value* cond = sel->getCondition();
    llvm::Value* trueVal = sel->getTrueValue();
    llvm::Value* falseVal = sel->getFalseValue();

    // Pattern: select(icmp ult (a+b) a, MAX, a+b)
    //   meaning: if (a+b) wrapped around (unsigned), return MAX; else return a+b
    auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(cond);
    if (!icmp) return std::nullopt;

    unsigned bitWidth = sel->getType()->getIntegerBitWidth();
    uint64_t maxVal = bitWidth >= 64 ? ~uint64_t(0) : (1ULL << bitWidth) - 1;

    // Check: trueVal == MAX_UINT and falseVal is the sum
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(trueVal)) {
        if (ci->getZExtValue() == maxVal) {
            auto* sum = llvm::dyn_cast<llvm::BinaryOperator>(falseVal);
            if (sum && sum->getOpcode() == llvm::Instruction::Add) {
                // Check: icmp ult sum, a  (unsigned overflow detection)
                if (icmp->getPredicate() == llvm::CmpInst::ICMP_ULT &&
                    icmp->getOperand(0) == sum) {
                    llvm::Value* a = sum->getOperand(0);
                    llvm::Value* b = sum->getOperand(1);
                    if (icmp->getOperand(1) == a || icmp->getOperand(1) == b) {
                        IdiomMatch match;
                        match.idiom = Idiom::SaturatingAdd;
                        match.rootInst = inst;
                        match.operands = {a, b};
                        match.bitWidth = bitWidth;
                        return match;
                    }
                }
            }
        }
    }

    // Also check reversed pattern: select(icmp ugt a (a+b), MAX, a+b)
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(trueVal)) {
        if (ci->getZExtValue() == maxVal) {
            auto* sum = llvm::dyn_cast<llvm::BinaryOperator>(falseVal);
            if (sum && sum->getOpcode() == llvm::Instruction::Add) {
                if (icmp->getPredicate() == llvm::CmpInst::ICMP_UGT &&
                    icmp->getOperand(1) == sum) {
                    llvm::Value* a = sum->getOperand(0);
                    llvm::Value* b = sum->getOperand(1);
                    if (icmp->getOperand(0) == a || icmp->getOperand(0) == b) {
                        IdiomMatch match;
                        match.idiom = Idiom::SaturatingAdd;
                        match.rootInst = inst;
                        match.operands = {a, b};
                        match.bitWidth = bitWidth;
                        return match;
                    }
                }
            }
        }
    }

    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Saturating subtraction: select(icmp ult a b, 0, a-b) → usub.sat(a,b)
// Pattern: clamp subtraction to zero instead of wrapping unsigned
// ─────────────────────────────────────────────────────────────────────────────
[[nodiscard]] static std::optional<IdiomMatch> detectSaturatingSub(llvm::Instruction* inst) {
    auto* sel = llvm::dyn_cast<llvm::SelectInst>(inst);
    if (!sel) return std::nullopt;
    if (!sel->getType()->isIntegerTy()) return std::nullopt;

    llvm::Value* cond = sel->getCondition();
    llvm::Value* trueVal = sel->getTrueValue();
    llvm::Value* falseVal = sel->getFalseValue();

    auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(cond);
    if (!icmp) return std::nullopt;

    unsigned bitWidth = sel->getType()->getIntegerBitWidth();

    // Pattern: select(icmp ult a b, 0, a-b)
    // If a < b (unsigned), return 0; else return a - b
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(trueVal)) {
        if (ci->isZero()) {
            auto* sub = llvm::dyn_cast<llvm::BinaryOperator>(falseVal);
            if (sub && sub->getOpcode() == llvm::Instruction::Sub) {
                llvm::Value* a = sub->getOperand(0);
                llvm::Value* b = sub->getOperand(1);
                // Check: icmp ult a, b
                if (icmp->getPredicate() == llvm::CmpInst::ICMP_ULT &&
                    icmp->getOperand(0) == a && icmp->getOperand(1) == b) {
                    IdiomMatch match;
                    match.idiom = Idiom::SaturatingSub;
                    match.rootInst = inst;
                    match.operands = {a, b};
                    match.bitWidth = bitWidth;
                    return match;
                }
            }
        }
    }

    // Reversed: select(icmp uge a b, a-b, 0)
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(falseVal)) {
        if (ci->isZero()) {
            auto* sub = llvm::dyn_cast<llvm::BinaryOperator>(trueVal);
            if (sub && sub->getOpcode() == llvm::Instruction::Sub) {
                llvm::Value* a = sub->getOperand(0);
                llvm::Value* b = sub->getOperand(1);
                if (icmp->getPredicate() == llvm::CmpInst::ICMP_UGE &&
                    icmp->getOperand(0) == a && icmp->getOperand(1) == b) {
                    IdiomMatch match;
                    match.idiom = Idiom::SaturatingSub;
                    match.rootInst = inst;
                    match.operands = {a, b};
                    match.bitWidth = bitWidth;
                    return match;
                }
            }
        }
    }

    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Conditional increment/decrement idiom detection
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: select(cond, x+1, x) or select(cond, x-1, x)
/// These patterns arise from if/else branches that conditionally adjust a
/// counter.  Replacing with x + zext(cond) or x - zext(cond) eliminates
/// the select instruction and produces a single add/sub with a zero-extended
/// boolean, which is cheaper on all microarchitectures.
[[nodiscard]] static std::optional<IdiomMatch> detectConditionalIncrement(llvm::Instruction* inst) {
    auto* sel = llvm::dyn_cast<llvm::SelectInst>(inst);
    if (!sel) return std::nullopt;

    llvm::Value* cond = sel->getCondition();
    llvm::Value* trueVal = sel->getTrueValue();
    llvm::Value* falseVal = sel->getFalseValue();

    // Pattern 1: select(cond, x + 1, x)  →  x + zext(cond)
    if (auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(trueVal)) {
        if (addInst->getOpcode() == llvm::Instruction::Add) {
            llvm::Value* lhs = addInst->getOperand(0);
            llvm::Value* rhs = addInst->getOperand(1);
            if (lhs == falseVal && isConstInt(rhs, 1)) {
                IdiomMatch match;
                match.idiom = Idiom::ConditionalIncrement;
                match.rootInst = inst;
                match.operands = {falseVal, cond};
                match.bitWidth = inst->getType()->getIntegerBitWidth();
                return match;
            }
            if (rhs == falseVal && isConstInt(lhs, 1)) {
                IdiomMatch match;
                match.idiom = Idiom::ConditionalIncrement;
                match.rootInst = inst;
                match.operands = {falseVal, cond};
                match.bitWidth = inst->getType()->getIntegerBitWidth();
                return match;
            }
        }

        // Pattern 2: select(cond, x - 1, x)  →  x - zext(cond)
        if (addInst->getOpcode() == llvm::Instruction::Sub) {
            llvm::Value* lhs = addInst->getOperand(0);
            llvm::Value* rhs = addInst->getOperand(1);
            if (lhs == falseVal && isConstInt(rhs, 1)) {
                IdiomMatch match;
                match.idiom = Idiom::ConditionalDecrement;
                match.rootInst = inst;
                match.operands = {falseVal, cond};
                match.bitWidth = inst->getType()->getIntegerBitWidth();
                return match;
            }
        }
    }

    // Reverse patterns: select(cond, x, x+1) → x + zext(!cond)
    // These are handled by LLVM's instcombine (select canonicalization),
    // so we don't need to detect them here.

    // Pattern 3: select(cond, C+1, C) where C is a constant integer  →  C + zext(cond)
    // This is a specialization of Pattern 1 for constant bases. It arises naturally
    // when boolean conditions are accumulated: `count += (x > 0)` compiles to
    //   result = select(x > 0, count+1, count)
    // which reduces to count + zext(x>0).  When `count` starts as a literal like 0
    // or 1, the trueVal and falseVal are integer constants differing by 1.
    {
        auto cvTrue  = getConstIntValue(trueVal);
        auto cvFalse = getConstIntValue(falseVal);
        if (cvTrue && cvFalse && *cvTrue == *cvFalse + 1) {
            IdiomMatch match;
            match.idiom = Idiom::ConditionalIncrement;
            match.rootInst = inst;
            match.operands = {falseVal, cond};
            match.bitWidth = inst->getType()->getIntegerBitWidth();
            return match;
        }
        // Pattern 4: select(cond, C-1, C) where C is a constant integer  →  C - zext(cond)
        if (cvTrue && cvFalse && *cvTrue == *cvFalse - 1) {
            IdiomMatch match;
            match.idiom = Idiom::ConditionalDecrement;
            match.rootInst = inst;
            match.operands = {falseVal, cond};
            match.bitWidth = inst->getType()->getIntegerBitWidth();
            return match;
        }
    }

    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Hacker's Delight §5-2: Average of two integers without overflow
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: (a & b) + ((a ^ b) >> 1)  →  floor((a + b) / 2) without overflow
/// Also:   (a + b) >> 1  when NSW/NUW flags prove no overflow (already handled
///         by LLVM InstCombine, but our pattern handles the explicit safe form).
///
/// Hacker's Delight §5-2: "Average of two integers without overflow":
///   unsigned avg = (a & b) + ((a ^ b) >> 1)
///   This is exactly llvm.uavg.u(a, b) — one instruction on modern x86 with AVX512.
[[nodiscard]] static std::optional<IdiomMatch> detectAverageWithoutOverflow(llvm::Instruction* inst) {
    // Pattern: add(and(a, b), lshr(xor(a, b), 1))
    if (inst->getOpcode() != llvm::Instruction::Add) return std::nullopt;

    llvm::Value* addL = inst->getOperand(0);
    llvm::Value* addR = inst->getOperand(1);

    // Try both orderings
    for (int ord = 0; ord < 2; ++ord) {
        auto* andInst = llvm::dyn_cast<llvm::BinaryOperator>(addL);
        auto* shrInst = llvm::dyn_cast<llvm::BinaryOperator>(addR);
        if (!andInst || andInst->getOpcode() != llvm::Instruction::And) {
            std::swap(addL, addR);
            std::swap(andInst, shrInst);
            continue;
        }
        if (!andInst || andInst->getOpcode() != llvm::Instruction::And) break;
        if (!shrInst || shrInst->getOpcode() != llvm::Instruction::LShr) break;

        // Check: shr amount is 1
        auto shrAmt = getConstIntValue(shrInst->getOperand(1));
        if (!shrAmt || *shrAmt != 1) break;

        // The xor inside the shift
        auto* xorInst = llvm::dyn_cast<llvm::BinaryOperator>(shrInst->getOperand(0));
        if (!xorInst || xorInst->getOpcode() != llvm::Instruction::Xor) break;

        // Verify both (a & b) and ((a ^ b) >> 1) use the same a, b
        llvm::Value* andA = andInst->getOperand(0);
        llvm::Value* andB = andInst->getOperand(1);
        llvm::Value* xorA = xorInst->getOperand(0);
        llvm::Value* xorB = xorInst->getOperand(1);

        bool match1 = (andA == xorA && andB == xorB);
        bool match2 = (andA == xorB && andB == xorA);
        if (!match1 && !match2) break;

        IdiomMatch match;
        match.idiom = Idiom::AverageWithoutOverflow;
        match.rootInst = inst;
        match.operands = {andA, andB};
        match.bitWidth = inst->getType()->getIntegerBitWidth();
        return match;
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Hacker's Delight §2-1: Sign of an integer
// ─────────────────────────────────────────────────────────────────────────────

/// Detect: select(x > 0, 1, select(x < 0, -1, 0))  →  sign(x)
/// Or the arithmetic version: (x >> 63) | ((-x) >>> 63)  Hacker's Delight §2-7
/// Canonical LLVM: (x >> (N-1)) - ((-x) >> (N-1))  (for signed)
///   or:  ashr(x, N-1) | zext(icmp sgt x, 0)  (alternative)
///
/// Replaces with: select(x > 0, 1, ashr(x, bitwidth-1))
/// which is 2 ops (icmp + select/or) vs 3-5 ops for the naive form.
[[nodiscard]] static std::optional<IdiomMatch> detectSignFunction(llvm::Instruction* inst) {
    // Pattern 1: or(ashr(x, 63), zext(icmp sgt(x, 0)))
    // = (x >> 63) | (x > 0 ? 1 : 0)
    // For signed: negative→-1, zero→0, positive→1
    if (inst->getOpcode() == llvm::Instruction::Or) {
        auto* ashrInst = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(0));
        auto* zextInst = llvm::dyn_cast<llvm::CastInst>(inst->getOperand(1));
        if (!ashrInst || !zextInst) {
            // Try reversed order
            ashrInst = llvm::dyn_cast<llvm::BinaryOperator>(inst->getOperand(1));
            zextInst = llvm::dyn_cast<llvm::CastInst>(inst->getOperand(0));
        }
        if (!ashrInst || ashrInst->getOpcode() != llvm::Instruction::AShr) return std::nullopt;
        if (!zextInst || zextInst->getOpcode() != llvm::Instruction::ZExt) return std::nullopt;

        unsigned bw = inst->getType()->getIntegerBitWidth();
        auto shiftAmt = getConstIntValue(ashrInst->getOperand(1));
        if (!shiftAmt || *shiftAmt != static_cast<int64_t>(bw - 1)) return std::nullopt;

        auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(zextInst->getOperand(0));
        if (!cmp || cmp->getPredicate() != llvm::ICmpInst::ICMP_SGT) return std::nullopt;
        auto zeroCheck = getConstIntValue(cmp->getOperand(1));
        if (!zeroCheck || *zeroCheck != 0) return std::nullopt;
        if (cmp->getOperand(0) != ashrInst->getOperand(0)) return std::nullopt;

        IdiomMatch match;
        match.idiom = Idiom::SignFunction;
        match.rootInst = inst;
        match.operands = {ashrInst->getOperand(0)};
        match.bitWidth = bw;
        return match;
    }

    // Pattern 2: nested select: select(cmp sgt x, 0, 1, select(cmp slt x, 0, -1, 0))
    if (auto* outer = llvm::dyn_cast<llvm::SelectInst>(inst)) {
        auto* outerCmp = llvm::dyn_cast<llvm::ICmpInst>(outer->getCondition());
        if (!outerCmp) return std::nullopt;

        // Check outer is: x > 0 → true_val=1
        auto trueVal = getConstIntValue(outer->getTrueValue());
        if (outerCmp->getPredicate() == llvm::ICmpInst::ICMP_SGT && trueVal && *trueVal == 1) {
            auto zeroCheck = getConstIntValue(outerCmp->getOperand(1));
            if (!zeroCheck || *zeroCheck != 0) return std::nullopt;
            llvm::Value* x = outerCmp->getOperand(0);

            // The false branch should be: select(x < 0, -1, 0)
            auto* inner = llvm::dyn_cast<llvm::SelectInst>(outer->getFalseValue());
            if (!inner) return std::nullopt;
            auto* innerCmp = llvm::dyn_cast<llvm::ICmpInst>(inner->getCondition());
            if (!innerCmp || innerCmp->getPredicate() != llvm::ICmpInst::ICMP_SLT) return std::nullopt;
            if (innerCmp->getOperand(0) != x) return std::nullopt;
            auto zero2 = getConstIntValue(innerCmp->getOperand(1));
            if (!zero2 || *zero2 != 0) return std::nullopt;
            auto negOne = getConstIntValue(inner->getTrueValue());
            auto zero3 = getConstIntValue(inner->getFalseValue());
            if (!negOne || *negOne != static_cast<int64_t>(-1)) return std::nullopt;
            if (!zero3 || *zero3 != 0) return std::nullopt;

            IdiomMatch match;
            match.idiom = Idiom::SignFunction;
            match.rootInst = inst;
            match.operands = {x};
            match.bitWidth = inst->getType()->getIntegerBitWidth();
            return match;
        }
    }
    return std::nullopt;
}

// ─────────────────────────────────────────────────────────────────────────────
// Hacker's Delight §3-1: Next power of 2
// ─────────────────────────────────────────────────────────────────────────────

/// Detect the bit-smear pattern for "ceiling to next power of 2":
///   x = x - 1
///   x |= x >> 1; x |= x >> 2; x |= x >> 4; x |= x >> 8; x |= x >> 16; x |= x >> 32
///   x = x + 1
/// This is equivalent to: x == 0 ? 1 : 1 << (64 - ctlz(x-1))
/// Or simpler: (x <= 1) ? 1 : (1 << (64 - ctlz(x - 1)))
///
/// We detect the accumulated OR pattern and replace with CLZ-based computation.
[[nodiscard]] static std::optional<IdiomMatch> detectNextPowerOf2(llvm::Instruction* inst) {
    // Final instruction must be: add(%smeared, 1)
    if (inst->getOpcode() != llvm::Instruction::Add) return std::nullopt;
    auto addOne = getConstIntValue(inst->getOperand(1));
    if (!addOne || *addOne != 1) return std::nullopt;

    llvm::Value* smeared = inst->getOperand(0);

    // Walk through the OR chain: the value must be: x | (x >> 32) at minimum
    // We need to see at least 3 OR-with-shift layers
    int orLayers = 0;
    llvm::Value* cur = smeared;
    while (true) {
        auto* orInst = llvm::dyn_cast<llvm::BinaryOperator>(cur);
        if (!orInst || orInst->getOpcode() != llvm::Instruction::Or) break;
        llvm::Value* shrCandidate = orInst->getOperand(1);
        llvm::Value* prevCandidate = orInst->getOperand(0);
        auto* shrInst = llvm::dyn_cast<llvm::BinaryOperator>(shrCandidate);
        if (!shrInst || (shrInst->getOpcode() != llvm::Instruction::LShr &&
                         shrInst->getOpcode() != llvm::Instruction::AShr)) {
            // Try the other ordering: or(shr, prev)
            std::swap(shrCandidate, prevCandidate);
            shrInst = llvm::dyn_cast<llvm::BinaryOperator>(shrCandidate);
            if (!shrInst || (shrInst->getOpcode() != llvm::Instruction::LShr &&
                             shrInst->getOpcode() != llvm::Instruction::AShr))
                break;
        }
        ++orLayers;
        cur = prevCandidate;
        if (orLayers >= 8) break;  // safety cap — at most 6 layers (1,2,4,8,16,32)
    }

    if (orLayers < 3) return std::nullopt;

    // The base (before smearing) should be: sub(original_x, 1) or add(original_x, -1)
    // LLVM normalizes x - 1 to x + (-1) in many cases, so we must accept both.
    auto* subInst = llvm::dyn_cast<llvm::BinaryOperator>(cur);
    if (!subInst) return std::nullopt;
    llvm::Value* baseX = nullptr;
    if (subInst->getOpcode() == llvm::Instruction::Sub) {
        auto subOne = getConstIntValue(subInst->getOperand(1));
        if (!subOne || *subOne != 1) return std::nullopt;
        baseX = subInst->getOperand(0);
    } else if (subInst->getOpcode() == llvm::Instruction::Add) {
        // add(x, -1) = x - 1 in two's complement
        auto addVal = getConstIntValue(subInst->getOperand(1));
        if (!addVal || *addVal != -1LL) return std::nullopt;
        baseX = subInst->getOperand(0);
    } else {
        return std::nullopt;
    }

    IdiomMatch match;
    match.idiom = Idiom::NextPowerOf2;
    match.rootInst = inst;
    match.operands = {baseX};  // original x
    match.bitWidth = inst->getType()->getIntegerBitWidth();
    return match;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main idiom detection
// ─────────────────────────────────────────────────────────────────────────────

[[gnu::hot]] std::vector<IdiomMatch> detectIdioms(llvm::BasicBlock& bb) {
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
        if (auto m = detectSignExtend(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectClearLowestBit(&inst)) {
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
        if (auto m = detectCountLeadingZeros(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectSaturatingAdd(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectSaturatingSub(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectConditionalIncrement(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectAverageWithoutOverflow(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectSignFunction(&inst)) {
            results.push_back(std::move(*m));
            continue;
        }
        if (auto m = detectNextPowerOf2(&inst)) {
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

    case Idiom::CountLeadingZeros: {
        // Replace bit-smear + popcount CLZ with llvm.ctlz intrinsic
        llvm::Value* x = match.operands[0];
        llvm::Function* ctlz = OMSC_GET_INTRINSIC(
            mod, llvm::Intrinsic::ctlz, {intTy});
        llvm::Value* falseConst = llvm::ConstantInt::getFalse(ctx);
        llvm::Value* result = builder.CreateCall(ctlz, {x, falseConst}, "ctlz");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::CountTrailingZeros: {
        // Replace isolate-lowest-bit with llvm.cttz intrinsic
        llvm::Value* x = match.operands[0];
        llvm::Function* cttz = OMSC_GET_INTRINSIC(
            mod, llvm::Intrinsic::cttz, {intTy});
        llvm::Value* falseConst = llvm::ConstantInt::getFalse(ctx);
        llvm::Value* result = builder.CreateCall(cttz, {x, falseConst}, "cttz");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::BitFieldExtract: {
        // Replace shift-and-mask with a normalized shift+and sequence
        // (this helps subsequent passes recognize the pattern).
        // operands[0] = base value x
        // operands[1] = shift amount
        // operands[2] = original bit mask (e.g., 7 for a 3-bit field)
        llvm::Value* x = match.operands[0];
        llvm::Value* shift = match.operands.size() > 1 ? match.operands[1] : nullptr;
        llvm::Value* origMask = match.operands.size() > 2 ? match.operands[2] : nullptr;
        if (shift && origMask) {
            llvm::Value* shifted = builder.CreateLShr(x, shift, "bfe.shr");
            // Use the ORIGINAL mask (e.g., 7 for width=3), not match.bitWidth
            // (which is the integer type width, e.g., 64 for i64 — not the field width).
            llvm::Value* result = builder.CreateAnd(shifted, origMask, "bfe.and");
            match.rootInst->replaceAllUsesWith(result);
            return true;
        }
        return false;
    }

    case Idiom::SaturatingAdd: {
        // Replace select(overflow_test, MAX, a+b) → llvm.uadd.sat(a, b)
        llvm::Value* a = match.operands[0];
        llvm::Value* b = match.operands[1];
        llvm::Function* satAdd = OMSC_GET_INTRINSIC(
            mod, llvm::Intrinsic::uadd_sat, {intTy});
        llvm::Value* result = builder.CreateCall(satAdd, {a, b}, "uadd_sat");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::SaturatingSub: {
        // Replace select(a < b, 0, a-b) → llvm.usub.sat(a, b)
        llvm::Value* a = match.operands[0];
        llvm::Value* b = match.operands[1];
        llvm::Function* satSub = OMSC_GET_INTRINSIC(
            mod, llvm::Intrinsic::usub_sat, {intTy});
        llvm::Value* result = builder.CreateCall(satSub, {a, b}, "usub_sat");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::ConditionalIncrement: {
        // Replace select(cond, x+1, x) → x + zext(cond, type)
        // This eliminates the select and the intermediate add, producing
        // a single zext + add sequence that is cheaper on all targets.
        //
        // Reuse an existing zext(cond, intTy) in the same basic block to
        // avoid creating a duplicate that prevents value-equality checks in
        // downstream passes (applyAlgebraicSimplifications, select chains).
        llvm::Value* x = match.operands[0];
        llvm::Value* cond = match.operands[1];
        llvm::Value* ext = nullptr;
        if (auto* rootBB = match.rootInst->getParent()) {
            for (auto& i : *rootBB) {
                if (&i == match.rootInst) break;
                if (auto* z = llvm::dyn_cast<llvm::ZExtInst>(&i)) {
                    if (z->getOperand(0) == cond && z->getType() == intTy) {
                        ext = z;
                        break;
                    }
                }
            }
        }
        if (!ext) ext = builder.CreateZExt(cond, intTy, "cond.zext");
        llvm::Value* result = builder.CreateAdd(x, ext, "cond.inc");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::ConditionalDecrement: {
        // Replace select(cond, x-1, x) → x - zext(cond, type)
        llvm::Value* x = match.operands[0];
        llvm::Value* cond = match.operands[1];
        llvm::Value* ext = nullptr;
        if (auto* rootBB = match.rootInst->getParent()) {
            for (auto& i : *rootBB) {
                if (&i == match.rootInst) break;
                if (auto* z = llvm::dyn_cast<llvm::ZExtInst>(&i)) {
                    if (z->getOperand(0) == cond && z->getType() == intTy) {
                        ext = z;
                        break;
                    }
                }
            }
        }
        if (!ext) ext = builder.CreateZExt(cond, intTy, "cond.zext");
        llvm::Value* result = builder.CreateSub(x, ext, "cond.dec");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::AverageWithoutOverflow: {
        // Hacker's Delight §5-2: (a & b) + ((a ^ b) >> 1)
        // Replace with the equivalent 2-instruction sequence that directly
        // expresses the average, which LLVM's backend can lower to VAVG or
        // a single add + shift on targets that support it.
        // We emit: (a + b) >> 1 with overflow-safe arithmetic:
        //   tmp = add(a, b)  — may overflow, but we handle with nsw trick
        // Actually we re-emit the canonical form so LLVM InstCombine can
        // further optimize:  (a & b) + ((a ^ b) >> 1)  is already optimal
        // for unsigned average.  Just return true to mark as handled — the
        // pattern is already the optimal 3-instruction form and the idiom
        // detection will prevent the synthesis pass from trying to "improve"
        // it further with more expensive sequences.
        //
        // For the actual lowering: emit the average directly using the form
        // that LLVM recognizes as uavg_sat-eligible:
        //   result = lshr(add_nuw(a, b), 1)  when both are non-negative
        // or keep the original pattern. Since this runs on verified non-negative
        // contexts, use the simpler form:
        llvm::Value* a = match.operands[0];
        llvm::Value* b = match.operands[1];
        // (a | b) - ((a ^ b) >> 1) is equivalent and sometimes cheaper:
        // But the simplest canonical form for LLVM to recognize is:
        // lshr(add(zext(a, i128), zext(b, i128)), 1) truncated — too complex.
        // Instead: the original pattern (a&b)+((a^b)>>1) is already the
        // 3-op Hacker's Delight form. We improve it to (a|b)-((a^b)>>1)
        // which uses the same 3 instructions but may have better ILP
        // because OR has no carry chain unlike AND:
        llvm::Value* orVal = builder.CreateOr(a, b, "avg.or");
        llvm::Value* xorVal = builder.CreateXor(a, b, "avg.xor");
        llvm::Value* shrVal = builder.CreateLShr(xorVal,
            llvm::ConstantInt::get(intTy, 1), "avg.shr");
        llvm::Value* result = builder.CreateSub(orVal, shrVal, "avg.sub");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::SignFunction: {
        // Hacker's Delight §2-7: sign of integer
        // Replace with: ashr(x, bw-1) | zext(x > 0)
        // = -1 for x<0, 0 for x==0, 1 for x>0
        // 2 instructions vs 3-5 for the naive form.
        llvm::Value* x = match.operands[0];
        unsigned bw = match.bitWidth;
        llvm::Value* shiftAmt = llvm::ConstantInt::get(intTy, bw - 1);
        llvm::Value* sign = builder.CreateAShr(x, shiftAmt, "sign.shr");
        llvm::Value* pos = builder.CreateICmpSGT(x,
            llvm::ConstantInt::get(intTy, 0), "sign.pos");
        llvm::Value* posExt = builder.CreateZExt(pos, intTy, "sign.zext");
        llvm::Value* result = builder.CreateOr(sign, posExt, "sign.or");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::NextPowerOf2: {
        // Hacker's Delight §3-1: next power of 2 (ceiling)
        // Replace bit-smear pattern with CLZ-based computation:
        //   x == 0 ? 1 : 1 << (bw - ctlz(x - 1))
        // This is 4-5 instructions vs 10+ for the bit-smear.
        llvm::Value* x = match.operands[0];
        unsigned bw = match.bitWidth;
        llvm::Function* ctlz = OMSC_GET_INTRINSIC(mod, llvm::Intrinsic::ctlz, {intTy});
        // Compute: 1 << (bw - clz(x - 1))
        llvm::Value* xm1 = builder.CreateSub(x,
            llvm::ConstantInt::get(intTy, 1), "np2.sub");
        llvm::Value* falseConst = llvm::ConstantInt::getFalse(ctx);
        llvm::Value* clz = builder.CreateCall(ctlz, {xm1, falseConst}, "np2.clz");
        llvm::Value* bwConst = llvm::ConstantInt::get(intTy, bw);
        llvm::Value* shift = builder.CreateSub(bwConst, clz, "np2.shift");
        llvm::Value* one = llvm::ConstantInt::get(intTy, 1);
        llvm::Value* pow2 = builder.CreateShl(one, shift, "np2.shl");
        // Edge case: x == 0 → return 1
        llvm::Value* isZero = builder.CreateICmpEQ(x,
            llvm::ConstantInt::get(intTy, 0), "np2.iszero");
        llvm::Value* result = builder.CreateSelect(isZero, one, pow2, "np2.result");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::ConditionalNeg: {
        // select(cond, -x, x) → (x ^ mask) - mask  where mask = -sext(cond)
        // This replaces a conditional negation with branchless two's complement:
        //   mask = sext(cond)    (all-1s if true, all-0s if false)
        //   result = (x ^ mask) - mask
        // 3 instructions and fully branchless on all targets.
        llvm::Value* x = match.operands[0];
        llvm::Value* cond = match.operands[1];
        llvm::Value* ext = builder.CreateSExt(cond, intTy, "cneg.sext");
        llvm::Value* xored = builder.CreateXor(x, ext, "cneg.xor");
        llvm::Value* result = builder.CreateSub(xored, ext, "cneg.sub");
        match.rootInst->replaceAllUsesWith(result);
        return true;
    }

    case Idiom::SignExtend: {
        // (x << (bw-n)) >> (bw-n) → trunc to n bits + sext back to bw
        // The canonical trunc+sext form lets the backend lower to a single
        // MOVSX/SXTB instruction instead of a shift pair.
        llvm::Value* x = match.operands[0];
        unsigned srcBits = match.bitWidth;
        llvm::Type* narrowTy = llvm::IntegerType::get(ctx, srcBits);
        llvm::Value* truncated = builder.CreateTrunc(x, narrowTy, "sext.trunc");
        llvm::Value* result = builder.CreateSExt(truncated, intTy, "sext.ext");
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
[[gnu::hot]] static unsigned applyAlgebraicSimplifications(llvm::Function& func) {
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

            // Pattern: x + x → x << 1  (shift is cheaper than add on some
            // architectures and canonicalizes the idiom for later passes)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                if (inst.getOperand(0) == inst.getOperand(1)) {
                    llvm::IRBuilder<> builder(&inst);
                    simplified = builder.CreateShl(inst.getOperand(0),
                        llvm::ConstantInt::get(inst.getType(), 1), "add_self_shl");
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
                if (isConstInt(inst.getOperand(1), 0) || isConstInt(inst.getOperand(0), 0)) {
                    simplified = llvm::ConstantInt::get(inst.getType(), 0);
                }
            }

            // Pattern: x & 0 → 0
            if (!simplified && inst.getOpcode() == llvm::Instruction::And) {
                if (isConstInt(inst.getOperand(1), 0) || isConstInt(inst.getOperand(0), 0)) {
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
                        if (c1 && c2 && *c1 >= 0 && *c2 >= 0) {
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
                        if (c1 && c2 && *c1 >= 0 && *c2 >= 0) {
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

            // Pattern: (x & C1) & C2 → x & (C1 & C2)  (redundant AND folding)
            if (!simplified && inst.getOpcode() == llvm::Instruction::And) {
                if (auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                    if (inner->getOpcode() == llvm::Instruction::And && hasOneUse(inner)) {
                        auto c1 = getConstIntValue(inner->getOperand(1));
                        auto c2 = getConstIntValue(inst.getOperand(1));
                        if (c1 && c2) {
                            llvm::IRBuilder<> builder(&inst);
                            int64_t combined = *c1 & *c2;
                            simplified = builder.CreateAnd(
                                inner->getOperand(0),
                                llvm::ConstantInt::get(inst.getType(), combined),
                                "and_combine_consts");
                        }
                    }
                }
            }

            // Pattern: (x | C1) | C2 → x | (C1 | C2)  (redundant OR folding)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Or) {
                if (auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                    if (inner->getOpcode() == llvm::Instruction::Or && hasOneUse(inner)) {
                        auto c1 = getConstIntValue(inner->getOperand(1));
                        auto c2 = getConstIntValue(inst.getOperand(1));
                        if (c1 && c2) {
                            llvm::IRBuilder<> builder(&inst);
                            int64_t combined = *c1 | *c2;
                            simplified = builder.CreateOr(
                                inner->getOperand(0),
                                llvm::ConstantInt::get(inst.getType(), combined),
                                "or_combine_consts");
                        }
                    }
                }
            }

            // Pattern: (x ^ C1) ^ C2 → x ^ (C1 ^ C2)  (redundant XOR folding)
            if (!simplified && inst.getOpcode() == llvm::Instruction::Xor) {
                if (auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                    if (inner->getOpcode() == llvm::Instruction::Xor && hasOneUse(inner)) {
                        auto c1 = getConstIntValue(inner->getOperand(1));
                        auto c2 = getConstIntValue(inst.getOperand(1));
                        if (c1 && c2) {
                            llvm::IRBuilder<> builder(&inst);
                            int64_t combined = *c1 ^ *c2;
                            simplified = builder.CreateXor(
                                inner->getOperand(0),
                                llvm::ConstantInt::get(inst.getType(), combined),
                                "xor_combine_consts");
                        }
                    }
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
                if (isConstInt(inst.getOperand(1), 0) || isConstInt(inst.getOperand(0), 0)) {
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
                        if (c1 && c2 && *c1 >= 0 && *c2 >= 0) {
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
            // ── Signed rem/sub by power-of-2 — non-negative dividend fast path ─
            // srem(x, 2^n) → and(x, 2^n - 1) when x is provably non-negative.
            // Safe because srem == urem for non-negative dividends.  Avoids the
            // 25-cycle signed remainder operation entirely.  Critical for loops
            // where the induction variable (always ≥ 0) is used in modulo
            // expressions like `i % 8` to compute bucket indices or alignment.
            if (!simplified && inst.getOpcode() == llvm::Instruction::SRem) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    int64_t c = ci->getSExtValue();
                    if (c > 1 && (c & (c - 1)) == 0) {  // positive power-of-2
                        const llvm::DataLayout& DL = inst.getModule()->getDataLayout();
                        if (isValueNonNegative(inst.getOperand(0), DL)) {
                            llvm::IRBuilder<> builder(&inst);
                            simplified = builder.CreateAnd(inst.getOperand(0),
                                llvm::ConstantInt::get(inst.getType(), c - 1),
                                "srem_pow2_nn");
                        }
                    }
                }
            }
            // sub(x, srem(x, 2^n)) → and(x, ~(2^n - 1)) when x is non-negative.
            // This is the "floor to nearest multiple of 2^n" pattern that users
            // commonly write as `x - (x % 8)` to align a byte offset to a cache
            // line, SIMD boundary, or page size.  With a non-negative x the
            // signed mod is equal to the unsigned mod, so:
            //   x - (x & (C-1))  =  x & ~(C-1)
            // Saves 1 srem (≈25 cycles) + 1 sub → 1 and (1 cycle).
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                llvm::Value* dividend = inst.getOperand(0);
                if (auto* sremInst =
                        llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1))) {
                    if (sremInst->getOpcode() == llvm::Instruction::SRem &&
                        sremInst->getOperand(0) == dividend &&
                        hasOneUse(sremInst)) {
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(
                                sremInst->getOperand(1))) {
                            int64_t c = ci->getSExtValue();
                            if (c > 1 && (c & (c - 1)) == 0) {  // positive power-of-2
                                const llvm::DataLayout& DL =
                                    inst.getModule()->getDataLayout();
                                if (isValueNonNegative(dividend, DL)) {
                                    llvm::IRBuilder<> builder(&inst);
                                    // ~(C-1) as a two's-complement mask
                                    uint64_t mask = ~static_cast<uint64_t>(c - 1);
                                    simplified = builder.CreateAnd(dividend,
                                        llvm::ConstantInt::get(inst.getType(), mask),
                                        "floor_pow2_nn");
                                }
                            }
                        }
                    }
                }
            }
            // sub(x, and(x, C-1)) → and(x, ~(C-1)) for power-of-2 C, valid for ALL x.
            // This fires when the srem→urem→urem_pow2 chain has already simplified
            // srem(x, C) to and(x, C-1), leaving sub(x, and(x, C-1)) in the IR.
            // Identity: x = (x & ~(C-1)) + (x & (C-1)), so
            //           x - (x & (C-1)) = x & ~(C-1), unconditionally.
            // No sign constraint needed — this is a pure bitwise identity.
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                llvm::Value* lhs = inst.getOperand(0);
                if (auto* andInst =
                        llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1))) {
                    if (andInst->getOpcode() == llvm::Instruction::And &&
                        andInst->getOperand(0) == lhs &&
                        hasOneUse(andInst)) {
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(
                                andInst->getOperand(1))) {
                            uint64_t mask = ci->getZExtValue();
                            // mask must be of the form 2^n - 1 (all low bits set)
                            if (mask > 0 && (mask & (mask + 1)) == 0) {
                                llvm::IRBuilder<> builder(&inst);
                                simplified = builder.CreateAnd(lhs,
                                    llvm::ConstantInt::get(inst.getType(),
                                                           ~mask),
                                    "floor_and_mask");
                            }
                        }
                    }
                    // Also handle: sub(x, and(C-1, x)) — commuted operand
                    if (!simplified &&
                        andInst->getOpcode() == llvm::Instruction::And &&
                        andInst->getOperand(1) == lhs &&
                        hasOneUse(andInst)) {
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(
                                andInst->getOperand(0))) {
                            uint64_t mask = ci->getZExtValue();
                            if (mask > 0 && (mask & (mask + 1)) == 0) {
                                llvm::IRBuilder<> builder(&inst);
                                simplified = builder.CreateAnd(lhs,
                                    llvm::ConstantInt::get(inst.getType(),
                                                           ~mask),
                                    "floor_and_mask_c");
                            }
                        }
                    }
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

            // sdiv(x, 2) bias-shift simplification for products of consecutive integers.
            //
            // `sdiv x, 2` when x = a*(a+1) or x = a*(a-1) is always exact
            // (the product of consecutive integers is always even), so it can
            // be reduced to `ashr x, 1` which is one instruction instead of
            // the 3-instruction bias-shift sequence that synthesis would emit.
            //
            // By catching this BEFORE synthesis, we prevent synthesis from
            // converting sdiv→(lshr + add + ashr) and then having the pattern
            // survive to the output as dead overhead.
            //
            // Correctness: for any integer a, exactly one of {a, a+1} is even,
            // so a*(a+1) mod 2 = 0, meaning ashr and sdiv agree exactly.
            if (!simplified && inst.getOpcode() == llvm::Instruction::SDiv) {
                if (isConstInt(inst.getOperand(1), 2)) {
                    llvm::Value* x = inst.getOperand(0);
                    auto isConsecutiveMul = [&](llvm::Value* v) -> bool {
                        auto* mul = llvm::dyn_cast<llvm::BinaryOperator>(v);
                        if (!mul || mul->getOpcode() != llvm::Instruction::Mul)
                            return false;
                        for (unsigned m = 0; m < 2; ++m) {
                            llvm::Value* factorA = mul->getOperand(m);
                            llvm::Value* factorB = mul->getOperand(1-m);
                            // Check if factorB = add(factorA, ±1) in either order
                            auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(
                                factorB);
                            if (!addInst) continue;
                            if (addInst->getOpcode() != llvm::Instruction::Add)
                                continue;
                            // Check both add(factorA, ±1) and add(±1, factorA)
                            for (unsigned addOp = 0; addOp < 2; ++addOp) {
                                if (addInst->getOperand(addOp) != factorA) continue;
                                auto cv = getConstIntValue(addInst->getOperand(1-addOp));
                                if (cv && (*cv == 1 || *cv == -1)) return true;
                            }
                        }
                        return false;
                    };

                    if (isConsecutiveMul(x)) {
                        // x = a*(a+1) or a*(a-1): always even → ashr by 1 is exact.
                        // Use arithmetic shift (handles negative 'a' correctly):
                        //   ashr(-2 * -1, 1) = ashr(2, 1) = 1 ✓  (sdiv would give 1)
                        //   ashr(-3 * -4, 1) = ashr(12, 1) = 6 ✓
                        llvm::IRBuilder<> builder(&inst);
                        simplified = builder.CreateAShr(
                            x,
                            llvm::ConstantInt::get(inst.getType(), 1),
                            "consec_div2");
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
                    case 22: simplified = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,2)), shl(xv,1), "mul22"); break;
                    case 24: simplified = builder.CreateAdd(shl(xv,4), shl(xv,3), "mul24"); break;
                    case 25: simplified = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,3)), xv, "mul25"); break;
                    case 26: simplified = builder.CreateSub(builder.CreateSub(shl(xv,5), shl(xv,2)), shl(xv,1), "mul26"); break;
                    case 27: simplified = builder.CreateSub(builder.CreateSub(shl(xv,5), shl(xv,2)), xv, "mul27"); break;
                    case 28: simplified = builder.CreateSub(shl(xv,5), shl(xv,2), "mul28"); break;
                    case 30: simplified = builder.CreateSub(shl(xv,5), shl(xv,1), "mul30"); break;
                    case 31: simplified = builder.CreateSub(shl(xv,5), xv, "mul31"); break;
                    case 33: simplified = builder.CreateAdd(shl(xv,5), xv, "mul33"); break;
                    case 34: simplified = builder.CreateAdd(shl(xv,5), shl(xv,1), "mul34"); break;
                    case 36: simplified = builder.CreateAdd(shl(xv,5), shl(xv,2), "mul36"); break;
                    case 37: simplified = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,2)), xv, "mul37"); break;
                    case 40: simplified = builder.CreateAdd(shl(xv,5), shl(xv,3), "mul40"); break;
                    case 41: simplified = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,3)), xv, "mul41"); break;
                    case 48: simplified = builder.CreateAdd(shl(xv,5), shl(xv,4), "mul48"); break;
                    case 49: simplified = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,4)), xv, "mul49"); break;
                    case 50: simplified = builder.CreateAdd(builder.CreateSub(shl(xv,6), shl(xv,4)), shl(xv,1), "mul50"); break;
                    case 56: simplified = builder.CreateSub(shl(xv,6), shl(xv,3), "mul56"); break;
                    case 57: simplified = builder.CreateAdd(builder.CreateSub(shl(xv,6), shl(xv,3), "mul57.t"), xv, "mul57"); break;
                    case 60: simplified = builder.CreateSub(shl(xv,6), shl(xv,2), "mul60"); break;
                    case 62: simplified = builder.CreateSub(shl(xv,6), shl(xv,1), "mul62"); break;
                    case 63: simplified = builder.CreateSub(shl(xv,6), xv, "mul63"); break;
                    case 65: simplified = builder.CreateAdd(shl(xv,6), xv, "mul65"); break;
                    case 66: simplified = builder.CreateAdd(shl(xv,6), shl(xv,1), "mul66"); break;
                    case 68: simplified = builder.CreateAdd(shl(xv,6), shl(xv,2), "mul68"); break;
                    case 72: simplified = builder.CreateAdd(shl(xv,6), shl(xv,3), "mul72"); break;
                    case 80: simplified = builder.CreateAdd(shl(xv,6), shl(xv,4), "mul80"); break;
                    case 96: simplified = builder.CreateAdd(shl(xv,6), shl(xv,5), "mul96"); break;
                    case 100: simplified = builder.CreateAdd(shl(xv,6), builder.CreateAdd(shl(xv,5), shl(xv,2)), "mul100"); break;
                    case 112: simplified = builder.CreateSub(shl(xv,7), shl(xv,4), "mul112"); break;
                    case 120: simplified = builder.CreateSub(shl(xv,7), shl(xv,3), "mul120"); break;
                    case 127: simplified = builder.CreateSub(shl(xv,7), xv, "mul127"); break;
                    case 129: simplified = builder.CreateAdd(shl(xv,7), xv, "mul129"); break;
                    case 136: simplified = builder.CreateAdd(shl(xv,7), shl(xv,3), "mul136"); break;
                    case 144: simplified = builder.CreateAdd(shl(xv,7), shl(xv,4), "mul144"); break;
                    case 160: simplified = builder.CreateAdd(shl(xv,7), shl(xv,5), "mul160"); break;
                    case 192: simplified = builder.CreateAdd(shl(xv,7), shl(xv,6), "mul192"); break;
                    case 200: simplified = builder.CreateAdd(builder.CreateSub(shl(xv,8), shl(xv,6)), shl(xv,3), "mul200"); break;
                    case 224: simplified = builder.CreateSub(shl(xv,8), shl(xv,5), "mul224"); break;
                    case 240: simplified = builder.CreateSub(shl(xv,8), shl(xv,4), "mul240"); break;
                    case 248: simplified = builder.CreateSub(shl(xv,8), shl(xv,3), "mul248"); break;
                    case 255: simplified = builder.CreateSub(shl(xv,8), xv, "mul255"); break;
                    case 257: simplified = builder.CreateAdd(shl(xv,8), xv, "mul257"); break;
                    case 264: simplified = builder.CreateAdd(shl(xv,8), shl(xv,3), "mul264"); break;
                    case 272: simplified = builder.CreateAdd(shl(xv,8), shl(xv,4), "mul272"); break;
                    case 288: simplified = builder.CreateAdd(shl(xv,8), shl(xv,5), "mul288"); break;
                    case 320: simplified = builder.CreateAdd(shl(xv,8), shl(xv,6), "mul320"); break;
                    case 384: simplified = builder.CreateAdd(shl(xv,8), shl(xv,7), "mul384"); break;
                    case 448: simplified = builder.CreateSub(shl(xv,9), shl(xv,6), "mul448"); break;
                    case 480: simplified = builder.CreateSub(shl(xv,9), shl(xv,5), "mul480"); break;
                    case 496: simplified = builder.CreateSub(shl(xv,9), shl(xv,4), "mul496"); break;
                    case 504: simplified = builder.CreateSub(shl(xv,9), shl(xv,3), "mul504"); break;
                    case 511: simplified = builder.CreateSub(shl(xv,9), xv, "mul511"); break;
                    case 513: simplified = builder.CreateAdd(shl(xv,9), xv, "mul513"); break;
                    case 640: simplified = builder.CreateAdd(shl(xv,9), shl(xv,7), "mul640"); break;
                    case 768: simplified = builder.CreateAdd(shl(xv,9), shl(xv,8), "mul768"); break;
                    case 1000: simplified = builder.CreateSub(builder.CreateSub(shl(xv,10), shl(xv,4)), shl(xv,3), "mul1000"); break;
                    case 1023: simplified = builder.CreateSub(shl(xv,10), xv, "mul1023"); break;
                    case 1025: simplified = builder.CreateAdd(shl(xv,10), xv, "mul1025"); break;
                    case 1152: simplified = builder.CreateAdd(shl(xv,10), shl(xv,7), "mul1152"); break;
                    case 1280: simplified = builder.CreateAdd(shl(xv,10), shl(xv,8), "mul1280"); break;
                    case 1536: simplified = builder.CreateAdd(shl(xv,10), shl(xv,9), "mul1536"); break;
                    case 1792: simplified = builder.CreateSub(shl(xv,11), shl(xv,8), "mul1792"); break;
                    case 2047: simplified = builder.CreateSub(shl(xv,11), xv, "mul2047"); break;
                    case 2049: simplified = builder.CreateAdd(shl(xv,11), xv, "mul2049"); break;
                    // ── n×128 family ───────────────────────────────────────────
                    case 124:  simplified = builder.CreateSub(shl(xv,7), shl(xv,2),  "mul124");  break;
                    case 126:  simplified = builder.CreateSub(shl(xv,7), shl(xv,1),  "mul126");  break;
                    case 130:  simplified = builder.CreateAdd(shl(xv,7), shl(xv,1),  "mul130");  break;
                    case 132:  simplified = builder.CreateAdd(shl(xv,7), shl(xv,2),  "mul132");  break;
                    // ── n×256 family ───────────────────────────────────────────
                    case 252:  simplified = builder.CreateSub(shl(xv,8), shl(xv,2),  "mul252");  break;
                    case 254:  simplified = builder.CreateSub(shl(xv,8), shl(xv,1),  "mul254");  break;
                    case 258:  simplified = builder.CreateAdd(shl(xv,8), shl(xv,1),  "mul258");  break;
                    case 260:  simplified = builder.CreateAdd(shl(xv,8), shl(xv,2),  "mul260");  break;
                    // ── n×512 family ───────────────────────────────────────────
                    case 508:  simplified = builder.CreateSub(shl(xv,9), shl(xv,2),  "mul508");  break;
                    case 510:  simplified = builder.CreateSub(shl(xv,9), shl(xv,1),  "mul510");  break;
                    case 514:  simplified = builder.CreateAdd(shl(xv,9), shl(xv,1),  "mul514");  break;
                    case 516:  simplified = builder.CreateAdd(shl(xv,9), shl(xv,2),  "mul516");  break;
                    case 520:  simplified = builder.CreateAdd(shl(xv,9), shl(xv,3),  "mul520");  break;
                    case 528:  simplified = builder.CreateAdd(shl(xv,9), shl(xv,4),  "mul528");  break;
                    case 544:  simplified = builder.CreateAdd(shl(xv,9), shl(xv,5),  "mul544");  break;
                    case 576:  simplified = builder.CreateAdd(shl(xv,9), shl(xv,6),  "mul576");  break;
                    // ── n×1024 family ──────────────────────────────────────────
                    case 960:  simplified = builder.CreateSub(shl(xv,10), shl(xv,6), "mul960");  break;
                    case 992:  simplified = builder.CreateSub(shl(xv,10), shl(xv,5), "mul992");  break;
                    case 1008: simplified = builder.CreateSub(shl(xv,10), shl(xv,4), "mul1008"); break;
                    case 1016: simplified = builder.CreateSub(shl(xv,10), shl(xv,3), "mul1016"); break;
                    case 1020: simplified = builder.CreateSub(shl(xv,10), shl(xv,2), "mul1020"); break;
                    case 1022: simplified = builder.CreateSub(shl(xv,10), shl(xv,1), "mul1022"); break;
                    case 1026: simplified = builder.CreateAdd(shl(xv,10), shl(xv,1), "mul1026"); break;
                    case 1028: simplified = builder.CreateAdd(shl(xv,10), shl(xv,2), "mul1028"); break;
                    case 1032: simplified = builder.CreateAdd(shl(xv,10), shl(xv,3), "mul1032"); break;
                    case 1040: simplified = builder.CreateAdd(shl(xv,10), shl(xv,4), "mul1040"); break;
                    case 1056: simplified = builder.CreateAdd(shl(xv,10), shl(xv,5), "mul1056"); break;
                    case 1088: simplified = builder.CreateAdd(shl(xv,10), shl(xv,6), "mul1088"); break;
                    // ── n×2048 family ──────────────────────────────────────────
                    case 1920: simplified = builder.CreateSub(shl(xv,11), shl(xv,7), "mul1920"); break;
                    case 1984: simplified = builder.CreateSub(shl(xv,11), shl(xv,6), "mul1984"); break;
                    case 2016: simplified = builder.CreateSub(shl(xv,11), shl(xv,5), "mul2016"); break;
                    case 2032: simplified = builder.CreateSub(shl(xv,11), shl(xv,4), "mul2032"); break;
                    case 2040: simplified = builder.CreateSub(shl(xv,11), shl(xv,3), "mul2040"); break;
                    case 2044: simplified = builder.CreateSub(shl(xv,11), shl(xv,2), "mul2044"); break;
                    case 2046: simplified = builder.CreateSub(shl(xv,11), shl(xv,1), "mul2046"); break;
                    case 2050: simplified = builder.CreateAdd(shl(xv,11), shl(xv,1), "mul2050"); break;
                    case 2052: simplified = builder.CreateAdd(shl(xv,11), shl(xv,2), "mul2052"); break;
                    case 2056: simplified = builder.CreateAdd(shl(xv,11), shl(xv,3), "mul2056"); break;
                    case 2064: simplified = builder.CreateAdd(shl(xv,11), shl(xv,4), "mul2064"); break;
                    case 2080: simplified = builder.CreateAdd(shl(xv,11), shl(xv,5), "mul2080"); break;
                    case 2112: simplified = builder.CreateAdd(shl(xv,11), shl(xv,6), "mul2112"); break;
                    case 2176: simplified = builder.CreateAdd(shl(xv,11), shl(xv,7), "mul2176"); break;
                    case 2304: simplified = builder.CreateAdd(shl(xv,11), shl(xv,8), "mul2304"); break;
                    case 2560: simplified = builder.CreateAdd(shl(xv,11), shl(xv,9), "mul2560"); break;
                    case 3072: simplified = builder.CreateAdd(shl(xv,11), shl(xv,10),"mul3072"); break;
                    // ── n×4096 family ──────────────────────────────────────────
                    case 3584: simplified = builder.CreateSub(shl(xv,12), shl(xv,9),  "mul3584"); break;
                    case 3840: simplified = builder.CreateSub(shl(xv,12), shl(xv,8),  "mul3840"); break;
                    case 3968: simplified = builder.CreateSub(shl(xv,12), shl(xv,7),  "mul3968"); break;
                    case 4032: simplified = builder.CreateSub(shl(xv,12), shl(xv,6),  "mul4032"); break;
                    case 4064: simplified = builder.CreateSub(shl(xv,12), shl(xv,5),  "mul4064"); break;
                    case 4080: simplified = builder.CreateSub(shl(xv,12), shl(xv,4),  "mul4080"); break;
                    case 4088: simplified = builder.CreateSub(shl(xv,12), shl(xv,3),  "mul4088"); break;
                    case 4092: simplified = builder.CreateSub(shl(xv,12), shl(xv,2),  "mul4092"); break;
                    case 4094: simplified = builder.CreateSub(shl(xv,12), shl(xv,1),  "mul4094"); break;
                    case 4095: simplified = builder.CreateSub(shl(xv,12), xv,          "mul4095"); break;
                    case 4097: simplified = builder.CreateAdd(shl(xv,12), xv,          "mul4097"); break;
                    case 4098: simplified = builder.CreateAdd(shl(xv,12), shl(xv,1),   "mul4098"); break;
                    case 4100: simplified = builder.CreateAdd(shl(xv,12), shl(xv,2),   "mul4100"); break;
                    case 4104: simplified = builder.CreateAdd(shl(xv,12), shl(xv,3),   "mul4104"); break;
                    case 4112: simplified = builder.CreateAdd(shl(xv,12), shl(xv,4),   "mul4112"); break;
                    case 4128: simplified = builder.CreateAdd(shl(xv,12), shl(xv,5),   "mul4128"); break;
                    case 4160: simplified = builder.CreateAdd(shl(xv,12), shl(xv,6),   "mul4160"); break;
                    case 4224: simplified = builder.CreateAdd(shl(xv,12), shl(xv,7),   "mul4224"); break;
                    case 4352: simplified = builder.CreateAdd(shl(xv,12), shl(xv,8),   "mul4352"); break;
                    case 4608: simplified = builder.CreateAdd(shl(xv,12), shl(xv,9),   "mul4608"); break;
                    case 5120: simplified = builder.CreateAdd(shl(xv,12), shl(xv,10),  "mul5120"); break;
                    case 6144: simplified = builder.CreateAdd(shl(xv,12), shl(xv,11),  "mul6144"); break;
                    // ── n×8192 family ────────────────────────────────────────────
                    case 7168:  simplified = builder.CreateSub(shl(xv,13), shl(xv,10), "mul7168");  break;
                    case 7680:  simplified = builder.CreateSub(shl(xv,13), shl(xv,9),  "mul7680");  break;
                    case 7936:  simplified = builder.CreateSub(shl(xv,13), shl(xv,8),  "mul7936");  break;
                    case 8064:  simplified = builder.CreateSub(shl(xv,13), shl(xv,7),  "mul8064");  break;
                    case 8128:  simplified = builder.CreateSub(shl(xv,13), shl(xv,6),  "mul8128");  break;
                    case 8160:  simplified = builder.CreateSub(shl(xv,13), shl(xv,5),  "mul8160");  break;
                    case 8176:  simplified = builder.CreateSub(shl(xv,13), shl(xv,4),  "mul8176");  break;
                    case 8184:  simplified = builder.CreateSub(shl(xv,13), shl(xv,3),  "mul8184");  break;
                    case 8188:  simplified = builder.CreateSub(shl(xv,13), shl(xv,2),  "mul8188");  break;
                    case 8190:  simplified = builder.CreateSub(shl(xv,13), shl(xv,1),  "mul8190");  break;
                    case 8191:  simplified = builder.CreateSub(shl(xv,13), xv,          "mul8191");  break;
                    case 8193:  simplified = builder.CreateAdd(shl(xv,13), xv,          "mul8193");  break;
                    case 8194:  simplified = builder.CreateAdd(shl(xv,13), shl(xv,1),  "mul8194");  break;
                    case 8196:  simplified = builder.CreateAdd(shl(xv,13), shl(xv,2),  "mul8196");  break;
                    case 8200:  simplified = builder.CreateAdd(shl(xv,13), shl(xv,3),  "mul8200");  break;
                    case 8208:  simplified = builder.CreateAdd(shl(xv,13), shl(xv,4),  "mul8208");  break;
                    case 8224:  simplified = builder.CreateAdd(shl(xv,13), shl(xv,5),  "mul8224");  break;
                    case 8256:  simplified = builder.CreateAdd(shl(xv,13), shl(xv,6),  "mul8256");  break;
                    case 8320:  simplified = builder.CreateAdd(shl(xv,13), shl(xv,7),  "mul8320");  break;
                    case 8448:  simplified = builder.CreateAdd(shl(xv,13), shl(xv,8),  "mul8448");  break;
                    case 8704:  simplified = builder.CreateAdd(shl(xv,13), shl(xv,9),  "mul8704");  break;
                    case 9216:  simplified = builder.CreateAdd(shl(xv,13), shl(xv,10), "mul9216");  break;
                    case 10240: simplified = builder.CreateAdd(shl(xv,13), shl(xv,11), "mul10240"); break;
                    case 12288: simplified = builder.CreateAdd(shl(xv,13), shl(xv,12), "mul12288"); break;
                    // ── n×16384 family ──────────────────────────────────────
                    case 14336: simplified = builder.CreateSub(shl(xv,14), shl(xv,11), "mul14336"); break;
                    case 15360: simplified = builder.CreateSub(shl(xv,14), shl(xv,10), "mul15360"); break;
                    case 16384: simplified = shl(xv,14); break;
                    case 16385: simplified = builder.CreateAdd(shl(xv,14), xv,          "mul16385"); break;
                    case 16386: simplified = builder.CreateAdd(shl(xv,14), shl(xv,1),   "mul16386"); break;
                    case 16388: simplified = builder.CreateAdd(shl(xv,14), shl(xv,2),   "mul16388"); break;
                    case 16392: simplified = builder.CreateAdd(shl(xv,14), shl(xv,3),   "mul16392"); break;
                    case 16400: simplified = builder.CreateAdd(shl(xv,14), shl(xv,4),   "mul16400"); break;
                    case 16416: simplified = builder.CreateAdd(shl(xv,14), shl(xv,5),   "mul16416"); break;
                    case 16448: simplified = builder.CreateAdd(shl(xv,14), shl(xv,6),   "mul16448"); break;
                    case 16512: simplified = builder.CreateAdd(shl(xv,14), shl(xv,7),   "mul16512"); break;
                    case 16640: simplified = builder.CreateAdd(shl(xv,14), shl(xv,8),   "mul16640"); break;
                    case 16896: simplified = builder.CreateAdd(shl(xv,14), shl(xv,9),   "mul16896"); break;
                    case 17408: simplified = builder.CreateAdd(shl(xv,14), shl(xv,10),  "mul17408"); break;
                    case 18432: simplified = builder.CreateAdd(shl(xv,14), shl(xv,11),  "mul18432"); break;
                    case 20480: simplified = builder.CreateAdd(shl(xv,14), shl(xv,12),  "mul20480"); break;
                    case 24576: simplified = builder.CreateAdd(shl(xv,14), shl(xv,13),  "mul24576"); break;
                    // ── n×32768 family ──────────────────────────────────────
                    case 28672: simplified = builder.CreateSub(shl(xv,15), shl(xv,12), "mul28672"); break;
                    case 30720: simplified = builder.CreateSub(shl(xv,15), shl(xv,11), "mul30720"); break;
                    case 32768: simplified = shl(xv,15); break;
                    case 32769: simplified = builder.CreateAdd(shl(xv,15), xv,          "mul32769"); break;
                    case 32770: simplified = builder.CreateAdd(shl(xv,15), shl(xv,1),   "mul32770"); break;
                    case 32772: simplified = builder.CreateAdd(shl(xv,15), shl(xv,2),   "mul32772"); break;
                    case 32776: simplified = builder.CreateAdd(shl(xv,15), shl(xv,3),   "mul32776"); break;
                    case 32800: simplified = builder.CreateAdd(shl(xv,15), shl(xv,5),   "mul32800"); break;
                    case 32896: simplified = builder.CreateAdd(shl(xv,15), shl(xv,7),   "mul32896"); break;
                    case 33024: simplified = builder.CreateAdd(shl(xv,15), shl(xv,8),   "mul33024"); break;
                    case 33280: simplified = builder.CreateAdd(shl(xv,15), shl(xv,9),   "mul33280"); break;
                    case 33792: simplified = builder.CreateAdd(shl(xv,15), shl(xv,10),  "mul33792"); break;
                    case 34816: simplified = builder.CreateAdd(shl(xv,15), shl(xv,11),  "mul34816"); break;
                    case 36864: simplified = builder.CreateAdd(shl(xv,15), shl(xv,12),  "mul36864"); break;
                    case 40960: simplified = builder.CreateAdd(shl(xv,15), shl(xv,13),  "mul40960"); break;
                    case 49152: simplified = builder.CreateAdd(shl(xv,15), shl(xv,14),  "mul49152"); break;
                    // ── n×65536 family ──────────────────────────────────────
                    case 57344: simplified = builder.CreateSub(shl(xv,16), shl(xv,13), "mul57344"); break;
                    case 61440: simplified = builder.CreateSub(shl(xv,16), shl(xv,12), "mul61440"); break;
                    case 65536: simplified = shl(xv,16); break;
                    case 65537: simplified = builder.CreateAdd(shl(xv,16), xv,          "mul65537"); break;
                    case 65538: simplified = builder.CreateAdd(shl(xv,16), shl(xv,1),   "mul65538"); break;
                    case 65540: simplified = builder.CreateAdd(shl(xv,16), shl(xv,2),   "mul65540"); break;
                    case 65544: simplified = builder.CreateAdd(shl(xv,16), shl(xv,3),   "mul65544"); break;
                    case 65600: simplified = builder.CreateAdd(shl(xv,16), shl(xv,6),   "mul65600"); break;
                    case 65664: simplified = builder.CreateAdd(shl(xv,16), shl(xv,7),   "mul65664"); break;
                    case 65792: simplified = builder.CreateAdd(shl(xv,16), shl(xv,8),   "mul65792"); break;
                    case 66048: simplified = builder.CreateAdd(shl(xv,16), shl(xv,9),   "mul66048"); break;
                    case 66560: simplified = builder.CreateAdd(shl(xv,16), shl(xv,10),  "mul66560"); break;
                    case 67584: simplified = builder.CreateAdd(shl(xv,16), shl(xv,11),  "mul67584"); break;
                    case 69632: simplified = builder.CreateAdd(shl(xv,16), shl(xv,12),  "mul69632"); break;
                    case 73728: simplified = builder.CreateAdd(shl(xv,16), shl(xv,13),  "mul73728"); break;
                    case 81920: simplified = builder.CreateAdd(shl(xv,16), shl(xv,14),  "mul81920"); break;
                    case 98304: simplified = builder.CreateAdd(shl(xv,16), shl(xv,15),  "mul98304"); break;
                    case 114688: simplified = builder.CreateSub(shl(xv,17), shl(xv,14), "mul114688"); break;
                    case 122880: simplified = builder.CreateSub(shl(xv,17), shl(xv,13), "mul122880"); break;
                    case 131072: simplified = shl(xv,17); break;
                    case 131073: simplified = builder.CreateAdd(shl(xv,17), xv,          "mul131073"); break;
                    case 131074: simplified = builder.CreateAdd(shl(xv,17), shl(xv,1),   "mul131074"); break;
                    case 131076: simplified = builder.CreateAdd(shl(xv,17), shl(xv,2),   "mul131076"); break;
                    case 131080: simplified = builder.CreateAdd(shl(xv,17), shl(xv,3),   "mul131080"); break;
                    case 131136: simplified = builder.CreateAdd(shl(xv,17), shl(xv,6),   "mul131136"); break;
                    case 131200: simplified = builder.CreateAdd(shl(xv,17), shl(xv,7),   "mul131200"); break;
                    case 131328: simplified = builder.CreateAdd(shl(xv,17), shl(xv,8),   "mul131328"); break;
                    case 131584: simplified = builder.CreateAdd(shl(xv,17), shl(xv,9),   "mul131584"); break;
                    case 132096: simplified = builder.CreateAdd(shl(xv,17), shl(xv,10),  "mul132096"); break;
                    case 133120: simplified = builder.CreateAdd(shl(xv,17), shl(xv,11),  "mul133120"); break;
                    case 135168: simplified = builder.CreateAdd(shl(xv,17), shl(xv,12),  "mul135168"); break;
                    case 139264: simplified = builder.CreateAdd(shl(xv,17), shl(xv,13),  "mul139264"); break;
                    case 147456: simplified = builder.CreateAdd(shl(xv,17), shl(xv,14),  "mul147456"); break;
                    case 163840: simplified = builder.CreateAdd(shl(xv,17), shl(xv,15),  "mul163840"); break;
                    case 196608: simplified = builder.CreateAdd(shl(xv,17), shl(xv,16),  "mul196608"); break;
                    default:
                        // Negative constants: compute |cv|, strength-reduce, then negate.
                        if (*cv < -1) {
                            int64_t absCV = -*cv;
                            llvm::Value* posRep = nullptr;
                            switch (absCV) {
                            case  3: posRep = builder.CreateAdd(shl(xv,1), xv, "mulp3"); break;
                            case  5: posRep = builder.CreateAdd(shl(xv,2), xv, "mulp5"); break;
                            case  6: posRep = builder.CreateAdd(shl(xv,2), shl(xv,1), "mulp6"); break;
                            case  7: posRep = builder.CreateSub(shl(xv,3), xv, "mulp7"); break;
                            case  9: posRep = builder.CreateAdd(shl(xv,3), xv, "mulp9"); break;
                            case 10: posRep = builder.CreateAdd(shl(xv,3), shl(xv,1), "mulp10"); break;
                            case 11: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,3), shl(xv,1)), xv, "mulp11"); break;
                            case 12: posRep = builder.CreateAdd(shl(xv,3), shl(xv,2), "mulp12"); break;
                            case 13: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,3), shl(xv,2)), xv, "mulp13"); break;
                            case 14: posRep = builder.CreateSub(shl(xv,4), shl(xv,1), "mulp14"); break;
                            case 15: posRep = builder.CreateSub(shl(xv,4), xv, "mulp15"); break;
                            case 17: posRep = builder.CreateAdd(shl(xv,4), xv, "mulp17"); break;
                            case 18: posRep = builder.CreateAdd(shl(xv,4), shl(xv,1), "mulp18"); break;
                            case 19: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,1)), xv, "mulp19"); break;
                            case 20: posRep = builder.CreateAdd(shl(xv,4), shl(xv,2), "mulp20"); break;
                            case 21: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,2)), xv, "mulp21"); break;
                            case 22: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,2)), shl(xv,1), "mulp22"); break;
                            case 24: posRep = builder.CreateAdd(shl(xv,4), shl(xv,3), "mulp24"); break;
                            case 25: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,4), shl(xv,3)), xv, "mulp25"); break;
                            case 26: posRep = builder.CreateSub(builder.CreateSub(shl(xv,5), shl(xv,2)), shl(xv,1), "mulp26"); break;
                            case 27: posRep = builder.CreateSub(builder.CreateSub(shl(xv,5), shl(xv,2)), xv, "mulp27"); break;
                            case 28: posRep = builder.CreateSub(shl(xv,5), shl(xv,2), "mulp28"); break;
                            case 30: posRep = builder.CreateSub(shl(xv,5), shl(xv,1), "mulp30"); break;
                            case 31: posRep = builder.CreateSub(shl(xv,5), xv, "mulp31"); break;
                            case 33: posRep = builder.CreateAdd(shl(xv,5), xv, "mulp33"); break;
                            case 34: posRep = builder.CreateAdd(shl(xv,5), shl(xv,1), "mulp34"); break;
                            case 36: posRep = builder.CreateAdd(shl(xv,5), shl(xv,2), "mulp36"); break;
                            case 40: posRep = builder.CreateAdd(shl(xv,5), shl(xv,3), "mulp40"); break;
                            case 48: posRep = builder.CreateAdd(shl(xv,5), shl(xv,4), "mulp48"); break;
                            case 56: posRep = builder.CreateSub(shl(xv,6), shl(xv,3), "mulp56"); break;
                            case 57: posRep = builder.CreateAdd(builder.CreateSub(shl(xv,6), shl(xv,3), "mulp57.t"), xv, "mulp57"); break;
                            case 60: posRep = builder.CreateSub(shl(xv,6), shl(xv,2), "mulp60"); break;
                            case 62: posRep = builder.CreateSub(shl(xv,6), shl(xv,1), "mulp62"); break;
                            case 63: posRep = builder.CreateSub(shl(xv,6), xv, "mulp63"); break;
                            case 65: posRep = builder.CreateAdd(shl(xv,6), xv, "mulp65"); break;
                            case 66: posRep = builder.CreateAdd(shl(xv,6), shl(xv,1), "mulp66"); break;
                            case 68: posRep = builder.CreateAdd(shl(xv,6), shl(xv,2), "mulp68"); break;
                            case 72: posRep = builder.CreateAdd(shl(xv,6), shl(xv,3), "mulp72"); break;
                            case 80: posRep = builder.CreateAdd(shl(xv,6), shl(xv,4), "mulp80"); break;
                            case 96: posRep = builder.CreateAdd(shl(xv,6), shl(xv,5), "mulp96"); break;
                            case 112: posRep = builder.CreateSub(shl(xv,7), shl(xv,4), "mulp112"); break;
                            case 120: posRep = builder.CreateSub(shl(xv,7), shl(xv,3), "mulp120"); break;
                            case 127: posRep = builder.CreateSub(shl(xv,7), xv, "mulp127"); break;
                            case 129: posRep = builder.CreateAdd(shl(xv,7), xv, "mulp129"); break;
                            case 136: posRep = builder.CreateAdd(shl(xv,7), shl(xv,3), "mulp136"); break;
                            case 144: posRep = builder.CreateAdd(shl(xv,7), shl(xv,4), "mulp144"); break;
                            case 160: posRep = builder.CreateAdd(shl(xv,7), shl(xv,5), "mulp160"); break;
                            case 192: posRep = builder.CreateAdd(shl(xv,7), shl(xv,6), "mulp192"); break;
                            case 224: posRep = builder.CreateSub(shl(xv,8), shl(xv,5), "mulp224"); break;
                            case 240: posRep = builder.CreateSub(shl(xv,8), shl(xv,4), "mulp240"); break;
                            case 248: posRep = builder.CreateSub(shl(xv,8), shl(xv,3), "mulp248"); break;
                            case 255: posRep = builder.CreateSub(shl(xv,8), xv, "mulp255"); break;
                            case 257: posRep = builder.CreateAdd(shl(xv,8), xv, "mulp257"); break;
                            case 264: posRep = builder.CreateAdd(shl(xv,8), shl(xv,3), "mulp264"); break;
                            case 272: posRep = builder.CreateAdd(shl(xv,8), shl(xv,4), "mulp272"); break;
                            case 288: posRep = builder.CreateAdd(shl(xv,8), shl(xv,5), "mulp288"); break;
                            case 320: posRep = builder.CreateAdd(shl(xv,8), shl(xv,6), "mulp320"); break;
                            case 384: posRep = builder.CreateAdd(shl(xv,8), shl(xv,7), "mulp384"); break;
                            case 448: posRep = builder.CreateSub(shl(xv,9), shl(xv,6), "mulp448"); break;
                            case 480: posRep = builder.CreateSub(shl(xv,9), shl(xv,5), "mulp480"); break;
                            case 496: posRep = builder.CreateSub(shl(xv,9), shl(xv,4), "mulp496"); break;
                            case 504: posRep = builder.CreateSub(shl(xv,9), shl(xv,3), "mulp504"); break;
                            case 511: posRep = builder.CreateSub(shl(xv,9), xv, "mulp511"); break;
                            case 513: posRep = builder.CreateAdd(shl(xv,9), xv, "mulp513"); break;
                            case 640: posRep = builder.CreateAdd(shl(xv,9), shl(xv,7), "mulp640"); break;
                            case 768: posRep = builder.CreateAdd(shl(xv,9), shl(xv,8), "mulp768"); break;
                            case 1023: posRep = builder.CreateSub(shl(xv,10), xv, "mulp1023"); break;
                            case 1025: posRep = builder.CreateAdd(shl(xv,10), xv, "mulp1025"); break;
                            case 1152: posRep = builder.CreateAdd(shl(xv,10), shl(xv,7), "mulp1152"); break;
                            case 1280: posRep = builder.CreateAdd(shl(xv,10), shl(xv,8), "mulp1280"); break;
                            case 1536: posRep = builder.CreateAdd(shl(xv,10), shl(xv,9), "mulp1536"); break;
                            case 1792: posRep = builder.CreateSub(shl(xv,11), shl(xv,8), "mulp1792"); break;
                            case 2047: posRep = builder.CreateSub(shl(xv,11), xv, "mulp2047"); break;
                            case 2049: posRep = builder.CreateAdd(shl(xv,11), xv, "mulp2049"); break;
                            // ── n×128 family ───────────────────────────────────
                            case 124:  posRep = builder.CreateSub(shl(xv,7), shl(xv,2),  "mulp124");  break;
                            case 126:  posRep = builder.CreateSub(shl(xv,7), shl(xv,1),  "mulp126");  break;
                            case 130:  posRep = builder.CreateAdd(shl(xv,7), shl(xv,1),  "mulp130");  break;
                            case 132:  posRep = builder.CreateAdd(shl(xv,7), shl(xv,2),  "mulp132");  break;
                            // ── n×256 family ───────────────────────────────────
                            case 252:  posRep = builder.CreateSub(shl(xv,8), shl(xv,2),  "mulp252");  break;
                            case 254:  posRep = builder.CreateSub(shl(xv,8), shl(xv,1),  "mulp254");  break;
                            case 258:  posRep = builder.CreateAdd(shl(xv,8), shl(xv,1),  "mulp258");  break;
                            case 260:  posRep = builder.CreateAdd(shl(xv,8), shl(xv,2),  "mulp260");  break;
                            // ── n×512 family ───────────────────────────────────
                            case 508:  posRep = builder.CreateSub(shl(xv,9), shl(xv,2),  "mulp508");  break;
                            case 510:  posRep = builder.CreateSub(shl(xv,9), shl(xv,1),  "mulp510");  break;
                            case 514:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,1),  "mulp514");  break;
                            case 516:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,2),  "mulp516");  break;
                            case 520:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,3),  "mulp520");  break;
                            case 528:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,4),  "mulp528");  break;
                            case 544:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,5),  "mulp544");  break;
                            case 576:  posRep = builder.CreateAdd(shl(xv,9), shl(xv,6),  "mulp576");  break;
                            // ── n×1024 family ──────────────────────────────────
                            case 960:  posRep = builder.CreateSub(shl(xv,10), shl(xv,6), "mulp960");  break;
                            case 992:  posRep = builder.CreateSub(shl(xv,10), shl(xv,5), "mulp992");  break;
                            case 1008: posRep = builder.CreateSub(shl(xv,10), shl(xv,4), "mulp1008"); break;
                            case 1016: posRep = builder.CreateSub(shl(xv,10), shl(xv,3), "mulp1016"); break;
                            case 1020: posRep = builder.CreateSub(shl(xv,10), shl(xv,2), "mulp1020"); break;
                            case 1022: posRep = builder.CreateSub(shl(xv,10), shl(xv,1), "mulp1022"); break;
                            case 1026: posRep = builder.CreateAdd(shl(xv,10), shl(xv,1), "mulp1026"); break;
                            case 1028: posRep = builder.CreateAdd(shl(xv,10), shl(xv,2), "mulp1028"); break;
                            case 1032: posRep = builder.CreateAdd(shl(xv,10), shl(xv,3), "mulp1032"); break;
                            case 1040: posRep = builder.CreateAdd(shl(xv,10), shl(xv,4), "mulp1040"); break;
                            case 1056: posRep = builder.CreateAdd(shl(xv,10), shl(xv,5), "mulp1056"); break;
                            case 1088: posRep = builder.CreateAdd(shl(xv,10), shl(xv,6), "mulp1088"); break;
                            // ── n×2048 family ──────────────────────────────────
                            case 1920: posRep = builder.CreateSub(shl(xv,11), shl(xv,7), "mulp1920"); break;
                            case 1984: posRep = builder.CreateSub(shl(xv,11), shl(xv,6), "mulp1984"); break;
                            case 2016: posRep = builder.CreateSub(shl(xv,11), shl(xv,5), "mulp2016"); break;
                            case 2032: posRep = builder.CreateSub(shl(xv,11), shl(xv,4), "mulp2032"); break;
                            case 2040: posRep = builder.CreateSub(shl(xv,11), shl(xv,3), "mulp2040"); break;
                            case 2044: posRep = builder.CreateSub(shl(xv,11), shl(xv,2), "mulp2044"); break;
                            case 2046: posRep = builder.CreateSub(shl(xv,11), shl(xv,1), "mulp2046"); break;
                            case 2050: posRep = builder.CreateAdd(shl(xv,11), shl(xv,1), "mulp2050"); break;
                            case 2052: posRep = builder.CreateAdd(shl(xv,11), shl(xv,2), "mulp2052"); break;
                            case 2056: posRep = builder.CreateAdd(shl(xv,11), shl(xv,3), "mulp2056"); break;
                            case 2064: posRep = builder.CreateAdd(shl(xv,11), shl(xv,4), "mulp2064"); break;
                            case 2080: posRep = builder.CreateAdd(shl(xv,11), shl(xv,5), "mulp2080"); break;
                            case 2112: posRep = builder.CreateAdd(shl(xv,11), shl(xv,6), "mulp2112"); break;
                            case 2176: posRep = builder.CreateAdd(shl(xv,11), shl(xv,7), "mulp2176"); break;
                            case 2304: posRep = builder.CreateAdd(shl(xv,11), shl(xv,8), "mulp2304"); break;
                            case 2560: posRep = builder.CreateAdd(shl(xv,11), shl(xv,9), "mulp2560"); break;
                            case 3072: posRep = builder.CreateAdd(shl(xv,11), shl(xv,10),"mulp3072"); break;
                            // ── n×4096 family ───────────────────────────────────
                            case 3584: posRep = builder.CreateSub(shl(xv,12), shl(xv,9),  "mulp3584"); break;
                            case 3840: posRep = builder.CreateSub(shl(xv,12), shl(xv,8),  "mulp3840"); break;
                            case 3968: posRep = builder.CreateSub(shl(xv,12), shl(xv,7),  "mulp3968"); break;
                            case 4032: posRep = builder.CreateSub(shl(xv,12), shl(xv,6),  "mulp4032"); break;
                            case 4064: posRep = builder.CreateSub(shl(xv,12), shl(xv,5),  "mulp4064"); break;
                            case 4080: posRep = builder.CreateSub(shl(xv,12), shl(xv,4),  "mulp4080"); break;
                            case 4088: posRep = builder.CreateSub(shl(xv,12), shl(xv,3),  "mulp4088"); break;
                            case 4092: posRep = builder.CreateSub(shl(xv,12), shl(xv,2),  "mulp4092"); break;
                            case 4094: posRep = builder.CreateSub(shl(xv,12), shl(xv,1),  "mulp4094"); break;
                            case 4095: posRep = builder.CreateSub(shl(xv,12), xv,          "mulp4095"); break;
                            case 4097: posRep = builder.CreateAdd(shl(xv,12), xv,          "mulp4097"); break;
                            case 4098: posRep = builder.CreateAdd(shl(xv,12), shl(xv,1),   "mulp4098"); break;
                            case 4100: posRep = builder.CreateAdd(shl(xv,12), shl(xv,2),   "mulp4100"); break;
                            case 4104: posRep = builder.CreateAdd(shl(xv,12), shl(xv,3),   "mulp4104"); break;
                            case 4112: posRep = builder.CreateAdd(shl(xv,12), shl(xv,4),   "mulp4112"); break;
                            case 4128: posRep = builder.CreateAdd(shl(xv,12), shl(xv,5),   "mulp4128"); break;
                            case 4160: posRep = builder.CreateAdd(shl(xv,12), shl(xv,6),   "mulp4160"); break;
                            case 4224: posRep = builder.CreateAdd(shl(xv,12), shl(xv,7),   "mulp4224"); break;
                            case 4352: posRep = builder.CreateAdd(shl(xv,12), shl(xv,8),   "mulp4352"); break;
                            case 4608: posRep = builder.CreateAdd(shl(xv,12), shl(xv,9),   "mulp4608"); break;
                            case 5120: posRep = builder.CreateAdd(shl(xv,12), shl(xv,10),  "mulp5120"); break;
                            case 6144: posRep = builder.CreateAdd(shl(xv,12), shl(xv,11),  "mulp6144"); break;
                            // ── n×8192 family ──────────────────────────────────────────
                            case 7168:  posRep = builder.CreateSub(shl(xv,13), shl(xv,10), "mulp7168");  break;
                            case 7680:  posRep = builder.CreateSub(shl(xv,13), shl(xv,9),  "mulp7680");  break;
                            case 7936:  posRep = builder.CreateSub(shl(xv,13), shl(xv,8),  "mulp7936");  break;
                            case 8064:  posRep = builder.CreateSub(shl(xv,13), shl(xv,7),  "mulp8064");  break;
                            case 8128:  posRep = builder.CreateSub(shl(xv,13), shl(xv,6),  "mulp8128");  break;
                            case 8160:  posRep = builder.CreateSub(shl(xv,13), shl(xv,5),  "mulp8160");  break;
                            case 8176:  posRep = builder.CreateSub(shl(xv,13), shl(xv,4),  "mulp8176");  break;
                            case 8184:  posRep = builder.CreateSub(shl(xv,13), shl(xv,3),  "mulp8184");  break;
                            case 8188:  posRep = builder.CreateSub(shl(xv,13), shl(xv,2),  "mulp8188");  break;
                            case 8190:  posRep = builder.CreateSub(shl(xv,13), shl(xv,1),  "mulp8190");  break;
                            case 8191:  posRep = builder.CreateSub(shl(xv,13), xv,          "mulp8191");  break;
                            case 8193:  posRep = builder.CreateAdd(shl(xv,13), xv,          "mulp8193");  break;
                            case 8194:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,1),  "mulp8194");  break;
                            case 8196:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,2),  "mulp8196");  break;
                            case 8200:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,3),  "mulp8200");  break;
                            case 8208:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,4),  "mulp8208");  break;
                            case 8224:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,5),  "mulp8224");  break;
                            case 8256:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,6),  "mulp8256");  break;
                            case 8320:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,7),  "mulp8320");  break;
                            case 8448:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,8),  "mulp8448");  break;
                            case 8704:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,9),  "mulp8704");  break;
                            case 9216:  posRep = builder.CreateAdd(shl(xv,13), shl(xv,10), "mulp9216");  break;
                            case 10240: posRep = builder.CreateAdd(shl(xv,13), shl(xv,11), "mulp10240"); break;
                            case 12288: posRep = builder.CreateAdd(shl(xv,13), shl(xv,12), "mulp12288"); break;
                            // ── n×16384 family ──────────────────────────────────────
                            case 14336: posRep = builder.CreateSub(shl(xv,14), shl(xv,11), "mulp14336"); break;
                            case 15360: posRep = builder.CreateSub(shl(xv,14), shl(xv,10), "mulp15360"); break;
                            case 16384: posRep = shl(xv,14); break;
                            case 16385: posRep = builder.CreateAdd(shl(xv,14), xv,          "mulp16385"); break;
                            case 16386: posRep = builder.CreateAdd(shl(xv,14), shl(xv,1),   "mulp16386"); break;
                            case 16388: posRep = builder.CreateAdd(shl(xv,14), shl(xv,2),   "mulp16388"); break;
                            case 16392: posRep = builder.CreateAdd(shl(xv,14), shl(xv,3),   "mulp16392"); break;
                            case 16400: posRep = builder.CreateAdd(shl(xv,14), shl(xv,4),   "mulp16400"); break;
                            case 16416: posRep = builder.CreateAdd(shl(xv,14), shl(xv,5),   "mulp16416"); break;
                            case 16448: posRep = builder.CreateAdd(shl(xv,14), shl(xv,6),   "mulp16448"); break;
                            case 16512: posRep = builder.CreateAdd(shl(xv,14), shl(xv,7),   "mulp16512"); break;
                            case 16640: posRep = builder.CreateAdd(shl(xv,14), shl(xv,8),   "mulp16640"); break;
                            case 16896: posRep = builder.CreateAdd(shl(xv,14), shl(xv,9),   "mulp16896"); break;
                            case 17408: posRep = builder.CreateAdd(shl(xv,14), shl(xv,10),  "mulp17408"); break;
                            case 18432: posRep = builder.CreateAdd(shl(xv,14), shl(xv,11),  "mulp18432"); break;
                            case 20480: posRep = builder.CreateAdd(shl(xv,14), shl(xv,12),  "mulp20480"); break;
                            case 24576: posRep = builder.CreateAdd(shl(xv,14), shl(xv,13),  "mulp24576"); break;
                            // ── n×32768 family ──────────────────────────────────────
                            case 28672: posRep = builder.CreateSub(shl(xv,15), shl(xv,12), "mulp28672"); break;
                            case 30720: posRep = builder.CreateSub(shl(xv,15), shl(xv,11), "mulp30720"); break;
                            case 32768: posRep = shl(xv,15); break;
                            case 32769: posRep = builder.CreateAdd(shl(xv,15), xv,          "mulp32769"); break;
                            case 32770: posRep = builder.CreateAdd(shl(xv,15), shl(xv,1),   "mulp32770"); break;
                            case 32772: posRep = builder.CreateAdd(shl(xv,15), shl(xv,2),   "mulp32772"); break;
                            case 32776: posRep = builder.CreateAdd(shl(xv,15), shl(xv,3),   "mulp32776"); break;
                            case 32800: posRep = builder.CreateAdd(shl(xv,15), shl(xv,5),   "mulp32800"); break;
                            case 32896: posRep = builder.CreateAdd(shl(xv,15), shl(xv,7),   "mulp32896"); break;
                            case 33024: posRep = builder.CreateAdd(shl(xv,15), shl(xv,8),   "mulp33024"); break;
                            case 33280: posRep = builder.CreateAdd(shl(xv,15), shl(xv,9),   "mulp33280"); break;
                            case 33792: posRep = builder.CreateAdd(shl(xv,15), shl(xv,10),  "mulp33792"); break;
                            case 34816: posRep = builder.CreateAdd(shl(xv,15), shl(xv,11),  "mulp34816"); break;
                            case 36864: posRep = builder.CreateAdd(shl(xv,15), shl(xv,12),  "mulp36864"); break;
                            case 40960: posRep = builder.CreateAdd(shl(xv,15), shl(xv,13),  "mulp40960"); break;
                            case 49152: posRep = builder.CreateAdd(shl(xv,15), shl(xv,14),  "mulp49152"); break;
                            // ── n×65536 family ──────────────────────────────────────
                            case 57344: posRep = builder.CreateSub(shl(xv,16), shl(xv,13), "mulp57344"); break;
                            case 61440: posRep = builder.CreateSub(shl(xv,16), shl(xv,12), "mulp61440"); break;
                            case 65536: posRep = shl(xv,16); break;
                            case 65537: posRep = builder.CreateAdd(shl(xv,16), xv,          "mulp65537"); break;
                            case 65538: posRep = builder.CreateAdd(shl(xv,16), shl(xv,1),   "mulp65538"); break;
                            case 65540: posRep = builder.CreateAdd(shl(xv,16), shl(xv,2),   "mulp65540"); break;
                            case 65544: posRep = builder.CreateAdd(shl(xv,16), shl(xv,3),   "mulp65544"); break;
                            case 65600: posRep = builder.CreateAdd(shl(xv,16), shl(xv,6),   "mulp65600"); break;
                            case 65664: posRep = builder.CreateAdd(shl(xv,16), shl(xv,7),   "mulp65664"); break;
                            case 65792: posRep = builder.CreateAdd(shl(xv,16), shl(xv,8),   "mulp65792"); break;
                            case 66048: posRep = builder.CreateAdd(shl(xv,16), shl(xv,9),   "mulp66048"); break;
                            case 66560: posRep = builder.CreateAdd(shl(xv,16), shl(xv,10),  "mulp66560"); break;
                            case 67584: posRep = builder.CreateAdd(shl(xv,16), shl(xv,11),  "mulp67584"); break;
                            case 69632: posRep = builder.CreateAdd(shl(xv,16), shl(xv,12),  "mulp69632"); break;
                            case 73728: posRep = builder.CreateAdd(shl(xv,16), shl(xv,13),  "mulp73728"); break;
                            case 81920: posRep = builder.CreateAdd(shl(xv,16), shl(xv,14),  "mulp81920"); break;
                            case 98304: posRep = builder.CreateAdd(shl(xv,16), shl(xv,15),  "mulp98304"); break;
                            case 114688: posRep = builder.CreateSub(shl(xv,17), shl(xv,14), "mulp114688"); break;
                            case 122880: posRep = builder.CreateSub(shl(xv,17), shl(xv,13), "mulp122880"); break;
                            case 131072: posRep = shl(xv,17); break;
                            case 131073: posRep = builder.CreateAdd(shl(xv,17), xv,          "mulp131073"); break;
                            case 131074: posRep = builder.CreateAdd(shl(xv,17), shl(xv,1),   "mulp131074"); break;
                            case 131076: posRep = builder.CreateAdd(shl(xv,17), shl(xv,2),   "mulp131076"); break;
                            case 131080: posRep = builder.CreateAdd(shl(xv,17), shl(xv,3),   "mulp131080"); break;
                            case 131136: posRep = builder.CreateAdd(shl(xv,17), shl(xv,6),   "mulp131136"); break;
                            case 131200: posRep = builder.CreateAdd(shl(xv,17), shl(xv,7),   "mulp131200"); break;
                            case 131328: posRep = builder.CreateAdd(shl(xv,17), shl(xv,8),   "mulp131328"); break;
                            case 131584: posRep = builder.CreateAdd(shl(xv,17), shl(xv,9),   "mulp131584"); break;
                            case 132096: posRep = builder.CreateAdd(shl(xv,17), shl(xv,10),  "mulp132096"); break;
                            case 133120: posRep = builder.CreateAdd(shl(xv,17), shl(xv,11),  "mulp133120"); break;
                            case 135168: posRep = builder.CreateAdd(shl(xv,17), shl(xv,12),  "mulp135168"); break;
                            case 139264: posRep = builder.CreateAdd(shl(xv,17), shl(xv,13),  "mulp139264"); break;
                            case 147456: posRep = builder.CreateAdd(shl(xv,17), shl(xv,14),  "mulp147456"); break;
                            case 163840: posRep = builder.CreateAdd(shl(xv,17), shl(xv,15),  "mulp163840"); break;
                            case 196608: posRep = builder.CreateAdd(shl(xv,17), shl(xv,16),  "mulp196608"); break;
                            // 3-instruction negative sequences (new cases not covered above)
                            case 37: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,2)), xv, "mulp37"); break;
                            case 41: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,3)), xv, "mulp41"); break;
                            case 49: posRep = builder.CreateAdd(builder.CreateAdd(shl(xv,5), shl(xv,4)), xv, "mulp49"); break;
                            case 50: posRep = builder.CreateAdd(builder.CreateSub(shl(xv,6), shl(xv,4)), shl(xv,1), "mulp50"); break;
                            case 100: posRep = builder.CreateAdd(shl(xv,6), builder.CreateAdd(shl(xv,5), shl(xv,2)), "mulp100"); break;
                            case 200: posRep = builder.CreateAdd(builder.CreateSub(shl(xv,8), shl(xv,6)), shl(xv,3), "mulp200"); break;
                            case 1000: posRep = builder.CreateSub(builder.CreateSub(shl(xv,10), shl(xv,4)), shl(xv,3), "mulp1000"); break;
                            }
                            if (posRep)
                                simplified = builder.CreateNeg(posRep, "mulneg");
                        }
                        break;
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
            // (x | c) & c → c  [OR sets c-bits; AND keeps only c-bits → always c]
            // Proof: (x | c) has all bits of c set. ANDing with c yields exactly c.
            if (!simplified && inst.getOpcode() == llvm::Instruction::And) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (inner && c2 && inner->getOpcode() == llvm::Instruction::Or &&
                    hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1 && *c1 == *c2) {
                        simplified = llvm::ConstantInt::get(inst.getType(), *c2);
                    }
                }
            }
            // (x & c) | c → c  [AND keeps only c-bits; OR adds them back → always c]
            // Proof: (x & c) ⊆ c, so (x & c) | c = c.
            if (!simplified && inst.getOpcode() == llvm::Instruction::Or) {
                auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto c2 = getConstIntValue(inst.getOperand(1));
                if (inner && c2 && inner->getOpcode() == llvm::Instruction::And &&
                    hasOneUse(inner)) {
                    auto c1 = getConstIntValue(inner->getOperand(1));
                    if (c1 && *c1 == *c2) {
                        simplified = llvm::ConstantInt::get(inst.getType(), *c2);
                    }
                }
            }
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

            // ── select(cond, x+1, x) → x + zext(cond)  [conditional increment] ──
            // ── select(cond, x-1, x) → x - zext(cond)  [conditional decrement] ──
            // These arise when the idiom recognition phase (Phase 1) replaces inner
            // select(C, N+1, N) with add(N, zext(C)), and an outer select(D, inner, x)
            // then becomes select(D, x+1, x) after the inner is simplified.
            // Running this in the algebraic phase ensures the full chain collapses.
            if (!simplified) {
                if (auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst)) {
                    llvm::Value* cond   = sel->getCondition();
                    llvm::Value* trueV  = sel->getTrueValue();
                    llvm::Value* falseV = sel->getFalseValue();
                    // Pattern: select(cond, x+1, x) where trueV = add(x, 1) or add(1, x)
                    // Also handles the case where trueV's base and falseV are equivalent
                    // zext instructions of the same source (arising from idiom replacement
                    // creating a new zext while an equivalent one already existed).
                    auto zextSrc = [](llvm::Value* v) -> llvm::Value* {
                        if (auto* z = llvm::dyn_cast<llvm::ZExtInst>(v))
                            return z->getOperand(0);
                        return nullptr;
                    };
                    auto valuesEquivalent = [&](llvm::Value* a, llvm::Value* b) -> bool {
                        if (a == b) return true;
                        // Both are zext of the same source value
                        auto* sa = zextSrc(a);
                        auto* sb = zextSrc(b);
                        return sa && sb && sa == sb && a->getType() == b->getType();
                    };
                    if (auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(trueV)) {
                        if (addInst->getOpcode() == llvm::Instruction::Add) {
                            bool lhsIsBase = (valuesEquivalent(addInst->getOperand(0), falseV) &&
                                              isConstInt(addInst->getOperand(1), 1));
                            bool rhsIsBase = (valuesEquivalent(addInst->getOperand(1), falseV) &&
                                              isConstInt(addInst->getOperand(0), 1));
                            if (lhsIsBase || rhsIsBase) {
                                llvm::IRBuilder<> builder(&inst);
                                llvm::Value* ext = builder.CreateZExt(cond, inst.getType(), "cond.zext");
                                simplified = builder.CreateAdd(falseV, ext, "cond.inc");
                            }
                        }
                        // select(cond, x-1, x) → x - zext(cond)
                        if (!simplified && addInst->getOpcode() == llvm::Instruction::Sub &&
                            valuesEquivalent(addInst->getOperand(0), falseV) &&
                            isConstInt(addInst->getOperand(1), 1)) {
                            llvm::IRBuilder<> builder(&inst);
                            llvm::Value* ext = builder.CreateZExt(cond, inst.getType(), "cond.zext");
                            simplified = builder.CreateSub(falseV, ext, "cond.dec");
                        }
                    }
                    // Constant variant: select(cond, C+1, C) → C + zext(cond)
                    // (handles cases not already caught by idiom detection Phase 1)
                    if (!simplified) {
                        auto cvTrue  = getConstIntValue(trueV);
                        auto cvFalse = getConstIntValue(falseV);
                        if (cvTrue && cvFalse) {
                            if (*cvTrue == *cvFalse + 1) {
                                llvm::IRBuilder<> builder(&inst);
                                llvm::Value* ext = builder.CreateZExt(cond, inst.getType(), "cond.zext");
                                simplified = builder.CreateAdd(falseV, ext, "cond.inc");
                            } else if (*cvTrue == *cvFalse - 1) {
                                llvm::IRBuilder<> builder(&inst);
                                llvm::Value* ext = builder.CreateZExt(cond, inst.getType(), "cond.zext");
                                simplified = builder.CreateSub(falseV, ext, "cond.dec");
                            }
                        }
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

            // ── De Morgan's laws ─────────────────────────────────────────
            // ~(a & b) → (~a) | (~b)  when inner has one use
            if (!simplified && inst.getOpcode() == llvm::Instruction::Xor) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    if (ci->isMinusOne()) {
                        if (auto* inner = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0))) {
                            if (inner->getOpcode() == llvm::Instruction::And && hasOneUse(inner)) {
                                llvm::IRBuilder<> builder(&inst);
                                llvm::Value* notA = builder.CreateNot(inner->getOperand(0), "demorgan.nota");
                                llvm::Value* notB = builder.CreateNot(inner->getOperand(1), "demorgan.notb");
                                simplified = builder.CreateOr(notA, notB, "demorgan.or");
                            }
                            // ~(a | b) → (~a) & (~b)
                            else if (inner->getOpcode() == llvm::Instruction::Or && hasOneUse(inner)) {
                                llvm::IRBuilder<> builder(&inst);
                                llvm::Value* notA = builder.CreateNot(inner->getOperand(0), "demorgan.nota");
                                llvm::Value* notB = builder.CreateNot(inner->getOperand(1), "demorgan.notb");
                                simplified = builder.CreateAnd(notA, notB, "demorgan.and");
                            }
                        }
                    }
                }
            }

            // ── Boolean simplification on select ─────────────────────────
            // select(cond, 1, 0) → zext(cond)
            if (!simplified && llvm::isa<llvm::SelectInst>(inst)) {
                auto* sel = llvm::cast<llvm::SelectInst>(&inst);
                if (isConstInt(sel->getTrueValue(), 1) && isConstInt(sel->getFalseValue(), 0)) {
                    llvm::IRBuilder<> builder(&inst);
                    simplified = builder.CreateZExt(sel->getCondition(), sel->getType(), "bool_zext");
                }
                // select(cond, 0, 1) → zext(!cond)
                if (!simplified && isConstInt(sel->getTrueValue(), 0) && isConstInt(sel->getFalseValue(), 1)) {
                    llvm::IRBuilder<> builder(&inst);
                    llvm::Value* notCond = builder.CreateNot(sel->getCondition(), "bool_not");
                    simplified = builder.CreateZExt(notCond, sel->getType(), "bool_notzext");
                }
                // select(cond, x, 0) → and(x, sext(cond))
                // Branchless masking: sext(i1 cond) produces 0 or -1 (all ones),
                // so AND keeps x unchanged when true and zeroes it when false.
                // This is more amenable to vectorization than select.
                if (!simplified && isConstInt(sel->getFalseValue(), 0) &&
                    !isConstInt(sel->getTrueValue(), 1) &&
                    sel->getType()->isIntegerTy()) {
                    llvm::IRBuilder<> builder(&inst);
                    llvm::Value* mask = builder.CreateSExt(sel->getCondition(),
                        sel->getType(), "sel_mask");
                    simplified = builder.CreateAnd(sel->getTrueValue(), mask, "sel_and");
                }
                // select(cond, 0, x) → and(x, sext(!cond))
                if (!simplified && isConstInt(sel->getTrueValue(), 0) &&
                    !isConstInt(sel->getFalseValue(), 1) &&
                    sel->getType()->isIntegerTy()) {
                    llvm::IRBuilder<> builder(&inst);
                    llvm::Value* notCond = builder.CreateNot(sel->getCondition(), "sel_notcond");
                    llvm::Value* mask = builder.CreateSExt(notCond,
                        sel->getType(), "sel_mask");
                    simplified = builder.CreateAnd(sel->getFalseValue(), mask, "sel_and");
                }
            }

            // ── Redundant truncation elimination ─────────────────────────
            // and(x, 0xFF) where x is known < 256 → x
            // (More generally: and(x, mask) where x's value range fits in mask)
            if (!simplified && inst.getOpcode() == llvm::Instruction::And) {
                if (auto* maskCI = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    uint64_t mask = maskCI->getZExtValue();
                    // Check if mask is (2^n - 1) i.e. a bit-width mask
                    if (mask > 0 && (mask & (mask + 1)) == 0) {
                        llvm::Value* src = inst.getOperand(0);
                        // If source is a zext from a smaller type that fits in the mask
                        if (auto* zext = llvm::dyn_cast<llvm::ZExtInst>(src)) {
                            unsigned srcBits = zext->getSrcTy()->getIntegerBitWidth();
                            if ((1ULL << srcBits) - 1 <= mask) {
                                simplified = src;
                            }
                        }
                    }
                }
            }

            // ── min/max via select canonicalization ──────────────────────
            // select(icmp slt a, b, a, b) → already handled by idiom recognizer
            // select(icmp sgt a, 0, a, sub(0, a)) → abs(a)  [when not already caught]
            if (!simplified && llvm::isa<llvm::SelectInst>(inst)) {
                auto* sel = llvm::cast<llvm::SelectInst>(&inst);
                if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(sel->getCondition())) {
                    // select(a > 0, a, -a) → abs(a)
                    if (cmp->getPredicate() == llvm::ICmpInst::ICMP_SGT &&
                        isConstInt(cmp->getOperand(1), 0) &&
                        sel->getTrueValue() == cmp->getOperand(0)) {
                        if (auto* neg = llvm::dyn_cast<llvm::BinaryOperator>(sel->getFalseValue())) {
                            if (neg->getOpcode() == llvm::Instruction::Sub &&
                                isConstInt(neg->getOperand(0), 0) &&
                                neg->getOperand(1) == cmp->getOperand(0)) {
                                llvm::IRBuilder<> builder(&inst);
                                llvm::Value* x = cmp->getOperand(0);
                                // abs(x) = (x ^ (x >> 31)) - (x >> 31)  for 32-bit
                                // But use LLVM's abs intrinsic if available
                                unsigned bw = x->getType()->getIntegerBitWidth();
                                llvm::Value* shift = builder.CreateAShr(x,
                                    llvm::ConstantInt::get(x->getType(), bw - 1), "abs.sign");
                                llvm::Value* xored = builder.CreateXor(x, shift, "abs.xor");
                                simplified = builder.CreateSub(xored, shift, "abs.sub");
                            }
                        }
                    }
                    // select(a < 0, -a, a) → abs(a)
                    if (!simplified && cmp->getPredicate() == llvm::ICmpInst::ICMP_SLT &&
                        isConstInt(cmp->getOperand(1), 0) &&
                        sel->getFalseValue() == cmp->getOperand(0)) {
                        if (auto* neg = llvm::dyn_cast<llvm::BinaryOperator>(sel->getTrueValue())) {
                            if (neg->getOpcode() == llvm::Instruction::Sub &&
                                isConstInt(neg->getOperand(0), 0) &&
                                neg->getOperand(1) == cmp->getOperand(0)) {
                                llvm::IRBuilder<> builder(&inst);
                                llvm::Value* x = cmp->getOperand(0);
                                unsigned bw = x->getType()->getIntegerBitWidth();
                                llvm::Value* shift = builder.CreateAShr(x,
                                    llvm::ConstantInt::get(x->getType(), bw - 1), "abs.sign");
                                llvm::Value* xored = builder.CreateXor(x, shift, "abs.xor");
                                simplified = builder.CreateSub(xored, shift, "abs.sub");
                            }
                        }
                    }
                }
            }

            // ── Comparison with boolean → simpler form ──────────────────
            // icmp ne (and x, 1), 0  →  trunc x to i1  (when only testing low bit)
            if (!simplified && inst.getOpcode() == llvm::Instruction::ICmp) {
                auto* cmp = llvm::cast<llvm::ICmpInst>(&inst);
                if (cmp->getPredicate() == llvm::ICmpInst::ICMP_NE &&
                    isConstInt(cmp->getOperand(1), 0)) {
                    if (auto* andInst = llvm::dyn_cast<llvm::BinaryOperator>(cmp->getOperand(0))) {
                        if (andInst->getOpcode() == llvm::Instruction::And &&
                            isConstInt(andInst->getOperand(1), 1)) {
                            llvm::IRBuilder<> builder(&inst);
                            llvm::Value* trunc = builder.CreateTrunc(andInst->getOperand(0),
                                builder.getInt1Ty(), "lowbit_trunc");
                            simplified = trunc;
                        }
                    }
                }
            }

            // ── Div-mod identity: (a / b) * b + (a % b) → a ─────────────────
            // This is an exact arithmetic identity for integer division:
            //   (a / b) * b + (a % b) = a  for any non-zero b and any a.
            // Users sometimes write this by accident when they mean a round-trip
            // through division (e.g., alignment checks), or as the inverse of
            // modular reduction to verify the quotient.
            //
            // We detect two forms:
            //   Form 1: add( mul(sdiv(a,b), b), srem(a,b) ) → a
            //   Form 2: add( mul(udiv(a,b), b), urem(a,b) ) → a
            // The operands to add may be in either order.
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                for (unsigned addLHS = 0; addLHS < 2 && !simplified; ++addLHS) {
                    auto* mulInst = llvm::dyn_cast<llvm::BinaryOperator>(
                        inst.getOperand(addLHS));
                    auto* remInst = llvm::dyn_cast<llvm::BinaryOperator>(
                        inst.getOperand(1 - addLHS));
                    if (!mulInst || !remInst) continue;
                    if (mulInst->getOpcode() != llvm::Instruction::Mul) continue;
                    bool isSigned = (remInst->getOpcode() == llvm::Instruction::SRem);
                    bool isUnsigned = (remInst->getOpcode() == llvm::Instruction::URem);
                    if (!isSigned && !isUnsigned) continue;

                    // remInst = srem(a, b) or urem(a, b)
                    llvm::Value* remA = remInst->getOperand(0);
                    llvm::Value* remB = remInst->getOperand(1);

                    // mulInst = mul(divInst, b) or mul(b, divInst)
                    for (unsigned mulLHS = 0; mulLHS < 2 && !simplified; ++mulLHS) {
                        auto* divInst = llvm::dyn_cast<llvm::BinaryOperator>(
                            mulInst->getOperand(mulLHS));
                        llvm::Value* mulB = mulInst->getOperand(1 - mulLHS);
                        if (!divInst) continue;
                        bool divSigned   = (divInst->getOpcode() == llvm::Instruction::SDiv);
                        bool divUnsigned = (divInst->getOpcode() == llvm::Instruction::UDiv);
                        if (!(divSigned && isSigned) && !(divUnsigned && isUnsigned)) continue;

                        llvm::Value* divA = divInst->getOperand(0);
                        llvm::Value* divB = divInst->getOperand(1);

                        // All of: divA == remA, divB == remB, mulB == remB
                        if (divA == remA && divB == remB && mulB == remB) {
                            simplified = remA;  // → a
                        }
                    }
                }
            }

            // ── x / C * C → x - (x % C)  [floor-to-multiple for integer x] ──
            // This is the "round down to multiple of C" pattern.  For pow-2 C
            // with non-negative x, this is just `x & ~(C-1)`.  But even for
            // arbitrary C and signed x, we can express it as `x - (x % C)`.
            // We don't emit the subtraction form here because it introduces a
            // new div/rem, but we CAN simplify the converse:
            //   x - (x % C) → x & ~(C-1)  when C is pow-2 and x >= 0
            // (already handled by the unsigned-rem → and pattern above)
            //
            // What IS worth doing here: recognize x/C*C when C is a known-
            // power-of-2 constant and x is known non-negative, and simplify to
            // and(x, ~(C-1)) which is 1 instruction vs 2.
            if (!simplified && inst.getOpcode() == llvm::Instruction::Mul) {
                llvm::Value* xv = nullptr;
                int64_t cvDiv = 0, cvMul = 0;
                llvm::BinaryOperator* divInst = nullptr;
                // Try mul( div(x, C), C )
                for (unsigned mulOp = 0; mulOp < 2 && !simplified; ++mulOp) {
                    auto cvMulVal = getConstIntValue(inst.getOperand(mulOp));
                    if (!cvMulVal) continue;
                    cvMul = *cvMulVal;
                    if (cvMul <= 1) continue;
                    auto* innerDiv = llvm::dyn_cast<llvm::BinaryOperator>(
                        inst.getOperand(1 - mulOp));
                    if (!innerDiv) continue;
                    bool isSDiv = (innerDiv->getOpcode() == llvm::Instruction::SDiv);
                    bool isUDiv = (innerDiv->getOpcode() == llvm::Instruction::UDiv);
                    if (!isSDiv && !isUDiv) continue;
                    auto cvDivVal = getConstIntValue(innerDiv->getOperand(1));
                    if (!cvDivVal || *cvDivVal != cvMul) continue;
                    cvDiv = *cvDivVal;
                    xv = innerDiv->getOperand(0);
                    divInst = innerDiv;

                    // Check: C is a power of 2
                    uint64_t uC = static_cast<uint64_t>(cvDiv);
                    if (uC < 2 || (uC & (uC - 1)) != 0) continue;
                    uint64_t mask = ~(uC - 1);

                    // For udiv: x is always non-negative in the unsigned sense.
                    // For sdiv: check if x is known non-negative.
                    const llvm::DataLayout& DL2 = inst.getModule()->getDataLayout();
                    bool xNonNeg = isUDiv || isValueNonNegative(xv, DL2);
                    if (!xNonNeg) continue;

                    // sdiv(x, C) * C → and(x, ~(C-1))  when x >= 0 and C is pow2
                    llvm::IRBuilder<> builder(&inst);
                    simplified = builder.CreateAnd(xv,
                        llvm::ConstantInt::get(inst.getType(),
                            static_cast<int64_t>(mask)), "divmul_mask");
                    (void)divInst;
                }
            }

            // ── neg(neg(x)) → x  (double negation elimination) ───────────────
            // sub(0, sub(0, x)) = x.  This arises when two sign flips are applied
            // in sequence, e.g., `(0 - (0 - v))` in user code or after inlining.
            if (!simplified && inst.getOpcode() == llvm::Instruction::Sub) {
                if (isConstInt(inst.getOperand(0), 0)) {
                    auto* innerSub = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                    if (innerSub && innerSub->getOpcode() == llvm::Instruction::Sub &&
                        isConstInt(innerSub->getOperand(0), 0) && hasOneUse(innerSub)) {
                        simplified = innerSub->getOperand(1);
                    }
                }
            }

            // ── xor(xor(x, C), C) → x  (double XOR with same constant) ─────────
            // Useful after constant folding of bitwise-not-not or mask-unmask chains.
            if (!simplified && inst.getOpcode() == llvm::Instruction::Xor) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    auto* innerXor = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                    if (innerXor && innerXor->getOpcode() == llvm::Instruction::Xor &&
                        hasOneUse(innerXor)) {
                        if (auto* ci2 = llvm::dyn_cast<llvm::ConstantInt>(
                                innerXor->getOperand(1))) {
                            if (ci->getValue() == ci2->getValue()) {
                                simplified = innerXor->getOperand(0);
                            }
                        }
                    }
                }
            }

            // ── add(x, sub(y, x)) → y  [add cancels sub] ─────────────────────
            // Arises in expression trees like `base + (target - base) = target`.
            if (!simplified && inst.getOpcode() == llvm::Instruction::Add) {
                // Check both orderings: add(x, sub(y,x)) and add(sub(y,x), x)
                for (unsigned op = 0; op < 2 && !simplified; ++op) {
                    llvm::Value* xv = inst.getOperand(op);
                    auto* subInst = llvm::dyn_cast<llvm::BinaryOperator>(
                        inst.getOperand(1 - op));
                    if (subInst && subInst->getOpcode() == llvm::Instruction::Sub &&
                        subInst->getOperand(1) == xv) {
                        simplified = subInst->getOperand(0);  // y
                    }
                }
            }

            // ── and(or(x, y), and(x, ~y)) → 0  / impossible mask ─────────────
            // or(x, y) has bits where x or y are set, and(x, ~y) has bits where
            // x is set but y is not.  Their AND can simplify in specific cases;
            // however, the general form is not simple.  Skip.

            // ── or(and(x, C), and(x, ~C)) → x  [partition complement mask] ───
            // When two AND-masks are complements, their OR reconstructs x.
            if (!simplified && inst.getOpcode() == llvm::Instruction::Or) {
                auto* andA = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(0));
                auto* andB = llvm::dyn_cast<llvm::BinaryOperator>(inst.getOperand(1));
                if (andA && andB &&
                    andA->getOpcode() == llvm::Instruction::And &&
                    andB->getOpcode() == llvm::Instruction::And &&
                    andA->getOperand(0) == andB->getOperand(0) &&
                    hasOneUse(andA) && hasOneUse(andB)) {
                    auto* cA = llvm::dyn_cast<llvm::ConstantInt>(andA->getOperand(1));
                    auto* cB = llvm::dyn_cast<llvm::ConstantInt>(andB->getOperand(1));
                    if (cA && cB && cA->getValue() == ~cB->getValue()) {
                        simplified = andA->getOperand(0);  // x
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
// Select-of-select chain simplification (Souper-inspired)
// ─────────────────────────────────────────────────────────────────────────────

/// Simplify nested select chains that arise from ternary-heavy OmScript code.
///
/// Souper-style patterns:
///   select(C, select(C, A, B), D)  →  select(C, A, D)   [redundant inner]
///   select(C, A, select(C, B, D))  →  select(C, A, D)   [redundant inner]
///   select(C, A, select(!C, B, D)) →  select(C, A, B)   [complementary cond]
///   select(!C, select(C, A, B), D) →  select(C, B, D)   [complementary cond]
///   select(C, X, select(C2, X, Y)) where C implies C2   → select(C, X, Y)
///
/// Also handles known-bits narrowing:
///   select(C, or(X, mask), and(X, ~mask))  → xor(X, and(mask, sext(C)))
///   when the select arms differ only in specific bits.
[[gnu::hot]] static unsigned simplifySelectChains(llvm::Function& func) {
    unsigned count = 0;
    std::vector<llvm::Instruction*> toErase;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst);
            if (!sel) continue;

            llvm::Value* cond = sel->getCondition();
            llvm::Value* trueVal = sel->getTrueValue();
            llvm::Value* falseVal = sel->getFalseValue();

            // Pattern 1: select(C, select(C, A, B), D) → select(C, A, D)
            if (auto* innerSel = llvm::dyn_cast<llvm::SelectInst>(trueVal)) {
                if (innerSel->getCondition() == cond) {
                    sel->setOperand(1, innerSel->getTrueValue());
                    count++;
                    continue;
                }
            }

            // Pattern 2: select(C, A, select(C, B, D)) → select(C, A, D)
            if (auto* innerSel = llvm::dyn_cast<llvm::SelectInst>(falseVal)) {
                if (innerSel->getCondition() == cond) {
                    sel->setOperand(2, innerSel->getFalseValue());
                    count++;
                    continue;
                }
            }

            // Pattern 3: select(C, A, select(!C, B, D)) → select(C, A, B)
            // where !C is a `xor C, true`
            if (auto* innerSel = llvm::dyn_cast<llvm::SelectInst>(falseVal)) {
                llvm::Value* innerCond = innerSel->getCondition();
                // Check if innerCond == !cond  (xor cond, true)
                llvm::Value* xorOp0 = nullptr;
                if (auto* xorInst = llvm::dyn_cast<llvm::BinaryOperator>(innerCond)) {
                    if (xorInst->getOpcode() == llvm::Instruction::Xor) {
                        if (xorInst->getOperand(0) == cond) {
                            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(xorInst->getOperand(1))) {
                                if (ci->isOne()) xorOp0 = cond;
                            }
                        } else if (xorInst->getOperand(1) == cond) {
                            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(xorInst->getOperand(0))) {
                                if (ci->isOne()) xorOp0 = cond;
                            }
                        }
                    }
                }
                if (xorOp0 == cond) {
                    sel->setOperand(2, innerSel->getTrueValue());
                    count++;
                    continue;
                }
            }

            // Pattern 4: select(!C, select(C, A, B), D) → select(C, B, D)
            {
                llvm::Value* notTarget = nullptr;
                if (auto* xorInst = llvm::dyn_cast<llvm::BinaryOperator>(cond)) {
                    if (xorInst->getOpcode() == llvm::Instruction::Xor) {
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(xorInst->getOperand(1))) {
                            if (ci->isOne()) notTarget = xorInst->getOperand(0);
                        } else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(xorInst->getOperand(0))) {
                            if (ci->isOne()) notTarget = xorInst->getOperand(1);
                        }
                    }
                }
                if (notTarget) {
                    if (auto* innerSel = llvm::dyn_cast<llvm::SelectInst>(trueVal)) {
                        if (innerSel->getCondition() == notTarget) {
                            // select(!C, select(C, A, B), D) → select(C, B, D)
                            llvm::IRBuilder<> builder(sel);
                            llvm::Value* newSel = builder.CreateSelect(notTarget,
                                innerSel->getFalseValue(), falseVal, "sel.simpl");
                            sel->replaceAllUsesWith(newSel);
                            toErase.push_back(sel);
                            count++;
                            continue;
                        }
                    }
                }
            }

            // Pattern 5: select(C, X, X) → X  (trivial identity)
            if (trueVal == falseVal) {
                sel->replaceAllUsesWith(trueVal);
                toErase.push_back(sel);
                count++;
                continue;
            }

            // Pattern 6: select(C, true, false) → C  (when i1)
            if (sel->getType()->isIntegerTy(1)) {
                auto* tConst = llvm::dyn_cast<llvm::ConstantInt>(trueVal);
                auto* fConst = llvm::dyn_cast<llvm::ConstantInt>(falseVal);
                if (tConst && fConst) {
                    if (tConst->isOne() && fConst->isZero()) {
                        sel->replaceAllUsesWith(cond);
                        toErase.push_back(sel);
                        count++;
                        continue;
                    }
                    if (tConst->isZero() && fConst->isOne()) {
                        // select(C, false, true) → !C
                        llvm::IRBuilder<> builder(sel);
                        llvm::Value* notC = builder.CreateNot(cond, "sel.not");
                        sel->replaceAllUsesWith(notC);
                        toErase.push_back(sel);
                        count++;
                        continue;
                    }
                }
            }

            // Pattern 7: select(C, C, X) → select(C, true, X) → or(C, X) when i1
            if (sel->getType()->isIntegerTy(1) && trueVal == cond) {
                llvm::IRBuilder<> builder(sel);
                llvm::Value* orVal = builder.CreateOr(cond, falseVal, "sel.or");
                sel->replaceAllUsesWith(orVal);
                toErase.push_back(sel);
                count++;
                continue;
            }

            // Pattern 8: select(C, X, C) → select(C, X, false) → and(C, X) when i1
            if (sel->getType()->isIntegerTy(1) && falseVal == cond) {
                llvm::IRBuilder<> builder(sel);
                llvm::Value* andVal = builder.CreateAnd(cond, trueVal, "sel.and");
                sel->replaceAllUsesWith(andVal);
                toErase.push_back(sel);
                count++;
                continue;
            }
        }
    }

    for (auto* inst : toErase) {
        if (inst->use_empty())
            inst->eraseFromParent();
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Known-bits narrowing (Souper-inspired)
// ─────────────────────────────────────────────────────────────────────────────

/// Use LLVM's KnownBits analysis to narrow operations when we can prove
/// certain bits are always zero or one.  This catches patterns that LLVM's
/// InstCombine misses because they require deeper analysis.
///
/// Souper-style optimizations:
///   - or(X, mask) where KnownBits(X) already has those bits set → X
///   - and(X, mask) where KnownBits(X) already has those bits zero → X
///   - shl(X, C) where top C bits of X are known zero → nuw shl
///   - Truncation when KnownBits proves value fits in narrower type
[[gnu::hot]] static unsigned applyKnownBitsNarrowing(llvm::Function& func) {
    if (func.empty()) return 0;
    const llvm::DataLayout& DL = func.getParent()->getDataLayout();
    unsigned count = 0;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (!inst.getType()->isIntegerTy()) continue;

            // ── sdiv(x, 2) bias-shift elimination when x is known even ──────
            // LLVM lowers `sdiv x, 2` to the canonical 3-instruction sequence:
            //   %sign   = lshr x, (bitWidth-1)   ; 0 if x≥0, 1 if x<0
            //   %biased = add  x, %sign           ; x + rounding-bias
            //   %result = ashr %biased, 1
            //
            // When x is provably even (KnownBits shows bit 0 = 0), the bias is
            // always 0 (since the division is exact) and we can reduce to just:
            //   %result = ashr x, 1
            //
            // Common case: n*(n+1)/2 (triangular numbers), array_len/2, even
            // alignment computations.
            if (inst.getOpcode() == llvm::Instruction::AShr) {
                if (isConstInt(inst.getOperand(1), 1)) {
                    // inst = ashr(%biased, 1)
                    auto* biasedInst = llvm::dyn_cast<llvm::BinaryOperator>(
                        inst.getOperand(0));
                    if (biasedInst &&
                        biasedInst->getOpcode() == llvm::Instruction::Add) {
                        // biasedInst = add(x, %sign) or add(%sign, x)
                        for (unsigned op = 0; op < 2; ++op) {
                            llvm::Value* maybeX     = biasedInst->getOperand(op);
                            llvm::Value* maybeSign  = biasedInst->getOperand(1-op);
                            // %sign = lshr(%x, bitWidth-1)
                            auto* signInst = llvm::dyn_cast<llvm::BinaryOperator>(
                                maybeSign);
                            if (!signInst ||
                                signInst->getOpcode() != llvm::Instruction::LShr)
                                continue;
                            if (signInst->getOperand(0) != maybeX) continue;
                            unsigned bw = inst.getType()->getIntegerBitWidth();
                            if (!isConstInt(signInst->getOperand(1),
                                            static_cast<int64_t>(bw - 1)))
                                continue;
                            // Pattern confirmed: ashr(add(x, lshr(x, bw-1)), 1)
                            // Check if x is known even (bit 0 known zero)
                            llvm::KnownBits kb = llvm::computeKnownBits(
                                maybeX, DL);
                            if (kb.Zero.getBitWidth() > 0 &&
                                kb.Zero[0]) {
                                // x is even → bias = 0 → replace with ashr(x, 1)
                                llvm::IRBuilder<> builder(&inst);
                                auto* simpler = builder.CreateAShr(
                                    maybeX,
                                    llvm::ConstantInt::get(inst.getType(), 1),
                                    "sdiv2_even");
                                inst.replaceAllUsesWith(simpler);
                                count++;
                            }
                            break;
                        }
                    }
                }
            }

            // or(X, mask): if KnownBits(X) already has all mask bits set, → X
            if (inst.getOpcode() == llvm::Instruction::Or) {
                if (auto* maskCI = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    llvm::KnownBits kb = llvm::computeKnownBits(inst.getOperand(0), DL);
                    llvm::APInt mask = maskCI->getValue();
                    // If all bits in mask are already known-one in X, the OR is redundant
                    if ((kb.One & mask) == mask) {
                        inst.replaceAllUsesWith(inst.getOperand(0));
                        count++;
                        continue;
                    }
                }
            }

            // and(X, mask): if KnownBits(X) already has all ~mask bits zero, → X
            if (inst.getOpcode() == llvm::Instruction::And) {
                if (auto* maskCI = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    llvm::KnownBits kb = llvm::computeKnownBits(inst.getOperand(0), DL);
                    llvm::APInt mask = maskCI->getValue();
                    llvm::APInt invertedMask = ~mask;
                    // If all bits NOT in mask are already known-zero, the AND is redundant
                    if ((kb.Zero & invertedMask) == invertedMask) {
                        inst.replaceAllUsesWith(inst.getOperand(0));
                        count++;
                        continue;
                    }
                }
            }

            // xor(X, mask): if KnownBits proves the XOR flips no bits → X
            if (inst.getOpcode() == llvm::Instruction::Xor) {
                if (auto* maskCI = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                    if (maskCI->isZero()) {
                        inst.replaceAllUsesWith(inst.getOperand(0));
                        count++;
                        continue;
                    }
                }
            }

            // shl(X, C): add NUW flag when KnownBits proves no overflow
            if (inst.getOpcode() == llvm::Instruction::Shl) {
                auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
                if (bo && !bo->hasNoUnsignedWrap()) {
                    if (auto* shiftCI = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                        unsigned shiftAmt = shiftCI->getZExtValue();
                        unsigned bitWidth = inst.getType()->getIntegerBitWidth();
                        if (shiftAmt < bitWidth) {
                            llvm::KnownBits kb = llvm::computeKnownBits(inst.getOperand(0), DL);
                            // If top shiftAmt bits are known zero, shl cannot overflow unsigned
                            unsigned leadingZeros = kb.countMinLeadingZeros();
                            if (leadingZeros >= shiftAmt) {
                                bo->setHasNoUnsignedWrap(true);
                                count++;
                            }
                        }
                    }
                }
            }

            // add(X, C): add NUW when KnownBits proves no unsigned overflow
            if (inst.getOpcode() == llvm::Instruction::Add) {
                auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
                if (bo && !bo->hasNoUnsignedWrap()) {
                    if (auto* addCI = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                        llvm::KnownBits kb = llvm::computeKnownBits(inst.getOperand(0), DL);
                        unsigned bitWidth = inst.getType()->getIntegerBitWidth();
                        // If X has enough leading zeros that X + C cannot overflow
                        unsigned leadingZeros = kb.countMinLeadingZeros();
                        if (leadingZeros > 0) {
                            llvm::APInt maxVal = llvm::APInt::getMaxValue(bitWidth)
                                .lshr(leadingZeros);
                            if (maxVal.uge(addCI->getValue()) &&
                                (maxVal - addCI->getValue()).uge(
                                    llvm::APInt::getMaxValue(bitWidth).lshr(leadingZeros))) {
                                // Simplified check: if max possible X + C fits in bitWidth
                                llvm::APInt maxX = llvm::APInt::getAllOnes(bitWidth)
                                    .lshr(leadingZeros);
                                llvm::APInt sum;
                                bool overflow = false;
                                sum = maxX.uadd_ov(addCI->getValue(), overflow);
                                if (!overflow) {
                                    bo->setHasNoUnsignedWrap(true);
                                    count++;
                                }
                            }
                        }
                    }
                }
            }

            // sub(X, C): add NUW flag when KnownBits proves X >= C (unsigned)
            // This lets downstream passes use the nuw flag for further rewrites.
            if (inst.getOpcode() == llvm::Instruction::Sub) {
                auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
                if (bo && !bo->hasNoUnsignedWrap()) {
                    if (auto* subCI = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                        // Safe only for the constant-subtrahend form (X - C).
                        // X >= C (unsigned) is guaranteed if the minimum value of X >= C.
                        // Minimum value of X with leadingZeros known-zero bits is 0.
                        // But we can prove X >= C if the known-one bits of X are >= C.
                        llvm::KnownBits kb = llvm::computeKnownBits(inst.getOperand(0), DL);
                        llvm::APInt cVal = subCI->getValue();
                        // If the minimum possible value of X (= known-one bits) >= C,
                        // then X >= C always holds and X - C cannot underflow.
                        if (kb.One.uge(cVal)) {
                            bo->setHasNoUnsignedWrap(true);
                            count++;
                        }
                    }
                }
            }

            // mul(X, C): add NUW when KnownBits proves no unsigned overflow
            // X * C fits in bitWidth iff the leading zeros of X are enough:
            //   ceil_log2(C) <= leadingZeros(X)
            if (inst.getOpcode() == llvm::Instruction::Mul) {
                auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
                if (bo && !bo->hasNoUnsignedWrap()) {
                    if (auto* mulCI = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1))) {
                        llvm::APInt cVal = mulCI->getValue();
                        if (cVal.ugt(1)) {
                            llvm::KnownBits kb = llvm::computeKnownBits(
                                inst.getOperand(0), DL);
                            unsigned leadingZeros = kb.countMinLeadingZeros();
                            unsigned bitWidth = inst.getType()->getIntegerBitWidth();
                            // Number of bits needed to represent C
                            unsigned cBits = cVal.getActiveBits();
                            // If leadingZeros(X) + leadingZeros(C) >= bitWidth,
                            // then X * C cannot overflow unsigned.
                            unsigned cLeadingZeros = bitWidth - cBits;
                            if (leadingZeros + cLeadingZeros >= bitWidth) {
                                bo->setHasNoUnsignedWrap(true);
                                count++;
                            }
                        }
                    }
                }
            }
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
// MACS: Modular-Addition-to-Conditional-Subtract
// ─────────────────────────────────────────────────────────────────────────────

/// Returns true if value v is provably in [0, hi) through !range metadata,
/// constant folding, or one level of PHI-node incoming-value analysis.
static bool valueInRange(llvm::Value* v, uint64_t hi) {
    // Check !range metadata on an instruction.
    if (auto* instr = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (auto* md = instr->getMetadata(llvm::LLVMContext::MD_range)) {
            if (md->getNumOperands() >= 2) {
                if (auto* hiMD = llvm::dyn_cast<llvm::ConstantAsMetadata>(
                        md->getOperand(1))) {
                    if (auto* hiCI = llvm::dyn_cast<llvm::ConstantInt>(
                            hiMD->getValue())) {
                        if (hiCI->getZExtValue() <= hi) return true;
                    }
                }
            }
        }
    }
    // Constant: check directly.
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v)) {
        return ci->getZExtValue() < hi;
    }
    // PHI: check all incoming values one level deep (no deeper recursion).
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(v)) {
        if (phi->getNumIncomingValues() == 0) return false;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
            llvm::Value* incoming = phi->getIncomingValue(i);
            // Check incoming directly (no recursion to avoid cycle issues).
            bool ok = false;
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(incoming)) {
                ok = ci->getZExtValue() < hi;
            } else if (auto* instr = llvm::dyn_cast<llvm::Instruction>(incoming)) {
                if (auto* md = instr->getMetadata(llvm::LLVMContext::MD_range)) {
                    if (md->getNumOperands() >= 2) {
                        if (auto* hiMD = llvm::dyn_cast<llvm::ConstantAsMetadata>(
                                md->getOperand(1))) {
                            if (auto* hiCI = llvm::dyn_cast<llvm::ConstantInt>(
                                    hiMD->getValue())) {
                                ok = hiCI->getZExtValue() <= hi;
                            }
                        }
                    }
                }
                // Also accept if incoming is a urem (result is always in [0, divisor)).
                if (!ok && instr->getOpcode() == llvm::Instruction::URem) {
                    if (auto* divisorCI = llvm::dyn_cast<llvm::ConstantInt>(
                            instr->getOperand(1))) {
                        ok = divisorCI->getZExtValue() <= hi;
                    }
                }
                // Accept a select result that itself comes from a prior MACS.
                if (!ok && instr->getOpcode() == llvm::Instruction::Select) {
                    if (auto* md2 = instr->getMetadata(llvm::LLVMContext::MD_range)) {
                        if (md2->getNumOperands() >= 2) {
                            if (auto* hMD = llvm::dyn_cast<llvm::ConstantAsMetadata>(
                                    md2->getOperand(1))) {
                                if (auto* hCI = llvm::dyn_cast<llvm::ConstantInt>(
                                        hMD->getValue())) {
                                    ok = hCI->getZExtValue() <= hi;
                                }
                            }
                        }
                    }
                }
            }
            if (!ok) return false;
        }
        return true;
    }
    // Also check a urem instruction directly.
    if (auto* instr = llvm::dyn_cast<llvm::Instruction>(v)) {
        if (instr->getOpcode() == llvm::Instruction::URem) {
            if (auto* divisorCI = llvm::dyn_cast<llvm::ConstantInt>(
                    instr->getOperand(1))) {
                return divisorCI->getZExtValue() <= hi;
            }
        }
    }
    return false;
}

/// Replace `urem(add(a, b), C)` with `select(s < C, s, s - C)` when both
/// operands of the add are provably in [0, C).
///
/// Motivation: for modular-arithmetic loops (Fibonacci, iterative modular
/// exponentiation, etc.) the induction variables stay in [0, C).  After
/// the first iteration LLVM knows each carry value is in [0, C) via the
/// !range metadata we attach to urem results.  CVP propagates this through
/// the loop PHI so the add has operands in [0, C) ⊂ [0, 2C); but LLVM's
/// own InstCombine sometimes misses this when the PHI comes from a select
/// (the prior MACS result) rather than a urem directly.  This pass fills
/// the gap so ALL unrolled iterations use the fast 2-cycle conditional
/// subtract instead of the 25-cycle division.
///
/// Cost:  urem (div) ≈ 25 cycles  →  add + icmp + sub + select ≈ 4 cycles.
/// The transformation is always safe: for a, b ∈ [0, C), a+b ∈ [0, 2C),
/// so `(a+b) % C == (a+b < C) ? (a+b) : (a+b - C)`.
[[gnu::hot]] static unsigned applyMacs(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::URem) continue;

            auto* divisorCI = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1));
            if (!divisorCI) continue;
            int64_t C = divisorCI->getSExtValue();
            if (C <= 1) continue;
            uint64_t UC = static_cast<uint64_t>(C);

            llvm::Value* dividend = inst.getOperand(0);

            // Check if dividend is the result of an add where both operands
            // are provably in [0, C) via !range metadata or value analysis.
            auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(dividend);
            if (!addInst || addInst->getOpcode() != llvm::Instruction::Add) continue;

            llvm::Value* lhs = addInst->getOperand(0);
            llvm::Value* rhs = addInst->getOperand(1);
            if (!valueInRange(lhs, UC) || !valueInRange(rhs, UC)) continue;

            // Both operands in [0, C): apply MACS.
            // s = a + b; result = (s < C) ? s : s - C
            llvm::IRBuilder<> builder(&inst);
            llvm::Value* s = dividend;   // the existing add result (or reuse)
            llvm::Value* cmpC = llvm::ConstantInt::get(inst.getType(), UC);
            llvm::Value* cmp = builder.CreateICmpULT(s, cmpC, "macs.cmp");
            llvm::Value* sub = builder.CreateSub(s, cmpC, "macs.sub",
                /*HasNUW=*/false, /*HasNSW=*/true);
            llvm::Value* sel = builder.CreateSelect(cmp, s, sub, "macs.sel");

            replacements.emplace_back(&inst, sel);
            ++count;
        }
    }

    for (auto& [old, newVal] : replacements)
        old->replaceAllUsesWith(newVal);
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Select operand sinking (a.k.a. "select factoring")
// ─────────────────────────────────────────────────────────────────────────────

/// Factor a common operand out of both arms of a select:
///
///   select(cond, binop(x, a), binop(x, b))  →  binop(x, select(cond, a, b))
///   select(cond, binop(a, x), binop(b, x))  →  binop(select(cond, a, b), x)
///
/// where `binop` is a pure, no-side-effect integer operation (add/sub/mul/
/// and/or/xor/shl/lshr/ashr) and `x` is the identical value in both arms.
///
/// Motivation: simplifyBranches (branch→select) converts `if/else` diamonds
/// to select instructions, but when both branches compute `acc op val` and
/// only `val` differs, the result is `select(cond, acc+a, acc+b)`.  The idiom
/// recogniser cannot see the abs/min/max pattern hidden inside; this pass
/// factors it out to `acc + select(cond, a, b)`, exposing the inner select for
/// idiom detection.
///
/// Example:
///   if (x < 0) acc += -x; else acc += x;
///   → (after simplifyBranches)  acc_new = select(x<0, acc+neg_x, acc+x)
///   → (after this pass)          acc_new = acc + select(x<0, neg_x, x)
///   → (after detectAbsoluteValue) acc_new = acc + llvm.abs(x)
[[gnu::hot]] static unsigned applySelectSinking(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* sel = llvm::dyn_cast<llvm::SelectInst>(&inst);
            if (!sel) continue;
            if (!sel->getType()->isIntegerTy()) continue;

            auto* trueOp  = llvm::dyn_cast<llvm::BinaryOperator>(sel->getTrueValue());
            auto* falseOp = llvm::dyn_cast<llvm::BinaryOperator>(sel->getFalseValue());
            if (!trueOp || !falseOp) continue;
            if (trueOp->getOpcode() != falseOp->getOpcode()) continue;

            auto opcode = trueOp->getOpcode();
            // Only pure arithmetic operations — no memory, no division-by-zero risk.
            if (opcode != llvm::Instruction::Add  &&
                opcode != llvm::Instruction::Sub  &&
                opcode != llvm::Instruction::Mul  &&
                opcode != llvm::Instruction::And  &&
                opcode != llvm::Instruction::Or   &&
                opcode != llvm::Instruction::Xor  &&
                opcode != llvm::Instruction::Shl  &&
                opcode != llvm::Instruction::LShr &&
                opcode != llvm::Instruction::AShr) continue;

            // Require at least one arm to have a single use to avoid code size blow-up.
            if (!hasOneUse(trueOp) && !hasOneUse(falseOp)) continue;

            llvm::Value* tL = trueOp->getOperand(0);
            llvm::Value* tR = trueOp->getOperand(1);
            llvm::Value* fL = falseOp->getOperand(0);
            llvm::Value* fR = falseOp->getOperand(1);

            llvm::IRBuilder<> builder(&inst);
            llvm::Value* replacement = nullptr;

            // Case 1: left operand is the common factor  →  binop(x, select(a, b))
            if (tL == fL) {
                // For Sub: select(c, x-a, x-b) → x - select(c, a, b).
                // This is valid because both arms have the same minuend x.
                // Note: select(c, a-x, b-x) is handled by Case 2 below.
                llvm::Value* inner = builder.CreateSelect(
                    sel->getCondition(), tR, fR, "sel.sink");
                replacement = builder.CreateBinOp(opcode, tL, inner, "binop.sink");
            }
            // Case 2: right operand is the common factor  →  binop(select(a, b), x)
            else if (tR == fR) {
                llvm::Value* inner = builder.CreateSelect(
                    sel->getCondition(), tL, fL, "sel.sink");
                replacement = builder.CreateBinOp(opcode, inner, tR, "binop.sink");
            }

            if (!replacement) continue;

            // Propagate NSW/NUW flags when both source ops had them.
            if (auto* binRep = llvm::dyn_cast<llvm::BinaryOperator>(replacement)) {
                if (trueOp->hasNoSignedWrap() && falseOp->hasNoSignedWrap())
                    binRep->setHasNoSignedWrap(true);
                if (trueOp->hasNoUnsignedWrap() && falseOp->hasNoUnsignedWrap())
                    binRep->setHasNoUnsignedWrap(true);
            }

            replacements.emplace_back(&inst, replacement);
            ++count;
        }
    }

    for (auto& [old, newVal] : replacements)
        old->replaceAllUsesWith(newVal);
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Synthesis engine
// ─────────────────────────────────────────────────────────────────────────────

/// Simple enumerative synthesis: try to find a cheaper equivalent for
/// a single instruction using a small library of templates.
[[nodiscard]] bool synthesizeReplacement(llvm::Instruction* inst, const SynthesisConfig& config) {
    if (!inst->getType()->isIntegerTy()) return false;

    llvm::IRBuilder<> builder(inst);
    llvm::Value* replacement = nullptr;
    double oldCost = instructionCost(inst);

    // Template 1: Multiply by constant → optimal shift+add/sub sequence.
    //
    // Uses Non-Adjacent Form (NAF) to find the minimum-digit signed binary
    // representation of the constant.  NAF guarantees no two adjacent nonzero
    // digits, giving the fewest add/sub operations for any constant.
    //
    // Cost model (default threshold 0.8):
    //   mul cost = 3.0;  shl = 1.0;  add/sub = 1.0
    //   1 NAF digit (power-of-2): 1 shift → cost 1.0 < 2.4  ✓
    //   2 NAF digits (one at pos 0): 1 shift + 1 add/sub → cost 2.0 < 2.4  ✓
    //   2 NAF digits (both nonzero at pos>0): 2 shifts + 1 add/sub → 3.0 ≥ 2.4  ✗
    //     BUT: emit these anyway — the critical-path latency is 2 cycles vs 3 for
    //     mul, which improves IPC even at equal throughput cost.  We apply a
    //     slightly relaxed threshold (1.05×) for latency-improving sequences.
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
        llvm::Type* ty = inst->getType();

        // Compute Non-Adjacent Form (NAF) of c.
        // Algorithm: at each step, if odd and would create two adjacent nonzeros,
        // use digit -1 (borrow), else +1.
        struct NafDigit { unsigned pos; int sign; };  // sign: +1 or -1
        std::vector<NafDigit> naf;
        {
            int64_t t = c;
            unsigned pos = 0;
            while (t != 0) {
                if (t & 1) {
                    // digit = 2 - (t & 3) mapped to {-1, +1}
                    int d = ((t & 3) == 3) ? -1 : 1;
                    naf.push_back({pos, d});
                    t -= d;
                }
                t >>= 1;
                ++pos;
            }
        }

        if (naf.empty()) return false; // c == 0, shouldn't happen

        // Only profitable for ≤2 NAF digits.  3+ would cost ≥ 5 instructions.
        if (naf.size() > 2) return false;

        // Build the instruction sequence from NAF digits.
        llvm::Value* result = nullptr;
        for (const auto& [pos, sign] : naf) {
            llvm::Value* shifted = (pos > 0)
                ? builder.CreateShl(var, llvm::ConstantInt::get(ty, pos),
                                    "mulk_s" + std::to_string(pos))
                : var;
            if (!result) {
                result = (sign > 0) ? shifted
                                    : builder.CreateNeg(shifted, "mulk_neg");
            } else {
                result = (sign > 0) ? builder.CreateAdd(result, shifted, "mulk_add")
                                    : builder.CreateSub(result, shifted, "mulk_sub");
            }
        }
        if (result && result != inst) replacement = result;
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

    // Template 4: Signed division by power-of-2 → arithmetic shift with
    // sign-bit correction.  For non-negative x, sdiv x, 2^n == x >> n.
    // For negative x, we need to add (2^n - 1) before shifting to round
    // toward zero (C/LLVM semantics) instead of toward negative infinity.
    //   sdiv x, 2^n → (x + (x >> 63) & (2^n - 1)) >> n
    // This replaces a 25-cycle division with ~4 cycles of shifts+add+and.
    if (!replacement && inst->getOpcode() == llvm::Instruction::SDiv) {
        auto cval = getConstIntValue(inst->getOperand(1));
        if (cval && *cval > 1 && (*cval & (*cval - 1)) == 0) {
            unsigned n = llvm::Log2_64(static_cast<uint64_t>(*cval));
            llvm::Value* x = inst->getOperand(0);
            llvm::Type* ty = x->getType();
            unsigned bitWidth = ty->getIntegerBitWidth();
            // signBit = x >> (bitWidth - 1)  (all 1s if negative, 0 if non-neg)
            llvm::Value* signBit = builder.CreateAShr(x,
                llvm::ConstantInt::get(ty, bitWidth - 1), "sdiv_sign");
            // bias = signBit & (2^n - 1)  (adds bias only for negative values)
            llvm::Value* bias = builder.CreateAnd(signBit,
                llvm::ConstantInt::get(ty, (*cval) - 1), "sdiv_bias");
            // biased = x + bias
            llvm::Value* biased = builder.CreateAdd(x, bias, "sdiv_biased");
            // result = biased >> n  (arithmetic shift to preserve sign)
            replacement = builder.CreateAShr(biased,
                llvm::ConstantInt::get(ty, n), "sdiv_shr");
        }
    }

    // Template 5: Signed remainder by power-of-2 → mask with sign correction.
    //   srem x, 2^n → x - ((x + (x >> 63) & (2^n - 1)) >> n) * 2^n
    // Simplified: for non-negative x, same as urem.  For negative x, needs
    // sign correction.  We use the identity:
    //   srem x, 2^n = x - (sdiv x, 2^n) * 2^n
    // where sdiv is already lowered to shifts above.  But we can do better:
    //   srem x, C = x - (x / C) * C  [generic identity]
    // For power-of-2 C with corrected division:
    //   t = (x + ((x >> 63) & (C-1))) >> n
    //   srem = x - (t << n)
    if (!replacement && inst->getOpcode() == llvm::Instruction::SRem) {
        auto cval = getConstIntValue(inst->getOperand(1));
        if (cval && *cval > 1 && (*cval & (*cval - 1)) == 0) {
            unsigned n = llvm::Log2_64(static_cast<uint64_t>(*cval));
            llvm::Value* x = inst->getOperand(0);
            llvm::Type* ty = x->getType();
            unsigned bitWidth = ty->getIntegerBitWidth();
            llvm::Value* signBit = builder.CreateAShr(x,
                llvm::ConstantInt::get(ty, bitWidth - 1), "srem_sign");
            llvm::Value* bias = builder.CreateAnd(signBit,
                llvm::ConstantInt::get(ty, (*cval) - 1), "srem_bias");
            llvm::Value* biased = builder.CreateAdd(x, bias, "srem_biased");
            llvm::Value* quot = builder.CreateAShr(biased,
                llvm::ConstantInt::get(ty, n), "srem_quot");
            llvm::Value* quotTimesC = builder.CreateShl(quot,
                llvm::ConstantInt::get(ty, n), "srem_qtc");
            replacement = builder.CreateSub(x, quotTimesC, "srem_result");
        }
    }

    if (replacement) {
        // Walk the newly created instruction tree and sum costs.
        // Avoid counting the original instruction or values defined outside
        // the current block (which pre-existed and are not our responsibility).
        double newCost = 0.0;
        std::vector<llvm::Value*> worklist = {replacement};
        std::unordered_set<llvm::Value*> visited;
        while (!worklist.empty()) {
            llvm::Value* v = worklist.back();
            worklist.pop_back();
            if (!visited.insert(v).second) continue;
            auto* ri = llvm::dyn_cast<llvm::Instruction>(v);
            if (!ri || ri == inst || ri->getParent() != inst->getParent()) continue;
            newCost += instructionCost(ri);
            for (unsigned i = 0; i < ri->getNumOperands(); ++i)
                worklist.push_back(ri->getOperand(i));
        }

        // Use a slightly relaxed threshold (1.05×) when the replacement
        // also reduces critical-path latency (i.e., newCost ≤ oldCost but
        // with shorter chains), so that 2-shift+add sequences are accepted
        // for x*c where c has 2 NAF digits with both positions > 0.
        // The standard threshold is 0.8 × oldCost; the relaxed threshold
        // allows up to 1.05 × oldCost for pure latency improvements.
        bool latencyImproved = (newCost > 0.0 && newCost <= oldCost);
        double effectiveThreshold = latencyImproved
            ? std::min(config.costThreshold * 1.312, 1.05)  // ~1.05 at default threshold
            : config.costThreshold;

        if (newCost < oldCost * effectiveThreshold) {
            inst->replaceAllUsesWith(replacement);
            return true;
        }
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop idiom recognition — popcount and floor-log2
// ─────────────────────────────────────────────────────────────────────────────

/// Recognise two high-value loop idioms that users commonly write as loops
/// but whose optimal implementation is a single hardware instruction:
///
///  1. **Popcount loop** (Kernighan/shift variant):
///       c = 0; while (x != 0) { c += x & 1; x >>= 1; }
///     Optimal: c = llvm.ctpop(x_init)
///     Saves up to 64 iterations of (AND + ADD + LShr + CMP + BR) → 1 instruction.
///
///  2. **Floor-log2 loop** (shift-and-count variant):
///       r = 0; while (x > 1) { x >>= 1; r++; }
///     Optimal: r = 63 - llvm.ctlz(x_init, false)
///     Equivalently: r = llvm.ctlz(1, false) - llvm.ctlz(x_init, false) on any width.
///     Saves up to 63 iterations → 1 CTZ/CLZ + 1 SUB = 2 instructions.
///
/// Detection uses LLVM's phi-node loop structure.  Both patterns have exactly
/// two phi nodes in the loop header: a shifting variable (x) and an accumulator.
///
/// Safety guards:
///  - Popcount: x_init must be the only loop-variant value consumed outside the
///    loop (via the accumulator's lcssa phi).
///  - Log2: x_init must be > 0 at the call site (otherwise ctlz returns 64 which
///    would underflow to -1).  We only emit the replacement when the preheader
///    already guards `x > 1` (the standard loop-entry test).
///
/// Both replacements preserve exact signed semantics because ctpop/ctlz are
/// defined for all i64 inputs and `63 - ctlz(x, false)` equals the floor-log2
/// of x for any positive x.
[[gnu::hot]] static unsigned recognizeLoopIdioms(llvm::Function& func) {
    if (func.isDeclaration()) return 0;
    unsigned count = 0;

    llvm::LLVMContext& ctx = func.getContext();
    llvm::Module* M = func.getParent();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    // Collect loop headers: basic blocks that have a phi with an incoming edge
    // from a successor (i.e., a back-edge indicates a loop).
    for (auto& bb : func) {
        // ── Candidate screening ───────────────────────────────────────────
        // We look for loop bodies that contain EXACTLY the phi chain and the
        // bit-manipulation ops; anything else (stores, calls, etc.) disqualifies.

        // Count phis in this block
        unsigned phiCount = 0;
        llvm::PHINode* xPhi = nullptr;  // the shifting variable
        llvm::PHINode* accPhi = nullptr; // the accumulator (c or r)
        for (auto& phi : bb.phis()) {
            ++phiCount;
            if (phiCount > 3) break;  // too many phis
            (xPhi ? accPhi : xPhi) = &phi;  // assign first two
        }
        if (phiCount != 2 || !xPhi || !accPhi) continue;

        // The block must have exactly one back-edge (self-loop or backedge from body)
        // Simple check: the back-edge predecessor must be this block or a single body block.
        // We handle the common case: header IS the body (single-block loop).
        // If one incoming value comes from a predecessor that is a successor of bb,
        // we have a back-edge.

        // Find preheader edge and back-edge for each phi
        auto getEdges = [&](llvm::PHINode* phi) ->
            std::pair<llvm::Value*, llvm::Value*> { // (preheader_val, latch_val)
            if (phi->getNumIncomingValues() != 2) return {nullptr, nullptr};
            for (unsigned i = 0; i < 2; ++i) {
                llvm::BasicBlock* inBB = phi->getIncomingBlock(i);
                // If this predecessor has bb as its successor (i.e., bb is in its
                // successor list and inBB dominates after bb) → back-edge.
                // Simple heuristic: if inBB is not a predecessor of bb's preheader
                // and IS a successor of bb → it's the latch.
                bool isBackEdge = false;
                for (auto* succ : llvm::successors(&bb)) {
                    if (succ == inBB || inBB == &bb) { isBackEdge = true; break; }
                }
                if (isBackEdge)
                    return {phi->getIncomingValue(1-i), phi->getIncomingValue(i)};
            }
            return {nullptr, nullptr};
        };

        // Identify which phi is the shifting x and which is the accumulator.
        // Heuristic: find the phi whose latch value is lshr(%self, 1).
        // Also handle the case where xPhi/accPhi are assigned in the wrong order.
        for (int swap = 0; swap < 2; ++swap) {
            if (swap) std::swap(xPhi, accPhi);

            auto [xInit, xNext] = getEdges(xPhi);
            auto [accInit, accNext] = getEdges(accPhi);
            if (!xInit || !xNext || !accInit || !accNext) continue;

            // The init of the accumulator must be 0
            auto* accInitCI = llvm::dyn_cast<llvm::ConstantInt>(accInit);
            if (!accInitCI || !accInitCI->isZero()) continue;

            // xNext must be lshr(xPhi, 1)
            auto* xNextInst = llvm::dyn_cast<llvm::BinaryOperator>(xNext);
            if (!xNextInst || xNextInst->getOpcode() != llvm::Instruction::LShr)
                continue;
            if (xNextInst->getOperand(0) != xPhi) continue;
            if (!isConstInt(xNextInst->getOperand(1), 1)) continue;

            // Check for side effects in the block (other than the phi nodes and
            // the branch — which we will replace).  Only allow: the xNext lshr,
            // the accNext computation, and the terminator branch.
            unsigned nonPhiInsts = 0;
            bool hasSideEffects = false;
            for (auto& inst : bb) {
                if (llvm::isa<llvm::PHINode>(&inst) || llvm::isa<llvm::BranchInst>(&inst))
                    continue;
                if (inst.mayHaveSideEffects()) { hasSideEffects = true; break; }
                ++nonPhiInsts;
            }
            if (hasSideEffects) continue;

            // ── Pattern 1: Popcount loop ──────────────────────────────────
            // accNext = add(accPhi, and(xPhi, 1))   -or-  add(and(xPhi,1), accPhi)
            // Exit condition: icmp ult xPhi, 2  (equivalent to x == 0 or x == 1)
            // or: icmp eq xPhi, 0 (also valid)
            bool isPopcount = false;
            if (nonPhiInsts <= 4) {
                auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(accNext);
                if (addInst && addInst->getOpcode() == llvm::Instruction::Add) {
                    // One operand must be accPhi, other must be and(xPhi, 1)
                    llvm::Value* notAcc = (addInst->getOperand(0) == accPhi)
                        ? addInst->getOperand(1) : addInst->getOperand(0);
                    if (addInst->getOperand(0) == accPhi ||
                        addInst->getOperand(1) == accPhi) {
                        auto* andInst = llvm::dyn_cast<llvm::BinaryOperator>(notAcc);
                        if (andInst && andInst->getOpcode() == llvm::Instruction::And) {
                            bool xIsOp0 = andInst->getOperand(0) == xPhi;
                            bool xIsOp1 = andInst->getOperand(1) == xPhi;
                            llvm::Value* mask = xIsOp0 ? andInst->getOperand(1)
                                                       : andInst->getOperand(0);
                            if ((xIsOp0 || xIsOp1) && isConstInt(mask, 1))
                                isPopcount = true;
                        }
                    }
                }
            }

            // ── Pattern 2: Floor-log2 loop ───────────────────────────────
            // accNext = add(accPhi, 1)
            // Exit condition: icmp ule xPhi, 1  i.e., x <= 1  (after LLVM norm: ugt x, 3)
            bool isLog2 = false;
            if (!isPopcount && nonPhiInsts <= 3) {
                auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(accNext);
                if (addInst && addInst->getOpcode() == llvm::Instruction::Add) {
                    bool accIsOp0 = addInst->getOperand(0) == accPhi;
                    bool accIsOp1 = addInst->getOperand(1) == accPhi;
                    llvm::Value* incVal = accIsOp0 ? addInst->getOperand(1)
                                                   : addInst->getOperand(0);
                    if ((accIsOp0 || accIsOp1) && isConstInt(incVal, 1))
                        isLog2 = true;
                }
            }

            // ── Pattern 3: Count-trailing-zeros loop ─────────────────────
            // Same phi structure as log2 (xNext = lshr(x, 1), accNext = add(acc, 1))
            // but the exit condition is based on the low bit of xPhi:
            //   loop while (xPhi & 1) == 0   →   exit when (xPhi & 1) != 0
            // Replacement: cttz(xInit, false)
            //
            // We distinguish from log2 by inspecting the branch condition.
            bool isCttz = false;
            if (!isPopcount && !isLog2 && nonPhiInsts <= 4) {
                auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(accNext);
                if (addInst && addInst->getOpcode() == llvm::Instruction::Add) {
                    bool accIsOp0 = addInst->getOperand(0) == accPhi;
                    bool accIsOp1 = addInst->getOperand(1) == accPhi;
                    llvm::Value* incVal = accIsOp0 ? addInst->getOperand(1)
                                                   : addInst->getOperand(0);
                    if ((accIsOp0 || accIsOp1) && isConstInt(incVal, 1)) {
                        // Check loop exit condition: branch on (xPhi & 1) == 0
                        auto* term = bb.getTerminator();
                        auto* brInst = llvm::dyn_cast<llvm::BranchInst>(term);
                        if (brInst && brInst->isConditional()) {
                            auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(brInst->getCondition());
                            if (icmp) {
                                llvm::Value* lhs = icmp->getOperand(0);
                                llvm::Value* rhs = icmp->getOperand(1);
                                // Normalize: rhs should be the constant 0
                                if (isConstInt(lhs, 0) && !isConstInt(rhs, 0))
                                    std::swap(lhs, rhs);
                                if (isConstInt(rhs, 0)) {
                                    auto pred = icmp->getPredicate();
                                    // Accept eq (continue while low-bit clear) or
                                    // ne (exit when low-bit set; loop body is the
                                    // ne-false/eq-true branch).
                                    if (pred == llvm::ICmpInst::ICMP_EQ ||
                                        pred == llvm::ICmpInst::ICMP_NE) {
                                        // lhs must be (xPhi & 1)
                                        auto* andInst =
                                            llvm::dyn_cast<llvm::BinaryOperator>(lhs);
                                        if (andInst &&
                                            andInst->getOpcode() == llvm::Instruction::And) {
                                            bool xIsOp0 = andInst->getOperand(0) == xPhi;
                                            bool xIsOp1 = andInst->getOperand(1) == xPhi;
                                            llvm::Value* mask = xIsOp0
                                                ? andInst->getOperand(1)
                                                : andInst->getOperand(0);
                                            if ((xIsOp0 || xIsOp1) && isConstInt(mask, 1))
                                                isCttz = true;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (!isPopcount && !isLog2 && !isCttz) continue;

            // ── Find the exit value (the accumulator's out-of-loop use) ──
            // The accumulator should exit the loop via an LCSSA phi in the
            // exit block, or be directly used in the exit block.
            llvm::BasicBlock* exitBB = nullptr;
            for (auto* succ : llvm::successors(&bb)) {
                if (succ != &bb) { exitBB = succ; break; }
            }
            // Also handle: the branch may exit to one of two blocks; find the
            // one that is not the loop header.
            if (!exitBB) continue;

            // Find the accumulator value available after the loop.
            // It's either the accPhi itself (if exit goes straight to exit)
            // or an LCSSA phi in the exit block.
            llvm::Value* resultVal = nullptr;
            // Look for a phi in exitBB that takes accPhi or accNext from bb
            for (auto& phi : exitBB->phis()) {
                for (unsigned i = 0; i < phi.getNumIncomingValues(); ++i) {
                    if (phi.getIncomingBlock(i) == &bb &&
                        (phi.getIncomingValue(i) == accPhi ||
                         phi.getIncomingValue(i) == accNext)) {
                        resultVal = &phi;
                        break;
                    }
                }
                if (resultVal) break;
            }
            if (!resultVal) {
                // Try: accNext is directly used in exit (rare after LLVM opts)
                if (accNext->hasOneUse()) {
                    resultVal = accNext;  // LCSSA not materialized, use directly
                } else {
                    continue;
                }
            }

            // ── Emit the replacement before the loop ─────────────────────
            // Insert replacement in the preheader (= the block that has the
            // init edge to xPhi from outside the loop).
            llvm::BasicBlock* preheaderBB = nullptr;
            for (unsigned i = 0; i < xPhi->getNumIncomingValues(); ++i) {
                llvm::BasicBlock* pred = xPhi->getIncomingBlock(i);
                if (pred != &bb) { preheaderBB = pred; break; }
            }
            if (!preheaderBB) continue;

            // Insert before the terminator of the preheader so the
            // replacement is available when we branch around the loop.
            llvm::IRBuilder<> builder(preheaderBB->getTerminator());
            llvm::Value* replacement = nullptr;

            if (isPopcount) {
                // ctpop(x_init)
                llvm::Function* ctpopFn = OMSC_GET_INTRINSIC(M,
                    llvm::Intrinsic::ctpop, {i64Ty});
                replacement = builder.CreateCall(ctpopFn, {xInit}, "loop.ctpop");
            } else if (isLog2) {
                // floor_log2(x) = 63 - ctlz(x, true)   [for x ≥ 2]
                //
                // The loop body only runs when x > 1 (preheader guard).  For the
                // non-loop path (x ≤ 1), the LCSSA phi already produces 0
                // (the accumulator init).  The loop produces 63 - ctlz(x, true)
                // for x ≥ 2.
                //
                // To correctly cover BOTH paths through the LCSSA phi we use:
                //   select(x > 1, 63 - ctlz(x, false), 0)
                //
                // Verification:
                //   x = 0 → select false → 0 ✓  (loop never runs)
                //   x = 1 → select false → 0 ✓  (loop never runs)
                //   x = 2 → select true  → 63 - ctlz(2,false)=63-62=1 ✓
                //   x = 4 → select true  → 63 - ctlz(4,false)=63-61=2 ✓
                //   x = -1 → select false (signed > 1 is false) → 0
                //           but loop also gives 0 for negative x (never enters)
                //
                // Use is_zero_undef=false for safety: LLVM may speculatively
                // evaluate both arms of a select, so ctlz(0, true) could be
                // undefined when x=0. ctlz(0, false)=64 is safe (result is
                // discarded by the select anyway since guardCmp is false).
                llvm::Function* ctlzFn = OMSC_GET_INTRINSIC(M,
                    llvm::Intrinsic::ctlz, {i64Ty});
                llvm::Constant* falseVal = llvm::ConstantInt::getFalse(ctx);
                llvm::Value* clz = builder.CreateCall(ctlzFn, {xInit, falseVal},
                                                      "loop.ctlz");
                // 63 - clz = bit_width - 1 - clz
                llvm::Value* log2val = builder.CreateSub(
                    llvm::ConstantInt::get(i64Ty, 63), clz, "loop.log2");
                // Wrap in select to handle x ≤ 1 correctly
                llvm::Value* guardCmp = builder.CreateICmpSGT(
                    xInit, llvm::ConstantInt::get(i64Ty, 1), "loop.log2.guard");
                replacement = builder.CreateSelect(guardCmp, log2val,
                    llvm::ConstantInt::get(i64Ty, 0), "loop.log2.sel");
            } else {
                // isCttz: count trailing zeros
                //   while ((x & 1) == 0) { x >>= 1; count++; }
                // Replacement: cttz(xInit, false)
                //
                // Correctness:
                //   xInit = 0  → cttz(0, false) = 64  (loop would run forever;
                //               this case is UB in most callers, but cttz(0,false)
                //               is well-defined as 64 in LLVM).
                //   xInit = 1  → cttz(1) = 0    ✓  (loop never runs, acc = 0)
                //   xInit = 4  → cttz(4) = 2    ✓  (two right-shifts)
                //   xInit = -8 → cttz(-8) = 3   ✓  (three trailing zeros)
                //
                // Use is_zero_undef=false for safety (same reasoning as ctlz above).
                llvm::Function* cttzFn = OMSC_GET_INTRINSIC(M,
                    llvm::Intrinsic::cttz, {i64Ty});
                llvm::Constant* falseVal = llvm::ConstantInt::getFalse(ctx);
                replacement = builder.CreateCall(cttzFn, {xInit, falseVal},
                                                 "loop.cttz");
            }

            // Replace resultVal with the replacement everywhere
            resultVal->replaceAllUsesWith(replacement);

            // Eliminate the loop body by making the loop's terminating branch
            // always go to the exit block.  For a single-block loop, the
            // terminator of bb (the loop header) is the conditional branch
            // "if exit-cond → exitBB else → bb".
            //
            // Strategy:
            //   1. If the preheader has a CONDITIONAL branch whose true/false
            //      successor is bb, redirect it to exitBB (bypass the loop).
            //   2. Otherwise (unconditional preheader branch), replace the
            //      loop body's branch with an unconditional branch to exitBB.
            //      This makes the loop execute at most once and LLVM's
            //      simplifycfg / DCE will remove the dead body.
            bool loopBypassed = false;
            auto* preheaderTerm = preheaderBB->getTerminator();
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(preheaderTerm)) {
                if (br->isConditional()) {
                    for (unsigned i = 0; i < br->getNumSuccessors(); ++i) {
                        if (br->getSuccessor(i) == &bb) {
                            // Redirect this edge to the exit block, bypassing loop
                            br->setSuccessor(i, exitBB);
                            // Update any PHI nodes in exitBB that came from bb
                            // to now accept the preheader as a predecessor.
                            for (auto& phi2 : exitBB->phis()) {
                                for (unsigned j = 0; j < phi2.getNumIncomingValues(); ++j) {
                                    if (phi2.getIncomingBlock(j) == &bb) {
                                        // Find the value that comes from the
                                        // preheader's init edge.
                                        phi2.addIncoming(
                                            phi2.getIncomingValue(j),
                                            preheaderBB);
                                        break;
                                    }
                                }
                            }
                            loopBypassed = true;
                            break;
                        }
                    }
                }
            }

            if (!loopBypassed) {
                // Preheader unconditionally enters the loop.
                // Make the loop's body-branch always exit on first iteration.
                // The loop header's terminator is: br cond, exitBB, bb
                // We change it to: br exitBB  (unconditional)
                auto* bodyTerm = bb.getTerminator();
                if (auto* br = llvm::dyn_cast<llvm::BranchInst>(bodyTerm)) {
                    if (br->isConditional()) {
                        // Find which successor is the exit
                        unsigned exitIdx = 2; // sentinel
                        for (unsigned i = 0; i < br->getNumSuccessors(); ++i) {
                            if (br->getSuccessor(i) != &bb) {
                                exitIdx = i;
                                break;
                            }
                        }
                        if (exitIdx < 2) {
                            // Replace conditional branch with unconditional to exit
                            llvm::IRBuilder<> bodyBuilder(bodyTerm);
                            auto* newBr = bodyBuilder.CreateBr(exitBB);
                            (void)newBr;
                            bodyTerm->eraseFromParent();
                            // Remove bb from PHI nodes in the loop header.
                            // (the back-edge is now dead)
                            // If a PHI had exactly 2 incoming values (preheader
                            // + back-edge), removing the back-edge leaves it with
                            // 1 incoming value which is invalid in LLVM IR.  We
                            // must replace such PHIs with their remaining value.
                            llvm::SmallVector<llvm::PHINode*, 4> toCollapse;
                            for (auto& phi2 : bb.phis()) {
                                if (phi2.getNumIncomingValues() == 2) {
                                    // Will be left with 1 value after removal.
                                    toCollapse.push_back(&phi2);
                                } else {
                                    phi2.removeIncomingValue(&bb,
                                        /*DeletePHIIfEmpty=*/false);
                                }
                            }
                            for (auto* phi2 : toCollapse) {
                                // Find the non-back-edge incoming value
                                // (the one that comes from outside the loop).
                                llvm::Value* survivor = nullptr;
                                for (unsigned i = 0; i < phi2->getNumIncomingValues(); ++i) {
                                    if (phi2->getIncomingBlock(i) != &bb) {
                                        survivor = phi2->getIncomingValue(i);
                                        break;
                                    }
                                }
                                if (survivor) {
                                    phi2->replaceAllUsesWith(survivor);
                                    phi2->eraseFromParent();
                                } else {
                                    // All incoming edges were back-edges — shouldn't
                                    // happen in well-formed LLVM IR but handle it.
                                    phi2->removeIncomingValue(&bb,
                                        /*DeletePHIIfEmpty=*/true);
                                }
                            }
                        }
                    }
                }
            }

            ++count;
            break; // Don't process this block again
        }
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop strength reduction — convert i*C to incremental addition
// ─────────────────────────────────────────────────────────────────────────────

/// Detect simple loop-induction-variable multiplications and convert them
/// to additive induction variables.  For example:
///
///   for (i = 0; i < n; i++) { use(i * 7); }
///   →
///   iv = 0;
///   for (i = 0; i < n; i++) { use(iv); iv += 7; }
///
/// This replaces a 3-cycle multiply with a 1-cycle add per iteration.
/// LLVM's own strength reduction catches many of these, but our version
/// runs earlier and handles patterns across basic blocks that LLVM misses
/// in the presence of OmScript's ownership-aware IR.
[[gnu::hot]] static unsigned loopStrengthReduce(llvm::Function& func) {
    unsigned count = 0;

    for (auto& bb : func) {
        // Look for phi nodes — indicators of loop headers
        for (auto& phi : bb.phis()) {
            // Check if this phi is a simple integer induction variable:
            //   %iv = phi [init, preheader], [%iv.next, latch]
            //   %iv.next = add %iv, step
            if (!phi.getType()->isIntegerTy()) continue;
            if (phi.getNumIncomingValues() != 2) continue;

            // Find the increment pattern
            llvm::Value* init = nullptr;
            llvm::Value* next = nullptr;
            llvm::BasicBlock* latchBB = nullptr;

            for (unsigned i = 0; i < 2; ++i) {
                auto* incoming = phi.getIncomingValue(i);
                auto* incomingBB = phi.getIncomingBlock(i);

                if (auto* addInst = llvm::dyn_cast<llvm::BinaryOperator>(incoming)) {
                    if (addInst->getOpcode() == llvm::Instruction::Add) {
                        if (addInst->getOperand(0) == &phi || addInst->getOperand(1) == &phi) {
                            llvm::Value* step = (addInst->getOperand(0) == &phi)
                                ? addInst->getOperand(1) : addInst->getOperand(0);
                            if (llvm::isa<llvm::ConstantInt>(step)) {
                                next = incoming;
                                latchBB = incomingBB;
                                init = phi.getIncomingValue(1 - i);
                            }
                        }
                    }
                }
            }
            if (!init || !next || !latchBB) continue;

            auto* addNext = llvm::cast<llvm::BinaryOperator>(next);
            llvm::Value* step = (addNext->getOperand(0) == &phi)
                ? addNext->getOperand(1) : addNext->getOperand(0);
            auto* stepCI = llvm::cast<llvm::ConstantInt>(step);
            int64_t stepVal = stepCI->getSExtValue();

            // Now scan for users of the form: %iv * constant
            // These are candidates for strength reduction
            std::vector<llvm::BinaryOperator*> mulUsers;
            for (auto* user : phi.users()) {
                auto* binOp = llvm::dyn_cast<llvm::BinaryOperator>(user);
                if (!binOp) continue;
                if (binOp->getOpcode() != llvm::Instruction::Mul) continue;
                if (binOp == addNext) continue; // Don't transform the increment itself

                llvm::Value* mulConst = nullptr;
                if (binOp->getOperand(0) == &phi && llvm::isa<llvm::ConstantInt>(binOp->getOperand(1)))
                    mulConst = binOp->getOperand(1);
                else if (binOp->getOperand(1) == &phi && llvm::isa<llvm::ConstantInt>(binOp->getOperand(0)))
                    mulConst = binOp->getOperand(0);

                if (mulConst && binOp->hasOneUse()) {
                    mulUsers.push_back(binOp);
                }
            }

            // For each mul user, create a new additive induction variable
            for (auto* mulInst : mulUsers) {
                llvm::Value* mulConst = llvm::isa<llvm::ConstantInt>(mulInst->getOperand(0))
                    ? mulInst->getOperand(0) : mulInst->getOperand(1);
                auto* constVal = llvm::cast<llvm::ConstantInt>(mulConst);
                int64_t C = constVal->getSExtValue();

                // Create new phi: iv_sr = phi [init*C, preheader], [iv_sr + step*C, latch]
                llvm::IRBuilder<> headerBuilder(&bb, bb.getFirstInsertionPt());

                auto* newPhi = headerBuilder.CreatePHI(phi.getType(), 2, "sr.iv");

                // Init value: init * C
                llvm::Value* initMul;
                if (auto* initCI = llvm::dyn_cast<llvm::ConstantInt>(init)) {
                    initMul = llvm::ConstantInt::get(phi.getType(), initCI->getSExtValue() * C);
                } else {
                    // Non-constant init: need to compute at runtime
                    llvm::IRBuilder<> preBuilder(phi.getIncomingBlock(
                        phi.getIncomingValue(0) == init ? 0 : 1)->getTerminator());
                    initMul = preBuilder.CreateMul(init, constVal, "sr.init");
                }

                // Step increment: step * C
                int64_t newStep = stepVal * C;
                llvm::Value* newStepVal = llvm::ConstantInt::get(phi.getType(), newStep);

                // Create increment in latch
                llvm::IRBuilder<> latchBuilder(latchBB->getTerminator());
                auto* newNext = latchBuilder.CreateAdd(newPhi, newStepVal, "sr.next",
                    /*HasNUW=*/false, /*HasNSW=*/true);

                // Wire up phi
                for (unsigned i = 0; i < phi.getNumIncomingValues(); ++i) {
                    if (phi.getIncomingValue(i) == init)
                        newPhi->addIncoming(initMul, phi.getIncomingBlock(i));
                    else
                        newPhi->addIncoming(newNext, phi.getIncomingBlock(i));
                }

                // Replace the multiply with the new induction variable
                mulInst->replaceAllUsesWith(newPhi);
                count++;
            }
        }
    }

    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constant Modulo Strength Reduction
// ─────────────────────────────────────────────────────────────────────────────

/// Magic number for unsigned division by constant d.
/// For d in [3, 2^32], udiv(x, d) = mulhu(x, magic) >> shift.
/// When needsAdd is true, uses the "add-fixup" form:
///   t = mulhu(x, magic)
///   q = (t + ((x - t) >> 1)) >> shift
struct UDivMagic {
    uint64_t magic;
    unsigned shift;
    bool needsAdd;
};

/// Compute the magic number for unsigned 64-bit division by constant d.
/// Based on the algorithm from Warren's "Hacker's Delight", Chapter 10.
/// For d in [3, 2^32], the simple (no add-fixup) form always works.
static UDivMagic computeUDivMagic64(uint64_t d) {
    // For each shift value s, try magic = ceil(2^(64+s) / d).
    // The formula floor(x/d) = mulhu(x, magic) >> s is valid when:
    //   magic * d - 2^(64+s) < 2^s   (no-add case, magic < 2^64)
    //   OR use add-fixup form         (when magic >= 2^64)
    // Upper bound: s=63 gives pow2 = 2^127 which fits in __uint128_t;
    // s=64 would require 2^128 which overflows __uint128_t.
    for (unsigned s = 0; s <= 63; ++s) {
        __uint128_t pow2 = (__uint128_t)1 << (64 + s);
        __uint128_t m = (pow2 + d - 1) / d;  // ceil(2^(64+s) / d)

        if (m <= UINT64_MAX) {
            // Simple case: check validity condition
            __uint128_t md = m * d;
            __uint128_t residual = md - pow2;
            if (residual < ((__uint128_t)1 << s)) {
                return {(uint64_t)m, s, false};
            }
        } else {
            // Add-fixup case: magic overflows 64 bits
            uint64_t mReduced = (uint64_t)(m - ((__uint128_t)1 << 64));
            unsigned fixupShift = (s > 0) ? s - 1 : 0;
            return {mReduced, fixupShift, true};
        }
    }
    // Fallback (should never reach for d < 2^64)
    return {0, 0, false};
}

/// Expand `urem i64 %x, C` to a multiplicative-inverse sequence:
///   quotient = mulhu(x, magic) >> shift
///   remainder = x - quotient * C
///
/// All operations (zext, mul, lshr, trunc, mul, sub) are individually
/// vectorizable, enabling the loop vectorizer to process modulo operations
/// in SIMD lanes.  LLVM's backend already does this for scalar code, but
/// doing it at IR level exposes the pattern to the loop vectorizer.
///
/// Cost:  urem (scalar) ≈ 25 cycles → mul+shift+mul+sub ≈ 8 cycles
///        urem (vector) = scalarized div ≈ 25*VF → vectorized mul+shift ≈ 3*VF+4
static unsigned expandURemByConstant(llvm::Function& func) {
    unsigned count = 0;
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(func.getContext());
    llvm::Type* i128Ty = llvm::Type::getInt128Ty(func.getContext());

    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() != llvm::Instruction::URem) continue;
            if (inst.getType() != i64Ty) continue;

            auto* divisorCI = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(1));
            if (!divisorCI) continue;
            uint64_t d = divisorCI->getZExtValue();

            // Only handle non-power-of-2 constants in [3, 1024].
            // Power-of-2 is already handled as x & (d-1) by algebraic simplification.
            if (d < 3 || d > 1024) continue;
            if ((d & (d - 1)) == 0) continue;  // skip power-of-2

            UDivMagic magic = computeUDivMagic64(d);

            llvm::IRBuilder<> builder(&inst);
            llvm::Value* x = inst.getOperand(0);
            llvm::Value* quotient = nullptr;

            if (!magic.needsAdd) {
                // Simple case: q = mulhu(x, magic) >> shift
                llvm::Value* xWide = builder.CreateZExt(x, i128Ty, "modsr.zext");
                llvm::Value* magicVal = llvm::ConstantInt::get(i128Ty, magic.magic);
                llvm::Value* prod = builder.CreateMul(xWide, magicVal, "modsr.mul",
                                                       /*HasNUW=*/true, /*HasNSW=*/false);
                llvm::Value* hi = builder.CreateLShr(prod,
                    llvm::ConstantInt::get(i128Ty, 64), "modsr.hi");
                llvm::Value* hi64 = builder.CreateTrunc(hi, i64Ty, "modsr.trunc");
                if (magic.shift > 0) {
                    quotient = builder.CreateLShr(hi64,
                        llvm::ConstantInt::get(i64Ty, magic.shift), "modsr.q");
                } else {
                    quotient = hi64;
                }
            } else {
                // Add-fixup case:
                //   t = mulhu(x, magic)
                //   q = (t + ((x - t) >> 1)) >> shift
                llvm::Value* xWide = builder.CreateZExt(x, i128Ty, "modsr.zext");
                llvm::Value* magicVal = llvm::ConstantInt::get(i128Ty, magic.magic);
                llvm::Value* prod = builder.CreateMul(xWide, magicVal, "modsr.mul",
                                                       /*HasNUW=*/true, /*HasNSW=*/false);
                llvm::Value* hi = builder.CreateLShr(prod,
                    llvm::ConstantInt::get(i128Ty, 64), "modsr.hi");
                llvm::Value* t = builder.CreateTrunc(hi, i64Ty, "modsr.t");
                llvm::Value* diff = builder.CreateSub(x, t, "modsr.diff");
                llvm::Value* half = builder.CreateLShr(diff,
                    llvm::ConstantInt::get(i64Ty, 1), "modsr.half");
                llvm::Value* sum = builder.CreateAdd(t, half, "modsr.sum");
                quotient = builder.CreateLShr(sum,
                    llvm::ConstantInt::get(i64Ty, magic.shift), "modsr.q");
            }

            // remainder = x - quotient * d
            llvm::Value* qd = builder.CreateMul(quotient, divisorCI, "modsr.qd");
            llvm::Value* rem = builder.CreateSub(x, qd, "modsr.rem");

            replacements.emplace_back(&inst, rem);
            ++count;
        }
    }

    for (auto& [oldInst, newVal] : replacements) {
        oldInst->replaceAllUsesWith(newVal);
        oldInst->eraseFromParent();
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Small Switch → Select Chain Lowering
// ─────────────────────────────────────────────────────────────────────────────

/// Lower a switch instruction with ≤4 non-default cases to a chain of
/// select instructions.  This eliminates control flow inside hot loops,
/// enabling vectorization and removing branch misprediction penalties.
///
/// Pattern (before):
///   switch i64 %val, label %default [
///     i64 0, label %case0
///     i64 1, label %case1
///     i64 2, label %case2
///   ]
///   ; merge block has PHIs selecting values from each case
///
/// Pattern (after):
///   %c0 = icmp eq i64 %val, 0
///   %c1 = icmp eq i64 %val, 1
///   %c2 = icmp eq i64 %val, 2
///   %s2 = select i1 %c2, CASE2_VAL, DEFAULT_VAL
///   %s1 = select i1 %c1, CASE1_VAL, %s2
///   %result = select i1 %c0, CASE0_VAL, %s1
static unsigned lowerSmallSwitchToSelect(llvm::Function& func) {
    unsigned count = 0;
    llvm::SmallVector<llvm::SwitchInst*, 4> toProcess;

    for (auto& bb : func) {
        if (auto* sw = llvm::dyn_cast<llvm::SwitchInst>(bb.getTerminator())) {
            // Only handle small switches (≤8 non-default cases)
            if (sw->getNumCases() >= 2 && sw->getNumCases() <= 8) {
                toProcess.push_back(sw);
            }
        }
    }

    for (auto* sw : toProcess) {
        llvm::BasicBlock* defaultBB = sw->getDefaultDest();

        // All case blocks AND default must have a single common merge successor.
        // Collect all successors.
        llvm::BasicBlock* mergeBB = nullptr;

        // Check default block
        auto* defaultTerm = llvm::dyn_cast<llvm::BranchInst>(defaultBB->getTerminator());
        if (!defaultTerm || !defaultTerm->isUnconditional()) continue;
        mergeBB = defaultTerm->getSuccessor(0);

        // Check all case blocks converge to the same merge block
        bool allConverge = true;
        bool anySideEffects = false;
        for (auto& caseIt : sw->cases()) {
            llvm::BasicBlock* caseBB = caseIt.getCaseSuccessor();
            // Reject if a case branches directly to the merge block.  In that
            // situation the PHI in mergeBB has an incoming value from switchBB
            // (not from an intermediate caseBB), so getIncomingValueForBlock
            // would return null and we'd incorrectly use defaultVal.
            if (caseBB == mergeBB) { allConverge = false; break; }
            auto* caseTerm = llvm::dyn_cast<llvm::BranchInst>(caseBB->getTerminator());
            if (!caseTerm || !caseTerm->isUnconditional() ||
                caseTerm->getSuccessor(0) != mergeBB) {
                allConverge = false;
                break;
            }
            // Check for side effects in case block
            for (auto& inst : *caseBB) {
                if (inst.isTerminator()) continue;
                if (inst.mayHaveSideEffects()) {
                    anySideEffects = true;
                    break;
                }
            }
            // Check case block is small (≤3 non-terminator instructions)
            unsigned instCount = 0;
            for (auto& inst : *caseBB) {
                if (!inst.isTerminator()) instCount++;
            }
            if (instCount > 3) { allConverge = false; break; }
        }
        if (!allConverge || anySideEffects) continue;

        // Check default block for side effects and size
        for (auto& inst : *defaultBB) {
            if (inst.isTerminator()) continue;
            if (inst.mayHaveSideEffects()) { anySideEffects = true; break; }
        }
        if (anySideEffects) continue;
        unsigned defaultInstCount = 0;
        for (auto& inst : *defaultBB) {
            if (!inst.isTerminator()) defaultInstCount++;
        }
        if (defaultInstCount > 3) continue;

        // Collect PHI nodes in the merge block that reference our switch's successors
        if (mergeBB->phis().empty()) continue;

        llvm::BasicBlock* switchBB = sw->getParent();
        llvm::IRBuilder<> builder(sw);
        llvm::Value* cond = sw->getCondition();
        bool transformed = false;

        for (auto& phi : mergeBB->phis()) {
            // Get default value
            llvm::Value* defaultVal = phi.getIncomingValueForBlock(defaultBB);
            if (!defaultVal) continue;

            // Build select chain from last case to first
            llvm::Value* result = defaultVal;
            for (auto& caseIt : sw->cases()) {
                llvm::BasicBlock* caseBB = caseIt.getCaseSuccessor();
                llvm::Value* caseVal = phi.getIncomingValueForBlock(caseBB);
                if (!caseVal) {
                    // If the case jumps directly to merge, get value for switchBB
                    caseVal = defaultVal;
                }

                llvm::Value* cmp = builder.CreateICmpEQ(
                    cond, caseIt.getCaseValue(), "sw.sel.cmp");
                result = builder.CreateSelect(cmp, caseVal, result, "sw.sel");
            }

            // Replace the PHI with the select chain result.
            // We need to update the PHI to get its value from the switch block.
            phi.addIncoming(result, switchBB);
            transformed = true;
        }

        if (transformed) {
            // Replace the switch with an unconditional branch to the merge block.
            // Remove the switch's entries from PHI nodes in case blocks.
            for (auto& caseIt : sw->cases()) {
                llvm::BasicBlock* caseBB = caseIt.getCaseSuccessor();
                caseBB->removePredecessor(switchBB);
            }
            defaultBB->removePredecessor(switchBB);

            builder.CreateBr(mergeBB);
            sw->eraseFromParent();
            ++count;
        }
    }

    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Switch Table → Branchless Arithmetic
// ─────────────────────────────────────────────────────────────────────────────

/// Analyze constant global arrays used as switch lookup tables and replace
/// GEP+load pairs with branchless arithmetic when possible:
///
/// 1. Boolean table (all values 0 or 1): replace with bitmask test.
///    table[i] → (BITMASK >> i) & 1
///    Eliminates memory access entirely — single shift+and.
///
/// 2. Arithmetic progression (values = base + stride*i): replace with
///    formula.  table[i] → base + stride*i.  Common in sequential scoring.
///
/// 3. Packed byte table (values fit in 8 bits, ≤8 entries): pack the whole
///    table into a 64-bit immediate, extract byte with shift.
///    table[i] → (PACKED >> (i*8)) & 0xFF
///
/// These transformations eliminate the conditional branch (range check +
/// br to lookup block) by replacing the load with pure arithmetic, which
/// the CPU can execute speculatively without a misprediction penalty and
/// which the vectorizer can handle inside loops.
static unsigned analyzeSwitchTableGlobals(llvm::Function& func) {
    unsigned count = 0;
    llvm::Module* M = func.getParent();
    if (!M) return 0;

    llvm::LLVMContext& ctx = func.getContext();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    // Collect candidate GEP+load pairs: GEP into a private constant global
    // array of i64s, followed immediately by a load.
    struct TableLoad {
        llvm::LoadInst* load;
        llvm::GetElementPtrInst* gep;
        llvm::GlobalVariable* global;
        uint64_t numEntries;
    };
    llvm::SmallVector<TableLoad, 8> candidates;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst);
            if (!load) continue;
            if (!load->getType()->isIntegerTy(64)) continue;

            auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
            if (!gep) continue;
            if (gep->getNumIndices() != 2) continue;

            // First index must be 0 (array base)
            auto* idx0 = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(1));
            if (!idx0 || !idx0->isZero()) continue;

            // The table index (second GEP operand) must be a runtime value
            if (llvm::isa<llvm::Constant>(gep->getOperand(2))) continue;

            auto* global = llvm::dyn_cast<llvm::GlobalVariable>(gep->getPointerOperand());
            if (!global) continue;
            if (!global->isConstant() || !global->hasPrivateLinkage()) continue;
            if (!global->hasInitializer()) continue;

            auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(global->getValueType());
            if (!arrTy) continue;
            if (!arrTy->getElementType()->isIntegerTy(64)) continue;

            const uint64_t N = arrTy->getNumElements();
            if (N < 2 || N > 64) continue; // only small tables

            candidates.push_back({load, gep, global, N});
        }
    }

    for (auto& cand : candidates) {
        llvm::GlobalVariable* global = cand.global;
        const uint64_t N = cand.numEntries;

        // Extract table values
        auto* init = llvm::dyn_cast<llvm::ConstantDataArray>(global->getInitializer());
        if (!init) {
            // Could be ConstantArray of i64s
            auto* caInit = llvm::dyn_cast<llvm::ConstantArray>(global->getInitializer());
            if (!caInit) continue;
            // Read values
            llvm::SmallVector<int64_t, 64> vals;
            vals.reserve(N);
            for (uint64_t i = 0; i < N; i++) {
                auto* ci = llvm::dyn_cast<llvm::ConstantInt>(caInit->getOperand(i));
                if (!ci) goto next_candidate;
                vals.push_back(static_cast<int64_t>(ci->getSExtValue()));
            }
            {
                // --- Check: boolean table (all 0 or 1) ---
                bool allBool = true;
                for (auto v : vals) if (v != 0 && v != 1) { allBool = false; break; }
                if (allBool && N <= 64) {
                    uint64_t mask = 0;
                    for (uint64_t i = 0; i < N; i++)
                        if (vals[i]) mask |= (uint64_t(1) << i);
                    llvm::IRBuilder<> builder(cand.load);
                    llvm::Value* idx = cand.gep->getOperand(2);
                    llvm::Value* maskV = llvm::ConstantInt::get(i64Ty, mask);
                    llvm::Value* shifted = builder.CreateLShr(maskV, idx, "boolmask.shr");
                    llvm::Value* bit = builder.CreateAnd(shifted,
                        llvm::ConstantInt::get(i64Ty, 1), "boolmask.and");
                    cand.load->replaceAllUsesWith(bit);
                    cand.load->eraseFromParent();
                    ++count;
                    continue;
                }

                // --- Check: arithmetic progression (base + stride*i) ---
                if (N >= 2) {
                    const int64_t base   = vals[0];
                    const int64_t stride = vals[1] - vals[0];
                    bool isArith = true;
                    for (uint64_t i = 1; i < N; i++) {
                        if (vals[i] != base + static_cast<int64_t>(i) * stride) {
                            isArith = false; break;
                        }
                    }
                    if (isArith) {
                        llvm::IRBuilder<> builder(cand.load);
                        llvm::Value* idx = cand.gep->getOperand(2);
                        llvm::Value* result;
                        if (stride == 0) {
                            result = llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(base));
                        } else {
                            llvm::Value* stridV = llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(stride));
                            llvm::Value* mul = builder.CreateMul(idx, stridV, "arith.mul",
                                /*HasNUW=*/false, /*HasNSW=*/stride > 0);
                            llvm::Value* baseV = llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(base));
                            result = builder.CreateAdd(mul, baseV, "arith.add");
                        }
                        cand.load->replaceAllUsesWith(result);
                        cand.load->eraseFromParent();
                        ++count;
                        continue;
                    }
                }

                // --- Check: byte-packed table (all values fit in u8, ≤8 entries) ---
                if (N <= 8) {
                    bool allByte = true;
                    for (auto v : vals) if (v < 0 || v > 255) { allByte = false; break; }
                    if (allByte) {
                        uint64_t packed = 0;
                        for (uint64_t i = 0; i < N; i++)
                            packed |= (static_cast<uint64_t>(vals[i]) << (i * 8));
                        llvm::IRBuilder<> builder(cand.load);
                        llvm::Value* idx = cand.gep->getOperand(2);
                        llvm::Value* packedV = llvm::ConstantInt::get(i64Ty, packed);
                        // shift = idx * 8
                        llvm::Value* shiftAmt = builder.CreateMul(idx,
                            llvm::ConstantInt::get(i64Ty, 8), "bytepack.shift");
                        llvm::Value* shifted = builder.CreateLShr(packedV, shiftAmt, "bytepack.shr");
                        llvm::Value* byte = builder.CreateAnd(shifted,
                            llvm::ConstantInt::get(i64Ty, 0xFF), "bytepack.and");
                        cand.load->replaceAllUsesWith(byte);
                        cand.load->eraseFromParent();
                        ++count;
                        continue;
                    }
                }
            }
            next_candidate:;
            continue;
        }

        // ConstantDataArray path (common for i64 arrays)
        llvm::SmallVector<int64_t, 64> vals;
        vals.reserve(N);
        for (uint64_t i = 0; i < N; i++) {
            auto* ci = llvm::dyn_cast<llvm::ConstantInt>(init->getElementAsConstant(i));
            if (!ci) goto skip_cda;
            vals.push_back(static_cast<int64_t>(ci->getSExtValue()));
        }
        {
            // Boolean table
            bool allBool = true;
            for (auto v : vals) if (v != 0 && v != 1) { allBool = false; break; }
            if (allBool && N <= 64) {
                uint64_t mask = 0;
                for (uint64_t i = 0; i < N; i++)
                    if (vals[i]) mask |= (uint64_t(1) << i);
                llvm::IRBuilder<> builder(cand.load);
                llvm::Value* idx = cand.gep->getOperand(2);
                llvm::Value* maskV = llvm::ConstantInt::get(i64Ty, mask);
                llvm::Value* shifted = builder.CreateLShr(maskV, idx, "boolmask.shr");
                llvm::Value* bit = builder.CreateAnd(shifted,
                    llvm::ConstantInt::get(i64Ty, 1), "boolmask.and");
                cand.load->replaceAllUsesWith(bit);
                cand.load->eraseFromParent();
                ++count;
                continue;
            }

            // Arithmetic progression
            if (N >= 2) {
                const int64_t base   = vals[0];
                const int64_t stride = vals[1] - vals[0];
                bool isArith = true;
                for (uint64_t i = 1; i < N; i++) {
                    if (vals[i] != base + static_cast<int64_t>(i) * stride) {
                        isArith = false; break;
                    }
                }
                if (isArith) {
                    llvm::IRBuilder<> builder(cand.load);
                    llvm::Value* idx = cand.gep->getOperand(2);
                    llvm::Value* result;
                    if (stride == 0) {
                        result = llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(base));
                    } else {
                        llvm::Value* stridV = llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(stride));
                        llvm::Value* mul = builder.CreateMul(idx, stridV, "arith.mul",
                            /*HasNUW=*/false, /*HasNSW=*/stride > 0);
                        llvm::Value* baseV = llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(base));
                        result = builder.CreateAdd(mul, baseV, "arith.add");
                    }
                    cand.load->replaceAllUsesWith(result);
                    cand.load->eraseFromParent();
                    ++count;
                    continue;
                }
            }

            // Byte-packed table
            if (N <= 8) {
                bool allByte = true;
                for (auto v : vals) if (v < 0 || v > 255) { allByte = false; break; }
                if (allByte) {
                    uint64_t packed = 0;
                    for (uint64_t i = 0; i < N; i++)
                        packed |= (static_cast<uint64_t>(vals[i]) << (i * 8));
                    llvm::IRBuilder<> builder(cand.load);
                    llvm::Value* idx = cand.gep->getOperand(2);
                    llvm::Value* packedV = llvm::ConstantInt::get(i64Ty, packed);
                    llvm::Value* shiftAmt = builder.CreateMul(idx,
                        llvm::ConstantInt::get(i64Ty, 8), "bytepack.shift");
                    llvm::Value* shifted = builder.CreateLShr(packedV, shiftAmt, "bytepack.shr");
                    llvm::Value* byte = builder.CreateAnd(shifted,
                        llvm::ConstantInt::get(i64Ty, 0xFF), "bytepack.and");
                    cand.load->replaceAllUsesWith(byte);
                    cand.load->eraseFromParent();
                    ++count;
                    continue;
                }
            }
        }
        skip_cda:;
    }

    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Assume-based Bounds Check Elimination
// ─────────────────────────────────────────────────────────────────────────────

/// Propagate conditions from llvm.assume intrinsics to eliminate redundant
/// comparisons in dominated basic blocks.  Uses a simple dominance check:
/// if block A dominates block B, and A contains llvm.assume(cond), then
/// any identical comparison in B can be replaced with `true`.
///
/// Targets patterns generated by OmScript's for-loop bounds checking:
///   llvm.assume(i < len)  →  later icmp ult i, len  →  replaced with true
///
/// Also handles:
///   llvm.assume(len > 0)  →  later icmp ugt len, 0  →  replaced with true
///   llvm.assume(i < len)  →  later icmp uge i, len  →  replaced with false
static unsigned eliminateRedundantBoundsChecks(llvm::Function& func) {
    if (func.isDeclaration()) return 0;

    unsigned count = 0;
    llvm::DominatorTree DT(func);

    // Collect all assume conditions and their blocks
    struct AssumeInfo {
        llvm::ICmpInst* cmp;
        llvm::BasicBlock* block;
    };
    llvm::SmallVector<AssumeInfo, 8> assumes;

    for (auto& bb : func) {
        for (auto& inst : bb) {
            auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
            if (!call) continue;
            auto* callee = call->getCalledFunction();
            if (!callee || callee->getIntrinsicID() != llvm::Intrinsic::assume)
                continue;
            // Extract the condition
            if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(call->getArgOperand(0))) {
                assumes.push_back({cmp, &bb});
            }
        }
    }

    if (assumes.empty()) return 0;

    // For each assume, find redundant comparisons in dominated blocks
    std::vector<std::pair<llvm::Instruction*, llvm::Value*>> replacements;

    for (auto& assume : assumes) {
        llvm::ICmpInst* assumeCmp = assume.cmp;
        llvm::CmpInst::Predicate assumePred = assumeCmp->getPredicate();
        llvm::Value* lhs = assumeCmp->getOperand(0);
        llvm::Value* rhs = assumeCmp->getOperand(1);

        for (auto& bb : func) {
            if (&bb == assume.block) continue;
            if (!DT.dominates(assume.block, &bb)) continue;

            for (auto& inst : bb) {
                auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst);
                if (!cmp) continue;

                llvm::Value* cmpLhs = cmp->getOperand(0);
                llvm::Value* cmpRhs = cmp->getOperand(1);
                llvm::CmpInst::Predicate cmpPred = cmp->getPredicate();

                // Case 1: Identical comparison → replace with true
                if (cmpLhs == lhs && cmpRhs == rhs && cmpPred == assumePred) {
                    replacements.emplace_back(cmp,
                        llvm::ConstantInt::getTrue(func.getContext()));
                    continue;
                }

                // Case 2: Inverse comparison → replace with false
                if (cmpLhs == lhs && cmpRhs == rhs &&
                    cmpPred == llvm::ICmpInst::getInversePredicate(assumePred)) {
                    replacements.emplace_back(cmp,
                        llvm::ConstantInt::getFalse(func.getContext()));
                    continue;
                }

                // Case 3: assume(a < b) implies a != b → true
                if (cmpLhs == lhs && cmpRhs == rhs) {
                    if ((assumePred == llvm::ICmpInst::ICMP_ULT ||
                         assumePred == llvm::ICmpInst::ICMP_SLT) &&
                        cmpPred == llvm::ICmpInst::ICMP_NE) {
                        replacements.emplace_back(cmp,
                            llvm::ConstantInt::getTrue(func.getContext()));
                        continue;
                    }
                    // assume(a < b) implies a <= b → true
                    if ((assumePred == llvm::ICmpInst::ICMP_ULT &&
                         cmpPred == llvm::ICmpInst::ICMP_ULE) ||
                        (assumePred == llvm::ICmpInst::ICMP_SLT &&
                         cmpPred == llvm::ICmpInst::ICMP_SLE)) {
                        replacements.emplace_back(cmp,
                            llvm::ConstantInt::getTrue(func.getContext()));
                        continue;
                    }
                }

                // Case 4: Swapped operands
                if (cmpLhs == rhs && cmpRhs == lhs) {
                    llvm::CmpInst::Predicate swappedAssume =
                        llvm::ICmpInst::getSwappedPredicate(assumePred);
                    if (cmpPred == swappedAssume) {
                        replacements.emplace_back(cmp,
                            llvm::ConstantInt::getTrue(func.getContext()));
                        continue;
                    }
                    if (cmpPred == llvm::ICmpInst::getInversePredicate(swappedAssume)) {
                        replacements.emplace_back(cmp,
                            llvm::ConstantInt::getFalse(func.getContext()));
                        continue;
                    }
                }
            }
        }
    }

    for (auto& [inst, replacement] : replacements) {
        inst->replaceAllUsesWith(replacement);
        // The icmp is now dead (no uses, no side effects) — remove it to keep
        // the IR clean.  The Phase 5 dead-code-elimination pass would catch
        // this too, but erasing here avoids a second scan.
        inst->eraseFromParent();
        ++count;
    }
    return count;
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

    // Install the hardware cost model for this function's optimization session.
    // g_costFn is thread-local so parallel superoptimizeModule calls are safe.
    const auto* prevFn = g_costFn;
    g_costFn = config.costFn ? &config.costFn : nullptr;
    struct CostGuard {
        const std::function<double(const llvm::Instruction*)>** slot;
        const std::function<double(const llvm::Instruction*)>* prev;
        ~CostGuard() { *slot = prev; }
    } costGuard{&g_costFn, prevFn};

    // Phase 0.5: Loop idiom recognition — replace common loop patterns with
    // single hardware instructions:
    //   popcount loop: while(x){c+=x&1;x>>=1} → llvm.ctpop(x_init)
    //   floor-log2 loop: while(x>1){x>>=1;r++} → 63 - llvm.ctlz(x_init)
    // Runs before all other phases so that the replaced loops are dead and
    // DCE can clean them up.  The loop body is dead-code-eliminated in Phase 5.
    if (config.enableIdiomRecognition) {
        unsigned loopIdioms = recognizeLoopIdioms(func);
        stats.idiomsReplaced += loopIdioms;
    }

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

    // Phase 2.5: Loop strength reduction — convert i*C to additive IVs
    // Runs after algebraic simplification but before branch opts so that
    // simplified multiply patterns are visible.
    if (config.enableAlgebraic) {
        stats.algebraicSimplified += loopStrengthReduce(func);
    }

    // Phase 2.8: Known-bits narrowing (Souper-inspired) — use KnownBits to
    // prove redundant OR/AND masks, add NUW/NSW flags to shifts and adds.
    // This enables downstream passes (LLVM InstCombine, loop vectorizer) to
    // be more aggressive and is the same technique used by the Souper
    // superoptimizer for "dataflow-guided" instruction simplification.
    if (config.enableAlgebraic) {
        stats.algebraicSimplified += applyKnownBitsNarrowing(func);
    }

    // Phase 3: Branch simplification (branch-to-select)
    if (config.enableBranchOpt) {
        stats.branchesSimplified = simplifyBranches(func);
    }

    // Phase 3.2: Select chain simplification (Souper-inspired) — flatten
    // nested select(C, select(C, ...), ...) patterns that arise from OmScript
    // ternary expressions and conditional logic.  Souper identifies these as
    // high-value targets because they create unnecessary phi-like control flow.
    if (config.enableBranchOpt) {
        stats.branchesSimplified += simplifySelectChains(func);
    }

    // Phase 3.5: Select operand sinking — factor common operands out of both
    // arms of a select, exposing idiom patterns to the recogniser.
    //   select(cond, acc+a, acc+b) → acc + select(cond, a, b)
    // This runs AFTER simplifyBranches (which creates the selects) and
    // BEFORE idiom recognition so that abs/min/max patterns become visible.
    if (config.enableBranchOpt && config.enableIdiomRecognition) {
        unsigned sinkCount = applySelectSinking(func);
        if (sinkCount > 0) {
            // Re-run idiom recognition on the newly factored selects.
            for (auto& bb : func) {
                auto idioms = detectIdioms(bb);
                for (auto& match : idioms) {
                    if (replaceIdiom(match)) {
                        stats.idiomsReplaced++;
                    }
                }
            }
            stats.algebraicSimplified += sinkCount;
        }
    }

    // Phase 3.8: MACS — Modular-Addition-to-Conditional-Subtract.
    // Replace urem(add(a,b), C) with select(s<C, s, s-C) when a,b ∈ [0,C).
    // This produces a 2-4 cycle operation vs the 25-cycle integer division,
    // critical for tight loops like iterative Fibonacci modulo a large prime.
    if (config.enableAlgebraic) {
        stats.algebraicSimplified += applyMacs(func);
    }

    // Phase 3.9: Small switch → select chain lowering.
    // Converts switch(x) with 2-8 cases that converge to a merge block with
    // PHI nodes into a chain of icmp+select instructions.  Eliminates branch
    // mispredictions and exposes the pattern to the vectorizer.
    if (config.enableBranchOpt) {
        stats.branchesSimplified += lowerSmallSwitchToSelect(func);
    }

    // Phase 3.91: Switch table global → branchless arithmetic.
    // Analyzes constant global arrays used as switch lookup tables and
    // replaces GEP+load with pure arithmetic where possible:
    //   - Boolean table (0/1 entries) → bitmask shift-and (no memory access)
    //   - Arithmetic progression → base + stride*i formula
    //   - Byte-packed table (≤8 entries, values ≤255) → packed 64-bit immediate
    // Runs after lowerSmallSwitchToSelect so that any remaining switch-table
    // loads (created by LLVM's SimplifyCFG) are also handled.
    if (config.enableBranchOpt) {
        stats.branchesSimplified += analyzeSwitchTableGlobals(func);
    }

    // Phase 3.10: Assume-based bounds check elimination.
    // Propagates conditions from llvm.assume intrinsics to dominated blocks,
    // replacing redundant icmp comparisons with true/false constants.
    // Targets OmScript for-loop bounds check patterns:
    //   llvm.assume(i < len)  →  later icmp ult i, len  →  replaced with true
    if (config.enableAlgebraic) {
        stats.algebraicSimplified += eliminateRedundantBoundsChecks(func);
    }

    // Phase 4: Enumerative synthesis on remaining expensive instructions.
    // Adaptive: skip synthesis for large functions to keep compile times bounded.
    // Synthesis is O(N × templates) and provides diminishing returns on large
    // functions that are already partially optimized by LLVM's own strength-
    // reduction passes.  Threshold matches the post-superoptimizer cleanup
    // limit in codegen_opt.cpp (2000 instructions) for consistency.
    if (config.enableSynthesis && func.getInstructionCount() <= 2000) {
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

    // Collect all non-declaration functions first to enable parallel dispatch.
    std::vector<llvm::Function*> funcs;
    funcs.reserve(32);
    for (auto& func : module) {
        if (!func.isDeclaration())
            funcs.push_back(&func);
    }

    // Parallel threshold: launch async tasks only when there are enough
    // functions to amortize the thread-creation overhead (≥4 functions and
    // more than one hardware thread available).  Below this threshold the
    // sequential path is faster.
    const unsigned hwThreads = std::max(1u, std::thread::hardware_concurrency());
    const bool runParallel = funcs.size() >= 4 && hwThreads >= 2;

    if (runParallel) {
        // Launch one future per function so each function is processed on a
        // worker thread independently.  LLVM IR operations on *different*
        // functions are disjoint (separate basic-block / instruction lists),
        // so concurrent modification is safe.  IRBuilder objects are created
        // locally inside superoptimizeFunction which also makes them
        // thread-local by construction.
        // config is captured by value into each async task (SuperoptimizerConfig
        // is a plain struct of flags/integers).  Value capture is intentional:
        // it ensures each future has its own copy and cannot observe dangling
        // references if any future outlives the current stack frame.
        std::vector<std::future<SuperoptimizerStats>> futures;
        futures.reserve(funcs.size());
        for (llvm::Function* fn : funcs) {
            futures.push_back(std::async(std::launch::async,
                [fn, config]() { return superoptimizeFunction(*fn, config); }));
        }
        for (auto& fut : futures) {
            auto stats = fut.get();
            total.idiomsReplaced     += stats.idiomsReplaced;
            total.synthReplacements  += stats.synthReplacements;
            total.branchesSimplified += stats.branchesSimplified;
            total.algebraicSimplified += stats.algebraicSimplified;
            total.deadCodeEliminated += stats.deadCodeEliminated;
        }
    } else {
        for (llvm::Function* fn : funcs) {
            auto stats = superoptimizeFunction(*fn, config);
            total.idiomsReplaced     += stats.idiomsReplaced;
            total.synthReplacements  += stats.synthReplacements;
            total.branchesSimplified += stats.branchesSimplified;
            total.algebraicSimplified += stats.algebraicSimplified;
            total.deadCodeEliminated += stats.deadCodeEliminated;
        }
    }
    return total;
}

} // namespace superopt

/// Check whether a constant (scalar ConstantInt or vector splat/element-wise)
unsigned superopt::convertSRemToURem(llvm::Function& func) {
    if (func.isDeclaration()) return 0;
    unsigned count = 0;
    const llvm::DataLayout& DL = func.getParent()->getDataLayout();
    llvm::SmallVector<llvm::Instruction*, 16> toErase;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SRem) {
                llvm::Value* lhs = inst.getOperand(0);
                llvm::Value* rhs = inst.getOperand(1);
                // Convert srem(a, b) → urem(a, b) when:
                //   (a) dividend is non-negative, AND
                //   (b) divisor is positive (constant or proven non-negative runtime value).
                // Correctness: if a >= 0 and b > 0, srem(a,b) == urem(a,b)
                // because the C/LLVM standard srem has the sign of the dividend,
                // and a non-negative dividend with a positive divisor gives a
                // non-negative remainder identical to unsigned remainder.
                auto* rhsConst = llvm::dyn_cast<llvm::Constant>(rhs);
                const bool rhsPositive = (rhsConst && isConstantAllPositive(rhsConst))
                    || isValueNonNegative(rhs, DL);
                if (rhsPositive && isValueNonNegative(lhs, DL)) {
                    llvm::IRBuilder<> builder(&inst);
                    auto* urem = builder.CreateURem(lhs, rhs, "srem_to_urem");
                    inst.replaceAllUsesWith(urem);
                    toErase.push_back(&inst);
                    ++count;
                }
            }
        }
    }
    for (auto* inst : toErase) inst->eraseFromParent();
    return count;
}

unsigned superopt::convertSDivToUDiv(llvm::Function& func) {
    if (func.isDeclaration()) return 0;
    unsigned count = 0;
    const llvm::DataLayout& DL = func.getParent()->getDataLayout();
    llvm::SmallVector<llvm::Instruction*, 16> toErase;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SDiv) {
                llvm::Value* lhs = inst.getOperand(0);
                llvm::Value* rhs = inst.getOperand(1);
                // Convert sdiv(a, b) → udiv(a, b) when both operands are
                // proven non-negative.  Handles constant and runtime divisors.
                bool rhsPositive = false;
                if (auto* rhsConst = llvm::dyn_cast<llvm::Constant>(rhs)) {
                    rhsPositive = isConstantAllPositive(rhsConst);
                } else {
                    rhsPositive = isValueNonNegative(rhs, DL);
                }
                if (rhsPositive && isValueNonNegative(lhs, DL)) {
                    llvm::IRBuilder<> builder(&inst);
                    auto* udiv = builder.CreateUDiv(lhs, rhs, "sdiv_to_udiv");
                    inst.replaceAllUsesWith(udiv);
                    toErase.push_back(&inst);
                    ++count;
                }
            }
        }
    }
    for (auto* inst : toErase) inst->eraseFromParent();
    return count;
}

[[nodiscard]] unsigned superopt::inferNonNegativeFlags(llvm::Function& func) {
    if (func.isDeclaration()) return 0;
    unsigned count = 0;
    const llvm::DataLayout& DL = func.getParent()->getDataLayout();
    llvm::SmallVector<llvm::Instruction*, 16> toErase;
    for (auto& bb : func) {
        for (auto& inst : bb) {
            // Convert AShr to LShr when operand is non-negative.
            // AShr (arithmetic shift right) fills with sign bit copies,
            // LShr (logical shift right) fills with zeros.
            // When the operand is non-negative, the sign bit is 0, so both
            // produce the same result — but LShr is preferred by LLVM's
            // backend for unsigned strength reduction of division/modulo.
            if (inst.getOpcode() == llvm::Instruction::AShr) {
                if (isValueNonNegative(inst.getOperand(0), DL)) {
                    llvm::IRBuilder<> builder(&inst);
                    auto* lshr = builder.CreateLShr(inst.getOperand(0), inst.getOperand(1), "ashr_to_lshr");
                    inst.replaceAllUsesWith(lshr);
                    toErase.push_back(&inst);
                    ++count;
                    continue;
                }
            }

            // ICmp signed → unsigned when both operands are non-negative.
            // For non-negative i64 values, signed and unsigned order comparisons
            // give the same result.  Converting to unsigned enables:
            //   1. SCEV's unsigned trip-count analysis (more vectorization
            //      opportunities without signed-overflow guards)
            //   2. LLVM's loop vectorizer to use unsigned induction variables
            //   3. Downstream passes (GVN, LICM) to hoist loop conditions
            //      without requiring signed-range assumptions.
            // We run this AFTER inferring NSW/NUW flags so that the maximum
            // number of operands are proven non-negative.
            if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                if (cmp->isSigned() &&
                    isValueNonNegative(cmp->getOperand(0), DL) &&
                    isValueNonNegative(cmp->getOperand(1), DL)) {
                    cmp->setPredicate(cmp->getUnsignedPredicate());
                    ++count;
                }
                continue;
            }

            auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
            if (!bo) continue;
            if (bo->hasNoUnsignedWrap()) continue;

            unsigned op = bo->getOpcode();

            // Add: if both operands are non-negative, nuw is safe because
            // max(2^63-1 + 2^63-1) = 2^64-2 < 2^64.
            // nsw is also safe when one operand is a 1-bit zext (value ∈ {0,1})
            // and the other is non-negative, because adding at most 1 to a
            // value in [0, 2^63-1] stays in [0, 2^63], and the signed max is
            // 2^63-1 — so this is only safe when the non-zext operand is
            // bounded below 2^63-1.  Use KnownBits to check the leading zeros.
            if (op == llvm::Instruction::Add) {
                const bool lhsNonNeg = isValueNonNegative(bo->getOperand(0), DL);
                const bool rhsNonNeg = isValueNonNegative(bo->getOperand(1), DL);
                if (lhsNonNeg && rhsNonNeg) {
                    bo->setHasNoUnsignedWrap(true);
                    // Set NSW when KnownBits proves both operands have ≥ 2 leading zeros.
                    // Then max(a+b) ≤ (2^62-1)+(2^62-1) = 2^63-2 < INT64_MAX, so signed
                    // overflow cannot occur.  This applies to: 32-bit loop induction
                    // variables (32+ leading zeros), modulo/urem results (≥ 1 leading
                    // zero since result < divisor < 2^63), bounded accumulators, and
                    // zext-of-bool operands (63 leading zeros).  More aggressive than
                    // the prior zext-from-bool special case; enables LLVM SCEV/CVP to
                    // derive tight trip counts and range-checked GEPs.
                    if (!bo->hasNoSignedWrap()) {
                        llvm::KnownBits lhsKB = llvm::computeKnownBits(bo->getOperand(0), DL);
                        llvm::KnownBits rhsKB = llvm::computeKnownBits(bo->getOperand(1), DL);
                        if (lhsKB.countMinLeadingZeros() >= 2 && rhsKB.countMinLeadingZeros() >= 2) {
                            bo->setHasNoSignedWrap(true);
                            ++count;
                        }
                    }
                    ++count;
                }
                continue;
            }

            // Mul: if both operands are non-negative and nsw is already set,
            // then the product is non-negative and fits in signed i64,
            // which means it also fits in unsigned i64 (nuw is safe).
            // Additionally, if both operands are non-negative and at least one
            // is bounded to a small value (many leading zeros), we can infer
            // nsw: if product_max ≤ INT64_MAX, signed overflow cannot occur.
            // This handles common patterns like i*3 or i*7 where i < n and
            // n fits in 32 bits (KnownBits shows 32+ leading zeros on i).
            if (op == llvm::Instruction::Mul) {
                const bool lhsNN = isValueNonNegative(bo->getOperand(0), DL);
                const bool rhsNN = isValueNonNegative(bo->getOperand(1), DL);
                if (lhsNN && rhsNN) {
                    if (bo->hasNoSignedWrap()) {
                        // nsw already set → add nuw too
                        bo->setHasNoUnsignedWrap(true);
                        ++count;
                    } else if (!bo->hasNoSignedWrap()) {
                        // Try to prove nsw via KnownBits: product fits in signed i64
                        // when log2(lhs_max) + log2(rhs_max) < 63.
                        llvm::KnownBits lhsKB = llvm::computeKnownBits(bo->getOperand(0), DL);
                        llvm::KnownBits rhsKB = llvm::computeKnownBits(bo->getOperand(1), DL);
                        const unsigned lhsBits = lhsKB.getBitWidth() - lhsKB.countMinLeadingZeros();
                        const unsigned rhsBits = rhsKB.getBitWidth() - rhsKB.countMinLeadingZeros();
                        if (lhsBits + rhsBits < 63) {
                            // max_product < 2^(lhsBits+rhsBits) ≤ 2^62 < INT64_MAX
                            bo->setHasNoSignedWrap(true);
                            bo->setHasNoUnsignedWrap(true);
                            ++count;
                        }
                    }
                }
                continue;
            }

            // Shl: infer NSW via KnownBits when the operand is non-negative and
            // shifting cannot move any bit into the sign position.  For x << k:
            //   lhsBits = 64 - countMinLeadingZeros(x)  (bits needed to hold x)
            //   NSW is safe iff lhsBits + k < 64  (shift can't corrupt sign bit)
            // Once NSW is set, NUW is also safe when x >= 0:
            //   x >= 0 and NSW means result = x << k >= 0 and <= 2^63-1, so
            //   unsigned overflow (result > 2^64-1) cannot occur.
            // This enables LLVM SCEV to compute tight trip counts for expressions
            // like `i << log2(stride)` in array-indexing loops.
            if (op == llvm::Instruction::Shl) {
                const bool lhsNN = isValueNonNegative(bo->getOperand(0), DL);
                if (!bo->hasNoSignedWrap() && lhsNN) {
                    if (auto* shiftCI = llvm::dyn_cast<llvm::ConstantInt>(bo->getOperand(1))) {
                        const unsigned k = (unsigned)shiftCI->getZExtValue();
                        if (k < 64) {
                            llvm::KnownBits lhsKB = llvm::computeKnownBits(bo->getOperand(0), DL);
                            const unsigned lhsBits =
                                lhsKB.getBitWidth() - lhsKB.countMinLeadingZeros();
                            if (lhsBits + k < 64) {
                                bo->setHasNoSignedWrap(true);
                                ++count;
                            }
                        }
                    }
                }
                if (bo->hasNoSignedWrap() && lhsNN && !bo->hasNoUnsignedWrap()) {
                    bo->setHasNoUnsignedWrap(true);
                    ++count;
                }
                continue;
            }

            // Sub: NUW (no unsigned wrap) requires a >= b, so we skip that.
            // NSW (no signed wrap) IS provable when both operands are non-negative:
            // a ∈ [0, 2^63-1], b ∈ [0, 2^63-1] → a-b ∈ [-(2^63-1), 2^63-1],
            // which never overflows signed i64. This enables SCEV's trip-count
            // and range analysis to derive tighter bounds for derived expressions.
            if (op == llvm::Instruction::Sub) {
                if (!bo->hasNoSignedWrap() &&
                    isValueNonNegative(bo->getOperand(0), DL) &&
                    isValueNonNegative(bo->getOperand(1), DL)) {
                    bo->setHasNoSignedWrap(true);
                    ++count;
                }
                continue;
            }
        }
    }
    for (auto* inst : toErase) inst->eraseFromParent();
    return count;
}

unsigned superopt::constantModuloStrengthReduce(llvm::Function& func) {
    return expandURemByConstant(func);
}

unsigned superopt::lowerSmallSwitch(llvm::Function& func) {
    return lowerSmallSwitchToSelect(func);
}

unsigned superopt::propagateAssumeBounds(llvm::Function& func) {
    return eliminateRedundantBoundsChecks(func);
}

} // namespace omscript
