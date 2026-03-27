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
#include <llvm/Support/CommandLine.h>
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
#include <llvm/Transforms/IPO/ArgumentPromotion.h>
#include <llvm/Transforms/IPO/ConstantMerge.h>
#include <llvm/Transforms/IPO/DeadArgumentElimination.h>
#include <llvm/Transforms/IPO/FunctionAttrs.h>
#include <llvm/Transforms/IPO/InferFunctionAttrs.h>
#include <llvm/Transforms/IPO/PartialInlining.h>
#include <llvm/Transforms/IPO/GlobalSplit.h>
#include <llvm/Transforms/IPO/StripDeadPrototypes.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/CorrelatedValuePropagation.h>
#include <llvm/Transforms/Scalar/ConstraintElimination.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/FlattenCFG.h>
#include <llvm/Transforms/Scalar/Float2Int.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/IndVarSimplify.h>
#include <llvm/Transforms/Scalar/LoopDeletion.h>
#include <llvm/Transforms/Scalar/DFAJumpThreading.h>
#include <llvm/Transforms/Scalar/InductiveRangeCheckElimination.h>
#include <llvm/Transforms/Scalar/InferAlignment.h>
#include <llvm/Transforms/Scalar/AlignmentFromAssumptions.h>
#include <llvm/Transforms/Scalar/LoopBoundSplit.h>
#include <llvm/Transforms/Scalar/LoopDistribute.h>
#include <llvm/Transforms/Scalar/LoopFlatten.h>
#include <llvm/Transforms/Scalar/LoopFuse.h>
#include <llvm/Transforms/Scalar/LoopIdiomRecognize.h>
#include <llvm/Transforms/Scalar/LoopInstSimplify.h>
#include <llvm/Transforms/Scalar/LoopInterchange.h>
#include <llvm/Transforms/Scalar/LoopLoadElimination.h>
#include <llvm/Transforms/Scalar/LoopDataPrefetch.h>
#include <llvm/Transforms/Scalar/LoopRotation.h>
#include <llvm/Transforms/Scalar/LoopSink.h>
#include <llvm/Transforms/Scalar/LoopUnrollAndJamPass.h>
#include <llvm/Transforms/Scalar/LoopVersioningLICM.h>
#include <llvm/Transforms/Scalar/LoopPredication.h>
#include <llvm/Transforms/Scalar/LoopSimplifyCFG.h>
#include <llvm/Transforms/Scalar/CallSiteSplitting.h>
#include <llvm/Transforms/Scalar/Sink.h>
#include <llvm/Transforms/Scalar/ConstantHoisting.h>
#include <llvm/Transforms/Scalar/SeparateConstOffsetFromGEP.h>
#include <llvm/Transforms/Scalar/PartiallyInlineLibCalls.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
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
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/Utils/CanonicalizeFreezeInLoops.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/LCSSA.h>
#include <llvm/Transforms/Utils/LibCallsShrinkWrap.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>
#include <llvm/Transforms/Scalar/InstSimplifyPass.h>
#include <llvm/Transforms/Scalar/LICM.h>
#include <llvm/Transforms/Vectorize/VectorCombine.h>
#include <llvm/Transforms/Vectorize/LoadStoreVectorizer.h>
#include <llvm/Transforms/Vectorize/SLPVectorizer.h>
#include <llvm/Transforms/Scalar/NewGVN.h>
#include <llvm/Transforms/Scalar/Scalarizer.h>
#include <llvm/Transforms/IPO/CalledValuePropagation.h>
#include <llvm/Transforms/IPO/Attributor.h>
#include <llvm/Transforms/IPO/SCCP.h>
#include <llvm/Transforms/Scalar/SCCP.h>
#include <llvm/Transforms/Scalar/LoopReroll.h>
#include <llvm/Transforms/Scalar/GuardWidening.h>
#include <llvm/Transforms/Scalar/TLSVariableHoist.h>
#include <llvm/Transforms/IPO/MergeFunctions.h>
#include <llvm/Transforms/Scalar/LowerExpectIntrinsic.h>
#include <llvm/Transforms/Scalar/WarnMissedTransforms.h>
#include <llvm/Transforms/Scalar/LowerConstantIntrinsics.h>
#include <optional>
#include <stdexcept>

namespace omscript {

// Aggressive SimplifyCFG options used throughout the optimization pipeline.
// Converts if-else chains to selects, hoists/sinks common instructions,
// converts switches to lookup tables, and speculatively simplifies blocks.
// bonusInstThreshold=6 allows up to 6 extra instructions to be speculated
// when converting branches to selects — enough for cascading-if classify()
// patterns and multi-condition guards without over-speculating complex branches.
// convertSwitchRangeToICmp: converts switch statements with contiguous case
// ranges to icmp+branch sequences, which are more efficient on modern OoO CPUs
// with branch prediction than the switch dispatch table.
static constexpr int kCFGSpeculationBonus = 6;
static llvm::SimplifyCFGOptions aggressiveCFGOpts() {
    return llvm::SimplifyCFGOptions()
        .convertSwitchToLookupTable(true)
        .convertSwitchRangeToICmp(true)
        .hoistCommonInsts(true)
        .sinkCommonInsts(true)
        .speculateBlocks(true)
        .forwardSwitchCondToPhi(true)
        .bonusInstThreshold(kCFGSpeculationBonus);
}

// Hyperblock-aggressive SimplifyCFG options for superblock/hyperblock formation.
// Uses a much higher bonusInstThreshold (12) to convert more branches to
// predicated (select) instructions, creating larger basic blocks that give
// the scheduler and register allocator more freedom.  This is the IR-level
// equivalent of hyperblock formation in traditional compilers.
// needCanonicalLoops(false) allows SimplifyCFG to break canonical loop form
// when it enables more aggressive if-conversion — the loop canonicalization
// passes that run later will restore the form if needed.
static constexpr int kHyperblockSpeculationBonus = 12;
static llvm::SimplifyCFGOptions hyperblockCFGOpts() {
    return llvm::SimplifyCFGOptions()
        .convertSwitchToLookupTable(true)
        .convertSwitchRangeToICmp(true)
        .hoistCommonInsts(true)
        .sinkCommonInsts(true)
        .speculateBlocks(true)
        .forwardSwitchCondToPhi(true)
        .needCanonicalLoops(false)
        .bonusInstThreshold(kHyperblockSpeculationBonus);
}

void CodeGenerator::resolveTargetCPU(std::string& cpu, std::string& features) const {
    const bool isNative = marchCpu_.empty() || marchCpu_ == "native";
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
    const std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
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
    const std::optional<llvm::Reloc::Model> RM = usePIC_ ? llvm::Reloc::PIC_ : llvm::Reloc::Static;

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
    const std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
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
        PTO.InlinerThreshold = 1000; // aggressive inlining for maximum IPC
        // ForgetAllSCEVInLoopUnroll forces SCEV to recompute trip counts after
        // each unrolling step.  Without this, stale trip-count information from
        // before unrolling can cause the unroller to make suboptimal decisions
        // on subsequent iterations (e.g. under-unrolling a loop whose trip
        // count became statically known after partial unrolling).
        PTO.ForgetAllSCEVInLoopUnroll = true;
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

    // At O2+, register InferFunctionAttrs early in the pipeline to annotate
    // library functions with known attributes (noalias, nocapture, readonly,
    // etc.) before the inliner and other IPO passes run.  This enables the
    // inliner to make better cost estimates and allows alias analysis to be
    // more precise, unlocking further optimizations downstream.
    // Lowered from O2 to O1 so that library-function attributes (noalias,
    // nocapture, readonly, etc.) are available at all optimization levels,
    // enabling zero-cost abstraction guarantees even at O1.
    if (optimizationLevel >= OptimizationLevel::O1) {
        PB.registerPipelineStartEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/) {
            MPM.addPass(llvm::InferFunctionAttrsPass());
            // LowerExpectIntrinsic converts llvm.expect intrinsics to branch
            // weight metadata early in the pipeline so that all downstream
            // passes (SimplifyCFG, JumpThreading, LoopRotate) can use the
            // likely/unlikely hints for better code layout.
            llvm::FunctionPassManager EarlyFPM;
            EarlyFPM.addPass(llvm::LowerExpectIntrinsicPass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(EarlyFPM)));
        });
    }

    // At O1+, register Reassociate and EarlyCSE early in the function pipeline.
    // Reassociate canonicalizes expression trees (e.g., (a+b)+c → a+(b+c))
    // enabling better CSE and constant folding.  EarlyCSE eliminates trivially
    // redundant computations before the heavier optimization passes run,
    // reducing IR size and compilation time for the downstream pipeline.
    // The PeepholeEP callback runs after EVERY InstCombine invocation in
    // the pipeline.  Keep this extremely lightweight — only EarlyCSE which
    // is cheap and catches redundancies InstCombine creates.  Reassociate
    // is already in the main pipeline and is too expensive to run repeatedly.
    if (optimizationLevel >= OptimizationLevel::O1) {
        const bool useMemSSA = optimizationLevel >= OptimizationLevel::O2;
        PB.registerPeepholeEPCallback([useMemSSA](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            FPM.addPass(llvm::EarlyCSEPass(useMemSSA));
        });
    }

    // At O2+, infer memory attributes (readnone, readonly, argmemonly, etc.)
    // on functions using the full call graph in reverse post-order.  Running
    // this early — before inlining and other IPO passes — gives alias analysis
    // and LICM accurate function-level memory effects, which propagates to
    // more precise results for every downstream function-level optimisation.
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerOptimizerEarlyEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/) {
            MPM.addPass(llvm::ReversePostOrderFunctionAttrsPass());
        });
    }

    // At O2+, register CallSiteSplitting in the scalar optimizer.
    // NOTE: SCCP is already part of the default O2/O3 pipeline — only
    // CallSiteSplitting, which is not in the default pipeline, is added.
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerScalarOptimizerLateEPCallback([](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            FPM.addPass(llvm::CallSiteSplittingPass());
        });
    }

    // ── Auto-parallelization pipeline configuration ─────────────────────
    // OmScript supports automatic loop parallelization via two complementary
    // mechanisms:
    //
    //   1. LLVM Loop Vectorizer: uses !llvm.access.group + parallel_accesses
    //      metadata (attached by codegen_stmt.cpp) to vectorize loops that
    //      are provably parallel.
    //
    //   2. Polly Polyhedral Optimizer: for affine loop nests, Polly performs
    //      high-level transformations (tiling, interchange, fusion) and can
    //      generate OpenMP parallel loops.  We configure Polly's parallelism
    //      options via LLVM's cl::opt mechanism after loading the plugin.
    //
    // The -fparallelize / -fno-parallelize flag controls both mechanisms,
    // and the @parallel / @noparallel function annotations provide per-
    // function control.

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
            // Now that Polly has registered its cl::opt entries, configure
            // automatic parallelization if -fparallelize is active.
            //
            // -polly-parallel: generate OpenMP parallel loops for profitable
            //   outer loops in affine loop nests
            // -polly-vectorizer=stripmine: use Polly's strip-mining vectorizer
            //   for inner loops (complementing LLVM's LoopVectorizer)
            // -polly-run-dce: dead code elimination after Polly transforms
            // -polly-run-inliner: inline small helper functions Polly creates
            // -polly-invariant-load-hoisting: hoist loop-invariant loads for
            //   better data locality
            // -polly-scheduling=dynamic: dynamic OpenMP scheduling for uneven
            //   workloads (more robust than static for user code)
            // -polly-scheduling-chunksize=1: fine-grained chunks for balance
            if (enableParallelize_) {
                const char* pollyArgs[] = {
                    "omsc",
                    "-polly-parallel",
                    "-polly-vectorizer=stripmine",
                    "-polly-run-dce",
                    "-polly-run-inliner",
                    "-polly-invariant-load-hoisting",
                    "-polly-scheduling=dynamic",
                    "-polly-scheduling-chunksize=1",
                };
                llvm::cl::ParseCommandLineOptions(
                    sizeof(pollyArgs) / sizeof(pollyArgs[0]), pollyArgs,
                    "OmScript Polly auto-parallelization\n");
            }
            if (verbose_) {
                std::cout << "    Polly plugin loaded successfully"
                          << (enableParallelize_ ? " (parallel mode)" : "")
                          << std::endl;
            }
        } else {
            llvm::errs() << "omsc: warning: failed to load Polly plugin; "
                            "polyhedral loop optimizations disabled\n";
            llvm::consumeError(pollyPlugin.takeError());
        }
    }
#endif

    // ── Superblock / Hyperblock Formation ───────────────────────────────
    // At O3, add a dedicated superblock/hyperblock formation phase that
    // creates larger basic blocks for better scheduling and register alloc.
    //
    // Superblock formation (trace scheduling): JumpThreading duplicates
    // blocks along hot paths to create single-entry multiple-exit regions,
    // eliminating branches that break instruction-level parallelism.
    //
    // Hyperblock formation (if-conversion): aggressive SimplifyCFG with a
    // high speculation threshold converts diamond-shaped branch patterns
    // into predicated (select) instructions, creating wider basic blocks
    // that give the instruction scheduler and register allocator more
    // freedom.  This is particularly effective for OmScript's pattern-
    // matching style (cascading if-else classify() functions).
    //
    // The pipeline runs:
    //   1. JumpThreading — duplicates blocks to form superblocks
    //   2. DFAJumpThreading — handles state-machine/switch patterns
    //   3. CorrelatedValuePropagation — sharpens value ranges from
    //      conditions duplicated by JumpThreading
    //   4. SpeculativeExecution — hoists cheap ops above remaining branches
    //   5. SimplifyCFG(hyperblock) — converts remaining branches to selects
    //   6. FlattenCFG — collapses nested if-else chains
    //   7. InstCombine + DCE — clean up
    //
    // Registered at OptimizerEarlyEP so it runs after inlining has created
    // the full intra-procedural CFG but before the loop optimizer and
    // vectorizer — giving them larger, straighter basic blocks to work with.
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerOptimizerEarlyEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/) {
            llvm::FunctionPassManager SuperblockFPM;
            // Phase 1: Superblock formation — duplicate blocks along
            // frequently-taken paths to form single-entry traces.
            SuperblockFPM.addPass(llvm::JumpThreadingPass());
            SuperblockFPM.addPass(llvm::DFAJumpThreadingPass());
            // Phase 2: Sharpen value ranges exposed by block duplication.
            SuperblockFPM.addPass(llvm::CorrelatedValuePropagationPass());
            // Phase 3: Hoist cheap instructions above remaining branches.
            SuperblockFPM.addPass(llvm::SpeculativeExecutionPass());
            // Phase 4: Hyperblock formation — convert branches to selects.
            SuperblockFPM.addPass(llvm::SimplifyCFGPass(hyperblockCFGOpts()));
            SuperblockFPM.addPass(llvm::FlattenCFGPass());
            // Phase 5: Cleanup — combine and eliminate dead code.
            SuperblockFPM.addPass(llvm::InstCombinePass());
            SuperblockFPM.addPass(llvm::ADCEPass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(SuperblockFPM)));
        });
    }

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

    // At O3, register ArgumentPromotionPass late in the CGSCC pipeline
    // to promote by-reference (pointer) arguments to by-value when the
    // callee only reads through the pointer.  This eliminates pointer
    // chasing and memory loads at call sites, and the promoted values
    // become SSA registers that downstream passes can optimise freely.
    // Only at O3 because argument promotion can increase register
    // pressure and code size when the promoted values are large.
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerCGSCCOptimizerLateEPCallback(
            [](llvm::CGSCCPassManager& CGPM, llvm::OptimizationLevel /*Level*/) {
            CGPM.addPass(llvm::ArgumentPromotionPass());
            // AttributorCGSCCPass performs context-sensitive, call-graph-aware
            // attribute inference.  Unlike InferFunctionAttrsPass (which only
            // infers library function attributes), Attributor propagates:
            // noalias, nofree, nosync, nounwind, readnone, readonly, willreturn,
            // nocapture, and more across the entire call graph.  This enables
            // downstream passes (LICM, DSE, vectorizer) to make stronger
            // assumptions about function behavior.
            CGPM.addPass(llvm::AttributorCGSCCPass());
        });
    }

    // At O2+ with -floop-optimize, register LoopDistributePass to run just
    // before the vectorizer starts.  Loop distribution splits a loop with
    // multiple independent memory access patterns into separate loops, each
    // touching a smaller working set — this improves cache locality and
    // At O2+, register all pre-vectorization passes in a single callback.
    // This runs LoopDistribute, LoopLoadElim, LoopFuse (if enabled), and
    // then re-canonicalizes loops + promotes reductions to SSA form.
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool loopOpt = enableLoopOptimize_;
        PB.registerVectorizerStartEPCallback(
            [loopOpt](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            if (loopOpt) FPM.addPass(llvm::LoopDistributePass());
            FPM.addPass(llvm::LoopLoadEliminationPass());
            if (loopOpt) FPM.addPass(llvm::LoopFusePass());
            FPM.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
            FPM.addPass(llvm::PromotePass());
            FPM.addPass(llvm::InstCombinePass());
            FPM.addPass(llvm::LoopSimplifyPass());
            FPM.addPass(llvm::LCSSAPass());
            FPM.addPass(llvm::createFunctionToLoopPassAdaptor(llvm::LoopRotatePass()));
            FPM.addPass(llvm::createFunctionToLoopPassAdaptor(
                llvm::LICMPass(llvm::LICMOptions()), /*UseMemorySSA=*/true));
            // Reassociate reorders commutative/associative expressions to
            // expose more CSE opportunities for GVN.
            FPM.addPass(llvm::ReassociatePass());
            FPM.addPass(llvm::GVNPass());
            FPM.addPass(llvm::InstSimplifyPass());
            FPM.addPass(llvm::AlignmentFromAssumptionsPass());
            // CorrelatedValuePropagation uses value-range information from
            // branch conditions to tighten comparisons, convert signed ops
            // to unsigned, and prove non-negativity.  Running it before the
            // vectorizer gives the cost model more precise value ranges,
            // enabling vectorization of loops that would otherwise be rejected
            // due to conservative overflow assumptions.
            FPM.addPass(llvm::CorrelatedValuePropagationPass());
            // SimplifyCFG merges trivially-redundant blocks and eliminates
            // unreachable code before the vectorizer.  Cleaner CFG structure
            // helps the vectorizer's control-flow analysis and reduces the
            // number of scalar epilogue paths it must handle.
            // Use aggressive options: convert if-else chains to selects,
            // hoist/sink common instructions, and convert switches to lookup
            // tables — critical for classify()-style cascading-if patterns.
            FPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
        });
    }

    // At O2+ with -floop-optimize, register LoopInterchangePass at the end of
    // the loop optimizer to interchange nested loops for better cache locality.
    // This transforms loop-nest orders so that the innermost loop accesses
    // memory contiguously (e.g. column-major → row-major), dramatically
    // reducing cache misses for matrix-style code.
    // Lowered from O3 to O2 because matrix-style loops are common even in
    // moderate-optimization builds.
    if (optimizationLevel >= OptimizationLevel::O2 && enableLoopOptimize_) {
        PB.registerLoopOptimizerEndEPCallback(
            [](llvm::LoopPassManager& LPM, llvm::OptimizationLevel /*Level*/) { LPM.addPass(llvm::LoopInterchangePass()); });
    }

    // NOTE: AggressiveInstCombine is already part of the default O2/O3
    // pipeline.  Registering it at PeepholeEP caused it to run after EVERY
    // InstCombine invocation, which is extremely expensive with diminishing
    // returns.  Removed to improve compile time.

    // At O2+, register all extra loop optimizer passes in consolidated callbacks.
    {
        const bool isO3 = optimizationLevel >= OptimizationLevel::O3;
        const bool loopOpt = enableLoopOptimize_;

        if (optimizationLevel >= OptimizationLevel::O2) {
            PB.registerLateLoopOptimizationsEPCallback([isO3, loopOpt](llvm::LoopPassManager& LPM, llvm::OptimizationLevel /*Level*/) {
                LPM.addPass(llvm::SimpleLoopUnswitchPass(/*NonTrivial=*/true));
                // LoopVersioningLICM creates a versioned copy of the loop
                // with runtime alias checks, allowing LICM to hoist loads
                // and stores that may alias.  Lowered from O3-only to O2+
                // because OmScript's noalias semantics make the runtime
                // checks trivially optimizable, benefiting general code.
                if (loopOpt)
                    LPM.addPass(llvm::LoopVersioningLICMPass());
            });
        }

        if (optimizationLevel >= OptimizationLevel::O2) {
            PB.registerLoopOptimizerEndEPCallback([isO3, loopOpt](llvm::LoopPassManager& LPM, llvm::OptimizationLevel /*Level*/) {
                // LoopIdiomRecognize detects loop patterns that implement
                // memset, memcpy, or similar operations and replaces them with
                // optimized library calls.
                LPM.addPass(llvm::LoopIdiomRecognizePass());
                // IndVarSimplify canonicalizes induction variables for
                // vectorization and unrolling.
                LPM.addPass(llvm::IndVarSimplifyPass());
                // LoopDeletion removes provably dead loops.
                LPM.addPass(llvm::LoopDeletionPass());
                LPM.addPass(llvm::LoopInstSimplifyPass());
                LPM.addPass(llvm::LoopSimplifyCFGPass());
                // CanonicalizeFreezeInLoops moves freeze instructions out of
                // loop bodies, allowing SCEV to compute precise trip counts
                // for loops that contain freeze-guarded induction variables.
                // This enables better vectorization and unrolling decisions.
                LPM.addPass(llvm::CanonicalizeFreezeInLoopsPass());
                if (isO3 && loopOpt) {
                    LPM.addPass(llvm::LoopFlattenPass());
                    LPM.addPass(llvm::LoopUnrollAndJamPass(/*OptLevel=*/3));
                }
                if (isO3) {
                    LPM.addPass(llvm::LoopBoundSplitPass());
                    LPM.addPass(llvm::LoopPredicationPass());
                    // LoopReroll recognizes manually-unrolled loop patterns and
                    // "rerolls" them into a single iteration with a larger trip
                    // count.  This enables the vectorizer to handle patterns
                    // that were over-unrolled by earlier passes or written as
                    // unrolled loops in the source code.
                    LPM.addPass(llvm::LoopRerollPass());
                }
            });
        }
    }

    // At O3, inject LoopDataPrefetchPass to insert software prefetch
    // instructions for loops with predictable stride access patterns.
    // This hides memory latency on hot loops that access large arrays by
    // issuing prefetch instructions ahead of the actual loads.  The pass
    // uses TTI (Target Transform Info) to decide prefetch distance and
    // cache line size based on the target CPU.
    // Only enabled at O3 because the cost of extra prefetch instructions
    // can hurt tight loops with good spatial locality (e.g. sequential
    // access to small arrays that fit in L1).
    // Registered at ScalarOptimizerLateEP because LoopDataPrefetchPass is a
    // FunctionPass that internally walks loop nests and analyses SCEVs.
    // At O2+, consolidate all ScalarOptimizerLateEP passes into one callback.
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool isO3 = optimizationLevel >= OptimizationLevel::O3;
        PB.registerScalarOptimizerLateEPCallback([isO3](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel /*Level*/) {
            if (isO3) FPM.addPass(llvm::LoopDataPrefetchPass());
            // CVP uses value-range info to sharpen comparisons and convert
            // signed ops to unsigned.
            FPM.addPass(llvm::CorrelatedValuePropagationPass());
            FPM.addPass(llvm::BDCEPass());
            // ADCE removes dead code more aggressively than DCE.
            FPM.addPass(llvm::ADCEPass());
            // DSE removes stores whose values are never read.
            FPM.addPass(llvm::DSEPass());
            // MemCpyOpt optimizes memory transfer operations.
            FPM.addPass(llvm::MemCpyOptPass());
            // Float2Int converts float operations to integer when possible.
            FPM.addPass(llvm::Float2IntPass());
            // TailCallElim converts tail-recursive calls into loops.
            FPM.addPass(llvm::TailCallElimPass());
            FPM.addPass(llvm::MergeICmpsPass());
            FPM.addPass(llvm::MergedLoadStoreMotionPass());
            FPM.addPass(llvm::StraightLineStrengthReducePass());
            FPM.addPass(llvm::NaryReassociatePass());
            FPM.addPass(llvm::ReassociatePass());
            FPM.addPass(llvm::InferAlignmentPass());
            // SCCP: sparse conditional constant propagation — more powerful
            // than simple constant folding because it tracks value ranges
            // through phi nodes and conditional branches.
            FPM.addPass(llvm::SCCPPass());
            FPM.addPass(llvm::InstCombinePass());
            if (isO3) {
                // LibCallsShrinkWrap wraps math library calls (sqrt, exp2, pow,
                // log, etc.) with fast-path domain checks.  When the argument
                // is known to be in the valid domain (e.g. non-negative for
                // sqrt), the expensive errno-setting/NaN-checking slow path is
                // bypassed entirely.  This is especially beneficial for
                // floating-point benchmarks that call math functions in loops.
                FPM.addPass(llvm::LibCallsShrinkWrapPass());
                FPM.addPass(llvm::ConstraintEliminationPass());
                // JumpThreading threads branches through basic blocks with
                // known conditions, reducing branch mispredictions.
                FPM.addPass(llvm::JumpThreadingPass());
                // DivRemPairs reuses quotient from div for rem.
                FPM.addPass(llvm::DivRemPairsPass());
                FPM.addPass(llvm::SpeculativeExecutionPass());
                FPM.addPass(llvm::IRCEPass());
                FPM.addPass(llvm::DFAJumpThreadingPass());
                FPM.addPass(llvm::ConstantHoistingPass());
                FPM.addPass(llvm::SeparateConstOffsetFromGEPPass());
                FPM.addPass(llvm::PartiallyInlineLibCallsPass());
                FPM.addPass(llvm::SinkingPass());
                // NewGVN is a graph-based GVN that catches redundancies
                // classic GVN misses (e.g. through PHI nodes and memory).
                FPM.addPass(llvm::NewGVNPass());
                // GuardWidening merges multiple guard checks into a single
                // wider check, reducing branching overhead in bounds-checked
                // loops.
                FPM.addPass(llvm::GuardWideningPass());
            }
            // Aggressive SimplifyCFG at the end to convert if-else chains
            // into select instructions or lookup tables after all inlining,
            // jump threading, and CVP have simplified conditions.
            FPM.addPass(llvm::SimplifyCFGPass(aggressiveCFGOpts()));
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
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/) {
            llvm::FunctionPassManager FPM;
            FPM.addPass(llvm::VectorCombinePass());
            FPM.addPass(llvm::LoopSinkPass());
            // LoadStoreVectorizer combines adjacent scalar loads/stores into
            // vector operations.  This is especially beneficial for struct
            // field access patterns and array initialization loops.
            FPM.addPass(llvm::LoadStoreVectorizerPass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(FPM)));
            // CalledValuePropagation propagates known function pointer values
            // through indirect calls, enabling devirtualization and inlining.
            MPM.addPass(llvm::CalledValuePropagationPass());
            // ConstantMerge deduplicates identical global constants across the
            // module (e.g. duplicate string literals, floating-point constants,
            // constant arrays).  Fewer unique constants means smaller data
            // sections, better D-cache utilization, and reduced link time.
            MPM.addPass(llvm::ConstantMergePass());
        });
    }

    // At O3, add a post-vectorizer superblock formation phase.  After the
    // loop vectorizer has transformed loops, the CFG often contains new
    // scalar-epilogue paths, predicated blocks, and vector-select patterns
    // that can be merged into larger basic blocks.  This improves the
    // backend's instruction scheduling and reduces branch overhead in
    // vectorized code.
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/) {
            llvm::FunctionPassManager PostVecFPM;
            PostVecFPM.addPass(llvm::JumpThreadingPass());
            PostVecFPM.addPass(llvm::SpeculativeExecutionPass());
            PostVecFPM.addPass(llvm::SimplifyCFGPass(hyperblockCFGOpts()));
            PostVecFPM.addPass(llvm::InstCombinePass());
            MPM.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(PostVecFPM)));
        });
    }

    // At O3, run dead argument elimination to remove unused function
    // parameters.  After inlining and constant propagation, many functions
    // have parameters that are never used in the function body.  Removing
    // these eliminates register pressure from passing dead values and
    // enables further inlining (smaller call signatures).
    //
    // IPSCCPPass is lowered to O2+ (from O3) because OmScript compiles as a
    // single translation unit — the full call graph is always available, making
    // interprocedural constant propagation cheap and highly effective.  At O2,
    // we enable function specialization (AllowFuncSpec=true by default) so that
    // functions called with constant arguments get specialized clones with the
    // constants folded in.  This is especially beneficial for OmScript programs
    // that use helper functions dispatched from a central switch/case.
    if (optimizationLevel >= OptimizationLevel::O2) {
        const bool isO3 = optimizationLevel >= OptimizationLevel::O3;
        PB.registerOptimizerLastEPCallback(
            [isO3](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/) {
            // IPSCCPPass performs inter-procedural sparse conditional constant
            // propagation — a more powerful version of SCCP that propagates
            // constants, value ranges, and struct field values across function
            // boundaries through the call graph.  This catches constant-folding
            // opportunities that function-local SCCP misses, such as functions
            // always called with a specific constant argument.
            MPM.addPass(llvm::IPSCCPPass());
            MPM.addPass(llvm::DeadArgumentEliminationPass());
            if (isO3) {
                // PartialInlinerPass outlines cold regions (error handling, slow
                // paths) from otherwise-hot functions and inlines only the hot
                // entry region into callers.  This gives the performance benefit
                // of inlining (no call overhead on the fast path) without the
                // code-size cost of inlining the entire function body.
                MPM.addPass(llvm::PartialInlinerPass());
            }
        });
    }

    // At O3, run srem→urem and sdiv→udiv conversion as an OptimizerLast
    // callback BEFORE HotColdSplitting.  The vectorizer (which runs earlier
    // in the pipeline) creates vector srem instructions from the original
    // scalar urem/srem.  At this point the loop counter PHI nodes still
    // carry non-negativity information (nsw/nuw flags, assume intrinsics).
    // After HotColdSplitting, the loop code may be outlined into a separate
    // function where the loop counters become parameters without non-negativity
    // metadata, making the srem→urem proof impossible.
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/) {
            struct SRemToURemPass : public llvm::PassInfoMixin<SRemToURemPass> {
                llvm::PreservedAnalyses run(llvm::Module& M, llvm::ModuleAnalysisManager&) {
                    unsigned total = 0;
                    for (int iter = 0; iter < 4; ++iter) {
                        unsigned iterCount = 0;
                        for (auto& F : M) {
                            iterCount += superopt::inferNonNegativeFlags(F);
                            iterCount += superopt::convertSRemToURem(F);
                            iterCount += superopt::convertSDivToUDiv(F);
                        }
                        total += iterCount;
                        if (iterCount == 0) break;
                    }
                    return total > 0 ? llvm::PreservedAnalyses::none()
                                     : llvm::PreservedAnalyses::all();
                }
            };
            MPM.addPass(SRemToURemPass());
        });
    }

    // At O3, run HotColdSplitting to outline cold code regions (error
    // handlers, assertion failures, rarely-taken branches) into separate
    // functions.  This improves I-cache density on the hot path and
    // enables better register allocation and instruction scheduling in
    // the remaining hot code.
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/) {
            MPM.addPass(llvm::HotColdSplittingPass());
            // StripDeadPrototypes removes unused external function declarations.
            // After inlining, dead argument elimination, and DCE, many declared-
            // but-never-called prototypes remain.  Removing them reduces symbol
            // table pressure and link time.
            MPM.addPass(llvm::StripDeadPrototypesPass());
            // GlobalSplit splits internal globals with multiple fields into
            // separate globals, enabling more precise alias analysis and
            // allowing SROA/mem2reg to promote individual fields to registers.
            MPM.addPass(llvm::GlobalSplitPass());
            // MergeFunctions deduplicates identical function bodies, reducing
            // code size and I-cache pressure.  OmScript's monomorphization of
            // generic functions often produces identical machine code for
            // different type instantiations.
            MPM.addPass(llvm::MergeFunctionsPass());
        });
    }
    // At O2, still strip dead prototypes for cleaner output.
    if (optimizationLevel == OptimizationLevel::O2) {
        PB.registerOptimizerLastEPCallback(
            [](llvm::ModulePassManager& MPM, llvm::OptimizationLevel /*Level*/) {
            MPM.addPass(llvm::StripDeadPrototypesPass());
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
    // Also run in LTO mode: single-TU programs benefit from GlobalOpt even
    // when the LTO pre-link pipeline is used, since the linker's LTO pass
    // may not run GlobalOpt on every module.
    if (optimizationLevel >= OptimizationLevel::O2) {
        if (verbose_) {
            std::cout << "    Adding GlobalOpt + GlobalDCE passes..." << std::endl;
        }
        MPM.addPass(llvm::GlobalOptPass());
        MPM.addPass(llvm::GlobalDCEPass());
        // Final StripDeadPrototypes cleanup: after GlobalDCE removes dead
        // functions, their forward declarations may linger.
        MPM.addPass(llvm::StripDeadPrototypesPass());
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
            superopt::convertSDivToUDiv(func);
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

    // Strip `cold` and `minsize` attributes from user-defined functions.
    // LLVM's HotColdSplitting pass (O3) can misidentify user functions as
    // cold when there's no profile data, leading to minsize codegen that
    // produces much slower code (register spills, no unrolling, etc.).
    // The `cold` attribute was only intended for runtime helpers like exit()
    // and abort() — not for user computation functions.  Stripping these
    // attributes after the pipeline restores full-speed codegen while keeping
    // the splitting pass's benefits for genuinely cold outlined blocks.
    // NOTE: Functions explicitly annotated with @cold by the user are preserved.
    if (optimizationLevel >= OptimizationLevel::O3) {
        for (auto& F : *module) {
            if (F.isDeclaration()) continue;
            if (userAnnotatedColdFunctions_.count(std::string(F.getName()))) continue; // preserve @cold
            F.removeFnAttr(llvm::Attribute::Cold);
            F.removeFnAttr(llvm::Attribute::MinSize);
        }
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
        static constexpr unsigned kRecursiveInlineDepth = 4;
        // Conservative size limit: the function BEFORE inlining must be
        // small enough that inlining won't create a huge function.
        // After inlining, each copy roughly doubles the size. So we limit
        // the pre-inline size to 200 instructions (~200 * 2^3 = 1600 max).
        static constexpr unsigned kMaxPreInlineSize = 200;
        llvm::SmallVector<llvm::Function*, 4> inlinedFuncs;
        for (auto& F : *module) {
            if (F.isDeclaration() || F.getName() == "main") continue;
            const unsigned preSize = F.getInstructionCount();
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
            // Only functions with successful inlining need cleanup
            inlinedFuncs.push_back(&F);
        }
        if (verbose_) {
            std::cout << "    Bounded recursive inlining complete" << std::endl;
        }
        // Post-recursive-inlining cleanup: only run on functions that were
        // actually inlined, not all functions in the module.
        if (!inlinedFuncs.empty()) {
            llvm::legacy::FunctionPassManager postInlineFPM(module.get());
            postInlineFPM.add(llvm::createGVNPass());
            postInlineFPM.add(llvm::createInstructionCombiningPass());
            postInlineFPM.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));
            postInlineFPM.add(llvm::createDeadCodeEliminationPass());
            postInlineFPM.doInitialization();
            for (auto* F : inlinedFuncs) {
                postInlineFPM.run(*F);
            }
            postInlineFPM.doFinalization();
        }
    }

    // ── Function specialization for constant arguments ──────────────────
    // When a non-recursive internal function is called with one or more
    // constant arguments, clone it with those constants inlined.  The
    // specialized version enables further constant folding, dead code
    // elimination, and loop optimizations within the clone.
    //
    // This is a compile-time-only transformation: no runtime dispatch.
    // We limit to small functions (≤200 instructions) and at most 4
    // specializations per function to control code-size growth.
    if (optimizationLevel >= OptimizationLevel::O3) {
        unsigned totalSpecialized = 0;
        constexpr unsigned kMaxSpecsPerFunc = 4;
        constexpr unsigned kMaxFuncSizeForSpec = 200;

        // Collect call sites with constant arguments
        struct SpecCandidate {
            llvm::CallBase* callSite;
            llvm::Function* callee;
            llvm::SmallVector<unsigned, 4> constArgIndices;
        };
        std::vector<SpecCandidate> candidates;

        for (auto& F : *module) {
            if (F.isDeclaration()) continue;
            for (auto& BB : F) {
                for (auto& I : BB) {
                    auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                    if (!CB) continue;
                    auto* callee = CB->getCalledFunction();
                    if (!callee || callee->isDeclaration()) continue;
                    if (callee->hasExactDefinition() == false) continue;
                    if (callee->getInstructionCount() > kMaxFuncSizeForSpec) continue;
                    if (callee == &F) continue;  // skip self-recursion

                    llvm::SmallVector<unsigned, 4> constArgs;
                    for (unsigned i = 0; i < CB->arg_size(); ++i) {
                        if (llvm::isa<llvm::ConstantInt>(CB->getArgOperand(i)) ||
                            llvm::isa<llvm::ConstantFP>(CB->getArgOperand(i))) {
                            constArgs.push_back(i);
                        }
                    }
                    if (!constArgs.empty()) {
                        candidates.push_back({CB, callee, constArgs});
                    }
                }
            }
        }

        // Group candidates by callee and specialize
        std::unordered_map<llvm::Function*, unsigned> specCount;
        for (auto& cand : candidates) {
            if (specCount[cand.callee] >= kMaxSpecsPerFunc) continue;

            // Create the specialized clone
            llvm::ValueToValueMapTy VMap;
            auto* clone = llvm::CloneFunction(cand.callee, VMap);
            if (!clone) continue;

            // Set constant arguments in the clone
            for (unsigned argIdx : cand.constArgIndices) {
                auto argIt = clone->arg_begin();
                std::advance(argIt, argIdx);
                llvm::Argument& cloneArg = *argIt;
                llvm::Value* constVal = cand.callSite->getArgOperand(argIdx);
                cloneArg.replaceAllUsesWith(constVal);
            }

            // Give the clone a unique name and internal linkage
            clone->setName(cand.callee->getName() + ".spec." +
                          std::to_string(specCount[cand.callee]));
            clone->setLinkage(llvm::GlobalValue::InternalLinkage);

            // Redirect the call site to use the specialized version
            cand.callSite->setCalledFunction(clone);

            specCount[cand.callee]++;
            totalSpecialized++;
        }

        if (verbose_ && totalSpecialized > 0) {
            std::cout << "    Function specialization: " << totalSpecialized
                      << " call sites specialized" << std::endl;
        }

        // Run a cleanup pass on specialized functions
        if (totalSpecialized > 0) {
            llvm::legacy::FunctionPassManager specFPM(module.get());
            specFPM.add(llvm::createGVNPass());
            specFPM.add(llvm::createInstructionCombiningPass());
            specFPM.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));
            specFPM.add(llvm::createDeadCodeEliminationPass());
            specFPM.doInitialization();
            for (auto& func : *module) {
                if (func.isDeclaration()) continue;
                if (func.getName().contains(".spec.")) {
                    specFPM.run(func);
                }
            }
            specFPM.doFinalization();
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
            std::cout << "    Running superoptimizer (level " << superoptLevel_
                      << ": idiom recognition, algebraic simplification"
                      << (superoptLevel_ >= 2 ? ", synthesis, branch-opt" : "")
                      << ")..." << std::endl;
        }
        superopt::SuperoptimizerConfig superConfig;
        // Configure based on superopt level:
        //   Level 1: idiom + algebraic only (fast compilation)
        //   Level 2: all features, default synthesis (balanced)
        //   Level 3: all features, aggressive synthesis (slower compilation)
        if (superoptLevel_ <= 1) {
            superConfig.enableSynthesis = false;
            superConfig.enableBranchOpt = false;
        } else if (superoptLevel_ >= 3 || optimizationLevel >= OptimizationLevel::O3) {
            superConfig.synthesis.maxInstructions = 5;
            superConfig.synthesis.costThreshold = 0.9;
        }
        auto superStats = superopt::superoptimizeModule(*module, superConfig);
        const unsigned totalSuperOpts = superStats.idiomsReplaced + superStats.synthReplacements +
                                 superStats.algebraicSimplified + superStats.branchesSimplified +
                                 superStats.deadCodeEliminated;
        if (verbose_) {
            std::cout << "    Superoptimizer complete: "
                      << superStats.idiomsReplaced << " idioms replaced, "
                      << superStats.algebraicSimplified << " algebraic simplifications, "
                      << superStats.synthReplacements << " synthesis replacements, "
                      << superStats.branchesSimplified << " branches simplified, "
                      << superStats.deadCodeEliminated << " dead instructions eliminated"
                      << " (" << totalSuperOpts << " total optimizations)" << std::endl;
        }

        // Post-superoptimizer cleanup: the superoptimizer creates algebraically
        // simplified patterns that may have new CSE opportunities.  A quick
        // GVN + InstCombine + SimplifyCFG pass cleans these up before the
        // signed→unsigned conversion and HGOE runs.
        // Only run on functions ≤2000 instructions to avoid excessive compile time.
        if (totalSuperOpts > 0) {
            llvm::legacy::FunctionPassManager postSuperFPM(module.get());
            postSuperFPM.add(llvm::createGVNPass());
            postSuperFPM.add(llvm::createInstructionCombiningPass());
            postSuperFPM.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));
            postSuperFPM.add(llvm::createDeadCodeEliminationPass());
            postSuperFPM.doInitialization();
            for (auto& func : *module) {
                if (func.isDeclaration()) continue;
                if (func.getInstructionCount() > 2000) continue;
                postSuperFPM.run(func);
            }
            postSuperFPM.doFinalization();
        }
    } else if (verbose_ && optimizationLevel >= OptimizationLevel::O2 && !enableSuperopt_) {
        std::cout << "    Superoptimizer disabled (-fno-superopt)" << std::endl;
    }

    // Post-pipeline srem→urem and sdiv→udiv conversion.  The pre-pipeline
    // conversion (above) catches srem/sdiv in the original loop body so the
    // vectorizer sees urem/udiv.  However, LLVM's loop unroller creates NEW
    // srem/sdiv instructions in unrolled copies that didn't go through the
    // pre-pipeline conversion.  For example, after 4x unrolling of
    //   sum += ((i^j) + k) % 37
    // the first iteration has urem (converted pre-pipeline) but iterations
    // 2-4 have srem (created by the unroller).  This post-pipeline pass
    // catches those residual srem/sdiv instructions.
    //
    // This is an OmScript-specific advantage: because OmScript's for-loop
    // iterators are immutable within the loop body, we can prove
    // non-negativity of iterator-derived expressions even after unrolling.
    // C compilers cannot make this guarantee because C loop variables can
    // be modified arbitrarily.
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2) {
        unsigned postConvCount = 0;
        // Iterate: first infer nuw flags on add instructions where both
        // operands are provably non-negative (this catches unrolled copies
        // that lost their flags).  Then convert srem→urem and sdiv→udiv.
        // Each pass may enable the next: inferring nuw on an add makes its
        // result provably non-negative, which enables srem→urem on srem
        // instructions that use that add as their dividend.
        // Converges in 2-3 iterations for typical unrolled loops.
        for (int iter = 0; iter < 3; ++iter) {
            unsigned iterCount = 0;
            for (auto& func : *module) {
                iterCount += superopt::inferNonNegativeFlags(func);
                iterCount += superopt::convertSRemToURem(func);
                iterCount += superopt::convertSDivToUDiv(func);
            }
            if (iterCount == 0) break;  // fixed point reached
            postConvCount += iterCount;
        }
        if (verbose_ && postConvCount > 0) {
            std::cout << "    Post-pipeline signed→unsigned: "
                      << postConvCount << " conversions" << std::endl;
        }
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

    // -----------------------------------------------------------------------
    // Post-HGOE srem→urem and sdiv→udiv conversion
    // -----------------------------------------------------------------------
    // The HGOE may introduce new arithmetic patterns (e.g. from FMA fusion
    // or loop restructuring) that contain srem/sdiv instructions.  Run one
    // more round of signed→unsigned conversion to catch these residuals.
    // This is particularly important because urem/udiv on x86-64 avoid the
    // sign-correction fixup (3 extra instructions per operation).
    if (enableSuperopt_ && optimizationLevel >= OptimizationLevel::O2
        && enableHGOE_ && !marchCpu_.empty()) {
        unsigned postHGOECount = 0;
        for (int iter = 0; iter < 2; ++iter) {
            unsigned iterCount = 0;
            for (auto& func : *module) {
                iterCount += superopt::inferNonNegativeFlags(func);
                iterCount += superopt::convertSRemToURem(func);
                iterCount += superopt::convertSDivToUDiv(func);
            }
            if (iterCount == 0) break;
            postHGOECount += iterCount;
        }
        if (verbose_ && postHGOECount > 0) {
            std::cout << "    Post-HGOE signed→unsigned: "
                      << postHGOECount << " conversions" << std::endl;
        }
    }

    // -----------------------------------------------------------------------
    // Post-Optimization Prefetch Cleanup
    // -----------------------------------------------------------------------
    // After all major optimizations (constant folding, loop elimination,
    // inlining, superoptimizer, HGOE), remove prefetch intrinsics that are
    // no longer meaningful:
    //   1. Target is a constant or derived from a constant (inttoptr of literal).
    //   2. Target is a stack allocation (alloca) or register-promoted value.
    //   3. The prefetched value has no remaining memory-accessing users.
    //   4. The associated computation was constant-folded or eliminated.
    //   5. Pure functions with no memory side effects need no prefetch.
    // Prefetch-tagged variables go to registers (promoted by SROA/mem2reg)
    // and stay there until invalidated — no special metadata bypass needed.
    if (optimizationLevel >= OptimizationLevel::O2) {
        unsigned prefetchesRemoved = 0;
        for (auto& F : *module) {
            if (F.isDeclaration()) continue;
            llvm::SmallVector<llvm::CallInst*, 8> toRemove;
            for (auto& BB : F) {
                for (auto& I : BB) {
                    auto* call = llvm::dyn_cast<llvm::CallInst>(&I);
                    if (!call) continue;
                    auto* callee = call->getCalledFunction();
                    if (!callee || callee->getIntrinsicID() != llvm::Intrinsic::prefetch)
                        continue;

                    llvm::Value* ptr = call->getArgOperand(0);
                    bool shouldRemove = false;

                    // Rule 1: Target is a constant or inttoptr of a constant.
                    if (llvm::isa<llvm::Constant>(ptr)) {
                        shouldRemove = true;
                    }

                    // Rule 1b: inttoptr of a constant integer.
                    if (!shouldRemove) {
                        if (auto* i2p = llvm::dyn_cast<llvm::IntToPtrInst>(ptr)) {
                            if (llvm::isa<llvm::ConstantInt>(i2p->getOperand(0))) {
                                shouldRemove = true;
                            }
                        }
                        // Also check ConstantExpr inttoptr
                        if (auto* ce = llvm::dyn_cast<llvm::ConstantExpr>(ptr)) {
                            if (ce->getOpcode() == llvm::Instruction::IntToPtr) {
                                shouldRemove = true;
                            }
                        }
                    }

                    // Rule 2: Target is a stack allocation (alloca).
                    // Skip if tagged with !omscript.memory_prefetch — these are
                    // intentional memory-resident prefetches for large types
                    // (structs, arrays) that cannot fit in registers.
                    if (!shouldRemove) {
                        llvm::Value* underlying = ptr->stripPointerCasts();
                        if (llvm::isa<llvm::AllocaInst>(underlying)) {
                            if (!call->getMetadata("omscript.memory_prefetch")) {
                                shouldRemove = true;
                            }
                        }
                    }

                    // Rule 3: No remaining memory-accessing users of the
                    // prefetch target (the prefetch itself is not useful).
                    if (!shouldRemove) {
                        llvm::Value* underlying = ptr->stripPointerCasts();
                        bool hasMemoryUser = false;
                        for (auto* user : underlying->users()) {
                            if (user == call) continue; // skip the prefetch itself
                            if (llvm::isa<llvm::LoadInst>(user) ||
                                llvm::isa<llvm::StoreInst>(user)) {
                                hasMemoryUser = true;
                                break;
                            }
                            // GEPs with load/store users also count
                            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user)) {
                                for (auto* gepUser : gep->users()) {
                                    if (llvm::isa<llvm::LoadInst>(gepUser) ||
                                        llvm::isa<llvm::StoreInst>(gepUser)) {
                                        hasMemoryUser = true;
                                        break;
                                    }
                                }
                            }
                            if (hasMemoryUser) break;
                        }
                        if (!hasMemoryUser) {
                            shouldRemove = true;
                        }
                    }

                    // Rule 5: Prefetch inside a function with no memory side
                    // effects (readnone/pure) is meaningless.
                    if (!shouldRemove) {
                        if (F.doesNotAccessMemory()) {
                            shouldRemove = true;
                        }
                    }

                    if (shouldRemove) {
                        toRemove.push_back(call);
                    }
                }
            }
            for (auto* call : toRemove) {
                call->eraseFromParent();
                ++prefetchesRemoved;
            }
        }
        if (verbose_ && prefetchesRemoved > 0) {
            std::cout << "    Post-optimization prefetch cleanup: "
                      << prefetchesRemoved << " redundant prefetch(es) removed"
                      << std::endl;
        }

        // Final aggressive cleanup after prefetch removal and all post-pipeline
        // optimizations.  GVN catches redundant computations exposed by
        // superoptimizer/HGOE transforms.  DCE runs twice: first catches
        // immediately dead code, InstCombine + CFGSimplification may expose
        // further dead code, and the second DCE catches that.
        llvm::legacy::FunctionPassManager cleanupFPM(module.get());
        cleanupFPM.add(llvm::createGVNPass());
        cleanupFPM.add(llvm::createDeadCodeEliminationPass());
        cleanupFPM.add(llvm::createInstructionCombiningPass());
        cleanupFPM.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));
        // Post-pipeline loop cleanup: LoopSimplify re-canonicalizes loops
        // after superoptimizer/HGOE transforms, enabling LICM to hoist
        // invariants and LSR to reduce address computation complexity.
        cleanupFPM.add(llvm::createLoopSimplifyPass());
        cleanupFPM.add(llvm::createLICMPass());
        cleanupFPM.add(llvm::createLoopStrengthReducePass());
        cleanupFPM.add(llvm::createDeadCodeEliminationPass());
        cleanupFPM.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));
        cleanupFPM.doInitialization();
        for (auto& func : *module) {
            if (!func.isDeclaration()) {
                cleanupFPM.run(func);
            }
        }
        cleanupFPM.doFinalization();
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
    fpm.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));
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
    //
    // We collect OPTMAX function pointers in a single pass so that the later
    // optimization loop can skip the name→string conversion per function.
    static constexpr unsigned kAlwaysInlineThreshold = 100; // instruction count
    llvm::SmallVector<llvm::Function*, 16> optMaxFuncs;
    for (auto& func : module->functions()) {
        if (func.isDeclaration())
            continue;
        const llvm::StringRef name = func.getName();
        if (!optMaxFunctions.count(std::string(name)))
            continue;
        func.addFnAttr(llvm::Attribute::NoUnwind);
        // WillReturn: OPTMAX functions always return (no infinite loops or
        // exceptions), enabling LLVM to speculate calls and eliminate dead ones.
        func.addFnAttr(llvm::Attribute::WillReturn);
        // NoSync: OPTMAX functions don't synchronize (no atomics, locks, or
        // thread-related operations), enabling aggressive reordering.
        func.addFnAttr(llvm::Attribute::NoSync);
        // NoFree: OPTMAX functions don't free heap memory, enabling the
        // optimizer to sink/hoist loads past calls to these functions.
        func.addFnAttr(llvm::Attribute::NoFree);
        // Mark small OPTMAX helpers as always-inline candidates.
        // Higher threshold (30 instrs) ensures utility functions like
        // classify(), add_one/two/four() are force-inlined, eliminating
        // call overhead in tight loops.
        if (func.getInstructionCount() < kAlwaysInlineThreshold) {
            func.addFnAttr(llvm::Attribute::AlwaysInline);
        }
        optMaxFuncs.push_back(&func);
    }

    llvm::legacy::FunctionPassManager fpm(module.get());

    // Phase 1: Early canonicalization
    fpm.add(llvm::createSROAPass());
    fpm.add(llvm::createEarlyCSEPass(/*UseMemorySSA=*/true));
    fpm.add(llvm::createPromoteMemoryToRegisterPass());
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createReassociatePass());
    fpm.add(llvm::createGVNPass());
    fpm.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));
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
    // Use aggressive loop unrolling for OPTMAX: OptLevel=3 for maximum
    // unroll factor, threshold=500 to allow larger loop bodies to unroll.
    // Higher threshold enables unrolling of loops containing modulo/division
    // sequences that expand to multiple µops during ISel.
    fpm.add(llvm::createLoopUnrollPass(/*OptLevel=*/3, /*OnlyWhenForced=*/false,
                                       /*ForgetAllSCEV=*/false, /*Threshold=*/1000));
    // Phase 2.5: Post-loop cleanup.  After unrolling, GVN + InstCombine catch
    // constant-foldable patterns in unrolled iterations (e.g. known-constant
    // IV values, redundant loads) that DCE alone would miss.
    fpm.add(llvm::createGVNPass());
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));
    fpm.add(llvm::createDeadCodeEliminationPass());

    // Phase 2.6: Superblock / Hyperblock formation for OPTMAX functions.
    // After loop unrolling and cleanup, the CFG contains many small blocks
    // from unrolled iterations and branch-heavy patterns.  We form
    // superblocks (via GVN which implicitly threads branches through known
    // values) and hyperblocks (via aggressive if-conversion with a high
    // speculation threshold) to create larger basic blocks that improve
    // instruction scheduling and reduce branch overhead.
    fpm.add(llvm::createGVNPass());
    fpm.add(llvm::createSpeculativeExecutionPass());
    fpm.add(llvm::createCFGSimplificationPass(hyperblockCFGOpts()));
    fpm.add(llvm::createFlattenCFGPass());
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createDeadCodeEliminationPass());

    // Phase 3: Post-loop optimizations
#if LLVM_VERSION_MAJOR < 18
    fpm.add(llvm::createMergedLoadStoreMotionPass());
#endif
    fpm.add(llvm::createSinkingPass());
    fpm.add(llvm::createStraightLineStrengthReducePass());
    fpm.add(llvm::createNaryReassociatePass());
    fpm.add(llvm::createReassociatePass());
    fpm.add(llvm::createTailCallEliminationPass());
    fpm.add(llvm::createConstantHoistingPass());
    fpm.add(llvm::createSeparateConstOffsetFromGEPPass());
    fpm.add(llvm::createSpeculativeExecutionPass());
    // PartiallyInlineLibCalls replaces calls to math library functions
    // (sqrt, etc.) with their inline fast-path + slow-path branch, avoiding
    // the overhead of a full function call when the fast path applies.
    fpm.add(llvm::createPartiallyInlineLibCallsPass());
    fpm.add(llvm::createFlattenCFGPass());

    // Phase 3.5: Aggressive cleanup passes for maximal optimization.
    fpm.add(llvm::createDeadCodeEliminationPass());
    fpm.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));

    // Phase 3.6: Post-cleanup loop re-canonicalization.  After aggressive DCE
    // and CFG simplification, loops may have simplified structure that enables
    // further LICM and strength reduction.  Re-canonicalize loops and run
    // another round of LICM + LSR to catch these opportunities.
    fpm.add(llvm::createLoopSimplifyPass());
    fpm.add(llvm::createLICMPass());
    fpm.add(llvm::createLoopStrengthReducePass());
    // MergeICmps transforms chains of integer comparisons (e.g. struct
    // equality) into a single memcmp call.  Running it in OPTMAX catches
    // comparison chains in classify()-style functions.
    fpm.add(llvm::createMergeICmpsLegacyPass());

    // Phase 4: Final cleanup — InstSimplify, InstCombine, CFGSimplification,
    // and DCE remove dead code and simplify patterns exposed by loop and
    // control-flow transformations above.
    fpm.add(llvm::createInstSimplifyLegacyPass());
    fpm.add(llvm::createInstructionCombiningPass());
    fpm.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));
    fpm.add(llvm::createDeadCodeEliminationPass());

    fpm.doInitialization();
    for (llvm::Function* func : optMaxFuncs) {
        // OPTMAX runs the aggressive pass stack twice to maximize optimization.
        // The first iteration does the heavy lifting; the second catches
        // patterns exposed by loop/strength-reduce transforms.  Beyond two,
        // passes reach a near-fixed-point and additional iterations produce
        // negligible changes while doubling compile time.
        constexpr int optMaxIterations = 3;
        for (int i = 0; i < optMaxIterations; ++i) {
            fpm.run(*func);
        }
    }
    fpm.doFinalization();
}

void CodeGenerator::writeObjectFile(const std::string& filename) {
    // Initialize only native target
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    const std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
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
        const std::string errMsg = "Error writing object file '" + filename + "': " + dest.error().message();
        dest.clear_error();
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, errMsg});
    }
}

void CodeGenerator::writeBitcodeFile(const std::string& filename) {
    // Initialize native target so the data layout can be set correctly.
    llvm::InitializeNativeTarget();

    const std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
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
        const std::string errMsg = "Error writing bitcode file '" + filename + "': " + dest.error().message();
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
    fpm.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));

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
    fpm.add(llvm::createCFGSimplificationPass(aggressiveCFGOpts()));
    fpm.add(llvm::createDeadCodeEliminationPass());

    fpm.doInitialization();

    // At O3, run the pass pipeline multiple times: each iteration can expose
    // new optimization opportunities (e.g., strength reduction creates patterns
    // that instcombine can simplify further).  Two iterations is the sweet spot
    // for per-function JIT optimization without excessive compile time.
    const int iterations = (optimizationLevel >= OptimizationLevel::O3) ? 2 : 1;
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
