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
    return builder->CreateLoad(loadType, it->second, expr->name.c_str());
}

llvm::Value* CodeGenerator::generateBinary(BinaryExpr* expr) {
    // --- Compile-time string constant folding ---
    // When both operands of '+' are string literals, concatenate at compile time
    // to avoid runtime malloc+strcpy overhead.
    if (expr->op == "+") {
        auto* leftLit = dynamic_cast<LiteralExpr*>(expr->left.get());
        auto* rightLit = dynamic_cast<LiteralExpr*>(expr->right.get());
        if (leftLit && rightLit &&
            leftLit->literalType == LiteralExpr::LiteralType::STRING &&
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
    bool leftIsStr  = left->getType()->isPointerTy()  || isStringExpr(expr->left.get());
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
            // Float exponentiation: use llvm.pow intrinsic
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
    if (expr->op == "*" && (leftIsStr || rightIsStr)) {
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Value* strVal   = leftIsStr  ? left  : right;
        llvm::Value* countVal = leftIsStr  ? right : left;
        countVal = toDefaultType(countVal);
        // Clamp negative counts to 0 to prevent integer overflow in the
        // totalLen = strLen * count multiplication.
        llvm::Value* zero = llvm::ConstantInt::get(getDefaultType(), 0);
        llvm::Value* isNeg = builder->CreateICmpSLT(countVal, zero, "strmul.isneg");
        countVal = builder->CreateSelect(isNeg, zero, countVal, "strmul.clamp");
        llvm::Value* strPtr = strVal->getType()->isPointerTy()
                                  ? strVal
                                  : builder->CreateIntToPtr(strVal, ptrTy, "strmul.ptr");
        llvm::Value* strLen   = builder->CreateCall(getOrDeclareStrlen(), {strPtr}, "strmul.len");
        llvm::Value* totalLen = builder->CreateMul(strLen, countVal, "strmul.total");
        llvm::Value* allocSz  = builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "strmul.alloc");
        llvm::Value* buf      = builder->CreateCall(getOrDeclareMalloc(), {allocSz}, "strmul.buf");
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), buf);
        // Loop countVal times, appending strPtr each iteration.
        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* preHdr  = builder->GetInsertBlock();
        llvm::BasicBlock* loopBB  = llvm::BasicBlock::Create(*context, "strmul.loop", curFn);
        llvm::BasicBlock* bodyBB  = llvm::BasicBlock::Create(*context, "strmul.body", curFn);
        llvm::BasicBlock* doneBB  = llvm::BasicBlock::Create(*context, "strmul.done", curFn);
        llvm::Value* one  = llvm::ConstantInt::get(getDefaultType(), 1);
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
                return v->getType()->isPointerTy()
                           ? v
                           : builder->CreateIntToPtr(v, ptrTy, "strcmp.cast");
            }
            return v->getType()->isPointerTy()
                       ? v
                       : builder->CreateIntToPtr(v, ptrTy, "strcmp.cast");
        };
        llvm::Value* lPtr = toStrPtr(left,  leftIsStr);
        llvm::Value* rPtr = toStrPtr(right, rightIsStr);
        llvm::Value* cmpResult = builder->CreateCall(getOrDeclareStrcmp(), {lPtr, rPtr}, "strcmp.res");
        llvm::Value* zero32 = builder->getInt32(0);
        llvm::Value* cmpBool;
        if (expr->op == "==")
            cmpBool = builder->CreateICmpEQ(cmpResult,  zero32, "scmp.eq");
        else if (expr->op == "!=")
            cmpBool = builder->CreateICmpNE(cmpResult,  zero32, "scmp.ne");
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
        }
    }
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
        int64_t lv = ci->getSExtValue();
        if (lv == 0) {
            if (expr->op == "+" || expr->op == "|" || expr->op == "^")
                return right; // 0+x, 0|x, 0^x → x
            if (expr->op == "*" || expr->op == "&")
                return llvm::ConstantInt::get(getDefaultType(), 0); // 0*x, 0&x → 0
        }
        if (lv == 1 && expr->op == "*")
            return right; // 1*x → x
        if (lv == 1 && expr->op == "**")
            return llvm::ConstantInt::get(getDefaultType(), 1); // 1**x → 1
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
            default:
                return nullptr;
            }
        };
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            if (auto* result = emitShiftAdd(left, ci->getSExtValue()))
                return result;
        }
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
            if (auto* result = emitShiftAdd(right, ci->getSExtValue()))
                return result;
        }
        return builder->CreateMul(left, right, "multmp");
    } else if (expr->op == "/" || expr->op == "%") {
        bool isDivision = expr->op == "/";

        // Optimization for any non-zero constant divisor: the divisor can never
        // be zero at runtime, so we can skip the division-by-zero guard and emit
        // SDiv/SRem directly.  SDiv/SRem are used (not shift-based sequences)
        // because signed division must truncate toward zero, not toward -infinity.
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
        return builder->CreateShl(left, safeShift, "shltmp", /*HasNUW=*/false, /*HasNSW=*/true);
    } else if (expr->op == ">>") {
        // Mask shift amount to [0, 63] to prevent undefined behavior
        llvm::Value* mask = llvm::ConstantInt::get(getDefaultType(), 63);
        llvm::Value* safeShift = builder->CreateAnd(right, mask, "shrmask");
        return builder->CreateAShr(left, safeShift, "ashrtmp");
    } else if (expr->op == "**") {
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

    // Type conversion if the alloca type and value type differ
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second);
    if (alloca) {
        llvm::Type* allocaType = alloca->getAllocatedType();
        if (allocaType->isDoubleTy() && value->getType()->isIntegerTy()) {
            value = builder->CreateSIToFP(value, getFloatType(), "itof");
        } else if (allocaType->isIntegerTy() && value->getType()->isDoubleTy()) {
            value = builder->CreateFPToSI(value, getDefaultType(), "ftoi");
        } else if (allocaType->isIntegerTy() && value->getType()->isPointerTy()) {
            value = builder->CreatePtrToInt(value, getDefaultType(), "ptoi");
        } else if (allocaType->isPointerTy() && value->getType()->isIntegerTy()) {
            value = builder->CreateIntToPtr(value, llvm::PointerType::getUnqual(*context), "itop");
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

        llvm::Value* arrPtr = builder->CreateIntToPtr(arrVal, llvm::PointerType::getUnqual(*context), "incdec.arrptr");

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
    llvm::Value* basePtr = arrVal->getType()->isPointerTy()
                               ? arrVal
                               : builder->CreateIntToPtr(arrVal, ptrTy, "idx.baseptr");

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* okBB   = llvm::BasicBlock::Create(*context, "idx.ok",   function);
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
    llvm::Value* notNeg   = builder->CreateICmpSGE(idxVal, llvm::ConstantInt::get(getDefaultType(), 0), "idx.notneg");
    llvm::Value* valid    = builder->CreateAnd(inBounds, notNeg, "idx.valid");

    builder->CreateCondBr(valid, okBB, failBB);

    // Out-of-bounds path: print error and abort
    builder->SetInsertPoint(failBB);
    llvm::Value* errMsg = isStr
        ? builder->CreateGlobalString("Runtime error: string index out of bounds\n", "idx_str_oob_msg")
        : builder->CreateGlobalString("Runtime error: array index out of bounds\n",  "idx_arr_oob_msg");
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
    llvm::Value* offset  = builder->CreateAdd(idxVal, llvm::ConstantInt::get(getDefaultType(), 1), "idx.offset");
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
    llvm::Value* basePtr = arrVal->getType()->isPointerTy()
                               ? arrVal
                               : builder->CreateIntToPtr(arrVal, ptrTy, "idxa.baseptr");

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* okBB   = llvm::BasicBlock::Create(*context, "idxa.ok",   function);
    llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "idxa.fail", function);

    llvm::Value* lenVal;
    if (isStr) {
        lenVal = builder->CreateCall(getOrDeclareStrlen(), {basePtr}, "idxa.strlen");
    } else {
        lenVal = builder->CreateLoad(getDefaultType(), basePtr, "idxa.len");
    }

    // Bounds check: 0 <= index < length
    llvm::Value* inBounds = builder->CreateICmpSLT(idxVal, lenVal, "idxa.inbounds");
    llvm::Value* notNeg   = builder->CreateICmpSGE(idxVal, llvm::ConstantInt::get(getDefaultType(), 0), "idxa.notneg");
    llvm::Value* valid    = builder->CreateAnd(inBounds, notNeg, "idxa.valid");

    builder->CreateCondBr(valid, okBB, failBB);

    // Out-of-bounds path: print error and abort
    builder->SetInsertPoint(failBB);
    llvm::Value* errMsg = isStr
        ? builder->CreateGlobalString("Runtime error: string index out of bounds\n", "idxa_str_oob_msg")
        : builder->CreateGlobalString("Runtime error: array index out of bounds\n",  "idxa_arr_oob_msg");
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
        llvm::Value* offset  = builder->CreateAdd(idxVal, llvm::ConstantInt::get(getDefaultType(), 1), "idxa.offset");
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

    // Allocate: numFields * 8 bytes (each field is an i64 slot)
    llvm::Value* byteSize = llvm::ConstantInt::get(getDefaultType(), numFields * 8);
    llvm::Value* ptr = builder->CreateCall(getOrDeclareMalloc(), {byteSize}, "struct.alloc");

    // Build field name → index map
    std::unordered_map<std::string, size_t> fieldIndex;
    for (size_t i = 0; i < fields.size(); i++) {
        fieldIndex[fields[i]] = i;
    }

    // Initialize all fields to 0
    for (size_t i = 0; i < numFields; i++) {
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), ptr,
                                                   llvm::ConstantInt::get(getDefaultType(), i), "struct.field.ptr");
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
        llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), ptr,
                                                   llvm::ConstantInt::get(getDefaultType(), fit->second),
                                                   "struct.field.ptr");
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
    llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), basePtr,
                                               llvm::ConstantInt::get(getDefaultType(), fieldIdx),
                                               "struct.field.load.ptr");
    return builder->CreateLoad(getDefaultType(), elemPtr, "struct.field.val");
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
    llvm::Value* elemPtr = builder->CreateGEP(getDefaultType(), basePtr,
                                               llvm::ConstantInt::get(getDefaultType(), fieldIdx),
                                               "struct.field.store.ptr");
    builder->CreateStore(newVal, elemPtr);
    return newVal;
}

} // namespace omscript
