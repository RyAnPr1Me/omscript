#include "aot_profile.h"
#include "deopt.h"
#include "jit_profiler.h"

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/SubtargetFeature.h>
#include <llvm/TargetParser/Triple.h>

#include <atomic>
#include <cassert>
#include <climits>
#include <mutex>

namespace omscript {

// ---------------------------------------------------------------------------
// Process-wide active runner — set during AdaptiveJITRunner::run() so that
// the C-linkage __omsc_adaptive_recompile callback can reach the runner.
// Atomic to avoid data races when the callback fires from JIT-compiled code.
// ---------------------------------------------------------------------------
static std::atomic<AdaptiveJITRunner*> g_activeRunner{nullptr};

AdaptiveJITRunner::AdaptiveJITRunner() = default;
AdaptiveJITRunner::~AdaptiveJITRunner() = default;

void AdaptiveJITRunner::ensureInitialized() {
    static std::once_flag initFlag;
    std::call_once(initFlag, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
    });
}

// ---------------------------------------------------------------------------
// injectCounters() — add call-counting dispatch prologs
// ---------------------------------------------------------------------------
// For every non-main, non-declaration function in @p mod this inserts:
//
//   @__omsc_calls_<name>  = internal global i64 0   ; call counter
//   @__omsc_fn_<name>     = internal global ptr null ; hot-patch slot
//
// And prepends a new entry block ("omsc.dispatch") before the original
// entry ("omsc.body"), implementing the two-tier dispatch:
//
//   omsc.dispatch:
//     %fp = load volatile ptr, @__omsc_fn_<name>   ; check hot-patch slot
//     br (%fp != null) ? omsc.hot : omsc.count
//
//   omsc.hot:                          ; Tier-2 — already recompiled
//     %r = call i64 %fp(same args)
//     ret i64 %r
//
//   omsc.count:                        ; Tier-1 — still running at O2
//     %old = atomicrmw add @__omsc_calls_<name>, 1, monotonic
//     %new = add %old, 1
//     br (%new == kThreshold) ? omsc.recompile : omsc.body
//
//   omsc.recompile:
//     call void @__omsc_adaptive_recompile(&name_str, %new, @__omsc_fn_<name>)
//     br omsc.body
//
//   omsc.body:        (original entry block, untouched)
//     <original function IR>
// ---------------------------------------------------------------------------
void AdaptiveJITRunner::injectCounters(llvm::Module& mod) {
    auto& ctx = mod.getContext();
    auto* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(ctx);
    auto* i8Ty = llvm::Type::getInt8Ty(ctx);
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    // Declare the C++ callback (defined in this translation unit via
    // the extern "C" wrapper at the bottom of the file).
    auto* callbackTy = llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, ptrTy}, false);
    auto* callbackFn =
        llvm::cast<llvm::Function>(mod.getOrInsertFunction("__omsc_adaptive_recompile", callbackTy).getCallee());
    callbackFn->addFnAttr(llvm::Attribute::NoUnwind);

    // Declare the argument profiling callback.
    // Signature: void __omsc_profile_arg(const char* name, uint32_t idx, uint8_t type, int64_t value)
    auto* argProfileTy = llvm::FunctionType::get(voidTy, {ptrTy, i32Ty, i8Ty, i64Ty}, false);
    auto* argProfileFn =
        llvm::cast<llvm::Function>(mod.getOrInsertFunction("__omsc_profile_arg", argProfileTy).getCallee());
    argProfileFn->addFnAttr(llvm::Attribute::NoUnwind);

    // Collect functions first to avoid iterator invalidation.
    llvm::SmallVector<llvm::Function*, 32> toInstrument;
    for (auto& fn : mod) {
        if (fn.isDeclaration() || fn.getName() == "main")
            continue;
        // Skip compiler-internal helpers (names starting with "__").
        if (fn.getName().starts_with("__"))
            continue;
        toInstrument.push_back(&fn);
    }

    for (auto* fn : toInstrument) {
        const std::string name = fn->getName().str();

        // The dispatch prolog adds volatile loads (fnPtrGV) and atomic
        // read-modify-write instructions (call counter).  Strip function-level
        // attributes that would contradict these new accesses and cause the
        // backend to miscompile the instrumented function.
        fn->removeFnAttr(llvm::Attribute::NoSync);   // atomicrmw is synchronising
        fn->removeFnAttr(llvm::Attribute::Memory);   // we now read global memory
        fn->removeFnAttr(llvm::Attribute::ReadNone); // legacy alias
        fn->removeFnAttr(llvm::Attribute::ReadOnly); // legacy alias

        auto* counterGV = new llvm::GlobalVariable(mod, i64Ty, /*isConst=*/false, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantInt::get(i64Ty, 0), "__omsc_calls_" + name);
        counterGV->setAlignment(llvm::Align(8));

        auto* fnPtrGV = new llvm::GlobalVariable(mod, ptrTy, /*isConst=*/false, llvm::GlobalValue::InternalLinkage,
                                                 llvm::Constant::getNullValue(ptrTy), "__omsc_fn_" + name);
        fnPtrGV->setAlignment(llvm::Align(8));

        // Function-name string constant (used by the callback).
        // createGlobalString returns a ptr (GEP) to the string data.
        llvm::IRBuilder<> tmpB(ctx);
        auto* nameGV = tmpB.CreateGlobalString(name, "__omsc_name_" + name, 0, &mod);

        // --- New basic blocks (inserted before original entry) ---
        auto& origEntry = fn->getEntryBlock();
        auto* dispatchBB = llvm::BasicBlock::Create(ctx, "omsc.dispatch", fn, &origEntry);
        auto* hotBB = llvm::BasicBlock::Create(ctx, "omsc.hot", fn, &origEntry);
        auto* countBB = llvm::BasicBlock::Create(ctx, "omsc.count", fn, &origEntry);
        auto* recompileBB = llvm::BasicBlock::Create(ctx, "omsc.recompile", fn, &origEntry);

        // --- omsc.dispatch: volatile-load fn-ptr, branch on non-null ---
        llvm::IRBuilder<> B(dispatchBB);
        auto* fp = B.CreateAlignedLoad(ptrTy, fnPtrGV, llvm::Align(8), /*isVolatile=*/true, "omsc.fp");
        auto* isHot =
            B.CreateICmpNE(B.CreatePtrToInt(fp, i64Ty, "omsc.fp_int"), llvm::ConstantInt::get(i64Ty, 0), "omsc.is_hot");
        // Branch weights: the hot path (isHot == true, 99%) is taken on every
        // call after the Tier-2 recompile writes fnPtrSlot; the counter path
        // (isHot == false, 1%) fires only during the warm-up phase before
        // the threshold.  Marking the hot path as 99:1 causes the branch
        // predictor and code layout to favour the direct-call fast path once
        // steady state is reached.
        llvm::MDBuilder mdB(ctx);
        auto* weights = mdB.createBranchWeights(99, 1);
        B.CreateCondBr(isHot, hotBB, countBB, weights);

        // --- omsc.hot: call the recompiled version and return its result ---
        // NOTE: this must be a regular (non-tail) call.  A tail call would
        // cause the Tier-2 native code to return directly to the dispatch
        // prolog's caller, bypassing any Tier-1 stack frames that are
        // currently mid-execution (e.g. the outer frames of a directly
        // recursive function).  With a regular call the extra stack frame
        // is trivial and correctness is preserved for all call patterns.
        B.SetInsertPoint(hotBB);
        {
            llvm::SmallVector<llvm::Value*, 8> args;
            for (auto& arg : fn->args())
                args.push_back(&arg);
            auto* hotCall = B.CreateCall(fn->getFunctionType(), fp, args, "omsc.hot_r");
            B.CreateRet(hotCall);
        }

        // --- omsc.count: atomic increment, check threshold, profile args ---
        B.SetInsertPoint(countBB);
        auto* oldCnt = B.CreateAtomicRMW(llvm::AtomicRMWInst::Add, counterGV, llvm::ConstantInt::get(i64Ty, 1),
                                         llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
        oldCnt->setName("omsc.old");
        auto* newCnt = B.CreateAdd(oldCnt, llvm::ConstantInt::get(i64Ty, 1), "omsc.new");

        // Inject argument type profiling for each parameter.
        // During the warm-up phase (before Tier-2 recompile) this records
        // the runtime type of each argument, which feeds into type
        // specialization during Tier-2 IR optimization.
        // All OmScript values are i64 at the IR level; ArgType::Integer (1)
        // is the default tag for the dynamically-typed representation.
        uint32_t argIdx = 0;
        for (auto& arg : fn->args()) {
            // Default: treat all args as Integer type since OmScript uses
            // i64 for its dynamic value representation.
            B.CreateCall(argProfileTy, argProfileFn,
                         {nameGV, llvm::ConstantInt::get(i32Ty, argIdx),
                          llvm::ConstantInt::get(i8Ty, static_cast<uint8_t>(ArgType::Integer)), &arg});
            argIdx++;
        }

        auto* hitThresh =
            B.CreateICmpEQ(newCnt, llvm::ConstantInt::get(i64Ty, AdaptiveJITRunner::kRecompileThreshold), "omsc.hit");
        B.CreateCondBr(hitThresh, recompileBB, &origEntry);

        // --- omsc.recompile: call the C++ recompiler, then fall to body ---
        B.SetInsertPoint(recompileBB);
        B.CreateCall(callbackTy, callbackFn, {nameGV, newCnt, fnPtrGV});
        B.CreateBr(&origEntry);
    }
}

// ---------------------------------------------------------------------------
// run() — the main entry point for `omsc run`
// ---------------------------------------------------------------------------
int AdaptiveJITRunner::run(llvm::Module* baseModule) {
    ensureInitialized();

    // Step 1: Serialise the CLEAN module to bitcode.
    // This bitcode has no dispatch prologs or counter globals — it is the
    // source used for Tier-2 PGO recompilation of hot functions.
    {
        llvm::SmallVector<char, 0> buf;
        llvm::raw_svector_ostream os(buf);
        llvm::WriteBitcodeToFile(*baseModule, os);
        cleanBitcode_.assign(buf.begin(), buf.end());
    }

    // Step 2: Re-parse a working copy in a fresh context.
    // We do not modify baseModule so the caller's codegen stays intact.
    auto instrCtx = std::make_unique<llvm::LLVMContext>();
    auto memBuf = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(cleanBitcode_.data(), cleanBitcode_.size()),
                                                   "omsc_working_bc", /*RequiresNullTerminator=*/false);
    auto modOrErr = llvm::parseBitcodeFile(memBuf->getMemBufferRef(), *instrCtx);
    if (!modOrErr)
        return 1;
    auto instrMod = std::move(*modOrErr);

    // Step 3: Inject call-counting dispatch prologs.
    injectCounters(*instrMod);

    // Verify the instrumented IR is well-formed before handing it to MCJIT.
    if (llvm::verifyModule(*instrMod, &llvm::errs())) {
        llvm::errs() << "omsc: internal error: instrumented IR failed verification\n";
        return 1;
    }

    // Step 4: JIT-compile the instrumented module.
    // We skip a separate IR optimisation pass here — MCJIT's own backend
    // optimiser (CodeGenOpt::Default = O2) gives good initial code quality
    // while keeping Tier-1 compilation fast so execution starts promptly.
    // Hot functions will be individually recompiled at O3 in Tier 2.
    JitModule jm;
    jm.ctx = std::move(instrCtx); // keep context alive with the engine

    std::string engineErr;
    llvm::EngineBuilder builder(std::move(instrMod));
    builder.setErrorStr(&engineErr);
    builder.setEngineKind(llvm::EngineKind::JIT);
#if LLVM_VERSION_MAJOR >= 18
    builder.setOptLevel(llvm::CodeGenOptLevel::Default);
#else
    builder.setOptLevel(llvm::CodeGenOpt::Default);
#endif

    llvm::ExecutionEngine* rawEngine = builder.create();
    if (!rawEngine) {
        llvm::errs() << "omsc: JIT engine creation failed: " << engineErr << "\n";
        return 1;
    }
    jm.engine.reset(rawEngine);

    // Register the C-linkage recompile callback so MCJIT can resolve it
    // without needing -rdynamic.  The symbol is in the executable but not
    // in the dynamic symbol table unless the binary is built with -rdynamic,
    // so we map it explicitly.
    rawEngine->addGlobalMapping("__omsc_adaptive_recompile", reinterpret_cast<uint64_t>(&__omsc_adaptive_recompile));

    // Register the profiling and deoptimization callbacks so JIT-compiled
    // code that calls them can be resolved by MCJIT.
    rawEngine->addGlobalMapping("__omsc_profile_branch", reinterpret_cast<uint64_t>(&__omsc_profile_branch));
    rawEngine->addGlobalMapping("__omsc_profile_arg", reinterpret_cast<uint64_t>(&__omsc_profile_arg));
    rawEngine->addGlobalMapping("__omsc_deopt_guard_fail", reinterpret_cast<uint64_t>(&__omsc_deopt_guard_fail));

    rawEngine->finalizeObject();

    uint64_t mainAddr = rawEngine->getFunctionAddress("main");
    if (!mainAddr)
        return 1;

    {
        std::lock_guard<std::mutex> lk(recompiledMtx_);
        modules_.push_back(std::move(jm));
    }

    // Step 5: Execute main() in-process.
    // Register this runner as the active one so that the C-linkage callback
    // can reach it during execution.  RAII guard ensures the pointer is
    // cleared even if main() exits via an exception.
    g_activeRunner.store(this, std::memory_order_release);
    struct RunnerGuard {
        ~RunnerGuard() {
            g_activeRunner.store(nullptr, std::memory_order_release);
        }
    } runnerGuard;
    using OmscMainFn = int64_t (*)();
    int64_t exitVal = reinterpret_cast<OmscMainFn>(mainAddr)();

    // Mirror what a process exit code would look like (0-255).
    return static_cast<int>(exitVal & 0xFF);
}

// ---------------------------------------------------------------------------
// onHotFunction() — Tier-2 PGO recompilation (synchronous)
// ---------------------------------------------------------------------------
// Called from the dispatch prolog when a function's call count first reaches
// kRecompileThreshold.  Runs synchronously (blocking the caller for the
// duration of O3 compilation) because LLVM MCJIT is not safe to use from
// a background thread while another MCJIT engine is executing code.
void AdaptiveJITRunner::onHotFunction(const char* name, int64_t callCount, void** fnPtrSlot) {
    const std::string funcName(name);

    // Recompile each function at most once.
    {
        std::lock_guard<std::mutex> lk(recompiledMtx_);
        if (recompiled_.count(funcName))
            return;
        // Mark as in-progress to prevent concurrent recompilation attempts.
        // On failure this is rolled back (see end of function).
        recompiled_.insert(funcName);
    }

    // RAII guard: on early return (failure), remove the function from
    // recompiled_ so that it can be retried on the next threshold hit.
    bool succeeded = false;
    auto rollback = [&]() {
        if (!succeeded) {
            std::lock_guard<std::mutex> lk(recompiledMtx_);
            recompiled_.erase(funcName);
        }
    };

    // --- Parse clean bitcode into a fresh context ---
    auto newCtx = std::make_unique<llvm::LLVMContext>();
    auto memBuf = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(cleanBitcode_.data(), cleanBitcode_.size()),
                                                   "omsc_pgo_bc", /*RequiresNullTerminator=*/false);
    auto modOrErr = llvm::parseBitcodeFile(memBuf->getMemBufferRef(), *newCtx);
    if (!modOrErr) {
        llvm::errs() << "omsc: Tier-2 recompile of '" << funcName << "' failed: could not parse bitcode\n";
        rollback();
        return;
    }
    auto mod = std::move(*modOrErr);

    // --- Apply PGO entry-count annotation ---
    // setEntryCount() tells every downstream LLVM pass (inliner, branch
    // layout, loop vectoriser, unroller) that this function is called
    // callCount times — shifting them from static heuristics to data-driven
    // optimisation.  Callees that are only reachable via this function get
    // entry counts of 1, making the inliner aggressively inline them *into*
    // the hot function while avoiding the reverse.
    llvm::Function* fn = mod->getFunction(funcName);
    if (!fn || fn->isDeclaration()) {
        llvm::errs() << "omsc: Tier-2 recompile of '" << funcName << "' failed: function not found in bitcode\n";
        rollback();
        return;
    }
    fn->setEntryCount(static_cast<uint64_t>(callCount));
    for (auto& other : *mod) {
        if (!other.isDeclaration() && other.getName() != funcName && !other.getEntryCount())
            other.setEntryCount(1);
    }

    // --- Apply collected branch profile data as LLVM branch weights ---
    // The JITProfiler records taken/not-taken counts for each branch site
    // during the warm-up phase.  Attaching these as branch weight metadata
    // guides the code layout, branch predictor hints, and LLVM's hot/cold
    // splitting passes in the O3 pipeline.
    {
        const FunctionProfile* prof = JITProfiler::instance().getProfile(funcName);
        if (prof && !prof->branches.empty()) {
            llvm::MDBuilder mdb(fn->getContext());
            uint32_t branchIdx = 0;
            for (auto& bb : *fn) {
                auto* term = bb.getTerminator();
                if (!term)
                    continue;
                auto* br = llvm::dyn_cast<llvm::BranchInst>(term);
                if (!br || !br->isConditional())
                    continue;
                if (branchIdx < prof->branches.size()) {
                    const auto& bp = prof->branches[branchIdx];
                    // Saturate to UINT32_MAX to avoid data loss from narrowing.
                    uint64_t tRaw = std::max(bp.takenCount, uint64_t{1});
                    uint64_t nRaw = std::max(bp.notTakenCount, uint64_t{1});
                    uint32_t taken = static_cast<uint32_t>(std::min(tRaw, uint64_t{UINT32_MAX}));
                    uint32_t notTaken = static_cast<uint32_t>(std::min(nRaw, uint64_t{UINT32_MAX}));
                    auto* bw = mdb.createBranchWeights(taken, notTaken);
                    br->setMetadata(llvm::LLVMContext::MD_prof, bw);
                }
                branchIdx++;
            }
        }
    }

    // --- Re-optimise at O3 with PGO guidance and full native CPU features ---
    {
        llvm::PipelineTuningOptions PTO;
        PTO.LoopVectorization = true;
        PTO.SLPVectorization = true;
        PTO.LoopUnrolling = true;
        PTO.LoopInterleaving = true;
        PTO.MergeFunctions = true;
        PTO.CallGraphProfile = true;
        PTO.InlinerThreshold = 400;

        // Build a native TargetMachine with ALL host CPU features exposed
        // so the vectoriser and scheduler can use AVX2/AVX-512/NEON etc.
        std::unique_ptr<llvm::TargetMachine> TM;
        {
            std::string triple = llvm::sys::getDefaultTargetTriple();
            std::string cpu = llvm::sys::getHostCPUName().str();
            llvm::SubtargetFeatures featureSet;
#if LLVM_VERSION_MAJOR >= 19
            llvm::StringMap<bool> hostFeatures = llvm::sys::getHostCPUFeatures();
#else
            llvm::StringMap<bool> hostFeatures;
            llvm::sys::getHostCPUFeatures(hostFeatures);
#endif
            for (auto& kv : hostFeatures)
                featureSet.AddFeature(kv.first(), kv.second);
            std::string features = featureSet.getString();

            std::string errStr;
            const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, errStr);
            if (!target) {
                llvm::errs() << "omsc: Tier-2 recompile of '" << funcName
                             << "' failed: could not lookup target: " << errStr << "\n";
                rollback();
                return;
            }
            llvm::TargetOptions opts;
            opts.EnableFastISel = false; // force optimising ISel for best codegen
#if LLVM_VERSION_MAJOR >= 21
            TM.reset(target->createTargetMachine(llvm::Triple(triple), cpu, features, opts,
                                                 std::optional<llvm::Reloc::Model>()));
#else
            TM.reset(target->createTargetMachine(triple, cpu, features, opts, std::optional<llvm::Reloc::Model>()));
#endif
            if (!TM) {
                llvm::errs() << "omsc: Tier-2 recompile of '" << funcName
                             << "' failed: could not create target machine\n";
                rollback();
                return;
            }
            mod->setDataLayout(TM->createDataLayout());
        }

        llvm::PassBuilder PB(TM.get(), PTO);
        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
        PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3).run(*mod, MAM);
    }

    // Verify the module after O3 optimization to catch optimizer bugs early
    // (the Tier-1 path already verifies at line 246; this mirrors it for Tier-2).
    if (llvm::verifyModule(*mod, &llvm::errs())) {
        llvm::errs() << "omsc: Tier-2 recompile of '" << funcName << "' failed: module verification after O3\n";
        rollback();
        return;
    }

    // --- JIT-compile the reoptimised module ---
    std::string engineErr;
    llvm::EngineBuilder eb(std::move(mod));
    eb.setErrorStr(&engineErr);
    eb.setEngineKind(llvm::EngineKind::JIT);
#if LLVM_VERSION_MAJOR >= 18
    eb.setOptLevel(llvm::CodeGenOptLevel::Aggressive);
#else
    eb.setOptLevel(llvm::CodeGenOpt::Aggressive);
#endif

    std::unique_ptr<llvm::ExecutionEngine> engine(eb.create());
    if (!engine) {
        llvm::errs() << "omsc: Tier-2 JIT compilation of '" << funcName << "' failed: " << engineErr << "\n";
        rollback();
        return;
    }
    engine->finalizeObject();

    uint64_t addr = engine->getFunctionAddress(funcName);
    if (!addr) {
        rollback();
        return;
    }

    // Mark successful — prevents rollback() from clearing the recompiled flag.
    succeeded = true;

    {
        std::lock_guard<std::mutex> lk(recompiledMtx_);
        modules_.push_back({std::move(newCtx), std::move(engine)});
    }

    // Hot-patch: the volatile load in the dispatch prolog will see this on
    // the very next call to this function.
    *fnPtrSlot = reinterpret_cast<void*>(addr);
}

} // namespace omscript

// ---------------------------------------------------------------------------
// C-linkage entry point — called from injected LLVM IR
// ---------------------------------------------------------------------------
extern "C" {

void __omsc_adaptive_recompile(const char* name, int64_t callCount, void** fnPtrSlot) {
    auto* runner = omscript::g_activeRunner.load(std::memory_order_acquire);
    if (runner)
        runner->onHotFunction(name, callCount, fnPtrSlot);
}

} // extern "C"
