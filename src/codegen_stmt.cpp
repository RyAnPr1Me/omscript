#include "codegen.h"
#include "diagnostic.h"
#include <iostream>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <set>
#include <stdexcept>

#if LLVM_VERSION_MAJOR >= 19
#define OMSC_GET_INTRINSIC_STMT llvm::Intrinsic::getOrInsertDeclaration
#else
#define OMSC_GET_INTRINSIC_STMT llvm::Intrinsic::getDeclaration
#endif

namespace omscript {

void CodeGenerator::generateVarDecl(VarDecl* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        codegenError("Variable declaration outside of function", stmt);
    }

    // Resolve the alloca type from the type annotation if present.
    llvm::Type* allocaType = stmt->typeName.empty()
                                 ? getDefaultType()
                                 : resolveAnnotatedType(stmt->typeName);
    llvm::Value* initValue = nullptr;

    // Check if this is a SIMD vector type.
    const bool isSimdType = allocaType->isVectorTy();

    if (stmt->initializer) {
        // SIMD vector initialization from array literal:
        //   var v: f32x4 = [1.0, 2.0, 3.0, 4.0];
        // Build an LLVM vector value from the array elements.
        if (isSimdType && stmt->initializer->type == ASTNodeType::ARRAY_EXPR) {
            auto* arrExpr = static_cast<ArrayExpr*>(stmt->initializer.get());
            auto* vecTy = llvm::cast<llvm::FixedVectorType>(allocaType);
            const unsigned numElems = vecTy->getNumElements();
            llvm::Type* elemTy = vecTy->getElementType();

            if (arrExpr->elements.size() != numElems) {
                codegenError("SIMD vector type requires exactly " + std::to_string(numElems) +
                                 " elements, but " + std::to_string(arrExpr->elements.size()) +
                                 " provided",
                             stmt);
            }

            // Start with an undef vector and insert each element.
            llvm::Value* vec = llvm::UndefValue::get(vecTy);
            for (unsigned i = 0; i < numElems; ++i) {
                llvm::Value* elem = generateExpression(arrExpr->elements[i].get());
                elem = convertToVectorElement(elem, elemTy);
                vec = builder->CreateInsertElement(vec, elem,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), i), "simd.ins");
            }
            initValue = vec;
        } else {
            initValue = generateExpression(stmt->initializer.get());
        }

        // When no annotation is present, infer the type from the initializer.
        if (stmt->typeName.empty())
            allocaType = initValue->getType();

        // When a string literal is assigned to a mutable string variable,
        // heap-allocate a copy via strdup().  This ensures the variable always
        // points to heap memory, allowing str_concat to use realloc() for
        // amortized O(1) appending (the common `s = str_concat(s, "x")` loop
        // pattern).  Without this, the first str_concat would receive a pointer
        // to read-only global memory and realloc would be undefined behaviour.
        if (!stmt->isConst &&
            stmt->initializer->type == ASTNodeType::LITERAL_EXPR &&
            static_cast<LiteralExpr*>(stmt->initializer.get())->literalType ==
                LiteralExpr::LiteralType::STRING) {
            initValue = builder->CreateCall(getOrDeclareStrdup(), {initValue}, "strdup.init");
            allocaType = initValue->getType();
        }

        // Convert the initializer to match the declared type when an annotation
        // is present (e.g. `var x: float = 42` should store 42.0 as double).
        // Skip for SIMD types — the vector was already built with correct types.
        if (!stmt->typeName.empty() && !isSimdType)
            initValue = convertTo(initValue, allocaType);
    }

    llvm::AllocaInst* alloca = createEntryBlockAlloca(function, stmt->name, allocaType);

    // Register keyword: force the variable into a CPU register.
    // We set high alignment to facilitate SROA/mem2reg promotion and track the
    // variable so that a mem2reg pass is run on this function immediately after
    // codegen, guaranteeing the alloca is promoted to an SSA register
    // regardless of the global optimization level.  The variable remains
    // mutable — `register` forces register allocation, not immutability.
    if (stmt->isRegister) {
        // Warn at compile time if the type can't be promoted to a register
        // (arrays, structs, pointers/strings are not promotable by mem2reg).
        if (allocaType->isArrayTy() || allocaType->isStructTy() ||
            allocaType->isPointerTy()) {
            const Diagnostic warn{DiagnosticSeverity::Warning,
                                  {"", stmt->line, stmt->column},
                                  "'register' variable '" + stmt->name +
                                      "' has a type that cannot be promoted to a CPU register; "
                                      "the keyword will have no effect"};
            std::cerr << warn.format() << "\n";
        } else {
            registerVars_.insert(stmt->name);
            // Emit llvm.lifetime.start so the register live-range is tightly
            // scoped, helping the register allocator.
            const uint64_t sz = module->getDataLayout().getTypeAllocSize(allocaType);
            auto* szVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), sz);
            auto* lifetimeStart = OMSC_GET_INTRINSIC_STMT(
                module.get(), llvm::Intrinsic::lifetime_start,
                {llvm::PointerType::getUnqual(*context)});
            builder->CreateCall(lifetimeStart, {szVal, alloca});
        }
        if (allocaType->isIntegerTy() || allocaType->isDoubleTy() || allocaType->isFloatTy())
            alloca->setAlignment(llvm::Align(16));
        if (allocaType->isVectorTy())
            alloca->setAlignment(llvm::Align(32));
    }

    // Track SIMD variables for operator dispatch.
    if (isSimdType)
        simdVars_.insert(stmt->name);

    bindVariable(stmt->name, alloca, stmt->isConst);

    if (initValue) {
        builder->CreateStore(initValue, alloca);
        // Track non-negativity: if a variable is initialized with a
        // non-negative value (constant or expression), mark the alloca so
        // that subsequent loads inherit the non-negative property.  This
        // enables unsigned comparisons, urem/udiv, and NSW flags on
        // downstream arithmetic.  For non-const vars, the tracking is
        // updated on each assignment (generateAssign) to maintain soundness.
        if (allocaType->isIntegerTy()) {
            bool initNonNeg = nonNegValues_.count(initValue) > 0;
            if (!initNonNeg) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(initValue))
                    initNonNeg = !ci->isNegative();
            }
            if (initNonNeg)
                nonNegValues_.insert(alloca);
            // Propagate tight upper bound for modular arithmetic.
            // If a variable is declared with `var t = b` where b has a bound,
            // t inherits that bound, allowing llvm.assume to convey the range.
            if (auto* ri = llvm::dyn_cast<llvm::Instruction>(initValue)) {
                if (ri->getOpcode() == llvm::Instruction::URem) {
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(ri->getOperand(1))) {
                        if (ci->getSExtValue() > 1)
                            allocaUpperBound_[alloca] = ci->getSExtValue();
                    }
                } else if (ri->getOpcode() == llvm::Instruction::Load) {
                    auto* srcAlloca = llvm::dyn_cast<llvm::AllocaInst>(ri->getOperand(0));
                    auto bit = allocaUpperBound_.find(srcAlloca);
                    if (srcAlloca && bit != allocaUpperBound_.end())
                        allocaUpperBound_[alloca] = bit->second;
                }
            }
            // Track constant integer values for `const` variables so that
            // downstream div/mod can substitute the constant directly and
            // use the fast urem/udiv path instead of the dynamic-divisor path.
            if (stmt->isConst) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(initValue))
                    constIntFolds_[stmt->name] = ci->getSExtValue();
            }
        }
        // Track whether this variable holds a string value so that print(),
        // concatenation, and comparison operators handle it correctly when
        // the variable's alloca type is i64 (e.g. assigned from a function
        // call that returns a string as i64 via ptrtoint).
        if (initValue->getType()->isPointerTy() || isStringExpr(stmt->initializer.get()))
            stringVars_.insert(stmt->name);
        else
            stringVars_.erase(stmt->name);
        // Track whether this variable holds an array of strings so that
        // arr[i] and for-each correctly propagate string type information.
        if (isStringArrayExpr(stmt->initializer.get()))
            stringArrayVars_.insert(stmt->name);
        else
            stringArrayVars_.erase(stmt->name);
        // Track whether this variable holds a struct value so that
        // field access/assignment can resolve the struct type.
        if (stmt->initializer->type == ASTNodeType::STRUCT_LITERAL_EXPR) {
            auto* structLit = static_cast<StructLiteralExpr*>(stmt->initializer.get());
            structVars_[stmt->name] = structLit->structName;
        } else {
            structVars_.erase(stmt->name);
        }
        // Track known array sizes from array_fill(N, val) calls.
        // When N is a compile-time constant (or a tracked value), record
        // the size so that subsequent bounds checks on arr[i] can be
        // proven statically without loading the length header at runtime.
        if (stmt->initializer->type == ASTNodeType::CALL_EXPR) {
            auto* callExpr = static_cast<CallExpr*>(stmt->initializer.get());
            if (callExpr->callee == "array_fill" && callExpr->arguments.size() == 2) {
                // The initValue is the result of array_fill, but sizeArg
                // was the first argument.  Re-derive from the AST: if the
                // first argument is a literal integer, store the constant.
                auto* sizeExpr = callExpr->arguments[0].get();
                if (sizeExpr->type == ASTNodeType::LITERAL_EXPR) {
                    auto* lit = static_cast<LiteralExpr*>(sizeExpr);
                    if (lit->literalType == LiteralExpr::LiteralType::INTEGER && lit->intValue > 0) {
                        knownArraySizes_[stmt->name] =
                            llvm::ConstantInt::get(getDefaultType(), lit->intValue);
                    }
                }
            }
        }
    } else {
        // Default-initialize based on type annotation.
        if (isSimdType) {
            builder->CreateStore(llvm::Constant::getNullValue(allocaType), alloca);
        } else if (allocaType->isDoubleTy())
            builder->CreateStore(llvm::ConstantFP::get(allocaType, 0.0), alloca);
        else {
            const unsigned bits = allocaType->isIntegerTy() ? allocaType->getIntegerBitWidth() : 64;
            builder->CreateStore(llvm::ConstantInt::get(*context, llvm::APInt(bits, 0)), alloca);
        }
    }
}

void CodeGenerator::generateReturn(ReturnStmt* stmt) {
    // Prefetch invalidation enforcement: check that all prefetched variables
    // have been explicitly invalidated before returning.  Variables that are
    // being returned via `move` are exempt (ownership transfer).
    if (!prefetchedVars_.empty()) {
        std::string returnedVar;
        if (stmt->value) {
            if (auto* moveExpr = dynamic_cast<MoveExpr*>(stmt->value.get())) {
                if (auto* ident = dynamic_cast<IdentifierExpr*>(moveExpr->source.get())) {
                    returnedVar = ident->name;
                }
            } else if (auto* ident = dynamic_cast<IdentifierExpr*>(stmt->value.get())) {
                returnedVar = ident->name;
            }
        }
        for (const auto& pfVar : prefetchedVars_) {
            if (pfVar == returnedVar) continue; // being returned — exempt
            if (!deadVars_.count(pfVar)) {
                codegenError("Prefetched variable '" + pfVar +
                             "' must be explicitly invalidated before return "
                             "(use 'invalidate " + pfVar + ";')", stmt);
            }
        }
    }

    if (stmt->value) {
        llvm::Value* retValue = generateExpression(stmt->value.get());
        // Record that the current function returns a string value so that
        // callers can use isStringExpr() on the CallExpr and track the result.
        if (retValue->getType()->isPointerTy() || isStringExpr(stmt->value.get())) {
            if (builder->GetInsertBlock() && builder->GetInsertBlock()->getParent())
                stringReturningFunctions_.insert(builder->GetInsertBlock()->getParent()->getName());
        }
        // Convert return value to match the function's declared return type.
        llvm::Function* currentFn = builder->GetInsertBlock()->getParent();
        llvm::Type* retTy = currentFn->getReturnType();
        retValue = convertTo(retValue, retTy);

        // Tail call optimization: if the return value is a direct function
        // call, mark it as a tail call so LLVM can eliminate the stack frame.
        if (optimizationLevel >= OptimizationLevel::O2) {
            if (auto* callInst = llvm::dyn_cast<llvm::CallInst>(retValue)) {
                callInst->setTailCallKind(llvm::CallInst::TCK_Tail);
            }
        }

        // @prefetch enforcement: determine which prefetched parameter (if any)
        // is being returned (transferred out).  All OTHER prefetched params
        // get cache-eviction hints since their memory is no longer needed.
        std::string transferredParam;
        if (auto* ident = dynamic_cast<IdentifierExpr*>(stmt->value.get())) {
            if (prefetchedParams_.count(ident->name)) {
                transferredParam = ident->name;
            }
        }
        for (const auto& pfParam : prefetchedParams_) {
            if (pfParam == transferredParam) continue; // transferred out — keep in cache
            auto it = namedValues.find(pfParam);
            if (it != namedValues.end()) {
                llvm::Value* ptrVal = builder->CreateLoad(
                    llvm::cast<llvm::AllocaInst>(it->second)->getAllocatedType(),
                    it->second, pfParam + ".pf.exit");
                llvm::Value* ptr = builder->CreateIntToPtr(
                    ptrVal, llvm::PointerType::getUnqual(*context), pfParam + ".pf.evict");
                llvm::Function* prefetchFn = OMSC_GET_INTRINSIC_STMT(
                    module.get(), llvm::Intrinsic::prefetch,
                    {llvm::PointerType::getUnqual(*context)});
                // locality=0: hint CPU to evict from cache (no temporal reuse)
                builder->CreateCall(prefetchFn, {
                    ptr,
                    builder->getInt32(0),  // read
                    builder->getInt32(0),  // locality=0: evict
                    builder->getInt32(1)   // data cache
                });
            }
        }

        builder->CreateRet(retValue);
    } else {
        // @prefetch enforcement: no return value, so ALL prefetched params
        // get cache-eviction hints.
        for (const auto& pfParam : prefetchedParams_) {
            auto it = namedValues.find(pfParam);
            if (it != namedValues.end()) {
                llvm::Value* ptrVal = builder->CreateLoad(
                    llvm::cast<llvm::AllocaInst>(it->second)->getAllocatedType(),
                    it->second, pfParam + ".pf.exit");
                llvm::Value* ptr = builder->CreateIntToPtr(
                    ptrVal, llvm::PointerType::getUnqual(*context), pfParam + ".pf.evict");
                llvm::Function* prefetchFn = OMSC_GET_INTRINSIC_STMT(
                    module.get(), llvm::Intrinsic::prefetch,
                    {llvm::PointerType::getUnqual(*context)});
                builder->CreateCall(prefetchFn, {
                    ptr,
                    builder->getInt32(0),
                    builder->getInt32(0),
                    builder->getInt32(1)
                });
            }
        }

        llvm::Function* currentFn = builder->GetInsertBlock()->getParent();
        llvm::Type* retTy = currentFn->getReturnType();
        if (retTy->isDoubleTy())
            builder->CreateRet(llvm::ConstantFP::get(retTy, 0.0));
        else
            builder->CreateRet(llvm::ConstantInt::get(*context, llvm::APInt(64, 0)));
    }
}

void CodeGenerator::generateIf(IfStmt* stmt) {
    llvm::Value* condition = generateExpression(stmt->condition.get());

    // Constant condition elimination: skip dead branch when condition is known
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(condition)) {
        if (ci->isZero()) {
            // Condition is false: only generate else branch if present
            if (stmt->elseBranch) {
                generateStatement(stmt->elseBranch.get());
            }
        } else {
            // Condition is true: only generate then branch
            generateStatement(stmt->thenBranch.get());
        }
        return;
    }

    llvm::Value* condBool = toBool(condition);

    llvm::Function* function = builder->GetInsertBlock()->getParent();

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(*context, "then", function);
    llvm::BasicBlock* elseBB = stmt->elseBranch ? llvm::BasicBlock::Create(*context, "else", function) : nullptr;
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "ifcont", function);

    llvm::BranchInst* br;
    if (elseBB) {
        br = builder->CreateCondBr(condBool, thenBB, elseBB);
    } else {
        br = builder->CreateCondBr(condBool, thenBB, mergeBB);
    }

    // Attach branch weight metadata for likely/unlikely hints.
    // likely  → then-branch is hot (weight 2000:1)
    // unlikely → then-branch is cold (weight 1:2000)
    if (stmt->hintLikely || stmt->hintUnlikely) {
        const uint32_t thenWeight = stmt->hintLikely ? 2000 : 1;
        const uint32_t elseWeight = stmt->hintLikely ? 1 : 2000;
        llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(thenWeight, elseWeight);
        br->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
    } else if (optimizationLevel >= OptimizationLevel::O2) {
        // Infer branch probability from modulo-equality patterns.
        // Pattern: if (x % k == 0) { … } where k is a positive constant.
        // The true branch fires ~1/k of the time; emitting accurate weights
        // (1 : k-1) lets the CPU's branch predictor and LLVM's block layout
        // optimise for the common (false) case — critical for inner-loop
        // branches like collatz and cond_arithmetic.
        //
        // The emitted condition chain is:
        //   %srem  = srem/urem %x, k
        //   %icmp  = icmp eq/ne %srem, 0
        //   %zext  = zext i1 %icmp to i64   (OM bool-to-int)
        //   %tobool = icmp ne i64 %zext, 0  (toBool)
        //   condBr %tobool, then, merge
        // Trace back through the %tobool → %zext → %icmp → %srem chain.
        [&]() {
            // condBool = icmp ne (%zext, 0)
            auto* outerNE = llvm::dyn_cast<llvm::ICmpInst>(condBool);
            if (!outerNE || outerNE->getPredicate() != llvm::ICmpInst::ICMP_NE) return;
            auto* outerRHS = llvm::dyn_cast<llvm::ConstantInt>(outerNE->getOperand(1));
            if (!outerRHS || !outerRHS->isZero()) return;
            // Peel zext
            auto* zext = llvm::dyn_cast<llvm::ZExtInst>(outerNE->getOperand(0));
            if (!zext) return;
            // Inner icmp eq/ne
            auto* innerCmp = llvm::dyn_cast<llvm::ICmpInst>(zext->getOperand(0));
            if (!innerCmp) return;
            const bool innerIsEq = (innerCmp->getPredicate() == llvm::ICmpInst::ICMP_EQ);
            const bool innerIsNe = (innerCmp->getPredicate() == llvm::ICmpInst::ICMP_NE);
            if (!innerIsEq && !innerIsNe) return;
            // One operand of the inner cmp must be 0
            llvm::Value* remVal = nullptr;
            if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(innerCmp->getOperand(1))) {
                if (c->isZero()) remVal = innerCmp->getOperand(0);
            }
            if (!remVal) {
                if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(innerCmp->getOperand(0))) {
                    if (c->isZero()) remVal = innerCmp->getOperand(1);
                }
            }
            if (!remVal) return;
            // remVal must be srem/urem with a positive constant divisor
            auto* remOp = llvm::dyn_cast<llvm::BinaryOperator>(remVal);
            if (!remOp) return;
            if (remOp->getOpcode() != llvm::Instruction::SRem &&
                remOp->getOpcode() != llvm::Instruction::URem) return;
            auto* divisorCI = llvm::dyn_cast<llvm::ConstantInt>(remOp->getOperand(1));
            if (!divisorCI) return;
            const int64_t k = divisorCI->getSExtValue();
            if (k <= 1 || k > 1024) return;  // sanity: reasonable divisors only
            // innerIsEq && outerNE(zext): branch fires 1/k of the time
            // innerIsNe && outerNE(zext): branch fires (k-1)/k of the time
            const auto kU = static_cast<uint32_t>(k);
            const uint32_t thenW = innerIsEq ? 1u : (kU - 1u);
            const uint32_t elseW = innerIsEq ? (kU - 1u) : 1u;
            llvm::MDNode* brWeights =
                llvm::MDBuilder(*context).createBranchWeights(thenW, elseW);
            br->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
        }();
    }

    // Save pre-if non-neg state so each branch starts from the same baseline.
    // Without this the else-branch inherits the then-branch's mutations, which
    // is unsound and can misclassify values as non-negative.
    const llvm::DenseSet<llvm::Value*> preIfNonNeg = nonNegValues_;

    // Then block
    builder->SetInsertPoint(thenBB);
    generateStatement(stmt->thenBranch.get());
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(mergeBB);
    }
    // Capture which allocas are non-negative after the then branch.
    const llvm::DenseSet<llvm::Value*> thenNonNeg = nonNegValues_;

    // Else block — restore to pre-if state before generating.
    if (elseBB) {
        nonNegValues_ = preIfNonNeg;
        builder->SetInsertPoint(elseBB);
        generateStatement(stmt->elseBranch.get());
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(mergeBB);
        }
        // After both branches: a value is non-negative at the merge point only
        // if it was non-negative in BOTH branches (conservative intersection).
        const llvm::DenseSet<llvm::Value*> elseNonNeg = nonNegValues_;
        nonNegValues_ = preIfNonNeg; // start from pre-if
        for (llvm::Value* v : thenNonNeg) {
            if (elseNonNeg.count(v))
                nonNegValues_.insert(v);
        }
    } else {
        // No else: at the merge point a value is non-negative only if it was
        // non-negative before the if AND remains so after the then-branch.
        // Conservative: intersect pre-if with post-then.
        for (llvm::Value* v : preIfNonNeg) {
            if (!thenNonNeg.count(v))
                nonNegValues_.erase(v);
        }
    }

    // Merge block
    builder->SetInsertPoint(mergeBB);
}

void CodeGenerator::generateWhile(WhileStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();

    // Signal to an enclosing for-loop that its body contains an inner loop,
    // so unrolling the outer loop should be conservative to avoid I-cache bloat.
    bodyHasInnerLoop_ = true;
    ++loopNestDepth_;

    // Save and reset body-analysis flags so this while loop's own body sets them
    // from scratch (not inherited from outer/prior loops in the same function).
    const bool savedBodyHasNonPow2Modulo = bodyHasNonPow2Modulo_;
    const bool savedBodyHasNonPow2ModuloValue = bodyHasNonPow2ModuloValue_;
    const bool savedBodyHasNonPow2ModuloArrayStore = bodyHasNonPow2ModuloArrayStore_;
    bodyHasNonPow2Modulo_ = false;
    bodyHasNonPow2ModuloValue_ = false;
    bodyHasNonPow2ModuloArrayStore_ = false;

    const ScopeGuard scope(*this);

    // Constant condition elimination — match generateIf's dead-branch pruning.
    // Evaluate the condition once; if it folds to a constant we can avoid
    // emitting the loop structure entirely (false) or the condition check (true).
    llvm::Value* preCondition = generateExpression(stmt->condition.get());
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(preCondition)) {
        if (ci->isZero()) {
            // Condition is statically false: loop body never executes.
            return;
        }
        // Condition is statically true: emit an unconditional infinite loop
        // (no condition check on each iteration).
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "whilebody", function);
        llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "whileend", function);
        builder->CreateBr(bodyBB);
        builder->SetInsertPoint(bodyBB);
        loopStack.push_back({endBB, bodyBB});
        generateStatement(stmt->body.get());
        loopStack.pop_back();
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(bodyBB);
        }
        builder->SetInsertPoint(endBB);
        return;
    }

    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "whilecond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "whilebody", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "whileend", function);

    builder->CreateBr(condBB);

    // Condition block
    builder->SetInsertPoint(condBB);
    llvm::Value* condition = generateExpression(stmt->condition.get());
    llvm::Value* condBool = toBool(condition);
    // Loop condition branches: hint the back-edge (body) as likely-taken.
    auto* whileCondBr = builder->CreateCondBr(condBool, bodyBB, endBB);
    if (optimizationLevel >= OptimizationLevel::O2) {
        llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(2000, 1);
        whileCondBr->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
    }


    // Body block
    builder->SetInsertPoint(bodyBB);
    loopStack.push_back({endBB, condBB});

    // Emit llvm.assume(var >= 0) for all alloca variables known to be
    // non-negative at O2+.  When a while loop carries a non-negative loop
    // variable (e.g. the Collatz x in `while (x != 1)`), LLVM's value-range
    // analysis does NOT automatically propagate the non-negativity across the
    // loop phi boundary.  Without an explicit assumption, LLVM conservatively
    // drops `nsw` flags from multiplications like `3 * x`, preventing
    // accumulation-interleaving and other dependency-chain optimisations.
    // Adding the assume INSIDE the body (on the phi successor value) gives
    // CVP and SCEV the evidence they need to keep nsw on derived operations.
    if (optimizationLevel >= OptimizationLevel::O2 && !dynamicCompilation_) {
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC_STMT(
            module.get(), llvm::Intrinsic::assume, {});
        llvm::Type* i64Ty = getDefaultType();
        for (auto& kv : namedValues) {
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(kv.second);
            if (!alloca) continue;
            if (!nonNegValues_.count(alloca)) continue;
            // Only assume for integer allocas — float/pointer non-negativity
            // is not expressed the same way and would confuse LLVM.
            if (!alloca->getAllocatedType()->isIntegerTy()) continue;
            const std::string varName = kv.first().str();
            llvm::Value* loaded = builder->CreateAlignedLoad(
                alloca->getAllocatedType(), alloca,
                llvm::MaybeAlign(8), (varName + ".assume.nn").c_str());
            // Widen to i64 if narrower (e.g. i1, i32) before comparison.
            llvm::Value* asI64 = loaded->getType() == i64Ty
                ? loaded
                : builder->CreateSExt(loaded, i64Ty, "nn.sext");
            llvm::Value* isNN = builder->CreateICmpSGE(
                asI64,
                llvm::ConstantInt::get(i64Ty, 0),
                (varName + ".nn").c_str());
            builder->CreateCall(assumeFn, {isNN});
        }
    }

    generateStatement(stmt->body.get());
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        auto* backBrWhile = builder->CreateBr(condBB);
        // hints: mustprogress for loop-idiom recognition.
        // NOTE: Interleave count and vectorize width are intentionally NOT
        // forced here.  Forcing these values overrides the vectorizer's cost
        // model, which can cause harmful code bloat and type widening.
        // The prefer-vector-width function attribute already guides the
        // vectorizer toward the correct register width.
        llvm::MDNode* mustProgress =
            llvm::MDNode::get(*context, {llvm::MDString::get(*context, "llvm.loop.mustprogress")});
        llvm::SmallVector<llvm::Metadata*, 4> loopMDs;
        loopMDs.push_back(nullptr);
        loopMDs.push_back(mustProgress);
        // While-loop unrolling: when truly nested inside another loop
        // (depth > 1, since we already incremented loopNestDepth_ for
        // this while-loop), cap the unroll factor to prevent LLVM from
        // over-unrolling branch-heavy inner loops that create massive
        // I-cache pressure.  Top-level while-loops (depth == 1) are
        // left to LLVM's cost model for optimal unrolling decisions.
        // When the function has @unroll, trust the user's intent and
        // let LLVM's cost model decide the factor for all nested loops.
        if (!inOptMaxFunction && !currentFuncHintUnroll_ && loopNestDepth_ > 1 && optimizationLevel >= OptimizationLevel::O2) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.unroll.disable")}));
        }
        // @vectorize / @novectorize: per-function loop vectorization hints.
        // Suppress vectorize.enable when the body contains a non-power-of-2
        // modulo used as a VALUE (e.g. `acc += (i*i) % 101`).  On x86-64
        // there is no native 64-bit vector division, so vectorizing such loops
        // scalarises urem anyway but causes LLVM to batch-process all unrolled
        // iterations upfront, which exhausts general-purpose registers and
        // introduces stack spills.  Without the hint LLVM's cost model picks
        // the naturally-interleaved scalar pattern that C's clang produces,
        // avoiding the spills and matching C performance.
        const bool whileBodyHasNonPow2ModVal = bodyHasNonPow2ModuloValue_;
        if (currentFuncHintNoVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        } else if (!whileBodyHasNonPow2ModVal
                   && (currentFuncHintVectorize_
                       || (currentFuncHintHot_ && optimizationLevel >= OptimizationLevel::O3
                           && loopNestDepth_ <= 1))) {
            // @hot at O3: auto-enable vectorization for top-level while-loops.
            // While-loops are less amenable to vectorization than for-loops
            // (no known trip count), so we require @hot as a signal that the
            // loop is performance-critical before applying this hint.
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        if (currentFuncHintHot_ && currentFuncHintVectorize_
            && !whileBodyHasNonPow2ModVal
            && optimizationLevel >= OptimizationLevel::O3
            && !currentFuncHintNoVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.interleave.count"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 4))}));
        }
        // parallel_accesses: suppress when the body has non-pow2 modulo
        // (scalar or value-producing).  In those loops, asserting independent
        // memory accesses causes LLVM to batch all unrolled iterations, which
        // creates excessive register pressure and stack spills.
        const bool wantParallelWhile = enableParallelize_
            && !currentFuncHintNoParallelize_
            && !bodyHasNonPow2Modulo_
            && loopNestDepth_ <= 1
            && (currentFuncHintParallelize_
                || currentFuncHintHot_
                || optimizationLevel >= OptimizationLevel::O3);
        if (wantParallelWhile) {
            llvm::MDNode* accessGroup = llvm::MDNode::getDistinct(*context, {});
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.parallel_accesses"),
                           accessGroup}));
            currentLoopAccessGroup_ = accessGroup;
        } else {
            currentLoopAccessGroup_ = nullptr;
        }

        llvm::MDNode* loopMD = llvm::MDNode::get(*context, loopMDs);
        loopMD->replaceOperandWith(0, loopMD);
        backBrWhile->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
    }

    // End block
    builder->SetInsertPoint(endBB);
    --loopNestDepth_;
    // Restore body-analysis flags, OR-ing this while loop's findings back in
    // so enclosing loops can see them.
    bodyHasNonPow2Modulo_ = savedBodyHasNonPow2Modulo || bodyHasNonPow2Modulo_;
    bodyHasNonPow2ModuloValue_ = savedBodyHasNonPow2ModuloValue || bodyHasNonPow2ModuloValue_;
    bodyHasNonPow2ModuloArrayStore_ = savedBodyHasNonPow2ModuloArrayStore || bodyHasNonPow2ModuloArrayStore_;
}

void CodeGenerator::generateDoWhile(DoWhileStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();

    const ScopeGuard scope(*this);

    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "dowhilebody", function);
    // Create condBB upfront so that 'continue' inside the body jumps to the
    // condition check (correct do-while semantics).  If the condition turns
    // out to be a compile-time constant we redirect condBB afterwards.
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "dowhilecond", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "dowhileend", function);

    // Jump directly to body (execute at least once)
    builder->CreateBr(bodyBB);

    // Body block
    builder->SetInsertPoint(bodyBB);
    loopStack.push_back({endBB, condBB});
    generateStatement(stmt->body.get());
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(condBB);
    }

    // Condition block
    builder->SetInsertPoint(condBB);
    llvm::Value* condition = generateExpression(stmt->condition.get());

    // Constant condition elimination for do-while: the body always executes
    // once, but we can skip the condition branch when the result is statically
    // known, replacing the conditional back-edge with an unconditional one.
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(condition)) {
        if (ci->isZero()) {
            // Condition is statically false: body executes once, fall through.
            builder->CreateBr(endBB);
        } else {
            // Condition is statically true: unconditional infinite loop.
            builder->CreateBr(bodyBB);
        }
        builder->SetInsertPoint(endBB);
        return;
    }

    llvm::Value* condBool = toBool(condition);
    auto* backBrDoWhile = builder->CreateCondBr(condBool, bodyBB, endBB);
    // Hint the back-edge (body) as likely-taken for branch prediction.
    if (optimizationLevel >= OptimizationLevel::O2) {
        llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(2000, 1);
        backBrDoWhile->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
    }
    // Attach loop metadata to the do-while back-edge, matching for-loop and
    // while-loop hints for consistent vectorization across all loop forms.
    {
        llvm::MDNode* mustProgress =
            llvm::MDNode::get(*context, {llvm::MDString::get(*context, "llvm.loop.mustprogress")});
        llvm::SmallVector<llvm::Metadata*, 4> loopMDs;
        loopMDs.push_back(nullptr);
        loopMDs.push_back(mustProgress);
        // @vectorize / @novectorize: per-function loop vectorization hints.
        if (currentFuncHintNoVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        } else if (currentFuncHintVectorize_
                   || (currentFuncHintHot_ && optimizationLevel >= OptimizationLevel::O3
                       && loopNestDepth_ <= 1)) {
            // Mirror while-loop: @hot+O3 for top-level do-while loops.
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        if (currentFuncHintHot_ && currentFuncHintVectorize_
            && optimizationLevel >= OptimizationLevel::O3
            && !currentFuncHintNoVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.interleave.count"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 4))}));
        }
        llvm::MDNode* loopMD = llvm::MDNode::get(*context, loopMDs);
        loopMD->replaceOperandWith(0, loopMD);
        backBrDoWhile->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
    }

    // End block
    builder->SetInsertPoint(endBB);
}

void CodeGenerator::generateFor(ForStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        codegenError("For loop outside of function", stmt);
    }

    ++loopNestDepth_;
    const bool savedBodyHasInnerLoop = bodyHasInnerLoop_;
    bodyHasInnerLoop_ = false;
    const bool savedBodyHasNonPow2Modulo = bodyHasNonPow2Modulo_;
    bodyHasNonPow2Modulo_ = false;
    const bool savedBodyHasNonPow2ModuloValue = bodyHasNonPow2ModuloValue_;
    bodyHasNonPow2ModuloValue_ = false;
    const bool savedBodyHasNonPow2ModuloArrayStore = bodyHasNonPow2ModuloArrayStore_;
    bodyHasNonPow2ModuloArrayStore_ = false;
    const bool savedBodyHasBackwardArrayRef = bodyHasBackwardArrayRef_;
    bodyHasBackwardArrayRef_ = false;
    // Track this loop's iterator name unconditionally (used to detect backward
    // array references at all optimization levels, not just O1+).
    loopIterVars_.insert(stmt->iteratorVar);

    const ScopeGuard scope(*this);

    // Allocate iterator variable — use annotated type when present
    llvm::Type* iterType = stmt->iteratorType.empty()
                               ? getDefaultType()
                               : resolveAnnotatedType(stmt->iteratorType);
    llvm::AllocaInst* iterAlloca = createEntryBlockAlloca(function, stmt->iteratorVar, iterType);
    bindVariable(stmt->iteratorVar, iterAlloca);

    // Initialize iterator
    llvm::Value* startVal = generateExpression(stmt->start.get());
    startVal = convertTo(startVal, iterType);
    builder->CreateStore(startVal, iterAlloca);

    // Get end value
    llvm::Value* endVal = generateExpression(stmt->end.get());
    endVal = convertTo(endVal, iterType);

    // Empty range elimination: when start and end are compile-time constants
    // and start == end, the loop body never executes (the condition check
    // `i < end` (ascending) or `i > end` (descending) fails on the first
    // iteration).  This avoids emitting the entire loop structure.
    if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
        if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endVal)) {
            if (startCI->getSExtValue() == endCI->getSExtValue()) {
                return;
            }
        }
    }

    // Get step value.  When not specified, default to +1 for ascending ranges
    // (start < end) and -1 for descending ranges (start > end) so that
    // `for (i in 5...0)` iterates 5,4,3,2,1 as users would expect.
    llvm::Value* stepVal;
    bool stepKnownPositive = false;
    bool stepKnownNonZero = false;
    if (stmt->step) {
        stepVal = generateExpression(stmt->step.get());
        stepVal = convertTo(stepVal, iterType);
        if (auto* stepCI = llvm::dyn_cast<llvm::ConstantInt>(stepVal)) {
            stepKnownPositive = stepCI->getSExtValue() > 0;
            stepKnownNonZero = stepCI->getSExtValue() != 0;
        }
    } else {
        // Detect compile-time ascending ranges and emit simpler loop code.
        bool ascending = false;
        if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
            if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endVal)) {
                // Both bounds known: ascending when start < end.
                ascending = startCI->getSExtValue() < endCI->getSExtValue();
            } else if (startCI->getSExtValue() >= 0) {
                // Common patterns: for (i in 0...n), for (i in 1...n), etc.
                // When start is a non-negative constant and end is variable,
                // the loop is virtually always ascending with the default
                // step of +1.  If end <= start, the condition (i < end) fails
                // immediately and the loop body never executes.  Descending
                // ranges from non-negative starts require explicit step
                // syntax (start...end...-1).
                ascending = true;
            }
        }
        const unsigned iterBits = iterType->getIntegerBitWidth();
        if (ascending) {
            stepVal = llvm::ConstantInt::get(iterType, 1);
            stepKnownPositive = true;
            stepKnownNonZero = true;
        } else {
            llvm::Value* isDesc = builder->CreateICmpSGT(startVal, endVal, "for.isdesc");
            llvm::Value* posOne = llvm::ConstantInt::get(iterType, 1);
            llvm::Value* negOne = llvm::ConstantInt::get(*context, llvm::APInt(iterBits, static_cast<uint64_t>(-1), true));
            stepVal = builder->CreateSelect(isDesc, negOne, posOne, "for.autostep");
            // Auto-computed step is always +1 or -1, never zero.
            stepKnownNonZero = true;
        }
    }

    llvm::Value* zero = llvm::ConstantInt::get(stepVal->getType(), 0, true);

    // Create blocks
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "forcond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "forbody", function);
    llvm::BasicBlock* incBB = llvm::BasicBlock::Create(*context, "forinc", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "forend", function);

    // Skip the step-zero check when step is known non-zero at compile time.
    if (stepKnownNonZero) {
        builder->CreateBr(condBB);
    } else {
        llvm::BasicBlock* stepCheckBB = llvm::BasicBlock::Create(*context, "forstepcheck", function);
        llvm::BasicBlock* stepFailBB = llvm::BasicBlock::Create(*context, "forstepfail", function);
        builder->CreateBr(stepCheckBB);
        builder->SetInsertPoint(stepCheckBB);
        llvm::Value* stepNonZero = builder->CreateICmpNE(stepVal, zero, "stepnonzero");
        builder->CreateCondBr(stepNonZero, condBB, stepFailBB);

        builder->SetInsertPoint(stepFailBB);
        const std::string errorMessage = "Runtime error: for-loop step cannot be zero for iterator '" + stmt->iteratorVar + "'\n";
        llvm::GlobalVariable* messageVar = builder->CreateGlobalString(errorMessage, "forstepmsg");
        llvm::Constant* zeroIndex = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0);
        llvm::Constant* indices[] = {zeroIndex, zeroIndex};
        llvm::Constant* message =
            llvm::ConstantExpr::getInBoundsGetElementPtr(messageVar->getValueType(), messageVar, indices);
        builder->CreateCall(getPrintfFunction(), {message});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();
    }

    builder->SetInsertPoint(condBB);
    llvm::Value* curVal = builder->CreateLoad(iterType, iterAlloca, stmt->iteratorVar.c_str());
    llvm::Value* continueCond;
    if (stepKnownPositive) {
        // Fast path: known ascending loop.
        // When the iterator alloca and end value are both proven non-negative,
        // use an unsigned comparison.  Two non-negative i64 values compare
        // identically whether the comparison is signed or unsigned, but unsigned
        // comparisons allow LLVM's vectorizer and SCEV to work with unsigned
        // induction variables, which unlocks more aggressive loop transforms.
        const bool iterNonNeg = nonNegValues_.count(iterAlloca) > 0;
        const auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endVal);
        const bool endNonNeg  = nonNegValues_.count(endVal) > 0
            || (endCI && !endCI->isNegative());
        if (iterNonNeg && endNonNeg) {
            continueCond = builder->CreateICmpULT(curVal, endVal, "forcond_ult");
        } else {
            continueCond = builder->CreateICmpSLT(curVal, endVal, "forcond_lt");
        }
    } else {
        llvm::Value* stepPositive = builder->CreateICmpSGT(stepVal, zero, "steppositive");
        llvm::Value* forwardCond = builder->CreateICmpSLT(curVal, endVal, "forcond_lt");
        llvm::Value* backwardCond = builder->CreateICmpSGT(curVal, endVal, "forcond_gt");
        continueCond = builder->CreateSelect(stepPositive, forwardCond, backwardCond, "forcond_range");
    }
    auto* forCondBr = builder->CreateCondBr(continueCond, bodyBB, endBB);
    // Hint loop back-edge as likely-taken for branch prediction.
    if (optimizationLevel >= OptimizationLevel::O2) {
        llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(2000, 1);
        forCondBr->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
    }

    // Body block
    builder->SetInsertPoint(bodyBB);

    // OmScript-specific optimization: emit @llvm.assume(iter >= 0) for
    // ascending for-loops.  Unlike C, OmScript's for-loop semantics guarantee
    // that the iterator is ALWAYS in [start, end) and cannot be modified inside
    // the loop body.  This assumption enables LLVM's CorrelatedValuePropagation
    // and SCEV passes to prove non-negativity of expressions involving the
    // iterator, which in turn enables srem→urem and sdiv→udiv conversions,
    // better vectorization cost modeling, and elimination of sign-extension
    // overhead.  C compilers cannot make this assumption because C loop
    // variables can be arbitrarily modified in the loop body.
    // Skip in JIT/dynamic mode: the assume adds raw instruction count that
    // the JIT baseline passes may not eliminate, and the per-function
    // optimization in JIT mode doesn't run the full CVP pipeline anyway.
    if (optimizationLevel >= OptimizationLevel::O2 && stepKnownPositive && !dynamicCompilation_) {
        // Check if start is known non-negative (common: for (i in 0...n))
        bool startNonNeg = false;
        if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
            startNonNeg = startCI->getSExtValue() >= 0;
        }
        if (startNonNeg) {
            llvm::Value* iterVal = builder->CreateLoad(iterType, iterAlloca, "iter.assume");
            llvm::Function* assumeFn = OMSC_GET_INTRINSIC_STMT(
                module.get(), llvm::Intrinsic::assume, {});
            // Lower bound: assume iter >= 0.
            llvm::Value* isNonNeg = builder->CreateICmpSGE(
                iterVal, llvm::ConstantInt::get(iterType, 0), "iter.nonneg");
            builder->CreateCall(assumeFn, {isNonNeg});
            // Upper bound: assume iter < end.  OmScript's for-loop semantics
            // guarantee the iterator never equals or exceeds end inside the body
            // (the loop exits before that).  This assumption lets LLVM's
            // CorrelatedValuePropagation, SCEV, and InstCombine prove tighter
            // ranges on derived expressions (e.g. iter*k, iter%m, arr[iter]),
            // enabling strength reduction, signed→unsigned conversions, and
            // bounds-check elimination for expressions involving the iterator.
            // C compilers emit this implicitly when the variable is loop-local
            // and the loop condition is visible; for function arguments (like n)
            // C cannot propagate this across call boundaries without LTO.
            llvm::Value* iterVal2 = builder->CreateLoad(iterType, iterAlloca, "iter.ub");
            llvm::Value* isLtEnd = builder->CreateICmpSLT(
                iterVal2, endVal, "iter.lt.end");
            builder->CreateCall(assumeFn, {isLtEnd});
            // Track the alloca as producing non-negative values so that
            // expressions derived from the loop counter can emit urem/udiv
            // instead of srem/sdiv.
            nonNegValues_.insert(iterAlloca);
        }
    }

    // Compile-time bounds check elimination: for ascending for-loops starting
    // at a non-negative constant, the iterator variable is always in
    // [start, endVal).  If the end bound is known (e.g. len(arr), a constant,
    // or a named variable), we record the iterator as safe so that array index
    // operations like arr[i] can skip runtime bounds checks when the array
    // length >= endVal.
    //
    // This is a zero-cost abstraction: the safety guarantee is enforced
    // statically by the compiler (OmScript's for-loop semantics guarantee
    // the iterator cannot be modified inside the body), so no runtime check
    // is needed.
    if (stepKnownPositive && optimizationLevel >= OptimizationLevel::O1) {
        bool startNonNegForElim = false;
        if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
            startNonNegForElim = startCI->getSExtValue() >= 0;
        }
        if (startNonNegForElim) {
            safeIndexVars_.insert(stmt->iteratorVar);
            loopIterEndBound_[stmt->iteratorVar] = endVal;
            // Track loop start bound for negative-offset bounds check
            // elision: for patterns like for(i in C...n) { arr[i - K] },
            // knowing start == C proves i - K >= C - K >= 0 when C >= K.
            loopIterStartBound_[stmt->iteratorVar] = startVal;
        }
    }

    loopStack.push_back({endBB, incBB});
    generateStatement(stmt->body.get());
    loopStack.pop_back();

    // Clean up: iterator no longer has guaranteed bounds outside the loop.
    safeIndexVars_.erase(stmt->iteratorVar);
    loopIterEndBound_.erase(stmt->iteratorVar);
    loopIterStartBound_.erase(stmt->iteratorVar);

    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(incBB);
    }

    // Increment block
    builder->SetInsertPoint(incBB);
    llvm::Value* nextVal = builder->CreateLoad(iterType, iterAlloca, stmt->iteratorVar.c_str());
    // OmScript advantage: for ascending loops starting from a non-negative
    // value, both nsw AND nuw flags are correct on the increment.
    //
    // nsw is correct because OmScript's for-loop semantics guarantee the
    // iterator stays within representable signed range (the loop condition
    // `iter < end` is checked with ICmpSLT before each iteration).
    //
    // nuw is also correct because: the iterator starts ≥ 0 and nsw
    // guarantees no signed overflow, so the result is always in
    // [0, 2^63-1].  Values in this range cannot wrap unsigned (they are
    // well below UINT64_MAX = 2^64-1).
    //
    // The nuw flag is critical because LLVM's loop unroller propagates it
    // to unrolled copies, enabling isValueNonNegative to prove non-negativity
    // of derived expressions (which in turn enables srem→urem conversion
    // in unrolled iterations).  C compilers can only safely set nsw.
    llvm::Value* incVal;
    bool startNonNegForInc = false;
    if (stepKnownPositive) {
        if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
            startNonNegForInc = startCI->getSExtValue() >= 0;
        }
    }
    if (startNonNegForInc) {
        incVal = builder->CreateAdd(nextVal, stepVal, "nextvar", /*HasNUW=*/true, /*HasNSW=*/true);
    } else {
        incVal = builder->CreateNSWAdd(nextVal, stepVal, "nextvar");
    }
    builder->CreateStore(incVal, iterAlloca);
    auto* backBr = builder->CreateBr(condBB);

    // Attach SIMD interleave hint to the for-loop back-edge at O2+.
    // Vectorization and unrolling are controlled globally via PipelineTuningOptions
    // in runOptimizationPasses(); per-loop forced enable hints are omitted to
    // avoid "unable to perform the requested transformation" diagnostic warnings
    // when the optimizer cannot satisfy them.
    {
        // Loop metadata: mustprogress enables loop-idiom recognition (auto
        // memset/memcpy detection); interleave.count improves SIMD throughput;
        // at O3, vectorize.width uses the target-aware preferred SIMD width.
        llvm::MDNode* mustProgress =
            llvm::MDNode::get(*context, {llvm::MDString::get(*context, "llvm.loop.mustprogress")});
        llvm::SmallVector<llvm::Metadata*, 6> loopMDs;
        loopMDs.push_back(nullptr); // self-reference placeholder (fixed below)
        loopMDs.push_back(mustProgress);
        // NOTE: interleave.count and vectorize.width are intentionally NOT
        // forced here — see the while-loop comment for rationale.
        // At O3, hint the unroller for small constant-trip-count loops.
        // When both start and end are compile-time constants, the trip count
        // is known; if it's ≤ 64, suggest full unrolling to eliminate loop
        // overhead entirely.  For trip counts ≤ 16, this typically produces
        // straight-line code that fits in the I-cache.
        bool addedUnrollHint = false;
        if (optimizationLevel >= OptimizationLevel::O3 && enableUnrollLoops_) {
            if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
                if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endVal)) {
                    const int64_t startV = startCI->getSExtValue();
                    const int64_t endV = endCI->getSExtValue();
                    // Compute trip count safely using unsigned subtraction to
                    // avoid signed overflow when start and end are far apart.
                    const uint64_t tripCount = (endV >= startV)
                        ? static_cast<uint64_t>(endV) - static_cast<uint64_t>(startV)
                        : static_cast<uint64_t>(startV) - static_cast<uint64_t>(endV);
                    if (tripCount > 0 && tripCount <= 64) {
                        if (tripCount <= 8) {
                            // Very small constant-trip-count loops: fully unroll.
                            // llvm.loop.unroll.full is a stronger hint than .count
                            // and eliminates the loop entirely, removing branch
                            // overhead and enabling cross-iteration optimization.
                            loopMDs.push_back(llvm::MDNode::get(
                                *context,
                                {llvm::MDString::get(*context, "llvm.loop.unroll.full")}));
                        } else {
                            llvm::MDNode* unrollCount = llvm::MDNode::get(
                                *context,
                                {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                                     llvm::Type::getInt32Ty(*context), static_cast<uint32_t>(tripCount)))});
                            loopMDs.push_back(unrollCount);
                        }
                        addedUnrollHint = true;
                    }
                }
            }
        }
        // For variable-trip-count for-loops at O3, trust LLVM's cost-model-
        // driven unroller to choose the optimal factor based on loop body
        // complexity, register pressure, and target microarchitecture.
        // Previously we capped unroll at 2, but that cap prevented the
        // vectorizer from choosing its own interleaving factor and sometimes
        // produced suboptimal code.  OPTMAX functions and @unroll-annotated
        // functions still get their explicit hints below.
        // @unroll / @nounroll: per-function loop unrolling overrides.
        // @nounroll disables unrolling entirely; @unroll requests aggressive unrolling.
        //
        // SMART HINT SUPPRESSION: Hints are advisory, not mandatory.  The
        // compiler performs deep analysis to determine whether applying a hint
        // would produce suboptimal machine code and suppresses it when:
        //
        //  (a) Loop nesting depth ≥ 2: Unrolling deeply-nested loops causes
        //      exponential code growth (4x unroll of outer * 4x unroll of inner
        //      = 16x code), massive register pressure, I-cache thrashing, and
        //      prevents the vectorizer from working on a clean inner loop.
        //      At depth ≥ 2, let LLVM's cost-model-driven unroller decide.
        //
        //  (b) Loop body contains data-dependent chains with ≥ 3 variables
        //      that feed back into themselves (e.g., acc ^= ...; b |= ...;
        //      c += ...).  Unrolling such loops increases register pressure
        //      without enabling ILP because the dependency chain serializes
        //      the computation anyway.
        //
        //  (c) When both @vectorize and @unroll are requested, the innermost
        //      loop prefers vectorization: the unroll hint is suppressed so
        //      the vectorizer sees a clean single-iteration loop body.  LLVM's
        //      vectorizer has its own interleaving heuristic that subsumes
        //      manual unrolling for vectorizable loops.
        //
        // In all suppression cases, the loop is left WITHOUT an explicit
        // unroll hint, allowing LLVM's cost model to choose the optimal
        // factor based on loop body complexity, register pressure, and
        // target microarchitecture — producing the absolute optimal machine
        // code for each specific scenario.
        const bool deeplyNested = loopNestDepth_ >= 2;
        const bool vectorizePreferred = currentFuncHintVectorize_ && !bodyHasInnerLoop_;
        const bool suppressUnrollHint = deeplyNested || vectorizePreferred;
        // Comparison-context non-pow2 modulo loops (e.g. i%3==0): suppress
        // distribute.enable, parallel_accesses, and cap OPTMAX unroll at 8x.
        // Used in multiple places below so defined early.
        const bool cmpModuloLoop = bodyHasNonPow2Modulo_ && !bodyHasNonPow2ModuloValue_;

        if (currentFuncHintNoUnroll_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.unroll.disable")}));
        } else if (bodyHasInnerLoop_) {
            // When the body contains an inner loop (while/for), disable
            // unrolling of the outer loop.  Each outer iteration already
            // contains the full inner loop, so unrolling the outer loop
            // creates N copies of the entire inner loop body — massive
            // I-cache pressure with zero throughput benefit.
            //
            // This matches clang's behaviour for equivalent C code: clang's
            // cost model sees the inner loop and emits no unroll hint,
            // leaving the outer loop as a single copy.
            //
            // For OPTMAX functions without an explicit @unroll: also disable
            // unrolling.  The OPTMAX LoopUnrollPass uses OnlyWhenForced=false
            // at OptLevel=3 and may ignore llvm.loop.unroll.disable.  Emitting
            // both unroll.disable and unroll.count=1 ensures the constraint is
            // respected even by aggressive unrollers.
            if (!currentFuncHintUnroll_) {
                // @hot outer loop at O3 with inner loop: emit unroll.count to enable
                // out-of-order ILP across independent outer iterations.  Clang's cost
                // model unrolls the outer for-loop 8× for equivalent C code (e.g.
                // Collatz), allowing the CPU's out-of-order backend to overlap
                // independent inner-loop chains.  Without this hint LLVM disables
                // unrolling when it sees an inner loop — correct for throughput-bound
                // loops, but wrong for latency-bound irregular inner loops.
                //
                // Use 8 for both @hot at O3 and OPTMAX: matches clang's default
                // cost-model choice for outer loops with irregular inner loops
                // (e.g. Collatz, modular exponentiation).  8 independent inner-loop
                // chains exercise the CPU's OOO backend; the ROB can overlap 2-4
                // chains with ~15-30 iterations each.  Only when not deeply nested.
                if (currentFuncHintHot_ && !deeplyNested
                        && optimizationLevel >= OptimizationLevel::O3) {
                    const unsigned outerUnrollCount = 8u;
                    loopMDs.push_back(llvm::MDNode::get(
                        *context,
                        {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                         llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                             llvm::Type::getInt32Ty(*context), outerUnrollCount))}));
                } else {
                    loopMDs.push_back(llvm::MDNode::get(
                        *context, {llvm::MDString::get(*context, "llvm.loop.unroll.disable")}));
                    // llvm.loop.unroll.count = 1 is a belt-and-suspenders constraint:
                    // it explicitly tells LLVM's LoopUnroll pass to produce exactly 1
                    // copy even when the pass ignores unroll.disable (e.g. at OptLevel=3
                    // with OnlyWhenForced=false as used in the OPTMAX pass).
                    loopMDs.push_back(llvm::MDNode::get(
                        *context,
                        {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                         llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                             llvm::Type::getInt32Ty(*context), 1u))}));
                }
            } else {
                // @unroll explicitly requested on an outer loop with inner loop:
                // use 8 for @hot at O3; otherwise conservative 2.
                const unsigned explicitCount =
                    (currentFuncHintHot_ && !deeplyNested
                     && optimizationLevel >= OptimizationLevel::O3)
                    ? 8u : 2u;
                loopMDs.push_back(llvm::MDNode::get(
                    *context,
                    {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                     llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                         llvm::Type::getInt32Ty(*context), explicitCount))}));
            }
        } else if (currentFuncHintUnroll_ && !addedUnrollHint && !suppressUnrollHint) {
            // @unroll on a non-suppressed loop: apply the unroll count hint.
            // For variable-trip-count loops, unroll.full is ignored by LLVM
            // (the unroller refuses to fully unroll unbounded loops).  Use a
            // concrete unroll.count instead, giving LLVM a target that matches
            // GCC's aggressive unrolling for FP-heavy loops.
            // OPTMAX uses 16 to maximally hide FP/call latency through ILP;
            // regular functions use 4 to balance ILP gains vs I-cache pressure.
            //
            // EXCEPTION: loops with backward array references (arr[i-K]) have a
            // loop-carried recurrence.  With 16x unrolling LLVM cannot recognize
            // the carry pattern and emits a 16-deep store-to-load chain (4 cycles
            // per hop × 16 hops = 64 cycles per 16 elements).  Capping at 4 gives
            // LLVM the short chain it needs to promote the running value to a
            // register PHI (1 cycle per element), matching C's recurrence handling.
            //
            // EXCEPTION: loops where a non-pow2 modulo result is stored to an array
             // (bodyHasNonPow2ModuloArrayStore_=true).  Here, providing an explicit
             // unroll.count=4 interferes with LLVM's O3 cost-model-driven unroller:
             // the hint causes the unroll pass to set up divisibility scaffolding
             // (an epilogue loop) but then the i128 magic-multiply cost model causes
             // it to abort the body unrolling, leaving a 1x scalar loop instead.
             // Without the explicit hint, LLVM's O3 cost-model unroller applies the
             // same analysis it uses for equivalent C code and unrolls more
             // aggressively (matching clang's 8x unrolling for modulo array-init loops).
             //
             // EXCEPTION: loops with comparison-context non-pow2 modulo (e.g. i%3==0).
             // OPTMAX would normally use unroll.count=16, but that creates 18 live
             // PHI variables per loop header (16 LSR IVs + acc + niter), exceeding
             // x86-64's 15 usable GP registers and causing 3+ register spills.
             // Capping at 8 (matching clang's behaviour for equivalent C loops) reduces
             // PHI count to 10, eliminating spills and matching C's performance.
             const bool modArrayStore = bodyHasNonPow2ModuloArrayStore_
                 && optimizationLevel >= OptimizationLevel::O3;
             // Backward array refs (arr[i] += arr[i-1]): serial loop-carried dependency.
             // Suppress explicit unroll hint at O3 — LLVM's cost-model freely chooses
             // the optimal factor (typically 8x for prefix-scan shapes), matching C.
             // An explicit unroll.count=4 would under-constrain at O3 where integrated
             // compilation pressure causes the backend to honour the hint literally.
             const bool suppressUnrollForBackward = bodyHasBackwardArrayRef_
                 && optimizationLevel >= OptimizationLevel::O3;
             if (!modArrayStore && !suppressUnrollForBackward) {
                 static constexpr unsigned kOptMaxUnrollCount = 16;
                 static constexpr unsigned kOptMaxCmpModuloUnrollCount = 8;
                 static constexpr unsigned kDefaultUnrollCount = 4;
                 const unsigned unrollCount =
                     inOptMaxFunction
                     ? (cmpModuloLoop ? kOptMaxCmpModuloUnrollCount : kOptMaxUnrollCount)
                     : kDefaultUnrollCount;
                 loopMDs.push_back(llvm::MDNode::get(
                     *context,
                     {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                          llvm::Type::getInt32Ty(*context), unrollCount))}));
             }
             // modArrayStore / suppressUnrollForBackward: emit no unroll hint — let
             // LLVM's O3 cost-model unroller decide freely, matching clang.
        }
        // @vectorize / @novectorize: per-function loop vectorization hints.
        // At O3, also enable vectorization for @hot functions even without
        // an explicit @vectorize annotation.  LLVM's vectorizer cost model
        // can be overly conservative for i64 loops (e.g. those containing
        // urem/udiv by constant) where the magic-number-multiply cost is
        // overestimated.  Explicitly enabling vectorization lets the
        // vectorizer proceed and produce profitable SIMD code that matches
        // clang's aggressive auto-vectorization of hot loops.
        if (currentFuncHintNoVectorize_
                || (bodyHasNonPow2Modulo_ && !bodyHasNonPow2ModuloValue_
                    && !currentFuncHintVectorize_)
                || (bodyHasNonPow2ModuloArrayStore_ && !currentFuncHintVectorize_)) {
            // Disable vectorization when:
            //  (a) @novectorize is set explicitly, OR
            //  (b) loop has non-power-of-2 modular arithmetic used ONLY as a
            //      branch condition (e.g. i%3==0), not as a value (i%100 for max).
            // Vectorizing case (b) generates urem <N x i64> — N serialized
            // ~25-cycle divisions vs scalar ~5-cycle magic-multiply reduction.
            // Case (b) is detected via inComparisonContext_ during codegen:
            //   bodyHasNonPow2Modulo_=true && bodyHasNonPow2ModuloValue_=false
            // When modulo feeds into max(a%100, b%100), bodyHasNonPow2ModuloValue_
            // is set, so we keep vectorize.enable=true for SIMD abs/min/max.
            //  (c) loop stores modulo result to an array element (arr[i]=val%K):
            //      x86-64 has no native 64-bit vector division — the vectorizer
            //      scalarizes urem <N x i64> to N sequential extract/divide/insert
            //      sequences.  Scalar unrolled code achieves better ILP because
            //      the CPU's OOO engine can pipeline N independent magic-multiply
            //      chains simultaneously, while the scalar extract overhead is zero.
            //  (d) loop has a backward array reference (arr[i] += arr[i-1]):
            //      serial loop-carried dependency — the vectorizer will correctly
            //      reject it, but not before adding unroll.disable to the metadata
            //      when it fails with vectorize.enable=true.  Suppress upfront.
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        } else if (currentFuncHintVectorize_
                   || (optimizationLevel >= OptimizationLevel::O3
                       && !bodyHasInnerLoop_ && stepKnownPositive)) {
            // Force-enable vectorization at O3 for hot loops.
            // Note: loops with non-power-of-2 modulo already have vectorize=0
            // emitted above (when used as condition) or rely on LLVM cost model
            // (when used as a value, e.g. for SIMD min/max patterns).
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        // Interleaving: @hot+@vectorize at O3 → 4 iterations; plain O3
        // auto-vectorized loops → 2 iterations (balances register pressure
        // with latency hiding without the aggressive 4× that @hot requests).
        // Suppress interleave.count for cmpModuloLoop (non-pow2 modulo used
        // only in comparison context, e.g. i%3==0).  For those loops we
        // already emit vectorize.enable=false + unroll.count=8.  When
        // interleave.count is ALSO present, LLVM's vectorizer treats it as
        // an implicit unroll-by-2 request, which conflicts with the explicit
        // unroll.count=8, causes the loop unroller to add unroll.disable
        // (preventing the 8× unrolling), and leaves a 4× scalar loop instead.
        // Without interleave.count, the vectorize.enable=false + unroll.count=8
        // combination cleanly produces the desired 8× unrolled loop, matching
        // clang's natural cost-model behaviour for equivalent C code.
        if (optimizationLevel >= OptimizationLevel::O3
            && !currentFuncHintNoVectorize_
            && !cmpModuloLoop
            && !bodyHasInnerLoop_ && stepKnownPositive) {
            const int32_t interleave = (currentFuncHintHot_ && currentFuncHintVectorize_) ? 4 : 2;
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.interleave.count"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), interleave))}));
        }

        // ── Enhanced vectorization: distribution hint ────────────────
        // At O3 for non-nested ascending loops, hint that the loop may be
        // distributed (split into multiple loops with simpler bodies).
        // This enables LLVM to vectorize subsets of a loop body even when
        // some parts have cross-iteration dependencies.
        //
        // Example:  for i in 0...n { arr[i]=i*2; sum+=arr[i] }
        //   → Loop 1: for i in 0...n { arr[i]=i*2 }  (vectorizable)
        //   → Loop 2: for i in 0...n { sum+=arr[i] }  (reduction)
        // At O2, enable distribution only for @hot functions since the
        // analysis cost is non-trivial for cold code.
        // Loops with comparison-context non-pow2 modulo (e.g. i%3==0) must
        // also suppress distribution.  LoopDistribute finds nothing to split
        // (no arrays), but marks the loop with unroll.disable after failing,
        // clobbering the unroll.count hint and leaving a 1x scalar loop.
        if (stepKnownPositive && !deeplyNested && !bodyHasInnerLoop_
            && !bodyHasBackwardArrayRef_
            && !bodyHasNonPow2ModuloArrayStore_
            && !cmpModuloLoop
            && (optimizationLevel >= OptimizationLevel::O3
                || (optimizationLevel >= OptimizationLevel::O2 && currentFuncHintHot_))) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.distribute.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }

        // Loops with backward array references (arr[i-K]) have a loop-carried
        // dependency.  Disable LoopVersioningLICM and LoopDistribute for these
        // loops: both passes create multi-version loop structures that use
        // N+1 element vector loads, causing partial store-to-load forwarding
        // stalls instead of the efficient register-carry pattern.
        if (bodyHasBackwardArrayRef_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.distribute.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.licm_versioning.disable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }

        // ── Software pipelining hint for @hot O3 innermost loops ─────
        // Software pipelining hints are only added when @vectorize is
        // explicitly requested.  For other loops, LLVM's backend makes
        // its own scheduling decisions which are generally optimal.
        // Adding pipeline hints unconditionally was found to interfere
        // with the vectorizer's cost model on some loop shapes.
        if (currentFuncHintVectorize_ && optimizationLevel >= OptimizationLevel::O3
            && !deeplyNested && !bodyHasInnerLoop_ && stepKnownPositive) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.pipeline.disable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.pipeline.initiationinterval"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1))}));
        }

        // ── Auto-parallelization hints ─────────────────────────────
        // At O2+ with -fparallelize, mark loops that are safe to execute
        // in parallel.  We attach llvm.loop.parallel_accesses metadata that
        // references an access group placed on every load/store in the loop
        // body.  This tells LLVM's LoopVectorizer and Polly that there are
        // no cross-iteration dependencies — enabling:
        //   1. Vectorization with unordered reductions
        //   2. Polly automatic OpenMP parallelization
        //   3. LoopDistribute to split parallelizable sub-loops
        //
        // We only mark loops as parallel when:
        //   - The step is known positive (forward iteration)
        //   - The loop is not deeply nested (to avoid over-parallelizing)
        //   - The function is @parallel or @hot (user-indicated hotness)
        //   - -fno-parallelize was not passed
        // At O3, all non-nested forward loops get the hint regardless of
        // @hot/@parallel annotations.
        //
        // IMPORTANT: Suppress parallel_accesses when the loop body has a
        // backward array reference (arr[i-K] pattern). Such loops have
        // loop-carried dependencies: iteration i reads arr[i-K] which was
        // written in a previous iteration. Marking these accesses as parallel
        // (independent) prevents LLVM from promoting arr[i-1] to a register
        // accumulator — the critical optimization that makes prefix scans,
        // sliding windows, and similar recurrence patterns fast. Without
        // parallel_accesses, LLVM can convert the store-to-load chain (4-cycle
        // latency per iteration) to a register-carried accumulator (1-cycle).
        // Also suppress for loops where a non-pow2 modulo result is stored to
        // an array (bodyHasNonPow2ModuloArrayStore_=true): these loops have
        // vectorize.enable=false, so parallel_accesses would trigger the
        // LoopDistribute pass on a non-vectorizable loop.  LoopDistribute
        // responds by rewriting the loop metadata and replacing the
        // unroll.count=4 hint with unroll.disable, causing the loop to run
        // scalar 1-element-per-iteration instead of 4x unrolled.
        const bool wantParallel = enableParallelize_
            && !currentFuncHintNoParallelize_
            && stepKnownPositive
            && !deeplyNested
            && !bodyHasInnerLoop_
            && !bodyHasBackwardArrayRef_
            && !bodyHasNonPow2ModuloArrayStore_
            && !cmpModuloLoop
            && (currentFuncHintParallelize_
                || currentFuncHintHot_
                || optimizationLevel >= OptimizationLevel::O3);
        if (wantParallel) {
            // Create an access group — this is a unique MDNode that we
            // attach to every load/store inside the loop body via
            // !llvm.access.group.  The loop metadata then references it
            // via llvm.loop.parallel_accesses, forming the contract:
            //   "all memory operations in this group are independent
            //    across iterations."
            llvm::MDNode* accessGroup = llvm::MDNode::getDistinct(*context, {});
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.parallel_accesses"),
                           accessGroup}));
            // Store the access group so codegen can attach it to memory
            // operations emitted inside this loop body.
            currentLoopAccessGroup_ = accessGroup;
        } else {
            currentLoopAccessGroup_ = nullptr;
        }

        llvm::MDNode* loopMD = llvm::MDNode::get(*context, loopMDs);
        loopMD->replaceOperandWith(0, loopMD);
        backBr->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
    }

    // End block
    builder->SetInsertPoint(endBB);
    --loopNestDepth_;
    loopIterVars_.erase(stmt->iteratorVar);
    // Restore the flag; propagate upward so enclosing for-loops also see it.
    bodyHasInnerLoop_ = savedBodyHasInnerLoop || bodyHasInnerLoop_;
    bodyHasNonPow2Modulo_ = savedBodyHasNonPow2Modulo || bodyHasNonPow2Modulo_;
    bodyHasNonPow2ModuloValue_ = savedBodyHasNonPow2ModuloValue || bodyHasNonPow2ModuloValue_;
    bodyHasNonPow2ModuloArrayStore_ = savedBodyHasNonPow2ModuloArrayStore || bodyHasNonPow2ModuloArrayStore_;
    // Backward refs don't propagate upward: a backward access in an inner loop
    // doesn't imply the outer loop's iterations are dependent.
    bodyHasBackwardArrayRef_ = savedBodyHasBackwardArrayRef;
}

void CodeGenerator::generateForEach(ForEachStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        codegenError("For-each loop outside of function", stmt);
    }

    const ScopeGuard scope(*this);

    // Evaluate the collection (array or string)
    llvm::Value* collVal = generateExpression(stmt->collection.get());

    // Detect whether the collection is a string.  Strings are raw char
    // pointers without a length header; arrays use [length, e0, e1, ...].
    const bool isStr = collVal->getType()->isPointerTy() || isStringExpr(stmt->collection.get());
    // Detect whether this is an array whose elements are string pointers.
    const bool isStrArray = !isStr && isStringArrayExpr(stmt->collection.get());

    // Normalise to i64 for uniformity; we'll re-cast to ptr below.
    collVal = toDefaultType(collVal);

    // Convert i64 → pointer
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    llvm::Value* basePtr = builder->CreateIntToPtr(collVal, ptrTy, "foreach.baseptr");

    // Get the collection length
    llvm::Value* lenVal;
    if (isStr) {
        // String: strlen (no length header)
        lenVal = builder->CreateCall(getOrDeclareStrlen(), {basePtr}, "foreach.strlen");
    } else {
        // Array: length stored in slot 0
        auto* lenLoad = builder->CreateLoad(getDefaultType(), basePtr, "foreach.len");
        // TBAA: array length (slot 0) never aliases element stores (slots 1+).
        lenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
        // !range: array lengths are always in [0, INT64_MAX).
        lenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        lenVal = lenLoad;
    }

    // Allocate hidden index variable and the user's iterator variable
    llvm::AllocaInst* idxAlloca = createEntryBlockAlloca(function, "_foreach_idx");
    builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), idxAlloca);
    // The foreach hidden index starts at 0 and only increments, so it is
    // always non-negative.  Track this so expressions derived from the
    // index (e.g. arr[i+1]) can use urem/udiv instead of srem/sdiv.
    nonNegValues_.insert(idxAlloca);

    llvm::AllocaInst* iterAlloca = createEntryBlockAlloca(function, stmt->iteratorVar);
    bindVariable(stmt->iteratorVar, iterAlloca);
    // If the collection is a string array, mark the iterator as a string
    // variable so that print(), len(), and comparison operators handle it
    // correctly inside the loop body.
    if (isStrArray)
        stringVars_.insert(stmt->iteratorVar);

    // Create blocks
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "foreach.cond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "foreach.body", function);
    llvm::BasicBlock* incBB = llvm::BasicBlock::Create(*context, "foreach.inc", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "foreach.end", function);

    builder->CreateBr(condBB);

    // Condition: idx < length
    builder->SetInsertPoint(condBB);
    llvm::Value* curIdx = builder->CreateLoad(getDefaultType(), idxAlloca, "foreach.idx");
    llvm::Value* cond = builder->CreateICmpSLT(curIdx, lenVal, "foreach.cmp");
    auto* foreachCondBr = builder->CreateCondBr(cond, bodyBB, endBB);
    // Hint the back-edge (body) as likely-taken.
    if (optimizationLevel >= OptimizationLevel::O2) {
        llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(2000, 1);
        foreachCondBr->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
    }

    // Body: load current element into iterator variable, then execute body
    builder->SetInsertPoint(bodyBB);
    llvm::Value* bodyIdx = builder->CreateLoad(getDefaultType(), idxAlloca, "foreach.bidx");

    // Emit @llvm.assume(idx >= 0) so LLVM's CorrelatedValuePropagation and
    // SCEV passes can prove non-negativity of the foreach index.  This
    // enables srem→urem and sdiv→udiv conversions for expressions derived
    // from the index variable.  The foreach index always starts at 0 and
    // increments by 1, so the assume is unconditionally correct.
    // Skip in JIT/dynamic mode to avoid adding instruction overhead.
    if (optimizationLevel >= OptimizationLevel::O2 && !dynamicCompilation_) {
        llvm::Value* isNonNeg = builder->CreateICmpSGE(
            bodyIdx, llvm::ConstantInt::get(getDefaultType(), 0), "foreach.nonneg");
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC_STMT(
            module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {isNonNeg});
    }

    llvm::Value* elemVal;
    if (isStr) {
        // String: load single byte at offset bodyIdx, zero-extend to i64
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), basePtr, bodyIdx, "foreach.charptr");
        llvm::Value* charByte = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "foreach.char");
        elemVal = builder->CreateZExt(charByte, getDefaultType(), "foreach.charext");
    } else {
        // Array: element is at slot (bodyIdx + 1).
        // The offset is always non-negative (bodyIdx starts at 0) so nsw+nuw
        // are safe and enable SCEV to compute tight trip counts.
        // The condition (bodyIdx < lenVal) proven by the loop guard ensures
        // offset = bodyIdx+1 ≤ len, which is always within the allocated
        // region ([len+1] elements at indices 0..len), so the GEP is inbounds.
        llvm::Value* offset =
            builder->CreateAdd(bodyIdx, llvm::ConstantInt::get(getDefaultType(), 1), "foreach.offset",
                               /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), basePtr, offset, "foreach.elem.ptr");
        elemVal = builder->CreateLoad(getDefaultType(), elemPtr, "foreach.elem");
    }
    builder->CreateStore(elemVal, iterAlloca);

    loopStack.push_back({endBB, incBB});
    generateStatement(stmt->body.get());
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(incBB);
    }

    // Increment hidden index.
    // The hidden index starts at 0 and increments by 1 up to the collection
    // length, so it is always non-negative.  Setting nuw+nsw enables LLVM's
    // SCEV to compute exact trip counts and prove induction variable
    // monotonicity, which is critical for loop vectorization and unrolling.
    builder->SetInsertPoint(incBB);
    llvm::Value* nextIdx = builder->CreateLoad(getDefaultType(), idxAlloca, "foreach.nidx");
    llvm::Value* incIdx = builder->CreateAdd(nextIdx, llvm::ConstantInt::get(getDefaultType(), 1), "foreach.next",
                                             /*HasNUW=*/true, /*HasNSW=*/true);
    builder->CreateStore(incIdx, idxAlloca);
    auto* backBr = builder->CreateBr(condBB);

    // Attach loop metadata to the back-edge branch so LLVM's loop optimizer,
    // vectorizer, and unroller can identify and transform the loop.
    // This mirrors the metadata generation in generateFor/generateWhile/
    // generateDoWhile so that for-each loops receive the same optimization
    // opportunities as regular counted loops.
    if (optimizationLevel >= OptimizationLevel::O1) {
        llvm::MDNode* mustProgress =
            llvm::MDNode::get(*context, {llvm::MDString::get(*context, "llvm.loop.mustprogress")});
        llvm::SmallVector<llvm::Metadata*, 6> loopMDs;
        loopMDs.push_back(nullptr); // self-reference placeholder
        loopMDs.push_back(mustProgress);
        // At O3 with unrolling enabled, hint the unroller with a moderate
        // count for static (non-JIT) compilation.  OPTMAX functions use
        // LLVM's cost model instead.  Matches generateFor/generateWhile logic.
        if (!inOptMaxFunction && optimizationLevel >= OptimizationLevel::O3 && enableUnrollLoops_ && !dynamicCompilation_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context,
                {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                     llvm::Type::getInt32Ty(*context), 2))}));
        }
        // @vectorize / @novectorize: per-function loop vectorization hints.
        // For-each loops over arrays always iterate forward from 0 to len,
        // so at O3 the vectorizer is safe to try even without @vectorize.
        // Use @novectorize to opt out.
        if (currentFuncHintNoVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        } else if (currentFuncHintVectorize_
                   || (optimizationLevel >= OptimizationLevel::O3 && loopNestDepth_ == 0)) {
            // O3 outermost for-each: auto-enable vectorization.
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        // Interleave: @hot+@vectorize at O3 → 4; plain O3 auto-vectorized → 2.
        if (optimizationLevel >= OptimizationLevel::O3
            && !currentFuncHintNoVectorize_ && loopNestDepth_ == 0) {
            const int32_t interleave = (currentFuncHintHot_ && currentFuncHintVectorize_) ? 4 : 2;
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.interleave.count"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), interleave))}));
        }
        // Loop distribution at O3: split complex for-each bodies into
        // sub-loops with simpler dependence graphs (mirrors for-loop logic).
        if (optimizationLevel >= OptimizationLevel::O3 && loopNestDepth_ == 0) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.distribute.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        // parallel_accesses: mark for-each as having iteration-independent
        // memory accesses at O3, enabling the vectorizer and loop distribution
        // to treat array loads/stores as independent across iterations.
        if (optimizationLevel >= OptimizationLevel::O3
            && enableParallelize_ && !currentFuncHintNoParallelize_
            && loopNestDepth_ == 0
            && (currentFuncHintParallelize_ || currentFuncHintHot_
                || optimizationLevel >= OptimizationLevel::O3)) {
            llvm::MDNode* accessGroup = llvm::MDNode::getDistinct(*context, {});
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.parallel_accesses"),
                           accessGroup}));
            currentLoopAccessGroup_ = accessGroup;
        } else {
            currentLoopAccessGroup_ = nullptr;
        }
        llvm::MDNode* loopMD = llvm::MDNode::get(*context, loopMDs);
        loopMD->replaceOperandWith(0, loopMD);
        backBr->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
    }

    // End
    builder->SetInsertPoint(endBB);
}

void CodeGenerator::generateBlock(BlockStmt* stmt) {
    const ScopeGuard scope(*this);
    for (auto& statement : stmt->statements) {
        if (builder->GetInsertBlock()->getTerminator()) {
            break; // Don't generate unreachable code
        }
        generateStatement(statement.get());
    }
}

void CodeGenerator::generateExprStmt(ExprStmt* stmt) {
    generateExpression(stmt->expression.get());
}

void CodeGenerator::generateSwitch(SwitchStmt* stmt) {
    llvm::Value* condVal = generateExpression(stmt->condition.get());
    condVal = toDefaultType(condVal);

    llvm::Function* function = builder->GetInsertBlock()->getParent();

    // Constant switch condition elimination: when the condition is a known
    // compile-time constant, directly generate only the matched case body
    // (or default), skipping the switch instruction and all dead branches.
    if (auto* condConst = llvm::dyn_cast<llvm::ConstantInt>(condVal)) {
        const int64_t condIntVal = condConst->getSExtValue();

        // Validate all case values first (reject floats).
        auto validateCaseType = [&](Expression* expr) {
            llvm::Value* v = generateExpression(expr);
            if (v->getType()->isDoubleTy()) {
                codegenError("case value must be an integer constant, not a float", expr);
            }
        };
        for (auto& sc : stmt->cases) {
            if (sc.isDefault)
                continue;
            validateCaseType(sc.value.get());
            for (auto& ev : sc.values) {
                validateCaseType(ev.get());
            }
        }

        // Find the matching case.
        SwitchCase* matchedCase = nullptr;
        SwitchCase* defaultCase = nullptr;
        for (auto& sc : stmt->cases) {
            if (sc.isDefault) {
                defaultCase = &sc;
                continue;
            }
            // Check primary value
            llvm::Value* cv = generateExpression(sc.value.get());
            cv = toDefaultType(cv);
            if (auto* cvConst = llvm::dyn_cast<llvm::ConstantInt>(cv)) {
                if (cvConst->getSExtValue() == condIntVal) {
                    matchedCase = &sc;
                    break;
                }
            }
            // Check additional values (multi-value case)
            for (auto& extraVal : sc.values) {
                llvm::Value* ev = generateExpression(extraVal.get());
                ev = toDefaultType(ev);
                if (auto* evConst = llvm::dyn_cast<llvm::ConstantInt>(ev)) {
                    if (evConst->getSExtValue() == condIntVal) {
                        matchedCase = &sc;
                        break;
                    }
                }
            }
            if (matchedCase)
                break;
        }

        // Fall back to default if no case matched.
        SwitchCase* targetCase = matchedCase ? matchedCase : defaultCase;
        if (targetCase) {
            // Create a merge block for break statements.
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "switch.end", function);
            loopStack.push_back({mergeBB, nullptr});
            {
                const ScopeGuard scope(*this);
                for (auto& s : targetCase->body) {
                    generateStatement(s.get());
                    if (builder->GetInsertBlock()->getTerminator())
                        break;
                }
            }
            loopStack.pop_back();
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }
            builder->SetInsertPoint(mergeBB);
        }
        // If no case and no default, nothing to generate.
        return;
    }

    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "switch.end", function);
    llvm::BasicBlock* defaultBB = mergeBB; // fall through to merge if no default

    // Find the default case block first so the switch instruction can reference it.
    for (auto& sc : stmt->cases) {
        if (sc.isDefault) {
            defaultBB = llvm::BasicBlock::Create(*context, "switch.default", function, mergeBB);
            break;
        }
    }

    llvm::SwitchInst* switchInst = builder->CreateSwitch(condVal, defaultBB, static_cast<unsigned>(stmt->cases.size()));

    // Push a loop context so that 'break' inside a case arm exits the switch
    // (matching C/C++ semantics where break leaves the nearest switch or loop).
    loopStack.push_back({mergeBB, nullptr});

    // Track seen case values to detect duplicates.
    std::set<int64_t> seenCaseValues;

    for (auto& sc : stmt->cases) {
        if (sc.isDefault) {
            // Generate default block body.
            builder->SetInsertPoint(defaultBB);
            {
                const ScopeGuard scope(*this);
                for (auto& s : sc.body) {
                    generateStatement(s.get());
                    if (builder->GetInsertBlock()->getTerminator())
                        break;
                }
            }
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }
        } else {
            // Collect all case values (primary + additional) for multi-value case.
            std::vector<llvm::ConstantInt*> caseConstants;

            auto addCaseValue = [&](Expression* expr) {
                llvm::Value* caseVal = generateExpression(expr);
                if (caseVal->getType()->isDoubleTy()) {
                    codegenError("case value must be an integer constant, not a float", expr);
                }
                caseVal = toDefaultType(caseVal);
                auto* caseConst = llvm::dyn_cast<llvm::ConstantInt>(caseVal);
                if (!caseConst) {
                    codegenError("case value must be a compile-time integer constant", expr);
                }
                const int64_t cv = caseConst->getSExtValue();
                if (!seenCaseValues.insert(cv).second) {
                    codegenError("duplicate case value " + std::to_string(cv) + " in switch statement", expr);
                }
                caseConstants.push_back(caseConst);
            };

            // Primary value
            addCaseValue(sc.value.get());
            // Additional values (multi-value case)
            for (auto& extraVal : sc.values) {
                addCaseValue(extraVal.get());
            }

            llvm::BasicBlock* caseBB = llvm::BasicBlock::Create(*context, "switch.case", function, mergeBB);
            for (auto* caseConst : caseConstants) {
                switchInst->addCase(caseConst, caseBB);
            }

            builder->SetInsertPoint(caseBB);
            {
                const ScopeGuard scope(*this);
                for (auto& s : sc.body) {
                    generateStatement(s.get());
                    if (builder->GetInsertBlock()->getTerminator())
                        break;
                }
            }
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(mergeBB);
            }
        }
    }

    loopStack.pop_back();

    // At O2+, add branch weight metadata to the switch instruction.
    // Uniform weights tell the optimizer that all cases are equally likely,
    // which enables SimplifyCFG's switch-to-lookup-table conversion and
    // prevents the backend from biasing layout toward any particular case.
    // Without explicit weights, LLVM may assume the default case is cold
    // and misoptimize the jump table structure.
    if (optimizationLevel >= OptimizationLevel::O2) {
        const unsigned numCases = switchInst->getNumCases();
        const bool hasDefault = (defaultBB != mergeBB);
        const unsigned totalSuccessors = numCases + (hasDefault ? 1 : 0);
        if (totalSuccessors > 0) {
            llvm::SmallVector<uint32_t, 16> weights;
            // Default case weight (first weight in the metadata)
            weights.push_back(hasDefault ? 1 : 0);
            // Per-case weights: uniform distribution
            for (unsigned i = 0; i < numCases; ++i) {
                weights.push_back(1);
            }
            llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(weights);
            switchInst->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
        }
    }

    builder->SetInsertPoint(mergeBB);
}

void CodeGenerator::generateTryCatch(TryCatchStmt* stmt) {
    // Implementation uses a global error-flag/value approach combined with
    // direct-branch throw semantics:
    //
    // For a `throw` that occurs DIRECTLY inside the try block (or in any
    // function on the call stack that is itself inside a try block),
    // generateThrow() uses tryCatchStack_.back() to branch straight to the
    // catch entry block, which terminates the basic block immediately and
    // prevents any subsequent code in the try block or the throwing function
    // from executing.
    //
    // For a `throw` that occurs in a callee that was compiled WITHOUT a
    // surrounding try block (i.e. the callee was emitted when tryCatchStack_
    // was empty), generateThrow() instead emits `ret i64 0` so the function
    // returns immediately.  The try block then performs a per-statement flag
    // check and branches to catchBB if the callee set the error flag.
    //
    // Steps:
    //   1. Save old error state, clear flag.
    //   2. Create catch/restore/end blocks BEFORE generating the try body
    //      (so generateThrow can reference catchBB via tryCatchStack_).
    //   3. Push catchBB onto tryCatchStack_; generate try body with
    //      per-statement flag checks; pop tryCatchStack_.
    //   4. Catch block: bind catch variable, clear flag, generate catch body.
    //   5. restoreBB: restore old error state.  endBB: normal continuation.

    // --- Get or create global error flag / value ---
    llvm::GlobalVariable* errFlag = module->getGlobalVariable("__om_error_flag", true);
    if (!errFlag) {
        errFlag = new llvm::GlobalVariable(*module, getDefaultType(), false, llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantInt::get(getDefaultType(), 0), "__om_error_flag");
    }
    llvm::GlobalVariable* errVal = module->getGlobalVariable("__om_error_value", true);
    if (!errVal) {
        errVal = new llvm::GlobalVariable(*module, getDefaultType(), false, llvm::GlobalValue::InternalLinkage,
                                          llvm::ConstantInt::get(getDefaultType(), 0), "__om_error_value");
    }

    llvm::Function* function = builder->GetInsertBlock()->getParent();

    // --- Save and clear old error state ---
    llvm::Value* oldFlag = builder->CreateLoad(getDefaultType(), errFlag, "try.oldflag");
    llvm::Value* oldVal = builder->CreateLoad(getDefaultType(), errVal, "try.oldval");
    builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), errFlag);

    // --- Create all blocks upfront so generateThrow() can reference catchBB ---
    llvm::BasicBlock* catchBB = llvm::BasicBlock::Create(*context, "catch.body", function);
    llvm::BasicBlock* restoreBB = llvm::BasicBlock::Create(*context, "try.restore", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "try.end", function);

    // --- Generate try body ---
    // Push catchBB so that any `throw` emitted during code generation of this
    // try block (or inside a callee that is currently being inlined/compiled
    // within this scope) branches directly here.
    tryCatchStack_.push_back(catchBB);
    {
        const ScopeGuard scope(*this);
        for (auto& s : stmt->tryBlock->statements) {
            // Stop if the current block already has a terminator (e.g. a direct
            // `throw` branched to catchBB or a `return`).
            if (builder->GetInsertBlock()->getTerminator())
                break;

            generateStatement(s.get());

            // After each statement that didn't terminate the block, check
            // whether a *called function* propagated an error via the flag.
            // Callees compiled without a surrounding try block use `ret i64 0`
            // + flag to propagate throws; we must detect that here.
            //
            // Performance note: this adds a load + compare + conditional branch
            // per try-block statement.  The branch is strongly predicted not-taken
            // because the cold weight is implied by LLVM's default branch predictor.
            // O3 can often hoist or eliminate the check when the statement is a
            // simple arithmetic expression with no calls (dead-store elimination of
            // the flag store + load pair).  The cost is acceptable because try blocks
            // are rare in tight loops; try/catch is an error-handling construct.
            if (!builder->GetInsertBlock()->getTerminator()) {
                llvm::Value* flagNow = builder->CreateLoad(getDefaultType(), errFlag, "try.chk");
                llvm::Value* flagSet =
                    builder->CreateICmpNE(flagNow, llvm::ConstantInt::get(getDefaultType(), 0), "try.flagset");
                llvm::BasicBlock* contBB = llvm::BasicBlock::Create(*context, "try.cont", function);
                builder->CreateCondBr(flagSet, catchBB, contBB);
                builder->SetInsertPoint(contBB);
            }
        }
    }
    tryCatchStack_.pop_back();

    // Fall through from the try body to the restore path (no error thrown).
    if (!builder->GetInsertBlock()->getTerminator())
        builder->CreateBr(restoreBB);

    // --- Catch block ---
    builder->SetInsertPoint(catchBB);
    {
        const ScopeGuard scope(*this);
        llvm::Value* errValLoaded = builder->CreateLoad(getDefaultType(), errVal, "catch.errval");
        llvm::AllocaInst* catchVar = createEntryBlockAlloca(function, stmt->catchVar);
        builder->CreateStore(errValLoaded, catchVar);
        bindVariable(stmt->catchVar, catchVar);

        // Clear error state so the catch body executes normally.
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), errFlag);
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), errVal);

        for (auto& s : stmt->catchBlock->statements) {
            if (builder->GetInsertBlock()->getTerminator())
                break;
            generateStatement(s.get());
        }
    }
    if (!builder->GetInsertBlock()->getTerminator())
        builder->CreateBr(restoreBB);

    // --- Restore old error state ---
    builder->SetInsertPoint(restoreBB);
    builder->CreateStore(oldFlag, errFlag);
    builder->CreateStore(oldVal, errVal);
    builder->CreateBr(endBB);

    // --- Continuation ---
    builder->SetInsertPoint(endBB);
}

void CodeGenerator::generateThrow(ThrowStmt* stmt) {
    // Get or create global error flag / value.
    llvm::GlobalVariable* errFlag = module->getGlobalVariable("__om_error_flag", true);
    if (!errFlag) {
        errFlag = new llvm::GlobalVariable(*module, getDefaultType(), false, llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantInt::get(getDefaultType(), 0), "__om_error_flag");
    }
    llvm::GlobalVariable* errVal = module->getGlobalVariable("__om_error_value", true);
    if (!errVal) {
        errVal = new llvm::GlobalVariable(*module, getDefaultType(), false, llvm::GlobalValue::InternalLinkage,
                                          llvm::ConstantInt::get(getDefaultType(), 0), "__om_error_value");
    }

    llvm::Value* val = generateExpression(stmt->value.get());
    val = toDefaultType(val);

    // Set the error flag and store the thrown value.
    builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 1), errFlag);
    builder->CreateStore(val, errVal);

    // Terminate the basic block so that no code after `throw` executes.
    //
    // Two cases:
    //   (a) We are currently generating code INSIDE a try block:
    //       tryCatchStack_.back() is the catch-entry block for the innermost
    //       surrounding try.  Branch there directly — this is the fast path
    //       that does not require the per-statement flag checks in the caller.
    //
    //   (b) We are NOT inside any try block (or we are inside a called function
    //       that was compiled without a surrounding try):
    //       Return 0 from the current function so the caller can detect the
    //       thrown error via the global flag.
    if (!tryCatchStack_.empty()) {
        builder->CreateBr(tryCatchStack_.back());
    } else {
        builder->CreateRet(llvm::ConstantInt::get(getDefaultType(), 0));
    }
}

// ---------------------------------------------------------------------------
// Ownership system codegen
// ---------------------------------------------------------------------------

void CodeGenerator::generateInvalidate(InvalidateStmt* stmt) {
    // Detect use-after-invalidate: the variable has already been
    // invalidated or moved, so re-invalidating is a logic error.
    if (deadVars_.count(stmt->varName)) {
        auto reason = deadVarReason_.find(stmt->varName);
        const std::string hint = (reason != deadVarReason_.end()) ? reason->second : "invalidated";
        codegenError("Variable '" + stmt->varName + "' has already been " + hint, stmt);
    }

    // Look up the variable's alloca
    auto it = namedValues.find(stmt->varName);
    if (it == namedValues.end()) {
        codegenError("Variable '" + stmt->varName + "' not found for invalidate", stmt);
    }
    llvm::Value* alloca = it->second;

    // Emit llvm.lifetime.end to mark the variable as dead.
    // This allows LLVM to reuse the stack slot and eliminate dead stores.
    auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(alloca);
    if (allocaInst) {
        auto* allocaTy = allocaInst->getAllocatedType();
        const uint64_t size = module->getDataLayout().getTypeAllocSize(allocaTy);
        auto* sizeVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), size);
        auto* lifetimeEnd = OMSC_GET_INTRINSIC_STMT(
            module.get(), llvm::Intrinsic::lifetime_end,
            {llvm::PointerType::getUnqual(*context)});
        builder->CreateCall(lifetimeEnd, {sizeVal, allocaInst});
    }

    // Store an undef/poison value to enable dead-store elimination.
    auto* allocaType = llvm::cast<llvm::AllocaInst>(alloca)->getAllocatedType();
    builder->CreateStore(llvm::UndefValue::get(allocaType), alloca);

    // Mark the variable as dead for use-after-invalidate detection.
    deadVars_.insert(stmt->varName);
    deadVarReason_[stmt->varName] = "invalidated";
    varOwnership_[stmt->varName] = OwnershipState::Invalidated;
}

void CodeGenerator::generateMoveDecl(MoveDecl* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        codegenError("Move declaration outside of function", stmt);
    }

    llvm::Value* initValue = nullptr;
    llvm::Type* allocaType = stmt->typeName.empty()
                                 ? getDefaultType()
                                 : resolveAnnotatedType(stmt->typeName);

    if (stmt->initializer) {
        initValue = generateExpression(stmt->initializer.get());
        if (stmt->typeName.empty())
            allocaType = initValue->getType();
        if (!stmt->typeName.empty())
            initValue = convertTo(initValue, allocaType);
    }

    llvm::AllocaInst* alloca = createEntryBlockAlloca(function, stmt->name, allocaType);
    bindVariable(stmt->name, alloca);

    if (initValue) {
        builder->CreateStore(initValue, alloca);

        // If the source is an identifier, mark the source as dead (emit
        // lifetime.end + store undef) to enable LLVM to elide the copy
        // and reuse the register/stack slot.
        if (stmt->initializer->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto* srcId = static_cast<IdentifierExpr*>(stmt->initializer.get());
            markVariableMoved(srcId->name);
        }
    } else {
        if (allocaType->isDoubleTy())
            builder->CreateStore(llvm::ConstantFP::get(allocaType, 0.0), alloca);
        else
            builder->CreateStore(llvm::ConstantInt::get(*context, llvm::APInt(64, 0)), alloca);
    }
}

llvm::Value* CodeGenerator::generateMoveExpr(MoveExpr* expr) {
    // Generate the source value.
    llvm::Value* val = generateExpression(expr->source.get());

    // If the source is an identifier, mark it dead after the move.
    if (expr->source->type == ASTNodeType::IDENTIFIER_EXPR) {
        auto* srcId = static_cast<IdentifierExpr*>(expr->source.get());
        markVariableMoved(srcId->name);
    }

    return val;
}

llvm::Value* CodeGenerator::generateBorrowExpr(BorrowExpr* expr) {
    // Generate the source value.
    llvm::Value* val = generateExpression(expr->source.get());

    // Track the source variable as borrowed in the ownership lattice.
    // Borrowed variables cannot be mutated, moved, or invalidated until
    // the borrow ends.
    if (auto* srcIdent = dynamic_cast<IdentifierExpr*>(expr->source.get())) {
        markVariableBorrowed(srcIdent->name);
    }

    // Attach !alias.scope and !noalias metadata to the load instruction.
    // The alias.scope declares which alias domain this access belongs to,
    // and noalias declares which domains it is guaranteed not to alias with.
    // Together, they tell LLVM that a borrowed pointer does not alias other
    // pointers in the function — this is safe because OmScript's ownership
    // model guarantees that borrows are unique read-only references and
    // no mutable alias can exist concurrently.
    //
    // LLVM's scoped noalias metadata structure:
    //   !domain = distinct !{!domain, !"omscript.borrow.domain"}
    //   !scope  = distinct !{!scope, !domain, !"omscript.borrow.<name>"}
    //   !alias.scope = !{!scope}    — "this access is in this scope"
    //   !noalias     = !{!scope}    — "this access does NOT alias this scope"
    //
    // Setting BOTH alias.scope and noalias on the borrow load enables LLVM's
    // ScopedNoAliasAA to prove that loads through the borrow don't alias
    // stores through other pointers, enabling vectorization, LICM, and
    // load/store reordering across borrow boundaries.
    if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(val)) {
        std::string scopeName = "omscript.borrow";
        if (auto* srcIdent = dynamic_cast<IdentifierExpr*>(expr->source.get())) {
            scopeName += "." + srcIdent->name;
        }

        llvm::MDNode* domain = llvm::MDNode::getDistinct(
            *context, {llvm::MDString::get(*context, "omscript.borrow.domain")});
        domain->replaceOperandWith(0, domain);

        llvm::MDNode* scope = llvm::MDNode::getDistinct(
            *context, {nullptr, domain, llvm::MDString::get(*context, scopeName)});
        scope->replaceOperandWith(0, scope);

        llvm::MDNode* scopeList = llvm::MDNode::get(*context, {scope});
        // alias.scope: declares this access belongs to the borrow scope
        loadInst->setMetadata(llvm::LLVMContext::MD_alias_scope, scopeList);
        // noalias: declares this access does not alias accesses in this scope
        // (other borrows get their own distinct scopes)
        loadInst->setMetadata(llvm::LLVMContext::MD_noalias, scopeList);
    }

    return val;
}

void CodeGenerator::generatePrefetch(PrefetchStmt* stmt) {
    llvm::Value* alloca = nullptr;
    std::string varName;

    if (stmt->varDecl) {
        // Prefetch with variable declaration: generate the VarDecl first.
        generateVarDecl(stmt->varDecl.get());
        varName = stmt->varDecl->name;
        prefetchedVars_.insert(varName);
        auto it = namedValues.find(varName);
        if (it != namedValues.end()) {
            alloca = it->second;
        }
    } else {
        // Standalone prefetch of an existing variable.
        varName = stmt->varName;
        prefetchedVars_.insert(varName);
        auto it = namedValues.find(varName);
        if (it != namedValues.end()) {
            alloca = it->second;
        } else {
            codegenError("Unknown variable '" + varName + "' in prefetch statement", stmt);
        }
    }

    if (alloca) {
        // If hintImmut is set, mark the variable's loads as invariant so LLVM
        // knows the value never changes and can hoist/CSE aggressively.
        if (stmt->hintImmut) {
            prefetchedImmutVars_.insert(varName);
        }

        auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(alloca);
        if (allocaInst) {
            llvm::Type* elemTy = allocaInst->getAllocatedType();
            const int32_t locality = stmt->hintHot ? 3 : 2;

            // Determine if this is a "large" type that cannot fit in a
            // register (e.g. arrays, structs stored as multi-slot allocas).
            // Large types are identified by:
            //   - ArrayType allocas (struct instances stored as i64 arrays)
            //   - StructType allocas
            //   - Allocas with array size > 1
            bool isLargeType = elemTy->isArrayTy() || elemTy->isStructTy();
            if (auto* arraySize = allocaInst->getArraySize()) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(arraySize)) {
                    if (ci->getZExtValue() > 1) isLargeType = true;
                }
            }

            if (isLargeType) {
                // Memory-resident prefetch: for large types (structs, arrays)
                // that cannot fit in registers, emit llvm.prefetch directly
                // on the alloca pointer.  This brings the variable's memory
                // into CPU cache and keeps it hot for the duration of the
                // scope.  The prefetch is tagged with !omscript.memory_prefetch
                // metadata so the post-optimization cleanup preserves it
                // (Rule 2 normally removes alloca-targeted prefetches).
                llvm::Function* prefetchFn = OMSC_GET_INTRINSIC_STMT(
                    module.get(), llvm::Intrinsic::prefetch,
                    {llvm::PointerType::getUnqual(*context)});
                auto* pfCall = builder->CreateCall(prefetchFn, {
                    alloca,
                    builder->getInt32(0),          // read prefetch
                    builder->getInt32(locality),   // temporal locality
                    builder->getInt32(1)           // data cache
                });
                // Tag with metadata so the cleanup pass preserves this.
                pfCall->setMetadata("omscript.memory_prefetch",
                    llvm::MDNode::get(*context, {}));

                // For multi-slot arrays (structs), also prefetch subsequent
                // cache lines to cover the full object.  Each cache line is
                // typically 64 bytes = 8 × i64 slots.
                if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(elemTy)) {
                    const uint64_t numSlots = arrTy->getNumElements();
                    const uint64_t slotSize = module->getDataLayout().getTypeAllocSize(
                        arrTy->getElementType());
                    const uint64_t totalBytes = numSlots * slotSize;
                    // Prefetch every 64 bytes (one cache line) beyond the first.
                    for (uint64_t offset = 64; offset < totalBytes; offset += 64) {
                        llvm::Value* gepIdx = llvm::ConstantInt::get(
                            getDefaultType(), offset / slotSize);
                        llvm::Value* nextPtr = builder->CreateGEP(
                            arrTy->getElementType(), alloca, gepIdx,
                            varName + ".pf.cacheline");
                        auto* pfCall2 = builder->CreateCall(prefetchFn, {
                            nextPtr,
                            builder->getInt32(0),
                            builder->getInt32(locality),
                            builder->getInt32(1)
                        });
                        pfCall2->setMetadata("omscript.memory_prefetch",
                            llvm::MDNode::get(*context, {}));
                    }
                }
            } else {
                // Register promotion strategy: do NOT emit llvm.prefetch on
                // the alloca itself — that would anchor the variable to memory
                // and prevent SROA/mem2reg from promoting it to an SSA register.
                // Instead, the variable goes straight to a register and stays
                // there until explicitly invalidated.
                //
                // For variables that hold pointer-sized values (i64/ptr), emit
                // a prefetch on the *pointed-to* memory — the variable's VALUE
                // is treated as a memory address.
                if (elemTy->isIntegerTy(64) || elemTy->isPointerTy()) {
                    llvm::Function* prefetchFn = OMSC_GET_INTRINSIC_STMT(
                        module.get(), llvm::Intrinsic::prefetch,
                        {llvm::PointerType::getUnqual(*context)});
                    llvm::Value* val = builder->CreateLoad(elemTy, alloca,
                                                            varName + ".pf.val");
                    llvm::Value* ptr = val;
                    if (!elemTy->isPointerTy()) {
                        ptr = builder->CreateIntToPtr(
                            val, llvm::PointerType::getUnqual(*context),
                            varName + ".pf.ptr");
                    }
                    builder->CreateCall(prefetchFn, {
                        ptr,
                        builder->getInt32(0),          // read prefetch
                        builder->getInt32(locality),   // temporal locality
                        builder->getInt32(1)           // data cache
                    });
                }
            }
        }
    }
}

void CodeGenerator::markVariableMoved(const std::string& varName) {
    auto srcIt = namedValues.find(varName);
    if (srcIt != namedValues.end()) {
        auto* srcAlloca = llvm::dyn_cast<llvm::AllocaInst>(srcIt->second);
        if (srcAlloca) {
            auto* srcTy = srcAlloca->getAllocatedType();
            const uint64_t sz = module->getDataLayout().getTypeAllocSize(srcTy);
            auto* szVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), sz);
#if LLVM_VERSION_MAJOR >= 19
            auto* lifetimeEnd = llvm::Intrinsic::getOrInsertDeclaration(
#else
            auto* lifetimeEnd = llvm::Intrinsic::getDeclaration(
#endif
                module.get(), llvm::Intrinsic::lifetime_end,
                {llvm::PointerType::getUnqual(*context)});
            builder->CreateCall(lifetimeEnd, {szVal, srcAlloca});
            builder->CreateStore(llvm::UndefValue::get(srcTy), srcAlloca);
        }
    }
    deadVars_.insert(varName);
    deadVarReason_[varName] = "moved";
    varOwnership_[varName] = OwnershipState::Moved;
}

void CodeGenerator::markVariableBorrowed(const std::string& varName) {
    borrowedVars_.insert(varName);
    varOwnership_[varName] = OwnershipState::Borrowed;
}

bool CodeGenerator::isVariableBorrowed(const std::string& varName) const {
    return borrowedVars_.count(varName) > 0;
}

OwnershipState CodeGenerator::getOwnershipState(const std::string& varName) const {
    auto it = varOwnership_.find(varName);
    return (it != varOwnership_.end()) ? it->second : OwnershipState::Owned;
}

} // namespace omscript
