#pragma once

#ifndef POLYOPT_H
#define POLYOPT_H

/// @file polyopt.h
/// @brief OmPolyOpt — Native Polyhedral Loop Optimizer for OmScript IR.
///
/// OmPolyOpt implements a Polly-style polyhedral optimization pipeline
/// operating directly on LLVM IR.  It extracts Static Control Parts (SCoPs)
/// from LLVM IR using Scalar Evolution (SCEV), builds polyhedral models of
/// loop nests and their memory accesses, performs dependence analysis, applies
/// a suite of legal and profitable loop transformations, then regenerates LLVM
/// IR for the transformed loop nests.
///
/// ## Pipeline Overview
///
///   1. **SCoP Detection** — identifies maximal affine regions (loops with
///      affine bounds and array subscripts, no calls, no irregular control
///      flow).  Uses SCEV to verify affinity of induction variables.
///
///   2. **Polyhedral Model Construction** — for each detected SCoP, builds:
///      - Iteration domains: polyhedra describing the set of valid iteration
///        vectors for each statement (e.g., {[i,j] : 0 ≤ i < N, 0 ≤ j < M}).
///      - Access functions: affine maps from iteration vectors to memory
///        addresses for each load and store.
///      - Dependence polyhedra: computed via GCD/Banerjee/Fourier-Motzkin
///        tests, describing pairs of iterations with memory dependences.
///
///   3. **Dependence Analysis** — Fourier-Motzkin elimination to test
///      feasibility of dependence systems; identifies RAW, WAR, and WAW
///      dependences and their distance vectors.
///
///   4. **Transformation Selection** — a cost model selects from:
///      - **Loop Tiling** (cache blocking): tiles loop nests by powers-of-2
///        tile sizes derived from L1/L2 cache sizes; provably legal when the
///        dependence distance is loop-independent or points inward.
///      - **Loop Interchange**: reorders loop levels to maximize memory
///        locality in the innermost loop; legal when all dependence vectors
///        satisfy the interchange legality condition.
///      - **Loop Skewing**: adds a multiple of an outer IV to an inner IV to
///        change the dependence direction; used to make wavefront parallelism
///        available or to enable strip-mining.
///      - **Loop Reversal**: reverses the direction of a loop; legal when no
///        loop-carried dependence in that dimension.
///      - **Loop Fusion**: merges adjacent loops with compatible iteration
///        spaces; improves register reuse and reduces loop overhead.
///      - **Loop Fission** (distribution): splits a loop body into multiple
///        independent loops for better vectorization.
///
///   5. **Legality Checking** — all transformations are verified via the
///      dependence distance / direction matrix: a transformation T is legal
///      iff for every dependence d, T·d ≥ 0 lexicographically.
///
///   6. **Code Generation** — emits the transformed loop nest back into LLVM
///      IR using SCEVExpander and IRBuilder.  Handles:
///      - Tile loop nests with proper tile/point loop IV relationships.
///      - Updated GEP subscript expressions using the new induction variables.
///      - Correct PHI node setup for tile and point loops.
///      - Preservation of all loop metadata (vectorize, unroll hints).
///
/// ## Integration
///
///   OmPolyOptPass is registered at the VectorizerStartEP and
///   LoopOptimizerEndEP callbacks so it runs after LICM/IndVarSimplify
///   (which normalize loops into canonical form) but before the LLVM loop
///   vectorizer (so the vectorizer sees the tiled inner loops).
///
/// ## Safety
///
///   Transformations are only applied when:
///   - All loop bounds are affine functions of outer IVs and module globals.
///   - All array subscripts are affine functions of IVs.
///   - The loop body contains no calls, no indirect memory, no exceptions.
///   - The dependence test proves the transformation is legal.
///   - The profitability model estimates a net positive performance gain.

#include <cstdint>
#include <vector>
#include <optional>
#include <string>

// LLVM pass infrastructure (needed for OmPolyOptFunctionPass definition)
#include <llvm/IR/PassManager.h>

namespace llvm {
class Function;
class Module;
class Loop;
class ScalarEvolution;
class DominatorTree;
class LoopInfo;
} // namespace llvm

namespace omscript {

// Forward declarations for the service pointers added to PolyOptConfig.
class LegalityService;
class CostModel;

namespace polyopt {

// ─────────────────────────────────────────────────────────────────────────────
// Configuration
// ─────────────────────────────────────────────────────────────────────────────

/// Configuration parameters for the polyhedral optimizer.
struct PolyOptConfig {
    /// Enable loop tiling (cache blocking).  Default: true.
    bool enableTiling = true;

    /// Enable loop interchange (reorder loop levels for locality).  Default: true.
    bool enableInterchange = true;

    /// Enable loop skewing (shear transformation).  Default: true.
    bool enableSkewing = true;

    /// Enable loop fusion (merge adjacent loops).  Default: true.
    bool enableFusion = true;

    /// Enable loop fission / distribution (split loops).  Default: true.
    bool enableFission = true;

    /// Enable loop reversal.  Default: true.
    bool enableReversal = true;

    /// L1 data cache size in bytes (used for tile size selection).
    /// 0 = auto-detect via TargetTransformInfo.
    unsigned l1CacheBytes = 0;

    /// L2 data cache size in bytes (used for L2 tile size selection).
    /// 0 = auto-detect.
    unsigned l2CacheBytes = 0;

    /// Cache line size in bytes.  Default: 64 (x86-64).
    unsigned cacheLineBytes = 64;

    /// Maximum loop nest depth to consider for polyhedral analysis.
    /// Deeper nests have exponential analysis cost.  Default: 6.
    unsigned maxLoopDepth = 6;

    /// Maximum number of statements per SCoP.  Default: 32.
    unsigned maxScopStatements = 32;

    /// Maximum number of array dimensions per access.  Default: 4.
    unsigned maxArrayDims = 4;

    /// Minimum trip count (approximate) for a loop to be worth tiling.
    /// Loops with fewer iterations skip the tiling analysis.  Default: 16.
    unsigned minTripCountForTiling = 16;

    /// Emit verbose diagnostics about SCoP detection and transformations.
    bool verbose = false;

    // ── Shared service pointers (optional; do not own) ────────────────────

    /// High-level transform safety oracle from OptimizationManager.
    /// When non-null, optimizeFunction() calls
    /// legality->canTransformFunction(F) before any SCoP extraction.
    /// An Illegal verdict skips the function; Unknown proceeds normally.
    const LegalityService* legality = nullptr;

    /// Shared instruction cost oracle from OptimizationManager.
    /// When non-null, the profitability model uses this to estimate
    /// the savings from a transform in addition to the cache-miss model.
    const CostModel* costModel = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
// Statistics
// ─────────────────────────────────────────────────────────────────────────────

/// Statistics returned by the polyhedral optimizer.
struct PolyOptStats {
    /// Number of SCoPs detected.
    unsigned scopsDetected = 0;

    /// Number of SCoPs successfully transformed.
    unsigned scopsTransformed = 0;

    /// Number of loop nests tiled.
    unsigned loopsTiled = 0;

    /// Number of loop interchanges applied.
    unsigned loopsInterchanged = 0;

    /// Number of loop skewings applied.
    unsigned loopsSkewed = 0;

    /// Number of loop fusions applied.
    unsigned loopsFused = 0;

    /// Number of loop fissions applied.
    unsigned loopsFissioned = 0;

    /// Number of loop reversals applied.
    unsigned loopsReversed = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// Main entry points
// ─────────────────────────────────────────────────────────────────────────────

/// Run the polyhedral optimizer on a single LLVM function.
/// Requires that the function has been normalized (LoopSimplify + LCSSA).
/// Returns statistics about transformations applied.
PolyOptStats optimizeFunction(llvm::Function& F,
                               const PolyOptConfig& config = PolyOptConfig{});

/// Run the polyhedral optimizer on all functions in a module.
PolyOptStats optimizeModule(llvm::Module& M,
                             const PolyOptConfig& config = PolyOptConfig{});

// ─────────────────────────────────────────────────────────────────────────────
// LLVM New Pass Manager integration
// ─────────────────────────────────────────────────────────────────────────────

/// OmPolyOptFunctionPass: wraps the polyhedral optimizer as an LLVM new-PM
/// FunctionPass.  Registered at VectorizerStartEP (after LoopSimplify + LCSSA,
/// before the vectorizer) so the inner point-loops produced by tiling are
/// exposed to LLVM's auto-vectorizer.
struct OmPolyOptFunctionPass
    : public llvm::PassInfoMixin<OmPolyOptFunctionPass> {

    PolyOptConfig config;
    explicit OmPolyOptFunctionPass(const PolyOptConfig& cfg = PolyOptConfig{})
        : config(cfg) {}

    llvm::PreservedAnalyses run(llvm::Function& F,
                                llvm::FunctionAnalysisManager& FAM);
};

// ─────────────────────────────────────────────────────────────────────────────
// Legality query API
// ─────────────────────────────────────────────────────────────────────────────

/// Result of a loop-nest legality check for all supported transformations.
/// Produced by checkLoopLegality() for a specific outermost loop.
struct LoopLegalityResult {
    /// True when the outermost loop is part of a detectable affine SCoP.
    /// When false, all transform-specific fields are also false (SCoP
    /// extraction failed — no polyhedral analysis was performed).
    bool scopDetected = false;

    bool interchange = false; ///< Loop interchange is legal for this nest
    bool tiling      = false; ///< Loop tiling is legal for this nest
    bool reversal    = false; ///< Inner-loop reversal is legal
    bool skewing     = false; ///< Skewing with factor 1 is legal
};

/// Check the legality of all loop transformations for a given outermost loop.
///
/// Performs SCoP detection, dependence analysis (Fourier-Motzkin), and runs
/// the internal legality predicates for interchange, tiling, reversal, and
/// skewing.  Requires that the function containing @p outerLoop is in
/// LoopSimplify + LCSSA form.
///
/// Returns all-false if @p outerLoop is null or is not a detectable SCoP.
///
/// This function is exposed so that OptimizationManager::legality() can
/// pre-screen loops before committing to the full polyopt pass.
LoopLegalityResult checkLoopLegality(llvm::Loop* outerLoop,
                                      llvm::ScalarEvolution& SE,
                                      llvm::DominatorTree& DT,
                                      llvm::LoopInfo& LI,
                                      const PolyOptConfig& config = PolyOptConfig{});

} // namespace polyopt
} // namespace omscript

#endif // POLYOPT_H
