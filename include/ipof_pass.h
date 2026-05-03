#pragma once
#ifndef IPOF_PASS_H
#define IPOF_PASS_H

// ─────────────────────────────────────────────────────────────────────────────
// ipof_pass.h — Implicit Phase Ordering Fixer (IPOF)
// ─────────────────────────────────────────────────────────────────────────────
//
// IPOF is a post-pipeline IR pass that detects optimization opportunities
// that the standard fixed-order pipeline missed due to phase-ordering effects,
// then locally re-applies a minimal, reordered sequence of sub-passes to
// recover that value.
//
// Pipeline position: after SDR (post-HGOE), before final lowering.
//
// ── Why phase ordering matters ──────────────────────────────────────────────
//
// Classic compilers apply passes in a fixed global order.  A typical sequence:
//
//   inline → const-fold → DCE → vectorize → LICM → …
//
// The "correct" order depends on the code.  Consider:
//
//   fn foo(x: int) -> int { return x * 2; }
//   fn bar() -> int { return foo(3) + foo(3); }
//
// If inlining runs before CSE, we miss the common subexpression.
// If CSE runs before inlining, we don't see the two identical calls.
// The fixed order can miss either.
//
// IPOF runs *after* the standard pipeline and detects residual missed
// opportunities, then re-runs the minimal pass subset needed to unlock them.
//
// ── Algorithm ───────────────────────────────────────────────────────────────
//
//   Step 1 — Scan for missed opportunities (MissedOp)
//     Walk every function in the module.  For each instruction, check whether
//     a well-known pattern is present that *should* have been eliminated or
//     transformed but wasn't.  Classify into one of:
//       ConstantFolding   — arithmetic on IR constants that wasn't folded
//       CommonSubexpr     — identical subexpressions computed multiple times
//       DeadCode          — instructions with no live users
//       RedundantLoad     — load that re-reads a value already in a register
//       NearVectorizable  — loop body with no data-dep blocker but not vectorized
//       CallWithConst     — call site with all-constant args (inlinable + foldable)
//
//   Step 2 — Build a dependency-hint graph
//     For each MissedOp record what pre-condition(s) must hold before the
//     local re-run can succeed.  E.g.:
//       NearVectorizable → ConstantFolding of the trip-count must fire first
//       CallWithConst    → Inline must run before ConstantFolding
//     This produces an ordering constraint graph over the minimal pass set.
//
//   Step 3 — Locally re-run passes in hint-graph order
//     Topological-sort the dependency graph to get a per-region re-run sequence.
//     Only the passes relevant to the region run (never the whole pipeline).
//     Built-in sub-pass sequences:
//       FOLD_THEN_DCE         → InstSimplify + ADCE
//       INLINE_FOLD_DCE       → AlwaysInliner + InstSimplify + ADCE + SimplifyCFG
//       CSE_FOLD_DCE          → EarlyCSE + InstSimplify + ADCE
//       LOAD_ELIM             → MemCpyOpt + InstSimplify
//       FOLD_THEN_VECTORIZE   → InstSimplify + ADCE + SLPVectorizer
//
//   Step 4 — Cost gate and cycle detection
//     Before applying: snapshot IR instruction count (proxy for cost).
//     After applying:  compare.  Keep only if instruction count decreased
//     by at least minInstrReduction.
//     Cycle detection: hash each function's IR (FNV-1a over instruction
//     opcodes).  If a hash repeats → oscillation → abort this opportunity.
//     Iteration cap: at most maxIterations passes per opportunity.
//
// ── Aggression levels ───────────────────────────────────────────────────────
//
//   Level 0 — disabled
//   Level 1 — ConstantFolding + DeadCode + CommonSubexpr only (fast; default at O2)
//   Level 2 — all opportunity types, up to 2 iterations (default at O3)
//   Level 3 — all types, up to 3 iterations, NearVectorizable enabled (@optmax)

#include <llvm/IR/Module.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <cstdint>

namespace omscript::ipof {

// ─────────────────────────────────────────────────────────────────────────────
// MissedOpKind — category of residual missed optimization
// ─────────────────────────────────────────────────────────────────────────────
enum class MissedOpKind : uint8_t {
    ConstantFolding,  ///< Arithmetic on IR constants that can still be folded
    CommonSubexpr,    ///< Identical sub-expressions computed more than once
    DeadCode,         ///< Instruction whose result is never used
    RedundantLoad,    ///< Load that re-reads a value already live in a register
    NearVectorizable, ///< Loop lacking only constant trip-count for vectorization
    CallWithConst,    ///< Call site with all-constant args (inline → fold opportunity)
};

// ─────────────────────────────────────────────────────────────────────────────
// IpofConfig
// ─────────────────────────────────────────────────────────────────────────────
struct IpofConfig {
    /// Aggression level (0–3).  See header comment.  Default: 1.
    unsigned aggressionLevel = 1;

    /// Maximum number of re-run iterations per missed-opportunity region.
    /// Capped internally at 4 regardless of this setting.  Default: 2.
    unsigned maxIterations = 2;

    /// Minimum instruction-count decrease required to accept a local re-run.
    /// Guards against micro-changes that slow compile time without benefit.
    /// Default: 1.
    unsigned minInstrReduction = 1;

    /// Maximum function instruction count eligible for IPOF analysis.
    /// Larger functions are skipped to bound compile time.  Default: 4000.
    unsigned maxFunctionSize = 4000;

    /// Enable NearVectorizable detection (requires loop analysis; slower).
    /// Forced false when aggressionLevel < 3.  Default: false.
    bool enableNearVectorizable = false;

    /// Enable CallWithConst detection and inline-then-fold sequences.
    /// Default: true at aggressionLevel ≥ 2.
    bool enableCallWithConst = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// IpofStats — per-module statistics from a single runIPOF() call
// ─────────────────────────────────────────────────────────────────────────────
struct IpofStats {
    // Step 1 — detection
    unsigned opportunitiesFound   = 0; ///< Total missed-op patterns detected
    unsigned opportunitiesActed   = 0; ///< Patterns for which re-run was attempted

    // Step 3 — re-runs
    unsigned rerunsApplied        = 0; ///< Successful re-run sequences applied

    // Step 4 — cost gate
    unsigned acceptedImprovements = 0; ///< Re-runs accepted (IR improved)
    unsigned rejectedNoGain       = 0; ///< Re-runs rejected (no improvement)
    unsigned rejectedOscillation  = 0; ///< Stopped due to repeated IR hash

    // Per-kind accepted improvements
    unsigned foldedConstants      = 0; ///< ConstantFolding improvements
    unsigned eliminatedCSE        = 0; ///< CommonSubexpr improvements
    unsigned eliminatedDead       = 0; ///< DeadCode improvements
    unsigned eliminatedLoads      = 0; ///< RedundantLoad improvements
    unsigned inlinedAndFolded     = 0; ///< CallWithConst improvements

    /// Net instruction reduction across all accepted improvements.
    int64_t  netInstrReduction    = 0;

    unsigned totalAccepted() const noexcept { return acceptedImprovements; }
};

// ─────────────────────────────────────────────────────────────────────────────
// runIPOF — the main entry point
// ─────────────────────────────────────────────────────────────────────────────
//
// @p getTTI  Callback supplying TargetTransformInfo per function.
// @p config  Tuning config.  Default: aggressionLevel=1.
IpofStats runIPOF(llvm::Module& module, const IpofConfig& config = {});

} // namespace omscript::ipof

#endif // IPOF_PASS_H
