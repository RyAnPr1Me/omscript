#include "aot_profile.h"
#include "deopt.h"
#include "jit_profiler.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
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
#include <llvm/Transforms/Scalar/LoopDistribute.h>

#include <atomic>
#include <cassert>
#include <climits>
#include <iostream>
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
// setJTMBOptLevel() — version-portable codegen optimization level setter
// ---------------------------------------------------------------------------
// LLVM 18 renamed CodeGenOpt::Level to CodeGenOptLevel, so every call site
// that sets the opt level needs a compile-time conditional.  This helper
// centralises the conditional so it appears exactly once.
static void setJTMBOptLevel(llvm::orc::JITTargetMachineBuilder& JTMB,
                             bool aggressive) {
#if LLVM_VERSION_MAJOR >= 18
    JTMB.setCodeGenOptLevel(aggressive ? llvm::CodeGenOptLevel::Aggressive
                                       : llvm::CodeGenOptLevel::None);
#else
    JTMB.setCodeGenOptLevel(aggressive ? llvm::CodeGenOpt::Aggressive
                                       : llvm::CodeGenOpt::None);
#endif
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
    auto* ptrTy = llvm::PointerType::getUnqual(ctx);
    auto* voidTy = llvm::Type::getVoidTy(ctx);

    // Declare the C++ callback (defined in this translation unit via
    // the extern "C" wrapper at the bottom of the file).
    auto* callbackTy = llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, ptrTy}, false);
    auto* callbackFn =
        llvm::cast<llvm::Function>(mod.getOrInsertFunction("__omsc_adaptive_recompile", callbackTy).getCallee());
    callbackFn->addFnAttr(llvm::Attribute::NoUnwind);

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

        // The dispatch prolog adds volatile loads (fnPtrGV) and writes to
        // global counters.  Strip function-level attributes that would
        // contradict these new accesses and cause the backend to miscompile
        // the instrumented function.
        fn->removeFnAttr(llvm::Attribute::NoSync);   // we call external profiling functions
        fn->removeFnAttr(llvm::Attribute::Memory);   // we now read/write global memory
        fn->removeFnAttr(llvm::Attribute::ReadNone); // legacy alias
        fn->removeFnAttr(llvm::Attribute::ReadOnly); // legacy alias

        // __omsc_calls_<name>: call counter, accessed only within the JIT'd code.
        // InternalLinkage keeps it private to the Tier-1 module — no lookup needed.
        auto* counterGV = new llvm::GlobalVariable(mod, i64Ty, /*isConst=*/false, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantInt::get(i64Ty, 0), "__omsc_calls_" + name);
        counterGV->setAlignment(llvm::Align(8));

        // __omsc_fn_<name>: the hot-path function-pointer slot.  ExternalLinkage
        // is required so ORC LLJIT's lookup() can resolve this global by name
        // after compilation (to pass the slot address to the eager-compilation
        // enqueue).  Names are unique (prefixed with the function name, itself
        // unique within the module) so there is no risk of cross-module collision.
        auto* fnPtrGV = new llvm::GlobalVariable(mod, ptrTy, /*isConst=*/false, llvm::GlobalValue::ExternalLinkage,
                                                 llvm::Constant::getNullValue(ptrTy), "__omsc_fn_" + name);
        fnPtrGV->setAlignment(llvm::Align(8));

        // Function-name string constant (used by the callback).
        // createGlobalString returns a ptr (GEP) to the string data.
        llvm::IRBuilder<> tmpB(ctx);
        auto* nameGV = tmpB.CreateGlobalString(name, "__omsc_name_" + name, 0, &mod);

        // ---------------------------------------------------------------
        // NOTE: We intentionally do NOT inject branch profiling, loop trip
        // count profiling, or call-site profiling into the function body.
        // These callbacks added measurable overhead on every branch, loop
        // iteration, and call site during Tier-1 execution.  Since Tier-1
        // only runs for a few calls (kTier2Threshold), the profile data
        // collected was minimal anyway.  Instead, the Tier-2 recompile
        // applies conservative PGO heuristics (equal branch weights,
        // aggressive inlining of all callees, default loop unroll factors)
        // which produce comparable code quality without the profiling cost.
        // ---------------------------------------------------------------

        // --- New basic blocks (inserted before original entry) ---
        // Simplified two-path dispatch: hot (fn-ptr set) or cold (counting).
        auto& origEntry = fn->getEntryBlock();
        auto* dispatchBB = llvm::BasicBlock::Create(ctx, "omsc.dispatch", fn, &origEntry);
        auto* hotCallBB = llvm::BasicBlock::Create(ctx, "omsc.hot_call", fn, &origEntry);
        auto* countBB = llvm::BasicBlock::Create(ctx, "omsc.count", fn, &origEntry);
        auto* recompileBB = llvm::BasicBlock::Create(ctx, "omsc.recompile", fn, &origEntry);

        // --- omsc.dispatch: atomic-load fn-ptr, branch on non-null ---
        // This is the entry point for EVERY call.  After the background
        // thread stores an optimized fn-ptr via atomic release, the atomic
        // acquire load here picks it up.  Acquire pairs with the writer's
        // release store so the reader is guaranteed to see the fully-
        // initialized function pointer (not a stale null or partial write)
        // on ALL architectures, including weakly-ordered ARM and PowerPC.
        // On x86/TSO this compiles to a plain MOV — acquire is free.
        llvm::IRBuilder<> B(dispatchBB);
        auto* fpLoad = B.CreateAlignedLoad(ptrTy, fnPtrGV, llvm::Align(8), /*isVolatile=*/false, "omsc.fp");
        // Acquire ordering synchronizes with the writer's release store,
        // ensuring the fn-ptr is fully visible on all architectures.
        fpLoad->setOrdering(llvm::AtomicOrdering::Acquire);
        auto* isHot = B.CreateICmpNE(fpLoad, llvm::Constant::getNullValue(ptrTy), "omsc.is_hot");
        llvm::MDBuilder mdB(ctx);
        B.CreateCondBr(isHot, hotCallBB, countBB, mdB.createBranchWeights(99, 1));

        // --- omsc.hot_call: fast path — call through optimized fn-ptr ---
        // After background recompilation, this is the only path taken.
        // Overhead: atomic load + null check + indirect call + ret.
        // The atomic monotonic load allows the CPU to pipeline efficiently
        // and the LLVM O0 backend to schedule optimally.
        B.SetInsertPoint(hotCallBB);
        {
            llvm::SmallVector<llvm::Value*, 8> args;
            for (auto& arg : fn->args())
                args.push_back(&arg);
            auto* hotCall = B.CreateCall(fn->getFunctionType(), fpLoad, args, "omsc.hot_r");
            B.CreateRet(hotCall);
        }

        // --- omsc.count: increment counter and check tier threshold ---
        // The cold path only runs before the first recompile (calls 1-5).
        // Using a plain (non-atomic) counter since the program is single-threaded.
        B.SetInsertPoint(countBB);
        auto* oldCnt = B.CreateLoad(i64Ty, counterGV, "omsc.old");
        auto* newCnt = B.CreateAdd(oldCnt, llvm::ConstantInt::get(i64Ty, 1), "omsc.new");
        B.CreateStore(newCnt, counterGV);

        // Check whether the call count hits the recompilation threshold.
        auto* hitT2 =
            B.CreateICmpEQ(newCnt, llvm::ConstantInt::get(i64Ty, AdaptiveJITRunner::kTier2Threshold), "omsc.hit");
        B.CreateCondBr(hitT2, recompileBB, &origEntry);

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

    if (verbose_)
        std::cout << "JIT: serialising module to bitcode..." << std::endl;

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
    if (verbose_)
        std::cout << "JIT: injecting call-count dispatch prologs (Tier-1)..." << std::endl;
    injectCounters(*instrMod);

    // Verify the instrumented IR is well-formed before handing it to MCJIT.
    // Only run verification in verbose mode to save startup time in production.
    // The counter injection is well-tested and deterministic.
    if (verbose_) {
        if (llvm::verifyModule(*instrMod, &llvm::errs())) {
            llvm::errs() << "omsc: internal error: instrumented IR failed verification\n";
            return 1;
        }
    }

    // Step 4: JIT-compile the instrumented module using ORC LLJIT.
    // Use O0 codegen for Tier-1 so the initial native code compiles as fast
    // as possible.  Tier-1 code is replaced by the Tier-2 recompile after
    // just a few calls, so spending any time on optimization at startup is
    // wasted — the code will be thrown away.  O0 gives the fastest possible
    // ORC LLJIT compilation time, minimizing startup latency.
    if (verbose_)
        std::cout << "JIT: compiling instrumented module (Tier-1, O0)..." << std::endl;

    // Create an ORC LLJIT instance with O0 backend codegen.
    auto JTMB1 = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!JTMB1) {
        llvm::errs() << "omsc: JIT engine creation failed: " << llvm::toString(JTMB1.takeError()) << "\n";
        return 1;
    }
    setJTMBOptLevel(*JTMB1, /*aggressive=*/false);

    auto jitOrErr = llvm::orc::LLJITBuilder()
                        .setJITTargetMachineBuilder(std::move(*JTMB1))
                        .create();
    if (!jitOrErr) {
        llvm::errs() << "omsc: JIT engine creation failed: " << llvm::toString(jitOrErr.takeError()) << "\n";
        return 1;
    }
    JitModule jm;
    jm.lljit = std::move(*jitOrErr);
    auto* rawLLJIT = jm.lljit.get();

    // Register C-linkage callbacks as absolute symbols so JIT'd code can
    // resolve them without -rdynamic.  They live in this process but are
    // not exported in the dynamic symbol table of a non-rdynamic binary.
    {
        auto& ES  = rawLLJIT->getExecutionSession();
        auto& mainJD = rawLLJIT->getMainJITDylib();

        llvm::orc::SymbolMap syms;
        syms[ES.intern("__omsc_adaptive_recompile")] = {
            llvm::orc::ExecutorAddr(reinterpret_cast<uint64_t>(&__omsc_adaptive_recompile)),
            llvm::JITSymbolFlags(llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable)};
        syms[ES.intern("__omsc_deopt_guard_fail")] = {
            llvm::orc::ExecutorAddr(reinterpret_cast<uint64_t>(&__omsc_deopt_guard_fail)),
            llvm::JITSymbolFlags(llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable)};
        if (auto err = mainJD.define(llvm::orc::absoluteSymbols(std::move(syms)))) {
            llvm::errs() << "omsc: failed to define JIT symbols: " << llvm::toString(std::move(err)) << "\n";
            return 1;
        }

        // Add a process-symbol generator so the JIT'd code can resolve libc
        // functions (printf, malloc, etc.) without needing -rdynamic.
        auto procGen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            rawLLJIT->getDataLayout().getGlobalPrefix());
        if (!procGen) {
            llvm::errs() << "omsc: failed to create process symbol generator: "
                         << llvm::toString(procGen.takeError()) << "\n";
            return 1;
        }
        mainJD.addGenerator(std::move(*procGen));
    }

    // Add the instrumented module to ORC. ThreadSafeModule takes ownership
    // of both the module and its LLVMContext.
    if (auto err = rawLLJIT->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(instrMod), std::move(instrCtx)))) {
        llvm::errs() << "omsc: failed to add IR module to JIT: " << llvm::toString(std::move(err)) << "\n";
        return 1;
    }

    // Trigger compilation by looking up main(). ORC materialises the entire
    // module on the first lookup, so all globals (__omsc_fn_*) are also
    // available in the symbol table immediately after this call returns.
    auto mainSymOrErr = rawLLJIT->lookup("main");
    if (!mainSymOrErr) {
        llvm::errs() << "omsc: JIT lookup of 'main' failed: " << llvm::toString(mainSymOrErr.takeError()) << "\n";
        return 1;
    }
    uint64_t mainAddr = mainSymOrErr->getValue();

    // --- Eager background compilation ---
    // Resolve every __omsc_fn_<name> slot from the Tier-1 LLJIT and
    // immediately enqueue ALL non-main user functions for background O3
    // recompilation.  This starts compiling optimised versions before
    // main() even begins executing, so by the time hot functions are
    // first called the optimised code is already (or nearly) available.
    // The dispatch prolog's atomic acquire load will pick up the fn-ptr
    // as soon as the background thread patches it via atomic release store.
    //
    // Start the background compilation thread pool FIRST so workers are
    // ready to consume tasks immediately.
    shutdownRequested_.store(false, std::memory_order_relaxed);
    bgThreads_.reserve(kNumBgThreads);
    for (int i = 0; i < kNumBgThreads; i++)
        bgThreads_.emplace_back(&AdaptiveJITRunner::backgroundWorker, this);

    {
        // Walk the clean bitcode to discover function names, then resolve
        // each function's __omsc_fn_* slot address via LLJIT lookup.
        // (The clean bitcode has the same user functions as the instrumented
        // module, minus the dispatch prolog globals.)
        auto eagerCtx = std::make_unique<llvm::LLVMContext>();
        auto eagerBuf = llvm::MemoryBuffer::getMemBuffer(
            llvm::StringRef(cleanBitcode_.data(), cleanBitcode_.size()),
            "omsc_eager_bc", /*RequiresNullTerminator=*/false);
        auto eagerMod = llvm::parseBitcodeFile(eagerBuf->getMemBufferRef(), *eagerCtx);
        if (!eagerMod) {
            // Log warning — eager compilation cannot proceed, but threshold-
            // triggered recompilation will still work as a fallback.
            if (verbose_)
                llvm::errs() << "JIT: warning: could not parse clean bitcode for "
                                "eager compilation; falling back to threshold-triggered\n";
            llvm::consumeError(eagerMod.takeError());
        } else if (eagerMod) {
            for (auto& fn : **eagerMod) {
                if (fn.isDeclaration() || fn.getName() == "main")
                    continue;
                if (fn.getName().starts_with("__"))
                    continue;
                std::string name = fn.getName().str();
                std::string slotName = "__omsc_fn_" + name;

                // Look up the slot global from the already-compiled Tier-1
                // module. Because __omsc_fn_* has ExternalLinkage it is
                // accessible via LLJIT lookup().
                auto slotOrErr = rawLLJIT->lookup(slotName);
                if (!slotOrErr) {
                    llvm::consumeError(slotOrErr.takeError());
                    continue;
                }
                void** fnPtrSlot = reinterpret_cast<void**>(slotOrErr->getValue());

                // Mark as tier-1 (eagerly queued, not yet compiled) instead of
                // tier-2.  This lets onHotFunction() later re-enqueue the same
                // function with the ACTUAL call count as the priority key.  Since
                // the priority queue is a max-heap, a threshold-triggered entry
                // (callCount >= kTier2Threshold, observed at runtime) naturally
                // preempts this eager entry (callCount == kTier2Threshold, synthetic),
                // so hot functions compile before cold ones.
                {
                    std::lock_guard<std::mutex> lk(recompiledMtx_);
                    functionTier_[name] = 1; // "eagerly queued, not yet compiled"
                }

                // Enqueue eagerly — a background thread will compile it.
                {
                    std::lock_guard<std::mutex> lk(queueMtx_);
                    taskQueue_.push({name, kTier2Threshold, fnPtrSlot});
                }
                queueCV_.notify_one();
            }
        }
    }

    {
        std::lock_guard<std::mutex> lk(recompiledMtx_);
        modules_.push_back(std::move(jm));
    }

    // Step 5: Execute main() in-process.
    // Register this runner as the active one so that the C-linkage callback
    // can reach it during execution.  RAII guard ensures the pointer is
    // cleared even if main() exits via an exception.
    if (verbose_)
        std::cout << "JIT: executing program..." << std::endl;

    // Background thread pool was already started above (eager compilation).

    g_activeRunner.store(this, std::memory_order_release);
    struct RunnerGuard {
        AdaptiveJITRunner* self;
        ~RunnerGuard() {
            g_activeRunner.store(nullptr, std::memory_order_release);
            // Drain the background thread — wait for all pending compilations
            // to finish before tearing down JIT modules.
            self->drainBackgroundThread();
        }
    } runnerGuard{this};
    using OmscMainFn = int64_t (*)();
    int64_t exitVal = reinterpret_cast<OmscMainFn>(mainAddr)();

    // Mirror what a process exit code would look like (0-255).
    return static_cast<int>(exitVal & 0xFF);
}

// ---------------------------------------------------------------------------
// tierForCallCount() — map a call count to its JIT tier
// ---------------------------------------------------------------------------
int AdaptiveJITRunner::tierForCallCount(int64_t count) {
    if (count >= kTier3Threshold)
        return 3;
    if (count >= kTier2Threshold)
        return 2;
    return 0;
}

// ---------------------------------------------------------------------------
// createTargetMachine() — thread-safe TargetMachine factory
// ---------------------------------------------------------------------------
// Creates the TargetMachine once (detecting host CPU, features, triple)
// and returns the cached instance on subsequent calls.  Saves ~1-3ms per
// recompilation by avoiding repeated target lookup and feature detection.
// Thread-safe factory that creates a new TargetMachine per call.
// The host CPU, features, and triple are detected once (via call_once)
// and cached; subsequent calls just create a fresh TM from the cached
// strings.  Each background thread gets its own TM.
std::unique_ptr<llvm::TargetMachine> AdaptiveJITRunner::createTargetMachine() {
    std::call_once(tmInitFlag_, [this] {
        cachedTriple_ = llvm::sys::getDefaultTargetTriple();
        cachedCPU_ = llvm::sys::getHostCPUName().str();
        llvm::SubtargetFeatures featureSet;
#if LLVM_VERSION_MAJOR >= 19
        llvm::StringMap<bool> hostFeatures = llvm::sys::getHostCPUFeatures();
#else
        llvm::StringMap<bool> hostFeatures;
        llvm::sys::getHostCPUFeatures(hostFeatures);
#endif
        for (auto& kv : hostFeatures)
            featureSet.AddFeature(kv.first(), kv.second);
        cachedFeatures_ = featureSet.getString();
    });

    std::string errStr;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(cachedTriple_, errStr);
    if (!target) {
        llvm::errs() << "omsc: could not lookup target: " << errStr << "\n";
        return nullptr;
    }
    llvm::TargetOptions opts;
    opts.EnableFastISel = false; // force optimising ISel for best codegen
    if (fastMath_) {
#if LLVM_VERSION_MAJOR < 22
        opts.UnsafeFPMath = true;
#endif
        opts.NoInfsFPMath = true;
        opts.NoNaNsFPMath = true;
        opts.NoSignedZerosFPMath = true;
    }
    std::unique_ptr<llvm::TargetMachine> TM;
#if LLVM_VERSION_MAJOR >= 21
    TM.reset(target->createTargetMachine(llvm::Triple(cachedTriple_), cachedCPU_, cachedFeatures_, opts,
                                         std::optional<llvm::Reloc::Model>()));
#else
    TM.reset(target->createTargetMachine(cachedTriple_, cachedCPU_, cachedFeatures_, opts,
                                         std::optional<llvm::Reloc::Model>()));
#endif
    return TM;
}

// ---------------------------------------------------------------------------
// onHotFunction() — non-blocking recompilation enqueue
// ---------------------------------------------------------------------------
// Called from the dispatch prolog when a function's call count hits the tier
// threshold.  Enqueues the function into the PRIORITY QUEUE with the actual
// runtime call count as the priority key so that the background threads always
// compile the hottest functions first.
//
// Two scenarios:
//   1. Function was NOT eagerly queued (functionTier_ == 0): standard path,
//      same as before.
//   2. Function WAS eagerly queued (functionTier_ == 1, "queued but not yet
//      compiled"): we are now called with the ACTUAL runtime callCount which
//      is >= kTier2Threshold.  We promote it to tier-2, enqueue a new entry
//      with the real callCount.  The old eager entry (callCount = kTier2Threshold)
//      will be found in the priority queue at LOWER priority and will be skipped
//      by backgroundWorker once it sees the fn-ptr slot already set.
//
// Uses try_lock to be completely non-blocking: if either lock is contended,
// skip the enqueue entirely rather than stalling the main execution thread.
// The function continues running baseline code.
void AdaptiveJITRunner::onHotFunction(const char* name, int64_t callCount, void** fnPtrSlot) {
    const std::string funcName(name);

    int targetTier = tierForCallCount(callCount);
    if (targetTier == 0)
        return; // Below all thresholds — nothing to do.

    int prevTier = 0;
    {
        // Non-blocking: if the lock is held by a bg thread, skip entirely.
        std::unique_lock<std::mutex> lk(recompiledMtx_, std::try_to_lock);
        if (!lk.owns_lock())
            return; // Lock contended — don't block the main thread.
        prevTier = functionTier_[funcName]; // 0 if never seen, 1 if eagerly queued
        // Already compiled (tier >= 2): nothing to do.
        // Tier-1 means "eagerly queued but not yet compiled" — allow promotion
        // to tier-2 with the real callCount so the hot entry preempts the eager
        // low-priority entry already in the queue.
        if (prevTier >= 2)
            return;
        // Tentatively promote to prevent duplicate enqueues from concurrent hits.
        functionTier_[funcName] = targetTier;
    }

    // Non-blocking enqueue: if the queue lock is held, rollback the tier
    // promotion so the function can be recompiled on a future threshold hit.
    {
        std::unique_lock<std::mutex> lk(queueMtx_, std::try_to_lock);
        if (!lk.owns_lock()) {
            // Rollback: task was never enqueued — revert so next hit can retry.
            std::lock_guard<std::mutex> rlk(recompiledMtx_);
            functionTier_[funcName] = prevTier;
            return;
        }
        // Push with ACTUAL callCount as priority key: hotter functions sort to
        // the top of the max-heap and compile before cooler ones.
        taskQueue_.push({funcName, callCount, fnPtrSlot});
    }
    queueCV_.notify_one();
}
// ---------------------------------------------------------------------------
// backgroundWorker() — priority-ordered background compilation thread loop
// ---------------------------------------------------------------------------
// Waits for tasks on the priority queue and processes them in hottest-first
// order (highest callCount = most-called function = compiled first).
//
// Duplicate-skip: a function may appear twice in the queue when
// onHotFunction() adds a high-priority entry for a function that was already
// eagerly enqueued at low priority.  The eager entry is harmlessly skipped
// once it reaches the top of the heap and we see the fn-ptr slot is already
// set by the earlier, higher-priority compilation.
void AdaptiveJITRunner::backgroundWorker() {
    while (true) {
        RecompileTask task;
        {
            std::unique_lock<std::mutex> lk(queueMtx_);
            queueCV_.wait(lk, [this] {
                return !taskQueue_.empty() || shutdownRequested_.load(std::memory_order_relaxed);
            });
            if (taskQueue_.empty() && shutdownRequested_.load(std::memory_order_relaxed))
                return; // Shutdown requested and no more work.
            // priority_queue::top() returns const ref; copy before pop.
            task = taskQueue_.top();
            taskQueue_.pop();
        }

        // Duplicate-skip (first guard — cheap): if the fn-ptr slot is already
        // non-null, a higher-priority entry already compiled and hot-patched
        // this function.  Skip before touching LLVM to avoid redundant work.
        // doRecompile() contains a matching second guard for the rare case
        // where the slot is patched between this check and compilation start.
        if (task.fnPtrSlot != nullptr &&
            __atomic_load_n(task.fnPtrSlot, __ATOMIC_ACQUIRE) != nullptr) {
            if (verbose_)
                std::cerr << "JIT: skipping duplicate queue entry for '" << task.funcName
                          << "' (already compiled by higher-priority entry)\n";
            continue;
        }

        // Catch any exceptions from LLVM to prevent the background thread
        // from crashing and hanging the main thread at shutdown (join).
        try {
            doRecompile(task.funcName, task.callCount, task.fnPtrSlot);
        } catch (const std::exception& e) {
            std::cerr << "JIT: background recompile of '" << task.funcName
                      << "' threw exception: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "JIT: background recompile of '" << task.funcName
                      << "' threw unknown exception\n";
        }
    }
}

// ---------------------------------------------------------------------------
// drainBackgroundThread() — stop the background worker pool cleanly
// ---------------------------------------------------------------------------
void AdaptiveJITRunner::drainBackgroundThread() {
    shutdownRequested_.store(true, std::memory_order_relaxed);
    queueCV_.notify_all();  // Wake all worker threads
    for (auto& t : bgThreads_) {
        if (t.joinable())
            t.join();
    }
    bgThreads_.clear();
}

// ---------------------------------------------------------------------------
// doRecompile() — multi-tier PGO recompilation (runs on background thread)
// ---------------------------------------------------------------------------
// Performs the actual heavy work of recompilation: parsing clean bitcode,
// applying PGO annotations, running the O3 pipeline, ORC LLJIT compilation,
// and atomically storing the new function pointer.
//
// Priority-queue interaction:
//   When onHotFunction() adds a high-priority entry for a function that was
//   already eagerly queued at low priority, both entries eventually reach
//   doRecompile().  The high-priority one runs first (by design of the max-
//   heap).  When the low-priority eager entry later fires, the fn-ptr slot
//   is already non-null (set by the first run), so we return early without
//   repeating the expensive compilation work.
void AdaptiveJITRunner::doRecompile(const std::string& funcName, int64_t callCount, void** fnPtrSlot) {

    int targetTier = tierForCallCount(callCount);
    if (targetTier == 0)
        return;

    // Duplicate-skip (second guard — safety net): handles the rare window
    // between backgroundWorker's first guard and the start of compilation
    // where another thread could have patched the slot.  Acquiring the
    // pointer here also synchronises with the release store in the hot-patch
    // so that any memory written by the other compilation is visible.
    if (fnPtrSlot != nullptr && __atomic_load_n(fnPtrSlot, __ATOMIC_ACQUIRE) != nullptr) {
        // Ensure tier reflects the completed compilation even if we skip.
        std::lock_guard<std::mutex> lk(recompiledMtx_);
        if (functionTier_[funcName] < targetTier)
            functionTier_[funcName] = targetTier;
        return;
    }

    const char* tierNames[] = {"", "", "Tier-2 (warm)", "Tier-3 (hot)"};
    const char* tierLabel =
        (targetTier >= 2 && targetTier <= AdaptiveJITRunner::kMaxTier) ? tierNames[targetTier] : "Tier-?";
    if (verbose_) {
        std::cerr << "JIT: recompiling '" << funcName << "' (calls: " << callCount << ") at " << tierLabel
                  << " O3+PGO [background]\n";
    }

    // RAII scope guard: on early return (failure), revert the tier so that
    // a future threshold hit can retry.  Dismissed (via succeeded=true)
    // only after a successful hot-patch write.
    bool succeeded = false;
    int prevTier = 0;
    {
        std::lock_guard<std::mutex> lk(recompiledMtx_);
        prevTier = functionTier_[funcName];
    }
    struct ScopeExit {
        bool& flag;
        std::mutex& mtx;
        std::unordered_map<std::string, int>& tierMap;
        const std::string& key;
        int rollbackTier;
        ~ScopeExit() {
            if (!flag) {
                std::lock_guard<std::mutex> lk(mtx);
                tierMap[key] = rollbackTier;
            }
        }
    } rollbackGuard{succeeded, recompiledMtx_, functionTier_, funcName, prevTier};

    // --- Parse clean bitcode into a fresh context ---
    auto newCtx = std::make_unique<llvm::LLVMContext>();
    // Suppress all LLVM diagnostics to avoid wasted time formatting diagnostic
    // strings.  In verbose mode, forward warnings/errors; in production mode,
    // discard everything for maximum speed.
    if (verbose_) {
        newCtx->setDiagnosticHandlerCallBack(
            [](const llvm::DiagnosticInfo& DI, void*) {
                if (DI.getSeverity() == llvm::DS_Remark)
                    return;
                llvm::DiagnosticPrinterRawOStream DP(llvm::errs());
                DI.print(DP);
                llvm::errs() << "\n";
            },
            nullptr);
    } else {
        newCtx->setDiagnosticHandlerCallBack([](const llvm::DiagnosticInfo&, void*) {}, nullptr);
    }
    auto memBuf = llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(cleanBitcode_.data(), cleanBitcode_.size()),
                                                   "omsc_pgo_bc", /*RequiresNullTerminator=*/false);
    auto modOrErr = llvm::parseBitcodeFile(memBuf->getMemBufferRef(), *newCtx);
    if (!modOrErr) {
        llvm::errs() << "omsc: Tier-2 recompile of '" << funcName << "' failed: could not parse bitcode\n";
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
        return;
    }
    fn->setEntryCount(static_cast<uint64_t>(callCount));
    // Mark the hot function with the `hot` attribute so LLVM's inliner,
    // branch layout, and code placement passes treat it with maximum priority.
    // This enables hot/cold splitting (moving error paths out of the hot region),
    // more aggressive inlining into this function, and better I-cache layout.
    fn->addFnAttr(llvm::Attribute::Hot);
    for (auto& other : *mod) {
        if (!other.isDeclaration() && other.getName() != funcName && !other.getEntryCount())
            other.setEntryCount(1);
    }

    // --- Snapshot profile data for thread safety ---
    // The main thread continues writing to the profiler while we read on the
    // background thread.  Take a deep copy of the profile data inside the
    // profiler's lock so all subsequent reads are race-free.
    std::unique_ptr<FunctionProfile> profSnapshot;
    {
        const FunctionProfile* rawProf = JITProfiler::instance().getProfile(funcName);
        if (rawProf)
            profSnapshot = std::make_unique<FunctionProfile>(*rawProf);
    }

    // --- Apply collected branch profile data as LLVM branch weights ---
    // The JITProfiler records taken/not-taken counts for each branch site
    // during the warm-up phase.  Attaching these as branch weight metadata
    // guides the code layout, branch predictor hints, and LLVM's hot/cold
    // splitting passes in the O3 pipeline.
    {
        const FunctionProfile* prof = profSnapshot.get();
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

    // --- Apply argument-type-driven function attributes ---
    // When profiling shows that all observed argument values are integers
    // (the dominant type), annotate parameters with LLVM attributes that
    // enable stronger optimization:
    //   - `noundef`: the value is always well-defined (not poison/undef),
    //     enabling value-range propagation and dead-code elimination.
    //   - `signext`: the value is sign-extended from a narrower type, which
    //     helps the backend select better instruction sequences.
    // These attributes feed into LLVM's IPSCCP, instcombine, and inliner
    // to produce tighter specialized code for the hot path.
    {
        const FunctionProfile* prof = profSnapshot.get();
        if (prof) {
            for (size_t i = 0; i < prof->args.size() && i < fn->arg_size(); i++) {
                const auto& ap = prof->args[i];
                if (ap.dominantType() == ArgType::Integer && ap.totalCalls > 0) {
                    // >90% integer → mark parameter as always-defined integer
                    // ap.totalCalls is guaranteed > 0 by the guard above.
                    double intRatio = static_cast<double>(ap.typeCounts[static_cast<uint8_t>(ArgType::Integer)]) /
                                      static_cast<double>(ap.totalCalls);
                    if (intRatio > 0.9) {
                        fn->addParamAttr(static_cast<unsigned>(i), llvm::Attribute::NoUndef);
                        fn->addParamAttr(static_cast<unsigned>(i), llvm::Attribute::SExt);
                    }
                }
            }
        }
    }

    // --- Constant specialization via llvm.assume ---
    // When profiling shows that >80% of calls pass the same integer constant
    // for a parameter, inject `llvm.assume(arg == constant)` at function entry.
    // LLVM's IPSCCP and instcombine see the assumption and propagate the
    // constant through the function body, enabling dead-branch elimination,
    // constant folding in expressions, and loop trip-count computation — all
    // without creating a separate specialized clone.
    //
    // At Tier-3, also use the top-K value frequency tracker: if a single
    // value dominates >60% of calls, inject the same assumption.  This catches
    // cases where the constant-count tracker was confused by early non-constant
    // calls but the top-K tracker correctly identified the dominant value.
    {
        const FunctionProfile* prof = profSnapshot.get();
        if (prof && !fn->empty()) {
            auto* i64Ty = llvm::Type::getInt64Ty(fn->getContext());
            // Declare @llvm.assume(i1) intrinsic.
#if LLVM_VERSION_MAJOR >= 19
            llvm::Function* assumeFn = llvm::Intrinsic::getOrInsertDeclaration(mod.get(), llvm::Intrinsic::assume);
#else
            llvm::Function* assumeFn = llvm::Intrinsic::getDeclaration(mod.get(), llvm::Intrinsic::assume);
#endif
            llvm::IRBuilder<> asB(&fn->getEntryBlock().front());
            for (size_t i = 0; i < prof->args.size() && i < fn->arg_size(); i++) {
                const auto& ap = prof->args[i];
                auto* arg = fn->getArg(static_cast<unsigned>(i));
                // hasConstantSpecialization() checks profile data (>80% same
                // value); the IR type check ensures the LLVM parameter is i64
                // so we can safely emit an ICmpEQ + ConstantInt.
                if (ap.hasConstantSpecialization() && arg->getType() == i64Ty) {
                    auto* cmp = asB.CreateICmpEQ(arg,
                                                 llvm::ConstantInt::get(i64Ty, ap.observedConstant), "omsc.const_spec");
                    asB.CreateCall(assumeFn, {cmp});
                } else if (targetTier >= 3 && ap.hasDominantValue() &&
                           arg->getType() == i64Ty) {
                    // Top-K fallback: dominant value detected at Tier-3.
                    auto* cmp = asB.CreateICmpEQ(arg,
                                                 llvm::ConstantInt::get(i64Ty, ap.dominantValue()),
                                                 "omsc.topk_spec");
                    asB.CreateCall(assumeFn, {cmp});
                }
            }
        }
    }

    // --- Range-based value specialization via llvm.assume ---
    // When profiling shows that >90% of an argument's observed values are
    // integers falling within a tight range [min, max] (width <= 1024),
    // inject `llvm.assume(arg >= min)` and `llvm.assume(arg <= max)` at
    // function entry.  This enables LLVM to:
    //   - Eliminate bounds/range checks that fall within the known range
    //   - Use narrower integer operations where the range permits
    //   - Simplify comparisons involving the parameter
    //   - Enable better loop trip count estimation when the arg is a bound
    // This is only applied when constant specialization was NOT already
    // injected (to avoid redundant assumptions).
    {
        const FunctionProfile* prof = profSnapshot.get();
        if (prof && !fn->empty()) {
            auto* i64Ty = llvm::Type::getInt64Ty(fn->getContext());
#if LLVM_VERSION_MAJOR >= 19
            llvm::Function* assumeFn = llvm::Intrinsic::getOrInsertDeclaration(mod.get(), llvm::Intrinsic::assume);
#else
            llvm::Function* assumeFn = llvm::Intrinsic::getDeclaration(mod.get(), llvm::Intrinsic::assume);
#endif
            llvm::IRBuilder<> rgB(&fn->getEntryBlock().front());
            for (size_t i = 0; i < prof->args.size() && i < fn->arg_size(); i++) {
                const auto& ap = prof->args[i];
                // Only apply range assumptions when:
                //   1. We have a tight observed range
                //   2. Constant specialization was NOT applied (avoid redundancy)
                //   3. The LLVM parameter type is i64
                if (ap.hasRangeSpecialization() && !ap.hasConstantSpecialization() &&
                    fn->getArg(static_cast<unsigned>(i))->getType() == i64Ty) {
                    auto* arg = fn->getArg(static_cast<unsigned>(i));
                    // assume(arg >= minObserved)
                    auto* geMin = rgB.CreateICmpSGE(arg, llvm::ConstantInt::get(i64Ty, ap.minObserved),
                                                    "omsc.range_ge_min");
                    rgB.CreateCall(assumeFn, {geMin});
                    // assume(arg <= maxObserved)
                    auto* leMax = rgB.CreateICmpSLE(arg, llvm::ConstantInt::get(i64Ty, ap.maxObserved),
                                                    "omsc.range_le_max");
                    rgB.CreateCall(assumeFn, {leMax});
                }
            }
        }
    }

    // --- Aggressive inlining of all direct callees ---
    // Since we no longer collect call-site frequency data (removed for
    // performance), we aggressively inline ALL direct callees of the hot
    // function.  This is justified because:
    //   1. The function has been called enough times to warrant recompilation
    //   2. OmScript programs are typically small enough that inlining everything
    //      doesn't cause excessive code bloat
    //   3. Inlining exposes constant folding, dead code elimination, and
    //      loop optimization opportunities that cross function boundaries
    {
        for (auto& bb : *fn) {
            for (auto& inst : bb) {
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                    if (auto* callee = call->getCalledFunction()) {
                        if (!callee->isDeclaration() && !callee->isIntrinsic() &&
                            callee != fn) {
                            callee->addFnAttr(llvm::Attribute::AlwaysInline);
                            callee->removeFnAttr(llvm::Attribute::Cold);
                            callee->removeFnAttr(llvm::Attribute::NoInline);
                        }
                    }
                }
            }
        }
    }

    // --- Strip functions unreachable from the hot function ---
    // The clean bitcode contains ALL program functions, but Tier-2 only needs
    // the hot function and its transitive callees.  Removing everything else
    // dramatically reduces the work the O3 pipeline has to do (often 5-10x
    // fewer functions to analyze), which directly cuts recompilation time.
    {
        llvm::SmallPtrSet<llvm::Function*, 16> reachable;
        llvm::SmallVector<llvm::Function*, 16> worklist;
        worklist.push_back(fn);
        while (!worklist.empty()) {
            llvm::Function* f = worklist.pop_back_val();
            if (!reachable.insert(f).second)
                continue;
            for (auto& bb : *f) {
                for (auto& inst : bb) {
                    if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                        if (auto* callee = call->getCalledFunction()) {
                            if (!callee->isDeclaration())
                                worklist.push_back(callee);
                        }
                    }
                }
            }
        }
        // Delete bodies of unreachable functions (turn them into declarations).
        for (auto& other : *mod) {
            if (other.isDeclaration() || reachable.count(&other))
                continue;
            other.deleteBody();
        }
    }

    // --- Mark non-hot reachable functions as cold ---
    // Tell LLVM's inliner to avoid inlining cold callees into the hot function
    // (saving I-cache space) and guide the code layout pass to place cold code
    // away from the hot region, reducing I-TLB and I-cache pressure.
    for (auto& other : *mod) {
        if (other.isDeclaration() || &other == fn)
            continue;
        if (!other.hasFnAttribute(llvm::Attribute::Hot))
            other.addFnAttr(llvm::Attribute::Cold);
    }

    // --- Attach loop vectorization, interleaving, and unroll metadata ---
    // The AOT pipeline attaches llvm.loop.mustprogress, interleave.count=4,
    // and vectorize.width=4 hints to every loop back-edge.  However, when
    // Tier-2 re-parses the clean bitcode these metadata nodes are absent
    // because the clean IR comes from the compiler frontend (which may have
    // been compiled at < O2).  Re-injecting the hints here ensures the O3
    // loop vectorizer and unroller treat every loop in the hot function as
    // a SIMD candidate, matching the AOT code quality.
    //
    // Additionally, when loop trip count profiling data is available, we
    // attach llvm.loop.unroll.count metadata with a profile-guided unroll
    // factor.  Small trip counts (<=16) get full unrolling; medium trip
    // counts (<=64) get unroll-by-4; larger loops keep the default strategy.
    {
        auto& ctx = fn->getContext();
        const FunctionProfile* loopProf = profSnapshot.get();
        // Build a set of blocks we've visited so far to detect back-edges in
        // O(n) total time instead of O(n²) per-branch inner scan.
        llvm::SmallPtrSet<llvm::BasicBlock*, 32> visited;
        uint32_t loopIdx = 0;
        for (auto& bb : *fn) {
            visited.insert(&bb);
            auto* term = bb.getTerminator();
            if (!term)
                continue;
            auto* br = llvm::dyn_cast<llvm::BranchInst>(term);
            if (!br)
                continue;
            // Detect loop back-edges: a branch to a block that appears
            // earlier in the function's block list (already in visited set).
            bool isBackEdge = false;
            for (unsigned s = 0; s < br->getNumSuccessors(); s++) {
                if (visited.count(br->getSuccessor(s))) {
                    isBackEdge = true;
                    break;
                }
            }
            if (!isBackEdge)
                continue;
            // Skip branches that already have loop metadata.
            if (br->getMetadata(llvm::LLVMContext::MD_loop)) {
                loopIdx++;
                continue;
            }
            llvm::MDNode* mustProgress = llvm::MDNode::get(ctx, {llvm::MDString::get(ctx, "llvm.loop.mustprogress")});
            llvm::MDNode* interleave = llvm::MDNode::get(
                ctx, {llvm::MDString::get(ctx, "llvm.loop.interleave.count"),
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 8))});
            llvm::MDNode* vecWidth = llvm::MDNode::get(
                ctx, {llvm::MDString::get(ctx, "llvm.loop.vectorize.width"),
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 8))});
            llvm::SmallVector<llvm::Metadata*, 6> loopMDs;
            loopMDs.push_back(nullptr); // self-reference placeholder
            loopMDs.push_back(mustProgress);
            loopMDs.push_back(interleave);
            loopMDs.push_back(vecWidth);

            // Attach profile-guided unroll count when trip count data is available.
            // Use the new constant-trip-count and narrow-range data for more
            // aggressive unrolling decisions.
            if (loopProf && loopIdx < loopProf->loops.size()) {
                const auto& lp = loopProf->loops[loopIdx];
                uint64_t avgTrip = lp.averageTripCount();
                uint32_t unrollCount = 0;
                bool fullUnroll = false;

                if (lp.hasConstantTripCount() &&
                    static_cast<uint64_t>(lp.constantTripCount) <= 64) {
                    // Constant trip count: fully unroll small loops for maximum
                    // optimization (eliminates loop overhead, enables constant
                    // propagation of induction variables).
                    auto ctc = static_cast<uint32_t>(lp.constantTripCount);
                    if (ctc > 0 && ctc <= 32) {
                        unrollCount = ctc;
                        fullUnroll = true;
                    } else if (ctc > 32) {
                        // 33-64: unroll by 8 for ILP without excessive code bloat.
                        unrollCount = 8;
                    }
                } else if (lp.hasNarrowTripRange() && lp.maxTripCount <= 32) {
                    // Narrow range of small trip counts: fully unroll to max.
                    unrollCount = static_cast<uint32_t>(lp.maxTripCount);
                    fullUnroll = true;
                } else if (avgTrip > 0 && avgTrip <= 16) {
                    // Small trip count: fully unroll by the trip count.
                    unrollCount = static_cast<uint32_t>(avgTrip);
                    fullUnroll = true;
                } else if (avgTrip > 16 && avgTrip <= 64) {
                    // Medium trip count: unroll by 8 for ILP.
                    unrollCount = 8;
                } else if (avgTrip > 64) {
                    // Large trip count: unroll by 16 for maximum ILP —
                    // JIT can afford larger code since it only processes
                    // hot functions (stripped module).
                    unrollCount = 16;
                }
                if (unrollCount > 0) {
                    if (fullUnroll) {
                        // Use llvm.loop.unroll.full for constant-trip loops
                        // that should be completely unrolled.
                        llvm::MDNode* unrollFullMD = llvm::MDNode::get(
                            ctx, {llvm::MDString::get(ctx, "llvm.loop.unroll.full")});
                        loopMDs.push_back(unrollFullMD);
                    }
                    llvm::MDNode* unrollMD = llvm::MDNode::get(
                        ctx, {llvm::MDString::get(ctx, "llvm.loop.unroll.count"),
                              llvm::ConstantAsMetadata::get(
                                  llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), unrollCount))});
                    loopMDs.push_back(unrollMD);
                }
                // For large-trip-count loops (>64), enable vectorization.
                if (avgTrip > 64) {
                    llvm::MDNode* vecEnable = llvm::MDNode::get(
                        ctx, {llvm::MDString::get(ctx, "llvm.loop.vectorize.enable"),
                              llvm::ConstantAsMetadata::get(
                                  llvm::ConstantInt::get(llvm::Type::getInt1Ty(ctx), 1))});
                    loopMDs.push_back(vecEnable);
                }
            }

            llvm::MDNode* loopMD = llvm::MDNode::get(ctx, loopMDs);
            loopMD->replaceOperandWith(0, loopMD);
            br->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
            loopIdx++;
        }
    }

    // --- Propagate fast-math IR flags to all floating-point instructions ---
    // When the user passes -ffast-math, the codegen frontend sets FMF.setFast()
    // on the IRBuilder, tagging every FP instruction with nnan/ninf/nsz/arcp/
    // contract/afn/reassoc flags.  The clean bitcode preserves these flags.
    // However, llvm.assume and other injected instructions in the PGO
    // annotation above may have been inserted WITHOUT fast-math flags.
    // To ensure the O3 pipeline sees consistent FP semantics, sweep the hot
    // function and set fast-math flags on any FP instruction that doesn't
    // already have them.  This enables FMA fusion, reciprocal estimation,
    // and reassociation across the entire hot function body.
    if (fastMath_) {
        llvm::FastMathFlags FMF;
        FMF.setFast();
        for (auto& bb : *fn) {
            for (auto& inst : bb) {
                if (llvm::isa<llvm::FPMathOperator>(inst)) {
                    inst.setFastMathFlags(FMF);
                }
            }
        }
    }

    // --- Re-optimise with PGO guidance and full native CPU features ---
    // Each tier uses progressively more aggressive optimisation settings:
    //   Tier 2 (warm,   10 calls):  O2 pipeline, inliner threshold 600
    //   Tier 3 (hot,  1000 calls):  O3 pipeline, inliner threshold 1200,
    //                               double O3 pass for cascading optimizations
    //
    // User flags (-ffast-math, -fvectorize, -funroll-loops, -floop-optimize)
    // are propagated from the command line to the recompilation pipeline.
    {
        llvm::PipelineTuningOptions PTO;
        PTO.LoopVectorization = vectorize_;
        PTO.SLPVectorization = vectorize_;
        PTO.LoopUnrolling = unrollLoops_;
        PTO.LoopInterleaving = vectorize_;
        PTO.MergeFunctions = true;
        PTO.CallGraphProfile = true;
        // Use very aggressive inliner threshold — significantly higher than
        // AOT's 400.  JIT can afford this because it only optimizes the hot
        // function and its transitive callees (stripped module), while AOT
        // must process the entire program.  The extra inlining exposes more
        // constant folding, dead code elimination, and vectorization across
        // function boundaries — the key to beating AOT code quality.
        PTO.InlinerThreshold = 2000;

        // Create a TargetMachine for this recompilation.  Each background
        // thread gets its own TM since LLVM TargetMachine is not thread-safe.
        auto TM = createTargetMachine();
        if (!TM) {
            llvm::errs() << "omsc: " << tierLabel << " recompile of '" << funcName
                         << "' failed: could not create target machine\n";
            return;
        }
        mod->setDataLayout(TM->createDataLayout());

        llvm::PassBuilder PB(TM.get(), PTO);

        // Register LoopDistributePass to run just before vectorisation,
        // matching the AOT O3 pipeline.  Loop distribution splits loops with
        // multiple independent memory streams into separate loops with smaller
        // working sets, improving cache locality and enabling downstream
        // vectorisation of the resulting simpler loops.
        // Only registered when the user has not disabled loop optimization.
        if (loopOptimize_) {
            PB.registerVectorizerStartEPCallback(
                [](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) {
                    FPM.addPass(llvm::LoopDistributePass());
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

        // Use O3 for maximum code quality in the single recompile tier.
        // Combined with AlwaysInline on all callees, aggressive inliner
        // threshold (1200), and stripped unreachable functions, O3 produces
        // code that can match or beat AOT for hot functions.
        auto pipeline = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
        pipeline.run(*mod, MAM);
    }

    // Verify the module after optimization — only in verbose mode to save time.
    if (verbose_) {
        if (llvm::verifyModule(*mod, &llvm::errs())) {
            llvm::errs() << "omsc: " << tierLabel << " recompile of '" << funcName
                         << "' failed: module verification after optimization\n";
            return;
        }
    }

    // --- JIT-compile the reoptimised module using ORC LLJIT ---
    // Create a fresh LLJIT instance with Aggressive backend codegen.
    // Each recompiled module gets its own LLJIT instance so there is no
    // sharing of mutable state between concurrent background threads.
    auto JTMB2 = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!JTMB2) {
        llvm::errs() << "omsc: " << tierLabel << " JIT compilation of '" << funcName
                     << "' failed: " << llvm::toString(JTMB2.takeError()) << "\n";
        return;
    }
    setJTMBOptLevel(*JTMB2, /*aggressive=*/true);
    // Propagate fast-math options into the backend TargetMachine so that
    // FMA fusion, reciprocal estimation, and FP-reassociation are enabled
    // in the native code emitter, matching the IR-level flags set above.
    if (fastMath_) {
        auto& opts = JTMB2->getOptions();
#if LLVM_VERSION_MAJOR < 22
        opts.UnsafeFPMath = true;
#endif
        opts.NoInfsFPMath = true;
        opts.NoNaNsFPMath = true;
        opts.NoSignedZerosFPMath = true;
    }

    auto jitOrErr2 = llvm::orc::LLJITBuilder()
                         .setJITTargetMachineBuilder(std::move(*JTMB2))
                         .create();
    if (!jitOrErr2) {
        llvm::errs() << "omsc: " << tierLabel << " JIT compilation of '" << funcName
                     << "' failed: " << llvm::toString(jitOrErr2.takeError()) << "\n";
        return;
    }
    auto jit = std::move(*jitOrErr2);

    // Add a process-symbol generator so the recompiled code can resolve
    // any libc / runtime symbols it calls.
    {
        auto& mainJD = jit->getMainJITDylib();
        auto procGen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix());
        if (procGen)
            mainJD.addGenerator(std::move(*procGen));
        else
            llvm::consumeError(procGen.takeError());
    }

    // Add the optimised module. ORC takes ownership of the module and its
    // context via ThreadSafeModule (context is already owned by newCtx).
    if (auto err = jit->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(mod), std::move(newCtx)))) {
        llvm::errs() << "omsc: " << tierLabel << " JIT compilation of '" << funcName
                     << "' failed: " << llvm::toString(std::move(err)) << "\n";
        return;
    }

    // Trigger compilation and retrieve the function's native address.
    auto symOrErr = jit->lookup(funcName);
    if (!symOrErr) {
        llvm::errs() << "omsc: " << tierLabel << " JIT lookup of '" << funcName
                     << "' failed: " << llvm::toString(symOrErr.takeError()) << "\n";
        return;
    }
    uint64_t addr = symOrErr->getValue();

    // Keep the LLJIT instance alive — it owns the JIT'd code memory.
    // Also explicitly update functionTier_ to targetTier here.  This is
    // needed for eagerly-queued functions (prevTier=1) that were compiled
    // without going through onHotFunction(): without this, the tier stays at
    // 1 and onHotFunction() could re-enqueue the function unnecessarily.
    {
        std::lock_guard<std::mutex> lk(recompiledMtx_);
        modules_.push_back({std::move(jit)});
        if (functionTier_[funcName] < targetTier)
            functionTier_[funcName] = targetTier;
    }

    // Mark successful — prevents the RAII scope guard from reverting the tier.
    succeeded = true;

    // Hot-patch: use an atomic store with release ordering so the dispatch
    // prolog's atomic acquire load sees the new pointer on the very next
    // function call.  We use __atomic_store_n (GCC/Clang built-in) instead of
    // reinterpret_cast<std::atomic<void*>*> to avoid strict-aliasing UB.
    // On x86 this compiles to a simple MOV (TSO provides release semantics
    // for free); on ARM/POWER it emits the appropriate release fence.
    void* newPtr = reinterpret_cast<void*>(addr);
    __atomic_store_n(fnPtrSlot, newPtr, __ATOMIC_RELEASE);
    if (verbose_) {
        std::cerr << "JIT: recompiled '" << funcName << "' successfully (" << tierLabel << " active) [background]\n";
    }
}

} // namespace omscript

// ---------------------------------------------------------------------------
// C-linkage entry point — called from injected LLVM IR
// ---------------------------------------------------------------------------
extern "C" {

void __omsc_adaptive_recompile(const char* name, int64_t callCount, void** fnPtrSlot) {
    auto* runner = omscript::g_activeRunner.load(std::memory_order_acquire);
    if (__builtin_expect(runner != nullptr, 1))
        runner->onHotFunction(name, callCount, fnPtrSlot);
}

} // extern "C"
