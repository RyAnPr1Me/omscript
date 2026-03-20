#include "codegen.h"
#include "diagnostic.h"
#include "hardware_graph.h"
#include "superoptimizer.h"
#include <iostream>
#include <llvm/ADT/StringMap.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#ifdef POLLY_LIB_PATH
#if LLVM_VERSION_MAJOR >= 22
#include <llvm/Plugins/PassPlugin.h>
#else
#include <llvm/Passes/PassPlugin.h>
#endif
#endif
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/PGOOptions.h>
#include <llvm/Support/TargetSelect.h>
#if LLVM_VERSION_MAJOR < 22
#include <llvm/Support/VirtualFileSystem.h>
#endif
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/GlobalDCE.h>
#include <llvm/Transforms/IPO/GlobalOpt.h>
#include <llvm/Transforms/IPO/HotColdSplitting.h>
#include <llvm/Transforms/IPO/InferFunctionAttrs.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/CorrelatedValuePropagation.h>
#include <llvm/Transforms/Scalar/ConstraintElimination.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/Float2Int.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/IndVarSimplify.h>
#include <llvm/Transforms/Scalar/LoopDeletion.h>
#include <llvm/Transforms/Scalar/LoopDistribute.h>
#include <llvm/Transforms/Scalar/LoopFuse.h>
#include <llvm/Transforms/Scalar/LoopIdiomRecognize.h>
#include <llvm/Transforms/Scalar/LoopInterchange.h>
#include <llvm/Transforms/Scalar/LoopLoadElimination.h>
#include <llvm/Transforms/Scalar/LoopRotation.h>
#include <llvm/Transforms/Scalar/LoopSink.h>
#include <llvm/Transforms/Scalar/MergeICmps.h>
#include <llvm/Transforms/Scalar/MergedLoadStoreMotion.h>
#include <llvm/Transforms/Scalar/MemCpyOptimizer.h>
#include <llvm/Transforms/Scalar/NaryReassociate.h>
#include <llvm/Transforms/Scalar/BDCE.h>
#include <llvm/Transforms/Scalar/JumpThreading.h>
#include <llvm/Transforms/Scalar/StraightLineStrengthReduce.h>
#include <llvm/Transforms/Scalar/TailRecursionElimination.h>
#include <llvm/Transforms/Scalar/LoopUnrollPass.h>
#include <llvm/Transforms/Scalar/SimpleLoopUnswitch.h>
#include <llvm/Transforms/Scalar/SpeculativeExecution.h>
#include <llvm/Transforms/Scalar/DivRemPairs.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Vectorize/VectorCombine.h>
#include <optional>
#include <stdexcept>

namespace omscript {

void CodeGenerator::resolveTargetCPU(std::string& cpu, std::string& features) const {
    bool isNative = marchCpu_.empty() || marchCpu_ == "native";
    if (isNative) {
        cpu = llvm::sys::getHostCPUName().str();
        llvm::SubtargetFeatures featureSet;
#if LLVM_VERSION_MAJOR >= 19
        llvm::StringMap<bool> hostFeatures = llvm::sys::getHostCPUFeatures();
#else
        llvm::StringMap<bool> hostFeatures;
        llvm::sys::getHostCPUFeatures(hostFeatures);
#endif
        for (auto& feature : hostFeatures) {
            featureSet.AddFeature(feature.first(), feature.second);
        }
        features = featureSet.getString();
    } else {
        // Use the specified CPU; LLVM derives features from the CPU name.
        cpu = marchCpu_;
        features = "";
    }

    // -mtune overrides the CPU used for scheduling when -march is native or
    // unset.  When an explicit -march is given, it takes precedence because
    // LLVM's createTargetMachine uses a single CPU parameter for both
    // instruction selection and scheduling.
    // "native" is resolved to the host CPU name just like -march=native.
    if (!mtuneCpu_.empty() && isNative) {
        if (mtuneCpu_ == "native") {
            cpu = llvm::sys::getHostCPUName().str();
        } else {
            cpu = mtuneCpu_;
        }
    }
}

// ---------------------------------------------------------------------------
// Shared TargetMachine construction
// ---------------------------------------------------------------------------
// Both runOptimizationPasses() and writeObjectFile() need a configured
// TargetMachine.  This helper consolidates the duplicated triple setup,
// target lookup, TargetOptions configuration, and version-conditional
// createTargetMachine() call into a single place.

std::unique_ptr<llvm::TargetMachine> CodeGenerator::createTargetMachine() const {
    std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
#endif

    std::string error;
#if LLVM_VERSION_MAJOR >= 19
    auto* target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
#else
    auto* target = llvm::TargetRegistry::lookupTarget(targetTripleStr, error);
#endif
    if (!target)
        return nullptr;

    llvm::TargetOptions opt;
    if (useFastMath_) {
#if LLVM_VERSION_MAJOR < 22
        opt.UnsafeFPMath = true;
#endif
        opt.NoInfsFPMath = true;
        opt.NoNaNsFPMath = true;
        opt.NoSignedZerosFPMath = true;
    }
    std::optional<llvm::Reloc::Model> RM = usePIC_ ? llvm::Reloc::PIC_ : llvm::Reloc::Static;

    std::string cpu;
    std::string features;
    resolveTargetCPU(cpu, features);

    // Map the compiler's optimization level to LLVM's backend CodeGenOpt level
    // so that instruction selection, scheduling, and register allocation use
    // the appropriate aggressiveness.
#if LLVM_VERSION_MAJOR >= 18
    llvm::CodeGenOptLevel cgOpt = llvm::CodeGenOptLevel::Default;
    switch (optimizationLevel) {
    case OptimizationLevel::O0:
        cgOpt = llvm::CodeGenOptLevel::None;
        break;
    case OptimizationLevel::O1:
        cgOpt = llvm::CodeGenOptLevel::Less;
        break;
    case OptimizationLevel::O2:
        cgOpt = llvm::CodeGenOptLevel::Default;
        break;
    case OptimizationLevel::O3:
        cgOpt = llvm::CodeGenOptLevel::Aggressive;
        break;
    }
#else
    llvm::CodeGenOpt::Level cgOpt = llvm::CodeGenOpt::Default;
    switch (optimizationLevel) {
    case OptimizationLevel::O0:
        cgOpt = llvm::CodeGenOpt::None;
        break;
    case OptimizationLevel::O1:
        cgOpt = llvm::CodeGenOpt::Less;
        break;
    case OptimizationLevel::O2:
        cgOpt = llvm::CodeGenOpt::Default;
        break;
    case OptimizationLevel::O3:
        cgOpt = llvm::CodeGenOpt::Aggressive;
        break;
    }
#endif

#if LLVM_VERSION_MAJOR >= 19
    return std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(targetTriple, cpu, features, opt, RM, std::nullopt, cgOpt));
#else
    return std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(targetTripleStr, cpu, features, opt, RM, std::nullopt, cgOpt));
#endif
}

void CodeGenerator::runOptimizationPasses() {
    // Ensure the native target is initialized before we try to create a
    // TargetMachine.  These calls are idempotent and fast after the first.
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    // Set the target triple on the module.
    std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
    module->setTargetTriple(targetTriple);
#else
    module->setTargetTriple(targetTripleStr);
#endif

    auto targetMachine = createTargetMachine();
    if (targetMachine) {
        module->setDataLayout(targetMachine->createDataLayout());
    } else if (optimizationLevel != OptimizationLevel::O0) {
        // Data layout is required for correct optimization; warn but continue.
        llvm::errs() << "omsc: warning: could not create target machine; "
                        "optimization passes may produce suboptimal code\n";
    }

    if (optimizationLevel == OptimizationLevel::O0) {
        if (verbose_) {
            std::cout << "    Optimization level O0: skipping all passes" << std::endl;
        }
        return;
    }

    // All optimization levels (O1, O2, O3) use the new pass manager's
    // standard pipeline which includes interprocedural optimizations such as
    // function inlining, IPSCCP (sparse conditional constant propagation),
    // GlobalDCE (dead function removal), jump threading, correlated value
    // propagation, loop vectorization (O2+), SLP vectorization (O2+), and more.
    //
    // PipelineTuningOptions controls vectorization and loop unrolling globally.
    // This is the correct way to enable/disable these passes in the new PM;
    // it replaces the previous approach of injecting per-loop metadata hints
    // with llvm.loop.vectorize.enable/llvm.loop.unroll.enable, which caused
    // "unable to perform the requested transformation" warnings when the
    // optimizer could not satisfy the forced hints.
    llvm::PipelineTuningOptions PTO;
    PTO.LoopVectorization = enableVectorize_;
    PTO.SLPVectorization = enableVectorize_;
    // Re-enable LLVM's cost-model-driven loop unrolling.  The standard O3
    // pipeline has excellent register-pressure-aware unrolling heuristics.
    // We previously disabled this because the HGOE pre-pipeline was injecting
    // bad unroll metadata that caused over-unrolling; now that pre-pipeline
    // annotation is disabled, LLVM's own unroller makes good decisions.
    PTO.LoopUnrolling = enableUnrollLoops_;
    PTO.LoopInterleaving = enableVectorize_; // enable loop interleaving at O2+

    if (verbose_) {
        const char* levelStr = "O2";
        if (optimizationLevel == OptimizationLevel::O1) levelStr = "O1";
        else if (optimizationLevel == OptimizationLevel::O3) levelStr = "O3";
        std::cout << "    Optimization level: " << levelStr << std::endl;
        std::cout << "    Pipeline options:"
                  << " vectorize=" << (enableVectorize_ ? "on" : "off")
                  << ", unroll=" << (enableUnrollLoops_ ? "on" : "off")
                  << ", loop-optimize=" << (enableLoopOptimize_ ? "on" : "off")
                  << std::endl;
        if (!pgoGenPath_.empty()) std::cout << "    PGO instrumentation: " << pgoGenPath_ << std::endl;
        if (!pgoUsePath_.empty()) std::cout << "    PGO profile-use: " << pgoUsePath_ << std::endl;
    }
    // Enable cross-function optimizations at O2 and above:
    // MergeFunctions deduplicates identical function bodies, shrinking
    // I-cache footprint. CallGraphProfile biases the inliner toward
    // functions on hot call-graph edges (useful even without explicit PGO).
    if (optimizationLevel >= OptimizationLevel::O2) {
        PTO.MergeFunctions = true;
        PTO.CallGraphProfile = true;
    }
    if (optimizationLevel == OptimizationLevel::O3) {
        PTO.InlinerThreshold = 400; // more aggressive inlining than the default ~225
    }

    // ---------------------------------------------------------------------------
    // PGO (Profile-Guided Optimization) support
    // ---------------------------------------------------------------------------
    // Two-phase workflow:
    //
    //   Phase 1 – Instrumentation generation (--pgo-gen=<path>):
    //     The compiler inserts LLVM IR instrumentation counters into every
    //     basic block and function entry.  At program exit the runtime writes
    //     a raw profile file (.profraw) to the specified path.  Run the binary
    //     with representative workloads to collect profile data, then convert:
    //       llvm-profdata merge -output=prog.profdata prog.profraw
    //
    //   Phase 2 – Profile-use optimization (--pgo-use=<path>):
    //     The optimizer reads the merged .profdata file and uses the branch
    //     and call-count data to guide: function inlining decisions (hot
    //     callees get larger inline budgets), branch layout (hot side goes
    //     in the fall-through path to improve I-cache density), loop
    //     trip-count estimation (enables better vectorization and unrolling),
    //     and cold-code sinking (rare error paths moved out of hot regions).
    std::optional<llvm::PGOOptions> pgoOpt;
    if (!pgoGenPath_.empty()) {
        // Instrumentation generation: insert counters, write .profraw on exit.
#if LLVM_VERSION_MAJOR >= 22
        pgoOpt = llvm::PGOOptions(pgoGenPath_,                // ProfileFile: output path for raw profile
                                  "",                         // CSProfileGenFile: context-sensitive profile (unused)
                                  "",                         // ProfileRemappingFile: symbol remapping (unused)
                                  "",                         // MemoryProfile: memory profile (unused)
                                  llvm::PGOOptions::IRInstr); // Action: IR-level instrumentation
#else
        pgoOpt = llvm::PGOOptions(pgoGenPath_, // ProfileFile: output path for raw profile
                                  "",          // CSProfileGenFile: context-sensitive profile (unused)
                                  "",          // ProfileRemappingFile: symbol remapping (unused)
                                  "",          // MemoryProfile: memory profile (unused)
                                  llvm::vfs::getRealFileSystem(),
                                  llvm::PGOOptions::IRInstr); // Action: IR-level instrumentation
#endif
    } else if (!pgoUsePath_.empty()) {
        // Profile-use: feed collected data into the optimization pipeline.
#if LLVM_VERSION_MAJOR >= 22
        pgoOpt = llvm::PGOOptions(pgoUsePath_,              // ProfileFile: input .profdata path
                                  "",                       // CSProfileGenFile: unused
                                  "",                       // ProfileRemappingFile: unused
                                  "",                       // MemoryProfile: unused
                                  llvm::PGOOptions::IRUse); // Action: apply IR-level profile
#else
        pgoOpt = llvm::PGOOptions(pgoUsePath_, // ProfileFile: input .profdata path
                                  "",          // CSProfileGenFile: unused
                                  "",          // ProfileRemappingFile: unused
                                  "",          // MemoryProfile: unused
                                  llvm::vfs::getRealFileSystem(),
                                  llvm::PGOOptions::IRUse); // Action: apply IR-level profile
#endif
    }

    llvm::PassBuilder PB(targetMachine.get(), PTO, pgoOpt);

    // At O2+ with -floop-optimize, load the LLVM Polly polyhedral optimizer
    // plugin.  Polly provides high-level loop transformations (tiling, fusion,
    // interchange) based on the polyhedral model, improving data locality and
    // enabling automatic parallelization for affine loop nests.  The plugin
    // registers its analysis passes and pipeline extension-point callbacks so
    // that the standard buildPerModuleDefaultPipeline() automatically invokes
    // Polly at the appropriate point in the optimization pipeline.
#ifdef POLLY_LIB_PATH
    if (optimizationLevel >= OptimizationLevel::O2 && enableLoopOptimize_) {
        if (verbose_) {
            std::cout << "    Loading Polly polyhedral loop optimizer plugin..." << std::endl;
        }
        auto pollyPlugin = llvm::PassPlugin::Load(POLLY_LIB_PATH);
        if (pollyPlugin) {
            pollyPlugin->registerPassBuilderCallbacks(PB);
            if (verbose_) {
                std::cout << "    Polly plugin loaded successfully" << std::endl;
            }
        } else {
            llvm::errs() << "omsc: warning: failed to load Polly plugin; "
                            "polyhedral loop optimizations disabled\n";
            llvm::consumeError(pollyPlugin.takeError());
        }
    }
#endif

    // NOTE: InferFunctionAttrsPass is intentionally NOT registered here.
    // LLVM's default pipeline already includes it, and we previously added it
    // again as an explicit pipeline-start callback.  However, this pass auto-
    // infers aggressive attributes (allocsize, allockind, inaccessibleMemOnly)
    // on well-known C library functions like malloc.  Those attributes interact
    // poorly with the loop unroller/optimizer when the allocated buffer is
    // immediately written to in a loop (e.g. string repetition via strcat or
    // memcpy): the optimizer can incorrectly eliminate the loop exit condition,
    // producing an infinite loop and segfault.  The attributes we manually set
    // on library function declarations are sufficient.

    // At O2+ with -floop-optimize, register LoopDistributePass to run just
    // before the vectorizer starts.  Loop distribution splits a loop with
    // multiple independent memory access patterns into separate loops, each
    // touching a smaller working set — this improves cache locality and
    // enables downstream vectorization of the resulting simpler loops.
    // Running it BEFORE vectorization (via registerVectorizerStartEPCallback)
    // is critical; appending it after the full pipeline produced
    // "unsupported transformation ordering" warnings because vectorization
    // had already consumed and transformed the original loop structure.
    if (optimizationLevel >= OptimizationLevel::O2 && enableLoopOptimize_) {
        PB.registerVectorizerStartEPCallback(
            [](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) { FPM.addPass(llvm::LoopDistributePass()); });
    }

    // At O2+, inject LoopLoadEliminationPass before vectorization to forward
    // values from stores to subsequent loads within the same loop iteration,
    // eliminating redundant memory traffic and enabling vectorization of
    // reduction-like patterns.
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerVectorizerStartEPCallback([](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) {
            FPM.addPass(llvm::LoopLoadEliminationPass());
        });
    }

    // At O3 with loop optimization enabled (enableLoopOptimize_ / -floop-optimize),
    // register LoopFusePass before vectorization to merge adjacent loops with
    // the same trip count into a single loop, dramatically improving data cache
    // locality and reducing loop overhead.  This is particularly beneficial for
    // array-processing code where successive passes over the same data can be
    // combined.
    if (optimizationLevel >= OptimizationLevel::O3 && enableLoopOptimize_) {
        PB.registerVectorizerStartEPCallback(
            [](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) { FPM.addPass(llvm::LoopFusePass()); });
    }

    // At O3 with -floop-optimize, register LoopInterchangePass at the end of
    // the loop optimizer to interchange nested loops for better cache locality.
    // This transforms loop-nest orders so that the innermost loop accesses
    // memory contiguously (e.g. column-major → row-major), dramatically
    // reducing cache misses for matrix-style code.
    if (optimizationLevel >= OptimizationLevel::O3 && enableLoopOptimize_) {
        PB.registerLoopOptimizerEndEPCallback(
            [](llvm::LoopPassManager& LPM, llvm::OptimizationLevel) { LPM.addPass(llvm::LoopInterchangePass()); });
    }

    // At O3, inject AggressiveInstCombine after the standard peephole passes
    // to catch multi-instruction patterns (e.g. truncation sequences, popcount
    // idioms) that regular InstCombine does not handle.
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerPeepholeEPCallback([](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) {
            FPM.addPass(llvm::AggressiveInstCombinePass());
        });
    }

    // At O2+, inject SimpleLoopUnswitchPass late in the loop optimizer to hoist
    // loop-invariant conditional branches out of loops.  This transforms a loop
    // with an invariant if/else into two specialized loops — one for the true
    // path and one for the false path — eliminating a branch per iteration
    // and enabling further loop-specific optimizations on each clone.
    // NonTrivial=true allows cloning of the loop for non-trivial unswitching
    // (i.e. the condition has side effects in the loop body).
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerLateLoopOptimizationsEPCallback([](llvm::LoopPassManager& LPM, llvm::OptimizationLevel) {
            LPM.addPass(llvm::SimpleLoopUnswitchPass(/*NonTrivial=*/true));
        });
    }

    // At O2+, register additional loop cleanup passes at the end of the loop
    // optimizer.  These run after all loop transformations (rotation, LICM,
    // unrolling, vectorization) have completed:
    //   LoopIdiomRecognizePass: detects loop patterns that implement memset,
    //     memcpy, or similar operations and replaces them with optimized
    //     library calls — a major performance win for array initialization
    //     and copy loops.
    //   IndVarSimplifyPass: canonicalizes induction variables (e.g. narrows
    //     types, removes sign-extensions, replaces derived IVs with a single
    //     canonical counter) to simplify trip-count computation and enable
    //     more aggressive vectorization and unrolling.
    //   LoopDeletionPass: removes loops that are provably dead (no observable
    //     side effects, results unused) — eliminates wasted cycles in code
    //     where earlier optimizations eliminated the loop's consumers.
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerLoopOptimizerEndEPCallback([](llvm::LoopPassManager& LPM, llvm::OptimizationLevel) {
            LPM.addPass(llvm::LoopIdiomRecognizePass());
            LPM.addPass(llvm::IndVarSimplifyPass());
            LPM.addPass(llvm::LoopDeletionPass());
        });
    }

    // At O2+, inject CorrelatedValuePropagation and DeadStoreElimination
    // late in the scalar optimizer.  CVP uses value-range information from
    // branch conditions to sharpen comparisons, convert signed operations
    // to unsigned (enabling more shifts/masks), and eliminate provably-dead
    // branches.  DSE removes stores whose values are never read (e.g.
    // overwritten before use, or stored to dead allocations).
    // BDCE (Bit-tracking Dead Code Elimination) identifies and eliminates
    // instructions whose results have unused bits, shrinking live ranges
    // and enabling further simplification.
    // TailCallElimPass converts tail-recursive calls into loops, eliminating
    // stack growth for recursive algorithms (e.g. list traversals, tree walks).
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerScalarOptimizerLateEPCallback([](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) {
            FPM.addPass(llvm::CorrelatedValuePropagationPass());
            FPM.addPass(llvm::BDCEPass());
            FPM.addPass(llvm::ADCEPass());
            FPM.addPass(llvm::DSEPass());
            FPM.addPass(llvm::MemCpyOptPass());
            FPM.addPass(llvm::Float2IntPass());
            FPM.addPass(llvm::TailCallElimPass());
            // MergeICmps merges chains of integer comparisons (e.g. struct
            // field-by-field equality) into efficient memcmp/bcmp calls,
            // which the back-end can lower to vectorized SIMD comparisons.
            FPM.addPass(llvm::MergeICmpsPass());
            // MergedLoadStoreMotion hoists loads and sinks stores across
            // diamond-shaped (if/else) control flow, hiding memory latency
            // and reducing static code size.
            FPM.addPass(llvm::MergedLoadStoreMotionPass());
            // StraightLineStrengthReduce detects and reuses redundant
            // address/offset computations in straight-line code (e.g.
            // base+i*stride patterns), replacing expensive multiplies with
            // incremental additions.
            FPM.addPass(llvm::StraightLineStrengthReducePass());
            // NaryReassociate reassociates n-ary add/mul expressions to
            // maximize reuse of existing sub-expressions (e.g. (a+b)+2
            // when (a+b) is already available → reuse + add 2).
            FPM.addPass(llvm::NaryReassociatePass());
        });
    }

    // At O3, inject ConstraintElimination and JumpThreading late in the
    // scalar optimizer.  ConstraintElimination removes redundant comparisons
    // using range constraints (e.g. after a bounds check, subsequent in-range
    // accesses skip the check).  JumpThreading threads branches through
    // basic blocks with known conditions, reducing branch mispredictions.
    // DivRemPairsPass hoists and decomposes division/remainder pairs — when
    // both x/y and x%y appear in the same function, the backend can reuse
    // the quotient from a single hardware division for both results.
    // SpeculativeExecutionPass speculatively executes instructions from a
    // branch to reduce branch misprediction cost on wide-issue CPUs.
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerScalarOptimizerLateEPCallback([](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) {
            FPM.addPass(llvm::ConstraintEliminationPass());
            FPM.addPass(llvm::JumpThreadingPass());
            FPM.addPass(llvm::DivRemPairsPass());
            FPM.addPass(llvm::SpeculativeExecutionPass());
        });
    }

    // At O2+, register VectorCombinePass and LoopSinkPass as one of the last
    // function-level optimizations.
    //   VectorCombinePass: optimizes scalar/vector interactions using target
    //     cost models — converts redundant scalar extract/insert sequences
    //     into direct vector operations and eliminates unnecessary shuffles.
    //   LoopSinkPass: sinks loop-invariant code back into the loop body
    //     when the instruction is only used on a cold path, reducing register
    //     pressure on the hot path.  Uses block frequency heuristics and can
    //     optionally benefit from profile data when available.
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerOptimizerLastEPCallback(
#if LLVM_VERSION_MAJOR >= 21
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel, llvm::ThinOrFullLTOPhase) {
#else
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel) {
#endif
            llvm::FunctionPassManager FPM;
            FPM.addPass(llvm::VectorCombinePass());
            FPM.addPass(llvm::LoopSinkPass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
        });
    }

    // At O3, run HotColdSplitting to outline cold code regions (error
    // handlers, assertion failures, rarely-taken branches) into separate
    // functions.  This improves I-cache density on the hot path and
    // enables better register allocation and instruction scheduling in
    // the remaining hot code.
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerOptimizerLastEPCallback(
#if LLVM_VERSION_MAJOR >= 21
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel, llvm::ThinOrFullLTOPhase) {
#else
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel) {
#endif
            MPM.addPass(llvm::HotColdSplittingPass());
        });
    }

    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::OptimizationLevel newPMLevel;
    switch (optimizationLevel) {
    case OptimizationLevel::O1:
        newPMLevel = llvm::OptimizationLevel::O1;
        break;
    case OptimizationLevel::O3:
        newPMLevel = llvm::OptimizationLevel::O3;
        break;
    default:
        newPMLevel = llvm::OptimizationLevel::O2;
        break;
    }
    llvm::ModulePassManager MPM;
    if (lto_) {
        // LTO mode: use the pre-link pipeline that runs lighter intra-procedural
        // passes and defers heavy IPO (inlining, IPSCCP, GlobalDCE) to the
        // linker.  This avoids double-optimizing the bitcode — once here and
        // again during the link-time optimization pass in the linker.
        if (verbose_) {
            std::cout << "    Building LTO pre-link default pipeline..." << std::endl;
        }
        MPM = PB.buildLTOPreLinkDefaultPipeline(newPMLevel);
    } else {
        if (verbose_) {
            const char* levelStr = "O2";
            if (optimizationLevel == OptimizationLevel::O1) levelStr = "O1";
            else if (optimizationLevel == OptimizationLevel::O3) levelStr = "O3";
            std::cout << "    Building per-module default pipeline at " << levelStr << "..." << std::endl;
        }
        MPM = PB.buildPerModuleDefaultPipeline(newPMLevel);
    }
    // At O2+, append GlobalOptPass after the standard pipeline to constant-fold
    // and internalize global variables, propagate initial values, and eliminate
    // globals that are only stored but never read.  This cleans up patterns the
    // default pipeline leaves behind (e.g. globals used only in main).
    if (optimizationLevel >= OptimizationLevel::O2 && !lto_) {
        if (verbose_) {
            std::cout << "    Adding GlobalOpt + GlobalDCE passes..." << std::endl;
        }
        MPM.addPass(llvm::GlobalOptPass());
        MPM.addPass(llvm::GlobalDCEPass());
    }
    if (verbose_) {
        std::cout << "    Running LLVM module pass pipeline..." << std::endl;
    }
    // Pre-pipeline srem→urem conversion: run BEFORE the LLVM pipeline so the
    // loop vectorizer (which runs as part of the pipeline) sees urem instead
    // of srem.  The vectorizer's cost model treats urem-by-constant as cheaper
    // than srem-by-constant (urem avoids the signed-correction fixup), enabling
    // vectorization of inner loops like `sum += ((i^j) + k) % 37`.
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
        for (auto& func : *module) {
            superopt::convertSRemToURem(func);
        }
    }
    // Pre-pipeline HGOE loop annotation: set target-optimal unroll count,
    // interleave count, and vector width on loops BEFORE the LLVM pipeline
    // runs.  This ensures the unroller and vectorizer respect the target
    // CPU's resource constraints (register count, I-cache size, pipeline
    // depth) instead of using generic heuristics that over-unroll for CPUs
    // with expensive modulo/division sequences.
    //
    // NOTE: Disabled because LLVM's O3 pipeline already has excellent
    // unroll heuristics with register pressure modeling.  Our pre-pipeline
    // metadata was being overridden by LLVM's aggressive multi-pass
    // unrolling anyway, and when it wasn't overridden, it often led to
    // suboptimal results because the cost model runs on pre-lowered IR
    // where operations like srem/sdiv appear as single instructions but
    // expand to 5-6 µops during selection.  The post-pipeline HGOE
    // (softwarePipelineLoops) runs on fully-optimized IR and gives more
    // accurate annotations for loops that escape LLVM's unroller.
    MPM.run(*module, MAM);
    if (verbose_) {
        std::cout << "    LLVM pass pipeline complete" << std::endl;
    }

    // Bounded recursive inlining: replicate GCC's deep recursive inlining
    // by manually inlining self-recursive calls a fixed number of levels.
    // LLVM's inliner refuses to inline self-recursive calls (to prevent
    // infinite expansion), so after the standard pipeline has converted one
    // branch to a tail-call loop, we manually inline the remaining recursive
    // call.  Each level doubles the work done per actual function call,
    // reducing call overhead by ~2^depth.  3 levels gives ~8x fewer calls,
    // matching GCC -O3's behavior for naive recursive algorithms like fib.
    if (optimizationLevel >= OptimizationLevel::O3) {
        static constexpr unsigned kRecursiveInlineDepth = 3;
        // Conservative size limit: the function BEFORE inlining must be
        // small enough that inlining won't create a huge function.
        // After inlining, each copy roughly doubles the size. So we limit
        // the pre-inline size to 200 instructions (~200 * 2^3 = 1600 max).
        static constexpr unsigned kMaxPreInlineSize = 200;
        for (auto& F : *module) {
            if (F.isDeclaration() || F.getName() == "main") continue;
            unsigned preSize = F.getInstructionCount();
            if (preSize > kMaxPreInlineSize) continue;

            // Only inline functions that still contain a self-call.
            bool hasSelfCall = false;
            for (auto& BB : F) {
                for (auto& I : BB) {
                    if (auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
                        if (CB->getCalledFunction() == &F) {
                            hasSelfCall = true;
                            break;
                        }
                    }
                }
                if (hasSelfCall) break;
            }
            if (!hasSelfCall) continue;

            for (unsigned level = 0; level < kRecursiveInlineDepth; ++level) {
                llvm::SmallVector<llvm::CallBase*, 4> selfCalls;
                for (auto& BB : F) {
                    for (auto& I : BB) {
                        if (auto* CB = llvm::dyn_cast<llvm::CallBase>(&I)) {
                            if (CB->getCalledFunction() == &F) {
                                selfCalls.push_back(CB);
                            }
                        }
                    }
                }
                if (selfCalls.empty()) break;

                // Only inline the first self-call per level to avoid
                // exponential blowup (fib has two calls: fib(n-1)+fib(n-2),
                // but LLVM's TCO eliminates one, leaving exactly one).
                unsigned inlined = 0;
                for (auto* CB : selfCalls) {
                    if (F.getInstructionCount() > preSize * 8) break;
                    llvm::InlineFunctionInfo IFI;
                    auto result = llvm::InlineFunction(*CB, IFI);
                    if (result.isSuccess()) ++inlined;
                }
                if (inlined == 0) break;
            }
        }
        if (verbose_) {
            std::cout << "    Bounded recursive inlining complete" << std::endl;
        }
    }

    // Superoptimizer: run after the standard LLVM pipeline to catch patterns
    // that individual passes miss.  The superoptimizer performs:
    //   - Idiom recognition (rotate, abs, min/max, popcount)
    //   - Algebraic identity simplification on LLVM IR
    //   - Branch-to-select conversion for simple diamond CFGs
    //   - Enumerative synthesis of cheaper instruction sequences
    // Enabled at O2+ unless explicitly disabled with -fno-superopt.
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
        if (verbose_) {
            std::cout << "    Running superoptimizer (idiom recognition, algebraic simplification, synthesis)..." << std::endl;
        }
        superopt::SuperoptimizerConfig superConfig;
        // At O3, enable more aggressive synthesis
        if (optimizationLevel >= OptimizationLevel::O3) {
            superConfig.synthesis.maxInstructions = 5;
            superConfig.synthesis.costThreshold = 0.9;
        }
        auto superStats = superopt::superoptimizeModule(*module, superConfig);
        if (verbose_) {
            unsigned totalOpts = superStats.idiomsReplaced + superStats.synthReplacements +
                                 superStats.algebraicSimplified + superStats.branchesSimplified +
                                 superStats.deadCodeEliminated;
            std::cout << "    Superoptimizer complete: "
                      << superStats.idiomsReplaced << " idioms replaced, "
                      << superStats.algebraicSimplified << " algebraic simplifications, "
                      << superStats.synthReplacements << " synthesis replacements, "
                      << superStats.branchesSimplified << " branches simplified, "
                      << superStats.deadCodeEliminated << " dead instructions eliminated"
                      << " (" << totalOpts << " total optimizations)" << std::endl;
        }
    } else if (verbose_ && optimizationLevel >= OptimizationLevel::O2 && !enableSuperopt_) {
        std::cout << "    Superoptimizer disabled (-fno-superopt)" << std::endl;
    }

    // Hardware Graph Optimization Engine: run after the superoptimizer when
    // -march or -mtune is explicitly provided.  The HGOE models the target
    // CPU as a directed graph of execution resources, maps the compiled
    // program onto that graph, and applies hardware-aware transformations
    // (FMA generation, prefetch insertion, branch layout optimisation).
    // When neither flag is set, this is a complete no-op.
    if (enableHGOE_ && optimizationLevel >= OptimizationLevel::O2) {
        if (verbose_) {
            std::cout << "    Running Hardware Graph Optimization Engine";
            if (!marchCpu_.empty()) std::cout << " (march=" << marchCpu_ << ")";
            if (!mtuneCpu_.empty()) std::cout << " (mtune=" << mtuneCpu_ << ")";
            std::cout << "..." << std::endl;
        }
        hgoe::HGOEConfig hgoeConfig;
        hgoeConfig.marchCpu = marchCpu_;
        hgoeConfig.mtuneCpu = mtuneCpu_;
        // Disable loop annotation when LTO is active — the LTO linker runs
        // its own loop optimizer and forced unroll/vectorize metadata causes
        // the LTO pipeline to spend excessive time or hang.
        hgoeConfig.enableLoopAnnotation = !lto_;
        auto hgoeStats = hgoe::optimizeModule(*module, hgoeConfig);
        if (verbose_) {
            if (hgoeStats.activated) {
                std::cout << "    HGOE complete: arch=" << hgoeStats.resolvedArch
                          << ", " << hgoeStats.functionsOptimized << " functions optimized"
                          << std::endl;
            } else {
                std::cout << "    HGOE not activated (no explicit -march/-mtune)" << std::endl;
            }
        }
    }
}

void CodeGenerator::optimizeFunction(llvm::Function* func) {
    // Per-function optimization for targeted optimization of individual functions.
    // This allows selectively optimizing specific functions (e.g., hot functions
    // identified at compile time, or OPTMAX-annotated functions) without running
    // the full module-wide optimization pipeline.
    llvm::legacy::FunctionPassManager fpm(module.get());

    fpm.add(llvm::createSROAPass());
    fpm.add(llvm::createEarlyCSEPass());
    fpm.add(llvm::createPromoteMemoryToRegisterPass());
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createReassociatePass());
    fpm.add(llvm::createGVNPass());
    fpm.add(llvm::createCFGSimplificationPass());
    fpm.add(llvm::createDeadCodeEliminationPass());

    fpm.doInitialization();
    fpm.run(*func);
    fpm.doFinalization();
}


void CodeGenerator::optimizeOptMaxFunctions() {
    // Phase 0: Apply aggressive function attributes to OPTMAX functions.
    // - nounwind: guaranteed no exceptions (enables tail call, smaller EH tables)
    // - optsize is NOT set (we want max speed, not size)
    // alwaysinline threshold is intentionally low: force-inlining large
    // functions into call sites with many other inlined functions inflates
    // code size and causes I-cache pressure.  The inliner already handles
    // profitable inlining decisions via inlinehint + cost model.
    static constexpr unsigned kAlwaysInlineThreshold = 15; // instruction count
    for (auto& func : module->functions()) {
        if (!func.isDeclaration() && optMaxFunctions.count(std::string(func.getName()))) {
            func.addFnAttr(llvm::Attribute::NoUnwind);
            // Mark small OPTMAX helpers as always-inline candidates
            if (func.getInstructionCount() < kAlwaysInlineThreshold) {
                func.addFnAttr(llvm::Attribute::AlwaysInline);
            }
        }
    }

    llvm::legacy::FunctionPassManager fpm(module.get());

    // Phase 1: Early canonicalization
    fpm.add(llvm::createSROAPass());
    fpm.add(llvm::createEarlyCSEPass(/*UseMemorySSA=*/true));
    fpm.add(llvm::createPromoteMemoryToRegisterPass());
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createReassociatePass());
    fpm.add(llvm::createGVNPass());
    fpm.add(llvm::createCFGSimplificationPass());
    fpm.add(llvm::createDeadCodeEliminationPass());
    // Phase 2: Loop optimizations (polyhedral-style)
    // Standard LLVM ordering: LoopSimplify normalises loop structure first
    // (adds pre-header, dedicated exit blocks) which is required before
    // LoopRotate can work.  LoopRotate then puts the back-edge condition at
    // the bottom, enabling LICM to hoist invariants from the loop header.
    // LoopInstSimplify, LoopDataPrefetch, LoopStrengthReduce, and LoopUnroll
    // all benefit from the canonical form produced by the earlier passes.
    fpm.add(llvm::createLoopSimplifyPass());
#if LLVM_VERSION_MAJOR < 19
    fpm.add(llvm::createLoopRotatePass());
#endif
    fpm.add(llvm::createLICMPass());
    fpm.add(llvm::createInstSimplifyLegacyPass()); // simplify instructions in loop bodies
    fpm.add(llvm::createLoopDataPrefetchPass());
    fpm.add(llvm::createLoopStrengthReducePass());
    fpm.add(llvm::createLoopUnrollPass());
    // Phase 2.5: Post-loop cleanup.  Loop strength reduction, unrolling,
    // and LICM can expose redundancies and dead code.  A lightweight
    // CFG simplification + DCE pass is sufficient here; the heavier GVN
    // and InstCombine passes are already in Phase 4 below and the full
    // pipeline runs 3× per function, so duplicating them here only adds
    // compile-time without improving generated code quality.
    fpm.add(llvm::createCFGSimplificationPass());
    fpm.add(llvm::createDeadCodeEliminationPass());
    // Phase 3: Post-loop optimizations
#if LLVM_VERSION_MAJOR < 18
    fpm.add(llvm::createMergedLoadStoreMotionPass());
#endif
    fpm.add(llvm::createSinkingPass());
    fpm.add(llvm::createStraightLineStrengthReducePass());
    fpm.add(llvm::createNaryReassociatePass());
    fpm.add(llvm::createTailCallEliminationPass());
    fpm.add(llvm::createConstantHoistingPass());
    fpm.add(llvm::createSeparateConstOffsetFromGEPPass());
    fpm.add(llvm::createSpeculativeExecutionPass());
    // PartiallyInlineLibCalls replaces calls to math library functions
    // (sqrt, etc.) with their inline fast-path + slow-path branch, avoiding
    // the overhead of a full function call when the fast path applies.
    fpm.add(llvm::createPartiallyInlineLibCallsPass());
    fpm.add(llvm::createFlattenCFGPass());
    // Phase 4: Final cleanup — InstSimplify, InstCombine, CFGSimplification,
    // and DCE remove dead code and simplify patterns exposed by loop and
    // control-flow transformations above.
    fpm.add(llvm::createInstSimplifyLegacyPass()); // lightweight complement to instcombine
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createCFGSimplificationPass());
    fpm.add(llvm::createDeadCodeEliminationPass());

    fpm.doInitialization();
    for (auto& func : module->functions()) {
        if (!func.isDeclaration() && optMaxFunctions.count(std::string(func.getName()))) {
            // OPTMAX runs the aggressive pass stack three times to maximize optimization.
            // Each iteration can expose new patterns for subsequent passes to simplify.
            // Three iterations is the sweet spot: the first pass does heavy lifting,
            // the second catches patterns exposed by loop/strength-reduce transforms,
            // and the third cleans up residuals.  Beyond three, passes reach a fixed
            // point and additional iterations produce no further changes.
            constexpr int optMaxIterations = 3;
            for (int i = 0; i < optMaxIterations; ++i) {
                fpm.run(func);
            }
        }
    }
    fpm.doFinalization();
}

void CodeGenerator::writeObjectFile(const std::string& filename) {
    // Initialize only native target
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
    module->setTargetTriple(targetTriple);
#else
    module->setTargetTriple(targetTripleStr);
#endif

    auto targetMachine = createTargetMachine();
    if (!targetMachine) {
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "Failed to create target machine"});
    }

    module->setDataLayout(targetMachine->createDataLayout());

    std::error_code EC;
    llvm::raw_fd_ostream dest(filename, EC, llvm::sys::fs::OF_None);

    if (EC) {
        throw DiagnosticError(Diagnostic{
            DiagnosticSeverity::Error, {"", 0, 0}, "Could not open file '" + filename + "': " + EC.message()});
    }

    llvm::legacy::PassManager pass;
#if LLVM_VERSION_MAJOR >= 18
    auto fileType = llvm::CodeGenFileType::ObjectFile;
#else
    auto fileType = llvm::CGFT_ObjectFile;
#endif

    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        throw DiagnosticError(
            Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, "TargetMachine can't emit a file of this type"});
    }

    pass.run(*module);
    dest.flush();
    // Detect errors during close (e.g. I/O errors that occurred during write).
    if (dest.has_error()) {
        std::string errMsg = "Error writing object file '" + filename + "': " + dest.error().message();
        dest.clear_error();
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, errMsg});
    }
}

void CodeGenerator::writeBitcodeFile(const std::string& filename) {
    // Initialize native target so the data layout can be set correctly.
    llvm::InitializeNativeTarget();

    std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
#if LLVM_VERSION_MAJOR >= 19
    llvm::Triple targetTriple(targetTripleStr);
    module->setTargetTriple(targetTriple);
#else
    module->setTargetTriple(targetTripleStr);
#endif

    auto targetMachine = createTargetMachine();
    if (targetMachine) {
        module->setDataLayout(targetMachine->createDataLayout());
    }

    std::error_code EC;
    llvm::raw_fd_ostream dest(filename, EC, llvm::sys::fs::OF_None);
    if (EC) {
        throw DiagnosticError(Diagnostic{
            DiagnosticSeverity::Error, {"", 0, 0}, "Could not open file '" + filename + "': " + EC.message()});
    }

    llvm::WriteBitcodeToFile(*module, dest);
    dest.flush();
    if (dest.has_error()) {
        std::string errMsg = "Error writing bitcode file '" + filename + "': " + dest.error().message();
        dest.clear_error();
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, errMsg});
    }
}

// ---------------------------------------------------------------------------
// JIT baseline optimization — intra-procedural passes without IPO
// ---------------------------------------------------------------------------

void CodeGenerator::runJITBaselinePasses() {
    // Apply intra-procedural (per-function) optimization passes to improve the
    // baseline IR quality for the adaptive JIT execution pipeline.
    //
    // These passes run WITHOUT inter-procedural optimizations (inlining, IPSCCP,
    // GlobalDCE) that would collapse function call boundaries and prevent the
    // adaptive JIT runtime from hot-patching individual functions.
    //
    // The pass set scales with the user's optimization level:
    //   O0: skip entirely for fastest JIT startup
    //   O1: basic canonicalization (mem2reg, instcombine, GVN, DCE)
    //   O2: O1 + loop optimizations (LICM, strength reduce, unroll)
    //   O3: O2 + aggressive loop passes, nary reassociate, constant hoisting,
    //       multiple iterations for maximum per-function optimization
    //
    // The resulting optimized IR is saved as the "clean bitcode" by the JIT
    // runner and serves as the starting point for both:
    //   - Tier-1 execution: MCJIT machine-code generation
    //   - Tier-2 recompilation: O3+PGO re-optimization of hot functions

    if (optimizationLevel == OptimizationLevel::O0) {
        // At O0, skip all optimization for fastest JIT startup.
        return;
    }

    llvm::legacy::FunctionPassManager fpm(module.get());

    // Phase 1: Memory-to-register promotion and canonicalization.
    // mem2reg is the highest-impact pass: it promotes alloca/load/store
    // patterns emitted by the front-end into SSA values, eliminating most
    // memory operations for scalar variables and dramatically improving
    // downstream analysis and optimization quality.
    fpm.add(llvm::createPromoteMemoryToRegisterPass());
    fpm.add(llvm::createSROAPass());
    fpm.add(llvm::createEarlyCSEPass(/*UseMemorySSA=*/true));
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createInstSimplifyLegacyPass());
    fpm.add(llvm::createReassociatePass());
    fpm.add(llvm::createCFGSimplificationPass());

    // Phase 2: Value numbering and redundancy elimination.
    fpm.add(llvm::createGVNPass());
    fpm.add(llvm::createDeadCodeEliminationPass());

    if (optimizationLevel >= OptimizationLevel::O2) {
        // Phase 3: Loop optimizations (intra-procedural only).
        fpm.add(llvm::createLoopSimplifyPass());
#if LLVM_VERSION_MAJOR < 19
        fpm.add(llvm::createLoopRotatePass());
#endif
        fpm.add(llvm::createLICMPass());
        fpm.add(llvm::createInstSimplifyLegacyPass());
        fpm.add(llvm::createLoopStrengthReducePass());
        fpm.add(llvm::createLoopUnrollPass());
        fpm.add(llvm::createLoopDataPrefetchPass());
        // MergedLoadStoreMotion merges load/store pairs across diamond-shaped
        // branches (if/else).  For long-running programs with array-heavy code
        // this eliminates redundant memory traffic that dominates runtime.
#if LLVM_VERSION_MAJOR < 18
        fpm.add(llvm::createMergedLoadStoreMotionPass());
#endif
        fpm.add(llvm::createSinkingPass());
    }

    if (optimizationLevel >= OptimizationLevel::O3) {
        // Phase 3b: Aggressive O3 per-function passes.
        // These provide significant speedups for compute-heavy code without
        // crossing function boundaries (no IPO).
        fpm.add(llvm::createStraightLineStrengthReducePass());
        fpm.add(llvm::createNaryReassociatePass());
        fpm.add(llvm::createConstantHoistingPass());
        // SeparateConstOffsetFromGEP extracts constant offsets from GEP
        // (GetElementPtr) address computations, enabling the backend to use
        // base+offset addressing modes and share common base addresses across
        // multiple array accesses in tight loops.
        fpm.add(llvm::createSeparateConstOffsetFromGEPPass());
        // SpeculativeExecution hoists cheap instructions (comparisons, shifts,
        // selects) above branches to hide latency on wide-issue CPUs.
        // Critical for compute-heavy inner loops where branch mispredictions
        // would otherwise stall the pipeline.
        fpm.add(llvm::createSpeculativeExecutionPass());
        fpm.add(llvm::createPartiallyInlineLibCallsPass());
        fpm.add(llvm::createFlattenCFGPass());
    }

    // Phase 4: Tail-call elimination converts tail-recursive calls to loops.
    fpm.add(llvm::createTailCallEliminationPass());

    // Phase 5: Final cleanup to remove instructions exposed by earlier passes.
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createCFGSimplificationPass());
    fpm.add(llvm::createDeadCodeEliminationPass());

    fpm.doInitialization();

    // At O3, run the pass pipeline multiple times: each iteration can expose
    // new optimization opportunities (e.g., strength reduction creates patterns
    // that instcombine can simplify further).  Two iterations is the sweet spot
    // for per-function JIT optimization without excessive compile time.
    int iterations = (optimizationLevel >= OptimizationLevel::O3) ? 2 : 1;
    for (auto& func : module->functions()) {
        if (!func.isDeclaration()) {
            for (int i = 0; i < iterations; ++i) {
                fpm.run(func);
            }
        }
    }
    fpm.doFinalization();
}

} // namespace omscript
