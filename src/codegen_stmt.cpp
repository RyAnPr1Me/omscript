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
                stringReturningFunctions_.insert(std::string(builder->GetInsertBlock()->getParent()->getName()));
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
    }

    // Then block
    builder->SetInsertPoint(thenBB);
    generateStatement(stmt->thenBranch.get());
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(mergeBB);
    }

    // Else block
    if (elseBB) {
        builder->SetInsertPoint(elseBB);
        generateStatement(stmt->elseBranch.get());
        if (!builder->GetInsertBlock()->getTerminator()) {
            builder->CreateBr(mergeBB);
        }
    }

    // Merge block
    builder->SetInsertPoint(mergeBB);
}

void CodeGenerator::generateWhile(WhileStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();

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
    generateStatement(stmt->body.get());
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        auto* backBrWhile = builder->CreateBr(condBB);
        // Attach loop metadata to the while-loop back-edge, matching for-loop
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
        // Unroll hint: match for-loop unrolling strategy.
        // For OPTMAX functions, let LLVM's cost model choose; for regular
        // functions, cap at 2 to prevent code bloat.
        if (!inOptMaxFunction && optimizationLevel >= OptimizationLevel::O3 && enableUnrollLoops_ && !dynamicCompilation_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context,
                {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                     llvm::Type::getInt32Ty(*context), 2))}));
        }
        // @vectorize / @novectorize: per-function loop vectorization overrides.
        if (currentFuncHintNoVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        } else if (currentFuncHintVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        llvm::MDNode* loopMD = llvm::MDNode::get(*context, loopMDs);
        loopMD->replaceOperandWith(0, loopMD);
        backBrWhile->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
    }

    // End block
    builder->SetInsertPoint(endBB);
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
        // @vectorize / @novectorize: per-function loop vectorization overrides.
        if (currentFuncHintNoVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        } else if (currentFuncHintVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
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
            } else if (startCI->getSExtValue() == 0) {
                // Common pattern: for (i in 0...n).  Step is always +1;
                // when n <= 0 the loop condition (i < n) fails immediately.
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
        // Fast path: known ascending loop, just compare i < end.
        continueCond = builder->CreateICmpSLT(curVal, endVal, "forcond_lt");
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
            llvm::Value* isNonNeg = builder->CreateICmpSGE(
                iterVal, llvm::ConstantInt::get(iterType, 0), "iter.nonneg");
            llvm::Function* assumeFn = OMSC_GET_INTRINSIC_STMT(
                module.get(), llvm::Intrinsic::assume, {});
            builder->CreateCall(assumeFn, {isNonNeg});
        }
    }

    loopStack.push_back({endBB, incBB});
    generateStatement(stmt->body.get());
    loopStack.pop_back();
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
                        llvm::MDNode* unrollCount = llvm::MDNode::get(
                            *context,
                            {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                             llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                                 llvm::Type::getInt32Ty(*context), static_cast<uint32_t>(tripCount)))});
                        loopMDs.push_back(unrollCount);
                        addedUnrollHint = true;
                    }
                }
            }
        }
        // For variable-trip-count for-loops at O3, cap the unroll factor
        // to prevent LLVM's runtime unroller from over-unrolling.
        // LLVM's O3 unroller can create 50-100 copies of the loop body for
        // loops with expensive operations (urem, division), causing massive
        // register pressure, stack spills, and I-cache misses.  OmScript
        // knows the loop structure (ascending, step=+1) and can guide the
        // unroller more precisely than C compilers.
        //
        // For OPTMAX functions, we omit the unroll hint entirely, allowing
        // LLVM's cost-model-driven unroller to choose the optimal factor
        // based on loop body complexity and register pressure.  The OPTMAX
        // per-function pipeline already includes an aggressive unroll pass.
        //
        // For regular functions, a factor of 2 keeps the code within L1
        // I-cache (2^3=8x worst case) while still amortizing loop overhead.
        if (!addedUnrollHint && !inOptMaxFunction && optimizationLevel >= OptimizationLevel::O3 && enableUnrollLoops_ && !dynamicCompilation_) {
            llvm::MDNode* unrollCount = llvm::MDNode::get(
                *context,
                {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                     llvm::Type::getInt32Ty(*context), 2))});
            loopMDs.push_back(unrollCount);
        }
        // @unroll / @nounroll: per-function loop unrolling overrides.
        // @nounroll disables unrolling entirely; @unroll requests aggressive unrolling.
        if (currentFuncHintNoUnroll_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.unroll.disable")}));
        } else if (currentFuncHintUnroll_ && !addedUnrollHint) {
            // For variable-trip-count loops, unroll.full is ignored by LLVM
            // (the unroller refuses to fully unroll unbounded loops).  Use
            // unroll.count=8 instead, which gives LLVM a concrete target and
            // matches GCC's aggressive unrolling behavior for FP-heavy loops.
            // For OPTMAX functions, this enables 4-8x loop body replication
            // that hides exp2/sqrt latency through instruction-level parallelism.
            loopMDs.push_back(llvm::MDNode::get(
                *context,
                {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                     llvm::Type::getInt32Ty(*context), 8))}));
        }
        // @vectorize / @novectorize: per-function loop vectorization overrides.
        if (currentFuncHintNoVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        } else if (currentFuncHintVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        llvm::MDNode* loopMD = llvm::MDNode::get(*context, loopMDs);
        loopMD->replaceOperandWith(0, loopMD);
        backBr->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
    }

    // End block
    builder->SetInsertPoint(endBB);
    --loopNestDepth_;
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
        lenVal = builder->CreateLoad(getDefaultType(), basePtr, "foreach.len");
    }

    // Allocate hidden index variable and the user's iterator variable
    llvm::AllocaInst* idxAlloca = createEntryBlockAlloca(function, "_foreach_idx");
    builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), idxAlloca);

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
    llvm::Value* elemVal;
    if (isStr) {
        // String: load single byte at offset bodyIdx, zero-extend to i64
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), basePtr, bodyIdx, "foreach.charptr");
        llvm::Value* charByte = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "foreach.char");
        elemVal = builder->CreateZExt(charByte, getDefaultType(), "foreach.charext");
    } else {
        // Array: element is at slot (bodyIdx + 1)
        llvm::Value* offset =
            builder->CreateAdd(bodyIdx, llvm::ConstantInt::get(getDefaultType(), 1), "foreach.offset");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), basePtr, offset, "foreach.elem.ptr");
        elemVal = builder->CreateLoad(getDefaultType(), elemPtr, "foreach.elem");
    }
    builder->CreateStore(elemVal, iterAlloca);

    loopStack.push_back({endBB, incBB});
    generateStatement(stmt->body.get());
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(incBB);
    }

    // Increment hidden index
    builder->SetInsertPoint(incBB);
    llvm::Value* nextIdx = builder->CreateLoad(getDefaultType(), idxAlloca, "foreach.nidx");
    llvm::Value* incIdx = builder->CreateAdd(nextIdx, llvm::ConstantInt::get(getDefaultType(), 1), "foreach.next");
    builder->CreateStore(incIdx, idxAlloca);
    builder->CreateBr(condBB);

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

    // If the source is an identifier, attach !noalias metadata to the load
    // as a hint that this borrow does not alias other pointers.
    // The noalias.scope metadata requires a valid scope domain structure:
    //   !scope = !{!scope, !domain, !"name"}
    //   !domain = !{!domain, !"domain_name"}
    if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(val)) {
        llvm::MDNode* domain = llvm::MDNode::getDistinct(
            *context, {llvm::MDString::get(*context, "omscript.borrow.domain")});
        // Patch self-ref: domain = !{domain, !"..."}
        domain->replaceOperandWith(0, domain);
        llvm::MDNode* scope = llvm::MDNode::getDistinct(
            *context, {nullptr, domain, llvm::MDString::get(*context, "omscript.borrow.scope")});
        scope->replaceOperandWith(0, scope);
        llvm::MDNode* scopeList = llvm::MDNode::get(*context, {scope});
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
}

} // namespace omscript
