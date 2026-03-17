#include "codegen.h"
#include "diagnostic.h"
#include <cstdlib>
#include <iostream>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Verifier.h>
#include <optional>
#include <set>
#include <stdexcept>

// LLVM 21 removed Attribute::NoCapture in favour of the captures(...) attribute.
#if LLVM_VERSION_MAJOR >= 21
#define OMSC_ADD_NOCAPTURE(fn, idx)                                                                                    \
    (fn)->addParamAttr((idx), llvm::Attribute::getWithCaptureInfo((fn)->getContext(), llvm::CaptureInfo::none()))
#else
#define OMSC_ADD_NOCAPTURE(fn, idx) (fn)->addParamAttr((idx), llvm::Attribute::NoCapture)
#endif

// LLVM 19 introduced getOrInsertDeclaration; older versions only have getDeclaration.
#if LLVM_VERSION_MAJOR >= 19
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getOrInsertDeclaration
#else
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getDeclaration
#endif

namespace omscript {

llvm::Value* CodeGenerator::generateCall(CallExpr* expr) {
    // All stdlib built-in functions are compiled to native machine code below.
    if (expr->callee == "print") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'print' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        Expression* argExpr = expr->arguments[0].get();
        llvm::Value* arg = generateExpression(argExpr);
        if (arg->getType()->isDoubleTy()) {
            // Print float value
            llvm::GlobalVariable* floatFmt = module->getGlobalVariable("print_float_fmt", true);
            if (!floatFmt) {
                floatFmt = builder->CreateGlobalString("%g\n", "print_float_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {floatFmt, arg});
            return llvm::ConstantInt::get(getDefaultType(), 0);
        } else if (arg->getType()->isPointerTy() || isStringExpr(argExpr)) {
            // Print string (literal, local string variable, or i64 holding a
            // string pointer that crossed a function boundary via ptrtoint).
            if (!arg->getType()->isPointerTy()) {
                arg = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "print.str.ptr");
            }
            llvm::GlobalVariable* strFmt = module->getGlobalVariable("print_str_fmt", true);
            if (!strFmt) {
                strFmt = builder->CreateGlobalString("%s\n", "print_str_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {strFmt, arg});
            return llvm::ConstantInt::get(getDefaultType(), 0);
        } else {
            // Print integer
            llvm::GlobalVariable* formatStr = module->getGlobalVariable("print_fmt", true);
            if (!formatStr) {
                formatStr = builder->CreateGlobalString("%lld\n", "print_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {formatStr, arg});
            return llvm::ConstantInt::get(getDefaultType(), 0);
        }
    }

    if (expr->callee == "abs") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'abs' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        if (arg->getType()->isDoubleTy()) {
            // Use llvm.fabs intrinsic for native hardware abs on floats
            llvm::Function* fabsIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::fabs, {getFloatType()});
            return builder->CreateCall(fabsIntrinsic, {arg}, "fabsval");
        }
        // Use llvm.abs.i64 intrinsic for native hardware abs on integers
        llvm::Function* absIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::abs, {getDefaultType()});
        // The second argument (is_int_min_poison) is false for safe behavior
        return builder->CreateCall(absIntrinsic, {arg, builder->getFalse()}, "absval");
    }

    if (expr->callee == "len") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'len' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        Expression* argExpr = expr->arguments[0].get();

        // String detection uses two complementary checks:
        //   1. arg->getType()->isPointerTy()  — string literals (ptr @"...") and
        //      string variables whose LLVM alloca holds a ptr type.
        //   2. isStringExpr(argExpr) — string variables tracked in stringVars_
        //      that are stored as i64 (pointer cast to integer, OmScript's
        //      canonical runtime representation for strings passed across call
        //      boundaries).  In that case the IntToPtr below reconstructs the
        //      char* needed by strlen.
        // If neither is true the argument is assumed to be an array.
        if (arg->getType()->isPointerTy() || isStringExpr(argExpr)) {
            // Reconstruct the char* for strlen when the value is stored as i64.
            llvm::Value* strPtr = arg->getType()->isPointerTy()
                                      ? arg
                                      : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "len.sptr");
            llvm::Value* rawLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "len.strlen");
            // strlen returns size_t (i64 on 64-bit); ensure we return the default type.
            return rawLen->getType() == getDefaultType()
                       ? rawLen
                       : builder->CreateZExtOrTrunc(rawLen, getDefaultType(), "len.strsz");
        }
        // Array is stored as an i64 holding a pointer to [length, elem0, elem1, ...]
        // Convert to integer first if needed (e.g. if stored in a float variable)
        arg = toDefaultType(arg);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "arrptr");
        return builder->CreateLoad(getDefaultType(), arrPtr, "arrlen");
    }

    if (expr->callee == "min") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'min' expects 2 arguments, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        if (a->getType()->isDoubleTy() || b->getType()->isDoubleTy()) {
            if (!a->getType()->isDoubleTy())
                a = ensureFloat(a);
            if (!b->getType()->isDoubleTy())
                b = ensureFloat(b);
            // Use llvm.minnum intrinsic for native hardware fmin
            llvm::Function* fminIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::minnum, {getFloatType()});
            return builder->CreateCall(fminIntrinsic, {a, b}, "fminval");
        }
        // Use llvm.smin intrinsic for native hardware signed integer min
        llvm::Function* sminIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smin, {getDefaultType()});
        return builder->CreateCall(sminIntrinsic, {a, b}, "minval");
    }

    if (expr->callee == "max") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'max' expects 2 arguments, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        if (a->getType()->isDoubleTy() || b->getType()->isDoubleTy()) {
            if (!a->getType()->isDoubleTy())
                a = ensureFloat(a);
            if (!b->getType()->isDoubleTy())
                b = ensureFloat(b);
            // Use llvm.maxnum intrinsic for native hardware fmax
            llvm::Function* fmaxIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::maxnum, {getFloatType()});
            return builder->CreateCall(fmaxIntrinsic, {a, b}, "fmaxval");
        }
        // Use llvm.smax intrinsic for native hardware signed integer max
        llvm::Function* smaxIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smax, {getDefaultType()});
        return builder->CreateCall(smaxIntrinsic, {a, b}, "maxval");
    }

    if (expr->callee == "sign") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sign' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        if (x->getType()->isDoubleTy()) {
            llvm::Value* fzero = llvm::ConstantFP::get(getFloatType(), 0.0);
            llvm::Value* pos = llvm::ConstantInt::get(getDefaultType(), 1, true);
            llvm::Value* neg = llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(-1), true);
            llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
            llvm::Value* isNeg = builder->CreateFCmpOLT(x, fzero, "fsignneg");
            llvm::Value* negOrZero = builder->CreateSelect(isNeg, neg, zero, "fsignNZ");
            llvm::Value* isPos = builder->CreateFCmpOGT(x, fzero, "fsignpos");
            return builder->CreateSelect(isPos, pos, negOrZero, "fsignval");
        }
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* pos = llvm::ConstantInt::get(getDefaultType(), 1, true);
        llvm::Value* neg = llvm::ConstantInt::get(getDefaultType(), static_cast<uint64_t>(-1), true);
        llvm::Value* isNeg = builder->CreateICmpSLT(x, zero, "signneg");
        llvm::Value* negOrZero = builder->CreateSelect(isNeg, neg, zero, "signNZ");
        llvm::Value* isPos = builder->CreateICmpSGT(x, zero, "signpos");
        return builder->CreateSelect(isPos, pos, negOrZero, "signval");
    }

    if (expr->callee == "clamp") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'clamp' expects 3 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        llvm::Value* lo = generateExpression(expr->arguments[1].get());
        llvm::Value* hi = generateExpression(expr->arguments[2].get());
        // clamp(val, lo, hi) = max(lo, min(val, hi))
        if (val->getType()->isDoubleTy() || lo->getType()->isDoubleTy() || hi->getType()->isDoubleTy()) {
            if (!val->getType()->isDoubleTy())
                val = ensureFloat(val);
            if (!lo->getType()->isDoubleTy())
                lo = ensureFloat(lo);
            if (!hi->getType()->isDoubleTy())
                hi = ensureFloat(hi);
            llvm::Function* fminIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::minnum, {getFloatType()});
            llvm::Function* fmaxIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::maxnum, {getFloatType()});
            llvm::Value* minVH = builder->CreateCall(fminIntrinsic, {val, hi}, "fclampmin");
            return builder->CreateCall(fmaxIntrinsic, {minVH, lo}, "fclampval");
        }
        llvm::Function* sminIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smin, {getDefaultType()});
        llvm::Function* smaxIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::smax, {getDefaultType()});
        llvm::Value* minVH = builder->CreateCall(sminIntrinsic, {val, hi}, "clampmin");
        return builder->CreateCall(smaxIntrinsic, {minVH, lo}, "clampval");
    }

    if (expr->callee == "pow") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'pow' expects 2 arguments, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* base = generateExpression(expr->arguments[0].get());
        llvm::Value* exp  = generateExpression(expr->arguments[1].get());

        // If either argument is a float, delegate to the llvm.pow.f64 intrinsic.
        // This handles pow(2.0, 0.5) = sqrt(2), pow(x, n) for fractional n, etc.
        if (base->getType()->isDoubleTy() || exp->getType()->isDoubleTy()) {
            if (!base->getType()->isDoubleTy()) base = ensureFloat(base);
            if (!exp->getType()->isDoubleTy())  exp  = ensureFloat(exp);
            llvm::Function* powFn =
                OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::pow, {getFloatType()});
            return builder->CreateCall(powFn, {base, exp}, "pow.fresult");
        }

        // Integer path: convert to i64 and use binary exponentiation (O(log n)).
        base = toDefaultType(base);
        exp  = toDefaultType(exp);

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // Binary exponentiation (exponentiation by squaring): O(log n) in the exponent
        llvm::BasicBlock* negExpBB = llvm::BasicBlock::Create(*context, "pow.negexp", function);
        llvm::BasicBlock* posExpBB = llvm::BasicBlock::Create(*context, "pow.posexp", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "pow.loop", function);
        llvm::BasicBlock* oddBB = llvm::BasicBlock::Create(*context, "pow.odd", function);
        llvm::BasicBlock* squareBB = llvm::BasicBlock::Create(*context, "pow.square", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "pow.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1, true);
        llvm::Value* isNegExp = builder->CreateICmpSLT(exp, zero, "pow.isneg");
        builder->CreateCondBr(isNegExp, negExpBB, posExpBB);

        // Negative exponent: return 0 (integer approximation of base^(-n))
        builder->SetInsertPoint(negExpBB);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(posExpBB);
        builder->CreateBr(loopBB);

        // Loop: result *= base when exponent is odd; base *= base; exp >>= 1
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 3, "pow.result");
        llvm::PHINode* curBase = builder->CreatePHI(getDefaultType(), 3, "pow.base");
        llvm::PHINode* counter = builder->CreatePHI(getDefaultType(), 3, "pow.counter");
        result->addIncoming(one, posExpBB);
        curBase->addIncoming(base, posExpBB);
        counter->addIncoming(exp, posExpBB);

        llvm::Value* done = builder->CreateICmpSLE(counter, zero, "pow.done.cmp");
        builder->CreateCondBr(done, doneBB, oddBB);

        // Check if exponent is odd
        builder->SetInsertPoint(oddBB);
        llvm::Value* expBit = builder->CreateAnd(counter, one, "pow.bit");
        llvm::Value* isOdd = builder->CreateICmpNE(expBit, zero, "pow.isodd");
        llvm::Value* newResult = builder->CreateMul(result, curBase, "pow.mul");
        llvm::Value* resultSel = builder->CreateSelect(isOdd, newResult, result, "pow.rsel");
        builder->CreateBr(squareBB);

        // Square the base and halve the exponent
        builder->SetInsertPoint(squareBB);
        llvm::Value* newBase = builder->CreateMul(curBase, curBase, "pow.sq");
        llvm::Value* newCounter = builder->CreateAShr(counter, one, "pow.halve");
        result->addIncoming(resultSel, squareBB);
        curBase->addIncoming(newBase, squareBB);
        counter->addIncoming(newCounter, squareBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* finalResult = builder->CreatePHI(getDefaultType(), 2, "pow.final");
        finalResult->addIncoming(zero, negExpBB);
        finalResult->addIncoming(result, loopBB);
        return finalResult;
    }

    if (expr->callee == "print_char") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'print_char' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        if (arg->getType()->isDoubleTy()) {
            arg = builder->CreateFPToSI(arg, getDefaultType(), "ftoi");
        }
        llvm::Value* charCode;
        if (arg->getType()->isPointerTy() || isStringExpr(expr->arguments[0].get())) {
            // Argument is a string (e.g. result of to_char): load the first byte.
            llvm::Value* ptr = arg->getType()->isPointerTy()
                                   ? arg
                                   : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "pc.strptr");
            llvm::Value* byte = builder->CreateLoad(llvm::Type::getInt8Ty(*context), ptr, "pc.byte");
            charCode = builder->CreateZExt(byte, llvm::Type::getInt32Ty(*context), "charval");
        } else {
            charCode = builder->CreateTrunc(arg, llvm::Type::getInt32Ty(*context), "charval");
        }
        builder->CreateCall(getOrDeclarePutchar(), {charCode});
        return arg; // return the original argument as documented
    }

    if (expr->callee == "input") {
        if (!expr->arguments.empty()) {
            codegenError("Built-in function 'input' expects 0 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::AllocaInst* inputAlloca = createEntryBlockAlloca(function, "input_val");
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), inputAlloca);
        llvm::GlobalVariable* scanfFmt = module->getGlobalVariable("scanf_fmt", true);
        if (!scanfFmt) {
            scanfFmt = builder->CreateGlobalString("%lld", "scanf_fmt");
        }
        builder->CreateCall(getOrDeclareScanf(), {scanfFmt, inputAlloca});
        return builder->CreateLoad(getDefaultType(), inputAlloca, "input_read");
    }

    if (expr->callee == "input_line") {
        if (!expr->arguments.empty()) {
            codegenError("Built-in function 'input_line' expects 0 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        // Allocate a 1024-byte buffer
        llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 1024);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "inputln.buf");
        // Declare stdin as external global
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::GlobalVariable* stdinVar = module->getGlobalVariable("stdin");
        if (!stdinVar) {
            stdinVar =
                new llvm::GlobalVariable(*module, ptrTy, false, llvm::GlobalValue::ExternalLinkage, nullptr, "stdin");
        }
        llvm::Value* stdinVal = builder->CreateLoad(ptrTy, stdinVar, "inputln.stdin");
        // Call fgets(buf, 1024, stdin)
        llvm::Value* intSize = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1024);
        llvm::Value* fgetsRet = builder->CreateCall(getOrDeclareFgets(), {buf, intSize, stdinVal}, "inputln.fgets");
        // If fgets returns NULL (EOF/error), store empty string in buffer
        llvm::Value* fgetsNull = builder->CreateICmpEQ(fgetsRet, llvm::ConstantPointerNull::get(ptrTy), "inputln.eof");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* eofBB = llvm::BasicBlock::Create(*context, "inputln.eof", function);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "inputln.ok", function);
        llvm::BasicBlock* stripBB = llvm::BasicBlock::Create(*context, "inputln.strip", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "inputln.done", function);
        builder->CreateCondBr(fgetsNull, eofBB, okBB);
        // EOF path: store '\0' at start of buffer
        builder->SetInsertPoint(eofBB);
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), buf);
        builder->CreateBr(doneBB);
        // OK path: strip trailing newline
        builder->SetInsertPoint(okBB);
        llvm::Value* nlChar = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10);
        llvm::Value* nlPtr = builder->CreateCall(getOrDeclareStrchr(), {buf, nlChar}, "inputln.nl");
        llvm::Value* isNull = builder->CreateICmpEQ(nlPtr, llvm::ConstantPointerNull::get(ptrTy), "inputln.isnull");
        builder->CreateCondBr(isNull, doneBB, stripBB);
        builder->SetInsertPoint(stripBB);
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nlPtr);
        builder->CreateBr(doneBB);
        builder->SetInsertPoint(doneBB);
        stringReturningFunctions_.insert("input_line");
        return buf;
    }

    if (expr->callee == "sqrt") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sqrt' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Use the llvm.sqrt intrinsic which maps directly to hardware sqrtsd/sqrtss
        // instructions on x86, producing results in a single cycle on modern CPUs.
        // Convert to double, compute sqrt, then truncate back to integer.
        llvm::Value* fval = ensureFloat(x);
        llvm::Function* sqrtIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sqrt, {getFloatType()});
        llvm::Value* result = builder->CreateCall(sqrtIntrinsic, {fval}, "sqrt.result");
        return builder->CreateFPToSI(result, getDefaultType(), "sqrt.int");
    }

    if (expr->callee == "is_even") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'is_even' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_even() is an integer operation
        x = toDefaultType(x);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* bit = builder->CreateAnd(x, one, "evenbit");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isEven = builder->CreateICmpEQ(bit, zero, "iseven");
        return builder->CreateZExt(isEven, getDefaultType(), "evenval");
    }

    if (expr->callee == "is_odd") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'is_odd' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_odd() is an integer operation
        x = toDefaultType(x);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        return builder->CreateAnd(x, one, "oddval");
    }

    if (expr->callee == "sum") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sum' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // Array layout: [length, elem0, elem1, ...]
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "sum.arrptr");
        llvm::Value* length = builder->CreateLoad(getDefaultType(), arrPtr, "sum.len");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "sum.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "sum.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "sum.done", function);

        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::PHINode* acc = builder->CreatePHI(getDefaultType(), 2, "sum.acc");
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "sum.idx");
        acc->addIncoming(zero, entryBB);
        idx->addIncoming(zero, entryBB);

        llvm::Value* done = builder->CreateICmpSGE(idx, length, "sum.done");
        builder->CreateCondBr(done, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        // Element is at offset (idx + 1) from array base
        llvm::Value* offset = builder->CreateAdd(idx, llvm::ConstantInt::get(getDefaultType(), 1), "sum.offset");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, offset, "sum.elemptr");
        llvm::Value* elem = builder->CreateLoad(getDefaultType(), elemPtr, "sum.elem");
        llvm::Value* newAcc = builder->CreateAdd(acc, elem, "sum.newacc");
        llvm::Value* newIdx = builder->CreateAdd(idx, llvm::ConstantInt::get(getDefaultType(), 1), "sum.newidx");
        acc->addIncoming(newAcc, bodyBB);
        idx->addIncoming(newIdx, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return acc;
    }

    if (expr->callee == "swap") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'swap' expects 3 arguments (array, i, j), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* i = generateExpression(expr->arguments[1].get());
        llvm::Value* j = generateExpression(expr->arguments[2].get());

        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "swap.arrptr");
        llvm::Value* length = builder->CreateLoad(getDefaultType(), arrPtr, "swap.len");

        // Bounds check both indices: 0 <= i < length and 0 <= j < length
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* iInBounds = builder->CreateICmpSLT(i, length, "swap.i.inbounds");
        llvm::Value* iNotNeg = builder->CreateICmpSGE(i, zero, "swap.i.notneg");
        llvm::Value* iValid = builder->CreateAnd(iInBounds, iNotNeg, "swap.i.valid");
        llvm::Value* jInBounds = builder->CreateICmpSLT(j, length, "swap.j.inbounds");
        llvm::Value* jNotNeg = builder->CreateICmpSGE(j, zero, "swap.j.notneg");
        llvm::Value* jValid = builder->CreateAnd(jInBounds, jNotNeg, "swap.j.valid");
        llvm::Value* bothValid = builder->CreateAnd(iValid, jValid, "swap.valid");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "swap.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "swap.fail", function);
        builder->CreateCondBr(bothValid, okBB, failBB);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: swap index out of bounds\n", "swap_oob_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Elements are at offset (index + 1)
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* offI = builder->CreateAdd(i, one, "swap.offi");
        llvm::Value* offJ = builder->CreateAdd(j, one, "swap.offj");
        llvm::Value* ptrI = builder->CreateGEP(getDefaultType(), arrPtr, offI, "swap.ptri");
        llvm::Value* ptrJ = builder->CreateGEP(getDefaultType(), arrPtr, offJ, "swap.ptrj");
        llvm::Value* valI = builder->CreateLoad(getDefaultType(), ptrI, "swap.vali");
        llvm::Value* valJ = builder->CreateLoad(getDefaultType(), ptrJ, "swap.valj");
        builder->CreateStore(valJ, ptrI);
        builder->CreateStore(valI, ptrJ);
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (expr->callee == "reverse") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'reverse' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arrPtr = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "rev.arrptr");
        llvm::Value* length = builder->CreateLoad(getDefaultType(), arrPtr, "rev.len");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "rev.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "rev.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "rev.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* lastIdx = builder->CreateSub(length, one, "rev.last");
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* lo = builder->CreatePHI(getDefaultType(), 2, "rev.lo");
        llvm::PHINode* hi = builder->CreatePHI(getDefaultType(), 2, "rev.hi");
        lo->addIncoming(zero, entryBB);
        hi->addIncoming(lastIdx, entryBB);

        llvm::Value* done = builder->CreateICmpSGE(lo, hi, "rev.done");
        builder->CreateCondBr(done, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* offLo = builder->CreateAdd(lo, one, "rev.offlo");
        llvm::Value* offHi = builder->CreateAdd(hi, one, "rev.offhi");
        llvm::Value* ptrLo = builder->CreateGEP(getDefaultType(), arrPtr, offLo, "rev.ptrlo");
        llvm::Value* ptrHi = builder->CreateGEP(getDefaultType(), arrPtr, offHi, "rev.ptrhi");
        llvm::Value* valLo = builder->CreateLoad(getDefaultType(), ptrLo, "rev.vallo");
        llvm::Value* valHi = builder->CreateLoad(getDefaultType(), ptrHi, "rev.valhi");
        builder->CreateStore(valHi, ptrLo);
        builder->CreateStore(valLo, ptrHi);
        llvm::Value* newLo = builder->CreateAdd(lo, one, "rev.newlo");
        llvm::Value* newHi = builder->CreateSub(hi, one, "rev.newhi");
        lo->addIncoming(newLo, bodyBB);
        hi->addIncoming(newHi, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return arg;
    }

    if (expr->callee == "to_char") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'to_char' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        // Allocate a 2-byte buffer [char, '\0'] and return a pointer to it,
        // so the result behaves like a one-character string.
        llvm::Value* code = generateExpression(expr->arguments[0].get());
        code = toDefaultType(code);  // ensure i64
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {two}, "tochar.buf");
        llvm::Value* byteVal = builder->CreateTrunc(code, llvm::Type::getInt8Ty(*context), "tochar.byte");
        builder->CreateStore(byteVal, buf);
        llvm::Value* nulPtr = builder->CreateGEP(
            llvm::Type::getInt8Ty(*context), buf,
            llvm::ConstantInt::get(getDefaultType(), 1), "tochar.nul");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nulPtr);
        stringReturningFunctions_.insert("to_char");
        return buf;
    }

    if (expr->callee == "is_alpha") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'is_alpha' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_alpha() is an integer operation
        x = toDefaultType(x);
        // is_alpha: (x >= 'A' && x <= 'Z') || (x >= 'a' && x <= 'z')
        llvm::Value* geA = builder->CreateICmpSGE(x, llvm::ConstantInt::get(getDefaultType(), 65), "ge.A");
        llvm::Value* leZ = builder->CreateICmpSLE(x, llvm::ConstantInt::get(getDefaultType(), 90), "le.Z");
        llvm::Value* upper = builder->CreateAnd(geA, leZ, "isupper");
        llvm::Value* gea = builder->CreateICmpSGE(x, llvm::ConstantInt::get(getDefaultType(), 97), "ge.a");
        llvm::Value* lez = builder->CreateICmpSLE(x, llvm::ConstantInt::get(getDefaultType(), 122), "le.z");
        llvm::Value* lower = builder->CreateAnd(gea, lez, "islower");
        llvm::Value* isAlpha = builder->CreateOr(upper, lower, "isalpha");
        return builder->CreateZExt(isAlpha, getDefaultType(), "alphaval");
    }

    if (expr->callee == "is_digit") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'is_digit' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* x = generateExpression(expr->arguments[0].get());
        // Convert float to integer since is_digit() is an integer operation
        x = toDefaultType(x);
        // is_digit: x >= '0' && x <= '9'
        llvm::Value* ge0 = builder->CreateICmpSGE(x, llvm::ConstantInt::get(getDefaultType(), 48), "ge.0");
        llvm::Value* le9 = builder->CreateICmpSLE(x, llvm::ConstantInt::get(getDefaultType(), 57), "le.9");
        llvm::Value* isDigit = builder->CreateAnd(ge0, le9, "isdigit");
        return builder->CreateZExt(isDigit, getDefaultType(), "digitval");
    }

    // typeof(x) returns a type tag: 1=integer, 2=float, 3=string.
    // The tag is determined from the LLVM IR type of the expression, which is
    // known statically for literals and for variables whose alloca type was
    // set at declaration time.  Integer variables stored as i64 and arrays
    // (also i64 pointers) both return 1; floats stored as double return 2;
    // string literals and tracked string variables return 3.
    if (expr->callee == "typeof") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'typeof' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        // Evaluate the argument for its side effects, then derive type tag.
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        long long tag;
        if (arg->getType()->isDoubleTy()) {
            tag = 2; // float
        } else if (arg->getType()->isPointerTy() || isStringExpr(expr->arguments[0].get())) {
            tag = 3; // string
        } else {
            tag = 1; // integer (default for all i64 values including arrays)
        }
        (void)arg;
        return llvm::ConstantInt::get(getDefaultType(), tag);
    }

    // assert(condition) — aborts with an error if the condition is falsy.
    if (expr->callee == "assert") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'assert' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* condVal = generateExpression(expr->arguments[0].get());
        condVal = toBool(condVal);

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "assert.fail", function);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "assert.ok", function);

        builder->CreateCondBr(condVal, okBB, failBB);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: assertion failed\n", "assert_fail_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        return llvm::ConstantInt::get(getDefaultType(), 1);
    }

    if (expr->callee == "str_len") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'str_len' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // String may be a raw pointer (from a literal/local) or an i64 holding a pointer.
        llvm::Value* strPtr = arg->getType()->isPointerTy()
                                  ? arg
                                  : builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "strlen.ptr");
        return builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strlen.result");
    }

    if (expr->callee == "char_at") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'char_at' expects 2 arguments (string, index), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* idxArg = generateExpression(expr->arguments[1].get());
        idxArg = toDefaultType(idxArg);
        // String may be a raw pointer or an i64 holding a pointer.
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "charat.ptr");

        // Bounds check: 0 <= index < str_len(s)
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "charat.strlen");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* inBounds = builder->CreateICmpSLT(idxArg, strLen, "charat.inbounds");
        llvm::Value* notNeg = builder->CreateICmpSGE(idxArg, zero, "charat.notneg");
        llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "charat.valid");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "charat.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "charat.fail", function);
        builder->CreateCondBr(valid, okBB, failBB);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg =
            builder->CreateGlobalString("Runtime error: char_at index out of bounds\n", "charat_oob_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Load char via GEP
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, idxArg, "charat.gep");
        llvm::Value* charVal = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "charat.char");
        // Zero-extend to i64
        return builder->CreateZExt(charVal, getDefaultType(), "charat.ext");
    }

    if (expr->callee == "str_eq") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_eq' expects 2 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* lhsArg = generateExpression(expr->arguments[0].get());
        llvm::Value* rhsArg = generateExpression(expr->arguments[1].get());
        // Convert to pointers; the value may already be a pointer (literal/local).
        llvm::Value* lhsPtr =
            lhsArg->getType()->isPointerTy()
                ? lhsArg
                : builder->CreateIntToPtr(lhsArg, llvm::PointerType::getUnqual(*context), "streq.lhs");
        llvm::Value* rhsPtr =
            rhsArg->getType()->isPointerTy()
                ? rhsArg
                : builder->CreateIntToPtr(rhsArg, llvm::PointerType::getUnqual(*context), "streq.rhs");
        llvm::Value* cmpResult = builder->CreateCall(getOrDeclareStrcmp(), {lhsPtr, rhsPtr}, "streq.cmp");
        // strcmp returns 0 on equality; convert to boolean (1 if equal, 0 otherwise)
        llvm::Value* isEqual = builder->CreateICmpEQ(cmpResult, builder->getInt32(0), "streq.eq");
        return builder->CreateZExt(isEqual, getDefaultType(), "streq.result");
    }

    if (expr->callee == "str_concat") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_concat' expects 2 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* lhsArg = generateExpression(expr->arguments[0].get());
        llvm::Value* rhsArg = generateExpression(expr->arguments[1].get());
        llvm::Value* lhsPtr =
            lhsArg->getType()->isPointerTy()
                ? lhsArg
                : builder->CreateIntToPtr(lhsArg, llvm::PointerType::getUnqual(*context), "concat.lhs");
        llvm::Value* rhsPtr =
            rhsArg->getType()->isPointerTy()
                ? rhsArg
                : builder->CreateIntToPtr(rhsArg, llvm::PointerType::getUnqual(*context), "concat.rhs");
        llvm::Value* len1 = builder->CreateCall(getOrDeclareStrlen(), {lhsPtr}, "concat.len1");
        llvm::Value* len2 = builder->CreateCall(getOrDeclareStrlen(), {rhsPtr}, "concat.len2");
        llvm::Value* totalLen = builder->CreateAdd(len1, len2, "concat.totallen");
        // Allocate with power-of-2 rounding for amortized O(1) growth.
        // This matches C's realloc(cap*=2) strategy: the extra capacity
        // means subsequent realloc calls (below) often extend in-place.
        llvm::Value* rawSize =
            builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "concat.rawsize");
        // Round up to next power of 2 (minimum 32) to reduce realloc frequency
        llvm::Value* one64 = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* v = builder->CreateSub(rawSize, one64, "concat.pm1");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 1)), "concat.p2a");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 2)), "concat.p2b");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 4)), "concat.p2c");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 8)), "concat.p2d");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 16)), "concat.p2e");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 32)), "concat.p2f");
        llvm::Value* allocSize = builder->CreateAdd(v, one64, "concat.allocsize");
        llvm::Value* minCap = llvm::ConstantInt::get(getDefaultType(), 32);
        llvm::Value* useMin = builder->CreateICmpSLT(allocSize, minCap, "concat.usemin");
        allocSize = builder->CreateSelect(useMin, minCap, allocSize, "concat.finalcap");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "concat.buf");
        // memcpy(buf, lhs, len1)
        builder->CreateCall(getOrDeclareMemcpy(), {buf, lhsPtr, len1});
        // memcpy(buf + len1, rhs, len2)
        llvm::Value* dst2 = builder->CreateGEP(builder->getInt8Ty(), buf, len1, "concat.dst2");
        builder->CreateCall(getOrDeclareMemcpy(), {dst2, rhsPtr, len2});
        // null-terminate: buf[totalLen] = '\0'
        llvm::Value* endPtr = builder->CreateGEP(builder->getInt8Ty(), buf, totalLen, "concat.end");
        builder->CreateStore(builder->getInt8(0), endPtr);
        // Mark return as string-returning so callers can track it
        stringReturningFunctions_.insert("str_concat");
        return buf;
    }

    if (expr->callee == "log2") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'log2' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* n = generateExpression(expr->arguments[0].get());
        n = toDefaultType(n);
        // Integer log2 via loop: count how many times we can right-shift before reaching 0.
        // Returns -1 for n <= 0.
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "log2.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "log2.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "log2.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);

        // n <= 0 → return -1
        llvm::Value* isNonPositive = builder->CreateICmpSLE(n, zero, "log2.nonpos");
        llvm::BasicBlock* startBB = llvm::BasicBlock::Create(*context, "log2.start", function);
        builder->CreateCondBr(isNonPositive, doneBB, startBB);

        builder->SetInsertPoint(startBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* val = builder->CreatePHI(getDefaultType(), 2, "log2.val");
        val->addIncoming(n, startBB);
        llvm::PHINode* count = builder->CreatePHI(getDefaultType(), 2, "log2.count");
        count->addIncoming(negOne, startBB);

        // while val > 0: val >>= 1, count++
        llvm::Value* stillPositive = builder->CreateICmpSGT(val, zero, "log2.pos");
        builder->CreateCondBr(stillPositive, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* newVal = builder->CreateLShr(val, one, "log2.shr");
        llvm::Value* newCount = builder->CreateAdd(count, one, "log2.inc");
        val->addIncoming(newVal, bodyBB);
        count->addIncoming(newCount, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "log2.result");
        result->addIncoming(negOne, entryBB);
        result->addIncoming(count, loopBB);
        return result;
    }

    if (expr->callee == "gcd") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'gcd' expects 2 arguments, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* a = generateExpression(expr->arguments[0].get());
        llvm::Value* b = generateExpression(expr->arguments[1].get());
        a = toDefaultType(a);
        b = toDefaultType(b);
        // Use absolute values
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* aNeg = builder->CreateICmpSLT(a, zero, "gcd.aneg");
        llvm::Value* aNegVal = builder->CreateNeg(a, "gcd.anegval");
        a = builder->CreateSelect(aNeg, aNegVal, a, "gcd.aabs");
        llvm::Value* bNeg = builder->CreateICmpSLT(b, zero, "gcd.bneg");
        llvm::Value* bNegVal = builder->CreateNeg(b, "gcd.bnegval");
        b = builder->CreateSelect(bNeg, bNegVal, b, "gcd.babs");

        // Euclidean algorithm: while (b != 0) { temp = b; b = a % b; a = temp; }
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheaderBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "gcd.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "gcd.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "gcd.done", function);

        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* phiA = builder->CreatePHI(getDefaultType(), 2, "gcd.a");
        phiA->addIncoming(a, preheaderBB);
        llvm::PHINode* phiB = builder->CreatePHI(getDefaultType(), 2, "gcd.b");
        phiB->addIncoming(b, preheaderBB);

        llvm::Value* bIsZero = builder->CreateICmpEQ(phiB, zero, "gcd.bzero");
        builder->CreateCondBr(bIsZero, doneBB, bodyBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* remainder = builder->CreateURem(phiA, phiB, "gcd.rem");
        phiA->addIncoming(phiB, bodyBB);
        phiB->addIncoming(remainder, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return phiA;
    }

    if (expr->callee == "to_string") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'to_string' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        bool isFloat = val->getType()->isDoubleTy();
        if (!isFloat)
            val = toDefaultType(val);
        if (isFloat) {
            // Float: use a 32-byte buffer and %g format to preserve decimal places.
            llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 32);
            llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "tostr.buf");
            llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_float_fmt", true);
            if (!fmtStr)
                fmtStr = builder->CreateGlobalString("%g", "tostr_float_fmt");
            builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmtStr, val});
            stringReturningFunctions_.insert("to_string");
            return buf;
        }
        // Integer: 21 bytes is enough for any 64-bit signed decimal plus null terminator.
        llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 21);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "tostr.buf");
        // snprintf(buf, 21, "%lld", val)
        llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_fmt", true);
        if (!fmtStr) {
            fmtStr = builder->CreateGlobalString("%lld", "tostr_fmt");
        }
        builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmtStr, val});
        stringReturningFunctions_.insert("to_string");
        return buf;
    }

    if (expr->callee == "str_find") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_find' expects 2 arguments (string, char_code), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* chArg = generateExpression(expr->arguments[1].get());
        chArg = toDefaultType(chArg);
        // String may be a raw pointer or an i64 holding a pointer.
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strfind.ptr");
        // Get string length
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strfind.len");
        // memchr(strPtr, ch, strLen)
        llvm::Value* chTrunc = builder->CreateTrunc(chArg, llvm::Type::getInt32Ty(*context), "strfind.ch32");
        llvm::Value* found = builder->CreateCall(getOrDeclareMemchr(), {strPtr, chTrunc, strLen}, "strfind.found");
        // If memchr returns null, return -1; otherwise return (found - strPtr)
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* isNull = builder->CreateICmpEQ(found, nullPtr, "strfind.isnull");
        llvm::Value* foundInt = builder->CreatePtrToInt(found, getDefaultType(), "strfind.foundint");
        llvm::Value* baseInt = builder->CreatePtrToInt(strPtr, getDefaultType(), "strfind.baseint");
        llvm::Value* offset = builder->CreateSub(foundInt, baseInt, "strfind.offset");
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        return builder->CreateSelect(isNull, negOne, offset, "strfind.result");
    }

    // -----------------------------------------------------------------------
    // Math built-ins: floor, ceil, round
    // -----------------------------------------------------------------------

    if (expr->callee == "floor") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'floor' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        // Use llvm.floor intrinsic for native hardware rounding
        llvm::Function* floorIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::floor, {getFloatType()});
        llvm::Value* result = builder->CreateCall(floorIntrinsic, {fval}, "floor.result");
        return builder->CreateFPToSI(result, getDefaultType(), "floor.int");
    }

    if (expr->callee == "ceil") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'ceil' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        // Use llvm.ceil intrinsic for native hardware rounding
        llvm::Function* ceilIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::ceil, {getFloatType()});
        llvm::Value* result = builder->CreateCall(ceilIntrinsic, {fval}, "ceil.result");
        return builder->CreateFPToSI(result, getDefaultType(), "ceil.int");
    }

    if (expr->callee == "round") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'round' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        llvm::Value* fval = ensureFloat(arg);
        // Use llvm.round intrinsic for native hardware rounding
        llvm::Function* roundIntrinsic = OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::round, {getFloatType()});
        llvm::Value* result = builder->CreateCall(roundIntrinsic, {fval}, "round.result");
        return builder->CreateFPToSI(result, getDefaultType(), "round.int");
    }

    // -----------------------------------------------------------------------
    // Type conversion built-ins: to_int, to_float
    // -----------------------------------------------------------------------

    if (expr->callee == "to_int") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'to_int' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        if (arg->getType()->isDoubleTy()) {
            return builder->CreateFPToSI(arg, getDefaultType(), "toint.ftoi");
        }
        // If the argument is a string, parse it with strtoll.
        if (arg->getType()->isPointerTy() || isStringExpr(expr->arguments[0].get())) {
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            llvm::Value* strPtr = arg->getType()->isPointerTy()
                                      ? arg
                                      : builder->CreateIntToPtr(arg, ptrTy, "toint.strptr");
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            auto* base10 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10);
            return builder->CreateCall(getOrDeclareStrtoll(), {strPtr, nullPtr, base10}, "toint.parsed");
        }
        return toDefaultType(arg);
    }

    if (expr->callee == "to_float") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'to_float' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arg = generateExpression(expr->arguments[0].get());
        // If the argument is a string, parse it with strtod.
        if (arg->getType()->isPointerTy() || isStringExpr(expr->arguments[0].get())) {
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            llvm::Value* strPtr = arg->getType()->isPointerTy()
                                      ? arg
                                      : builder->CreateIntToPtr(arg, ptrTy, "tofloat.strptr");
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            return builder->CreateCall(getOrDeclareStrtod(), {strPtr, nullPtr}, "tofloat.parsed");
        }
        return ensureFloat(arg);
    }

    // -----------------------------------------------------------------------
    // String built-ins: str_substr, str_upper, str_lower, str_contains,
    //   str_replace, str_trim, str_starts_with, str_ends_with,
    //   str_index_of, str_repeat, str_reverse
    // -----------------------------------------------------------------------

    if (expr->callee == "str_substr") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'str_substr' expects 3 arguments (string, start, length), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* startArg = generateExpression(expr->arguments[1].get());
        llvm::Value* lenArg = generateExpression(expr->arguments[2].get());
        startArg = toDefaultType(startArg);
        lenArg = toDefaultType(lenArg);
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "substr.ptr");

        // Bounds checking: clamp start and length to valid range.
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "substr.strlen");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        // Clamp start: max(0, min(start, strLen))
        llvm::Value* startNeg = builder->CreateICmpSLT(startArg, zero, "substr.startneg");
        startArg = builder->CreateSelect(startNeg, zero, startArg, "substr.startclamp");
        llvm::Value* startOverflow = builder->CreateICmpSGT(startArg, strLen, "substr.startover");
        startArg = builder->CreateSelect(startOverflow, strLen, startArg, "substr.startfinal");
        // Clamp length: max(0, min(len, strLen - start))
        llvm::Value* remaining = builder->CreateSub(strLen, startArg, "substr.remaining");
        llvm::Value* lenNeg = builder->CreateICmpSLT(lenArg, zero, "substr.lenneg");
        lenArg = builder->CreateSelect(lenNeg, zero, lenArg, "substr.lenclamp");
        llvm::Value* lenOverflow = builder->CreateICmpSGT(lenArg, remaining, "substr.lenover");
        lenArg = builder->CreateSelect(lenOverflow, remaining, lenArg, "substr.lenfinal");

        // Allocate buffer: len + 1
        llvm::Value* allocSize =
            builder->CreateAdd(lenArg, llvm::ConstantInt::get(getDefaultType(), 1), "substr.alloc");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "substr.buf");
        // memcpy(buf, strPtr + start, len)
        llvm::Value* srcPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, startArg, "substr.src");
        builder->CreateCall(getOrDeclareMemcpy(), {buf, srcPtr, lenArg});
        // Null-terminate: buf[len] = 0
        llvm::Value* endPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), buf, lenArg, "substr.end");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), endPtr);
        stringReturningFunctions_.insert("str_substr");
        return buf;
    }

    if (expr->callee == "str_upper") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'str_upper' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "upper.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "upper.len");
        llvm::Value* allocSize = builder->CreateAdd(strLen, llvm::ConstantInt::get(getDefaultType(), 1), "upper.alloc");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "upper.buf");
        // Copy string then transform each character in a loop
        builder->CreateCall(getOrDeclareStrcpy(), {buf, strPtr});
        // Loop: for i = 0; i < strLen; i++ { buf[i] = toupper(buf[i]); }
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "upper.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "upper.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "upper.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "upper.idx");
        idx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpSLT(idx, strLen, "upper.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), buf, idx, "upper.charptr");
        llvm::Value* ch = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "upper.ch");
        llvm::Value* ch32 = builder->CreateZExt(ch, llvm::Type::getInt32Ty(*context), "upper.ch32");
        llvm::Value* upper = builder->CreateCall(getOrDeclareToupper(), {ch32}, "upper.toupper");
        llvm::Value* upper8 = builder->CreateTrunc(upper, llvm::Type::getInt8Ty(*context), "upper.trunc");
        builder->CreateStore(upper8, charPtr);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "upper.next");
        idx->addIncoming(nextIdx, bodyBB);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(doneBB);
        stringReturningFunctions_.insert("str_upper");
        return buf;
    }

    if (expr->callee == "str_lower") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'str_lower' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "lower.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "lower.len");
        llvm::Value* allocSize = builder->CreateAdd(strLen, llvm::ConstantInt::get(getDefaultType(), 1), "lower.alloc");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "lower.buf");
        builder->CreateCall(getOrDeclareStrcpy(), {buf, strPtr});
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "lower.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "lower.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "lower.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "lower.idx");
        idx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpSLT(idx, strLen, "lower.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), buf, idx, "lower.charptr");
        llvm::Value* ch = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "lower.ch");
        llvm::Value* ch32 = builder->CreateZExt(ch, llvm::Type::getInt32Ty(*context), "lower.ch32");
        llvm::Value* lower = builder->CreateCall(getOrDeclareTolower(), {ch32}, "lower.tolower");
        llvm::Value* lower8 = builder->CreateTrunc(lower, llvm::Type::getInt8Ty(*context), "lower.trunc");
        builder->CreateStore(lower8, charPtr);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "lower.next");
        idx->addIncoming(nextIdx, bodyBB);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(doneBB);
        stringReturningFunctions_.insert("str_lower");
        return buf;
    }

    if (expr->callee == "str_contains") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_contains' expects 2 arguments (haystack, needle), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* haystackArg = generateExpression(expr->arguments[0].get());
        llvm::Value* needleArg = generateExpression(expr->arguments[1].get());
        llvm::Value* haystackPtr =
            haystackArg->getType()->isPointerTy()
                ? haystackArg
                : builder->CreateIntToPtr(haystackArg, llvm::PointerType::getUnqual(*context), "contains.haystack");
        llvm::Value* needlePtr =
            needleArg->getType()->isPointerTy()
                ? needleArg
                : builder->CreateIntToPtr(needleArg, llvm::PointerType::getUnqual(*context), "contains.needle");
        llvm::Value* result = builder->CreateCall(getOrDeclareStrstr(), {haystackPtr, needlePtr}, "contains.strstr");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* isNotNull = builder->CreateICmpNE(result, nullPtr, "contains.notnull");
        return builder->CreateZExt(isNotNull, getDefaultType(), "contains.result");
    }

    if (expr->callee == "str_index_of") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_index_of' expects 2 arguments (haystack, needle), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* haystackArg = generateExpression(expr->arguments[0].get());
        llvm::Value* needleArg = generateExpression(expr->arguments[1].get());
        llvm::Value* haystackPtr =
            haystackArg->getType()->isPointerTy()
                ? haystackArg
                : builder->CreateIntToPtr(haystackArg, llvm::PointerType::getUnqual(*context), "indexof.haystack");
        llvm::Value* needlePtr =
            needleArg->getType()->isPointerTy()
                ? needleArg
                : builder->CreateIntToPtr(needleArg, llvm::PointerType::getUnqual(*context), "indexof.needle");
        llvm::Value* result = builder->CreateCall(getOrDeclareStrstr(), {haystackPtr, needlePtr}, "indexof.strstr");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* isNull = builder->CreateICmpEQ(result, nullPtr, "indexof.isnull");
        llvm::Value* foundInt = builder->CreatePtrToInt(result, getDefaultType(), "indexof.foundint");
        llvm::Value* baseInt = builder->CreatePtrToInt(haystackPtr, getDefaultType(), "indexof.baseint");
        llvm::Value* offset = builder->CreateSub(foundInt, baseInt, "indexof.offset");
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        return builder->CreateSelect(isNull, negOne, offset, "indexof.result");
    }

    if (expr->callee == "str_replace") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'str_replace' expects 3 arguments (string, old, new), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* oldArg = generateExpression(expr->arguments[1].get());
        llvm::Value* newArg = generateExpression(expr->arguments[2].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, ptrTy, "replace.str");
        llvm::Value* oldPtr =
            oldArg->getType()->isPointerTy()
                ? oldArg
                : builder->CreateIntToPtr(oldArg, ptrTy, "replace.old");
        llvm::Value* newPtr =
            newArg->getType()->isPointerTy()
                ? newArg
                : builder->CreateIntToPtr(newArg, ptrTy, "replace.new");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::Value* nullPtr  = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* zero     = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one      = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* i8zero   = llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0);

        llvm::Value* oldLen  = builder->CreateCall(getOrDeclareStrlen(), {oldPtr}, "replace.oldlen");
        llvm::Value* newLen  = builder->CreateCall(getOrDeclareStrlen(), {newPtr}, "replace.newlen");
        llvm::Value* strLen  = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "replace.strlen");

        // If old is the empty string, just return a copy of str to avoid
        // an infinite loop (strstr("x","") always succeeds).
        llvm::BasicBlock* emptyOldBB = llvm::BasicBlock::Create(*context, "replace.emptyold", function);
        llvm::BasicBlock* replaceMainBB = llvm::BasicBlock::Create(*context, "replace.main", function);
        llvm::Value* oldIsEmpty = builder->CreateICmpEQ(oldLen, zero, "replace.oldempty");
        builder->CreateCondBr(oldIsEmpty, emptyOldBB, replaceMainBB);

        // Empty old: return strdup(str)
        builder->SetInsertPoint(emptyOldBB);
        llvm::Value* copySize0 = builder->CreateAdd(strLen, one, "replace.copysize0");
        llvm::Value* copyBuf0  = builder->CreateCall(getOrDeclareMalloc(), {copySize0}, "replace.copybuf0");
        builder->CreateCall(getOrDeclareStrcpy(), {copyBuf0, strPtr});

        llvm::BasicBlock* emptyOldExitBB = builder->GetInsertBlock();
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "replace.merge", function);
        builder->CreateBr(mergeBB);

        // ---------------------------------------------------------------
        // Pass 1: count occurrences of old in str
        // ---------------------------------------------------------------
        builder->SetInsertPoint(replaceMainBB);
        llvm::BasicBlock* countLoopBB = llvm::BasicBlock::Create(*context, "replace.countloop", function);
        llvm::BasicBlock* countBodyBB = llvm::BasicBlock::Create(*context, "replace.countbody", function);
        llvm::BasicBlock* countDoneBB = llvm::BasicBlock::Create(*context, "replace.countdone", function);
        builder->CreateBr(countLoopBB);

        // Loop header: PHIs for cursor position and running count.
        builder->SetInsertPoint(countLoopBB);
        llvm::PHINode* cCursor = builder->CreatePHI(ptrTy, 2, "replace.ccursor");
        cCursor->addIncoming(strPtr, replaceMainBB);
        llvm::PHINode* cCount  = builder->CreatePHI(getDefaultType(), 2, "replace.ccount");
        cCount->addIncoming(zero, replaceMainBB);

        // Single strstr call per iteration.
        llvm::Value* cFound  = builder->CreateCall(getOrDeclareStrstr(), {cCursor, oldPtr}, "replace.cfound");
        llvm::Value* cIsNull = builder->CreateICmpEQ(cFound, nullPtr, "replace.cisnull");
        builder->CreateCondBr(cIsNull, countDoneBB, countBodyBB);

        // Body: increment count, advance cursor past the matched occurrence.
        builder->SetInsertPoint(countBodyBB);
        llvm::Value* newCount   = builder->CreateAdd(cCount, one, "replace.newcount");
        llvm::Value* nextCursor = builder->CreateGEP(llvm::Type::getInt8Ty(*context), cFound, oldLen, "replace.nextcursor");
        cCursor->addIncoming(nextCursor, countBodyBB);
        cCount->addIncoming(newCount, countBodyBB);
        builder->CreateBr(countLoopBB);

        // Done: cCount holds the total number of occurrences.
        builder->SetInsertPoint(countDoneBB);
        // cCount is the final count; use it directly (single predecessor from countLoopBB).
        llvm::Value* totalCount = cCount;

        // ---------------------------------------------------------------
        // Compute result size and allocate
        // ---------------------------------------------------------------
        // resultLen = strLen + totalCount * (newLen - oldLen)
        llvm::Value* lenDiff   = builder->CreateSub(newLen, oldLen, "replace.lendiff");
        llvm::Value* extraLen  = builder->CreateMul(totalCount, lenDiff, "replace.extralen");
        llvm::Value* resultLen = builder->CreateAdd(strLen, extraLen, "replace.resultlen");
        llvm::Value* resultSize= builder->CreateAdd(resultLen, one, "replace.resultsize");
        llvm::Value* resultBuf = builder->CreateCall(getOrDeclareMalloc(), {resultSize}, "replace.resultbuf");

        // ---------------------------------------------------------------
        // Pass 2: build output string, replacing every occurrence
        // ---------------------------------------------------------------
        llvm::BasicBlock* buildLoopBB = llvm::BasicBlock::Create(*context, "replace.buildloop", function);
        llvm::BasicBlock* buildBodyBB = llvm::BasicBlock::Create(*context, "replace.buildbody", function);
        llvm::BasicBlock* buildDoneBB = llvm::BasicBlock::Create(*context, "replace.builddone", function);
        builder->CreateBr(buildLoopBB);

        builder->SetInsertPoint(buildLoopBB);
        llvm::PHINode* bSrc = builder->CreatePHI(ptrTy, 2, "replace.bsrc");
        bSrc->addIncoming(strPtr, countDoneBB);
        llvm::PHINode* bDst = builder->CreatePHI(ptrTy, 2, "replace.bdst");
        bDst->addIncoming(resultBuf, countDoneBB);

        llvm::Value* bFound  = builder->CreateCall(getOrDeclareStrstr(), {bSrc, oldPtr}, "replace.bfound");
        llvm::Value* bIsNull = builder->CreateICmpEQ(bFound, nullPtr, "replace.bnull");
        builder->CreateCondBr(bIsNull, buildDoneBB, buildBodyBB);

        // Body: copy prefix, then replacement, advance
        builder->SetInsertPoint(buildBodyBB);
        llvm::Value* prefLen = builder->CreatePtrDiff(llvm::Type::getInt8Ty(*context), bFound, bSrc, "replace.preflen");
        builder->CreateCall(getOrDeclareMemcpy(), {bDst, bSrc, prefLen});
        llvm::Value* dstAfterPref = builder->CreateGEP(llvm::Type::getInt8Ty(*context), bDst, prefLen, "replace.dstpref");
        builder->CreateCall(getOrDeclareMemcpy(), {dstAfterPref, newPtr, newLen});
        llvm::Value* dstAfterNew  = builder->CreateGEP(llvm::Type::getInt8Ty(*context), dstAfterPref, newLen, "replace.dstnew");
        llvm::Value* srcAfterOld  = builder->CreateGEP(llvm::Type::getInt8Ty(*context), bFound, oldLen, "replace.srcold");
        bSrc->addIncoming(srcAfterOld, buildBodyBB);
        bDst->addIncoming(dstAfterNew, buildBodyBB);
        builder->CreateBr(buildLoopBB);

        // Done: copy remaining tail and null-terminate.
        // bSrc and bDst are live values from the loop header (single predecessor).
        builder->SetInsertPoint(buildDoneBB);
        // Copy remaining chars: tail = strLen - (bSrc - strPtr)
        llvm::Value* srcBase  = builder->CreatePtrToInt(strPtr, getDefaultType(), "replace.srcbase");
        llvm::Value* srcCurr  = builder->CreatePtrToInt(bSrc,   getDefaultType(), "replace.srccurr");
        llvm::Value* consumed = builder->CreateSub(srcCurr, srcBase, "replace.consumed");
        llvm::Value* tail     = builder->CreateSub(strLen, consumed, "replace.tail");
        builder->CreateCall(getOrDeclareMemcpy(), {bDst, bSrc, tail});
        llvm::Value* endPtr   = builder->CreateGEP(llvm::Type::getInt8Ty(*context), bDst, tail, "replace.end");
        builder->CreateStore(i8zero, endPtr);

        llvm::BasicBlock* buildExitBB = builder->GetInsertBlock();
        builder->CreateBr(mergeBB);

        // ---------------------------------------------------------------
        // Merge: return result (or copy if old was empty)
        // ---------------------------------------------------------------
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* resultPhi = builder->CreatePHI(ptrTy, 2, "replace.result");
        resultPhi->addIncoming(copyBuf0,   emptyOldExitBB);
        resultPhi->addIncoming(resultBuf,  buildExitBB);
        stringReturningFunctions_.insert("str_replace");
        return resultPhi;
    }

    if (expr->callee == "str_trim") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'str_trim' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
                                  ? strArg
                                  : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "trim.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "trim.len");

        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // Find start (skip leading whitespace)
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* startLoopBB = llvm::BasicBlock::Create(*context, "trim.startloop", function);
        llvm::BasicBlock* startBodyBB = llvm::BasicBlock::Create(*context, "trim.startbody", function);
        llvm::BasicBlock* startDoneBB = llvm::BasicBlock::Create(*context, "trim.startdone", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        builder->CreateBr(startLoopBB);

        builder->SetInsertPoint(startLoopBB);
        llvm::PHINode* startIdx = builder->CreatePHI(getDefaultType(), 2, "trim.startidx");
        startIdx->addIncoming(zero, preheader);
        llvm::Value* startCond = builder->CreateICmpSLT(startIdx, strLen, "trim.startcond");
        builder->CreateCondBr(startCond, startBodyBB, startDoneBB);

        builder->SetInsertPoint(startBodyBB);
        llvm::Value* startCharPtr =
            builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, startIdx, "trim.startcharptr");
        llvm::Value* startChar = builder->CreateLoad(llvm::Type::getInt8Ty(*context), startCharPtr, "trim.startchar");
        llvm::Value* startChar32 = builder->CreateZExt(startChar, llvm::Type::getInt32Ty(*context), "trim.startchar32");
        llvm::Value* isStartSpace = builder->CreateCall(getOrDeclareIsspace(), {startChar32}, "trim.isspace");
        llvm::Value* isStartSpaceBool = builder->CreateICmpNE(isStartSpace, builder->getInt32(0), "trim.isspacebool");
        llvm::Value* nextStartIdx = builder->CreateAdd(startIdx, one, "trim.nextstartidx");
        startIdx->addIncoming(nextStartIdx, startBodyBB);
        // If space, continue; otherwise done
        llvm::BasicBlock* startContBB = llvm::BasicBlock::Create(*context, "trim.startcont", function);
        builder->CreateCondBr(isStartSpaceBool, startContBB, startDoneBB);
        builder->SetInsertPoint(startContBB);
        builder->CreateBr(startLoopBB);
        // Update PHI to accept from cont block instead of body block
        startIdx->removeIncomingValue(startBodyBB);
        startIdx->addIncoming(nextStartIdx, startContBB);

        builder->SetInsertPoint(startDoneBB);
        llvm::PHINode* trimStart = builder->CreatePHI(getDefaultType(), 2, "trim.start");
        trimStart->addIncoming(startIdx, startLoopBB); // reached end of string
        trimStart->addIncoming(startIdx, startBodyBB); // found non-space

        // Find end (skip trailing whitespace)
        llvm::BasicBlock* endPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* endLoopBB = llvm::BasicBlock::Create(*context, "trim.endloop", function);
        llvm::BasicBlock* endBodyBB = llvm::BasicBlock::Create(*context, "trim.endbody", function);
        llvm::BasicBlock* endDoneBB = llvm::BasicBlock::Create(*context, "trim.enddone", function);
        builder->CreateBr(endLoopBB);

        builder->SetInsertPoint(endLoopBB);
        llvm::PHINode* endIdx = builder->CreatePHI(getDefaultType(), 2, "trim.endidx");
        endIdx->addIncoming(strLen, endPreBB);
        llvm::Value* endCond = builder->CreateICmpSGT(endIdx, trimStart, "trim.endcond");
        builder->CreateCondBr(endCond, endBodyBB, endDoneBB);

        builder->SetInsertPoint(endBodyBB);
        llvm::Value* prevEndIdx = builder->CreateSub(endIdx, one, "trim.prevendidx");
        llvm::Value* endCharPtr =
            builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, prevEndIdx, "trim.endcharptr");
        llvm::Value* endChar = builder->CreateLoad(llvm::Type::getInt8Ty(*context), endCharPtr, "trim.endchar");
        llvm::Value* endChar32 = builder->CreateZExt(endChar, llvm::Type::getInt32Ty(*context), "trim.endchar32");
        llvm::Value* isEndSpace = builder->CreateCall(getOrDeclareIsspace(), {endChar32}, "trim.isendspace");
        llvm::Value* isEndSpaceBool = builder->CreateICmpNE(isEndSpace, builder->getInt32(0), "trim.isendbool");
        llvm::BasicBlock* endContBB = llvm::BasicBlock::Create(*context, "trim.endcont", function);
        builder->CreateCondBr(isEndSpaceBool, endContBB, endDoneBB);
        builder->SetInsertPoint(endContBB);
        endIdx->addIncoming(prevEndIdx, endContBB);
        builder->CreateBr(endLoopBB);

        builder->SetInsertPoint(endDoneBB);
        llvm::PHINode* trimEnd = builder->CreatePHI(getDefaultType(), 2, "trim.end");
        trimEnd->addIncoming(endIdx, endLoopBB); // empty result
        trimEnd->addIncoming(endIdx, endBodyBB); // found non-space

        // Build trimmed string
        llvm::Value* trimLen = builder->CreateSub(trimEnd, trimStart, "trim.len2");
        llvm::Value* trimAlloc = builder->CreateAdd(trimLen, llvm::ConstantInt::get(getDefaultType(), 1), "trim.alloc");
        llvm::Value* trimBuf = builder->CreateCall(getOrDeclareMalloc(), {trimAlloc}, "trim.buf");
        llvm::Value* trimSrc = builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, trimStart, "trim.src");
        builder->CreateCall(getOrDeclareMemcpy(), {trimBuf, trimSrc, trimLen});
        llvm::Value* trimEndPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), trimBuf, trimLen, "trim.endptr");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), trimEndPtr);
        stringReturningFunctions_.insert("str_trim");
        return trimBuf;
    }

    if (expr->callee == "str_starts_with") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_starts_with' expects 2 arguments (string, prefix), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* prefixArg = generateExpression(expr->arguments[1].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "startswith.str");
        llvm::Value* prefixPtr =
            prefixArg->getType()->isPointerTy()
                ? prefixArg
                : builder->CreateIntToPtr(prefixArg, llvm::PointerType::getUnqual(*context), "startswith.prefix");
        // Check if strstr(str, prefix) == str
        llvm::Value* found = builder->CreateCall(getOrDeclareStrstr(), {strPtr, prefixPtr}, "startswith.found");
        llvm::Value* isSame = builder->CreateICmpEQ(found, strPtr, "startswith.eq");
        return builder->CreateZExt(isSame, getDefaultType(), "startswith.result");
    }

    if (expr->callee == "str_ends_with") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_ends_with' expects 2 arguments (string, suffix), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* suffixArg = generateExpression(expr->arguments[1].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "endswith.str");
        llvm::Value* suffixPtr =
            suffixArg->getType()->isPointerTy()
                ? suffixArg
                : builder->CreateIntToPtr(suffixArg, llvm::PointerType::getUnqual(*context), "endswith.suffix");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "endswith.strlen");
        llvm::Value* sufLen = builder->CreateCall(getOrDeclareStrlen(), {suffixPtr}, "endswith.suflen");
        // If suffix longer than string, return 0
        llvm::Value* tooLong = builder->CreateICmpSGT(sufLen, strLen, "endswith.toolong");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(*context, "endswith.check", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "endswith.fail", function);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "endswith.merge", function);
        builder->CreateCondBr(tooLong, failBB, checkBB);
        builder->SetInsertPoint(failBB);
        builder->CreateBr(mergeBB);
        builder->SetInsertPoint(checkBB);
        // Compare str + (strLen - sufLen) with suffix
        llvm::Value* offset = builder->CreateSub(strLen, sufLen, "endswith.offset");
        llvm::Value* tailPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, offset, "endswith.tail");
        llvm::Value* cmpResult = builder->CreateCall(getOrDeclareStrcmp(), {tailPtr, suffixPtr}, "endswith.cmp");
        llvm::Value* isEqual = builder->CreateICmpEQ(cmpResult, builder->getInt32(0), "endswith.eq");
        llvm::Value* resultCheck = builder->CreateZExt(isEqual, getDefaultType(), "endswith.result");
        builder->CreateBr(mergeBB);
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "endswith.phi");
        result->addIncoming(llvm::ConstantInt::get(getDefaultType(), 0), failBB);
        result->addIncoming(resultCheck, checkBB);
        return result;
    }

    if (expr->callee == "str_repeat") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_repeat' expects 2 arguments (string, count), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* countArg = generateExpression(expr->arguments[1].get());
        countArg = toDefaultType(countArg);
        // Clamp negative counts to 0 to prevent integer overflow in the
        // totalLen = strLen * count multiplication (negative * positive wraps
        // to a large unsigned value, causing malloc to over-allocate or fail).
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isNeg = builder->CreateICmpSLT(countArg, zero, "repeat.isneg");
        countArg = builder->CreateSelect(isNeg, zero, countArg, "repeat.clamp");
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "repeat.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "repeat.len");
        llvm::Value* totalLen = builder->CreateMul(strLen, countArg, "repeat.total");
        llvm::Value* allocSize =
            builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "repeat.alloc");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "repeat.buf");
        // Use memcpy with tracked offset instead of strcat to avoid O(n²) rescanning
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "repeat.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "repeat.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "repeat.done", function);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "repeat.idx");
        idx->addIncoming(zero, preheader);
        llvm::PHINode* offset = builder->CreatePHI(getDefaultType(), 2, "repeat.off");
        offset->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpSLT(idx, countArg, "repeat.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        // memcpy(buf + offset, str, strLen)
        llvm::Value* dst = builder->CreateGEP(builder->getInt8Ty(), buf, offset, "repeat.dst");
        builder->CreateCall(getOrDeclareMemcpy(), {dst, strPtr, strLen});
        llvm::Value* nextOffset = builder->CreateAdd(offset, strLen, "repeat.nextoff");
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "repeat.next");
        idx->addIncoming(nextIdx, bodyBB);
        offset->addIncoming(nextOffset, bodyBB);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(doneBB);
        // Null-terminate: buf[totalLen] = '\0'
        llvm::Value* endPtr = builder->CreateGEP(builder->getInt8Ty(), buf, totalLen, "repeat.end");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), endPtr);
        stringReturningFunctions_.insert("str_repeat");
        return buf;
    }

    if (expr->callee == "str_reverse") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'str_reverse' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strrev.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strrev.len");
        llvm::Value* allocSize =
            builder->CreateAdd(strLen, llvm::ConstantInt::get(getDefaultType(), 1), "strrev.alloc");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "strrev.buf");
        // Loop: buf[i] = str[len-1-i]
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "strrev.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "strrev.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "strrev.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "strrev.idx");
        idx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpSLT(idx, strLen, "strrev.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* revIdx = builder->CreateSub(builder->CreateSub(strLen, one, "strrev.lenm1"), idx, "strrev.revidx");
        llvm::Value* srcPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, revIdx, "strrev.srcptr");
        llvm::Value* ch = builder->CreateLoad(llvm::Type::getInt8Ty(*context), srcPtr, "strrev.ch");
        llvm::Value* dstPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), buf, idx, "strrev.dstptr");
        builder->CreateStore(ch, dstPtr);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "strrev.next");
        idx->addIncoming(nextIdx, bodyBB);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(doneBB);
        // Null-terminate
        llvm::Value* endPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), buf, strLen, "strrev.endptr");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), endPtr);
        stringReturningFunctions_.insert("str_reverse");
        return buf;
    }

    // -----------------------------------------------------------------------
    // Array built-ins: push, pop, index_of, array_contains, sort,
    //   array_fill, array_concat, array_slice
    // -----------------------------------------------------------------------

    if (expr->callee == "push") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'push' expects 2 arguments (array, value), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        valArg = toDefaultType(valArg);
        // Array layout: [length, elem0, elem1, ...]
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "push.arrptr");
        llvm::Value* oldLen = builder->CreateLoad(getDefaultType(), arrPtr, "push.oldlen");
        llvm::Value* newLen = builder->CreateAdd(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "push.newlen");
        // Round allocation up to next power of 2 for amortized O(1) growth.
        // Compute slots = newLen + 1 (header + elements), then round to next power of 2.
        llvm::Value* slots = builder->CreateAdd(newLen, llvm::ConstantInt::get(getDefaultType(), 1), "push.slots");
        // capacity = max(16, nextPow2(slots)): subtract 1, OR-cascade, add 1
        llvm::Value* one64 = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* v = builder->CreateSub(slots, one64, "push.pm1");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 1)), "push.p2a");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 2)), "push.p2b");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 4)), "push.p2c");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 8)), "push.p2d");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 16)), "push.p2e");
        v = builder->CreateOr(v, builder->CreateLShr(v, llvm::ConstantInt::get(getDefaultType(), 32)), "push.p2f");
        llvm::Value* cap = builder->CreateAdd(v, one64, "push.cap");
        // Ensure minimum capacity of 16 slots
        llvm::Value* minCap = llvm::ConstantInt::get(getDefaultType(), 16);
        llvm::Value* useMin = builder->CreateICmpSLT(cap, minCap, "push.usemin");
        cap = builder->CreateSelect(useMin, minCap, cap, "push.finalcap");
        llvm::Value* newSize = builder->CreateMul(cap,
            llvm::ConstantInt::get(getDefaultType(), 8), "push.bytes");
        llvm::Value* newBuf = builder->CreateCall(getOrDeclareRealloc(), {arrPtr, newSize}, "push.newbuf");
        // Update length
        builder->CreateStore(newLen, newBuf);
        // Store new value at index oldLen + 1 (after header)
        llvm::Value* newElemIdx =
            builder->CreateAdd(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "push.elemidx");
        llvm::Value* newElemPtr = builder->CreateGEP(getDefaultType(), newBuf, newElemIdx, "push.elemptr");
        builder->CreateStore(valArg, newElemPtr);
        // Return new array pointer as i64
        return builder->CreatePtrToInt(newBuf, getDefaultType(), "push.result");
    }

    if (expr->callee == "pop") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'pop' expects 1 argument (array), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "pop.arrptr");
        llvm::Value* oldLen = builder->CreateLoad(getDefaultType(), arrPtr, "pop.oldlen");

        // Guard against popping from an empty array.
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isEmpty = builder->CreateICmpSLE(oldLen, zero, "pop.empty");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "pop.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "pop.fail", function);
        builder->CreateCondBr(isEmpty, failBB, okBB);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: pop from empty array\n", "pop_empty_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        // Return the last element
        llvm::Value* lastIdx =
            builder->CreateAdd(builder->CreateSub(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "pop.lastoff"),
                               llvm::ConstantInt::get(getDefaultType(), 1), "pop.lastidx");
        llvm::Value* lastPtr = builder->CreateGEP(getDefaultType(), arrPtr, lastIdx, "pop.lastptr");
        llvm::Value* lastVal = builder->CreateLoad(getDefaultType(), lastPtr, "pop.lastval");
        // Decrease length in-place
        llvm::Value* newLen = builder->CreateSub(oldLen, llvm::ConstantInt::get(getDefaultType(), 1), "pop.newlen");
        builder->CreateStore(newLen, arrPtr);
        return lastVal;
    }

    if (expr->callee == "index_of") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'index_of' expects 2 arguments (array, value), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        valArg = toDefaultType(valArg);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "indexof.arrptr");
        llvm::Value* arrLen = builder->CreateLoad(getDefaultType(), arrPtr, "indexof.len");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "indexof.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "indexof.body", function);
        llvm::BasicBlock* nextBB = llvm::BasicBlock::Create(*context, "indexof.next", function);
        llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(*context, "indexof.found", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "indexof.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "indexof.idx");
        idx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpSLT(idx, arrLen, "indexof.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "indexof.elemidx");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, elemIdx, "indexof.elemptr");
        llvm::Value* elem = builder->CreateLoad(getDefaultType(), elemPtr, "indexof.elem");
        llvm::Value* match = builder->CreateICmpEQ(elem, valArg, "indexof.match");
        builder->CreateCondBr(match, foundBB, nextBB);
        builder->SetInsertPoint(nextBB);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "indexof.next");
        idx->addIncoming(nextIdx, nextBB);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(foundBB);
        builder->CreateBr(doneBB);
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "indexof.result");
        result->addIncoming(negOne, loopBB);
        result->addIncoming(idx, foundBB);
        return result;
    }

    if (expr->callee == "array_contains") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'array_contains' expects 2 arguments (array, value), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        valArg = toDefaultType(valArg);
        llvm::Value* arrPtr =
            builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "contains.arrptr");
        llvm::Value* arrLen = builder->CreateLoad(getDefaultType(), arrPtr, "contains.len");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "contains.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "contains.body", function);
        llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(*context, "contains.found", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "contains.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "contains.idx");
        idx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpSLT(idx, arrLen, "contains.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "contains.elemidx");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, elemIdx, "contains.elemptr");
        llvm::Value* elem = builder->CreateLoad(getDefaultType(), elemPtr, "contains.elem");
        llvm::Value* match = builder->CreateICmpEQ(elem, valArg, "contains.match");
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "contains.next");
        idx->addIncoming(nextIdx, bodyBB);
        builder->CreateCondBr(match, foundBB, loopBB);
        builder->SetInsertPoint(foundBB);
        builder->CreateBr(doneBB);
        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "contains.result");
        result->addIncoming(llvm::ConstantInt::get(getDefaultType(), 0), loopBB);
        result->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), foundBB);
        return result;
    }

    if (expr->callee == "sort") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sort' expects 1 argument (array), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        bool sortStrings = isStringArrayExpr(expr->arguments[0].get());
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "sort.arrptr");
        llvm::Value* arrLen = builder->CreateLoad(getDefaultType(), arrPtr, "sort.len");
        // Simple bubble sort in LLVM IR (in-place)
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* outerLoopBB = llvm::BasicBlock::Create(*context, "sort.outer", function);
        llvm::BasicBlock* innerLoopBB = llvm::BasicBlock::Create(*context, "sort.inner", function);
        llvm::BasicBlock* innerBodyBB = llvm::BasicBlock::Create(*context, "sort.innerbody", function);
        llvm::BasicBlock* swapBB = llvm::BasicBlock::Create(*context, "sort.swap", function);
        llvm::BasicBlock* noswapBB = llvm::BasicBlock::Create(*context, "sort.noswap", function);
        llvm::BasicBlock* outerIncBB = llvm::BasicBlock::Create(*context, "sort.outerinc", function);
        llvm::BasicBlock* outerDoneBB = llvm::BasicBlock::Create(*context, "sort.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* limit = builder->CreateSub(arrLen, one, "sort.limit");
        builder->CreateBr(outerLoopBB);

        builder->SetInsertPoint(outerLoopBB);
        llvm::PHINode* i = builder->CreatePHI(getDefaultType(), 2, "sort.i");
        i->addIncoming(zero, preheader);
        llvm::Value* outerCond = builder->CreateICmpSLT(i, limit, "sort.outercond");
        builder->CreateCondBr(outerCond, innerLoopBB, outerDoneBB);

        builder->SetInsertPoint(innerLoopBB);
        llvm::PHINode* j = builder->CreatePHI(getDefaultType(), 2, "sort.j");
        j->addIncoming(zero, outerLoopBB);
        llvm::Value* innerLimit = builder->CreateSub(limit, i, "sort.innerlimit");
        llvm::Value* innerCond = builder->CreateICmpSLT(j, innerLimit, "sort.innercond");
        builder->CreateCondBr(innerCond, innerBodyBB, outerIncBB);

        builder->SetInsertPoint(innerBodyBB);
        llvm::Value* idx1 = builder->CreateAdd(j, one, "sort.idx1");
        llvm::Value* idx2 = builder->CreateAdd(j, llvm::ConstantInt::get(getDefaultType(), 2), "sort.idx2");
        llvm::Value* ptr1 = builder->CreateGEP(getDefaultType(), arrPtr, idx1, "sort.ptr1");
        llvm::Value* ptr2 = builder->CreateGEP(getDefaultType(), arrPtr, idx2, "sort.ptr2");
        llvm::Value* val1 = builder->CreateLoad(getDefaultType(), ptr1, "sort.val1");
        llvm::Value* val2 = builder->CreateLoad(getDefaultType(), ptr2, "sort.val2");
        llvm::Value* needSwap;
        if (sortStrings) {
            // String sort: use strcmp(val1_as_ptr, val2_as_ptr) > 0
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            llvm::Value* sptr1 = builder->CreateIntToPtr(val1, ptrTy, "sort.sptr1");
            llvm::Value* sptr2 = builder->CreateIntToPtr(val2, ptrTy, "sort.sptr2");
            llvm::Value* cmpRes = builder->CreateCall(getOrDeclareStrcmp(), {sptr1, sptr2}, "sort.strcmp");
            needSwap = builder->CreateICmpSGT(cmpRes, builder->getInt32(0), "sort.needswap");
        } else {
            needSwap = builder->CreateICmpSGT(val1, val2, "sort.needswap");
        }
        builder->CreateCondBr(needSwap, swapBB, noswapBB);

        builder->SetInsertPoint(swapBB);
        builder->CreateStore(val2, ptr1);
        builder->CreateStore(val1, ptr2);
        builder->CreateBr(noswapBB);

        builder->SetInsertPoint(noswapBB);
        llvm::Value* nextJ = builder->CreateAdd(j, one, "sort.nextj");
        j->addIncoming(nextJ, noswapBB);
        builder->CreateBr(innerLoopBB);

        builder->SetInsertPoint(outerIncBB);
        llvm::Value* nextI = builder->CreateAdd(i, one, "sort.nexti");
        i->addIncoming(nextI, outerIncBB);
        builder->CreateBr(outerLoopBB);

        builder->SetInsertPoint(outerDoneBB);
        return arrArg; // Return the array itself
    }

    if (expr->callee == "array_fill") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'array_fill' expects 2 arguments (size, value), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* sizeArg = generateExpression(expr->arguments[0].get());
        llvm::Value* valArg = generateExpression(expr->arguments[1].get());
        sizeArg = toDefaultType(sizeArg);
        valArg = toDefaultType(valArg);
        // Clamp negative sizes to 0 to prevent integer overflow in the
        // allocation size calculation (negative * 8 wraps to a huge unsigned
        // value, causing malloc to over-allocate or fail).
        {
            llvm::Value* zeroClamp = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* isNeg = builder->CreateICmpSLT(sizeArg, zeroClamp, "fill.isneg");
            sizeArg = builder->CreateSelect(isNeg, zeroClamp, sizeArg, "fill.clamp");
        }
        // Allocate: (size + 1) * 8 bytes
        llvm::Value* slots = builder->CreateAdd(sizeArg, llvm::ConstantInt::get(getDefaultType(), 1), "fill.slots");
        llvm::Value* bytes = builder->CreateMul(slots, llvm::ConstantInt::get(getDefaultType(), 8), "fill.bytes");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "fill.buf");
        // Store length
        builder->CreateStore(sizeArg, buf);
        // Fill loop
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "fill.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "fill.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "fill.done", function);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "fill.idx");
        idx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpSLT(idx, sizeArg, "fill.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "fill.elemidx");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), buf, elemIdx, "fill.elemptr");
        builder->CreateStore(valArg, elemPtr);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "fill.next");
        idx->addIncoming(nextIdx, bodyBB);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "fill.result");
    }

    if (expr->callee == "array_concat") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'array_concat' expects 2 arguments (array1, array2), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arr1Arg = generateExpression(expr->arguments[0].get());
        llvm::Value* arr2Arg = generateExpression(expr->arguments[1].get());
        arr1Arg = toDefaultType(arr1Arg);
        arr2Arg = toDefaultType(arr2Arg);
        llvm::Value* arr1Ptr = builder->CreateIntToPtr(arr1Arg, llvm::PointerType::getUnqual(*context), "aconcat.ptr1");
        llvm::Value* arr2Ptr = builder->CreateIntToPtr(arr2Arg, llvm::PointerType::getUnqual(*context), "aconcat.ptr2");
        llvm::Value* len1 = builder->CreateLoad(getDefaultType(), arr1Ptr, "aconcat.len1");
        llvm::Value* len2 = builder->CreateLoad(getDefaultType(), arr2Ptr, "aconcat.len2");
        llvm::Value* totalLen = builder->CreateAdd(len1, len2, "aconcat.total");
        // Allocate: (totalLen + 1) * 8
        llvm::Value* slots = builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "aconcat.slots");
        llvm::Value* bytes = builder->CreateMul(slots, llvm::ConstantInt::get(getDefaultType(), 8), "aconcat.bytes");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "aconcat.buf");
        // Store length
        builder->CreateStore(totalLen, buf);
        // Copy arr1 elements (len1 * 8 bytes starting at arr1[1])
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* src1 = builder->CreateGEP(getDefaultType(), arr1Ptr, one, "aconcat.src1");
        llvm::Value* dst1 = builder->CreateGEP(getDefaultType(), buf, one, "aconcat.dst1");
        llvm::Value* copy1Size = builder->CreateMul(len1, eight, "aconcat.copy1size");
        builder->CreateCall(getOrDeclareMemcpy(), {dst1, src1, copy1Size});
        // Copy arr2 elements
        llvm::Value* dst2Idx = builder->CreateAdd(len1, one, "aconcat.dst2idx");
        llvm::Value* dst2 = builder->CreateGEP(getDefaultType(), buf, dst2Idx, "aconcat.dst2");
        llvm::Value* src2 = builder->CreateGEP(getDefaultType(), arr2Ptr, one, "aconcat.src2");
        llvm::Value* copy2Size = builder->CreateMul(len2, eight, "aconcat.copy2size");
        builder->CreateCall(getOrDeclareMemcpy(), {dst2, src2, copy2Size});
        return builder->CreatePtrToInt(buf, getDefaultType(), "aconcat.result");
    }

    if (expr->callee == "array_slice") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'array_slice' expects 3 arguments (array, start, end), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* startArg = generateExpression(expr->arguments[1].get());
        llvm::Value* endArg = generateExpression(expr->arguments[2].get());
        arrArg = toDefaultType(arrArg);
        startArg = toDefaultType(startArg);
        endArg = toDefaultType(endArg);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "slice.arrptr");
        llvm::Value* arrLen = builder->CreateLoad(getDefaultType(), arrPtr, "slice.arrlen");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        // Clamp start: max(0, min(start, arrLen))
        llvm::Value* startNeg = builder->CreateICmpSLT(startArg, zero, "slice.startneg");
        startArg = builder->CreateSelect(startNeg, zero, startArg, "slice.startclamp");
        llvm::Value* startOver = builder->CreateICmpSGT(startArg, arrLen, "slice.startover");
        startArg = builder->CreateSelect(startOver, arrLen, startArg, "slice.startfinal");
        // Clamp end: max(start, min(end, arrLen))
        llvm::Value* endNeg = builder->CreateICmpSLT(endArg, startArg, "slice.endneg");
        endArg = builder->CreateSelect(endNeg, startArg, endArg, "slice.endclamp");
        llvm::Value* endOver = builder->CreateICmpSGT(endArg, arrLen, "slice.endover");
        endArg = builder->CreateSelect(endOver, arrLen, endArg, "slice.endfinal");

        llvm::Value* sliceLen = builder->CreateSub(endArg, startArg, "slice.len");
        // Allocate: (sliceLen + 1) * 8
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots = builder->CreateAdd(sliceLen, one, "slice.slots");
        llvm::Value* bytes = builder->CreateMul(slots, eight, "slice.bytes");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "slice.buf");
        builder->CreateStore(sliceLen, buf);
        // Copy elements: arr[start+1..end+1) to buf[1..)
        llvm::Value* srcIdx = builder->CreateAdd(startArg, one, "slice.srcidx");
        llvm::Value* src = builder->CreateGEP(getDefaultType(), arrPtr, srcIdx, "slice.src");
        llvm::Value* dst = builder->CreateGEP(getDefaultType(), buf, one, "slice.dst");
        llvm::Value* copySize = builder->CreateMul(sliceLen, eight, "slice.copysize");
        builder->CreateCall(getOrDeclareMemcpy(), {dst, src, copySize});
        return builder->CreatePtrToInt(buf, getDefaultType(), "slice.result");
    }

    if (expr->callee == "array_copy") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'array_copy' expects 1 argument (array), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "acopy.arrptr");
        llvm::Value* arrLen = builder->CreateLoad(getDefaultType(), arrPtr, "acopy.len");
        // Allocate: (length + 1) * 8 bytes
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "acopy.slots");
        llvm::Value* bytes = builder->CreateMul(slots, eight, "acopy.bytes");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "acopy.buf");
        // Copy all data: (length + 1) * 8 bytes
        builder->CreateCall(getOrDeclareMemcpy(), {buf, arrPtr, bytes});
        return builder->CreatePtrToInt(buf, getDefaultType(), "acopy.result");
    }

    if (expr->callee == "array_remove") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'array_remove' expects 2 arguments (array, index), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* idxArg = generateExpression(expr->arguments[1].get());
        arrArg = toDefaultType(arrArg);
        idxArg = toDefaultType(idxArg);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "aremove.arrptr");
        llvm::Value* arrLen = builder->CreateLoad(getDefaultType(), arrPtr, "aremove.len");
        // Bounds check: 0 <= idx < length
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* inBounds = builder->CreateICmpSLT(idxArg, arrLen, "aremove.inbounds");
        llvm::Value* notNeg = builder->CreateICmpSGE(idxArg, zero, "aremove.notneg");
        llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "aremove.valid");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "aremove.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "aremove.fail", function);
        builder->CreateCondBr(valid, okBB, failBB);
        // Out-of-bounds: print error and abort
        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg =
            builder->CreateGlobalString("Runtime error: array_remove index out of bounds\n", "aremove_oob_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();
        // In-bounds: save removed value, shift elements left, decrement length
        builder->SetInsertPoint(okBB);
        llvm::Value* elemOffset = builder->CreateAdd(idxArg, one, "aremove.elemoff");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, elemOffset, "aremove.elemptr");
        llvm::Value* removedVal = builder->CreateLoad(getDefaultType(), elemPtr, "aremove.removed");
        // memmove(&arr[idx+1], &arr[idx+2], (length - idx - 1) * 8)
        llvm::Value* srcOffset =
            builder->CreateAdd(idxArg, llvm::ConstantInt::get(getDefaultType(), 2), "aremove.srcoff");
        llvm::Value* srcPtr = builder->CreateGEP(getDefaultType(), arrPtr, srcOffset, "aremove.srcptr");
        llvm::Value* shiftCount =
            builder->CreateSub(arrLen, builder->CreateAdd(idxArg, one, "aremove.idxp1"), "aremove.shiftcnt");
        llvm::Value* shiftBytes = builder->CreateMul(shiftCount, eight, "aremove.shiftbytes");
        builder->CreateCall(getOrDeclareMemmove(), {elemPtr, srcPtr, shiftBytes});
        // Decrement length
        llvm::Value* newLen = builder->CreateSub(arrLen, one, "aremove.newlen");
        builder->CreateStore(newLen, arrPtr);
        return removedVal;
    }

    // -----------------------------------------------------------------------
    // array_map(arr, "fn_name") — apply named function to each element, return new array
    // -----------------------------------------------------------------------
    if (expr->callee == "array_map") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'array_map' expects 2 arguments (array, function_name), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        // The second argument must be a string literal (function name)
        auto* fnNameLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit || fnNameLit->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("array_map: second argument must be a string literal (function name)", expr);
        }
        std::string fnName = fnNameLit->stringValue;
        // Look up the target function in the LLVM module
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second) {
            codegenError("array_map: unknown function '" + fnName + "'", expr);
        }
        llvm::Function* mapFn = calleeIt->second;
        if (mapFn->arg_size() < 1) {
            codegenError("array_map: function '" + fnName + "' must accept at least 1 argument", expr);
        }

        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "amap.arrptr");
        llvm::Value* arrLen = builder->CreateLoad(getDefaultType(), arrPtr, "amap.len");

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Allocate result array: (arrLen + 1) * 8
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "amap.slots");
        llvm::Value* bytes = builder->CreateMul(slots, eight, "amap.bytes");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "amap.buf");
        builder->CreateStore(arrLen, buf);

        // Loop: for each element, call mapFn and store result
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "amap.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "amap.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "amap.done", function);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "amap.idx");
        idx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpSLT(idx, arrLen, "amap.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        // Load element from source: arrPtr[idx + 1]
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "amap.elemidx");
        llvm::Value* srcPtr = builder->CreateGEP(getDefaultType(), arrPtr, elemIdx, "amap.srcptr");
        llvm::Value* elem = builder->CreateLoad(getDefaultType(), srcPtr, "amap.elem");
        // Call the map function with the element
        llvm::Value* mapped = builder->CreateCall(mapFn, {elem}, "amap.mapped");
        mapped = toDefaultType(mapped);
        // Store into result: buf[idx + 1]
        llvm::Value* dstPtr = builder->CreateGEP(getDefaultType(), buf, elemIdx, "amap.dstptr");
        builder->CreateStore(mapped, dstPtr);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "amap.next");
        idx->addIncoming(nextIdx, bodyBB);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "amap.result");
    }

    // -----------------------------------------------------------------------
    // array_filter(arr, "fn_name") — return new array of elements where fn returns non-zero
    // -----------------------------------------------------------------------
    if (expr->callee == "array_filter") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'array_filter' expects 2 arguments (array, function_name), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        auto* fnNameLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit || fnNameLit->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("array_filter: second argument must be a string literal (function name)", expr);
        }
        std::string fnName = fnNameLit->stringValue;
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second) {
            codegenError("array_filter: unknown function '" + fnName + "'", expr);
        }
        llvm::Function* filterFn = calleeIt->second;
        if (filterFn->arg_size() < 1) {
            codegenError("array_filter: function '" + fnName + "' must accept at least 1 argument", expr);
        }

        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        arrArg = toDefaultType(arrArg);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "afilt.arrptr");
        llvm::Value* arrLen = builder->CreateLoad(getDefaultType(), arrPtr, "afilt.len");

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Allocate result array with max possible size: (arrLen + 1) * 8
        llvm::Value* slots = builder->CreateAdd(arrLen, one, "afilt.slots");
        llvm::Value* bytes = builder->CreateMul(slots, eight, "afilt.bytes");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "afilt.buf");
        // Initialize length to 0 (will be updated as we add elements)
        builder->CreateStore(zero, buf);

        // Loop: for each element, call filterFn; if non-zero, add to result
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "afilt.loop", function);
        llvm::BasicBlock* testBB = llvm::BasicBlock::Create(*context, "afilt.test", function);
        llvm::BasicBlock* addBB = llvm::BasicBlock::Create(*context, "afilt.add", function);
        llvm::BasicBlock* incBB = llvm::BasicBlock::Create(*context, "afilt.inc", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "afilt.done", function);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "afilt.idx");
        idx->addIncoming(zero, preheader);
        llvm::PHINode* outIdx = builder->CreatePHI(getDefaultType(), 2, "afilt.outidx");
        outIdx->addIncoming(zero, preheader);
        llvm::Value* cond = builder->CreateICmpSLT(idx, arrLen, "afilt.cond");
        builder->CreateCondBr(cond, testBB, doneBB);

        builder->SetInsertPoint(testBB);
        // Load element from source
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "afilt.elemidx");
        llvm::Value* srcPtr = builder->CreateGEP(getDefaultType(), arrPtr, elemIdx, "afilt.srcptr");
        llvm::Value* elem = builder->CreateLoad(getDefaultType(), srcPtr, "afilt.elem");
        // Call the filter function
        llvm::Value* result = builder->CreateCall(filterFn, {elem}, "afilt.result");
        result = toDefaultType(result);
        llvm::Value* keep = builder->CreateICmpNE(result, zero, "afilt.keep");
        builder->CreateCondBr(keep, addBB, incBB);

        // Add element to result
        builder->SetInsertPoint(addBB);
        llvm::Value* dstIdx = builder->CreateAdd(outIdx, one, "afilt.dstidx");
        llvm::Value* dstPtr = builder->CreateGEP(getDefaultType(), buf, dstIdx, "afilt.dstptr");
        builder->CreateStore(elem, dstPtr);
        llvm::Value* newOutIdx = builder->CreateAdd(outIdx, one, "afilt.newoutidx");
        builder->CreateBr(incBB);

        // Increment loop counter
        builder->SetInsertPoint(incBB);
        llvm::PHINode* outIdxMerge = builder->CreatePHI(getDefaultType(), 2, "afilt.outmerge");
        outIdxMerge->addIncoming(outIdx, testBB);
        outIdxMerge->addIncoming(newOutIdx, addBB);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "afilt.next");
        idx->addIncoming(nextIdx, incBB);
        outIdx->addIncoming(outIdxMerge, incBB);
        builder->CreateBr(loopBB);

        // Done: store final length
        builder->SetInsertPoint(doneBB);
        builder->CreateStore(outIdx, buf);
        return builder->CreatePtrToInt(buf, getDefaultType(), "afilt.result");
    }

    // -----------------------------------------------------------------------
    // array_reduce(arr, "fn_name", initial) — reduce array to single value
    //   fn_name must be a function that takes 2 arguments: (accumulator, element)
    // -----------------------------------------------------------------------
    if (expr->callee == "array_reduce") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'array_reduce' expects 3 arguments (array, function_name, initial), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        auto* fnNameLit = dynamic_cast<LiteralExpr*>(expr->arguments[1].get());
        if (!fnNameLit || fnNameLit->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("array_reduce: second argument must be a string literal (function name)", expr);
        }
        std::string fnName = fnNameLit->stringValue;
        auto calleeIt = functions.find(fnName);
        if (calleeIt == functions.end() || !calleeIt->second) {
            codegenError("array_reduce: unknown function '" + fnName + "'", expr);
        }
        llvm::Function* reduceFn = calleeIt->second;
        if (reduceFn->arg_size() < 2) {
            codegenError("array_reduce: function '" + fnName +
                             "' must accept at least 2 arguments (accumulator, element)",
                         expr);
        }

        llvm::Value* arrArg = generateExpression(expr->arguments[0].get());
        llvm::Value* initVal = generateExpression(expr->arguments[2].get());
        arrArg = toDefaultType(arrArg);
        initVal = toDefaultType(initVal);
        llvm::Value* arrPtr = builder->CreateIntToPtr(arrArg, llvm::PointerType::getUnqual(*context), "areduce.arrptr");
        llvm::Value* arrLen = builder->CreateLoad(getDefaultType(), arrPtr, "areduce.len");

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);

        // Loop: accumulate with fn(acc, element)
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "areduce.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "areduce.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "areduce.done", function);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "areduce.idx");
        idx->addIncoming(zero, preheader);
        llvm::PHINode* acc = builder->CreatePHI(getDefaultType(), 2, "areduce.acc");
        acc->addIncoming(initVal, preheader);
        llvm::Value* cond = builder->CreateICmpSLT(idx, arrLen, "areduce.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        // Load element from source: arrPtr[idx + 1]
        llvm::Value* elemIdx = builder->CreateAdd(idx, one, "areduce.elemidx");
        llvm::Value* srcPtr = builder->CreateGEP(getDefaultType(), arrPtr, elemIdx, "areduce.srcptr");
        llvm::Value* elem = builder->CreateLoad(getDefaultType(), srcPtr, "areduce.elem");
        // Call reduce function: fn(accumulator, element)
        llvm::Value* newAcc = builder->CreateCall(reduceFn, {acc, elem}, "areduce.newacc");
        newAcc = toDefaultType(newAcc);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "areduce.next");
        idx->addIncoming(nextIdx, bodyBB);
        acc->addIncoming(newAcc, bodyBB);
        builder->CreateBr(loopBB);

        // Done: return final accumulator
        builder->SetInsertPoint(doneBB);
        return acc;
    }

    // -----------------------------------------------------------------------
    // println(x) — print value followed by newline (same as print but explicit)
    // -----------------------------------------------------------------------
    if (expr->callee == "println") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'println' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        Expression* argExpr = expr->arguments[0].get();
        llvm::Value* arg = generateExpression(argExpr);
        if (arg->getType()->isDoubleTy()) {
            llvm::GlobalVariable* floatFmt = module->getGlobalVariable("println_float_fmt", true);
            if (!floatFmt) {
                floatFmt = builder->CreateGlobalString("%g\n", "println_float_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {floatFmt, arg});
        } else if (arg->getType()->isPointerTy() || isStringExpr(argExpr)) {
            if (!arg->getType()->isPointerTy()) {
                arg = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "println.str.ptr");
            }
            llvm::GlobalVariable* strFmt = module->getGlobalVariable("println_str_fmt", true);
            if (!strFmt) {
                strFmt = builder->CreateGlobalString("%s\n", "println_str_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {strFmt, arg});
        } else {
            llvm::GlobalVariable* formatStr = module->getGlobalVariable("println_fmt", true);
            if (!formatStr) {
                formatStr = builder->CreateGlobalString("%lld\n", "println_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {formatStr, arg});
        }
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // -----------------------------------------------------------------------
    // write(x) — print without trailing newline
    // -----------------------------------------------------------------------
    if (expr->callee == "write") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'write' expects 1 argument, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        Expression* argExpr = expr->arguments[0].get();
        llvm::Value* arg = generateExpression(argExpr);
        if (arg->getType()->isDoubleTy()) {
            llvm::GlobalVariable* floatFmt = module->getGlobalVariable("write_float_fmt", true);
            if (!floatFmt) {
                floatFmt = builder->CreateGlobalString("%g", "write_float_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {floatFmt, arg});
        } else if (arg->getType()->isPointerTy() || isStringExpr(argExpr)) {
            if (!arg->getType()->isPointerTy()) {
                arg = builder->CreateIntToPtr(arg, llvm::PointerType::getUnqual(*context), "write.str.ptr");
            }
            llvm::GlobalVariable* strFmt = module->getGlobalVariable("write_str_fmt", true);
            if (!strFmt) {
                strFmt = builder->CreateGlobalString("%s", "write_str_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {strFmt, arg});
        } else {
            llvm::GlobalVariable* formatStr = module->getGlobalVariable("write_fmt", true);
            if (!formatStr) {
                formatStr = builder->CreateGlobalString("%lld", "write_fmt");
            }
            builder->CreateCall(getPrintfFunction(), {formatStr, arg});
        }
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // -----------------------------------------------------------------------
    // exit_program(code) — terminate the process with the given exit code
    // -----------------------------------------------------------------------
    if (expr->callee == "exit_program") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'exit_program' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* code = generateExpression(expr->arguments[0].get());
        code = toDefaultType(code);
        llvm::Value* code32 = builder->CreateTrunc(code, llvm::Type::getInt32Ty(*context), "exit.code");
        builder->CreateCall(getOrDeclareExit(), {code32});
        builder->CreateUnreachable();
        // Create a dead block so subsequent IR generation still has an insert point.
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* deadBB = llvm::BasicBlock::Create(*context, "exit.dead", function);
        builder->SetInsertPoint(deadBB);
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // -----------------------------------------------------------------------
    // random() — returns a pseudo-random integer (seeds once automatically)
    // -----------------------------------------------------------------------
    if (expr->callee == "random") {
        if (!expr->arguments.empty()) {
            codegenError("Built-in function 'random' expects 0 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        // Seed on first call via a global flag
        llvm::GlobalVariable* seeded = module->getGlobalVariable("__om_rand_seeded", true);
        if (!seeded) {
            seeded = new llvm::GlobalVariable(
                *module, llvm::Type::getInt32Ty(*context), false, llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "__om_rand_seeded");
        }
        llvm::Value* flag = builder->CreateLoad(llvm::Type::getInt32Ty(*context), seeded, "rand.flag");
        llvm::Value* isZero =
            builder->CreateICmpEQ(flag, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "rand.cmp");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* seedBB = llvm::BasicBlock::Create(*context, "rand.seed", function);
        llvm::BasicBlock* callBB = llvm::BasicBlock::Create(*context, "rand.call", function);

        builder->CreateCondBr(isZero, seedBB, callBB);

        builder->SetInsertPoint(seedBB);
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* t = builder->CreateCall(getOrDeclareTimeFunc(), {nullPtr}, "rand.time");
        llvm::Value* t32 = builder->CreateTrunc(t, llvm::Type::getInt32Ty(*context), "rand.time32");
        builder->CreateCall(getOrDeclareSrand(), {t32});
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1), seeded);
        builder->CreateBr(callBB);

        builder->SetInsertPoint(callBB);
        llvm::Value* r = builder->CreateCall(getOrDeclareRand(), {}, "rand.val");
        return builder->CreateSExt(r, getDefaultType(), "rand.ext");
    }

    // -----------------------------------------------------------------------
    // time() — returns current Unix timestamp in seconds
    // -----------------------------------------------------------------------
    if (expr->callee == "time") {
        if (!expr->arguments.empty()) {
            codegenError("Built-in function 'time' expects 0 arguments, but " + std::to_string(expr->arguments.size()) +
                             " provided",
                         expr);
        }
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        return builder->CreateCall(getOrDeclareTimeFunc(), {nullPtr}, "time.val");
    }

    // -----------------------------------------------------------------------
    // sleep(ms) — sleep for given milliseconds
    // -----------------------------------------------------------------------
    if (expr->callee == "sleep") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'sleep' expects 1 argument (milliseconds), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* ms = generateExpression(expr->arguments[0].get());
        ms = toDefaultType(ms);
        // usleep takes microseconds, so multiply by 1000
        llvm::Value* us = builder->CreateMul(ms, llvm::ConstantInt::get(getDefaultType(), 1000), "sleep.us");
        llvm::Value* us32 = builder->CreateTrunc(us, llvm::Type::getInt32Ty(*context), "sleep.us32");
        builder->CreateCall(getOrDeclareUsleep(), {us32});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    // -----------------------------------------------------------------------
    // str_to_int(s) — parse string to integer
    // -----------------------------------------------------------------------
    if (expr->callee == "str_to_int") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'str_to_int' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strtoi.ptr");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        llvm::Value* base10 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10);
        return builder->CreateCall(getOrDeclareStrtoll(), {strPtr, nullPtr, base10}, "strtoi.val");
    }

    // -----------------------------------------------------------------------
    // str_to_float(s) — parse string to float
    // -----------------------------------------------------------------------
    if (expr->callee == "str_to_float") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'str_to_float' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "strtof.ptr");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context));
        return builder->CreateCall(getOrDeclareStrtod(), {strPtr, nullPtr}, "strtof.val");
    }

    // -----------------------------------------------------------------------
    // str_split(s, delim) — split string by delimiter, returns array of strings
    // -----------------------------------------------------------------------
    if (expr->callee == "str_split") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'str_split' expects 2 arguments (string, delimiter), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* delimArg = generateExpression(expr->arguments[1].get());

        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "split.str");
        llvm::Value* delimPtr =
            delimArg->getType()->isPointerTy()
                ? delimArg
                : builder->CreateIntToPtr(delimArg, llvm::PointerType::getUnqual(*context), "split.delim");

        // Get the delimiter character (first char of delimiter string)
        llvm::Value* delimChar = builder->CreateLoad(llvm::Type::getInt8Ty(*context), delimPtr, "split.delimch");
        llvm::Value* delimChar32 = builder->CreateZExt(delimChar, llvm::Type::getInt32Ty(*context), "split.delimch32");

        // Count delimiters to know array size
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "split.strlen");
        llvm::Function* function = builder->GetInsertBlock()->getParent();

        // Count pass
        llvm::BasicBlock* preheader = builder->GetInsertBlock();
        llvm::BasicBlock* countLoopBB = llvm::BasicBlock::Create(*context, "split.countloop", function);
        llvm::BasicBlock* countBodyBB = llvm::BasicBlock::Create(*context, "split.countbody", function);
        llvm::BasicBlock* countIncBB = llvm::BasicBlock::Create(*context, "split.countinc", function);
        llvm::BasicBlock* countDoneBB = llvm::BasicBlock::Create(*context, "split.countdone", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        builder->CreateBr(countLoopBB);

        builder->SetInsertPoint(countLoopBB);
        llvm::PHINode* ci = builder->CreatePHI(getDefaultType(), 2, "split.ci");
        ci->addIncoming(zero, preheader);
        llvm::PHINode* cnt = builder->CreatePHI(getDefaultType(), 2, "split.cnt");
        cnt->addIncoming(one, preheader); // at least 1 part
        llvm::Value* ccond = builder->CreateICmpSLT(ci, strLen, "split.ccond");
        builder->CreateCondBr(ccond, countBodyBB, countDoneBB);

        builder->SetInsertPoint(countBodyBB);
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, ci, "split.cptr");
        llvm::Value* ch = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "split.ch");
        llvm::Value* ch32 = builder->CreateZExt(ch, llvm::Type::getInt32Ty(*context), "split.ch32");
        llvm::Value* isDelim = builder->CreateICmpEQ(ch32, delimChar32, "split.isdelim");
        llvm::Value* inc = builder->CreateSelect(isDelim, one, zero, "split.inc");
        llvm::Value* newCnt = builder->CreateAdd(cnt, inc, "split.newcnt");
        builder->CreateBr(countIncBB);

        builder->SetInsertPoint(countIncBB);
        llvm::Value* nextCi = builder->CreateAdd(ci, one, "split.nextci");
        ci->addIncoming(nextCi, countIncBB);
        cnt->addIncoming(newCnt, countIncBB);
        builder->CreateBr(countLoopBB);

        builder->SetInsertPoint(countDoneBB);
        // cnt now holds the number of parts

        // Allocate result array: (cnt + 1) * 8 bytes (length + elements)
        llvm::Value* slots = builder->CreateAdd(cnt, one, "split.slots");
        llvm::Value* bytes = builder->CreateMul(slots, eight, "split.bytes");
        llvm::Value* arrBuf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "split.arr");
        builder->CreateStore(cnt, arrBuf);

        // Split pass: iterate and create substrings
        llvm::BasicBlock* splitPreBB = builder->GetInsertBlock();
        llvm::BasicBlock* splitLoopBB = llvm::BasicBlock::Create(*context, "split.loop", function);
        llvm::BasicBlock* splitBodyBB = llvm::BasicBlock::Create(*context, "split.body", function);
        llvm::BasicBlock* splitDelimBB = llvm::BasicBlock::Create(*context, "split.delim", function);
        llvm::BasicBlock* splitContBB = llvm::BasicBlock::Create(*context, "split.cont", function);
        llvm::BasicBlock* splitDoneBB = llvm::BasicBlock::Create(*context, "split.done", function);

        builder->CreateBr(splitLoopBB);

        builder->SetInsertPoint(splitLoopBB);
        llvm::PHINode* si = builder->CreatePHI(getDefaultType(), 2, "split.si");
        si->addIncoming(zero, splitPreBB);
        llvm::PHINode* partIdx = builder->CreatePHI(getDefaultType(), 2, "split.pidx");
        partIdx->addIncoming(zero, splitPreBB);
        llvm::PHINode* partStart = builder->CreatePHI(getDefaultType(), 2, "split.pstart");
        partStart->addIncoming(zero, splitPreBB);
        llvm::Value* scond = builder->CreateICmpSLE(si, strLen, "split.scond");
        builder->CreateCondBr(scond, splitBodyBB, splitDoneBB);

        builder->SetInsertPoint(splitBodyBB);
        // Check if at end of string or at delimiter
        llvm::Value* atEnd = builder->CreateICmpEQ(si, strLen, "split.atend");
        llvm::Value* bodyCharPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, si, "split.bptr");
        llvm::Value* bodyCh = builder->CreateLoad(llvm::Type::getInt8Ty(*context), bodyCharPtr, "split.bch");
        llvm::Value* bodyCh32 = builder->CreateZExt(bodyCh, llvm::Type::getInt32Ty(*context), "split.bch32");
        llvm::Value* bodyIsDelim = builder->CreateICmpEQ(bodyCh32, delimChar32, "split.bisdelim");
        llvm::Value* shouldSplit = builder->CreateOr(atEnd, bodyIsDelim, "split.shouldsplit");
        builder->CreateCondBr(shouldSplit, splitDelimBB, splitContBB);

        builder->SetInsertPoint(splitDelimBB);
        // Create substring from partStart to si
        llvm::Value* partLen = builder->CreateSub(si, partStart, "split.plen");
        llvm::Value* srcStart =
            builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, partStart, "split.srcstart");
        llvm::Value* sub = builder->CreateCall(getOrDeclareStrndup(), {srcStart, partLen}, "split.sub");
        llvm::Value* subInt = builder->CreatePtrToInt(sub, getDefaultType(), "split.subint");
        // Store in array at (partIdx + 1) position
        llvm::Value* arrSlot = builder->CreateAdd(partIdx, one, "split.slot");
        llvm::Value* arrSlotPtr = builder->CreateGEP(getDefaultType(), arrBuf, arrSlot, "split.slotptr");
        builder->CreateStore(subInt, arrSlotPtr);
        llvm::Value* nextPartIdx = builder->CreateAdd(partIdx, one, "split.npidx");
        llvm::Value* nextPartStart = builder->CreateAdd(si, one, "split.npstart");
        builder->CreateBr(splitContBB);

        builder->SetInsertPoint(splitContBB);
        llvm::PHINode* mergedIdx = builder->CreatePHI(getDefaultType(), 2, "split.midx");
        mergedIdx->addIncoming(partIdx, splitBodyBB);
        mergedIdx->addIncoming(nextPartIdx, splitDelimBB);
        llvm::PHINode* mergedStart = builder->CreatePHI(getDefaultType(), 2, "split.mstart");
        mergedStart->addIncoming(partStart, splitBodyBB);
        mergedStart->addIncoming(nextPartStart, splitDelimBB);
        llvm::Value* nextSi = builder->CreateAdd(si, one, "split.nextsi");
        si->addIncoming(nextSi, splitContBB);
        partIdx->addIncoming(mergedIdx, splitContBB);
        partStart->addIncoming(mergedStart, splitContBB);
        builder->CreateBr(splitLoopBB);

        builder->SetInsertPoint(splitDoneBB);
        return builder->CreatePtrToInt(arrBuf, getDefaultType(), "split.result");
    }

    // -----------------------------------------------------------------------
    // str_chars(s) — convert string into array of character codes
    // -----------------------------------------------------------------------
    if (expr->callee == "str_chars") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'str_chars' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        llvm::Value* strPtr =
            strArg->getType()->isPointerTy()
                ? strArg
                : builder->CreateIntToPtr(strArg, llvm::PointerType::getUnqual(*context), "chars.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "chars.len");

        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);

        // Allocate array: (len + 1) * 8
        llvm::Value* slots = builder->CreateAdd(strLen, one, "chars.slots");
        llvm::Value* bytes = builder->CreateMul(slots, eight, "chars.bytes");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "chars.buf");
        builder->CreateStore(strLen, buf);

        // Fill loop
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "chars.loop", function);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "chars.body", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "chars.done", function);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "chars.idx");
        idx->addIncoming(zero, preBB);
        llvm::Value* cond = builder->CreateICmpSLT(idx, strLen, "chars.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* charP = builder->CreateGEP(llvm::Type::getInt8Ty(*context), strPtr, idx, "chars.cptr");
        llvm::Value* ch = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charP, "chars.ch");
        llvm::Value* chExt = builder->CreateZExt(ch, getDefaultType(), "chars.chext");
        llvm::Value* arrSlot = builder->CreateAdd(idx, one, "chars.slot");
        llvm::Value* arrSlotPtr = builder->CreateGEP(getDefaultType(), buf, arrSlot, "chars.slotptr");
        builder->CreateStore(chExt, arrSlotPtr);
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "chars.next");
        idx->addIncoming(nextIdx, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "chars.result");
    }

    // -----------------------------------------------------------------------
    // File I/O built-ins
    // -----------------------------------------------------------------------

    if (expr->callee == "file_read") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'file_read' expects 1 argument (path)", expr);
        }
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        llvm::Value* pathPtr =
            pathArg->getType()->isPointerTy()
                ? pathArg
                : builder->CreateIntToPtr(pathArg, llvm::PointerType::getUnqual(*context), "fread.path");

        // mode = "rb"
        llvm::GlobalVariable* mode = module->getGlobalVariable("__fread_mode", true);
        if (!mode)
            mode = builder->CreateGlobalString("rb", "__fread_mode");

        // FILE* fp = fopen(path, "rb")
        llvm::Value* fp = builder->CreateCall(getOrDeclareFopen(), {pathPtr, mode}, "fread.fp");

        // Check for null
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* isNull = builder->CreateICmpEQ(fp, nullPtr, "fread.isnull");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(*context, "fread.null", parentFn);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "fread.ok", parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "fread.merge", parentFn);
        builder->CreateCondBr(isNull, nullBB, okBB);

        // Null path: return empty string
        builder->SetInsertPoint(nullBB);
        llvm::Value* emptyBuf = builder->CreateCall(getOrDeclareMalloc(),
            {llvm::ConstantInt::get(getDefaultType(), 1)}, "fread.empty");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), emptyBuf);
        llvm::Value* emptyResult = emptyBuf;
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* nullEndBB = builder->GetInsertBlock();

        // OK path: seek to end, get size, read
        builder->SetInsertPoint(okBB);
        // fseek(fp, 0, SEEK_END=2)
        builder->CreateCall(getOrDeclareFseek(),
            {fp, llvm::ConstantInt::get(getDefaultType(), 0),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 2)});
        // size = ftell(fp)
        llvm::Value* fileSize = builder->CreateCall(getOrDeclareFtell(), {fp}, "fread.size");
        // fseek(fp, 0, SEEK_SET=0)
        builder->CreateCall(getOrDeclareFseek(),
            {fp, llvm::ConstantInt::get(getDefaultType(), 0),
             llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0)});
        // buf = malloc(size + 1)
        llvm::Value* bufSize = builder->CreateAdd(fileSize,
            llvm::ConstantInt::get(getDefaultType(), 1), "fread.bufsize");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "fread.buf");
        // fread(buf, 1, size, fp)
        builder->CreateCall(getOrDeclareFread(),
            {buf, llvm::ConstantInt::get(getDefaultType(), 1), fileSize, fp});
        // null terminate
        llvm::Value* nullTermPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), buf, fileSize, "fread.nullterm");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), nullTermPtr);
        // fclose(fp)
        builder->CreateCall(getOrDeclareFclose(), {fp});
        llvm::Value* okResult = buf;
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* okEndBB = builder->GetInsertBlock();

        // Merge
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(ptrTy, 2, "fread.result");
        phi->addIncoming(emptyResult, nullEndBB);
        phi->addIncoming(okResult, okEndBB);
        stringReturningFunctions_.insert("file_read");
        return phi;
    }

    if (expr->callee == "file_write") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'file_write' expects 2 arguments (path, content)", expr);
        }
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        llvm::Value* contentArg = generateExpression(expr->arguments[1].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* pathPtr = pathArg->getType()->isPointerTy()
            ? pathArg : builder->CreateIntToPtr(pathArg, ptrTy, "fwrite.path");
        llvm::Value* contentPtr = contentArg->getType()->isPointerTy()
            ? contentArg : builder->CreateIntToPtr(contentArg, ptrTy, "fwrite.content");

        llvm::GlobalVariable* mode = module->getGlobalVariable("__fwrite_mode", true);
        if (!mode)
            mode = builder->CreateGlobalString("wb", "__fwrite_mode");

        llvm::Value* fp = builder->CreateCall(getOrDeclareFopen(), {pathPtr, mode}, "fwrite.fp");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* isNull = builder->CreateICmpEQ(fp, nullPtr, "fwrite.isnull");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(*context, "fwrite.null", parentFn);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "fwrite.ok", parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "fwrite.merge", parentFn);
        builder->CreateCondBr(isNull, nullBB, okBB);

        builder->SetInsertPoint(nullBB);
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(okBB);
        llvm::Value* slen = builder->CreateCall(getOrDeclareStrlen(), {contentPtr}, "fwrite.len");
        builder->CreateCall(getOrDeclareFwrite(),
            {contentPtr, llvm::ConstantInt::get(getDefaultType(), 1), slen, fp});
        builder->CreateCall(getOrDeclareFclose(), {fp});
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(getDefaultType(), 2, "fwrite.result");
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), nullBB);
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 0), okBB);
        return phi;
    }

    if (expr->callee == "file_append") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'file_append' expects 2 arguments (path, content)", expr);
        }
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        llvm::Value* contentArg = generateExpression(expr->arguments[1].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* pathPtr = pathArg->getType()->isPointerTy()
            ? pathArg : builder->CreateIntToPtr(pathArg, ptrTy, "fappend.path");
        llvm::Value* contentPtr = contentArg->getType()->isPointerTy()
            ? contentArg : builder->CreateIntToPtr(contentArg, ptrTy, "fappend.content");

        llvm::GlobalVariable* mode = module->getGlobalVariable("__fappend_mode", true);
        if (!mode)
            mode = builder->CreateGlobalString("a", "__fappend_mode");

        llvm::Value* fp = builder->CreateCall(getOrDeclareFopen(), {pathPtr, mode}, "fappend.fp");
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* isNull = builder->CreateICmpEQ(fp, nullPtr, "fappend.isnull");

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* nullBB = llvm::BasicBlock::Create(*context, "fappend.null", parentFn);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "fappend.ok", parentFn);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "fappend.merge", parentFn);
        builder->CreateCondBr(isNull, nullBB, okBB);

        builder->SetInsertPoint(nullBB);
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(okBB);
        llvm::Value* slen = builder->CreateCall(getOrDeclareStrlen(), {contentPtr}, "fappend.len");
        builder->CreateCall(getOrDeclareFwrite(),
            {contentPtr, llvm::ConstantInt::get(getDefaultType(), 1), slen, fp});
        builder->CreateCall(getOrDeclareFclose(), {fp});
        builder->CreateBr(mergeBB);

        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(getDefaultType(), 2, "fappend.result");
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 1), nullBB);
        phi->addIncoming(llvm::ConstantInt::get(getDefaultType(), 0), okBB);
        return phi;
    }

    if (expr->callee == "file_exists") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'file_exists' expects 1 argument (path)", expr);
        }
        llvm::Value* pathArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* pathPtr = pathArg->getType()->isPointerTy()
            ? pathArg : builder->CreateIntToPtr(pathArg, ptrTy, "fexists.path");
        // access(path, F_OK=0) returns 0 on success, -1 on failure
        llvm::Value* result = builder->CreateCall(getOrDeclareAccess(),
            {pathPtr, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0)}, "fexists.access");
        llvm::Value* isZero = builder->CreateICmpEQ(result,
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0), "fexists.cmp");
        return builder->CreateZExt(isZero, getDefaultType(), "fexists.result");
    }

    // -----------------------------------------------------------------------
    // Map/Dictionary built-ins
    // -----------------------------------------------------------------------

    if (expr->callee == "map_new") {
        if (expr->arguments.size() != 0) {
            codegenError("Built-in function 'map_new' expects 0 arguments", expr);
        }
        // Allocate array with just the length header = 0
        llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 8);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "mapnew.buf");
        // Store length = 0
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), buf);
        return builder->CreatePtrToInt(buf, getDefaultType(), "mapnew.result");
    }

    if (expr->callee == "map_set") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'map_set' expects 3 arguments (map, key, value)", expr);
        }
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        llvm::Value* valArg = generateExpression(expr->arguments[2].get());
        mapArg = toDefaultType(mapArg);
        keyArg = toDefaultType(keyArg);
        valArg = toDefaultType(valArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = builder->CreateIntToPtr(mapArg, ptrTy, "mapset.ptr");
        llvm::Value* mapLen = builder->CreateLoad(getDefaultType(), mapPtr, "mapset.len");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "mapset.loop", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "mapset.body", parentFn);
        llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(*context, "mapset.found", parentFn);
        llvm::BasicBlock* nextBB = llvm::BasicBlock::Create(*context, "mapset.next", parentFn);
        llvm::BasicBlock* appendBB = llvm::BasicBlock::Create(*context, "mapset.append", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "mapset.done", parentFn);

        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "mapset.idx");
        idx->addIncoming(zero, entryBB);
        llvm::Value* cond = builder->CreateICmpSLT(idx, mapLen, "mapset.cond");
        builder->CreateCondBr(cond, bodyBB, appendBB);

        // Body: check if key matches
        builder->SetInsertPoint(bodyBB);
        llvm::Value* keySlot = builder->CreateAdd(idx, one, "mapset.keyslot");
        llvm::Value* keyPtr = builder->CreateGEP(getDefaultType(), mapPtr, keySlot, "mapset.keyptr");
        llvm::Value* existingKey = builder->CreateLoad(getDefaultType(), keyPtr, "mapset.existkey");
        llvm::Value* keyMatch = builder->CreateICmpEQ(existingKey, keyArg, "mapset.match");
        builder->CreateCondBr(keyMatch, foundBB, nextBB);

        // Next: increment and loop back
        builder->SetInsertPoint(nextBB);
        llvm::Value* nextIdx = builder->CreateAdd(idx, two, "mapset.next");
        idx->addIncoming(nextIdx, nextBB);
        builder->CreateBr(loopBB);

        // Found: update value in place
        builder->SetInsertPoint(foundBB);
        llvm::Value* valSlot = builder->CreateAdd(idx, two, "mapset.valslot");
        llvm::Value* valPtr = builder->CreateGEP(getDefaultType(), mapPtr, valSlot, "mapset.valptr");
        builder->CreateStore(valArg, valPtr);
        builder->CreateBr(doneBB);

        // Append: allocate new buffer with 2 more slots
        builder->SetInsertPoint(appendBB);
        llvm::Value* newLen = builder->CreateAdd(mapLen, two, "mapset.newlen");
        llvm::Value* newSlots = builder->CreateAdd(newLen, one, "mapset.newslots");
        llvm::Value* newSize = builder->CreateMul(newSlots, eight, "mapset.newsize");
        llvm::Value* newBuf = builder->CreateCall(getOrDeclareMalloc(), {newSize}, "mapset.newbuf");
        // Copy old data
        llvm::Value* oldSlots = builder->CreateAdd(mapLen, one, "mapset.oldslots");
        llvm::Value* oldSize = builder->CreateMul(oldSlots, eight, "mapset.oldsize");
        builder->CreateCall(getOrDeclareMemcpy(), {newBuf, mapPtr, oldSize});
        // Store new length
        builder->CreateStore(newLen, newBuf);
        // Store new key at [mapLen + 1]
        llvm::Value* newKeySlot = builder->CreateAdd(mapLen, one, "mapset.nkeyslot");
        llvm::Value* newKeyPtr = builder->CreateGEP(getDefaultType(), newBuf, newKeySlot, "mapset.nkeyptr");
        builder->CreateStore(keyArg, newKeyPtr);
        // Store new value at [mapLen + 2]
        llvm::Value* newValSlot = builder->CreateAdd(mapLen, two, "mapset.nvalslot");
        llvm::Value* newValPtr = builder->CreateGEP(getDefaultType(), newBuf, newValSlot, "mapset.nvalptr");
        builder->CreateStore(valArg, newValPtr);
        llvm::Value* appendResult = builder->CreatePtrToInt(newBuf, getDefaultType(), "mapset.appendres");
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "mapset.result");
        result->addIncoming(mapArg, foundBB);
        result->addIncoming(appendResult, appendBB);
        return result;
    }

    if (expr->callee == "map_get") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'map_get' expects 3 arguments (map, key, default)", expr);
        }
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        llvm::Value* defArg = generateExpression(expr->arguments[2].get());
        mapArg = toDefaultType(mapArg);
        keyArg = toDefaultType(keyArg);
        defArg = toDefaultType(defArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = builder->CreateIntToPtr(mapArg, ptrTy, "mapget.ptr");
        llvm::Value* mapLen = builder->CreateLoad(getDefaultType(), mapPtr, "mapget.len");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "mapget.loop", parentFn);
        llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(*context, "mapget.check", parentFn);
        llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(*context, "mapget.found", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "mapget.done", parentFn);

        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "mapget.idx");
        idx->addIncoming(zero, entryBB);
        llvm::Value* cond = builder->CreateICmpSLT(idx, mapLen, "mapget.cond");
        builder->CreateCondBr(cond, checkBB, doneBB);

        builder->SetInsertPoint(checkBB);
        llvm::Value* keySlot = builder->CreateAdd(idx, one, "mapget.keyslot");
        llvm::Value* keyPtr = builder->CreateGEP(getDefaultType(), mapPtr, keySlot, "mapget.keyptr");
        llvm::Value* existingKey = builder->CreateLoad(getDefaultType(), keyPtr, "mapget.existkey");
        llvm::Value* keyMatch = builder->CreateICmpEQ(existingKey, keyArg, "mapget.match");
        llvm::Value* nextIdx = builder->CreateAdd(idx, two, "mapget.next");
        idx->addIncoming(nextIdx, checkBB);
        builder->CreateCondBr(keyMatch, foundBB, loopBB);

        builder->SetInsertPoint(foundBB);
        llvm::Value* valSlot = builder->CreateAdd(idx, two, "mapget.valslot");
        llvm::Value* valPtr = builder->CreateGEP(getDefaultType(), mapPtr, valSlot, "mapget.valptr");
        llvm::Value* foundVal = builder->CreateLoad(getDefaultType(), valPtr, "mapget.val");
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "mapget.result");
        result->addIncoming(defArg, loopBB);
        result->addIncoming(foundVal, foundBB);
        return result;
    }

    if (expr->callee == "map_has") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'map_has' expects 2 arguments (map, key)", expr);
        }
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        mapArg = toDefaultType(mapArg);
        keyArg = toDefaultType(keyArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = builder->CreateIntToPtr(mapArg, ptrTy, "maphas.ptr");
        llvm::Value* mapLen = builder->CreateLoad(getDefaultType(), mapPtr, "maphas.len");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "maphas.loop", parentFn);
        llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(*context, "maphas.check", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "maphas.done", parentFn);

        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "maphas.idx");
        idx->addIncoming(zero, entryBB);
        llvm::Value* cond = builder->CreateICmpSLT(idx, mapLen, "maphas.cond");
        builder->CreateCondBr(cond, checkBB, doneBB);

        builder->SetInsertPoint(checkBB);
        llvm::Value* keySlot = builder->CreateAdd(idx, one, "maphas.keyslot");
        llvm::Value* keyPtr = builder->CreateGEP(getDefaultType(), mapPtr, keySlot, "maphas.keyptr");
        llvm::Value* existingKey = builder->CreateLoad(getDefaultType(), keyPtr, "maphas.existkey");
        llvm::Value* keyMatch = builder->CreateICmpEQ(existingKey, keyArg, "maphas.match");
        llvm::Value* nextIdx = builder->CreateAdd(idx, two, "maphas.next");
        idx->addIncoming(nextIdx, checkBB);
        // If found, go to done with 1; otherwise loop
        llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(*context, "maphas.found", parentFn);
        builder->CreateCondBr(keyMatch, foundBB, loopBB);

        builder->SetInsertPoint(foundBB);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "maphas.result");
        result->addIncoming(zero, loopBB);
        result->addIncoming(one, foundBB);
        return result;
    }

    if (expr->callee == "map_remove") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'map_remove' expects 2 arguments (map, key)", expr);
        }
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        llvm::Value* keyArg = generateExpression(expr->arguments[1].get());
        mapArg = toDefaultType(mapArg);
        keyArg = toDefaultType(keyArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = builder->CreateIntToPtr(mapArg, ptrTy, "maprem.ptr");
        llvm::Value* mapLen = builder->CreateLoad(getDefaultType(), mapPtr, "maprem.len");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "maprem.loop", parentFn);
        llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(*context, "maprem.check", parentFn);
        llvm::BasicBlock* foundBB = llvm::BasicBlock::Create(*context, "maprem.found", parentFn);
        llvm::BasicBlock* notFoundBB = llvm::BasicBlock::Create(*context, "maprem.notfound", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "maprem.done", parentFn);

        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "maprem.idx");
        idx->addIncoming(zero, entryBB);
        llvm::Value* cond = builder->CreateICmpSLT(idx, mapLen, "maprem.cond");
        builder->CreateCondBr(cond, checkBB, notFoundBB);

        builder->SetInsertPoint(checkBB);
        llvm::Value* keySlot = builder->CreateAdd(idx, one, "maprem.keyslot");
        llvm::Value* keyPtr = builder->CreateGEP(getDefaultType(), mapPtr, keySlot, "maprem.keyptr");
        llvm::Value* existingKey = builder->CreateLoad(getDefaultType(), keyPtr, "maprem.existkey");
        llvm::Value* keyMatch = builder->CreateICmpEQ(existingKey, keyArg, "maprem.match");
        llvm::Value* nextIdx = builder->CreateAdd(idx, two, "maprem.next");
        idx->addIncoming(nextIdx, checkBB);
        builder->CreateCondBr(keyMatch, foundBB, loopBB);

        // Not found: return original map
        builder->SetInsertPoint(notFoundBB);
        builder->CreateBr(doneBB);

        // Found: build new array without this pair
        builder->SetInsertPoint(foundBB);
        llvm::Value* newLen = builder->CreateSub(mapLen, two, "maprem.newlen");
        llvm::Value* newSlots = builder->CreateAdd(newLen, one, "maprem.newslots");
        llvm::Value* newSize = builder->CreateMul(newSlots, eight, "maprem.newsize");
        llvm::Value* newBuf = builder->CreateCall(getOrDeclareMalloc(), {newSize}, "maprem.newbuf");
        builder->CreateStore(newLen, newBuf);

        // Copy elements before the removed pair: slots 1..idx (idx elements)
        llvm::Value* beforeBytes = builder->CreateMul(idx, eight, "maprem.beforebytes");
        llvm::Value* hasBefore = builder->CreateICmpSGT(idx, zero, "maprem.hasbefore");

        llvm::BasicBlock* copyBeforeBB = llvm::BasicBlock::Create(*context, "maprem.copybefore", parentFn);
        llvm::BasicBlock* afterCopyBeforeBB = llvm::BasicBlock::Create(*context, "maprem.afterbefore", parentFn);
        builder->CreateCondBr(hasBefore, copyBeforeBB, afterCopyBeforeBB);

        builder->SetInsertPoint(copyBeforeBB);
        llvm::Value* srcBefore = builder->CreateGEP(getDefaultType(), mapPtr, one, "maprem.srcbefore");
        llvm::Value* dstBefore = builder->CreateGEP(getDefaultType(), newBuf, one, "maprem.dstbefore");
        builder->CreateCall(getOrDeclareMemcpy(), {dstBefore, srcBefore, beforeBytes});
        builder->CreateBr(afterCopyBeforeBB);

        builder->SetInsertPoint(afterCopyBeforeBB);
        // Copy elements after the removed pair
        llvm::Value* afterStart = builder->CreateAdd(idx, two, "maprem.afterstart");
        llvm::Value* afterCount = builder->CreateSub(mapLen, afterStart, "maprem.aftercount");
        llvm::Value* hasAfter = builder->CreateICmpSGT(afterCount, zero, "maprem.hasafter");

        llvm::BasicBlock* copyAfterBB = llvm::BasicBlock::Create(*context, "maprem.copyafter", parentFn);
        llvm::BasicBlock* afterCopyAfterBB = llvm::BasicBlock::Create(*context, "maprem.afterafter", parentFn);
        builder->CreateCondBr(hasAfter, copyAfterBB, afterCopyAfterBB);

        builder->SetInsertPoint(copyAfterBB);
        llvm::Value* srcAfterSlot = builder->CreateAdd(afterStart, one, "maprem.srcafterslot");
        llvm::Value* srcAfter = builder->CreateGEP(getDefaultType(), mapPtr, srcAfterSlot, "maprem.srcafter");
        llvm::Value* dstAfterSlot = builder->CreateAdd(idx, one, "maprem.dstafterslot");
        llvm::Value* dstAfter = builder->CreateGEP(getDefaultType(), newBuf, dstAfterSlot, "maprem.dstafter");
        llvm::Value* afterBytes = builder->CreateMul(afterCount, eight, "maprem.afterbytes");
        builder->CreateCall(getOrDeclareMemcpy(), {dstAfter, srcAfter, afterBytes});
        builder->CreateBr(afterCopyAfterBB);

        builder->SetInsertPoint(afterCopyAfterBB);
        llvm::Value* foundResult = builder->CreatePtrToInt(newBuf, getDefaultType(), "maprem.foundres");
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "maprem.result");
        result->addIncoming(mapArg, notFoundBB);
        result->addIncoming(foundResult, afterCopyAfterBB);
        return result;
    }

    if (expr->callee == "map_keys") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'map_keys' expects 1 argument (map)", expr);
        }
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        mapArg = toDefaultType(mapArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = builder->CreateIntToPtr(mapArg, ptrTy, "mapkeys.ptr");
        llvm::Value* mapLen = builder->CreateLoad(getDefaultType(), mapPtr, "mapkeys.len");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Number of pairs = mapLen / 2
        llvm::Value* numPairs = builder->CreateSDiv(mapLen, two, "mapkeys.numpairs");
        // Allocate: (numPairs + 1) * 8
        llvm::Value* arrSlots = builder->CreateAdd(numPairs, one, "mapkeys.arrslots");
        llvm::Value* arrSize = builder->CreateMul(arrSlots, eight, "mapkeys.arrsize");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrSize}, "mapkeys.buf");
        builder->CreateStore(numPairs, buf);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "mapkeys.loop", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "mapkeys.body", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "mapkeys.done", parentFn);

        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* i = builder->CreatePHI(getDefaultType(), 2, "mapkeys.i");
        i->addIncoming(zero, entryBB);
        llvm::Value* cond = builder->CreateICmpSLT(i, numPairs, "mapkeys.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        // Key is at map slot: i*2 + 1
        llvm::Value* mapSlot = builder->CreateAdd(builder->CreateMul(i, two, "mapkeys.mi"), one, "mapkeys.mapslot");
        llvm::Value* keyPtr = builder->CreateGEP(getDefaultType(), mapPtr, mapSlot, "mapkeys.keyptr");
        llvm::Value* keyVal = builder->CreateLoad(getDefaultType(), keyPtr, "mapkeys.key");
        // Store at buf slot: i + 1
        llvm::Value* arrSlot = builder->CreateAdd(i, one, "mapkeys.arrslot");
        llvm::Value* arrElemPtr = builder->CreateGEP(getDefaultType(), buf, arrSlot, "mapkeys.arrptr");
        builder->CreateStore(keyVal, arrElemPtr);
        llvm::Value* nextI = builder->CreateAdd(i, one, "mapkeys.next");
        i->addIncoming(nextI, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "mapkeys.result");
    }

    if (expr->callee == "map_values") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'map_values' expects 1 argument (map)", expr);
        }
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        mapArg = toDefaultType(mapArg);

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = builder->CreateIntToPtr(mapArg, ptrTy, "mapvals.ptr");
        llvm::Value* mapLen = builder->CreateLoad(getDefaultType(), mapPtr, "mapvals.len");
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* two = llvm::ConstantInt::get(getDefaultType(), 2);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        llvm::Value* numPairs = builder->CreateSDiv(mapLen, two, "mapvals.numpairs");
        llvm::Value* arrSlots = builder->CreateAdd(numPairs, one, "mapvals.arrslots");
        llvm::Value* arrSize = builder->CreateMul(arrSlots, eight, "mapvals.arrsize");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrSize}, "mapvals.buf");
        builder->CreateStore(numPairs, buf);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "mapvals.loop", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "mapvals.body", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "mapvals.done", parentFn);

        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* i = builder->CreatePHI(getDefaultType(), 2, "mapvals.i");
        i->addIncoming(zero, entryBB);
        llvm::Value* cond = builder->CreateICmpSLT(i, numPairs, "mapvals.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        // Value is at map slot: i*2 + 2
        llvm::Value* mapSlot = builder->CreateAdd(builder->CreateMul(i, two, "mapvals.mi"), two, "mapvals.mapslot");
        llvm::Value* valPtr = builder->CreateGEP(getDefaultType(), mapPtr, mapSlot, "mapvals.valptr");
        llvm::Value* valVal = builder->CreateLoad(getDefaultType(), valPtr, "mapvals.val");
        llvm::Value* arrSlot = builder->CreateAdd(i, one, "mapvals.arrslot");
        llvm::Value* arrElemPtr = builder->CreateGEP(getDefaultType(), buf, arrSlot, "mapvals.arrptr");
        builder->CreateStore(valVal, arrElemPtr);
        llvm::Value* nextI = builder->CreateAdd(i, one, "mapvals.next");
        i->addIncoming(nextI, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "mapvals.result");
    }

    if (expr->callee == "map_size") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'map_size' expects 1 argument (map)", expr);
        }
        llvm::Value* mapArg = generateExpression(expr->arguments[0].get());
        mapArg = toDefaultType(mapArg);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mapPtr = builder->CreateIntToPtr(mapArg, ptrTy, "mapsize.ptr");
        llvm::Value* mapLen = builder->CreateLoad(getDefaultType(), mapPtr, "mapsize.len");
        // Number of pairs = mapLen / 2
        return builder->CreateSDiv(mapLen,
            llvm::ConstantInt::get(getDefaultType(), 2), "mapsize.result");
    }

    // -----------------------------------------------------------------------
    // Range and utility built-ins
    // -----------------------------------------------------------------------

    if (expr->callee == "range") {
        if (expr->arguments.size() != 2) {
            codegenError("Built-in function 'range' expects 2 arguments (start, end)", expr);
        }
        llvm::Value* startArg = generateExpression(expr->arguments[0].get());
        llvm::Value* endArg = generateExpression(expr->arguments[1].get());
        startArg = toDefaultType(startArg);
        endArg = toDefaultType(endArg);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // count = max(end - start, 0)
        llvm::Value* diff = builder->CreateSub(endArg, startArg, "range.diff");
        llvm::Value* isPos = builder->CreateICmpSGT(diff, zero, "range.ispos");
        llvm::Value* count = builder->CreateSelect(isPos, diff, zero, "range.count");

        // Allocate: (count + 1) * 8
        llvm::Value* arrSlots = builder->CreateAdd(count, one, "range.arrslots");
        llvm::Value* arrSize = builder->CreateMul(arrSlots, eight, "range.arrsize");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrSize}, "range.buf");
        builder->CreateStore(count, buf);

        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "range.loop", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "range.body", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "range.done", parentFn);

        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* i = builder->CreatePHI(getDefaultType(), 2, "range.i");
        i->addIncoming(zero, entryBB);
        llvm::Value* cond = builder->CreateICmpSLT(i, count, "range.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* val = builder->CreateAdd(startArg, i, "range.val");
        llvm::Value* slot = builder->CreateAdd(i, one, "range.slot");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), buf, slot, "range.elemptr");
        builder->CreateStore(val, elemPtr);
        llvm::Value* nextI = builder->CreateAdd(i, one, "range.next");
        i->addIncoming(nextI, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "range.result");
    }

    if (expr->callee == "range_step") {
        if (expr->arguments.size() != 3) {
            codegenError("Built-in function 'range_step' expects 3 arguments (start, end, step)", expr);
        }
        llvm::Value* startArg = generateExpression(expr->arguments[0].get());
        llvm::Value* endArg = generateExpression(expr->arguments[1].get());
        llvm::Value* stepArg = generateExpression(expr->arguments[2].get());
        startArg = toDefaultType(startArg);
        endArg = toDefaultType(endArg);
        stepArg = toDefaultType(stepArg);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);

        // Guard: step must not be 0 (division by zero).
        llvm::Value* stepIsZero = builder->CreateICmpEQ(stepArg, zero, "rstep.stepzero");
        llvm::Function* parentFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* stepOkBB = llvm::BasicBlock::Create(*context, "rstep.stepok", parentFn);
        llvm::BasicBlock* stepFailBB = llvm::BasicBlock::Create(*context, "rstep.stepfail", parentFn);
        builder->CreateCondBr(stepIsZero, stepFailBB, stepOkBB);

        builder->SetInsertPoint(stepFailBB);
        llvm::Value* errMsg = builder->CreateGlobalString(
            "Runtime error: range step cannot be zero\n", "rstep_zero_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(stepOkBB);

        // count = max((end - start + step - 1) / step, 0) for positive step
        // Simplified: count = max(0, (end - start + step - sign) / step)
        // Use a safe approach: (end - start) / step, clamped to 0
        llvm::Value* diff = builder->CreateSub(endArg, startArg, "rstep.diff");
        // For positive step: count = (diff + step - 1) / step if diff > 0
        llvm::Value* adjDiff = builder->CreateAdd(diff,
            builder->CreateSub(stepArg, one, "rstep.stepm1"), "rstep.adjdiff");
        llvm::Value* count = builder->CreateSDiv(adjDiff, stepArg, "rstep.count");
        llvm::Value* isPos = builder->CreateICmpSGT(count, zero, "rstep.ispos");
        count = builder->CreateSelect(isPos, count, zero, "rstep.clampcount");

        llvm::Value* arrSlots = builder->CreateAdd(count, one, "rstep.arrslots");
        llvm::Value* arrSize = builder->CreateMul(arrSlots, eight, "rstep.arrsize");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {arrSize}, "rstep.buf");
        builder->CreateStore(count, buf);

        llvm::BasicBlock* entryBB = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "rstep.loop", parentFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "rstep.body", parentFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "rstep.done", parentFn);

        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* i = builder->CreatePHI(getDefaultType(), 2, "rstep.i");
        i->addIncoming(zero, entryBB);
        llvm::Value* cond = builder->CreateICmpSLT(i, count, "rstep.cond");
        builder->CreateCondBr(cond, bodyBB, doneBB);

        builder->SetInsertPoint(bodyBB);
        llvm::Value* val = builder->CreateAdd(startArg,
            builder->CreateMul(i, stepArg, "rstep.offset"), "rstep.val");
        llvm::Value* slot = builder->CreateAdd(i, one, "rstep.slot");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), buf, slot, "rstep.elemptr");
        builder->CreateStore(val, elemPtr);
        llvm::Value* nextI = builder->CreateAdd(i, one, "rstep.next");
        i->addIncoming(nextI, bodyBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        return builder->CreatePtrToInt(buf, getDefaultType(), "rstep.result");
    }

    if (expr->callee == "char_code") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'char_code' expects 1 argument (string)", expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
            ? strArg : builder->CreateIntToPtr(strArg, ptrTy, "charcode.ptr");
        llvm::Value* ch = builder->CreateLoad(llvm::Type::getInt8Ty(*context), strPtr, "charcode.ch");
        return builder->CreateZExt(ch, getDefaultType(), "charcode.result");
    }

    if (expr->callee == "number_to_string") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'number_to_string' expects 1 argument", expr);
        }
        llvm::Value* val = generateExpression(expr->arguments[0].get());
        bool isFloat = val->getType()->isDoubleTy();
        if (!isFloat)
            val = toDefaultType(val);
        if (isFloat) {
            llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 32);
            llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "numtostr.buf");
            llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_float_fmt", true);
            if (!fmtStr)
                fmtStr = builder->CreateGlobalString("%g", "tostr_float_fmt");
            builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmtStr, val});
            stringReturningFunctions_.insert("number_to_string");
            return buf;
        }
        llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 21);
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "numtostr.buf");
        llvm::GlobalVariable* fmtStr = module->getGlobalVariable("tostr_fmt", true);
        if (!fmtStr)
            fmtStr = builder->CreateGlobalString("%lld", "tostr_fmt");
        builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmtStr, val});
        stringReturningFunctions_.insert("number_to_string");
        return buf;
    }

    if (expr->callee == "string_to_number") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'string_to_number' expects 1 argument (string)", expr);
        }
        llvm::Value* strArg = generateExpression(expr->arguments[0].get());
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strPtr = strArg->getType()->isPointerTy()
            ? strArg : builder->CreateIntToPtr(strArg, ptrTy, "strtonum.ptr");
        // Use strtoll to parse as integer first
        llvm::Value* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* result = builder->CreateCall(getOrDeclareStrtoll(),
            {strPtr, nullPtr, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 10)}, "strtonum.result");
        return result;
    }

    // -----------------------------------------------------------------------
    // Concurrency primitives (pthreads)
    // -----------------------------------------------------------------------

    if (expr->callee == "thread_create") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'thread_create' expects 1 argument (function name as string), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        // thread_create("func_name") — look up the function by name and call pthread_create
        auto* ptrTy = llvm::PointerType::getUnqual(*context);

        // The argument should be a string containing a function name.
        // We look up the function directly by checking if the argument is a literal.
        auto* litArg = dynamic_cast<LiteralExpr*>(expr->arguments[0].get());
        if (!litArg || litArg->literalType != LiteralExpr::LiteralType::STRING) {
            codegenError("thread_create requires a string literal function name", expr);
        }
        auto fnIt = functions.find(litArg->stringValue);
        if (fnIt == functions.end() || !fnIt->second) {
            codegenError("thread_create: unknown function '" + litArg->stringValue + "'", expr);
        }
        llvm::Function* targetFunc = fnIt->second;

        // Generate a wrapper function: void* __thread_wrapper_<name>(void* arg) { target(); return NULL; }
        std::string wrapperName = "__thread_wrapper_" + litArg->stringValue;
        llvm::Function* wrapper = module->getFunction(wrapperName);
        if (!wrapper) {
            auto* wrapperType = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
            wrapper = llvm::Function::Create(wrapperType, llvm::Function::InternalLinkage,
                                             wrapperName, module.get());
            auto* savedBB = builder->GetInsertBlock();
            auto* savedPoint = builder->GetInsertPoint() != builder->GetInsertBlock()->end()
                                    ? &*builder->GetInsertPoint() : nullptr;
            auto* entry = llvm::BasicBlock::Create(*context, "entry", wrapper);
            builder->SetInsertPoint(entry);
            builder->CreateCall(targetFunc);
            builder->CreateRet(llvm::ConstantPointerNull::get(ptrTy));
            if (savedPoint) {
                builder->SetInsertPoint(savedBB, llvm::BasicBlock::iterator(savedPoint));
            } else {
                builder->SetInsertPoint(savedBB);
            }
        }

        // Allocate pthread_t on the stack (8 bytes = i64)
        auto* parentFunc = builder->GetInsertBlock()->getParent();
        auto* tidAlloca = createEntryBlockAlloca(parentFunc, "tid");
        llvm::Value* nullAttr = llvm::ConstantPointerNull::get(ptrTy);
        llvm::Value* nullArg = llvm::ConstantPointerNull::get(ptrTy);

        builder->CreateCall(getOrDeclarePthreadCreate(),
                            {tidAlloca, nullAttr, wrapper, nullArg});

        // Return the thread id
        return builder->CreateLoad(getDefaultType(), tidAlloca, "tid.val");
    }

    if (expr->callee == "thread_join") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'thread_join' expects 1 argument (thread id), but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* tid = generateExpression(expr->arguments[0].get());
        tid = toDefaultType(tid);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nullRetval = llvm::ConstantPointerNull::get(ptrTy);
        builder->CreateCall(getOrDeclarePthreadJoin(), {tid, nullRetval});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (expr->callee == "mutex_new") {
        if (expr->arguments.size() != 0) {
            codegenError("Built-in function 'mutex_new' expects 0 arguments, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        // Allocate space for pthread_mutex_t.  The struct size varies by
        // platform (40 bytes on Linux x86_64, 64 on macOS).  We allocate a
        // generous fixed size that covers all common platforms.
        static constexpr int64_t kMutexAllocSize = 64;
        llvm::Value* size = llvm::ConstantInt::get(getDefaultType(), kMutexAllocSize);
        llvm::Value* mutex = builder->CreateCall(getOrDeclareMalloc(), {size}, "mutex.ptr");
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* nullAttr = llvm::ConstantPointerNull::get(ptrTy);
        builder->CreateCall(getOrDeclarePthreadMutexInit(), {mutex, nullAttr});
        // Return as i64
        return builder->CreatePtrToInt(mutex, getDefaultType(), "mutex.val");
    }

    if (expr->callee == "mutex_lock") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'mutex_lock' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* mutexVal = generateExpression(expr->arguments[0].get());
        mutexVal = toDefaultType(mutexVal);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mutexPtr = builder->CreateIntToPtr(mutexVal, ptrTy, "mutex.ptr");
        builder->CreateCall(getOrDeclarePthreadMutexLock(), {mutexPtr});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (expr->callee == "mutex_unlock") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'mutex_unlock' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* mutexVal = generateExpression(expr->arguments[0].get());
        mutexVal = toDefaultType(mutexVal);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mutexPtr = builder->CreateIntToPtr(mutexVal, ptrTy, "mutex.ptr");
        builder->CreateCall(getOrDeclarePthreadMutexUnlock(), {mutexPtr});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (expr->callee == "mutex_destroy") {
        if (expr->arguments.size() != 1) {
            codegenError("Built-in function 'mutex_destroy' expects 1 argument, but " +
                             std::to_string(expr->arguments.size()) + " provided",
                         expr);
        }
        llvm::Value* mutexVal = generateExpression(expr->arguments[0].get());
        mutexVal = toDefaultType(mutexVal);
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* mutexPtr = builder->CreateIntToPtr(mutexVal, ptrTy, "mutex.ptr");
        builder->CreateCall(getOrDeclarePthreadMutexDestroy(), {mutexPtr});
        builder->CreateCall(getOrDeclareFree(), {mutexPtr});
        return llvm::ConstantInt::get(getDefaultType(), 0);
    }

    if (inOptMaxFunction) {
        // Stdlib functions are always native machine code, so they're safe to call from OPTMAX
        if (!isStdlibFunction(expr->callee) && optMaxFunctions.find(expr->callee) == optMaxFunctions.end()) {
            std::string currentFunction = "<unknown>";
            if (builder->GetInsertBlock() && builder->GetInsertBlock()->getParent()) {
                currentFunction = std::string(builder->GetInsertBlock()->getParent()->getName());
            }
            codegenError("OPTMAX function \"" + currentFunction + "\" cannot invoke non-OPTMAX function \"" +
                             expr->callee + "\"",
                         expr);
        }
    }
    auto calleeIt = functions.find(expr->callee);
    if (calleeIt == functions.end() || !calleeIt->second) {
        // Build "did you mean?" suggestion from known functions.
        std::string msg = "Unknown function: " + expr->callee;
        std::vector<std::string> candidates;
        candidates.reserve(functions.size());
        for (const auto& kv : functions) {
            if (kv.second)
                candidates.push_back(kv.first);
        }
        std::string suggestion = suggestSimilar(expr->callee, candidates);
        if (!suggestion.empty()) {
            msg += " (did you mean '" + suggestion + "'?)";
        }
        codegenError(msg, expr);
    }
    llvm::Function* callee = calleeIt->second;

    auto declIt = functionDecls_.find(expr->callee);
    size_t requiredArgs = callee->arg_size();
    if (declIt != functionDecls_.end()) {
        requiredArgs = declIt->second->requiredParameters();
    }
    if (expr->arguments.size() < requiredArgs || expr->arguments.size() > callee->arg_size()) {
        codegenError("Function '" + expr->callee + "' expects " +
                         (requiredArgs < callee->arg_size()
                              ? std::to_string(requiredArgs) + " to " + std::to_string(callee->arg_size())
                              : std::to_string(callee->arg_size())) +
                         " argument(s), but " + std::to_string(expr->arguments.size()) + " provided",
                     expr);
    }

    std::vector<llvm::Value*> args;
    for (size_t i = 0; i < callee->arg_size(); ++i) {
        if (i < expr->arguments.size()) {
            llvm::Value* argVal = generateExpression(expr->arguments[i].get());
            // Function parameters are i64, convert if needed
            argVal = toDefaultType(argVal);
            args.push_back(argVal);
        } else if (declIt != functionDecls_.end()) {
            auto& param = declIt->second->parameters[i];
            if (param.defaultValue) {
                llvm::Value* argVal = generateExpression(param.defaultValue.get());
                argVal = toDefaultType(argVal);
                args.push_back(argVal);
            }
        }
    }

    return builder->CreateCall(callee, args, "calltmp");
}

} // namespace omscript
