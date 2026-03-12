#include "codegen.h"
#include "diagnostic.h"
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
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/CorrelatedValuePropagation.h>
#include <llvm/Transforms/Scalar/ConstraintElimination.h>
#include <llvm/Transforms/Scalar/DeadStoreElimination.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/LoopDistribute.h>
#include <llvm/Transforms/Scalar/LoopFuse.h>
#include <llvm/Transforms/Scalar/LoopLoadElimination.h>
#include <llvm/Transforms/Scalar/MemCpyOptimizer.h>
#include <llvm/Transforms/Utils.h>
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
    if (!mtuneCpu_.empty() && isNative) {
        cpu = mtuneCpu_;
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
    PTO.LoopUnrolling = enableUnrollLoops_;
    PTO.LoopInterleaving = enableVectorize_; // enable loop interleaving at O2+
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

    // At O3 with -floop-optimize, register LoopFusePass before vectorization
    // to merge adjacent loops with the same trip count into a single loop,
    // dramatically improving data cache locality and reducing loop overhead.
    // This is particularly beneficial for array-processing code where
    // successive passes over the same data can be combined.
    if (optimizationLevel >= OptimizationLevel::O3 && enableLoopOptimize_) {
        PB.registerVectorizerStartEPCallback(
            [](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) { FPM.addPass(llvm::LoopFusePass()); });
    }

    // At O3, inject AggressiveInstCombine after the standard peephole passes
    // to catch multi-instruction patterns (e.g. truncation sequences, popcount
    // idioms) that regular InstCombine does not handle.
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerPeepholeEPCallback([](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) {
            FPM.addPass(llvm::AggressiveInstCombinePass());
        });
    }

    // At O2+, inject CorrelatedValuePropagation and DeadStoreElimination
    // late in the scalar optimizer.  CVP uses value-range information from
    // branch conditions to sharpen comparisons, convert signed operations
    // to unsigned (enabling more shifts/masks), and eliminate provably-dead
    // branches.  DSE removes stores whose values are never read (e.g.
    // overwritten before use, or stored to dead allocations).
    if (optimizationLevel >= OptimizationLevel::O2) {
        PB.registerScalarOptimizerLateEPCallback([](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) {
            FPM.addPass(llvm::CorrelatedValuePropagationPass());
            FPM.addPass(llvm::DSEPass());
            FPM.addPass(llvm::MemCpyOptPass());
        });
    }

    // At O3, inject ConstraintElimination late in the scalar optimizer to
    // remove redundant comparisons and branches using range constraints
    // (e.g. after a bounds check, subsequent in-range accesses skip the check).
    if (optimizationLevel >= OptimizationLevel::O3) {
        PB.registerScalarOptimizerLateEPCallback([](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) {
            FPM.addPass(llvm::ConstraintEliminationPass());
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
        MPM = PB.buildLTOPreLinkDefaultPipeline(newPMLevel);
    } else {
        MPM = PB.buildPerModuleDefaultPipeline(newPMLevel);
    }
    // At O2+, append GlobalOptPass after the standard pipeline to constant-fold
    // and internalize global variables, propagate initial values, and eliminate
    // globals that are only stored but never read.  This cleans up patterns the
    // default pipeline leaves behind (e.g. globals used only in main).
    if (optimizationLevel >= OptimizationLevel::O2 && !lto_) {
        MPM.addPass(llvm::GlobalOptPass());
        MPM.addPass(llvm::GlobalDCEPass());
    }
    MPM.run(*module, MAM);
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
    static constexpr unsigned kAlwaysInlineThreshold = 50; // instruction count
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
    // Phase 2.5: Additional aggressive cleanup after loop optimizations.
    // A second GVN + instcombine round catches patterns exposed by loop
    // strength reduction, unrolling, and LICM that the first round missed.
    fpm.add(llvm::createGVNPass());
    fpm.add(llvm::createInstructionCombiningPass());
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
