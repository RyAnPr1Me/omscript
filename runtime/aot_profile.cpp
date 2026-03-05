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

        // --- Per-function sample counter for profiling overhead reduction ---
        // Used by the cold-path argument profiling to only fire callbacks
        // every 8th call, reducing function-call overhead while maintaining
        // sufficient statistical accuracy.
        auto* sampleGV = new llvm::GlobalVariable(mod, i64Ty, /*isConst=*/false, llvm::GlobalValue::InternalLinkage,
                                                   llvm::ConstantInt::get(i64Ty, 0), "__omsc_sample_" + name);
        sampleGV->setAlignment(llvm::Align(8));

        // --- Inject branch-site profiling calls in the original function body ---
        // We must collect and instrument conditional branches BEFORE adding the
        // dispatch prolog blocks so our iteration only sees the original (clean)
        // function structure.  The sequential branch IDs produced here must match
        // the iteration order used in onHotFunction() when applying branch weight
        // metadata during Tier-2 recompilation (both iterate blocks in order,
        // counting conditional branch terminators from 0).
        //
        // Branch profiling overhead is reduced at the C++ callback level: the
        // profiler uses try_lock and an atomic sample counter to skip most
        // invocations while maintaining statistical accuracy.
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
        // so Tier-3 threshold can trigger.  The higher-tier check
        // is a single EQ comparison — negligible overhead.
        B.SetInsertPoint(hotBB);
        auto* hotOld = B.CreateAtomicRMW(llvm::AtomicRMWInst::Add, counterGV, llvm::ConstantInt::get(i64Ty, 1),
                                         llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
        hotOld->setName("omsc.hot_old");
        auto* hotNew = B.CreateAdd(hotOld, llvm::ConstantInt::get(i64Ty, 1), "omsc.hot_new");
        auto* hitHigher =
            B.CreateICmpEQ(hotNew, llvm::ConstantInt::get(i64Ty, AdaptiveJITRunner::kTier3Threshold), "omsc.ht3");
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

        // --- omsc.hot_recomp: enqueue recompile, then call current fn-ptr ---
        // With background compilation, the callback returns immediately.
        // The fn-ptr has not been updated yet, so we just call the current
        // optimised version.  The background thread will atomically update
        // the pointer when it finishes, and subsequent calls will use it.
        B.SetInsertPoint(hotRecompBB);
        B.CreateCall(callbackTy, callbackFn, {nameGV, hotNew, fnPtrGV});
        B.CreateBr(hotCallBB);

        // --- omsc.hot_call: normal hot-path call with current fn-ptr ---
        B.SetInsertPoint(hotCallBB);
        {
            llvm::SmallVector<llvm::Value*, 8> args;
            for (auto& arg : fn->args())
                args.push_back(&arg);
            auto* hotCall = B.CreateCall(fn->getFunctionType(), fp, args, "omsc.hot_r");
            B.CreateRet(hotCall);
        }

        // --- omsc.count: atomic increment, check thresholds, sampled arg profile ---
        B.SetInsertPoint(countBB);
        auto* oldCnt = B.CreateAtomicRMW(llvm::AtomicRMWInst::Add, counterGV, llvm::ConstantInt::get(i64Ty, 1),
                                         llvm::MaybeAlign(), llvm::AtomicOrdering::Monotonic);
        oldCnt->setName("omsc.old");
        auto* newCnt = B.CreateAdd(oldCnt, llvm::ConstantInt::get(i64Ty, 1), "omsc.new");

        // Increment the per-function sample counter (non-atomic — single-threaded
        // cold path only, the hot path already redirects to recompiled code).
        auto* oldSample = B.CreateLoad(i64Ty, sampleGV, "omsc.smp_old");
        auto* newSample = B.CreateAdd(oldSample, llvm::ConstantInt::get(i64Ty, 1), "omsc.smp_new");
        B.CreateStore(newSample, sampleGV);

        // Inject argument type profiling every 8th call (sample counter & 0x7 == 0).
        // This reduces function-call overhead on the cold path by ~8x while
        // maintaining sufficient statistical accuracy for type/constant/range
        // profiling decisions.
        auto* shouldProfileArgs = B.CreateICmpEQ(
            B.CreateAnd(newSample, llvm::ConstantInt::get(i64Ty, 0x7), "omsc.asmod"),
            llvm::ConstantInt::get(i64Ty, 0), "omsc.aprof");
        auto* argProfileBB = llvm::BasicBlock::Create(ctx, "omsc.arg_prof", fn);
        auto* argSkipBB = llvm::BasicBlock::Create(ctx, "omsc.arg_skip", fn);
        B.CreateCondBr(shouldProfileArgs, argProfileBB, argSkipBB,
                       llvm::MDBuilder(ctx).createBranchWeights(1, 7));

        // Arg profiling block.
        B.SetInsertPoint(argProfileBB);
        uint32_t argIdx = 0;
        for (auto& arg : fn->args()) {
            B.CreateCall(argProfileTy, argProfileFn,
                         {nameGV, llvm::ConstantInt::get(i32Ty, argIdx),
                          llvm::ConstantInt::get(i8Ty, static_cast<uint8_t>(ArgType::Integer)), &arg});
            argIdx++;
        }
        B.CreateBr(argSkipBB);

        // Continue with threshold checks.
        B.SetInsertPoint(argSkipBB);

        // Check whether the call count hits ANY of the multi-tier thresholds.
        auto* hitT2 =
            B.CreateICmpEQ(newCnt, llvm::ConstantInt::get(i64Ty, AdaptiveJITRunner::kTier2Threshold), "omsc.hit_t2");
        auto* hitT3 =
            B.CreateICmpEQ(newCnt, llvm::ConstantInt::get(i64Ty, AdaptiveJITRunner::kTier3Threshold), "omsc.hit_t3");
        auto* hitAny = B.CreateOr(hitT2, hitT3, "omsc.hit");
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
    // Use Default (O2) backend optimization for Tier-1 so the initial native
    // code compiles quickly while maintaining reasonable quality.  The Tier-2
    // O2+PGO recompile at 20 calls quickly replaces this baseline with
    // profile-guided code, so the O2 startup cost is negligible.
    if (verbose_)
        std::cout << "JIT: compiling instrumented module (Tier-1, O2)..." << std::endl;
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

    // Start the background compilation thread before execution begins.
    // All tier-threshold recompilations will be offloaded to this thread,
    // allowing the main execution thread to continue without pausing.
    shutdownRequested_.store(false, std::memory_order_relaxed);
    bgThread_ = std::thread(&AdaptiveJITRunner::backgroundWorker, this);

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
// onHotFunction() — non-blocking recompilation enqueue
// ---------------------------------------------------------------------------
// Called from the dispatch prolog when a function's call count hits any of
// the tier thresholds (20 / 2000).  Instead of blocking the main execution
// thread, this enqueues a task on the background compilation thread and
// returns immediately.  The function continues executing baseline code
// until the background thread finishes and atomically patches the slot.
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
        // Tentatively promote to prevent duplicate enqueues.
        functionTier_[funcName] = targetTier;
    }

    // Enqueue the recompilation task on the background thread.
    // This returns immediately — zero pause on the hot path.
    {
        std::lock_guard<std::mutex> lk(queueMtx_);
        taskQueue_.push({funcName, callCount, fnPtrSlot});
    }
    queueCV_.notify_one();
}

// ---------------------------------------------------------------------------
// backgroundWorker() — background compilation thread loop
// ---------------------------------------------------------------------------
// Waits for recompilation tasks on the queue, processes them sequentially.
// Tasks are processed one at a time to avoid contention on LLVM's
// thread-unsafe MCJIT infrastructure.
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
            task = std::move(taskQueue_.front());
            taskQueue_.pop();
        }
        doRecompile(task.funcName, task.callCount, task.fnPtrSlot);
    }
}

// ---------------------------------------------------------------------------
// drainBackgroundThread() — stop the background worker cleanly
// ---------------------------------------------------------------------------
void AdaptiveJITRunner::drainBackgroundThread() {
    if (!bgThread_.joinable())
        return;
    shutdownRequested_.store(true, std::memory_order_relaxed);
    queueCV_.notify_one();
    bgThread_.join();
}

// ---------------------------------------------------------------------------
// doRecompile() — multi-tier PGO recompilation (runs on background thread)
// ---------------------------------------------------------------------------
// Performs the actual heavy work of recompilation: parsing clean bitcode,
// applying PGO annotations, running the O2/O3 pipeline, JIT-compiling,
// and atomically storing the new function pointer.
void AdaptiveJITRunner::doRecompile(const std::string& funcName, int64_t callCount, void** fnPtrSlot) {

    int targetTier = tierForCallCount(callCount);
    if (targetTier == 0)
        return;

    const char* tierNames[] = {"", "", "Tier-2 (warm)", "Tier-3 (hot)"};
    const char* tierLabel =
        (targetTier >= 2 && targetTier <= AdaptiveJITRunner::kMaxTier) ? tierNames[targetTier] : "Tier-?";
    std::cerr << "JIT: recompiling '" << funcName << "' (calls: " << callCount << ") at " << tierLabel << " O"
              << (targetTier >= 3 ? "3" : "2") << "+PGO [background]\n";

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
    //
    // At Tier-3, also use the top-K value frequency tracker: if a single
    // value dominates >60% of calls, inject the same assumption.  This catches
    // cases where the constant-count tracker was confused by early non-constant
    // calls but the top-K tracker correctly identified the dominant value.
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
    // At Tier-3, callees that dominate >40% of calls get `alwaysinline`
    // to guarantee full inlining of the hot path.
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
                            // At Tier-3 (2000+ calls), dominant callees (>40% of calls)
                            // get alwaysinline to guarantee full inlining.
                            if (targetTier >= 3 && ratio > 0.4) {
                                callee->addFnAttr(llvm::Attribute::AlwaysInline);
                            } else {
                                callee->addFnAttr(llvm::Attribute::InlineHint);
                            }
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
                    // Medium trip count: unroll by 4.
                    unrollCount = 4;
                } else if (avgTrip > 64) {
                    // Large trip count: unroll by 8 for ILP.
                    unrollCount = 8;
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
    //   Tier 2 (warm,   20 calls):  O2 pipeline, inliner threshold 600
    //   Tier 3 (hot,  2000 calls):  O3 pipeline, inliner threshold 1200,
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
        // Scale the inliner threshold with the tier: higher tiers justify the
        // extra code bloat because the function has proven itself truly hot.
        switch (targetTier) {
        default:
        case 2:
            PTO.InlinerThreshold = 600;
            break;
        case 3:
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
            // Propagate user's -ffast-math flag to the TargetMachine so that
            // the backend can use reciprocal estimates, FMA fusion, and
            // relaxed NaN/Inf handling in instruction selection.
            if (fastMath_) {
#if LLVM_VERSION_MAJOR < 22
                opts.UnsafeFPMath = true;
#endif
                opts.NoInfsFPMath = true;
                opts.NoNaNsFPMath = true;
                opts.NoSignedZerosFPMath = true;
            }
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

        // Tier-2 (warm) uses O2 for fast recompilation with PGO guidance.
        // Tier-3 (hot) uses O3 for maximum optimisation of proven hot code.
        auto optLevel = (targetTier >= 3) ? llvm::OptimizationLevel::O3 : llvm::OptimizationLevel::O2;
        auto pipeline = PB.buildPerModuleDefaultPipeline(optLevel);
        pipeline.run(*mod, MAM);

        // At Tier-3 (2000+ calls), run the O3 pipeline a second time.
        // The first pass often creates new optimization opportunities
        // (e.g., inlining exposes constant folding, dead code elimination
        // reveals further loop simplification) that a second pass can exploit.
        // This mirrors the AOT pipeline's iterative approach at O3.
        if (targetTier >= 3) {
            // The first O3 pass may have added module flags (e.g., "CG Profile")
            // that would conflict with the second pass trying to add them again.
            // Strip all module flags to avoid "module flag identifiers must be
            // unique" verification failures.
            if (auto* modFlags = mod->getModuleFlagsMetadata())
                mod->eraseNamedMetadata(modFlags);

            // Re-create analysis managers for the second pass.
            llvm::LoopAnalysisManager LAM2;
            llvm::FunctionAnalysisManager FAM2;
            llvm::CGSCCAnalysisManager CGAM2;
            llvm::ModuleAnalysisManager MAM2;
            PB.registerModuleAnalyses(MAM2);
            PB.registerCGSCCAnalyses(CGAM2);
            PB.registerFunctionAnalyses(FAM2);
            PB.registerLoopAnalyses(LAM2);
            PB.crossRegisterProxies(LAM2, FAM2, CGAM2, MAM2);
            PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3).run(*mod, MAM2);
        }
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

    // Hot-patch: use an atomic store with release ordering so the dispatch
    // prolog's volatile load (which acts as an acquire on x86, and pairs
    // with the release here on weakly-ordered architectures) sees the new
    // pointer on the very next function call.  This is the key to making
    // background compilation have zero performance impact — the main thread
    // never pauses; it simply starts seeing the optimised pointer.
    reinterpret_cast<std::atomic<void*>*>(fnPtrSlot)->store(
        reinterpret_cast<void*>(addr), std::memory_order_release);
    std::cerr << "JIT: recompiled '" << funcName << "' successfully (" << tierLabel << " active) [background]\n";
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
