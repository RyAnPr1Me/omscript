#include "aot_profile.h"
#include "deopt.h"
#include "jit_profiler.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
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

    // Declare the branch profiling callback.
    // Signature: void __omsc_profile_branch(const char* name, uint32_t branchId, int64_t taken)
    // taken == 1 means the true branch was taken; 0 means the false branch.
    auto* branchProfileTy = llvm::FunctionType::get(voidTy, {ptrTy, i32Ty, i64Ty}, false);
    auto* branchProfileFn =
        llvm::cast<llvm::Function>(mod.getOrInsertFunction("__omsc_profile_branch", branchProfileTy).getCallee());
    branchProfileFn->addFnAttr(llvm::Attribute::NoUnwind);

    // Declare the loop trip count profiling callback.
    // Signature: void __omsc_profile_loop(const char* name, uint32_t loopId, uint64_t tripCount)
    auto* loopProfileTy = llvm::FunctionType::get(voidTy, {ptrTy, i32Ty, i64Ty}, false);
    auto* loopProfileFn =
        llvm::cast<llvm::Function>(mod.getOrInsertFunction("__omsc_profile_loop", loopProfileTy).getCallee());
    loopProfileFn->addFnAttr(llvm::Attribute::NoUnwind);

    // Declare the call-site frequency profiling callback.
    // Signature: void __omsc_profile_call_site(const char* callerName, const char* calleeName)
    auto* callSiteProfileTy = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
    auto* callSiteProfileFn =
        llvm::cast<llvm::Function>(mod.getOrInsertFunction("__omsc_profile_call_site", callSiteProfileTy).getCallee());
    callSiteProfileFn->addFnAttr(llvm::Attribute::NoUnwind);

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

        // --- Inject branch-site profiling calls in the original function body ---
        // We must collect and instrument conditional branches BEFORE adding the
        // dispatch prolog blocks so our iteration only sees the original (clean)
        // function structure.  The sequential branch IDs produced here must match
        // the iteration order used in onHotFunction() when applying branch weight
        // metadata during Tier-2 recompilation (both iterate blocks in order,
        // counting conditional branch terminators from 0).
        {
            llvm::SmallVector<std::pair<llvm::BranchInst*, uint32_t>, 16> branchSites;
            uint32_t brIdx = 0;
            for (auto& bb : *fn) {
                if (auto* br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator()))
                    if (br->isConditional())
                        branchSites.push_back({br, brIdx++});
            }
            for (auto& [br, branchId] : branchSites) {
                llvm::IRBuilder<> brB(br); // inserts immediately before the branch
                // Widen the i1 condition to i64: 1 = branch taken (true edge),
                // 0 = branch not taken (false edge).
                auto* cond64 = brB.CreateZExt(br->getCondition(), i64Ty, "omsc.cond64");
                brB.CreateCall(branchProfileTy, branchProfileFn,
                               {nameGV, llvm::ConstantInt::get(i32Ty, branchId), cond64});
            }
        }

        // --- Inject loop trip count profiling ---
        // For each loop back-edge we create a counter alloca that is
        // incremented on every back-edge iteration and reported when the
        // loop exits.  This feeds average trip counts into Tier-2 unroll
        // decisions.  We detect loops by looking for back-edges (branches
        // to blocks earlier in the block list).
        {
            llvm::SmallPtrSet<llvm::BasicBlock*, 32> visited;
            llvm::SmallVector<std::pair<llvm::BranchInst*, llvm::BasicBlock*>, 8> backEdges;
            for (auto& bb : *fn) {
                visited.insert(&bb);
                auto* term = bb.getTerminator();
                if (!term)
                    continue;
                auto* br = llvm::dyn_cast<llvm::BranchInst>(term);
                if (!br)
                    continue;
                for (unsigned s = 0; s < br->getNumSuccessors(); s++) {
                    if (visited.count(br->getSuccessor(s))) {
                        backEdges.push_back({br, br->getSuccessor(s)});
                        break;
                    }
                }
            }
            uint32_t loopIdx = 0;
            for (auto& [backEdgeBr, headerBB] : backEdges) {
                // Insert a trip count counter: alloca at function entry, increment
                // at back-edge, report at loop exit.
                // To minimize overhead we only insert the profiling call at the
                // loop exit edge (the non-back-edge successor of the back-edge branch).
                llvm::BasicBlock* exitBB = nullptr;
                for (unsigned s = 0; s < backEdgeBr->getNumSuccessors(); s++) {
                    if (backEdgeBr->getSuccessor(s) != headerBB) {
                        exitBB = backEdgeBr->getSuccessor(s);
                        break;
                    }
                }
                if (!exitBB) {
                    loopIdx++;
                    continue; // infinite loop with no exit — skip
                }
                // Create a trip-count alloca in the entry block.
                llvm::IRBuilder<> entryB(&fn->getEntryBlock().front());
                auto* tripCountAlloca = entryB.CreateAlloca(i64Ty, nullptr, "omsc.trip_cnt_" + std::to_string(loopIdx));
                entryB.CreateStore(llvm::ConstantInt::get(i64Ty, 0), tripCountAlloca);

                // Increment trip count at the back-edge.
                llvm::IRBuilder<> beB(backEdgeBr);
                auto* oldTrip = beB.CreateLoad(i64Ty, tripCountAlloca, "omsc.trip_old");
                auto* newTrip = beB.CreateAdd(oldTrip, llvm::ConstantInt::get(i64Ty, 1), "omsc.trip_new");
                beB.CreateStore(newTrip, tripCountAlloca);

                // Report trip count at loop exit.
                // Use getFirstNonPHI() to insert after any PHI nodes at the
                // top of the exit block (e.g. LCSSA PHIs from loop-closed SSA).
                // Inserting before PHIs would violate the LLVM invariant that
                // PHI nodes must be grouped at the top of a basic block.
                llvm::IRBuilder<> exitB(exitBB->getFirstNonPHI());
                auto* finalTrip = exitB.CreateLoad(i64Ty, tripCountAlloca, "omsc.trip_final");
                exitB.CreateCall(loopProfileTy, loopProfileFn,
                                 {nameGV, llvm::ConstantInt::get(i32Ty, loopIdx), finalTrip});
                // Reset for next loop execution.
                exitB.CreateStore(llvm::ConstantInt::get(i64Ty, 0), tripCountAlloca);

                loopIdx++;
            }
        }

        // --- Inject call-site frequency profiling ---
        // For each call instruction in the function body, insert a profiling
        // callback that records caller→callee pairs.  This data drives inline
        // hint decisions during Tier-2 recompilation.
        {
            llvm::SmallVector<llvm::CallInst*, 16> callSites;
            for (auto& bb : *fn) {
                for (auto& inst : bb) {
                    if (auto* ci = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                        auto* callee = ci->getCalledFunction();
                        // Only track direct calls to user functions (not
                        // intrinsics or profiling callbacks).
                        if (callee && !callee->isDeclaration() && !callee->isIntrinsic() &&
                            !callee->getName().starts_with("__omsc_") && callee->getName() != "main") {
                            callSites.push_back(ci);
                        }
                    }
                }
            }
            for (auto* ci : callSites) {
                auto calleeName = ci->getCalledFunction()->getName().str();
                llvm::IRBuilder<> csB(ci);
                auto* calleeNameGV = csB.CreateGlobalString(calleeName, "__omsc_callee_" + calleeName, 0, &mod);
                csB.CreateCall(callSiteProfileTy, callSiteProfileFn, {nameGV, calleeNameGV});
            }
        }

        // --- New basic blocks (inserted before original entry) ---
        auto& origEntry = fn->getEntryBlock();
        auto* dispatchBB = llvm::BasicBlock::Create(ctx, "omsc.dispatch", fn, &origEntry);
        auto* hotBB = llvm::BasicBlock::Create(ctx, "omsc.hot", fn, &origEntry);
        auto* hotCheckBB = llvm::BasicBlock::Create(ctx, "omsc.hot_check", fn, &origEntry);
        auto* hotRecompBB = llvm::BasicBlock::Create(ctx, "omsc.hot_recomp", fn, &origEntry);
        auto* hotCallBB = llvm::BasicBlock::Create(ctx, "omsc.hot_call", fn, &origEntry);
        auto* countBB = llvm::BasicBlock::Create(ctx, "omsc.count", fn, &origEntry);
        auto* recompileBB = llvm::BasicBlock::Create(ctx, "omsc.recompile", fn, &origEntry);

        // --- omsc.dispatch: volatile-load fn-ptr, branch on non-null ---
        llvm::IRBuilder<> B(dispatchBB);
        auto* fp = B.CreateAlignedLoad(ptrTy, fnPtrGV, llvm::Align(8), /*isVolatile=*/true, "omsc.fp");
        auto* isHot =
            B.CreateICmpNE(B.CreatePtrToInt(fp, i64Ty, "omsc.fp_int"), llvm::ConstantInt::get(i64Ty, 0), "omsc.is_hot");
        llvm::MDBuilder mdB(ctx);
        auto* weights = mdB.createBranchWeights(99, 1);
        B.CreateCondBr(isHot, hotBB, countBB, weights);

        // --- omsc.hot: increment counter and check for higher-tier thresholds ---
        // After a recompile the fn-ptr slot is set, so every subsequent call
        // takes this path.  We still increment the counter (cheap atomic add)
        // so Tier-3 and Tier-4 thresholds can trigger.  The higher-tier check
        // is a single EQ comparison per tier — negligible overhead.
        B.SetInsertPoint(hotBB);
        auto* hotOld = B.CreateAtomicRMW(llvm::AtomicRMWInst::Add, counterGV, llvm::ConstantInt::get(i64Ty, 1),
                                         llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
        hotOld->setName("omsc.hot_old");
        auto* hotNew = B.CreateAdd(hotOld, llvm::ConstantInt::get(i64Ty, 1), "omsc.hot_new");
        auto* hitHigherT3 =
            B.CreateICmpEQ(hotNew, llvm::ConstantInt::get(i64Ty, AdaptiveJITRunner::kTier3Threshold), "omsc.ht3");
        auto* hitHigherT4 =
            B.CreateICmpEQ(hotNew, llvm::ConstantInt::get(i64Ty, AdaptiveJITRunner::kTier4Threshold), "omsc.ht4");
        auto* hitHigher = B.CreateOr(hitHigherT3, hitHigherT4, "omsc.hit_higher");
        B.CreateCondBr(hitHigher, hotCheckBB, hotCallBB, mdB.createBranchWeights(1, 999));

        // --- omsc.hot_check: profile args then trigger recompile ---
        B.SetInsertPoint(hotCheckBB);
        {
            uint32_t aIdx = 0;
            for (auto& arg : fn->args()) {
                B.CreateCall(argProfileTy, argProfileFn,
                             {nameGV, llvm::ConstantInt::get(i32Ty, aIdx),
                              llvm::ConstantInt::get(i8Ty, static_cast<uint8_t>(ArgType::Integer)), &arg});
                aIdx++;
            }
        }
        B.CreateBr(hotRecompBB);

        // --- omsc.hot_recomp: call recompiler, then re-load fn-ptr and call ---
        B.SetInsertPoint(hotRecompBB);
        B.CreateCall(callbackTy, callbackFn, {nameGV, hotNew, fnPtrGV});
        {
            // Re-load the fn-ptr slot — it may have been updated by the recompiler.
            auto* newFP = B.CreateAlignedLoad(ptrTy, fnPtrGV, llvm::Align(8), /*isVolatile=*/true, "omsc.new_fp");
            llvm::SmallVector<llvm::Value*, 8> args;
            for (auto& arg : fn->args())
                args.push_back(&arg);
            auto* reCall = B.CreateCall(fn->getFunctionType(), newFP, args, "omsc.re_r");
            B.CreateRet(reCall);
        }

        // --- omsc.hot_call: normal hot-path call with current fn-ptr ---
        B.SetInsertPoint(hotCallBB);
        {
            llvm::SmallVector<llvm::Value*, 8> args;
            for (auto& arg : fn->args())
                args.push_back(&arg);
            auto* hotCall = B.CreateCall(fn->getFunctionType(), fp, args, "omsc.hot_r");
            B.CreateRet(hotCall);
        }

        // --- omsc.count: atomic increment, check thresholds, profile args ---
        B.SetInsertPoint(countBB);
        auto* oldCnt = B.CreateAtomicRMW(llvm::AtomicRMWInst::Add, counterGV, llvm::ConstantInt::get(i64Ty, 1),
                                         llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
        oldCnt->setName("omsc.old");
        auto* newCnt = B.CreateAdd(oldCnt, llvm::ConstantInt::get(i64Ty, 1), "omsc.new");

        // Inject argument type profiling for each parameter.
        uint32_t argIdx = 0;
        for (auto& arg : fn->args()) {
            B.CreateCall(argProfileTy, argProfileFn,
                         {nameGV, llvm::ConstantInt::get(i32Ty, argIdx),
                          llvm::ConstantInt::get(i8Ty, static_cast<uint8_t>(ArgType::Integer)), &arg});
            argIdx++;
        }

        // Check whether the call count hits ANY of the multi-tier thresholds.
        auto* hitT2 =
            B.CreateICmpEQ(newCnt, llvm::ConstantInt::get(i64Ty, AdaptiveJITRunner::kTier2Threshold), "omsc.hit_t2");
        auto* hitT3 =
            B.CreateICmpEQ(newCnt, llvm::ConstantInt::get(i64Ty, AdaptiveJITRunner::kTier3Threshold), "omsc.hit_t3");
        auto* hitT4 =
            B.CreateICmpEQ(newCnt, llvm::ConstantInt::get(i64Ty, AdaptiveJITRunner::kTier4Threshold), "omsc.hit_t4");
        auto* hitAny = B.CreateOr(B.CreateOr(hitT2, hitT3, "omsc.hit23"), hitT4, "omsc.hit");
        B.CreateCondBr(hitAny, recompileBB, &origEntry);

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
    if (llvm::verifyModule(*instrMod, &llvm::errs())) {
        llvm::errs() << "omsc: internal error: instrumented IR failed verification\n";
        return 1;
    }

    // Step 4: JIT-compile the instrumented module.
    // Use Aggressive (O3) backend optimization for Tier-1 so the initial
    // native code is high quality — this minimises the performance gap between
    // Tier-1 execution and the Tier-2 O3+PGO recompiled code.  The extra
    // backend compilation cost is negligible for the handful of functions in
    // a typical OmScript program.
    if (verbose_)
        std::cout << "JIT: compiling instrumented module (Tier-1, O3)..." << std::endl;
    JitModule jm;
    jm.ctx = std::move(instrCtx); // keep context alive with the engine

    std::string engineErr;
    llvm::EngineBuilder builder(std::move(instrMod));
    builder.setErrorStr(&engineErr);
    builder.setEngineKind(llvm::EngineKind::JIT);
#if LLVM_VERSION_MAJOR >= 18
    builder.setOptLevel(llvm::CodeGenOptLevel::Aggressive);
#else
    builder.setOptLevel(llvm::CodeGenOpt::Aggressive);
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
    rawEngine->addGlobalMapping("__omsc_profile_loop", reinterpret_cast<uint64_t>(&__omsc_profile_loop));
    rawEngine->addGlobalMapping("__omsc_profile_call_site", reinterpret_cast<uint64_t>(&__omsc_profile_call_site));
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
    if (verbose_)
        std::cout << "JIT: executing program..." << std::endl;
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
// tierForCallCount() — map a call count to its JIT tier
// ---------------------------------------------------------------------------
int AdaptiveJITRunner::tierForCallCount(int64_t count) {
    if (count >= kTier4Threshold)
        return 4;
    if (count >= kTier3Threshold)
        return 3;
    if (count >= kTier2Threshold)
        return 2;
    return 0;
}

// ---------------------------------------------------------------------------
// onHotFunction() — multi-tier PGO recompilation (synchronous)
// ---------------------------------------------------------------------------
// Called from the dispatch prolog when a function's call count hits any of
// the tier thresholds (50 / 500 / 5000).  Each successive tier uses richer
// profile data and more aggressive optimisation settings.  Runs synchronously
// because LLVM MCJIT is not safe to use from a background thread while
// another MCJIT engine is executing code.
void AdaptiveJITRunner::onHotFunction(const char* name, int64_t callCount, void** fnPtrSlot) {
    const std::string funcName(name);

    // Determine which tier this call count corresponds to and whether
    // the function has already been compiled at this tier or higher.
    int targetTier = tierForCallCount(callCount);
    if (targetTier == 0)
        return; // Below all thresholds — nothing to do.
    {
        std::lock_guard<std::mutex> lk(recompiledMtx_);
        int currentTier = functionTier_[funcName]; // 0 if never compiled
        if (targetTier <= currentTier)
            return; // Already at this tier or above.
    }

    const char* tierNames[] = {"", "", "Tier-2 (warm)", "Tier-3 (hot)", "Tier-4 (saturated)"};
    const char* tierLabel =
        (targetTier >= 2 && targetTier <= AdaptiveJITRunner::kMaxTier) ? tierNames[targetTier] : "Tier-?";
    std::cerr << "JIT: recompiling '" << funcName << "' (calls: " << callCount << ") at " << tierLabel << " O3+PGO\n";

    // RAII scope guard: on early return (failure), revert the tier so that
    // a future threshold hit can retry.  Dismissed (via succeeded=true)
    // only after a successful hot-patch write.
    bool succeeded = false;
    int prevTier = 0;
    {
        std::lock_guard<std::mutex> lk(recompiledMtx_);
        prevTier = functionTier_[funcName];
        functionTier_[funcName] = targetTier; // tentatively promote
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
    // Suppress LLVM optimization remarks (e.g. "loop not vectorized") that add
    // noise to stderr and waste time formatting diagnostic strings.  Only errors
    // and warnings are forwarded; remarks are silently discarded.
    newCtx->setDiagnosticHandlerCallBack(
        [](const llvm::DiagnosticInfo& DI, void*) {
            if (DI.getSeverity() == llvm::DS_Remark)
                return;
            llvm::DiagnosticPrinterRawOStream DP(llvm::errs());
            DI.print(DP);
            llvm::errs() << "\n";
        },
        nullptr);
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
        const FunctionProfile* prof = JITProfiler::instance().getProfile(funcName);
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
    {
        const FunctionProfile* prof = JITProfiler::instance().getProfile(funcName);
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
                // hasConstantSpecialization() checks profile data (>80% same
                // value); the IR type check ensures the LLVM parameter is i64
                // so we can safely emit an ICmpEQ + ConstantInt.
                if (ap.hasConstantSpecialization() && fn->getArg(static_cast<unsigned>(i))->getType() == i64Ty) {
                    auto* cmp = asB.CreateICmpEQ(fn->getArg(static_cast<unsigned>(i)),
                                                 llvm::ConstantInt::get(i64Ty, ap.observedConstant), "omsc.const_spec");
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
        const FunctionProfile* prof = JITProfiler::instance().getProfile(funcName);
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

    // --- Apply call-site-frequency-driven inline hints ---
    // When profiling shows that certain callees are called frequently from
    // this hot function, add `inlinehint` to those callees to guide LLVM's
    // inliner to prioritize inlining them.  Callees that account for >20%
    // of all calls from this function are considered hot call sites.
    {
        const FunctionProfile* prof = JITProfiler::instance().getProfile(funcName);
        if (prof && !prof->callSites.empty()) {
            uint64_t totalCalls = 0;
            for (const auto& cs : prof->callSites)
                totalCalls += cs.second;
            if (totalCalls > 0) {
                for (const auto& cs : prof->callSites) {
                    double ratio = static_cast<double>(cs.second) / static_cast<double>(totalCalls);
                    if (ratio > 0.2) {
                        llvm::Function* callee = mod->getFunction(cs.first);
                        if (callee && !callee->isDeclaration()) {
                            callee->addFnAttr(llvm::Attribute::InlineHint);
                            // Remove the cold attribute if it was previously set
                            // so the inliner doesn't skip this hot callee.
                            callee->removeFnAttr(llvm::Attribute::Cold);
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
        const FunctionProfile* loopProf = JITProfiler::instance().getProfile(funcName);
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
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 4))});
            llvm::MDNode* vecWidth = llvm::MDNode::get(
                ctx, {llvm::MDString::get(ctx, "llvm.loop.vectorize.width"),
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), 4))});
            llvm::SmallVector<llvm::Metadata*, 6> loopMDs;
            loopMDs.push_back(nullptr); // self-reference placeholder
            loopMDs.push_back(mustProgress);
            loopMDs.push_back(interleave);
            loopMDs.push_back(vecWidth);

            // Attach profile-guided unroll count when trip count data is available.
            if (loopProf && loopIdx < loopProf->loops.size()) {
                uint64_t avgTrip = loopProf->loops[loopIdx].averageTripCount();
                uint32_t unrollCount = 0;
                if (avgTrip > 0 && avgTrip <= 16) {
                    // Small trip count: fully unroll by the trip count.
                    unrollCount = static_cast<uint32_t>(avgTrip);
                } else if (avgTrip > 16 && avgTrip <= 64) {
                    // Medium trip count: unroll by 4.
                    unrollCount = 4;
                } else if (avgTrip > 64) {
                    // Large trip count: unroll by 8 for ILP.
                    unrollCount = 8;
                }
                if (unrollCount > 0) {
                    llvm::MDNode* unrollMD = llvm::MDNode::get(
                        ctx, {llvm::MDString::get(ctx, "llvm.loop.unroll.count"),
                              llvm::ConstantAsMetadata::get(
                                  llvm::ConstantInt::get(llvm::Type::getInt32Ty(ctx), unrollCount))});
                    loopMDs.push_back(unrollMD);
                }
            }

            llvm::MDNode* loopMD = llvm::MDNode::get(ctx, loopMDs);
            loopMD->replaceOperandWith(0, loopMD);
            br->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
            loopIdx++;
        }
    }

    // --- Re-optimise at O3 with PGO guidance and full native CPU features ---
    // Each tier uses progressively more aggressive optimisation settings:
    //   Tier 2 (warm,  50 calls): inliner threshold 600
    //   Tier 3 (hot,  500 calls): inliner threshold 800, merge functions
    //   Tier 4 (sat, 5000 calls): inliner threshold 1200, maximum aggression
    {
        llvm::PipelineTuningOptions PTO;
        PTO.LoopVectorization = true;
        PTO.SLPVectorization = true;
        PTO.LoopUnrolling = true;
        PTO.LoopInterleaving = true;
        PTO.MergeFunctions = true;
        PTO.CallGraphProfile = true;
        // Scale the inliner threshold with the tier: higher tiers justify the
        // extra code bloat because the function has proven itself truly hot.
        switch (targetTier) {
        default:
        case 2:
            PTO.InlinerThreshold = 600;
            break;
        case 3:
            PTO.InlinerThreshold = 800;
            break;
        case 4:
            PTO.InlinerThreshold = 1200;
            break;
        }

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
                return;
            }
            mod->setDataLayout(TM->createDataLayout());
        }

        llvm::PassBuilder PB(TM.get(), PTO);

        // Register LoopDistributePass to run just before vectorisation,
        // matching the AOT O3 pipeline.  Loop distribution splits loops with
        // multiple independent memory streams into separate loops with smaller
        // working sets, improving cache locality and enabling downstream
        // vectorisation of the resulting simpler loops.
        PB.registerVectorizerStartEPCallback(
            [](llvm::FunctionPassManager& FPM, llvm::OptimizationLevel) { FPM.addPass(llvm::LoopDistributePass()); });

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

    // Verify the module after O3 optimization to catch optimizer bugs early.
    if (llvm::verifyModule(*mod, &llvm::errs())) {
        llvm::errs() << "omsc: " << tierLabel << " recompile of '" << funcName
                     << "' failed: module verification after O3\n";
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
        llvm::errs() << "omsc: " << tierLabel << " JIT compilation of '" << funcName << "' failed: " << engineErr
                     << "\n";
        return;
    }
    engine->finalizeObject();

    uint64_t addr = engine->getFunctionAddress(funcName);
    if (!addr) {
        return;
    }

    // Mark successful — prevents the RAII scope guard from reverting the tier.
    succeeded = true;

    {
        std::lock_guard<std::mutex> lk(recompiledMtx_);
        modules_.push_back({std::move(newCtx), std::move(engine)});
    }

    // Hot-patch: the volatile load in the dispatch prolog will see this on
    // the very next call to this function.
    *fnPtrSlot = reinterpret_cast<void*>(addr);
    std::cerr << "JIT: recompiled '" << funcName << "' successfully (" << tierLabel << " active)\n";
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
