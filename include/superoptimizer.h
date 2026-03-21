#pragma once

#ifndef SUPEROPTIMIZER_H
#define SUPEROPTIMIZER_H

/// @file superoptimizer.h
/// @brief Superoptimizer for LLVM IR — finds globally optimal instruction sequences.
///
/// This module implements a practical superoptimizer that operates on LLVM IR
/// to discover shorter or cheaper instruction sequences that compute the same
/// function as the original code. Unlike traditional peephole optimizers that
/// use hand-written patterns, the superoptimizer uses:
///
///   1. **Idiom Recognition** — detects high-level patterns in low-level IR
///      (e.g., popcount loops, byte-reversal, bit rotation, min/max)
///   2. **Enumerative Synthesis** — for small expression trees, exhaustively
///      searches the space of possible replacements up to a cost bound
///   3. **Concrete Verification** — validates candidate replacements against
///      multiple test vectors to ensure semantic equivalence
///   4. **Cost-Driven Selection** — only applies a replacement if it is
///      strictly cheaper than the original sequence
///
/// Integration:
///   The superoptimizer runs as a late-stage pass after LLVM's standard
///   optimization pipeline, catching patterns that individual passes miss.
///   It is enabled at O2+ and can be controlled with -fsuperopt / -fno-superopt.

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <optional>

namespace omscript {
namespace superopt {

// ─────────────────────────────────────────────────────────────────────────────
// Cost model for LLVM IR instructions
// ─────────────────────────────────────────────────────────────────────────────

/// Returns the estimated execution cost of a single LLVM instruction
/// on a modern x86-64 out-of-order CPU (latency in cycles).
double instructionCost(const llvm::Instruction* inst);

/// Returns the total cost of all instructions in a basic block.
double blockCost(const llvm::BasicBlock* bb);

// ─────────────────────────────────────────────────────────────────────────────
// Concrete evaluator — runs LLVM IR snippets on test vectors
// ─────────────────────────────────────────────────────────────────────────────

/// A test vector maps value names to concrete 64-bit values.
struct TestVector {
    std::vector<uint64_t> inputs;
    uint64_t expectedOutput;
};

/// Evaluate a simple expression tree on a set of concrete inputs.
/// Returns std::nullopt if the expression cannot be evaluated (e.g., memory ops).
std::optional<uint64_t> evaluateInst(const llvm::Instruction* inst,
                                      const std::vector<uint64_t>& argValues);

// ─────────────────────────────────────────────────────────────────────────────
// Idiom patterns — recognized instruction sequences
// ─────────────────────────────────────────────────────────────────────────────

/// Recognized idiom types.
enum class Idiom {
    None,
    PopCount,       ///< Population count (number of 1-bits)
    ByteSwap,       ///< Byte reversal (endian swap)
    RotateLeft,     ///< Bit rotation left
    RotateRight,    ///< Bit rotation right
    CountLeadingZeros,  ///< CLZ
    CountTrailingZeros, ///< CTZ
    AbsoluteValue,  ///< abs(x) = x < 0 ? -x : x
    IntMin,         ///< min(a,b) via select or branch
    IntMax,         ///< max(a,b) via select or branch
    IsPowerOf2,     ///< (x & (x-1)) == 0
    SignExtend,     ///< Manual sign extension
    BitFieldExtract,///< Shift-and-mask pattern
    MultiplyByConst,///< Multi-instruction multiply sequence
    DivideByConst,  ///< Multi-instruction divide sequence
    ConditionalNeg, ///< Conditional negation pattern
    SaturatingAdd,  ///< Addition with overflow clamp
    SaturatingSub,  ///< Subtraction with underflow clamp
};

/// Result of idiom detection on an instruction or sequence.
struct IdiomMatch {
    Idiom idiom = Idiom::None;
    llvm::Instruction* rootInst = nullptr;  ///< The instruction that produces the final result
    std::vector<llvm::Value*> operands;     ///< Extracted operands for the idiom
    unsigned bitWidth = 64;                 ///< Bit width of the operation
};

/// Scan a basic block for recognized idioms.
std::vector<IdiomMatch> detectIdioms(llvm::BasicBlock& bb);

// ─────────────────────────────────────────────────────────────────────────────
// Synthesis engine — generates optimal replacement sequences
// ─────────────────────────────────────────────────────────────────────────────

/// Configuration for the synthesis search.
struct SynthesisConfig {
    unsigned maxInstructions = 3;   ///< Maximum instructions in synthesized sequence
    unsigned numTestVectors = 16;   ///< Number of test vectors for verification
    double costThreshold = 0.8;     ///< Only replace if new cost < threshold * old cost
};

/// Attempt to synthesize a cheaper replacement for the given instruction.
/// Returns true if a replacement was made.
bool synthesizeReplacement(llvm::Instruction* inst, const SynthesisConfig& config);

// ─────────────────────────────────────────────────────────────────────────────
// Superoptimizer pass — the main entry point
// ─────────────────────────────────────────────────────────────────────────────

/// Configuration for the superoptimizer.
struct SuperoptimizerConfig {
    bool enableIdiomRecognition = true;  ///< Detect and replace known idioms
    bool enableSynthesis = true;         ///< Enumerative synthesis for small sequences
    bool enableBranchOpt = true;         ///< Branch-to-select conversion
    bool enableAlgebraic = true;         ///< Algebraic identity simplification
    bool enableDeadCodeElim = true;      ///< Remove dead instructions after optimization
    SynthesisConfig synthesis;
};

/// Statistics from a superoptimizer run.
struct SuperoptimizerStats {
    unsigned idiomsReplaced = 0;
    unsigned synthReplacements = 0;
    unsigned branchesSimplified = 0;
    unsigned algebraicSimplified = 0;
    unsigned deadCodeEliminated = 0;
    double estimatedSpeedup = 0.0;     ///< Estimated percentage improvement
};

/// Run the superoptimizer on a single LLVM function.
/// Returns statistics about optimizations applied.
SuperoptimizerStats superoptimizeFunction(llvm::Function& func,
                                           const SuperoptimizerConfig& config = {});

/// Run the superoptimizer on all functions in a module.
SuperoptimizerStats superoptimizeModule(llvm::Module& module,
                                         const SuperoptimizerConfig& config = {});

/// Convert srem-by-positive-constant → urem when the dividend is provably
/// non-negative.  This is factored out as a standalone pass so it can run
/// before the loop vectorizer (the vectorizer's cost model favours urem
/// over srem because urem avoids the signed-correction fixup).
/// Returns the number of srem instructions converted.
unsigned convertSRemToURem(llvm::Function& func);

/// Convert sdiv-by-positive-constant → udiv when the dividend is provably
/// non-negative.  Like convertSRemToURem, this runs before the LLVM pipeline
/// so the vectorizer and loop optimizer see the cheaper unsigned operation.
/// Returns the number of sdiv instructions converted.
unsigned convertSDivToUDiv(llvm::Function& func);

/// Infer nuw (no unsigned wrap) flags on add instructions where both operands
/// are provably non-negative.  This is particularly useful after loop
/// unrolling, where the unroller creates add instructions without flags for
/// unrolled copies.  Setting nuw enables downstream convertSRemToURem to
/// prove non-negativity through the add chain.
/// Returns the number of instructions updated.
unsigned inferNonNegativeFlags(llvm::Function& func);

} // namespace superopt
} // namespace omscript

#endif // SUPEROPTIMIZER_H
