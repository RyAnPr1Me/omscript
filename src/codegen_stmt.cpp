#include "codegen.h"
#include "diagnostic.h"
#include <iostream>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/MDBuilder.h>
#include <set>
#include <stdexcept>

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

    if (stmt->initializer) {
        initValue = generateExpression(stmt->initializer.get());
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
        if (!stmt->typeName.empty())
            initValue = convertTo(initValue, allocaType);
    }

    llvm::AllocaInst* alloca = createEntryBlockAlloca(function, stmt->name, allocaType);
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
        if (allocaType->isDoubleTy())
            builder->CreateStore(llvm::ConstantFP::get(allocaType, 0.0), alloca);
        else
            builder->CreateStore(llvm::ConstantInt::get(*context, llvm::APInt(64, 0)), alloca);
    }
}

void CodeGenerator::generateReturn(ReturnStmt* stmt) {
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
        builder->CreateRet(retValue);
    } else {
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

    if (elseBB) {
        builder->CreateCondBr(condBool, thenBB, elseBB);
    } else {
        builder->CreateCondBr(condBool, thenBB, mergeBB);
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

    ScopeGuard scope(*this);

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
    builder->CreateCondBr(condBool, bodyBB, endBB);


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
        llvm::MDNode* loopMD = llvm::MDNode::get(*context, loopMDs);
        loopMD->replaceOperandWith(0, loopMD);
        backBrWhile->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
    }

    // End block
    builder->SetInsertPoint(endBB);
}

void CodeGenerator::generateDoWhile(DoWhileStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();

    ScopeGuard scope(*this);

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
    // Attach loop metadata to the do-while back-edge, matching for-loop and
    // while-loop hints for consistent vectorization across all loop forms.
    {
        llvm::MDNode* mustProgress =
            llvm::MDNode::get(*context, {llvm::MDString::get(*context, "llvm.loop.mustprogress")});
        llvm::SmallVector<llvm::Metadata*, 4> loopMDs;
        loopMDs.push_back(nullptr);
        loopMDs.push_back(mustProgress);
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

    ScopeGuard scope(*this);

    // Allocate iterator variable
    llvm::AllocaInst* iterAlloca = createEntryBlockAlloca(function, stmt->iteratorVar);
    bindVariable(stmt->iteratorVar, iterAlloca);

    // Initialize iterator
    llvm::Value* startVal = generateExpression(stmt->start.get());
    // For-loop iterator is always integer, convert to integer
    startVal = toDefaultType(startVal);
    builder->CreateStore(startVal, iterAlloca);

    // Get end value
    llvm::Value* endVal = generateExpression(stmt->end.get());
    // Convert to integer since loop bounds are always integer
    endVal = toDefaultType(endVal);

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
        // Convert to integer since loop step is always integer
        stepVal = toDefaultType(stepVal);
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
        if (ascending) {
            stepVal = llvm::ConstantInt::get(*context, llvm::APInt(64, 1));
            stepKnownPositive = true;
            stepKnownNonZero = true;
        } else {
            llvm::Value* isDesc = builder->CreateICmpSGT(startVal, endVal, "for.isdesc");
            llvm::Value* posOne = llvm::ConstantInt::get(*context, llvm::APInt(64, 1));
            llvm::Value* negOne = llvm::ConstantInt::get(*context, llvm::APInt(64, static_cast<uint64_t>(-1), true));
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
        std::string errorMessage = "Runtime error: for-loop step cannot be zero for iterator '" + stmt->iteratorVar + "'\n";
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
    llvm::Value* curVal = builder->CreateLoad(getDefaultType(), iterAlloca, stmt->iteratorVar.c_str());
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
    builder->CreateCondBr(continueCond, bodyBB, endBB);

    // Body block
    builder->SetInsertPoint(bodyBB);
    loopStack.push_back({endBB, incBB});
    generateStatement(stmt->body.get());
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(incBB);
    }

    // Increment block
    builder->SetInsertPoint(incBB);
    llvm::Value* nextVal = builder->CreateLoad(getDefaultType(), iterAlloca, stmt->iteratorVar.c_str());
    llvm::Value* incVal = builder->CreateNSWAdd(nextVal, stepVal, "nextvar");
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
        if (optimizationLevel >= OptimizationLevel::O3 && enableUnrollLoops_) {
            if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
                if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endVal)) {
                    int64_t startV = startCI->getSExtValue();
                    int64_t endV = endCI->getSExtValue();
                    // Compute trip count safely using unsigned subtraction to
                    // avoid signed overflow when start and end are far apart.
                    uint64_t tripCount = (endV >= startV)
                        ? static_cast<uint64_t>(endV) - static_cast<uint64_t>(startV)
                        : static_cast<uint64_t>(startV) - static_cast<uint64_t>(endV);
                    if (tripCount > 0 && tripCount <= 64) {
                        llvm::MDNode* unrollCount = llvm::MDNode::get(
                            *context,
                            {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                             llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                                 llvm::Type::getInt32Ty(*context), static_cast<uint32_t>(tripCount)))});
                        loopMDs.push_back(unrollCount);
                    }
                }
            }
        }
        llvm::MDNode* loopMD = llvm::MDNode::get(*context, loopMDs);
        loopMD->replaceOperandWith(0, loopMD);
        backBr->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
    }

    // End block
    builder->SetInsertPoint(endBB);
}

void CodeGenerator::generateForEach(ForEachStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        codegenError("For-each loop outside of function", stmt);
    }

    ScopeGuard scope(*this);

    // Evaluate the collection (array or string)
    llvm::Value* collVal = generateExpression(stmt->collection.get());

    // Detect whether the collection is a string.  Strings are raw char
    // pointers without a length header; arrays use [length, e0, e1, ...].
    bool isStr = collVal->getType()->isPointerTy() || isStringExpr(stmt->collection.get());
    // Detect whether this is an array whose elements are string pointers.
    bool isStrArray = !isStr && isStringArrayExpr(stmt->collection.get());

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
    builder->CreateCondBr(cond, bodyBB, endBB);

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
    ScopeGuard scope(*this);
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
                ScopeGuard scope(*this);
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
            llvm::Value* caseVal = generateExpression(sc.value.get());
            if (caseVal->getType()->isDoubleTy()) {
                codegenError("case value must be an integer constant, not a float", sc.value.get());
            }
            caseVal = toDefaultType(caseVal);
            auto* caseConst = llvm::dyn_cast<llvm::ConstantInt>(caseVal);
            if (!caseConst) {
                codegenError("case value must be a compile-time integer constant", sc.value.get());
            }

            int64_t cv = caseConst->getSExtValue();
            if (!seenCaseValues.insert(cv).second) {
                codegenError("duplicate case value " + std::to_string(cv) + " in switch statement", sc.value.get());
            }

            llvm::BasicBlock* caseBB = llvm::BasicBlock::Create(*context, "switch.case", function, mergeBB);
            switchInst->addCase(caseConst, caseBB);

            builder->SetInsertPoint(caseBB);
            {
                ScopeGuard scope(*this);
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
        ScopeGuard scope(*this);
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
        ScopeGuard scope(*this);
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
        uint64_t size = module->getDataLayout().getTypeAllocSize(allocaTy);
        auto* sizeVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), size);
        auto* lifetimeEnd = llvm::Intrinsic::getDeclaration(
            module.get(), llvm::Intrinsic::lifetime_end,
            {llvm::PointerType::getUnqual(*context)});
        builder->CreateCall(lifetimeEnd, {sizeVal, allocaInst});
    }

    // Store an undef/poison value to enable dead-store elimination.
    auto* allocaType = llvm::cast<llvm::AllocaInst>(alloca)->getAllocatedType();
    builder->CreateStore(llvm::UndefValue::get(allocaType), alloca);
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
            auto srcIt = namedValues.find(srcId->name);
            if (srcIt != namedValues.end()) {
                auto* srcAlloca = llvm::dyn_cast<llvm::AllocaInst>(srcIt->second);
                if (srcAlloca) {
                    auto* srcTy = srcAlloca->getAllocatedType();
                    uint64_t sz = module->getDataLayout().getTypeAllocSize(srcTy);
                    auto* szVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), sz);
                    auto* lifetimeEnd = llvm::Intrinsic::getDeclaration(
                        module.get(), llvm::Intrinsic::lifetime_end,
                        {llvm::PointerType::getUnqual(*context)});
                    builder->CreateCall(lifetimeEnd, {szVal, srcAlloca});
                    builder->CreateStore(llvm::UndefValue::get(srcTy), srcAlloca);
                }
            }
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
        auto srcIt = namedValues.find(srcId->name);
        if (srcIt != namedValues.end()) {
            auto* srcAlloca = llvm::dyn_cast<llvm::AllocaInst>(srcIt->second);
            if (srcAlloca) {
                auto* srcTy = srcAlloca->getAllocatedType();
                uint64_t sz = module->getDataLayout().getTypeAllocSize(srcTy);
                auto* szVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), sz);
                auto* lifetimeEnd = llvm::Intrinsic::getDeclaration(
                    module.get(), llvm::Intrinsic::lifetime_end,
                    {llvm::PointerType::getUnqual(*context)});
                builder->CreateCall(lifetimeEnd, {szVal, srcAlloca});
                builder->CreateStore(llvm::UndefValue::get(srcTy), srcAlloca);
            }
        }
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

} // namespace omscript
