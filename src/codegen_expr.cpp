#include "codegen.h"
#include "diagnostic.h"
#include <climits>
#include <cstdint>
#include <iostream>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <stdexcept>

// LLVM 19 introduced getOrInsertDeclaration; older versions only have getDeclaration.
#if LLVM_VERSION_MAJOR >= 19
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getOrInsertDeclaration
#else
#define OMSC_GET_INTRINSIC llvm::Intrinsic::getDeclaration
#endif

namespace {

/// Returns the log2 of a positive power-of-two value, or -1 if the value is
/// not a power of two (or is non-positive).  Used by strength-reduction passes
/// to convert multiply/divide/modulo by a constant power of 2 into cheaper
/// shift or bitwise-AND operations.
inline int log2IfPowerOf2(int64_t val) {
    if (val <= 0 || (val & (val - 1)) != 0)
        return -1;
    int shift = 0;
    while (val > 1) {
        val >>= 1;
        shift++;
    }
    return shift;
}

} // namespace

namespace omscript {

llvm::Value* CodeGenerator::generateLiteral(LiteralExpr* expr) {
    if (expr->literalType == LiteralExpr::LiteralType::INTEGER) {
        return llvm::ConstantInt::get(*context, llvm::APInt(64, expr->intValue));
    } else if (expr->literalType == LiteralExpr::LiteralType::FLOAT) {
        return llvm::ConstantFP::get(getFloatType(), expr->floatValue);
    } else {
        // String literal - return as a pointer to the global string data.
        // When passed directly to print(), the pointer form is used with %s.
        return builder->CreateGlobalString(expr->stringValue, "str");
    }
}

llvm::Value* CodeGenerator::generateIdentifier(IdentifierExpr* expr) {
    // Check for use-after-move or use-after-invalidate.
    auto deadIt = deadVars_.find(expr->name);
    if (deadIt != deadVars_.end()) {
        auto reasonIt = deadVarReason_.find(expr->name);
        std::string reason = (reasonIt != deadVarReason_.end()) ? reasonIt->second : "moved or invalidated";
        codegenError("Use of " + reason + " variable '" + expr->name + "'", expr);
    }

    auto it = namedValues.find(expr->name);
    if (it == namedValues.end() || !it->second) {
        // Check if this is an enum constant
        auto enumIt = enumConstants_.find(expr->name);
        if (enumIt != enumConstants_.end()) {
            return llvm::ConstantInt::get(getDefaultType(), enumIt->second);
        }
        // Build "did you mean?" suggestion from known variables.
        std::string msg = "Unknown variable: " + expr->name;
        std::vector<std::string> candidates;
        candidates.reserve(namedValues.size());
        for (const auto& kv : namedValues) {
            if (kv.second)
                candidates.push_back(kv.first);
        }
        std::string suggestion = suggestSimilar(expr->name, candidates);
        if (!suggestion.empty()) {
            msg += " (did you mean '" + suggestion + "'?)";
        }
        codegenError(msg, expr);
    }
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second);
    llvm::Type* loadType = alloca ? alloca->getAllocatedType() : getDefaultType();
    auto* load = builder->CreateLoad(loadType, it->second, expr->name.c_str());

    // If this is a const variable, mark the load as invariant so LLVM knows
    // the value never changes and can hoist/CSE it aggressively.
    auto constIt = constValues.find(expr->name);
    if (constIt != constValues.end() && constIt->second) {
        if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(load)) {
            loadInst->setMetadata(llvm::LLVMContext::MD_invariant_load,
                                  llvm::MDNode::get(*context, {}));
        }
    }
    return load;
}

llvm::Value* CodeGenerator::generateBinary(BinaryExpr* expr) {
    // --- Compile-time string constant folding ---
    // When both operands of '+' are string literals, concatenate at compile time
    // to avoid runtime malloc+strcpy overhead.
    if (expr->op == "+") {
        auto* leftLit = dynamic_cast<LiteralExpr*>(expr->left.get());
        auto* rightLit = dynamic_cast<LiteralExpr*>(expr->right.get());
        if (leftLit && rightLit && leftLit->literalType == LiteralExpr::LiteralType::STRING &&
            rightLit->literalType == LiteralExpr::LiteralType::STRING) {
            std::string folded = leftLit->stringValue + rightLit->stringValue;
            return builder->CreateGlobalString(folded, "strfold");
        }
    }
    // --- End string constant folding ---

    llvm::Value* left = generateExpression(expr->left.get());
    if (expr->op == "&&" || expr->op == "||") {
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::Value* leftBool = toBool(left);
        llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(*context, "logic.rhs", function);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "logic.cont", function);
        if (expr->op == "&&") {
            builder->CreateCondBr(leftBool, rhsBB, mergeBB);
        } else {
            builder->CreateCondBr(leftBool, mergeBB, rhsBB);
        }
        llvm::BasicBlock* leftBB = builder->GetInsertBlock();
        builder->SetInsertPoint(rhsBB);
        llvm::Value* right = generateExpression(expr->right.get());
        llvm::Value* rightBool = toBool(right);
        builder->CreateBr(mergeBB);
        llvm::BasicBlock* rightBB = builder->GetInsertBlock();
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder->CreatePHI(llvm::Type::getInt1Ty(*context), 2, "bool_result");
        if (expr->op == "&&") {
            phi->addIncoming(llvm::ConstantInt::getFalse(*context), leftBB);
            phi->addIncoming(rightBool, rightBB);
        } else {
            phi->addIncoming(llvm::ConstantInt::getTrue(*context), leftBB);
            phi->addIncoming(rightBool, rightBB);
        }
        return builder->CreateZExt(phi, getDefaultType(), "booltmp");
    }

    // Null coalescing operator: x ?? y → x != 0 ? x : y (short-circuit)
    if (expr->op == "??") {
        left = toDefaultType(left);
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* isNonZero = builder->CreateICmpNE(left, zero, "coalesce.nz");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* nonZeroBB = llvm::BasicBlock::Create(*context, "coalesce.nonzero", function);
        llvm::BasicBlock* zeroBB = llvm::BasicBlock::Create(*context, "coalesce.zero", function);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "coalesce.merge", function);
        builder->CreateCondBr(isNonZero, nonZeroBB, zeroBB);
        builder->SetInsertPoint(nonZeroBB);
        builder->CreateBr(mergeBB);
        builder->SetInsertPoint(zeroBB);
        llvm::Value* right = generateExpression(expr->right.get());
        right = toDefaultType(right);
        builder->CreateBr(mergeBB);
        builder->SetInsertPoint(mergeBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 2, "coalesce.result");
        result->addIncoming(left, nonZeroBB);
        result->addIncoming(right, zeroBB);
        return result;
    }

    llvm::Value* right = generateExpression(expr->right.get());

    bool leftIsFloat = left->getType()->isDoubleTy();
    bool rightIsFloat = right->getType()->isDoubleTy();

    // Pre-compute string flags once, used by both the float-skip guard below
    // and the string concatenation block that follows.
    bool leftIsStr = left->getType()->isPointerTy() || isStringExpr(expr->left.get());
    bool rightIsStr = right->getType()->isPointerTy() || isStringExpr(expr->right.get());

    // Float operations path — but only when neither operand is a string.
    // When the '+' operator has one string and one float (e.g. "pi=" + 3.14),
    // the string-concatenation block below must handle it; otherwise
    // ensureFloat() would reinterpret the string pointer as an integer and
    // silently produce garbage.
    if ((leftIsFloat || rightIsFloat) && !(expr->op == "+" && (leftIsStr || rightIsStr))) {
        if (!leftIsFloat)
            left = ensureFloat(left);
        if (!rightIsFloat)
            right = ensureFloat(right);

        // Float constant folding — compute at compile time when both operands
        // are known constants, avoiding runtime FP instructions entirely.
        if (auto* lc = llvm::dyn_cast<llvm::ConstantFP>(left)) {
            if (auto* rc = llvm::dyn_cast<llvm::ConstantFP>(right)) {
                double lv = lc->getValueAPF().convertToDouble();
                double rv = rc->getValueAPF().convertToDouble();
                if (expr->op == "+")
                    return llvm::ConstantFP::get(getFloatType(), lv + rv);
                if (expr->op == "-")
                    return llvm::ConstantFP::get(getFloatType(), lv - rv);
                if (expr->op == "*")
                    return llvm::ConstantFP::get(getFloatType(), lv * rv);
                if (expr->op == "/" && rv != 0.0)
                    return llvm::ConstantFP::get(getFloatType(), lv / rv);
                if (expr->op == "==" || expr->op == "!=" || expr->op == "<" || expr->op == "<=" || expr->op == ">" ||
                    expr->op == ">=") {
                    int64_t result = 0;
                    if (expr->op == "==")
                        result = lv == rv ? 1 : 0;
                    else if (expr->op == "!=")
                        result = lv != rv ? 1 : 0;
                    else if (expr->op == "<")
                        result = lv < rv ? 1 : 0;
                    else if (expr->op == "<=")
                        result = lv <= rv ? 1 : 0;
                    else if (expr->op == ">")
                        result = lv > rv ? 1 : 0;
                    else if (expr->op == ">=")
                        result = lv >= rv ? 1 : 0;
                    return llvm::ConstantInt::get(getDefaultType(), result);
                }
            }
        }

        // Float algebraic identity optimizations — eliminate no-op float
        // operations when one operand is a known constant.  These supplement
        // LLVM's InstCombine by catching patterns at the IR emission level.
        if (auto* rc = llvm::dyn_cast<llvm::ConstantFP>(right)) {
            double rv = rc->getValueAPF().convertToDouble();
            if (rv == 0.0 && (expr->op == "+" || expr->op == "-"))
                return left;  // x+0.0, x-0.0 → x
            if (rv == 1.0 && (expr->op == "*" || expr->op == "/"))
                return left;  // x*1.0, x/1.0 → x
            // Note: x*0.0 → 0.0 is NOT valid under IEEE-754: NaN*0=NaN,
            // Inf*0=NaN, and (-x)*0 = -0.0 (not +0.0).  We leave this to
            // LLVM InstCombine with fast-math flags if the user opts in.
            if (rv == 2.0 && expr->op == "*")
                return builder->CreateFAdd(left, left, "fmul2"); // x*2.0 → x+x
            if (rv == 2.0 && expr->op == "**")
                return builder->CreateFMul(left, left, "fsq");   // x**2.0 → x*x
            if (rv == 0.0 && expr->op == "**")
                return llvm::ConstantFP::get(getFloatType(), 1.0); // x**0.0 → 1.0
            if (rv == 1.0 && expr->op == "**")
                return left;  // x**1.0 → x
            // Float division by power-of-2 constant → multiply by reciprocal.
            // x / 2.0 → x * 0.5, x / 4.0 → x * 0.25, etc.
            // Multiplication is faster than division on all modern CPUs (3-5 cyc
            // vs 15-25 cyc).  Only applies to exact powers of 2 where the
            // reciprocal is exactly representable in IEEE-754 (no rounding error).
            if (expr->op == "/" && rv != 0.0) {
                double abs_rv = rv < 0 ? -rv : rv;
                bool isPow2 = false;
                if (abs_rv >= 1.0 && abs_rv <= 4503599627370496.0) { // 2^52 max exact int in double
                    auto u = static_cast<uint64_t>(abs_rv);
                    // Verify the cast is exact (no truncation of fractional part)
                    isPow2 = (static_cast<double>(u) == abs_rv) && u > 0 && (u & (u - 1)) == 0;
                } else if (abs_rv > 0.0 && abs_rv < 1.0) {
                    double inv = 1.0 / abs_rv;
                    auto u = static_cast<uint64_t>(inv);
                    isPow2 = (static_cast<double>(u) == inv) && u > 0 && (u & (u - 1)) == 0;
                }
                if (isPow2) {
                    double recip = 1.0 / rv;
                    return builder->CreateFMul(left, llvm::ConstantFP::get(getFloatType(), recip), "fdivrecip");
                }
            }
        }
        if (auto* lc = llvm::dyn_cast<llvm::ConstantFP>(left)) {
            double lv = lc->getValueAPF().convertToDouble();
            if (lv == 0.0 && expr->op == "+")
                return right; // 0.0+x → x
            if (lv == 0.0 && expr->op == "-")
                return builder->CreateFNeg(right, "fnegtmp"); // 0.0-x → -x
            if (lv == 1.0 && expr->op == "*")
                return right; // 1.0*x → x
            // Note: 0.0*x → 0.0 is NOT valid under IEEE-754: 0*NaN=NaN,
            // 0*Inf=NaN, and 0*(-x) = -0.0 (not +0.0).
            if (lv == 2.0 && expr->op == "*")
                return builder->CreateFAdd(right, right, "fmul2"); // 2.0*x → x+x
        }

        if (expr->op == "+")
            return builder->CreateFAdd(left, right, "faddtmp");
        if (expr->op == "-")
            return builder->CreateFSub(left, right, "fsubtmp");
        if (expr->op == "*")
            return builder->CreateFMul(left, right, "fmultmp");
        if (expr->op == "/")
            return builder->CreateFDiv(left, right, "fdivtmp");
        if (expr->op == "%")
            return builder->CreateFRem(left, right, "fremtmp");

        if (expr->op == "==") {
            auto cmp = builder->CreateFCmpOEQ(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == "!=") {
            auto cmp = builder->CreateFCmpONE(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == "<") {
            auto cmp = builder->CreateFCmpOLT(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == "<=") {
            auto cmp = builder->CreateFCmpOLE(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == ">") {
            auto cmp = builder->CreateFCmpOGT(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == ">=") {
            auto cmp = builder->CreateFCmpOGE(left, right, "fcmptmp");
            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        }
        if (expr->op == "**") {
            // Float exponent specialization: emit cheaper inline sequences
            // for common small-integer and half-integer exponents.
            if (auto* rc = llvm::dyn_cast<llvm::ConstantFP>(right)) {
                double rv = rc->getValueAPF().convertToDouble();
                if (rv == 0.5) {
                    // x**0.5 → sqrt(x): single-instruction latency vs llvm.pow call
                    llvm::Function* sqrtFn =
                        OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sqrt, {llvm::Type::getDoubleTy(*context)});
                    return builder->CreateCall(sqrtFn, {left}, "fsqrt");
                }
                if (rv == 3.0) {
                    // x**3.0 → x*x*x  (2 fmuls vs pow call)
                    auto* sq = builder->CreateFMul(left, left, "fpow3.sq");
                    return builder->CreateFMul(sq, left, "fpow3");
                }
            }
            // General float exponentiation: use llvm.pow intrinsic
            llvm::Function* powFn =
                OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::pow, {llvm::Type::getDoubleTy(*context)});
            return builder->CreateCall(powFn, {left, right}, "fpowtmp");
        }

        codegenError("Invalid binary operator for float operands: " + expr->op, expr);
    }

    // String concatenation path: either operand is a string (ptr or i64-as-string).
    if (expr->op == "+") {
        if (leftIsStr || rightIsStr) {
            // Auto-convert non-string operand to string via snprintf (like to_string builtin).
            auto ensureStrPtr = [&](llvm::Value* val, bool isStr) -> llvm::Value* {
                if (isStr) {
                    if (!val->getType()->isPointerTy())
                        val = builder->CreateIntToPtr(val, llvm::PointerType::getUnqual(*context), "str.cast.ptr");
                    return val;
                }
                // Convert integer or float to string.
                llvm::Value* bufSize = llvm::ConstantInt::get(getDefaultType(), 32);
                llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bufSize}, "autostr.buf");
                if (val->getType()->isDoubleTy()) {
                    llvm::GlobalVariable* fmt = module->getGlobalVariable("autostr_float_fmt", true);
                    if (!fmt)
                        fmt = builder->CreateGlobalString("%g", "autostr_float_fmt");
                    builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmt, val});
                } else {
                    val = toDefaultType(val);
                    llvm::GlobalVariable* fmt = module->getGlobalVariable("tostr_fmt", true);
                    if (!fmt)
                        fmt = builder->CreateGlobalString("%lld", "tostr_fmt");
                    builder->CreateCall(getOrDeclareSnprintf(), {buf, bufSize, fmt, val});
                }
                return buf;
            };
            left = ensureStrPtr(left, leftIsStr);
            right = ensureStrPtr(right, rightIsStr);

            llvm::Value* len1 = builder->CreateCall(getOrDeclareStrlen(), {left}, "len1");
            llvm::Value* len2 = builder->CreateCall(getOrDeclareStrlen(), {right}, "len2");
            llvm::Value* totalLen = builder->CreateAdd(len1, len2, "totallen");
            llvm::Value* allocSize =
                builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "allocsize");
            llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSize}, "strbuf");
            builder->CreateCall(getOrDeclareStrcpy(), {buf, left});
            builder->CreateCall(getOrDeclareStrcat(), {buf, right});
            return buf;
        }
    }

    // String repetition: str * n  (or  n * str) → repeat str n times.
    // This is the binary-operator equivalent of str_repeat(str, n).
    //
    // Uses the same strcat-loop strategy as the str_repeat() builtin,
    // which is robust under LLVM O2+ optimizations.
    if (expr->op == "*" && (leftIsStr || rightIsStr)) {
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strVal = leftIsStr ? left : right;
        llvm::Value* countVal = leftIsStr ? right : left;
        countVal = toDefaultType(countVal);
        // Clamp negative counts to 0 to prevent integer overflow in the
        // totalLen = strLen * count multiplication.
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isNeg = builder->CreateICmpSLT(countVal, zero, "strmul.isneg");
        countVal = builder->CreateSelect(isNeg, zero, countVal, "strmul.clamp");
        llvm::Value* strPtr =
            strVal->getType()->isPointerTy() ? strVal : builder->CreateIntToPtr(strVal, ptrTy, "strmul.ptr");
        llvm::Value* strLen = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strmul.len");
        llvm::Value* totalLen = builder->CreateMul(strLen, countVal, "strmul.total");
        llvm::Value* allocSz =
            builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "strmul.alloc");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSz}, "strmul.buf");
        // Null-terminate first byte so strcat works from empty buffer
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), buf);
        // Loop countVal times, strcat'ing strPtr each iteration.
        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preHdr = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "strmul.loop", curFn);
        llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "strmul.body", curFn);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "strmul.done", curFn);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* idx = builder->CreatePHI(getDefaultType(), 2, "strmul.idx");
        idx->addIncoming(zero, preHdr);
        builder->CreateCondBr(builder->CreateICmpSLT(idx, countVal, "strmul.cond"), bodyBB, doneBB);
        builder->SetInsertPoint(bodyBB);
        builder->CreateCall(getOrDeclareStrcat(), {buf, strPtr});
        llvm::Value* nextIdx = builder->CreateAdd(idx, one, "strmul.next");
        idx->addIncoming(nextIdx, bodyBB);
        builder->CreateBr(loopBB);
        builder->SetInsertPoint(doneBB);
        return buf;
    }


    // for all relational and equality operators.  Without this, the ptr→int
    // fallback below would compare raw memory addresses instead of lexicographic
    // order, giving results that depend on allocation order rather than content.
    if (leftIsStr || rightIsStr) {
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        auto toStrPtr = [&](llvm::Value* v, bool isStr) -> llvm::Value* {
            if (!isStr) {
                // Non-string operand in a mixed comparison: treat as pointer.
                return v->getType()->isPointerTy() ? v : builder->CreateIntToPtr(v, ptrTy, "strcmp.cast");
            }
            return v->getType()->isPointerTy() ? v : builder->CreateIntToPtr(v, ptrTy, "strcmp.cast");
        };
        llvm::Value* lPtr = toStrPtr(left, leftIsStr);
        llvm::Value* rPtr = toStrPtr(right, rightIsStr);
        llvm::Value* cmpResult = builder->CreateCall(getOrDeclareStrcmp(), {lPtr, rPtr}, "strcmp.res");
        llvm::Value* zero32 = builder->getInt32(0);
        llvm::Value* cmpBool;
        if (expr->op == "==")
            cmpBool = builder->CreateICmpEQ(cmpResult, zero32, "scmp.eq");
        else if (expr->op == "!=")
            cmpBool = builder->CreateICmpNE(cmpResult, zero32, "scmp.ne");
        else if (expr->op == "<")
            cmpBool = builder->CreateICmpSLT(cmpResult, zero32, "scmp.lt");
        else if (expr->op == "<=")
            cmpBool = builder->CreateICmpSLE(cmpResult, zero32, "scmp.le");
        else if (expr->op == ">")
            cmpBool = builder->CreateICmpSGT(cmpResult, zero32, "scmp.gt");
        else if (expr->op == ">=")
            cmpBool = builder->CreateICmpSGE(cmpResult, zero32, "scmp.ge");
        else
            cmpBool = nullptr;

        if (cmpBool)
            return builder->CreateZExt(cmpBool, getDefaultType(), "scmp.result");
        // For non-comparison operators on strings (should not normally occur here),
        // fall through to the integer path.
    }

    // Convert pointer types to i64 for integer operations (fallback)
    if (left->getType()->isPointerTy()) {
        left = builder->CreatePtrToInt(left, getDefaultType(), "ptoi");
    }
    if (right->getType()->isPointerTy()) {
        right = builder->CreatePtrToInt(right, getDefaultType(), "ptoi");
    }

    // Normalize integer widths: when operands have different integer bit widths
    // (e.g. i8 from u8, i32 from u32, i64 from default), extend both to the
    // wider of the two types so that LLVM binary instructions see matching types.
    if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy() &&
        left->getType() != right->getType()) {
        unsigned leftBits = left->getType()->getIntegerBitWidth();
        unsigned rightBits = right->getType()->getIntegerBitWidth();
        if (leftBits < rightBits) {
            left = builder->CreateSExt(left, right->getType(), "sext");
        } else {
            right = builder->CreateSExt(right, left->getType(), "sext");
        }
    }

    // Constant folding optimization - if both operands are constants, compute at compile time
    if (llvm::isa<llvm::ConstantInt>(left) && llvm::isa<llvm::ConstantInt>(right)) {
        auto leftConst = llvm::dyn_cast<llvm::ConstantInt>(left);
        auto rightConst = llvm::dyn_cast<llvm::ConstantInt>(right);
        int64_t lval = leftConst->getSExtValue();
        int64_t rval = rightConst->getSExtValue();
        // Use unsigned arithmetic for +, -, * to avoid signed overflow UB.
        // The unsigned result, when reinterpreted as signed, gives the correct
        // two's-complement wrapping behavior that matches LLVM's add/sub/mul.
        uint64_t ulval = static_cast<uint64_t>(lval);
        uint64_t urval = static_cast<uint64_t>(rval);

        if (expr->op == "+") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, ulval + urval));
        } else if (expr->op == "-") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, ulval - urval));
        } else if (expr->op == "*") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, ulval * urval));
        } else if (expr->op == "/") {
            if (rval != 0 && !(lval == INT64_MIN && rval == -1)) {
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval / rval));
            }
        } else if (expr->op == "%") {
            if (rval != 0 && !(lval == INT64_MIN && rval == -1)) {
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval % rval));
            }
        } else if (expr->op == "==") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval == rval ? 1 : 0));
        } else if (expr->op == "!=") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval != rval ? 1 : 0));
        } else if (expr->op == "<") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval < rval ? 1 : 0));
        } else if (expr->op == "<=") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval <= rval ? 1 : 0));
        } else if (expr->op == ">") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval > rval ? 1 : 0));
        } else if (expr->op == ">=") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval >= rval ? 1 : 0));
        } else if (expr->op == "&") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval & rval));
        } else if (expr->op == "|") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval | rval));
        } else if (expr->op == "^") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, lval ^ rval));
        } else if (expr->op == "<<") {
            if (rval >= 0 && rval < 64)
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval << rval));
        } else if (expr->op == ">>") {
            if (rval >= 0 && rval < 64)
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval >> rval));
        } else if (expr->op == "**") {
            if (rval >= 0) {
                int64_t result = 1;
                bool overflow = false;
                for (int64_t i = 0; i < rval; i++) {
                    if (lval != 0 && lval != 1 && lval != -1) {
                        uint64_t ab = (lval < 0) ? static_cast<uint64_t>(-static_cast<uint64_t>(lval))
                                                 : static_cast<uint64_t>(lval);
                        uint64_t ar = (result < 0) ? static_cast<uint64_t>(-static_cast<uint64_t>(result))
                                                   : static_cast<uint64_t>(result);
                        if (ar > static_cast<uint64_t>(INT64_MAX) / ab) {
                            overflow = true;
                            break;
                        }
                    }
                    result *= lval;
                }
                if (!overflow)
                    return llvm::ConstantInt::get(*context, llvm::APInt(64, result));
                // Fall through to emit runtime power operation on overflow.
            } else {
                return llvm::ConstantInt::get(*context, llvm::APInt(64, 0));
            }
        }
    }

    // Algebraic identity optimizations — when one operand is a known constant,
    // many operations can be simplified without emitting any instruction.
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
        int64_t rv = ci->getSExtValue();
        if (rv == 0) {
            if (expr->op == "+" || expr->op == "-" || expr->op == "|" || expr->op == "^" || expr->op == "<<" ||
                expr->op == ">>")
                return left; // x+0, x-0, x|0, x^0, x<<0, x>>0 → x
            if (expr->op == "*" || expr->op == "&")
                return llvm::ConstantInt::get(getDefaultType(), 0); // x*0, x&0 → 0
            if (expr->op == "**")
                return llvm::ConstantInt::get(getDefaultType(), 1); // x**0 → 1
        }
        if (rv == 1) {
            if (expr->op == "*" || expr->op == "/" || expr->op == "**")
                return left; // x*1, x/1, x**1 → x
            if (expr->op == "%")
                return llvm::ConstantInt::get(getDefaultType(), 0); // x%1 → 0
        }
        if (rv == -1 && expr->op == "*")
            return builder->CreateNeg(left, "negtmp"); // x*(-1) → -x
        // x & -1 (all ones) → x
        if (rv == -1 && expr->op == "&")
            return left;
        // x | -1 (all ones) → -1
        if (rv == -1 && expr->op == "|")
            return llvm::ConstantInt::get(getDefaultType(), -1);
    }
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
        int64_t lv = ci->getSExtValue();
        if (lv == 0) {
            if (expr->op == "+" || expr->op == "|" || expr->op == "^")
                return right; // 0+x, 0|x, 0^x → x
            if (expr->op == "*" || expr->op == "&" || expr->op == "<<" || expr->op == ">>")
                return llvm::ConstantInt::get(getDefaultType(), 0); // 0*x, 0&x, 0<<x, 0>>x → 0
            if (expr->op == "-")
                return builder->CreateNeg(right, "negtmp"); // 0-x → -x
        }
        if (lv == 1 && expr->op == "*")
            return right; // 1*x → x
        if (lv == 1 && expr->op == "**")
            return llvm::ConstantInt::get(getDefaultType(), 1); // 1**x → 1
        if (lv == -1 && expr->op == "*")
            return builder->CreateNeg(right, "negtmp"); // (-1)*x → -x
        // -1 & x → x
        if (lv == -1 && expr->op == "&")
            return right;
        // -1 | x → -1
        if (lv == -1 && expr->op == "|")
            return llvm::ConstantInt::get(getDefaultType(), -1);
    }

    // Same-value identity optimizations — when both operands are the exact
    // same SSA value, several operations can be simplified without emitting
    // any instruction.  This fires after mem2reg promotes allocas to SSA
    // values and catches patterns like f(x) ^ f(x) where the call result
    // is reused, or when both sides of an operator reference the same
    // already-loaded variable value.
    if (left == right) {
        if (expr->op == "-" || expr->op == "^")
            return llvm::ConstantInt::get(getDefaultType(), 0); // x-x→0, x^x→0
        if (expr->op == "&" || expr->op == "|")
            return left; // x&x→x, x|x→x
        // Comparison identities: reflexive comparisons on the same SSA value.
        if (expr->op == "==" || expr->op == "<=" || expr->op == ">=")
            return llvm::ConstantInt::get(getDefaultType(), 1); // x==x→1, x<=x→1, x>=x→1
        if (expr->op == "!=" || expr->op == "<" || expr->op == ">")
            return llvm::ConstantInt::get(getDefaultType(), 0); // x!=x→0, x<x→0, x>x→0
        // Same-value division and modulo identities.
        if (expr->op == "/")
            return llvm::ConstantInt::get(getDefaultType(), 1); // x/x→1
        if (expr->op == "%")
            return llvm::ConstantInt::get(getDefaultType(), 0); // x%x→0
    }

    // Comparison-against-zero strength reduction: when one side of an
    // equality/inequality comparison is the constant 0, exploit the fact
    // that x86-64 (and most architectures) can test for zero with a single
    // TEST/CMP instruction.  We canonicalize to place 0 on the right.
    if (expr->op == "==" || expr->op == "!=") {
        llvm::Value* testVal = nullptr;
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            if (ci->isZero()) { testVal = left; }
        }
        if (!testVal) {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
                if (ci->isZero()) { testVal = right; }
            }
        }
        // x * N == 0  →  x == 0  (when N is a nonzero constant)
        // This eliminates a multiply when comparing the product against zero.
        if (testVal) {
            if (auto* mulInst = llvm::dyn_cast<llvm::BinaryOperator>(testVal)) {
                if (mulInst->getOpcode() == llvm::Instruction::Mul) {
                    llvm::Value* mulLeft = mulInst->getOperand(0);
                    llvm::Value* mulRight = mulInst->getOperand(1);
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(mulRight)) {
                        if (!ci->isZero()) {
                            llvm::Value* cmp = (expr->op == "==")
                                ? builder->CreateICmpEQ(mulLeft, llvm::ConstantInt::get(getDefaultType(), 0), "cmptmp")
                                : builder->CreateICmpNE(mulLeft, llvm::ConstantInt::get(getDefaultType(), 0), "cmptmp");
                            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
                        }
                    }
                    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(mulLeft)) {
                        if (!ci->isZero()) {
                            llvm::Value* cmp = (expr->op == "==")
                                ? builder->CreateICmpEQ(mulRight, llvm::ConstantInt::get(getDefaultType(), 0), "cmptmp")
                                : builder->CreateICmpNE(mulRight, llvm::ConstantInt::get(getDefaultType(), 0), "cmptmp");
                            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
                        }
                    }
                }
            }
        }
    }

    // Subtraction-comparison strength reduction: when one side of an
    // equality/inequality comparison is a subtraction and the other is 0,
    // replace (x - y) == 0 with x == y and (x - y) != 0 with x != y.
    // This eliminates a sub instruction since the comparison subsumes it.
    if (expr->op == "==" || expr->op == "!=") {
        auto tryFoldSub = [&](llvm::Value* lhs, llvm::Value* rhs) -> llvm::Value* {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(rhs)) {
                if (ci->isZero()) {
                    if (auto* subInst = llvm::dyn_cast<llvm::BinaryOperator>(lhs)) {
                        if (subInst->getOpcode() == llvm::Instruction::Sub) {
                            llvm::Value* cmp = (expr->op == "==")
                                ? builder->CreateICmpEQ(subInst->getOperand(0), subInst->getOperand(1), "cmptmp")
                                : builder->CreateICmpNE(subInst->getOperand(0), subInst->getOperand(1), "cmptmp");
                            return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
                        }
                    }
                }
            }
            return nullptr;
        };
        if (auto* result = tryFoldSub(left, right))
            return result;
        if (auto* result = tryFoldSub(right, left))
            return result;
    }

    // Regular code generation for non-constant expressions.
    // Integer arithmetic uses wrapping (no NSW/NUW flags): NSW/NUW flags tell
    // LLVM that overflow is undefined behavior, which can cause miscompilation
    // when overflow actually occurs.  Omitting them guarantees defined
    // two's-complement behavior on every overflow path.
    if (expr->op == "+") {
        return builder->CreateAdd(left, right, "addtmp");
    } else if (expr->op == "-") {
        return builder->CreateSub(left, right, "subtmp");
    } else if (expr->op == "*") {
        // Strength reduction: multiply by power of 2 → left shift
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            int s = log2IfPowerOf2(ci->getSExtValue());
            if (s >= 0)
                return builder->CreateShl(left, llvm::ConstantInt::get(getDefaultType(), s), "shltmp");
        }
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
            int s = log2IfPowerOf2(ci->getSExtValue());
            if (s >= 0)
                return builder->CreateShl(right, llvm::ConstantInt::get(getDefaultType(), s), "shltmp");
        }
        // Strength reduction: multiply by small non-power-of-2 constants
        // to shift+add/sub sequences (faster on many microarchitectures).
        // n*3 → (n<<1)+n, n*5 → (n<<2)+n, n*7 → (n<<3)-n, n*9 → (n<<3)+n
        // n*10 → (n<<3)+(n<<1), n*15 → (n<<4)-n, n*17 → (n<<4)+n
        auto emitShiftAdd = [&](llvm::Value* base, int64_t multiplier) -> llvm::Value* {
            switch (multiplier) {
            case 3: {
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul3.shl");
                return builder->CreateAdd(shl, base, "mul3");
            }
            case 5: {
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul5.shl");
                return builder->CreateAdd(shl, base, "mul5");
            }
            case 7: {
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul7.shl");
                return builder->CreateSub(shl, base, "mul7");
            }
            case 9: {
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul9.shl");
                return builder->CreateAdd(shl, base, "mul9");
            }
            case 10: {
                // n*10 → (n<<3) + (n<<1)
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul10.shl3");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul10.shl1");
                return builder->CreateAdd(shl3, shl1, "mul10");
            }
            case 15: {
                // n*15 → (n<<4) - n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul15.shl");
                return builder->CreateSub(shl, base, "mul15");
            }
            case 17: {
                // n*17 → (n<<4) + n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul17.shl");
                return builder->CreateAdd(shl, base, "mul17");
            }
            case 6: {
                // n*6 → (n<<2) + (n<<1)  (= n*4 + n*2)
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul6.shl2");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul6.shl1");
                return builder->CreateAdd(shl2, shl1, "mul6");
            }
            case 12: {
                // n*12 → (n<<3) + (n<<2)  (= n*8 + n*4)
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul12.shl3");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul12.shl2");
                return builder->CreateAdd(shl3, shl2, "mul12");
            }
            case 24: {
                // n*24 → (n<<5) - (n<<3)  (= n*32 - n*8)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul24.shl5");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul24.shl3");
                return builder->CreateSub(shl5, shl3, "mul24");
            }
            case 25: {
                // n*25 → ((n<<5) - (n<<3)) + n  (= (n*32 - n*8) + n = n*24 + n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul25.shl5");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul25.shl3");
                auto* t = builder->CreateSub(shl5, shl3, "mul25.t");
                return builder->CreateAdd(t, base, "mul25");
            }
            case 31: {
                // n*31 → (n<<5) - n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul31.shl");
                return builder->CreateSub(shl, base, "mul31");
            }
            case 33: {
                // n*33 → (n<<5) + n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul33.shl");
                return builder->CreateAdd(shl, base, "mul33");
            }
            case 63: {
                // n*63 → (n<<6) - n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 6), "mul63.shl");
                return builder->CreateSub(shl, base, "mul63");
            }
            case 65: {
                // n*65 → (n<<6) + n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 6), "mul65.shl");
                return builder->CreateAdd(shl, base, "mul65");
            }
            case 127: {
                // n*127 → (n<<7) - n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 7), "mul127.shl");
                return builder->CreateSub(shl, base, "mul127");
            }
            case 255: {
                // n*255 → (n<<8) - n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 8), "mul255.shl");
                return builder->CreateSub(shl, base, "mul255");
            }
            case 1000: {
                // n*1000 → (n<<10) - (n<<5) + (n<<3)  (= 1024n - 32n + 8n)
                auto* shl10 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 10), "mul1000.shl10");
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul1000.shl5");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul1000.shl3");
                auto* t = builder->CreateSub(shl10, shl5, "mul1000.t");
                return builder->CreateAdd(t, shl3, "mul1000");
            }
            case 100: {
                // n*100 → (n<<7) - (n<<5) + (n<<2)  (= 128n - 32n + 4n)
                auto* shl7 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 7), "mul100.shl7");
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul100.shl5");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul100.shl2");
                auto* t = builder->CreateSub(shl7, shl5, "mul100.t");
                return builder->CreateAdd(t, shl2, "mul100");
            }
            case 11: {
                // n*11 → (n<<3) + (n<<1) + n  (= 8n + 2n + n)
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul11.shl3");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul11.shl1");
                auto* t = builder->CreateAdd(shl3, shl1, "mul11.t");
                return builder->CreateAdd(t, base, "mul11");
            }
            case 13: {
                // n*13 → (n<<4) - (n<<1) - n  (= 16n - 2n - n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul13.shl4");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul13.shl1");
                auto* t = builder->CreateSub(shl4, shl1, "mul13.t");
                return builder->CreateSub(t, base, "mul13");
            }
            case 20: {
                // n*20 → (n<<4) + (n<<2)  (= 16n + 4n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul20.shl4");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul20.shl2");
                return builder->CreateAdd(shl4, shl2, "mul20");
            }
            case 21: {
                // n*21 → (n<<4) + (n<<2) + n  (= 16n + 4n + n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul21.shl4");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul21.shl2");
                auto* t = builder->CreateAdd(shl4, shl2, "mul21.t");
                return builder->CreateAdd(t, base, "mul21");
            }
            case 14: {
                // n*14 → (n<<4) - (n<<1)  (= 16n - 2n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul14.shl4");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul14.shl1");
                return builder->CreateSub(shl4, shl1, "mul14");
            }
            case 28: {
                // n*28 → (n<<5) - (n<<2)  (= 32n - 4n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul28.shl5");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul28.shl2");
                return builder->CreateSub(shl5, shl2, "mul28");
            }
            case 40: {
                // n*40 → (n<<5) + (n<<3)  (= 32n + 8n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul40.shl5");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul40.shl3");
                return builder->CreateAdd(shl5, shl3, "mul40");
            }
            case 48: {
                // n*48 → (n<<5) + (n<<4)  (= 32n + 16n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul48.shl5");
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul48.shl4");
                return builder->CreateAdd(shl5, shl4, "mul48");
            }
            case 60: {
                // n*60 → (n<<6) - (n<<2)  (= 64n - 4n)
                auto* shl6 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 6), "mul60.shl6");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul60.shl2");
                return builder->CreateSub(shl6, shl2, "mul60");
            }
            case 96: {
                // n*96 → (n<<7) - (n<<5)  (= 128n - 32n)
                auto* shl7 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 7), "mul96.shl7");
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul96.shl5");
                return builder->CreateSub(shl7, shl5, "mul96");
            }
            case 120: {
                // n*120 → (n<<7) - (n<<3)  (= 128n - 8n)
                auto* shl7 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 7), "mul120.shl7");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul120.shl3");
                return builder->CreateSub(shl7, shl3, "mul120");
            }
            case 256: {
                // n*256 → (n<<8)
                return builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 8), "mul256");
            }
            case 512: {
                // n*512 → (n<<9)
                return builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 9), "mul512");
            }
            case 1024: {
                // n*1024 → (n<<10)
                return builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 10), "mul1024");
            }
            default:
                return nullptr;
            }
        };
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            if (auto* result = emitShiftAdd(left, ci->getSExtValue()))
                return result;
            // Negative constant strength reduction: n * (-K) → neg(n * K).
            // This leverages the existing shift+add patterns for the absolute
            // value, then negates the result.  A single neg (sub 0, x) is far
            // cheaper than a hardware multiply.
            int64_t rv = ci->getSExtValue();
            if (rv < -1) {
                if (auto* posResult = emitShiftAdd(left, -rv)) {
                    return builder->CreateNeg(posResult, "mulneg");
                }
            }
        }
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
            if (auto* result = emitShiftAdd(right, ci->getSExtValue()))
                return result;
            int64_t lv = ci->getSExtValue();
            if (lv < -1) {
                if (auto* posResult = emitShiftAdd(right, -lv)) {
                    return builder->CreateNeg(posResult, "mulneg");
                }
            }
        }
        return builder->CreateMul(left, right, "multmp");
    } else if (expr->op == "/" || expr->op == "%") {
        bool isDivision = expr->op == "/";

        // Division/modulo by a non-zero constant: let the LLVM optimizer handle
        // strength reduction via InstCombine.  LLVM converts srem/sdiv by
        // constants to magic-number multiply-shift sequences, AND converts
        // srem → urem when it can prove the dividend is non-negative (via
        // CorrelatedValuePropagation).  The unsigned path avoids the sign-
        // correction fixup, saving ~2 instructions per operation.
        //
        // Previously, the codegen emitted its own magic-number code with
        // unconditional sign correction.  That prevented LLVM from applying
        // the srem→urem optimisation, causing a ~2× slowdown on benchmarks
        // like triple-nested loops with modulo (e.g. `((i^j)+k) % 37`).
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            if (!ci->isZero()) {
                return isDivision ? builder->CreateSDiv(left, right, "divtmp")
                                  : builder->CreateSRem(left, right, "modtmp");
            }
        }

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* isZero = builder->CreateICmpEQ(right, zero, "divzero");
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        const char* zeroName = isDivision ? "div.zero" : "mod.zero";
        const char* opName = isDivision ? "div.op" : "mod.op";
        llvm::BasicBlock* zeroBB = llvm::BasicBlock::Create(*context, zeroName, function);
        llvm::BasicBlock* opBB = llvm::BasicBlock::Create(*context, opName, function);
        // Branch weights: division by zero is extremely unlikely; mark the
        // error path cold so the branch predictor favours the fast path and
        // the code layout keeps the error stub out of the hot I-cache region.
        llvm::MDBuilder mdBuilder(*context);
        auto* brWeights = mdBuilder.createBranchWeights(1, 1000); // 1:1000 zero:nonzero
        builder->CreateCondBr(isZero, zeroBB, opBB, brWeights);

        builder->SetInsertPoint(zeroBB);
        const char* messageText = isDivision ? "Runtime error: division by zero\n" : "Runtime error: modulo by zero\n";
        const char* messageName = isDivision ? "divzero_msg" : "modzero_msg";
        llvm::Value* message = builder->CreateGlobalString(messageText, messageName);
        builder->CreateCall(getPrintfFunction(), {message});
        builder->CreateCall(getOrDeclareExit(), {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1)});
        builder->CreateUnreachable();

        builder->SetInsertPoint(opBB);
        return isDivision ? builder->CreateSDiv(left, right, "divtmp") : builder->CreateSRem(left, right, "modtmp");
    } else if (expr->op == "==") {
        llvm::Value* cmp = builder->CreateICmpEQ(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == "!=") {
        llvm::Value* cmp = builder->CreateICmpNE(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == "<") {
        llvm::Value* cmp = builder->CreateICmpSLT(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == "<=") {
        llvm::Value* cmp = builder->CreateICmpSLE(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == ">") {
        llvm::Value* cmp = builder->CreateICmpSGT(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == ">=") {
        llvm::Value* cmp = builder->CreateICmpSGE(left, right, "cmptmp");
        return builder->CreateZExt(cmp, getDefaultType(), "booltmp");
    } else if (expr->op == "&") {
        return builder->CreateAnd(left, right, "andtmp");
    } else if (expr->op == "|") {
        return builder->CreateOr(left, right, "ortmp");
    } else if (expr->op == "^") {
        return builder->CreateXor(left, right, "xortmp");
    } else if (expr->op == "<<") {
        // Mask shift amount to [0, 63] to prevent undefined behavior
        llvm::Value* mask = llvm::ConstantInt::get(getDefaultType(), 63);
        llvm::Value* safeShift = builder->CreateAnd(right, mask, "shlmask");
        // Do NOT set nsw/nuw: left-shifts on arbitrary values can overflow,
        // and marking nsw would let LLVM treat overflowing shifts as poison.
        return builder->CreateShl(left, safeShift, "shltmp");
    } else if (expr->op == ">>") {
        // Mask shift amount to [0, 63] to prevent undefined behavior
        llvm::Value* mask = llvm::ConstantInt::get(getDefaultType(), 63);
        llvm::Value* safeShift = builder->CreateAnd(right, mask, "shrmask");
        // Logical (unsigned) shift right: fills high bits with 0.
        return builder->CreateLShr(left, safeShift, "lshrtmp");
    } else if (expr->op == "**") {
        // Small constant exponent specialization — emit inline multiplications
        // instead of the general binary-exponentiation loop.  This eliminates
        // loop overhead and branches for the most common exponents,
        // producing straight-line code that the backend can schedule optimally.
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            int64_t exp = ci->getSExtValue();
            if (exp == 2) {
                // x**2 → x*x  (1 mul)
                return builder->CreateMul(left, left, "pow2");
            }
            if (exp == 3) {
                // x**3 → x*x*x  (2 muls)
                auto* sq = builder->CreateMul(left, left, "pow3.sq");
                return builder->CreateMul(sq, left, "pow3");
            }
            if (exp == 4) {
                // x**4 → t=x*x; t*t  (2 muls)
                auto* sq = builder->CreateMul(left, left, "pow4.sq");
                return builder->CreateMul(sq, sq, "pow4");
            }
            if (exp == 5) {
                // x**5 → t=x*x; t*t*x  (3 muls)
                auto* sq = builder->CreateMul(left, left, "pow5.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow5.q4");
                return builder->CreateMul(q4, left, "pow5");
            }
            if (exp == 6) {
                // x**6 → t=x*x; u=t*t; u*t  (3 muls)
                auto* sq = builder->CreateMul(left, left, "pow6.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow6.q4");
                return builder->CreateMul(q4, sq, "pow6");
            }
            if (exp == 7) {
                // x**7 → t=x*x; u=t*t; u*t*x  (4 muls)
                auto* sq = builder->CreateMul(left, left, "pow7.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow7.q4");
                auto* q6 = builder->CreateMul(q4, sq, "pow7.q6");
                return builder->CreateMul(q6, left, "pow7");
            }
            if (exp == 8) {
                // x**8 → t=x*x; u=t*t; u*u  (3 muls)
                auto* sq = builder->CreateMul(left, left, "pow8.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow8.q4");
                return builder->CreateMul(q4, q4, "pow8");
            }
            if (exp == 9) {
                // x**9 → t=x*x; u=t*t; v=u*u; v*x  (4 muls)
                auto* sq = builder->CreateMul(left, left, "pow9.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow9.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow9.q8");
                return builder->CreateMul(q8, left, "pow9");
            }
            if (exp == 10) {
                // x**10 → t=x*x; u=t*t; v=u*u; v*t  (4 muls)
                auto* sq = builder->CreateMul(left, left, "pow10.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow10.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow10.q8");
                return builder->CreateMul(q8, sq, "pow10");
            }
            if (exp == 11) {
                // x**11 → t=x*x; u=t*t; v=u*u; v*t*x  (5 muls)
                auto* sq = builder->CreateMul(left, left, "pow11.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow11.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow11.q8");
                auto* q10 = builder->CreateMul(q8, sq, "pow11.q10");
                return builder->CreateMul(q10, left, "pow11");
            }
            if (exp == 12) {
                // x**12 → t=x*x; u=t*t; v=u*u; v*u  (4 muls)
                auto* sq = builder->CreateMul(left, left, "pow12.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow12.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow12.q8");
                return builder->CreateMul(q8, q4, "pow12");
            }
            if (exp == 16) {
                // x**16 → t=x*x; u=t*t; v=u*u; v*v  (4 muls)
                auto* sq = builder->CreateMul(left, left, "pow16.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow16.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow16.q8");
                return builder->CreateMul(q8, q8, "pow16");
            }
            if (exp == 13) {
                // x**13 → t=x*x; u=t*t; v=u*t; w=v*v; w*x  (5 muls)
                auto* sq = builder->CreateMul(left, left, "pow13.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow13.q4");
                auto* q6 = builder->CreateMul(q4, sq, "pow13.q6");
                auto* q12 = builder->CreateMul(q6, q6, "pow13.q12");
                return builder->CreateMul(q12, left, "pow13");
            }
            if (exp == 14) {
                // x**14 = x^12 * x^2 → sq=x*x; q4=sq*sq; q8=q4*q4; q12=q8*q4; q12*sq  (5 muls)
                auto* sq = builder->CreateMul(left, left, "pow14.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow14.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow14.q8");
                auto* q12 = builder->CreateMul(q8, q4, "pow14.q12");
                return builder->CreateMul(q12, sq, "pow14");
            }
            if (exp == 15) {
                // x**15 = x^12 * x^3 → sq; q3=sq*x; q6=q3*q3; q12=q6*q6; q12*q3  (5 muls)
                auto* sq = builder->CreateMul(left, left, "pow15.sq");
                auto* q3 = builder->CreateMul(sq, left, "pow15.q3");
                auto* q6 = builder->CreateMul(q3, q3, "pow15.q6");
                auto* q12 = builder->CreateMul(q6, q6, "pow15.q12");
                return builder->CreateMul(q12, q3, "pow15");
            }
            if (exp == 17) {
                // x**17 = x^16 * x  (5 muls)
                auto* sq = builder->CreateMul(left, left, "pow17.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow17.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow17.q8");
                auto* q16 = builder->CreateMul(q8, q8, "pow17.q16");
                return builder->CreateMul(q16, left, "pow17");
            }
            if (exp == 18) {
                // x**18 = x^16 * x^2  (5 muls)
                auto* sq = builder->CreateMul(left, left, "pow18.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow18.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow18.q8");
                auto* q16 = builder->CreateMul(q8, q8, "pow18.q16");
                return builder->CreateMul(q16, sq, "pow18");
            }
            if (exp == 20) {
                // x**20 = x^16 * x^4  (5 muls)
                auto* sq = builder->CreateMul(left, left, "pow20.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow20.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow20.q8");
                auto* q16 = builder->CreateMul(q8, q8, "pow20.q16");
                return builder->CreateMul(q16, q4, "pow20");
            }
            if (exp == 24) {
                // x**24 = x^16 * x^8  (5 muls)
                auto* sq = builder->CreateMul(left, left, "pow24.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow24.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow24.q8");
                auto* q16 = builder->CreateMul(q8, q8, "pow24.q16");
                return builder->CreateMul(q16, q8, "pow24");
            }
            if (exp == 25) {
                // x**25 = x^24 * x = x^16 * x^8 * x  (6 muls)
                auto* sq = builder->CreateMul(left, left, "pow25.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow25.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow25.q8");
                auto* q16 = builder->CreateMul(q8, q8, "pow25.q16");
                auto* q24 = builder->CreateMul(q16, q8, "pow25.q24");
                return builder->CreateMul(q24, left, "pow25");
            }
            if (exp == 32) {
                // x**32 → ((((x*x)^2)^2)^2)^2  (5 muls)
                auto* sq = builder->CreateMul(left, left, "pow32.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow32.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow32.q8");
                auto* q16 = builder->CreateMul(q8, q8, "pow32.q16");
                return builder->CreateMul(q16, q16, "pow32");
            }
            if (exp == 64) {
                // x**64 → (((((x*x)^2)^2)^2)^2)^2  (6 muls)
                auto* sq = builder->CreateMul(left, left, "pow64.sq");
                auto* q4 = builder->CreateMul(sq, sq, "pow64.q4");
                auto* q8 = builder->CreateMul(q4, q4, "pow64.q8");
                auto* q16 = builder->CreateMul(q8, q8, "pow64.q16");
                auto* q32 = builder->CreateMul(q16, q16, "pow64.q32");
                return builder->CreateMul(q32, q32, "pow64");
            }
        }

        // Integer exponentiation via binary exponentiation (exponentiation by
        // squaring).  This is O(log n) in the exponent vs. the naive O(n) loop.
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* negExpBB = llvm::BasicBlock::Create(*context, "pow.negexp", function);
        llvm::BasicBlock* posExpBB = llvm::BasicBlock::Create(*context, "pow.posexp", function);
        llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "pow.loop", function);
        llvm::BasicBlock* oddBB = llvm::BasicBlock::Create(*context, "pow.odd", function);
        llvm::BasicBlock* squareBB = llvm::BasicBlock::Create(*context, "pow.square", function);
        llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "pow.done", function);

        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0, true);
        llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1, true);
        llvm::Value* isNegExp = builder->CreateICmpSLT(right, zero, "pow.isneg");
        builder->CreateCondBr(isNegExp, negExpBB, posExpBB);

        // Negative exponent: integer result is 0 for |base| > 1
        builder->SetInsertPoint(negExpBB);
        builder->CreateBr(doneBB);

        builder->SetInsertPoint(posExpBB);
        builder->CreateBr(loopBB);

        // Loop: result *= base when exponent is odd; base *= base; exp >>= 1
        builder->SetInsertPoint(loopBB);
        llvm::PHINode* result = builder->CreatePHI(getDefaultType(), 3, "pow.result");
        llvm::PHINode* base = builder->CreatePHI(getDefaultType(), 3, "pow.base");
        llvm::PHINode* exp = builder->CreatePHI(getDefaultType(), 3, "pow.exp");
        result->addIncoming(one, posExpBB);
        base->addIncoming(left, posExpBB);
        exp->addIncoming(right, posExpBB);

        llvm::Value* done = builder->CreateICmpSLE(exp, zero, "pow.done.cmp");
        builder->CreateCondBr(done, doneBB, oddBB);

        // Check if exponent is odd
        builder->SetInsertPoint(oddBB);
        llvm::Value* expBit = builder->CreateAnd(exp, one, "pow.bit");
        llvm::Value* isOdd = builder->CreateICmpNE(expBit, zero, "pow.isodd");
        llvm::Value* newResult = builder->CreateMul(result, base, "pow.mul");
        llvm::Value* resultSel = builder->CreateSelect(isOdd, newResult, result, "pow.rsel");
        builder->CreateBr(squareBB);

        // Square the base and halve the exponent
        builder->SetInsertPoint(squareBB);
        llvm::Value* newBase = builder->CreateMul(base, base, "pow.sq");
        llvm::Value* newExp = builder->CreateAShr(exp, one, "pow.halve");
        result->addIncoming(resultSel, squareBB);
        base->addIncoming(newBase, squareBB);
        exp->addIncoming(newExp, squareBB);
        builder->CreateBr(loopBB);

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* finalResult = builder->CreatePHI(getDefaultType(), 2, "pow.final");
        finalResult->addIncoming(zero, negExpBB);
        finalResult->addIncoming(result, loopBB);
        return finalResult;
    }

    codegenError("Unknown binary operator: " + expr->op, expr);
}

llvm::Value* CodeGenerator::generateUnary(UnaryExpr* expr) {
    // Double negation elimination — detect op(op(x)) patterns at the AST level
    // and short-circuit to just x: -(-x) → x, ~(~x) → x, !(!x) → x.
    if (auto* inner = dynamic_cast<UnaryExpr*>(expr->operand.get())) {
        if (inner->op == expr->op && (expr->op == "-" || expr->op == "~" || expr->op == "!")) {
            return generateExpression(inner->operand.get());
        }
    }

    llvm::Value* operand = generateExpression(expr->operand.get());

    // Constant folding for unary operations
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(operand)) {
        int64_t val = ci->getSExtValue();
        if (expr->op == "-") {
            if (val != INT64_MIN)
                return llvm::ConstantInt::get(*context, llvm::APInt(64, -val));
        } else if (expr->op == "!") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, val == 0 ? 1 : 0));
        } else if (expr->op == "~") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, ~val));
        }
    }
    if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(operand)) {
        if (expr->op == "-") {
            return llvm::ConstantFP::get(getFloatType(), -cf->getValueAPF().convertToDouble());
        }
    }

    if (expr->op == "-") {
        if (operand->getType()->isDoubleTy()) {
            return builder->CreateFNeg(operand, "fnegtmp");
        }
        return builder->CreateNeg(operand, "negtmp");
    } else if (expr->op == "!") {
        llvm::Value* boolVal = toBool(operand);
        llvm::Value* notVal = builder->CreateNot(boolVal, "nottmp");
        return builder->CreateZExt(notVal, getDefaultType(), "booltmp");
    } else if (expr->op == "~") {
        if (operand->getType()->isDoubleTy()) {
            operand = builder->CreateFPToSI(operand, getDefaultType(), "ftoi");
        }
        return builder->CreateNot(operand, "bitnottmp");
    }

    codegenError("Unknown unary operator: " + expr->op, expr);
}

llvm::Value* CodeGenerator::generateAssign(AssignExpr* expr) {
    llvm::Value* value = generateExpression(expr->value.get());
    auto it = namedValues.find(expr->name);
    if (it == namedValues.end() || !it->second) {
        codegenError("Unknown variable: " + expr->name, expr);
    }
    checkConstModification(expr->name, "modify");

    // Re-assigning to a dead (moved/invalidated) variable revives it.
    deadVars_.erase(expr->name);
    deadVarReason_.erase(expr->name);

    // Type conversion if the alloca type and value type differ
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second);
    if (alloca) {
        llvm::Type* allocaType = alloca->getAllocatedType();
        if (allocaType->isDoubleTy() && value->getType()->isIntegerTy()) {
            value = builder->CreateSIToFP(value, getFloatType(), "itof");
        } else if (allocaType->isIntegerTy() && value->getType()->isDoubleTy()) {
            value = builder->CreateFPToSI(value, allocaType, "ftoi");
        } else if (allocaType->isIntegerTy() && value->getType()->isPointerTy()) {
            value = builder->CreatePtrToInt(value, allocaType, "ptoi");
        } else if (allocaType->isPointerTy() && value->getType()->isIntegerTy()) {
            value = builder->CreateIntToPtr(value, llvm::PointerType::getUnqual(*context), "itop");
        } else if (allocaType->isIntegerTy() && value->getType()->isIntegerTy() &&
                   allocaType != value->getType()) {
            value = convertTo(value, allocaType);
        }
    }

    builder->CreateStore(value, it->second);
    // Update string variable tracking after assignment.
    if (value->getType()->isPointerTy() || isStringExpr(expr->value.get()))
        stringVars_.insert(expr->name);
    else
        stringVars_.erase(expr->name);
    // Update string-array tracking after assignment.
    if (isStringArrayExpr(expr->value.get()))
        stringArrayVars_.insert(expr->name);
    else
        stringArrayVars_.erase(expr->name);
    return value;
}

// ---------------------------------------------------------------------------
// Shared prefix/postfix increment/decrement helper
// ---------------------------------------------------------------------------
// Factored out of generatePostfix() and generatePrefix() which were ~90%
// identical.  The only semantic difference is the return value: postfix
// returns the value *before* the update, prefix returns the value *after*.

llvm::Value* CodeGenerator::generateIncDec(Expression* operandExpr, const std::string& op, bool isPostfix,
                                           const ASTNode* errorNode) {
    if (op != "++" && op != "--") {
        codegenError("Unknown increment/decrement operator: " + op, errorNode);
    }

    // Handle array element increment/decrement: arr[i]++ / ++arr[i]
    auto* indexExpr = dynamic_cast<IndexExpr*>(operandExpr);
    if (indexExpr) {
        llvm::Value* arrVal = generateExpression(indexExpr->array.get());
        llvm::Value* idxVal = generateExpression(indexExpr->index.get());
        // Normalize index to i64 — generateIndex() does this (line 1003) but
        // generateIncDec() was missing it, causing type mismatches in the
        // ICmpSLT bounds check below when the index is a float or i1.
        idxVal = toDefaultType(idxVal);

        // Convert i64 → pointer; check if already a pointer first to avoid
        // invalid IntToPtr of a pointer value (matches generateIndex() pattern).
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* arrPtr =
            arrVal->getType()->isPointerTy() ? arrVal : builder->CreateIntToPtr(arrVal, ptrTy, "incdec.arrptr");

        // Bounds check
        llvm::Value* lenVal = builder->CreateLoad(getDefaultType(), arrPtr, "incdec.len");
        llvm::Value* inBounds = builder->CreateICmpSLT(idxVal, lenVal, "incdec.inbounds");
        llvm::Value* notNeg =
            builder->CreateICmpSGE(idxVal, llvm::ConstantInt::get(getDefaultType(), 0), "incdec.notneg");
        llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "incdec.valid");

        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "incdec.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "incdec.fail", function);
        builder->CreateCondBr(valid, okBB, failBB);

        builder->SetInsertPoint(failBB);
        llvm::Value* errMsg = builder->CreateGlobalString("Runtime error: array index out of bounds\n", "idx_oob_msg");
        builder->CreateCall(getPrintfFunction(), {errMsg});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        llvm::Value* offset = builder->CreateAdd(idxVal, llvm::ConstantInt::get(getDefaultType(), 1), "incdec.offset");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr, offset, "incdec.elem.ptr");
        llvm::Value* current = builder->CreateLoad(getDefaultType(), elemPtr, "incdec.elem");

        llvm::Value* delta = llvm::ConstantInt::get(getDefaultType(), 1, true);
        llvm::Value* updated =
            (op == "++") ? builder->CreateAdd(current, delta, "inc") : builder->CreateSub(current, delta, "dec");
        builder->CreateStore(updated, elemPtr);
        return isPostfix ? current : updated;
    }

    // Handle simple variable increment/decrement
    auto* identifier = dynamic_cast<IdentifierExpr*>(operandExpr);
    if (!identifier) {
        codegenError("Increment/decrement operators require an lvalue operand", errorNode);
    }

    auto it = namedValues.find(identifier->name);
    if (it == namedValues.end() || !it->second) {
        codegenError("Unknown variable: " + identifier->name, errorNode);
    }
    checkConstModification(identifier->name, "modify");

    auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(it->second);
    llvm::Type* loadType = allocaInst ? allocaInst->getAllocatedType() : getDefaultType();
    llvm::Value* current = builder->CreateLoad(loadType, it->second, identifier->name.c_str());

    llvm::Value* updated;
    if (current->getType()->isDoubleTy()) {
        llvm::Value* one = llvm::ConstantFP::get(getFloatType(), 1.0);
        updated = (op == "++") ? builder->CreateFAdd(current, one, "finc") : builder->CreateFSub(current, one, "fdec");
    } else {
        llvm::Value* delta = llvm::ConstantInt::get(getDefaultType(), 1, true);
        updated = (op == "++") ? builder->CreateAdd(current, delta, "inc") : builder->CreateSub(current, delta, "dec");
    }

    builder->CreateStore(updated, it->second);
    return isPostfix ? current : updated;
}

llvm::Value* CodeGenerator::generatePostfix(PostfixExpr* expr) {
    return generateIncDec(expr->operand.get(), expr->op, /*isPostfix=*/true, expr);
}

llvm::Value* CodeGenerator::generatePrefix(PrefixExpr* expr) {
    return generateIncDec(expr->operand.get(), expr->op, /*isPostfix=*/false, expr);
}

llvm::Value* CodeGenerator::generateTernary(TernaryExpr* expr) {
    llvm::Value* condition = generateExpression(expr->condition.get());

    // Constant condition elimination — when the condition is known at compile
    // time we can evaluate only the selected branch, avoiding the branch/PHI
    // overhead entirely (matches generateIf's dead-branch pruning).
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(condition)) {
        if (ci->isZero()) {
            return generateExpression(expr->elseExpr.get());
        }
        return generateExpression(expr->thenExpr.get());
    }

    llvm::Value* condBool = toBool(condition);

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(*context, "tern.then", function);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(*context, "tern.else", function);
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "tern.cont", function);

    builder->CreateCondBr(condBool, thenBB, elseBB);

    builder->SetInsertPoint(thenBB);
    llvm::Value* thenVal = generateExpression(expr->thenExpr.get());
    thenBB = builder->GetInsertBlock();

    builder->SetInsertPoint(elseBB);
    llvm::Value* elseVal = generateExpression(expr->elseExpr.get());
    elseBB = builder->GetInsertBlock();

    // Ensure matching types for PHI node
    if (thenVal->getType() != elseVal->getType()) {
        if (thenVal->getType()->isDoubleTy() || elseVal->getType()->isDoubleTy()) {
            if (!thenVal->getType()->isDoubleTy()) {
                builder->SetInsertPoint(thenBB);
                thenVal = ensureFloat(thenVal);
            }
            if (!elseVal->getType()->isDoubleTy()) {
                builder->SetInsertPoint(elseBB);
                elseVal = ensureFloat(elseVal);
            }
        }
    }

    builder->SetInsertPoint(thenBB);
    builder->CreateBr(mergeBB);
    thenBB = builder->GetInsertBlock();

    builder->SetInsertPoint(elseBB);
    builder->CreateBr(mergeBB);
    elseBB = builder->GetInsertBlock();

    builder->SetInsertPoint(mergeBB);
    llvm::PHINode* phi = builder->CreatePHI(thenVal->getType(), 2, "ternval");
    phi->addIncoming(thenVal, thenBB);
    phi->addIncoming(elseVal, elseBB);

    return phi;
}

llvm::Value* CodeGenerator::generateArray(ArrayExpr* expr) {
    // Check if any elements are spread expressions
    bool hasSpread = false;
    for (const auto& elem : expr->elements) {
        if (elem->type == ASTNodeType::SPREAD_EXPR) {
            hasSpread = true;
            break;
        }
    }

    if (!hasSpread) {
        // Original fast path: all elements are plain expressions with known count
        size_t numElements = expr->elements.size();
        size_t totalSlots = 1 + numElements;

        llvm::Value* byteSize = llvm::ConstantInt::get(getDefaultType(), totalSlots * 8);
        llvm::Value* arrPtr = builder->CreateCall(getOrDeclareMalloc(), {byteSize}, "arr");

        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), numElements), arrPtr);

        for (size_t i = 0; i < numElements; i++) {
            llvm::Value* elemVal = generateExpression(expr->elements[i].get());
            elemVal = toDefaultType(elemVal);
            llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), arrPtr,
                                                      llvm::ConstantInt::get(getDefaultType(), i + 1), "arr.elem.ptr");
            builder->CreateStore(elemVal, elemPtr);
        }

        return builder->CreatePtrToInt(arrPtr, getDefaultType(), "arr.int");
    }

    // Spread path: compute total length dynamically
    // First pass: compute total number of elements
    llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
    llvm::Value* one = llvm::ConstantInt::get(getDefaultType(), 1);
    llvm::Value* eight = llvm::ConstantInt::get(getDefaultType(), 8);
    llvm::Value* totalLen = zero;

    // We need to evaluate spread source arrays to get their lengths
    // Store evaluated values so we don't evaluate twice
    struct ElemInfo {
        llvm::Value* val; // Evaluated value (element or array ptr)
        bool isSpread;    // Whether this is a spread element
        llvm::Value* len; // Length of spread array (if isSpread)
    };
    std::vector<ElemInfo> evalElems;

    for (const auto& elem : expr->elements) {
        if (elem->type == ASTNodeType::SPREAD_EXPR) {
            auto* spread = static_cast<SpreadExpr*>(elem.get());
            llvm::Value* arrVal = generateExpression(spread->operand.get());
            arrVal = toDefaultType(arrVal);
            llvm::Value* arrPtr =
                builder->CreateIntToPtr(arrVal, llvm::PointerType::getUnqual(*context), "spread.arrptr");
            llvm::Value* arrLen = builder->CreateLoad(getDefaultType(), arrPtr, "spread.len");
            totalLen = builder->CreateAdd(totalLen, arrLen, "spread.addlen");
            evalElems.push_back({arrVal, true, arrLen});
        } else {
            llvm::Value* elemVal = generateExpression(elem.get());
            elemVal = toDefaultType(elemVal);
            totalLen = builder->CreateAdd(totalLen, one, "spread.addone");
            evalElems.push_back({elemVal, false, nullptr});
        }
    }

    // Allocate result array: (totalLen + 1) * 8
    llvm::Value* slots = builder->CreateAdd(totalLen, one, "spread.slots");
    llvm::Value* bytes = builder->CreateMul(slots, eight, "spread.bytes");
    llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {bytes}, "spread.buf");
    builder->CreateStore(totalLen, buf);

    // Second pass: copy elements into the result array
    // We use an alloca to track the current write index
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::AllocaInst* writeIdx = createEntryBlockAlloca(function, "_spread_idx");
    builder->CreateStore(zero, writeIdx);

    for (const auto& ei : evalElems) {
        if (ei.isSpread) {
            // Copy all elements from the spread array
            llvm::Value* srcVal = ei.val;
            llvm::Value* srcPtr = builder->CreateIntToPtr(srcVal, llvm::PointerType::getUnqual(*context), "spread.src");
            llvm::Value* srcLen = ei.len;

            // Loop: copy srcLen elements
            llvm::BasicBlock* preheader = builder->GetInsertBlock();
            llvm::BasicBlock* loopBB = llvm::BasicBlock::Create(*context, "spread.copy.loop", function);
            llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "spread.copy.body", function);
            llvm::BasicBlock* doneBB = llvm::BasicBlock::Create(*context, "spread.copy.done", function);

            builder->CreateBr(loopBB);
            builder->SetInsertPoint(loopBB);
            llvm::PHINode* i = builder->CreatePHI(getDefaultType(), 2, "spread.i");
            i->addIncoming(zero, preheader);
            llvm::Value* cond = builder->CreateICmpSLT(i, srcLen, "spread.cond");
            builder->CreateCondBr(cond, bodyBB, doneBB);

            builder->SetInsertPoint(bodyBB);
            // Load element from source: srcPtr[i + 1]
            llvm::Value* srcIdx = builder->CreateAdd(i, one, "spread.srcidx");
            llvm::Value* srcElemPtr = builder->CreateGEP(getDefaultType(), srcPtr, srcIdx, "spread.srcelem");
            llvm::Value* elem = builder->CreateLoad(getDefaultType(), srcElemPtr, "spread.elem");
            // Store in dest: buf[writeIdx + 1]
            llvm::Value* curIdx = builder->CreateLoad(getDefaultType(), writeIdx, "spread.curidx");
            llvm::Value* dstIdx = builder->CreateAdd(curIdx, one, "spread.dstidx");
            llvm::Value* dstPtr = builder->CreateGEP(getDefaultType(), buf, dstIdx, "spread.dstptr");
            builder->CreateStore(elem, dstPtr);
            // Increment write index
            llvm::Value* newIdx = builder->CreateAdd(curIdx, one, "spread.newidx");
            builder->CreateStore(newIdx, writeIdx);
            // Increment loop counter
            llvm::Value* nextI = builder->CreateAdd(i, one, "spread.nexti");
            i->addIncoming(nextI, bodyBB);
            builder->CreateBr(loopBB);

            builder->SetInsertPoint(doneBB);
        } else {
            // Single element: store at buf[writeIdx + 1]
            llvm::Value* curIdx = builder->CreateLoad(getDefaultType(), writeIdx, "spread.curidx");
            llvm::Value* dstIdx = builder->CreateAdd(curIdx, one, "spread.dstidx");
            llvm::Value* dstPtr = builder->CreateGEP(getDefaultType(), buf, dstIdx, "spread.dstptr");
            builder->CreateStore(ei.val, dstPtr);
            llvm::Value* newIdx = builder->CreateAdd(curIdx, one, "spread.newidx");
            builder->CreateStore(newIdx, writeIdx);
        }
    }

    return builder->CreatePtrToInt(buf, getDefaultType(), "spread.result");
}

llvm::Value* CodeGenerator::generateIndex(IndexExpr* expr) {
    llvm::Value* arrVal = generateExpression(expr->array.get());
    llvm::Value* idxVal = generateExpression(expr->index.get());
    idxVal = toDefaultType(idxVal);

    // Detect whether the base expression is a string.  Strings are raw char
    // pointers without a length header; arrays have the layout [length, e0,
    // e1, ...] and each element is 8 bytes (i64).
    bool isStr = arrVal->getType()->isPointerTy() || isStringExpr(expr->array.get());

    // Convert i64 → pointer (strings may arrive as i64 via ptrtoint)
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    llvm::Value* basePtr =
        arrVal->getType()->isPointerTy() ? arrVal : builder->CreateIntToPtr(arrVal, ptrTy, "idx.baseptr");

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "idx.ok", function);
    llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "idx.fail", function);

    llvm::Value* lenVal;
    if (isStr) {
        // String: use strlen to get length — no length header.
        lenVal = builder->CreateCall(getOrDeclareStrlen(), {basePtr}, "idx.strlen");
    } else {
        // Array: length is stored in slot 0 of the buffer.
        lenVal = builder->CreateLoad(getDefaultType(), basePtr, "idx.len");
    }

    // Bounds check: 0 <= index < length
    llvm::Value* inBounds = builder->CreateICmpSLT(idxVal, lenVal, "idx.inbounds");
    llvm::Value* notNeg = builder->CreateICmpSGE(idxVal, llvm::ConstantInt::get(getDefaultType(), 0), "idx.notneg");
    llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "idx.valid");

    builder->CreateCondBr(valid, okBB, failBB);

    // Out-of-bounds path: print error and abort
    builder->SetInsertPoint(failBB);
    llvm::Value* errMsg =
        isStr ? builder->CreateGlobalString("Runtime error: string index out of bounds\n", "idx_str_oob_msg")
              : builder->CreateGlobalString("Runtime error: array index out of bounds\n", "idx_arr_oob_msg");
    builder->CreateCall(getPrintfFunction(), {errMsg});
    builder->CreateCall(getOrDeclareAbort());
    builder->CreateUnreachable();

    // Success path
    builder->SetInsertPoint(okBB);
    if (isStr) {
        // Load single byte at offset index, zero-extend to i64
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), basePtr, idxVal, "idx.charptr");
        llvm::Value* charVal = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "idx.char");
        return builder->CreateZExt(charVal, getDefaultType(), "idx.charext");
    }
    // Array: element is at slot (index + 1) in the i64 buffer.
    llvm::Value* offset = builder->CreateAdd(idxVal, llvm::ConstantInt::get(getDefaultType(), 1), "idx.offset");
    llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), basePtr, offset, "idx.elem.ptr");
    return builder->CreateLoad(getDefaultType(), elemPtr, "idx.elem");
}

llvm::Value* CodeGenerator::generateIndexAssign(IndexAssignExpr* expr) {
    llvm::Value* arrVal = generateExpression(expr->array.get());
    llvm::Value* idxVal = generateExpression(expr->index.get());
    llvm::Value* newVal = generateExpression(expr->value.get());
    newVal = toDefaultType(newVal);
    idxVal = toDefaultType(idxVal);

    // Detect whether the base is a string (raw char* without a length header).
    bool isStr = arrVal->getType()->isPointerTy() || isStringExpr(expr->array.get());

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    llvm::Value* basePtr =
        arrVal->getType()->isPointerTy() ? arrVal : builder->CreateIntToPtr(arrVal, ptrTy, "idxa.baseptr");

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "idxa.ok", function);
    llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "idxa.fail", function);

    llvm::Value* lenVal;
    if (isStr) {
        lenVal = builder->CreateCall(getOrDeclareStrlen(), {basePtr}, "idxa.strlen");
    } else {
        lenVal = builder->CreateLoad(getDefaultType(), basePtr, "idxa.len");
    }

    // Bounds check: 0 <= index < length
    llvm::Value* inBounds = builder->CreateICmpSLT(idxVal, lenVal, "idxa.inbounds");
    llvm::Value* notNeg = builder->CreateICmpSGE(idxVal, llvm::ConstantInt::get(getDefaultType(), 0), "idxa.notneg");
    llvm::Value* valid = builder->CreateAnd(inBounds, notNeg, "idxa.valid");

    builder->CreateCondBr(valid, okBB, failBB);

    // Out-of-bounds path: print error and abort
    builder->SetInsertPoint(failBB);
    llvm::Value* errMsg =
        isStr ? builder->CreateGlobalString("Runtime error: string index out of bounds\n", "idxa_str_oob_msg")
              : builder->CreateGlobalString("Runtime error: array index out of bounds\n", "idxa_arr_oob_msg");
    builder->CreateCall(getPrintfFunction(), {errMsg});
    builder->CreateCall(getOrDeclareAbort());
    builder->CreateUnreachable();

    // Success path
    builder->SetInsertPoint(okBB);
    if (isStr) {
        // Truncate to i8 and store at byte offset index
        llvm::Value* byteVal = builder->CreateTrunc(newVal, llvm::Type::getInt8Ty(*context), "idxa.byte");
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), basePtr, idxVal, "idxa.charptr");
        builder->CreateStore(byteVal, charPtr);
    } else {
        // Array: store i64 element at slot (index + 1)
        llvm::Value* offset = builder->CreateAdd(idxVal, llvm::ConstantInt::get(getDefaultType(), 1), "idxa.offset");
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), basePtr, offset, "idxa.elem.ptr");
        builder->CreateStore(newVal, elemPtr);
    }
    return newVal;
}


std::string CodeGenerator::resolveStructType(Expression* objExpr) const {
    if (objExpr->type == ASTNodeType::IDENTIFIER_EXPR) {
        auto* id = static_cast<IdentifierExpr*>(objExpr);
        auto vit = structVars_.find(id->name);
        if (vit != structVars_.end()) {
            return vit->second;
        }
    } else if (objExpr->type == ASTNodeType::STRUCT_LITERAL_EXPR) {
        return static_cast<StructLiteralExpr*>(objExpr)->structName;
    }
    // For function returns, parameters, and nested field access the struct
    // type is unknown at compile time in this dynamically typed language.
    return "";
}

size_t CodeGenerator::resolveFieldIndex(const std::string& structType, const std::string& fieldName,
                                        const ASTNode* errorNode) {
    // If we know the exact struct type, use it directly.
    if (!structType.empty()) {
        auto it = structDefs_.find(structType);
        if (it != structDefs_.end()) {
            for (size_t i = 0; i < it->second.size(); i++) {
                if (it->second[i] == fieldName) {
                    return i;
                }
            }
            codegenError("Struct '" + structType + "' has no field '" + fieldName + "'", errorNode);
        }
    }

    // Fallback: search all struct definitions.  Error if the field is
    // ambiguous (same name at different indices in multiple structs).
    size_t foundIdx = 0;
    bool found = false;
    bool ambiguous = false;
    for (auto& [sname, sfields] : structDefs_) {
        for (size_t i = 0; i < sfields.size(); i++) {
            if (sfields[i] == fieldName) {
                if (found && i != foundIdx) {
                    ambiguous = true;
                }
                foundIdx = i;
                found = true;
                break;
            }
        }
    }
    if (ambiguous) {
        codegenError("Ambiguous field '" + fieldName + "' exists at different indices in multiple structs", errorNode);
    }
    if (!found) {
        codegenError("Unknown field '" + fieldName + "'", errorNode);
    }
    return foundIdx;
}

llvm::Value* CodeGenerator::generateStructLiteral(StructLiteralExpr* expr) {
    auto it = structDefs_.find(expr->structName);
    if (it == structDefs_.end()) {
        codegenError("Unknown struct type '" + expr->structName + "'", expr);
    }
    const auto& fields = it->second;
    size_t numFields = fields.size();

    // Use stack allocation (alloca) for structs.  This avoids malloc overhead
    // and allows LLVM's mem2reg / SROA passes to promote small structs to
    // SSA registers, matching C's plain-variable performance.
    llvm::Function* curFn = builder->GetInsertBlock()->getParent();
    llvm::IRBuilder<> tmpBuilder(&curFn->getEntryBlock(), curFn->getEntryBlock().begin());
    llvm::Type* slotTy = getDefaultType();
    llvm::Value* ptr = tmpBuilder.CreateAlloca(
        llvm::ArrayType::get(slotTy, numFields), nullptr, "struct.alloca");
    // Cast to pointer for GEP
    ptr = builder->CreateBitOrPointerCast(ptr, llvm::PointerType::getUnqual(*context), "struct.ptr");

    // Build field name → index map
    std::unordered_map<std::string, size_t> fieldIndex;
    for (size_t i = 0; i < fields.size(); i++) {
        fieldIndex[fields[i]] = i;
    }

    // Initialize all fields to 0
    for (size_t i = 0; i < numFields; i++) {
        llvm::Value* elemPtr =
            builder->CreateGEP(getDefaultType(), ptr, llvm::ConstantInt::get(getDefaultType(), i), "struct.field.ptr");
        builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), elemPtr);
    }

    // Set provided field values
    for (auto& [fieldName, valueExpr] : expr->fieldValues) {
        auto fit = fieldIndex.find(fieldName);
        if (fit == fieldIndex.end()) {
            codegenError("Unknown field '" + fieldName + "' in struct '" + expr->structName + "'", expr);
        }
        llvm::Value* val = generateExpression(valueExpr.get());
        val = toDefaultType(val);
        llvm::Value* elemPtr = builder->CreateGEP(
            getDefaultType(), ptr, llvm::ConstantInt::get(getDefaultType(), fit->second), "struct.field.ptr");
        builder->CreateStore(val, elemPtr);
    }

    return builder->CreatePtrToInt(ptr, getDefaultType(), "struct.int");
}

llvm::Value* CodeGenerator::generateFieldAccess(FieldAccessExpr* expr) {
    std::string structType = resolveStructType(expr->object.get());
    size_t fieldIdx = resolveFieldIndex(structType, expr->fieldName, expr);

    llvm::Value* objVal = generateExpression(expr->object.get());
    objVal = toDefaultType(objVal);

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    llvm::Value* basePtr = builder->CreateIntToPtr(objVal, ptrTy, "struct.baseptr");
    llvm::Value* elemPtr = builder->CreateGEP(
        getDefaultType(), basePtr, llvm::ConstantInt::get(getDefaultType(), fieldIdx), "struct.field.load.ptr");

    // Determine alignment from struct field attributes.
    unsigned fieldAlign = 0;
    auto declIt = structFieldDecls_.find(structType);
    const FieldAttrs* attrs = nullptr;
    if (declIt != structFieldDecls_.end() && fieldIdx < declIt->second.size()) {
        attrs = &declIt->second[fieldIdx].attrs;
        if (attrs->align > 0) {
            fieldAlign = static_cast<unsigned>(attrs->align);
        }
    }

    llvm::LoadInst* load;
    if (fieldAlign > 0) {
        load = builder->CreateAlignedLoad(getDefaultType(), elemPtr,
                                          llvm::MaybeAlign(fieldAlign), "struct.field.val");
    } else {
        load = builder->CreateLoad(getDefaultType(), elemPtr, "struct.field.val");
    }

    // Apply LLVM metadata from struct field attributes.
    if (attrs) {
        // immut → !invariant.load metadata: tells LLVM the value never changes
        if (attrs->immut) {
            load->setMetadata(llvm::LLVMContext::MD_invariant_load,
                              llvm::MDNode::get(*context, {}));
        }
        // noalias → !noalias scope metadata (similar to borrow)
        if (attrs->noalias) {
            llvm::MDNode* domain = llvm::MDNode::getDistinct(*context,
                {llvm::MDString::get(*context, "struct.noalias.domain")});
            domain->replaceOperandWith(0, domain);
            llvm::MDNode* scope = llvm::MDNode::getDistinct(*context,
                {llvm::MDString::get(*context, "struct.noalias.scope")});
            scope->replaceOperandWith(0, scope);
            llvm::MDNode* scopeList = llvm::MDNode::get(*context, {scope});
            load->setMetadata(llvm::LLVMContext::MD_noalias, scopeList);
        }
        // range(min,max) → !range metadata for integer range propagation
        if (attrs->hasRange) {
            auto* i64Ty = llvm::Type::getInt64Ty(*context);
            llvm::Metadata* rangeMD[] = {
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(attrs->rangeMin))),
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(i64Ty, static_cast<uint64_t>(attrs->rangeMax) + 1))
            };
            load->setMetadata(llvm::LLVMContext::MD_range,
                              llvm::MDNode::get(*context, rangeMD));
        }
        // !nontemporal hint for cold fields — tells cache to skip caching
        if (attrs->cold) {
            llvm::Metadata* one[] = {
                llvm::ConstantAsMetadata::get(
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1))
            };
            load->setMetadata(llvm::LLVMContext::MD_nontemporal,
                              llvm::MDNode::get(*context, one));
        }
    }
    return load;
}

llvm::Value* CodeGenerator::generateFieldAssign(FieldAssignExpr* expr) {
    std::string structType = resolveStructType(expr->object.get());
    size_t fieldIdx = resolveFieldIndex(structType, expr->fieldName, expr);

    llvm::Value* objVal = generateExpression(expr->object.get());
    objVal = toDefaultType(objVal);
    llvm::Value* newVal = generateExpression(expr->value.get());
    newVal = toDefaultType(newVal);

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    llvm::Value* basePtr = builder->CreateIntToPtr(objVal, ptrTy, "struct.baseptr");
    llvm::Value* elemPtr = builder->CreateGEP(
        getDefaultType(), basePtr, llvm::ConstantInt::get(getDefaultType(), fieldIdx), "struct.field.store.ptr");

    // Apply alignment from struct field attributes.
    auto declIt = structFieldDecls_.find(structType);
    const FieldAttrs* attrs = nullptr;
    if (declIt != structFieldDecls_.end() && fieldIdx < declIt->second.size()) {
        attrs = &declIt->second[fieldIdx].attrs;
    }

    llvm::StoreInst* store;
    if (attrs && attrs->align > 0) {
        store = builder->CreateAlignedStore(newVal, elemPtr,
                                            llvm::MaybeAlign(static_cast<unsigned>(attrs->align)));
    } else {
        store = builder->CreateStore(newVal, elemPtr);
    }

    // !nontemporal hint for cold fields — bypass cache on write
    if (attrs && attrs->cold) {
        llvm::Metadata* one[] = {
            llvm::ConstantAsMetadata::get(
                llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1))
        };
        store->setMetadata(llvm::LLVMContext::MD_nontemporal,
                           llvm::MDNode::get(*context, one));
    }

    return newVal;
}

} // namespace omscript
