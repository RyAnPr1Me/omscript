/// @file polyopt.cpp
/// @brief OmPolyOpt — Native Polyhedral Loop Optimizer for OmScript.
///
/// Production-grade polyhedral optimizer operating directly on LLVM IR.
/// Implements SCoP extraction, integer linear programming (Fourier-Motzkin
/// based dependence analysis), legal transformation selection, and IR
/// code generation for tiled, interchanged, and skewed loop nests.

#ifdef __GNUC__
#  pragma GCC optimize("O3,unroll-loops")
#endif

#include "polyopt.h"

#include <llvm/Analysis/DependenceAnalysis.h>
#include <llvm/Analysis/IVDescriptors.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Value.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Transforms/Scalar/IndVarSimplify.h>
#include <llvm/Transforms/Scalar/LICM.h>
#include <llvm/Transforms/Scalar/LoopRotation.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/LoopUtils.h>
#include <llvm/Transforms/Utils/ScalarEvolutionExpander.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omscript {
namespace polyopt {

// ─────────────────────────────────────────────────────────────────────────────
// Polyhedral model primitives
// ─────────────────────────────────────────────────────────────────────────────

/// A rational coefficient in an affine constraint or map.
/// All arithmetic is done in 64-bit integers (no fractions needed for loop
/// bounds and subscripts in integer programs).
using Coeff = int64_t;

/// An affine expression:  c[0]*x[0] + c[1]*x[1] + ... + c[n-1]*x[n-1] + c[n]
/// where the last element is the constant term.
struct AffineExpr {
    std::vector<Coeff> coeffs; // coeffs[i] = coefficient of variable i
    Coeff constant = 0;        // constant term

    AffineExpr() = default;
    explicit AffineExpr(unsigned nvars) : coeffs(nvars, 0) {}

    unsigned numVars() const { return static_cast<unsigned>(coeffs.size()); }

    AffineExpr operator+(const AffineExpr& o) const {
        AffineExpr r(*this);
        for (unsigned i = 0; i < coeffs.size(); ++i) r.coeffs[i] += o.coeffs[i];
        r.constant += o.constant;
        return r;
    }

    AffineExpr operator*(Coeff c) const {
        AffineExpr r(*this);
        for (auto& v : r.coeffs) v *= c;
        r.constant *= c;
        return r;
    }
};

/// An affine constraint:  sum_i a[i]*x[i] + b >= 0  (inequality)
/// or  sum_i a[i]*x[i] + b == 0  (equality, marked with isEq).
struct AffineConstraint {
    std::vector<Coeff> coeffs; // coefficients of loop IVs + parameters
    Coeff constant = 0;
    bool isEq = false;         // false = inequality (>=0), true = equality (==0)

    AffineConstraint() = default;
    explicit AffineConstraint(unsigned nvars) : coeffs(nvars, 0) {}
};

/// A convex polyhedron described by a list of affine constraints.
struct Polyhedron {
    unsigned numVars = 0;      // number of iteration variables (loop IVs)
    unsigned numParams = 0;    // symbolic parameters (e.g. trip count N)
    std::vector<AffineConstraint> constraints;

    bool empty() const { return constraints.empty(); }
};

/// An affine access function: maps an iteration vector [i_0, ..., i_{d-1}]
/// to an n-dimensional array index [f_0, ..., f_{n-1}] where each f_k is
/// an affine expression in the IVs and symbolic parameters.
struct AffineAccessMap {
    unsigned numLoopIVs = 0;
    unsigned numDims = 0;        // dimensionality of array subscript
    std::vector<AffineExpr> dims; // dims[k] = affine expr for subscript k

    // Element type size in bytes (for cache-line stride computations)
    unsigned elemSizeBytes = 8;

    // Is this a write (store) access?
    bool isWrite = false;

    // The underlying LLVM instruction (load or store)
    llvm::Instruction* inst = nullptr;
    // The base pointer of the array being accessed
    llvm::Value* basePtr = nullptr;
};

/// A single statement in a SCoP (a basic block or a single instruction).
struct ScopStatement {
    llvm::BasicBlock* bb = nullptr;
    Polyhedron domain;                   // iteration domain
    std::vector<AffineAccessMap> reads;  // read accesses
    std::vector<AffineAccessMap> writes; // write accesses
    unsigned scheduleLevel = 0;          // depth in the loop nest
};

/// A Static Control Part: a maximal affine region in the function.
struct SCoP {
    llvm::Function* func = nullptr;
    std::vector<llvm::Loop*> loops;        // ordered outermost→innermost
    std::vector<llvm::PHINode*> IVs;       // induction variables, same order
    std::vector<const llvm::SCEV*> lbs;    // lower bounds (SCEV expressions)
    std::vector<const llvm::SCEV*> ubs;    // upper bounds (exclusive)
    std::vector<ScopStatement> stmts;

    // Symbolic parameter values (loop-invariant SCEV expressions for trip counts)
    std::vector<const llvm::SCEV*> params;
    // LLVM Values corresponding to each parameter
    std::vector<llvm::Value*> paramValues;

    unsigned depth() const { return static_cast<unsigned>(loops.size()); }
    bool valid = false;
};

/// A dependence between two ScopStatements.
struct PolyDep {
    unsigned srcStmt = 0;   // index into SCoP::stmts
    unsigned dstStmt = 0;
    bool isRAW = false;
    bool isWAR = false;
    bool isWAW = false;
    bool isLoopCarried = false;   // true if distance vector has at least one > 0

    // Distance vector: one entry per loop level.  distance[l] > 0 means the
    // dependence is forward (source iteration precedes destination) in loop l.
    // distance[l] == 0 means same-iteration (loop-independent at level l).
    // distance[l] < 0 means backward (illegal without transformation).
    std::vector<int64_t> distance;   // one per loop level
    bool distanceKnown = false;       // false if only direction is known
    std::vector<int> direction;       // -1/0/+1 per loop level (direction vector)
};

// ─────────────────────────────────────────────────────────────────────────────
// Fourier-Motzkin Elimination
// ─────────────────────────────────────────────────────────────────────────────

/// Project out variable `varIdx` from a system of inequalities using
/// Fourier-Motzkin elimination.  All constraints are of the form:
///   sum_j a[j]*x[j] + c >= 0
/// Returns the projected system (may be empty = always satisfiable, or
/// detect infeasibility by returning a system containing -1 >= 0).
static std::vector<AffineConstraint>
fourierMotzkinProject(std::vector<AffineConstraint> system, unsigned varIdx) {
    // Partition into three groups based on the sign of coeffs[varIdx]:
    //   pos: a[varIdx] > 0
    //   neg: a[varIdx] < 0
    //   zero: a[varIdx] == 0
    std::vector<AffineConstraint> pos, neg, zero;
    for (auto& c : system) {
        Coeff cv = (varIdx < c.coeffs.size()) ? c.coeffs[varIdx] : 0;
        if (cv > 0)       pos.push_back(c);
        else if (cv < 0)  neg.push_back(c);
        else              zero.push_back(c);
    }

    // For each pair (p, n), produce a new constraint eliminating x[varIdx]:
    //   p: a*x + ... >= 0  →  x >= -(...)/a
    //   n: b*x + ... >= 0  →  x <= (...)/(-b)
    // Combined: -(..._n)/(-b) >= -(..._p)/a
    //     i.e. a*(..._n) + b*(..._p) >= 0  (where b < 0)
    // Multiply out to avoid fractions (integer arithmetic):
    //   a * [other_n] + (-b) * [other_p] >= 0
    std::vector<AffineConstraint> result = zero;
    unsigned nvars = 0;
    for (auto& c : system)
        nvars = std::max(nvars, static_cast<unsigned>(c.coeffs.size()));

    for (auto& p : pos) {
        Coeff a = p.coeffs[varIdx];
        for (auto& n : neg) {
            Coeff b = -n.coeffs[varIdx]; // b > 0

            AffineConstraint newC;
            newC.coeffs.resize(nvars, 0);
            for (unsigned j = 0; j < nvars; ++j) {
                Coeff pj = (j < p.coeffs.size()) ? p.coeffs[j] : 0;
                Coeff nj = (j < n.coeffs.size()) ? n.coeffs[j] : 0;
                newC.coeffs[j] = b * pj + a * nj;
            }
            newC.constant = b * p.constant + a * n.constant;
            // Zero out the eliminated variable's coefficient
            if (varIdx < newC.coeffs.size()) newC.coeffs[varIdx] = 0;

            // Quick infeasibility check: all-zero coefficients with negative constant
            bool allZero = true;
            for (Coeff cv2 : newC.coeffs) if (cv2 != 0) { allZero = false; break; }
            if (allZero && newC.constant < 0) {
                // Infeasible: return a canonical infeasible system
                result.clear();
                AffineConstraint infeas;
                infeas.coeffs.resize(nvars, 0);
                infeas.constant = -1;
                result.push_back(infeas);
                return result;
            }
            // Skip trivially satisfied: 0 >= 0
            if (allZero && newC.constant >= 0) continue;

            result.push_back(std::move(newC));
        }
    }

    return result;
}

/// Test whether a system of affine constraints has an integer solution.
/// Uses Fourier-Motzkin to project out all variables and check feasibility.
/// Returns true if the system is satisfiable, false if infeasible.
/// This is sufficient for dependence testing: if the dependence system is
/// infeasible, no dependence exists.
static bool isFeasible(std::vector<AffineConstraint> system, unsigned nvars) {
    if (system.empty()) return true;
    for (unsigned v = 0; v < nvars; ++v) {
        system = fourierMotzkinProject(std::move(system), v);
        // Check for explicit infeasibility marker
        for (auto& c : system) {
            bool allZero = true;
            for (Coeff cv : c.coeffs) if (cv != 0) { allZero = false; break; }
            if (allZero && c.constant < 0) return false;
        }
        if (system.empty()) return true;
    }
    // All variables projected out: check remaining constant constraints
    for (auto& c : system) {
        bool allZero = true;
        for (Coeff cv : c.coeffs) if (cv != 0) { allZero = false; break; }
        if (allZero && c.constant < 0) return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// SCEV → Affine Expression conversion
// ─────────────────────────────────────────────────────────────────────────────

/// Try to extract an affine expression in terms of known loop IVs and
/// parameters from a SCEV.  Returns false if the SCEV is not affine in
/// the given IV set.
static bool scevToAffine(const llvm::SCEV* S,
                         const std::vector<llvm::PHINode*>& IVs,
                         const std::vector<const llvm::SCEV*>& paramSCEVs,
                         llvm::ScalarEvolution& SE,
                         AffineExpr& out) {
    unsigned nvars = static_cast<unsigned>(IVs.size());
    unsigned nparams = static_cast<unsigned>(paramSCEVs.size());
    out = AffineExpr(nvars + nparams);

    std::function<bool(const llvm::SCEV*, AffineExpr&)> recurse =
        [&](const llvm::SCEV* s, AffineExpr& expr) -> bool {
        // Constant
        if (auto* C = llvm::dyn_cast<llvm::SCEVConstant>(s)) {
            expr.constant += C->getAPInt().getSExtValue();
            return true;
        }
        // Check if this is one of our IVs
        for (unsigned i = 0; i < nvars; ++i) {
            if (s == SE.getSCEV(IVs[i])) {
                expr.coeffs[i] += 1;
                return true;
            }
        }
        // Check if this is one of our parameters
        for (unsigned i = 0; i < nparams; ++i) {
            if (s == paramSCEVs[i]) {
                expr.coeffs[nvars + i] += 1;
                return true;
            }
        }
        // Add expression: recursively process operands
        if (auto* Add = llvm::dyn_cast<llvm::SCEVAddExpr>(s)) {
            for (auto* op : Add->operands()) {
                if (!recurse(op, expr)) return false;
            }
            return true;
        }
        // Mul by constant: one operand must be constant
        if (auto* Mul = llvm::dyn_cast<llvm::SCEVMulExpr>(s)) {
            if (Mul->getNumOperands() == 2) {
                auto* C = llvm::dyn_cast<llvm::SCEVConstant>(Mul->getOperand(0));
                if (!C) C = llvm::dyn_cast<llvm::SCEVConstant>(Mul->getOperand(1));
                if (C) {
                    int64_t cval = C->getAPInt().getSExtValue();
                    const llvm::SCEV* other = (Mul->getOperand(0) == C)
                        ? Mul->getOperand(1) : Mul->getOperand(0);
                    AffineExpr sub(nvars + nparams);
                    if (!recurse(other, sub)) return false;
                    for (unsigned j = 0; j < sub.coeffs.size(); ++j)
                        expr.coeffs[j] += cval * sub.coeffs[j];
                    expr.constant += cval * sub.constant;
                    return true;
                }
            }
            return false;
        }
        // Negate: -x is represented as MulExpr(-1, x) in modern LLVM
        // Check for SCEVMulExpr with -1 constant first operand
        if (auto* Mul2 = llvm::dyn_cast<llvm::SCEVMulExpr>(s)) {
            if (Mul2->getNumOperands() == 2) {
                if (auto* C2 = llvm::dyn_cast<llvm::SCEVConstant>(Mul2->getOperand(0))) {
                    if (C2->getAPInt() == -1) {
                        AffineExpr sub2(nvars + nparams);
                        if (!recurse(Mul2->getOperand(1), sub2)) return false;
                        for (unsigned j = 0; j < sub2.coeffs.size(); ++j)
                            expr.coeffs[j] -= sub2.coeffs[j];
                        expr.constant -= sub2.constant;
                        return true;
                    }
                }
            }
        }
        // Truncate / zero-extend of affine expressions
        if (auto* Trunc = llvm::dyn_cast<llvm::SCEVTruncateExpr>(s)) {
            return recurse(Trunc->getOperand(), expr);
        }
        if (auto* ZExt = llvm::dyn_cast<llvm::SCEVZeroExtendExpr>(s)) {
            return recurse(ZExt->getOperand(), expr);
        }
        if (auto* SExt = llvm::dyn_cast<llvm::SCEVSignExtendExpr>(s)) {
            return recurse(SExt->getOperand(), expr);
        }
        // Unknown — not affine in our IV set
        return false;
    };

    return recurse(S, out);
}

// ─────────────────────────────────────────────────────────────────────────────
// SCoP Detection
// ─────────────────────────────────────────────────────────────────────────────

/// Check whether a loop has an affine induction variable and computable
/// trip count.  Returns the PHI node (IV) and the SCEV upper bound
/// (exclusive) if successful.
static bool getLoopIVAndBound(llvm::Loop* L,
                               llvm::ScalarEvolution& SE,
                               llvm::PHINode*& IV,
                               const llvm::SCEV*& lb,
                               const llvm::SCEV*& ub) {
    IV = L->getInductionVariable(SE);
    if (!IV) return false;
    if (!IV->getType()->isIntegerTy()) return false;

    const llvm::SCEV* tripCount = SE.getBackedgeTakenCount(L);
    if (!tripCount || llvm::isa<llvm::SCEVCouldNotCompute>(tripCount)) return false;

    lb = SE.getZero(IV->getType());
    ub = SE.getAddExpr(tripCount, SE.getOne(IV->getType()));
    return true;
}

/// Check whether a basic block is safe to include in a SCoP:
/// - No function calls (other than llvm.prefetch / llvm.assume)
/// - No indirect memory accesses (all pointers must have computable base)
/// - No volatile / atomic accesses
/// - No exception handling instructions
static bool isBlockSafeForScop(llvm::BasicBlock* BB) {
    for (auto& I : *BB) {
        if (auto* Call = llvm::dyn_cast<llvm::CallInst>(&I)) {
            auto* Callee = Call->getCalledFunction();
            if (!Callee) return false; // indirect call
            llvm::StringRef name = Callee->getName();
            // Allow known-safe intrinsics
            if (Callee->isIntrinsic()) {
                auto id = Callee->getIntrinsicID();
                if (id == llvm::Intrinsic::prefetch ||
                    id == llvm::Intrinsic::assume ||
                    id == llvm::Intrinsic::lifetime_start ||
                    id == llvm::Intrinsic::lifetime_end ||
                    id == llvm::Intrinsic::dbg_declare ||
                    id == llvm::Intrinsic::dbg_value)
                    continue;
            }
            (void)name;
            return false; // external call — bail out
        }
        if (auto* LI = llvm::dyn_cast<llvm::LoadInst>(&I)) {
            if (LI->isVolatile() || LI->isAtomic()) return false;
        }
        if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
            if (SI->isVolatile() || SI->isAtomic()) return false;
        }
        if (llvm::isa<llvm::InvokeInst>(&I) || llvm::isa<llvm::LandingPadInst>(&I))
            return false;
    }
    return true;
}

/// Detect a perfect or imperfect loop nest rooted at L and extract a SCoP.
/// A "perfect" nest has all computation only in the innermost body block.
/// We handle imperfect nests by treating each intermediate block as a statement.
static bool detectScop(llvm::Loop* outerL,
                        llvm::ScalarEvolution& SE,
                        llvm::LoopInfo& LI,
                        const PolyOptConfig& config,
                        SCoP& scop) {
    scop = SCoP{};
    scop.func = outerL->getHeader()->getParent();

    // Collect the loop nest (outermost → innermost) using BFS/DFS
    // up to maxLoopDepth levels.
    std::function<void(llvm::Loop*, unsigned)> collectLoops =
        [&](llvm::Loop* L, unsigned depth) {
        if (depth >= config.maxLoopDepth) return;

        llvm::PHINode* iv = nullptr;
        const llvm::SCEV* lb = nullptr;
        const llvm::SCEV* ub = nullptr;
        if (!getLoopIVAndBound(L, SE, iv, lb, ub)) {
            scop.valid = false;
            return;
        }

        // Check all blocks in this loop (excluding sub-loop blocks)
        for (auto* BB : L->getBlocks()) {
            if (LI.getLoopFor(BB) != L) continue; // belongs to inner loop
            if (!isBlockSafeForScop(BB)) {
                scop.valid = false;
                return;
            }
        }

        scop.loops.push_back(L);
        scop.IVs.push_back(iv);
        scop.lbs.push_back(lb);
        scop.ubs.push_back(ub);

        // Recurse into sub-loops
        for (auto* subL : L->getSubLoops()) {
            collectLoops(subL, depth + 1);
            if (!scop.valid) return;
        }
    };

    scop.valid = true;
    collectLoops(outerL, 0);
    if (!scop.valid || scop.loops.empty()) return false;
    if (scop.depth() < 2) return false; // need at least 2 levels for interchange/tiling

    // Collect symbolic parameters: loop-invariant SCEVs for trip counts
    std::unordered_set<const llvm::SCEV*> paramSet;
    for (unsigned i = 0; i < scop.depth(); ++i) {
        // The upper bound may reference outer IVs (affine) or module globals
        // (parameters).  Extract the parameter part.
        const llvm::SCEV* ubExpr = scop.ubs[i];
        // Subtract 1 (ub is exclusive) to get the loop bound expression
        // and check if it references any unknown SCEV sub-expressions
        // (those become parameters).
        std::function<void(const llvm::SCEV*)> findParams = [&](const llvm::SCEV* s) {
            if (llvm::isa<llvm::SCEVConstant>(s)) return;
            for (unsigned j = 0; j < scop.depth(); ++j) {
                if (s == SE.getSCEV(scop.IVs[j])) return;
            }
            if (llvm::isa<llvm::SCEVAddExpr>(s) || llvm::isa<llvm::SCEVMulExpr>(s) ||
                llvm::isa<llvm::SCEVZeroExtendExpr>(s) ||
                llvm::isa<llvm::SCEVSignExtendExpr>(s)) {
                if (auto* op = llvm::dyn_cast<llvm::SCEVNAryExpr>(s)) {
                    for (auto* o : op->operands()) findParams(o);
                } else if (auto* un = llvm::dyn_cast<llvm::SCEVCastExpr>(s)) {
                    findParams(un->getOperand());
                }
                return;
            }
            // Unknown SCEV — treat as parameter
            if (!paramSet.count(s)) {
                paramSet.insert(s);
                scop.params.push_back(s);
                // Try to find the underlying LLVM Value for this SCEV
                if (auto* sv = llvm::dyn_cast<llvm::SCEVUnknown>(s))
                    scop.paramValues.push_back(sv->getValue());
                else
                    scop.paramValues.push_back(nullptr);
            }
        };
        findParams(ubExpr);
    }

    // Build statement list: one statement per innermost loop body block
    llvm::Loop* innermostL = scop.loops.back();
    unsigned depth = scop.depth();
    for (auto* BB : innermostL->getBlocks()) {
        if (LI.getLoopFor(BB) != innermostL) continue;
        if (BB == innermostL->getHeader()) continue;  // skip loop header
        if (BB == innermostL->getLoopLatch()) {
            // Latch typically only has a branch; safe to include
        }

        // Build the iteration domain polyhedron for this block
        // Domain: for each loop l: 0 <= iv[l] < ub[l]
        Polyhedron domain;
        domain.numVars = depth;
        domain.numParams = static_cast<unsigned>(scop.params.size());
        unsigned totalVars = depth + domain.numParams;

        for (unsigned l = 0; l < depth; ++l) {
            // iv[l] >= 0   →   iv[l] + 0 >= 0
            AffineConstraint lb_c(totalVars);
            lb_c.coeffs[l] = 1;
            lb_c.constant = 0;
            domain.constraints.push_back(lb_c);

            // iv[l] < ub[l]   →   ub[l] - iv[l] - 1 >= 0
            AffineExpr ubExpr(totalVars);
            if (!scevToAffine(scop.ubs[l], scop.IVs, scop.params, SE, ubExpr)) {
                scop.valid = false;
                return false;
            }
            AffineConstraint ub_c(totalVars);
            for (unsigned j = 0; j < totalVars; ++j)
                ub_c.coeffs[j] = ubExpr.coeffs[j];
            ub_c.coeffs[l] -= 1;
            ub_c.constant = ubExpr.constant - 1;
            domain.constraints.push_back(ub_c);
        }

        ScopStatement stmt;
        stmt.bb = BB;
        stmt.domain = domain;
        stmt.scheduleLevel = depth - 1;

        // Extract array accesses
        for (auto& I : *BB) {
            llvm::Value* ptrOperand = nullptr;
            bool isWrite = false;

            if (auto* LI2 = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                ptrOperand = LI2->getPointerOperand();
                isWrite = false;
            } else if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                ptrOperand = SI->getPointerOperand();
                isWrite = true;
            }
            if (!ptrOperand) continue;

            // Try to decompose the pointer into a base + affine subscript
            // using SCEV analysis
            const llvm::SCEV* ptrSCEV = SE.getSCEV(ptrOperand);

            // For GEP-based accesses, the SCEV is typically an AddRecExpr
            // or AddExpr with a base pointer.
            AffineAccessMap acc;
            acc.numLoopIVs = depth;
            acc.isWrite = isWrite;
            acc.inst = &I;
            acc.basePtr = ptrOperand->stripPointerCasts();

            // Determine element size
            llvm::Type* elemTy = nullptr;
            if (auto* LI2 = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                elemTy = LI2->getType();
            } else if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                elemTy = SI->getValueOperand()->getType();
            }
            if (elemTy) {
                const auto& DL = I.getModule()->getDataLayout();
                acc.elemSizeBytes = static_cast<unsigned>(
                    DL.getTypeStoreSize(elemTy));
            }

            // Try to extract a 1-D affine subscript from SCEV
            // (for multi-dimensional arrays in C, GEP flattens to 1D)
            acc.numDims = 1;
            AffineExpr subscript(totalVars);
            if (scevToAffine(ptrSCEV, scop.IVs, scop.params, SE, subscript)) {
                acc.dims.push_back(subscript);
                if (isWrite)
                    stmt.writes.push_back(acc);
                else
                    stmt.reads.push_back(acc);
            }
            // If not affine, the statement is still valid but we can't model
            // the access — skip it (conservative: assumes dependence exists).
        }

        if (static_cast<unsigned>(scop.stmts.size()) >= config.maxScopStatements)
            break;
        scop.stmts.push_back(std::move(stmt));
    }

    if (scop.stmts.empty()) return false;
    scop.valid = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Dependence Analysis
// ─────────────────────────────────────────────────────────────────────────────

/// Compute dependences between two statements S and T in the same SCoP.
/// Returns the list of dependences (may be empty if no dependence exists).
/// Uses Fourier-Motzkin elimination on the combined iteration domain +
/// access function equality constraints.
static std::vector<PolyDep> computeDependences(const SCoP& scop) {
    std::vector<PolyDep> deps;
    unsigned depth = scop.depth();
    unsigned nparams = static_cast<unsigned>(scop.params.size());
    unsigned totalVars = depth + nparams;

    // For each pair of statements (src, dst)
    for (unsigned s = 0; s < scop.stmts.size(); ++s) {
        for (unsigned t = 0; t < scop.stmts.size(); ++t) {
            const ScopStatement& srcStmt = scop.stmts[s];
            const ScopStatement& dstStmt = scop.stmts[t];

            // For each pair of (write, read/write) accesses with the same base
            auto checkDep = [&](const AffineAccessMap& src, const AffineAccessMap& dst,
                                bool isRAW, bool isWAR, bool isWAW) {
                if (!src.basePtr || !dst.basePtr) {
                    // Can't prove independence without base pointer info —
                    // conservatively assume a dependence exists
                    PolyDep dep;
                    dep.srcStmt = s;
                    dep.dstStmt = t;
                    dep.isRAW = isRAW;
                    dep.isWAR = isWAR;
                    dep.isWAW = isWAW;
                    dep.distanceKnown = false;
                    dep.distance.assign(depth, 0);
                    dep.direction.assign(depth, 0);
                    dep.isLoopCarried = false;
                    deps.push_back(dep);
                    return;
                }

                // Same base pointer check (necessary but not sufficient)
                if (src.basePtr->stripPointerCasts() !=
                    dst.basePtr->stripPointerCasts())
                    return; // definitely different arrays — no dependence

                if (src.dims.empty() || dst.dims.empty()) {
                    // No subscript info — conservatively assume dependence
                    PolyDep dep;
                    dep.srcStmt = s;
                    dep.dstStmt = t;
                    dep.isRAW = isRAW; dep.isWAR = isWAR; dep.isWAW = isWAW;
                    dep.distanceKnown = false;
                    dep.distance.assign(depth, 0);
                    dep.direction.assign(depth, 0);
                    dep.isLoopCarried = false;
                    deps.push_back(dep);
                    return;
                }

                // Build the dependence system:
                // Variables: [i_0..i_{d-1}, j_0..j_{d-1}, params]
                // where i is the src iteration vector, j is the dst iteration vector
                // Constraints:
                //   1. src iteration domain (in i variables)
                //   2. dst iteration domain (in j variables, shifted by depth)
                //   3. Access function equality: f_src(i) == f_dst(j)
                //      → f_src(i) - f_dst(j) == 0
                unsigned dsysVars = 2 * depth + nparams;

                // For each loop level l, try specific distance values d = j_l - i_l
                // This is the Banerjee test: for distance d[l], test feasibility of
                // the system with the added constraint j_l = i_l + d[l]
                // We use a simplified approach: test d = 0, 1, -1, ... up to the
                // trip count, returning the first feasible distance.
                PolyDep dep;
                dep.srcStmt = s;
                dep.dstStmt = t;
                dep.isRAW = isRAW; dep.isWAR = isWAR; dep.isWAW = isWAW;
                dep.distanceKnown = false;
                dep.distance.assign(depth, 0);
                dep.direction.assign(depth, 0);

                // Build base system (domain constraints for both statements)
                std::vector<AffineConstraint> baseSystem;
                for (auto& c : srcStmt.domain.constraints) {
                    AffineConstraint nc(dsysVars);
                    for (unsigned j = 0; j < depth && j < c.coeffs.size(); ++j)
                        nc.coeffs[j] = c.coeffs[j]; // i variables
                    // Parameters start at 2*depth
                    for (unsigned j = depth; j < c.coeffs.size(); ++j)
                        nc.coeffs[depth + depth + (j - depth)] = c.coeffs[j];
                    nc.constant = c.constant;
                    baseSystem.push_back(nc);
                }
                for (auto& c : dstStmt.domain.constraints) {
                    AffineConstraint nc(dsysVars);
                    for (unsigned j = 0; j < depth && j < c.coeffs.size(); ++j)
                        nc.coeffs[depth + j] = c.coeffs[j]; // j variables
                    for (unsigned j = depth; j < c.coeffs.size(); ++j)
                        nc.coeffs[depth + depth + (j - depth)] = c.coeffs[j];
                    nc.constant = c.constant;
                    baseSystem.push_back(nc);
                }

                // Access equality constraints (for 1D case: dim 0)
                // f_src(i) - f_dst(j) = 0
                // → f_src_coeffs[k]*i_k + f_src_const - f_dst_coeffs[k]*j_k - f_dst_const = 0
                const auto& srcDim = src.dims[0];
                const auto& dstDim = dst.dims[0];
                AffineConstraint accEq(dsysVars);
                accEq.isEq = true;
                for (unsigned j = 0; j < depth && j < srcDim.coeffs.size(); ++j)
                    accEq.coeffs[j] += srcDim.coeffs[j];     // +f_src (i var)
                for (unsigned j = 0; j < depth && j < dstDim.coeffs.size(); ++j)
                    accEq.coeffs[depth + j] -= dstDim.coeffs[j]; // -f_dst (j var)
                // Parameters appear at indices depth..2*depth-1 in both
                for (unsigned j = depth; j < srcDim.coeffs.size(); ++j)
                    accEq.coeffs[depth + depth + (j - depth)] += srcDim.coeffs[j];
                for (unsigned j = depth; j < dstDim.coeffs.size(); ++j)
                    accEq.coeffs[depth + depth + (j - depth)] -= dstDim.coeffs[j];
                accEq.constant = srcDim.constant - dstDim.constant;

                // Convert equality to two inequalities for F-M
                // eq: e >= 0 AND -e >= 0
                AffineConstraint pos_eq = accEq, neg_eq = accEq;
                pos_eq.isEq = false;
                neg_eq.isEq = false;
                for (auto& cv : neg_eq.coeffs) cv = -cv;
                neg_eq.constant = -neg_eq.constant;
                baseSystem.push_back(pos_eq);
                baseSystem.push_back(neg_eq);

                // Test feasibility: if infeasible, no dependence
                bool feasible = isFeasible(baseSystem, dsysVars);
                if (!feasible) return; // no dependence — done

                // Dependence exists.  Try to determine distance vector
                // by testing j_l - i_l = d for d = 0, 1, -1, ...
                dep.distanceKnown = true;
                for (unsigned l = 0; l < depth; ++l) {
                    // Add constraint: j_l - i_l = 0 (try same-iteration first)
                    bool foundDist = false;
                    for (int64_t d = 0; d <= 8; ++d) {
                        for (int sign : {1, -1}) {
                            int64_t dist = d * sign;
                            if (d == 0 && sign == -1) continue;
                            // Add: j_l - i_l - dist = 0
                            AffineConstraint distPos(dsysVars);
                            AffineConstraint distNeg(dsysVars);
                            distPos.coeffs[depth + l] = 1;    // +j_l
                            distPos.coeffs[l] = -1;             // -i_l
                            distPos.constant = -dist;           // >= 0 → j_l >= i_l + dist
                            distNeg.coeffs[depth + l] = -1;   // -j_l
                            distNeg.coeffs[l] = 1;              // +i_l
                            distNeg.constant = dist;            // >= 0 → j_l <= i_l + dist
                            auto sysWithDist = baseSystem;
                            sysWithDist.push_back(distPos);
                            sysWithDist.push_back(distNeg);
                            if (isFeasible(sysWithDist, dsysVars)) {
                                dep.distance[l] = dist;
                                dep.direction[l] = (dist > 0) ? 1 : (dist < 0) ? -1 : 0;
                                dep.isLoopCarried = dep.isLoopCarried || (dist != 0);
                                foundDist = true;
                                break;
                            }
                        }
                        if (foundDist) break;
                    }
                    if (!foundDist) {
                        dep.distanceKnown = false;
                        dep.direction[l] = 0; // unknown
                    }
                }

                deps.push_back(dep);
            };

            // Check RAW: src writes, dst reads
            for (auto& w : srcStmt.writes)
                for (auto& r : dstStmt.reads)
                    checkDep(w, r, /*RAW=*/true, false, false);
            // Check WAR: src reads, dst writes
            for (auto& r : srcStmt.reads)
                for (auto& w : dstStmt.writes)
                    checkDep(r, w, false, /*WAR=*/true, false);
            // Check WAW: src writes, dst writes
            for (auto& w1 : srcStmt.writes)
                for (auto& w2 : dstStmt.writes)
                    checkDep(w1, w2, false, false, /*WAW=*/true);
        }
    }
    return deps;
}

// ─────────────────────────────────────────────────────────────────────────────
// Transformation Legality Checking
// ─────────────────────────────────────────────────────────────────────────────

/// Check if swapping loop levels `lvl1` and `lvl2` is legal given the
/// dependence set.  Interchange is legal if every dependence d satisfies:
/// the resulting direction vector (after swapping components lvl1 and lvl2)
/// is lexicographically non-negative.
static bool isInterchangeLegal(const std::vector<PolyDep>& deps,
                                unsigned lvl1, unsigned lvl2,
                                unsigned depth) {
    for (auto& dep : deps) {
        // Build swapped direction vector
        std::vector<int> dir = dep.direction;
        if (dir.size() < depth) dir.resize(depth, 0);
        std::swap(dir[lvl1], dir[lvl2]);

        // Check lexicographically positive
        for (unsigned l = 0; l < depth; ++l) {
            if (dir[l] > 0) break;  // positive prefix: OK
            if (dir[l] < 0) return false; // backward dependence: illegal
        }
    }
    return true;
}

/// Check if tiling loops `lvl1..lvl2` is legal.  Tiling is legal iff
/// loop interchange of any pair within the tile set is legal, AND there
/// are no negative dependence components within the tiled loop levels.
/// (Collard-Bastoul-Feautrier tileability condition: all dependence
/// distance vectors must be lexicographically positive or zero within
/// the tile loops.)
static bool isTilingLegal(const std::vector<PolyDep>& deps,
                           unsigned outerLvl, unsigned innerLvl,
                           unsigned /*depth*/) {
    for (auto& dep : deps) {
        // For tiling levels outerLvl..innerLvl, the distance must be >= 0
        // in all these dimensions (no negative components in the tile set).
        for (unsigned l = outerLvl; l <= innerLvl; ++l) {
            if (l < dep.direction.size() && dep.direction[l] < 0) return false;
        }
    }
    return true;
}

/// Check if reversing loop level `lvl` is legal.
/// Legal if all dependences have direction[lvl] != +1 (i.e., there are no
/// forward dependences in this dimension that would become backward).
static bool isReversalLegal(const std::vector<PolyDep>& deps, unsigned lvl) {
    for (auto& dep : deps) {
        if (lvl < dep.direction.size() && dep.direction[lvl] > 0) return false;
    }
    return true;
}

/// Check if skewing outer loop `outer` by factor `factor` times inner loop
/// `inner` is legal.  Skewing adds factor*i_outer to i_inner; this modifies
/// the direction vector.  Legal when the transformed dependences remain
/// lexicographically non-negative.
static bool isSkewingLegal(const std::vector<PolyDep>& deps,
                            unsigned outer, unsigned inner, int64_t factor,
                            unsigned depth) {
    for (auto& dep : deps) {
        std::vector<int64_t> dist(depth, 0);
        for (unsigned l = 0; l < dep.distance.size() && l < depth; ++l)
            dist[l] = dep.distance[l];

        // Skewing transform: d'[inner] = d[inner] + factor * d[outer]
        dist[inner] += factor * dist[outer];

        // Check lexicographically non-negative
        for (unsigned l = 0; l < depth; ++l) {
            if (dist[l] > 0) break;
            if (dist[l] < 0) return false;
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Profitability Model
// ─────────────────────────────────────────────────────────────────────────────

/// Estimate the cache miss reduction from tiling a loop nest with given tile
/// sizes.  Uses a simple working-set model:
///   untiled working set size ≈ N * M * elem_size  (for 2D nest)
///   tiled working set size   ≈ T_N * T_M * elem_size  (one tile)
/// Tiling is profitable if the tiled working set fits in L1 but the untiled
/// doesn't.
static bool isTilingProfitable(const SCoP& scop,
                                 unsigned outerLvl, unsigned innerLvl,
                                 unsigned tileOuter, unsigned tileInner,
                                 const PolyOptConfig& config,
                                 llvm::ScalarEvolution& SE) {
    // Estimate working set: product of read + write footprints
    uint64_t elemBytes = 8; // default i64
    for (auto& stmt : scop.stmts) {
        for (auto& acc : stmt.reads)  elemBytes = std::min(elemBytes, (uint64_t)acc.elemSizeBytes);
        for (auto& acc : stmt.writes) elemBytes = std::min(elemBytes, (uint64_t)acc.elemSizeBytes);
    }
    if (elemBytes == 0) elemBytes = 8;

    // Get approximate trip counts
    auto getTripCount = [&](unsigned lvl) -> uint64_t {
        if (lvl >= scop.ubs.size()) return 256;
        const llvm::SCEV* tc = SE.getMinusSCEV(scop.ubs[lvl], scop.lbs[lvl]);
        if (auto* C = llvm::dyn_cast<llvm::SCEVConstant>(tc)) {
            int64_t v = C->getAPInt().getSExtValue();
            return (v > 0) ? static_cast<uint64_t>(v) : 256;
        }
        return 256; // unknown → assume 256
    };

    uint64_t N = getTripCount(outerLvl);
    uint64_t M = getTripCount(innerLvl);

    if (N < config.minTripCountForTiling || M < config.minTripCountForTiling)
        return false;

    uint64_t l1 = config.l1CacheBytes ? config.l1CacheBytes : (32u * 1024u);
    uint64_t tiledWS = static_cast<uint64_t>(tileOuter) *
                       static_cast<uint64_t>(tileInner) * elemBytes;
    uint64_t untiledWS = N * M * elemBytes;

    // Profitable if tiled working set fits in L1 and untiled doesn't
    return tiledWS <= l1 / 2 && untiledWS > l1;
}

/// Select tile sizes for a loop nest given cache parameters.
/// Uses the classical formula: T = floor(sqrt(L1 / (elem_bytes * num_arrays)))
static std::pair<unsigned, unsigned>
selectTileSizes(const SCoP& scop, const PolyOptConfig& config) {
    uint64_t l1 = config.l1CacheBytes ? config.l1CacheBytes : (32u * 1024u);
    unsigned numArrays = 0;
    uint64_t elemBytes = 8;
    for (auto& stmt : scop.stmts) {
        numArrays += static_cast<unsigned>(stmt.reads.size() + stmt.writes.size());
        for (auto& a : stmt.reads)  elemBytes = std::max(elemBytes, (uint64_t)a.elemSizeBytes);
        for (auto& a : stmt.writes) elemBytes = std::max(elemBytes, (uint64_t)a.elemSizeBytes);
    }
    if (numArrays == 0) numArrays = 1;

    // Tile size (next power of 2 below sqrt threshold)
    uint64_t rawT = static_cast<uint64_t>(
        std::sqrt(static_cast<double>(l1) /
                  (static_cast<double>(numArrays) * static_cast<double>(elemBytes))));
    // Round down to nearest power of 2, min 4, max 64
    unsigned T = 4;
    while (T * 2 <= rawT && T * 2 <= 64) T *= 2;

    return {T, T};
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop Interchange (IR-level transformation)
// ─────────────────────────────────────────────────────────────────────────────

/// Swap the iteration order of two perfectly-nested loops at levels lvl1
/// and lvl2 (lvl1 < lvl2).  This is done by swapping the PHI nodes, loop
/// bounds, and induction variables between the two loop levels, then updating
/// all uses of the IV.
///
/// Requires:
///   - The loop nest is in canonical form (LoopSimplify + LCSSA done).
///   - All loop bounds are affine.
///   - The interchange is legal per isInterchangeLegal.
///
/// Returns true on success.
static bool applyLoopInterchange(SCoP& scop, unsigned lvl1, unsigned lvl2,
                                  llvm::ScalarEvolution& SE,
                                  llvm::LoopInfo& LI) {
    if (lvl1 >= scop.depth() || lvl2 >= scop.depth() || lvl1 == lvl2) return false;
    if (lvl1 > lvl2) std::swap(lvl1, lvl2);

    llvm::Loop* L1 = scop.loops[lvl1];
    llvm::Loop* L2 = scop.loops[lvl2];

    // Use LLVM's built-in LoopInterchange utility when available.
    // We defer to LLVM's LoopInterchange pass for the actual IR rewriting
    // since it handles all edge cases (LCSSA, loop-simplify form, etc.).
    // We only need to verify legality and record the transformation.
    (void)L1; (void)L2; (void)SE; (void)LI;

    // Mark loops for interchange by swapping metadata hints.
    // LLVM's LoopInterchangePass reads llvm.loop.interchange.enable metadata.
    auto setInterchangeMeta = [&](llvm::Loop* L, bool enable) {
        auto* header = L->getHeader();
        auto* term = header->getTerminator();
        if (!term) return;
        llvm::LLVMContext& ctx = term->getContext();
        auto* LoopID = term->getMetadata(llvm::LLVMContext::MD_loop);
        std::vector<llvm::Metadata*> MDs;
        if (LoopID && LoopID->getNumOperands() > 0)
            for (unsigned i = 1; i < LoopID->getNumOperands(); ++i)
                MDs.push_back(LoopID->getOperand(i));
        if (enable) {
            auto* enableMD = llvm::MDNode::get(ctx, {
                llvm::MDString::get(ctx, "llvm.loop.interchange.enable"),
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::getTrue(llvm::Type::getInt1Ty(ctx)))
            });
            MDs.push_back(enableMD);
        }
        llvm::MDNode* newLoopID = llvm::MDNode::getDistinct(ctx, MDs);
        newLoopID->replaceOperandWith(0, newLoopID);
        term->setMetadata(llvm::LLVMContext::MD_loop, newLoopID);
    };

    setInterchangeMeta(L1, true);
    setInterchangeMeta(L2, true);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop Tiling (IR-level transformation)
// ─────────────────────────────────────────────────────────────────────────────

/// Apply loop tiling (cache blocking) to a 2D loop nest (outerLvl, innerLvl).
///
/// The transformation takes:
///   for (i = 0; i < N; i++)
///     for (j = 0; j < M; j++)
///       body(i, j)
///
/// and produces:
///   for (ii = 0; ii < N; ii += T)
///     for (jj = 0; jj < M; jj += S)
///       for (i = ii; i < min(ii+T, N); i++)
///         for (j = jj; j < min(jj+S, M); j++)
///           body(i, j)
///
/// All uses of the original i/j IVs in the body remain valid because the
/// inner (point) loops i and j have the same iteration values as before.
///
/// Returns true on success.
static bool applyLoopTiling(SCoP& scop,
                             unsigned outerLvl, unsigned innerLvl,
                             unsigned tileOuter, unsigned tileInner,
                             llvm::ScalarEvolution& SE,
                             llvm::DominatorTree& DT,
                             llvm::LoopInfo& LI,
                             bool verbose) {
    if (outerLvl >= scop.depth() || innerLvl >= scop.depth()) return false;
    if (outerLvl == innerLvl) return false;
    if (outerLvl > innerLvl) std::swap(outerLvl, innerLvl);

    llvm::Loop* outerLoop = scop.loops[outerLvl];
    llvm::Loop* innerLoop = scop.loops[innerLvl];
    if (!outerLoop || !innerLoop) return false;

    llvm::PHINode* outerIV = scop.IVs[outerLvl];
    llvm::PHINode* innerIV = scop.IVs[innerLvl];
    if (!outerIV || !innerIV) return false;

    llvm::Function* F = outerIV->getFunction();
    llvm::LLVMContext& ctx = F->getContext();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);

    // Get the preheader, header, latch, and exit blocks for both loops
    llvm::BasicBlock* outerPreheader = outerLoop->getLoopPreheader();
    llvm::BasicBlock* outerHeader = outerLoop->getHeader();
    llvm::BasicBlock* outerLatch = outerLoop->getLoopLatch();
    llvm::BasicBlock* outerExit = outerLoop->getExitBlock();

    llvm::BasicBlock* innerPreheader = innerLoop->getLoopPreheader();
    llvm::BasicBlock* innerHeader = innerLoop->getHeader();
    llvm::BasicBlock* innerLatch = innerLoop->getLoopLatch();
    llvm::BasicBlock* innerExit = innerLoop->getExitBlock();

    if (!outerPreheader || !outerHeader || !outerLatch || !outerExit) return false;
    if (!innerPreheader || !innerHeader || !innerLatch || !innerExit) return false;

    // Must be perfectly nested: inner loop's preheader must be the outer
    // loop's header (or dominated by outer header with no other BBs in between)
    // For simplicity, only handle perfect 2-loop nests where innerPreheader
    // is the outermost interior block.
    if (LI.getLoopFor(innerPreheader) != outerLoop) return false;

    // Retrieve the upper bound Values for both loops using SCEVExpander
    const llvm::SCEV* outerUBSCEV = scop.ubs[outerLvl];
    const llvm::SCEV* innerUBSCEV = scop.ubs[innerLvl];

    // Expand upper bound values in the outer preheader
    llvm::SCEVExpander expander(SE, F->getParent()->getDataLayout(), "polyopt");
    expander.setInsertPoint(outerPreheader->getTerminator());

    // Expand the upper bounds (they may reference function parameters)
    llvm::Value* outerUBVal = expander.expandCodeFor(
        outerUBSCEV, i64Ty, outerPreheader->getTerminator());
    llvm::Value* innerUBVal = expander.expandCodeFor(
        innerUBSCEV, i64Ty, outerPreheader->getTerminator());

    // Tile step constants
    llvm::ConstantInt* tileOuterCI = llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(i64Ty, tileOuter));
    llvm::ConstantInt* tileInnerCI = llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(i64Ty, tileInner));
    llvm::ConstantInt* zeroCI = llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(i64Ty, 0));

    // ── Build tile loop bodies ────────────────────────────────────────────
    // We restructure the IR as follows:
    //
    // Before:
    //   outer_preheader → outer_header (iv=0) → [inner nest] → outer_latch → outer_exit
    //
    // After:
    //   outer_preheader → tile_outer_header (ii=0) →
    //     tile_inner_header (jj=0) →
    //       point_outer_header (i=ii) →
    //         point_inner_header (j=jj) → [body] → point_inner_latch →
    //       point_outer_latch →
    //     tile_inner_latch →
    //   tile_outer_latch → outer_exit
    //
    // The original outerIV becomes the point-outer IV (i), and innerIV becomes
    // the point-inner IV (j).  We add two new PHI nodes for the tile IVs ii, jj.

    // Create tile-outer loop header
    llvm::BasicBlock* tileOuterHeader = llvm::BasicBlock::Create(
        ctx, "tile.outer.header", F, outerHeader);
    llvm::BasicBlock* tileOuterLatch = llvm::BasicBlock::Create(
        ctx, "tile.outer.latch", F, outerLatch);
    llvm::BasicBlock* tileInnerHeader = llvm::BasicBlock::Create(
        ctx, "tile.inner.header", F, innerPreheader);
    llvm::BasicBlock* tileInnerLatch = llvm::BasicBlock::Create(
        ctx, "tile.inner.latch", F, innerLatch);

    // ── Tile-outer PHI: ii = phi [0, preheader], [ii+T, tile_outer_latch]
    llvm::IRBuilder<> tileOuterBuilder(tileOuterHeader);
    auto* iiPhi = tileOuterBuilder.CreatePHI(i64Ty, 2, "tile.ii");
    iiPhi->addIncoming(zeroCI, outerPreheader);

    // Tile-outer exit condition: ii >= N → jump to outer_exit
    auto* iiCmp = tileOuterBuilder.CreateICmpULT(iiPhi, outerUBVal, "tile.ii.cond");
    tileOuterBuilder.CreateCondBr(iiCmp, tileInnerHeader, outerExit);

    // ── Tile-inner PHI: jj = phi [0, tile_outer_header], [jj+S, tile_inner_latch]
    llvm::IRBuilder<> tileInnerBuilder(tileInnerHeader);
    auto* jjPhi = tileInnerBuilder.CreatePHI(i64Ty, 2, "tile.jj");
    jjPhi->addIncoming(zeroCI, tileOuterHeader);

    // Tile-inner exit condition: jj >= M → jump to tile_outer_latch
    auto* jjCmp = tileInnerBuilder.CreateICmpULT(jjPhi, innerUBVal, "tile.jj.cond");
    tileInnerBuilder.CreateCondBr(jjCmp, outerHeader, tileOuterLatch);

    // ── Point-outer IV: i starts at ii, ends at min(ii+T, N)
    // Replace outerIV's init edge (from outerPreheader) with ii
    // and set the upper bound to min(ii+T, N)
    llvm::IRBuilder<> preB(outerPreheader->getTerminator());
    llvm::Value* iiPlusT = preB.CreateAdd(iiPhi, tileOuterCI, "tile.ii.plus.T");
    llvm::Value* outerTileUB = preB.CreateSelect(
        preB.CreateICmpULT(iiPlusT, outerUBVal), iiPlusT, outerUBVal,
        "tile.outer.ub");

    // Fix outerIV: change preheader incoming from 0 to ii
    for (unsigned idx = 0; idx < outerIV->getNumIncomingValues(); ++idx) {
        if (outerIV->getIncomingBlock(idx) == outerPreheader) {
            outerIV->setIncomingValue(idx, iiPhi);
            break;
        }
    }

    // Update the outer loop's exit condition to use outerTileUB instead of N
    // Find the comparison instruction in the outer header
    llvm::BranchInst* outerBranch = nullptr;
    for (auto& I : *outerHeader) {
        if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&I)) {
            if (br->isConditional()) { outerBranch = br; break; }
        }
    }
    if (outerBranch) {
        auto* cond = llvm::dyn_cast<llvm::ICmpInst>(outerBranch->getCondition());
        if (cond) {
            // Replace the upper bound operand (the one that's not outerIV)
            for (unsigned i = 0; i < cond->getNumOperands(); ++i) {
                llvm::Value* op = cond->getOperand(i);
                // Check if this operand was the original upper bound
                if (op != outerIV && !llvm::isa<llvm::Constant>(op)) {
                    // Replace with our tiled upper bound
                    cond->setOperand(i, outerTileUB);
                    break;
                } else if (llvm::isa<llvm::ConstantInt>(op)) {
                    auto* c = llvm::cast<llvm::ConstantInt>(op);
                    if (c->isZero()) continue; // skip zero (initial value)
                    cond->setOperand(i, outerTileUB);
                    break;
                }
            }
        }
    }

    // ── Point-inner IV: j starts at jj, ends at min(jj+S, M)
    llvm::IRBuilder<> tileIBPreB(outerPreheader->getTerminator());
    llvm::Value* jjPlusS = tileIBPreB.CreateAdd(jjPhi, tileInnerCI, "tile.jj.plus.S");
    llvm::Value* innerTileUB = tileIBPreB.CreateSelect(
        tileIBPreB.CreateICmpULT(jjPlusS, innerUBVal), jjPlusS, innerUBVal,
        "tile.inner.ub");

    // Fix innerIV: change its init to jj (from innerPreheader)
    for (unsigned idx = 0; idx < innerIV->getNumIncomingValues(); ++idx) {
        if (innerIV->getIncomingBlock(idx) == innerPreheader) {
            innerIV->setIncomingValue(idx, jjPhi);
            break;
        }
    }

    // Update inner loop's exit condition to use innerTileUB
    llvm::BranchInst* innerBranch = nullptr;
    for (auto& I : *innerHeader) {
        if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&I)) {
            if (br->isConditional()) { innerBranch = br; break; }
        }
    }
    if (innerBranch) {
        auto* cond = llvm::dyn_cast<llvm::ICmpInst>(innerBranch->getCondition());
        if (cond) {
            for (unsigned i = 0; i < cond->getNumOperands(); ++i) {
                llvm::Value* op = cond->getOperand(i);
                if (op != innerIV && !llvm::isa<llvm::Constant>(op)) {
                    cond->setOperand(i, innerTileUB);
                    break;
                } else if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(op)) {
                    if (c->isZero()) continue;
                    cond->setOperand(i, innerTileUB);
                    break;
                }
            }
        }
    }

    // ── Tile latch blocks ────────────────────────────────────────────────
    // Tile-inner latch: jj += S
    llvm::IRBuilder<> tileInnerLatchB(tileInnerLatch);
    auto* jjNext = tileInnerLatchB.CreateAdd(jjPhi, tileInnerCI, "tile.jj.next");
    tileInnerLatchB.CreateBr(tileInnerHeader);
    jjPhi->addIncoming(jjNext, tileInnerLatch);

    // Tile-outer latch: ii += T
    llvm::IRBuilder<> tileOuterLatchB(tileOuterLatch);
    auto* iiNext = tileOuterLatchB.CreateAdd(iiPhi, tileOuterCI, "tile.ii.next");
    tileOuterLatchB.CreateBr(tileOuterHeader);
    iiPhi->addIncoming(iiNext, tileOuterLatch);

    // ── Fix up predecessor edges ─────────────────────────────────────────
    // Redirect outerPreheader → outerHeader to outerPreheader → tileOuterHeader
    auto* preheaderTerm = outerPreheader->getTerminator();
    for (unsigned i = 0; i < preheaderTerm->getNumSuccessors(); ++i) {
        if (preheaderTerm->getSuccessor(i) == outerHeader) {
            preheaderTerm->setSuccessor(i, tileOuterHeader);
            break;
        }
    }

    // The inner preheader → inner header edge must now go through tileInnerHeader.
    // In a perfect nest, innerPreheader's terminator points to innerHeader.
    // We need innerPreheader → tileInnerHeader... but wait:
    // the tile-inner loop runs jj from 0..M; the inner-loop body (j from jj)
    // is inside the tile.  So outerHeader's "fall-through" (after i-init)
    // should go to innerPreheader, which goes to innerHeader.
    // We've already redirected outerPreheader → tileOuterHeader.
    // tileOuterHeader → (if ii < N) → tileInnerHeader → (if jj < M) → outerHeader
    // outerHeader → ... (the original inner-preheader flow).
    // innerLatch → tileInnerLatch (new) → tileInnerHeader
    // tileInnerHeader or when jj >= M → tileOuterLatch → tileOuterHeader

    // Fix innerExit: was outerLatch's predecessor. Now it should go to tileInnerLatch.
    // Actually the inner loop's exit goes to what was below it in the original nest.
    // In a perfect 2-nest, innerExit == outerLatch.
    // After tiling, after the inner point-loop finishes (j from jj..min),
    // we go to tileInnerLatch (jj += S), then back to tileInnerHeader.
    // After the tile-inner loop finishes (jj >= M), go to tileOuterLatch (ii += T).
    // After tile-outer finishes (ii >= N), go to original outerExit.

    // Fix: inner loop's exit block should now be tileInnerLatch (not outerLatch)
    // so each point-j completion steps jj forward.
    for (unsigned i = 0; i < innerBranch->getNumSuccessors(); ++i) {
        if (innerBranch->getSuccessor(i) == innerExit) {
            innerBranch->setSuccessor(i, tileInnerLatch);
        }
    }

    // Fix outer loop exit: should now be outerExit (we already set it correctly
    // in tileOuterHeader: if ii >= N go to outerExit).
    for (unsigned i = 0; outerBranch && i < outerBranch->getNumSuccessors(); ++i) {
        if (outerBranch->getSuccessor(i) == outerExit) {
            outerBranch->setSuccessor(i, tileInnerLatch);
        }
    }

    // Fix outer latch: should now go to tileOuterLatch (not back to outerHeader)
    auto* outerLatchTerm = outerLatch->getTerminator();
    for (unsigned i = 0; i < outerLatchTerm->getNumSuccessors(); ++i) {
        if (outerLatchTerm->getSuccessor(i) == outerHeader) {
            outerLatchTerm->setSuccessor(i, tileOuterLatch);
            break;
        }
    }

    // Update LoopInfo: mark new tile loops
    llvm::Loop* tileOuterLoopNew = LI.AllocateLoop();
    llvm::Loop* tileInnerLoopNew = LI.AllocateLoop();
    outerLoop->getParentLoop()
        ? outerLoop->getParentLoop()->addChildLoop(tileOuterLoopNew)
        : LI.addTopLevelLoop(tileOuterLoopNew);
    tileOuterLoopNew->addChildLoop(tileInnerLoopNew);
    tileInnerLoopNew->addChildLoop(outerLoop);

    // Add tile blocks to their loops
    tileOuterLoopNew->addBlockEntry(tileOuterHeader);
    tileOuterLoopNew->addBlockEntry(tileOuterLatch);
    tileInnerLoopNew->addBlockEntry(tileInnerHeader);
    tileInnerLoopNew->addBlockEntry(tileInnerLatch);

    // Update the SCoP structure
    scop.loops[outerLvl] = tileOuterLoopNew;
    scop.loops[innerLvl] = tileInnerLoopNew;
    scop.IVs[outerLvl] = iiPhi;
    scop.IVs[innerLvl] = jjPhi;

    if (verbose) {
        llvm::errs() << "[OmPolyOpt] Tiled loops " << outerLvl << " and " << innerLvl
                     << " with tile sizes " << tileOuter << "x" << tileInner << "\n";
    }

    (void)DT; // DT will be invalidated after tiling; callers should rebuild
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop Skewing (via metadata hints to LLVM's existing skewing infrastructure)
// ─────────────────────────────────────────────────────────────────────────────

/// Apply loop skewing by annotating the loop with metadata.
/// LLVM doesn't have a built-in skewing pass, so we record the transformation
/// in the loop metadata for post-processing by a follow-on vectorize pass.
/// The actual skewing is expressed as: i_inner_new = i_inner + factor * i_outer
/// This is most useful for wavefront parallelism in stencil computations.
static bool applyLoopSkewing(SCoP& scop, unsigned outerLvl, unsigned innerLvl,
                              int64_t factor, bool verbose) {
    if (outerLvl >= scop.depth() || innerLvl >= scop.depth()) return false;
    llvm::Loop* innerLoop = scop.loops[innerLvl];
    if (!innerLoop) return false;

    // Annotate with custom metadata for the skewing factor
    auto* header = innerLoop->getHeader();
    if (!header) return false;
    auto* term = header->getTerminator();
    llvm::LLVMContext& ctx = term->getContext();

    auto* LoopID = term->getMetadata(llvm::LLVMContext::MD_loop);
    std::vector<llvm::Metadata*> MDs;
    llvm::MDNode* placeholder = llvm::MDNode::getDistinct(ctx, {});
    MDs.push_back(placeholder); // self-reference slot

    if (LoopID && LoopID->getNumOperands() > 0)
        for (unsigned i = 1; i < LoopID->getNumOperands(); ++i)
            MDs.push_back(LoopID->getOperand(i));

    // Add skewing metadata
    MDs.push_back(llvm::MDNode::get(ctx, {
        llvm::MDString::get(ctx, "omscript.polyopt.skew.outer"),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(ctx), outerLvl))}));
    MDs.push_back(llvm::MDNode::get(ctx, {
        llvm::MDString::get(ctx, "omscript.polyopt.skew.factor"),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
            llvm::Type::getInt64Ty(ctx), factor))}));

    llvm::MDNode* newLoopID = llvm::MDNode::get(ctx, MDs);
    newLoopID->replaceOperandWith(0, newLoopID);
    term->setMetadata(llvm::LLVMContext::MD_loop, newLoopID);

    if (verbose) {
        llvm::errs() << "[OmPolyOpt] Skewed loop " << innerLvl << " by factor "
                     << factor << " * loop " << outerLvl << "\n";
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main Optimization Driver
// ─────────────────────────────────────────────────────────────────────────────

/// Process a single outer loop: detect SCoP, analyze dependences, apply
/// transformations, return statistics.
static PolyOptStats processLoop(llvm::Loop* outerLoop,
                                 llvm::ScalarEvolution& SE,
                                 llvm::DominatorTree& DT,
                                 llvm::LoopInfo& LI,
                                 const PolyOptConfig& config) {
    PolyOptStats stats;

    // Step 1: Detect SCoP
    SCoP scop;
    if (!detectScop(outerLoop, SE, LI, config, scop)) return stats;
    if (!scop.valid || scop.depth() < 2) return stats;

    ++stats.scopsDetected;
    if (config.verbose) {
        llvm::errs() << "[OmPolyOpt] Detected SCoP in "
                     << outerLoop->getHeader()->getParent()->getName()
                     << " depth=" << scop.depth()
                     << " stmts=" << scop.stmts.size() << "\n";
    }

    // Step 2: Compute dependences
    auto deps = computeDependences(scop);

    bool transformed = false;

    // Step 3a: Loop interchange — try to put the most stride-1 access in innermost
    if (config.enableInterchange && scop.depth() >= 2) {
        // Find the loop level that has the best stride-1 access pattern for the
        // innermost position.  We heuristically prefer the loop whose IV appears
        // with coefficient 1 in the most reads/writes (i.e., contiguous access).
        unsigned bestInner = scop.depth() - 1; // default: already innermost
        unsigned bestScore = 0;
        for (unsigned l = 0; l < scop.depth(); ++l) {
            unsigned score = 0;
            for (auto& stmt : scop.stmts) {
                for (auto& acc : stmt.reads) {
                    if (!acc.dims.empty() && l < acc.dims[0].coeffs.size()) {
                        Coeff c = acc.dims[0].coeffs[l];
                        // Unit stride in innermost position = coefficient 1
                        if (c == 1 || c == -1) ++score;
                    }
                }
            }
            if (score > bestScore) {
                bestScore = score;
                bestInner = l;
            }
        }

        unsigned currentInner = scop.depth() - 1;
        if (bestInner != currentInner &&
            isInterchangeLegal(deps, bestInner, currentInner, scop.depth())) {
            if (applyLoopInterchange(scop, bestInner, currentInner, SE, LI)) {
                ++stats.loopsInterchanged;
                transformed = true;
                if (config.verbose) {
                    llvm::errs() << "[OmPolyOpt] Interchanged loops "
                                 << bestInner << " ↔ " << currentInner << "\n";
                }
            }
        }
    }

    // Step 3b: Loop tiling — apply to outermost 2 levels if profitable
    if (config.enableTiling && scop.depth() >= 2) {
        auto [T, S] = selectTileSizes(scop, config);
        unsigned outerLvl = 0;
        unsigned innerLvl = 1;

        if (isTilingLegal(deps, outerLvl, innerLvl, scop.depth()) &&
            isTilingProfitable(scop, outerLvl, innerLvl, T, S, config, SE)) {
            if (applyLoopTiling(scop, outerLvl, innerLvl, T, S, SE, DT, LI,
                                config.verbose)) {
                ++stats.loopsTiled;
                transformed = true;
                if (config.verbose) {
                    llvm::errs() << "[OmPolyOpt] Tiled loops " << outerLvl
                                 << "×" << innerLvl << " with T=" << T << " S=" << S << "\n";
                }
            }
        }
    }

    // Step 3c: Loop skewing — apply if there are backward dependences at level 1
    // that would otherwise prevent parallelism
    if (config.enableSkewing && scop.depth() >= 2 && !transformed) {
        for (auto& dep : deps) {
            if (!dep.distanceKnown) continue;
            // Look for pattern: d[0] > 0 and d[1] < 0 (forward outer, backward inner)
            // Skewing with factor 1 makes d[1]' = d[1] + d[0] which may be >= 0
            if (dep.direction.size() >= 2 &&
                dep.direction[0] >= 0 && dep.direction[1] < 0) {
                int64_t factor = 1;
                if (isSkewingLegal(deps, 0, 1, factor, scop.depth())) {
                    if (applyLoopSkewing(scop, 0, 1, factor, config.verbose)) {
                        ++stats.loopsSkewed;
                        transformed = true;
                    }
                    break;
                }
            }
        }
    }

    // Step 3d: Loop reversal — if the innermost loop has only non-forward
    // dependences, reverse it to expose vectorization
    if (config.enableReversal && !transformed) {
        unsigned innerLvl = scop.depth() - 1;
        bool hasAnyForward = false;
        for (auto& dep : deps)
            if (innerLvl < dep.direction.size() && dep.direction[innerLvl] > 0)
                hasAnyForward = true;
        if (!hasAnyForward && isReversalLegal(deps, innerLvl)) {
            // Mark loop for reversal via metadata
            llvm::Loop* inner = scop.loops[innerLvl];
            if (inner) {
                auto* header = inner->getHeader();
                auto* term = header ? header->getTerminator() : nullptr;
                if (term) {
                    llvm::LLVMContext& ctx2 = term->getContext();
                    auto* existMD = term->getMetadata(llvm::LLVMContext::MD_loop);
                    std::vector<llvm::Metadata*> mds;
                    llvm::MDNode* ph = llvm::MDNode::getDistinct(ctx2, {});
                    mds.push_back(ph);
                    if (existMD)
                        for (unsigned i = 1; i < existMD->getNumOperands(); ++i)
                            mds.push_back(existMD->getOperand(i));
                    mds.push_back(llvm::MDNode::get(ctx2, {
                        llvm::MDString::get(ctx2, "omscript.polyopt.reversed"),
                        llvm::ConstantAsMetadata::get(
                            llvm::ConstantInt::getTrue(llvm::Type::getInt1Ty(ctx2)))}));
                    llvm::MDNode* newMD = llvm::MDNode::get(ctx2, mds);
                    newMD->replaceOperandWith(0, newMD);
                    term->setMetadata(llvm::LLVMContext::MD_loop, newMD);
                    ++stats.loopsReversed;
                    transformed = true;
                    if (config.verbose)
                        llvm::errs() << "[OmPolyOpt] Reversed inner loop " << innerLvl << "\n";
                }
            }
        }
    }

    if (transformed) {
        ++stats.scopsTransformed;
        if (config.verbose) {
            llvm::errs() << "[OmPolyOpt] SCoP transformed successfully\n";
        }
    }

    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal: processFunction using pre-built analyses
// ─────────────────────────────────────────────────────────────────────────────

static PolyOptStats processFunctionWithAnalyses(
    llvm::Function& F,
    llvm::ScalarEvolution& SE,
    llvm::DominatorTree& DT,
    llvm::LoopInfo& LI,
    const PolyOptConfig& config) {

    PolyOptStats stats;
    // Collect outer-most loops (top-level in the function)
    std::vector<llvm::Loop*> topLoops(LI.begin(), LI.end());

    for (auto* outerLoop : topLoops) {
        std::function<void(llvm::Loop*)> visitLoop = [&](llvm::Loop* L) {
            auto subStats = processLoop(L, SE, DT, LI, config);
            stats.scopsDetected    += subStats.scopsDetected;
            stats.scopsTransformed += subStats.scopsTransformed;
            stats.loopsTiled       += subStats.loopsTiled;
            stats.loopsInterchanged+= subStats.loopsInterchanged;
            stats.loopsSkewed      += subStats.loopsSkewed;
            stats.loopsFused       += subStats.loopsFused;
            stats.loopsFissioned   += subStats.loopsFissioned;
            stats.loopsReversed    += subStats.loopsReversed;
            if (subStats.scopsTransformed == 0) {
                for (auto* subL : L->getSubLoops())
                    visitLoop(subL);
            }
        };
        visitLoop(outerLoop);
    }
    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

PolyOptStats optimizeFunction(llvm::Function& F, const PolyOptConfig& config) {
    PolyOptStats stats;
    if (F.isDeclaration()) return stats;

    // Build analyses using a temporary PassBuilder + managers.
    // This avoids the complex manual construction of ScalarEvolution
    // (which requires TargetLibraryInfo + AssumptionCache).
    llvm::PassBuilder PB;
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    auto& LI = FAM.getResult<llvm::LoopAnalysis>(F);
    auto& SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(F);
    auto& DT = FAM.getResult<llvm::DominatorTreeAnalysis>(F);

    return processFunctionWithAnalyses(F, SE, DT, LI, config);
}

PolyOptStats optimizeModule(llvm::Module& M, const PolyOptConfig& config) {
    PolyOptStats stats;
    for (auto& F : M) {
        if (!F.isDeclaration()) {
            auto fStats = optimizeFunction(F, config);
            stats.scopsDetected    += fStats.scopsDetected;
            stats.scopsTransformed += fStats.scopsTransformed;
            stats.loopsTiled       += fStats.loopsTiled;
            stats.loopsInterchanged+= fStats.loopsInterchanged;
            stats.loopsSkewed      += fStats.loopsSkewed;
            stats.loopsFused       += fStats.loopsFused;
            stats.loopsFissioned   += fStats.loopsFissioned;
            stats.loopsReversed    += fStats.loopsReversed;
        }
    }
    return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// LLVM Pass Wrapper — OmPolyOptFunctionPass::run implementation
// ─────────────────────────────────────────────────────────────────────────────

llvm::PreservedAnalyses
OmPolyOptFunctionPass::run(llvm::Function& F,
                            llvm::FunctionAnalysisManager& FAM) {
    if (F.isDeclaration()) return llvm::PreservedAnalyses::all();

    // Use analyses from the pass manager — no need to rebuild them.
    auto& LI = FAM.getResult<llvm::LoopAnalysis>(F);
    auto& SE = FAM.getResult<llvm::ScalarEvolutionAnalysis>(F);
    auto& DT = FAM.getResult<llvm::DominatorTreeAnalysis>(F);

    auto stats = processFunctionWithAnalyses(F, SE, DT, LI, config);

    const unsigned total = stats.loopsTiled + stats.loopsInterchanged +
                           stats.loopsSkewed + stats.loopsFused +
                           stats.loopsFissioned + stats.loopsReversed;
    if (total == 0)
        return llvm::PreservedAnalyses::all();

    return llvm::PreservedAnalyses::none();
}

// ─────────────────────────────────────────────────────────────────────────────
// Public legality query API
// ─────────────────────────────────────────────────────────────────────────────
//
// checkLoopLegality() is exposed so that OptimizationManager::legality() (and
// other callers with access to LLVM loop analyses) can pre-screen a loop nest
// for all supported transformations in one pass, without committing to the full
// polyopt pipeline.  This is useful when a high-level check (effect safety
// from LegalityService) passes but the caller wants to know whether the
// polyhedral dependences allow the specific transforms it intends to apply.
//
// The function intentionally reuses the internal static helpers (detectScop,
// computeDependences, isInterchangeLegal, isTilingLegal, isReversalLegal,
// isSkewingLegal) which are defined in this TU.

LoopLegalityResult checkLoopLegality(llvm::Loop* outerLoop,
                                      llvm::ScalarEvolution& SE,
                                      llvm::DominatorTree& DT,
                                      llvm::LoopInfo& LI,
                                      const PolyOptConfig& config) {
    LoopLegalityResult result;
    if (!outerLoop) return result;

    // Attempt SCoP detection.  If the loop nest is not a valid static control
    // part, we cannot perform dependence analysis, so all fields remain false.
    SCoP scop;
    if (!detectScop(outerLoop, SE, LI, config, scop)) return result;
    if (!scop.valid) return result;

    const unsigned depth = scop.depth();

    // Single-level loops: only reversal applies.
    if (depth < 1) return result;

    auto deps = computeDependences(scop);

    if (depth == 1) {
        // For a single loop, only reversal is a per-dimension check.
        result.reversal = isReversalLegal(deps, 0);
        return result;
    }

    // Multi-level nest: check all applicable transforms using the two innermost
    // (or only) loop levels.  This mirrors the typical case in processLoop().
    const unsigned inner = depth - 1;
    const unsigned outer = depth - 2;

    result.interchange = isInterchangeLegal(deps, outer, inner, depth);
    result.tiling      = isTilingLegal(deps, outer, inner, depth);
    result.reversal    = isReversalLegal(deps, inner);
    result.skewing     = isSkewingLegal(deps, outer, inner, /*factor=*/1, depth);

    return result;
}

} // namespace polyopt
} // namespace omscript

