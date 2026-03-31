#include "codegen.h"
#include "diagnostic.h"
#include <climits>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Support/KnownBits.h>
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
        const std::string reason = (reasonIt != deadVarReason_.end()) ? reasonIt->second : "moved or invalidated";
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
                candidates.push_back(kv.getKey().str());
        }
        const std::string suggestion = suggestSimilar(expr->name, candidates);
        if (!suggestion.empty()) {
            msg += " (did you mean '" + suggestion + "'?)";
        }
        codegenError(msg, expr);
    }

    // Constant folding for `const` integer variables: return the constant
    // directly instead of emitting a load.  This allows downstream div/mod,
    // multiply, and comparison operations to see a ConstantInt and use the
    // fast urem/udiv path, NSWMul, and other constant-specific optimizations
    // that would otherwise only fire after LLVM's mem2reg pass.
    {
        auto foldIt = constIntFolds_.find(expr->name);
        if (foldIt != constIntFolds_.end()) {
            auto* ci = llvm::ConstantInt::get(getDefaultType(), foldIt->second);
            if (foldIt->second >= 0)
                nonNegValues_.insert(ci);
            return ci;
        }
    }

    // Register-promotion strategy: prefetched variables go straight to
    // registers (promoted by SROA/mem2reg) and stay there until invalidated.
    // No use-site llvm.prefetch is emitted on the alloca — that would anchor
    // the variable to memory and defeat register promotion.
    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second);

    llvm::Type* loadType = alloca ? alloca->getAllocatedType() : getDefaultType();
    auto* load = builder->CreateAlignedLoad(loadType, it->second,
        llvm::MaybeAlign(8), expr->name.c_str());

    // If this is a const variable or a prefetch-immut variable, mark the
    // load as invariant so LLVM knows the value never changes and can
    // hoist/CSE it aggressively.
    bool isInvariant = false;
    auto constIt = constValues.find(expr->name);
    if (constIt != constValues.end() && constIt->second) {
        isInvariant = true;
    }
    if (prefetchedImmutVars_.count(expr->name)) {
        isInvariant = true;
    }
    // Register variables are mutable; do not mark loads as invariant.
    if (isInvariant) {
        if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(load)) {
            loadInst->setMetadata(llvm::LLVMContext::MD_invariant_load,
                                  llvm::MDNode::get(*context, {}));
        }
    }
    // Track non-negativity: if the alloca is known to hold a non-negative
    // value (e.g., ascending for-loop counter), mark the loaded value.
    if (nonNegValues_.count(it->second)) {
        nonNegValues_.insert(load);
        // Add !range [0, INT64_MAX) metadata so LLVM IR-level passes (LVI,
        // CVP, InstCombine, loop unroller) see the non-negativity directly
        // in the IR without relying solely on llvm.assume intrinsics.  The
        // two approaches are complementary: the assume fires at the point in
        // the function body where it is emitted; the !range metadata travels
        // with every individual load and is visible to every pass that
        // processes the load instruction — including interprocedural passes
        // that inline the function and lose the assume context.
        if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(load)) {
            if (loadInst->getType()->isIntegerTy(64)
                    && optimizationLevel >= OptimizationLevel::O1) {
                loadInst->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            }
        }
    }
    return load;
}

llvm::Value* CodeGenerator::generateBinary(BinaryExpr* expr) {
    // --- Compile-time string constant folding ---
    // When both operands of '+' are string literals, concatenate at compile time
    // to avoid runtime malloc+strcpy overhead.
    if (expr->op == "+") {
        if (expr->left->type == ASTNodeType::LITERAL_EXPR && expr->right->type == ASTNodeType::LITERAL_EXPR) {
            auto* leftLit = static_cast<LiteralExpr*>(expr->left.get());
            auto* rightLit = static_cast<LiteralExpr*>(expr->right.get());
            if (leftLit->literalType == LiteralExpr::LiteralType::STRING &&
                rightLit->literalType == LiteralExpr::LiteralType::STRING) {
                const std::string folded = leftLit->stringValue + rightLit->stringValue;
                return builder->CreateGlobalString(folded, "strfold");
            }
        }
    }
    // --- End string constant folding ---

    llvm::Value* left = generateExpression(expr->left.get());
    if (expr->op == "&&" || expr->op == "||") {
        // Constant folding: when the left side is a known constant, we can
        // skip branch generation entirely and either short-circuit or
        // evaluate just the right side.
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
            const bool leftTrue = !ci->isZero();
            if (expr->op == "&&") {
                if (!leftTrue)
                    return llvm::ConstantInt::get(getDefaultType(), 0); // false && x → 0
                // true && x → bool(x)
                llvm::Value* right = generateExpression(expr->right.get());
                // Fold entirely when right side is also a constant.
                if (auto* ri = llvm::dyn_cast<llvm::ConstantInt>(right))
                    return llvm::ConstantInt::get(getDefaultType(), ri->isZero() ? 0 : 1);
                llvm::Value* rightBool = toBool(right);
                return builder->CreateZExt(rightBool, getDefaultType(), "booltmp");
            } else {
                if (leftTrue)
                    return llvm::ConstantInt::get(getDefaultType(), 1); // true || x → 1
                // false || x → bool(x)
                llvm::Value* right = generateExpression(expr->right.get());
                // Fold entirely when right side is also a constant.
                if (auto* ri = llvm::dyn_cast<llvm::ConstantInt>(right))
                    return llvm::ConstantInt::get(getDefaultType(), ri->isZero() ? 0 : 1);
                llvm::Value* rightBool = toBool(right);
                return builder->CreateZExt(rightBool, getDefaultType(), "booltmp");
            }
        }
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
        // Constant folding: when the left side is a known constant, skip branches.
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
            if (!ci->isZero())
                return left;  // nonzero ?? y → nonzero
            // 0 ?? y → y
            llvm::Value* right = generateExpression(expr->right.get());
            return toDefaultType(right);
        }
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

    // -----------------------------------------------------------------------
    // SIMD vector operations — when either operand is a vector type,
    // dispatch to LLVM vector arithmetic instructions.
    // -----------------------------------------------------------------------
    const bool leftIsVec = left->getType()->isVectorTy();
    const bool rightIsVec = right->getType()->isVectorTy();
    if (leftIsVec || rightIsVec) {
        // Both operands must be the same vector type for arithmetic.
        if (leftIsVec && rightIsVec && left->getType() != right->getType()) {
            codegenError("SIMD vector type mismatch in '" + expr->op + "' operation", expr);
        }
        // If one side is scalar, splat it to match the vector type.
        if (leftIsVec && !rightIsVec) {
            right = splatScalarToVector(right, left->getType());
        } else if (rightIsVec && !leftIsVec) {
            left = splatScalarToVector(left, right->getType());
        }

        auto* vecTy = llvm::cast<llvm::FixedVectorType>(left->getType());
        const bool isFloatVec = vecTy->getElementType()->isFloatingPointTy();

        if (expr->op == "+") return isFloatVec ? builder->CreateFAdd(left, right, "simd.fadd")
                                               : builder->CreateAdd(left, right, "simd.add");
        if (expr->op == "-") return isFloatVec ? builder->CreateFSub(left, right, "simd.fsub")
                                               : builder->CreateSub(left, right, "simd.sub");
        if (expr->op == "*") return isFloatVec ? builder->CreateFMul(left, right, "simd.fmul")
                                               : builder->CreateMul(left, right, "simd.mul");
        if (expr->op == "/") return isFloatVec ? builder->CreateFDiv(left, right, "simd.fdiv")
                                               : builder->CreateSDiv(left, right, "simd.sdiv");
        if (expr->op == "%") {
            if (isFloatVec) return builder->CreateFRem(left, right, "simd.frem");
            return builder->CreateSRem(left, right, "simd.srem");
        }
        // Bitwise operations on integer vectors
        if (!isFloatVec) {
            if (expr->op == "&") return builder->CreateAnd(left, right, "simd.and");
            if (expr->op == "|") return builder->CreateOr(left, right, "simd.or");
            if (expr->op == "^") return builder->CreateXor(left, right, "simd.xor");
            if (expr->op == "<<") return builder->CreateShl(left, right, "simd.shl");
            if (expr->op == ">>") return builder->CreateAShr(left, right, "simd.ashr");
        }
        codegenError("Unsupported operator '" + expr->op + "' for SIMD vector types", expr);
    }

    // -----------------------------------------------------------------------
    // Operator overloading — dispatch to user-defined operator functions
    // when both operands are known struct types with a matching overload.
    // -----------------------------------------------------------------------
    {
        std::string leftStructType;
        if (expr->left->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto* id = static_cast<IdentifierExpr*>(expr->left.get());
            auto vit = structVars_.find(id->name);
            if (vit != structVars_.end()) leftStructType = vit->second;
        } else if (expr->left->type == ASTNodeType::STRUCT_LITERAL_EXPR) {
            leftStructType = static_cast<StructLiteralExpr*>(expr->left.get())->structName;
        }
        if (!leftStructType.empty()) {
            const std::string key = leftStructType + "::" + expr->op;
            auto overIt = operatorOverloads_.find(key);
            if (overIt != operatorOverloads_.end()) {
                // Call the operator overload function with (self=left, other=right).
                llvm::Function* opFunc = module->getFunction(overIt->second);
                if (opFunc) {
                    return builder->CreateCall(opFunc, {left, right}, "op.result");
                }
            }
        }
    }

    const bool leftIsFloat = left->getType()->isDoubleTy();
    const bool rightIsFloat = right->getType()->isDoubleTy();

    // Pre-compute string flags once, used by both the float-skip guard below
    // and the string concatenation block that follows.
    const bool leftIsStr = left->getType()->isPointerTy() || isStringExpr(expr->left.get());
    const bool rightIsStr = right->getType()->isPointerTy() || isStringExpr(expr->right.get());

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
                const double lv = lc->getValueAPF().convertToDouble();
                const double rv = rc->getValueAPF().convertToDouble();
                if (expr->op == "+")
                    return llvm::ConstantFP::get(getFloatType(), lv + rv);
                if (expr->op == "-")
                    return llvm::ConstantFP::get(getFloatType(), lv - rv);
                if (expr->op == "*")
                    return llvm::ConstantFP::get(getFloatType(), lv * rv);
                if (expr->op == "/" && rv != 0.0)
                    return llvm::ConstantFP::get(getFloatType(), lv / rv);
                if (expr->op == "%" && rv != 0.0)
                    return llvm::ConstantFP::get(getFloatType(), std::fmod(lv, rv));
                if (expr->op == "**")
                    return llvm::ConstantFP::get(getFloatType(), std::pow(lv, rv));
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
            const double rv = rc->getValueAPF().convertToDouble();
            if (rv == 0.0 && (expr->op == "+" || expr->op == "-"))
                return left;  // x+0.0, x-0.0 → x
            if (rv == 1.0 && (expr->op == "*" || expr->op == "/"))
                return left;  // x*1.0, x/1.0 → x
            if (rv == -1.0 && expr->op == "*")
                return builder->CreateFNeg(left, "fnegtmp"); // x*(-1.0) → -x
            if (rv == -1.0 && expr->op == "/")
                return builder->CreateFNeg(left, "fnegtmp"); // x/(-1.0) → -x
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
                const double abs_rv = rv < 0 ? -rv : rv;
                bool isPow2 = false;
                if (abs_rv >= 1.0 && abs_rv <= 4503599627370496.0) { // 2^52 max exact int in double
                    auto u = static_cast<uint64_t>(abs_rv);
                    // Verify the cast is exact (no truncation of fractional part)
                    isPow2 = (static_cast<double>(u) == abs_rv) && u > 0 && (u & (u - 1)) == 0;
                } else if (abs_rv > 0.0 && abs_rv < 1.0) {
                    const double inv = 1.0 / abs_rv;
                    auto u = static_cast<uint64_t>(inv);
                    isPow2 = (static_cast<double>(u) == inv) && u > 0 && (u & (u - 1)) == 0;
                }
                if (isPow2) {
                    const double recip = 1.0 / rv;
                    return builder->CreateFMul(left, llvm::ConstantFP::get(getFloatType(), recip), "fdivrecip");
                }
            }
        }
        if (auto* lc = llvm::dyn_cast<llvm::ConstantFP>(left)) {
            const double lv = lc->getValueAPF().convertToDouble();
            if (lv == 0.0 && expr->op == "+")
                return right; // 0.0+x → x
            if (lv == 0.0 && expr->op == "-")
                return builder->CreateFNeg(right, "fnegtmp"); // 0.0-x → -x
            if (lv == 1.0 && expr->op == "*")
                return right; // 1.0*x → x
            if (lv == -1.0 && expr->op == "*")
                return builder->CreateFNeg(right, "fnegtmp"); // (-1.0)*x → -x
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
                const double rv = rc->getValueAPF().convertToDouble();
                if (rv == 0.5) {
                    // x**0.5 → sqrt(x): single-instruction latency vs llvm.pow call
                    llvm::Function* sqrtFn =
                        OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sqrt, {llvm::Type::getDoubleTy(*context)});
                    return builder->CreateCall(sqrtFn, {left}, "fsqrt");
                }
                if (rv == 0.25) {
                    // x**0.25 → sqrt(sqrt(x)): 2 sqrtsd instructions vs pow call
                    llvm::Function* sqrtFn =
                        OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sqrt, {llvm::Type::getDoubleTy(*context)});
                    auto* inner = builder->CreateCall(sqrtFn, {left}, "fsqrt.inner");
                    return builder->CreateCall(sqrtFn, {inner}, "fsqrt.outer");
                }
                if (rv == -0.5) {
                    // x**(-0.5) → 1.0/sqrt(x): sqrtsd + fdiv vs pow call
                    llvm::Function* sqrtFn =
                        OMSC_GET_INTRINSIC(module.get(), llvm::Intrinsic::sqrt, {llvm::Type::getDoubleTy(*context)});
                    auto* sq = builder->CreateCall(sqrtFn, {left}, "fsqrt.inv");
                    return builder->CreateFDiv(llvm::ConstantFP::get(getFloatType(), 1.0), sq, "frsqrt");
                }
                if (rv == -1.0) {
                    // x**(-1.0) → 1.0/x: single fdiv vs pow call
                    return builder->CreateFDiv(llvm::ConstantFP::get(getFloatType(), 1.0), left, "frecip");
                }
                if (rv == 3.0) {
                    // x**3.0 → x*x*x  (2 fmuls vs pow call)
                    auto* sq = builder->CreateFMul(left, left, "fpow3.sq");
                    return builder->CreateFMul(sq, left, "fpow3");
                }
                if (rv == 4.0) {
                    // x**4.0 → (x*x)*(x*x)  (2 fmuls vs pow call, better than 3)
                    auto* sq = builder->CreateFMul(left, left, "fpow4.sq");
                    return builder->CreateFMul(sq, sq, "fpow4");
                }
                if (rv == 5.0) {
                    // x**5.0 → (x*x)*(x*x)*x  (3 fmuls vs pow call)
                    auto* sq = builder->CreateFMul(left, left, "fpow5.sq");
                    auto* q4 = builder->CreateFMul(sq, sq, "fpow5.q4");
                    return builder->CreateFMul(q4, left, "fpow5");
                }
                if (rv == 6.0) {
                    // x**6.0 → ((x*x)*x)²  (3 fmuls vs pow call)
                    auto* sq = builder->CreateFMul(left, left, "fpow6.sq");
                    auto* cb = builder->CreateFMul(sq, left, "fpow6.cb");
                    return builder->CreateFMul(cb, cb, "fpow6");
                }
                if (rv == 7.0) {
                    // x**7.0 → ((x*x)*x)² * x  (4 fmuls vs pow call)
                    auto* sq = builder->CreateFMul(left, left, "fpow7.sq");
                    auto* cb = builder->CreateFMul(sq, left, "fpow7.cb");
                    auto* p6 = builder->CreateFMul(cb, cb, "fpow7.p6");
                    return builder->CreateFMul(p6, left, "fpow7");
                }
                if (rv == 8.0) {
                    // x**8.0 → ((x*x)*(x*x))²  (3 fmuls vs pow call)
                    auto* sq = builder->CreateFMul(left, left, "fpow8.sq");
                    auto* q4 = builder->CreateFMul(sq, sq, "fpow8.q4");
                    return builder->CreateFMul(q4, q4, "fpow8");
                }
                if (rv == -2.0) {
                    // x**(-2.0) → 1.0/(x*x)  (fmul + fdiv vs pow call)
                    auto* sq = builder->CreateFMul(left, left, "fpow_n2.sq");
                    return builder->CreateFDiv(llvm::ConstantFP::get(getFloatType(), 1.0), sq, "fpow_n2");
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

        // Guard against multiplication overflow: strLen * countVal could wrap
        // around the 64-bit range, causing a tiny allocation followed by a
        // heap-buffer-overflow in the strcat loop.  Use umul.with.overflow to
        // detect this and abort with a clear error message.
        llvm::Function* overflowIntrinsic = OMSC_GET_INTRINSIC(
            module.get(), llvm::Intrinsic::umul_with_overflow, {getDefaultType()});
        llvm::Value* mulResult = builder->CreateCall(overflowIntrinsic, {strLen, countVal}, "strmul.ovf");
        llvm::Value* totalLen = builder->CreateExtractValue(mulResult, 0, "strmul.total");
        llvm::Value* overflowed = builder->CreateExtractValue(mulResult, 1, "strmul.ovflag");

        llvm::Function* curFn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* ovfBB = llvm::BasicBlock::Create(*context, "strmul.overflow", curFn);
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "strmul.ok", curFn);
        builder->CreateCondBr(overflowed, ovfBB, okBB);

        builder->SetInsertPoint(ovfBB);
        llvm::Value* ovfMsg = builder->CreateGlobalString(
            "Runtime error: string repetition size overflow\n", "strmul_ovf_msg");
        builder->CreateCall(getPrintfFunction(), {ovfMsg});
        builder->CreateCall(getOrDeclareExit(),
            {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 1)});
        builder->CreateUnreachable();

        builder->SetInsertPoint(okBB);
        llvm::Value* allocSz =
            builder->CreateAdd(totalLen, llvm::ConstantInt::get(getDefaultType(), 1), "strmul.alloc");
        llvm::Value* buf = builder->CreateCall(getOrDeclareMalloc(), {allocSz}, "strmul.buf");
        // Null-terminate first byte so strcat works from empty buffer
        builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(*context), 0), buf);
        // Loop countVal times, strcat'ing strPtr each iteration.
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
        auto* strmulBackBr = builder->CreateBr(loopBB);
        if (optimizationLevel >= OptimizationLevel::O1) {
            llvm::SmallVector<llvm::Metadata*, 2> mds;
            mds.push_back(nullptr);
            mds.push_back(llvm::MDNode::get(*context,
                {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
            llvm::MDNode* md = llvm::MDNode::get(*context, mds);
            md->replaceOperandWith(0, md);
            strmulBackBr->setMetadata(llvm::LLVMContext::MD_loop, md);
        }
        builder->SetInsertPoint(doneBB);
        return buf;
    }


    // for all relational and equality operators.  Without this, the ptr→int
    // fallback below would compare raw memory addresses instead of lexicographic
    // order, giving results that depend on allocation order rather than content.
    if (leftIsStr || rightIsStr) {
        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        auto toStrPtr = [&](llvm::Value* v) -> llvm::Value* {
            return v->getType()->isPointerTy() ? v : builder->CreateIntToPtr(v, ptrTy, "strcmp.cast");
        };
        llvm::Value* lPtr = toStrPtr(left);
        llvm::Value* rPtr = toStrPtr(right);
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
        const unsigned leftBits = left->getType()->getIntegerBitWidth();
        const unsigned rightBits = right->getType()->getIntegerBitWidth();
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
        const int64_t lval = leftConst->getSExtValue();
        const int64_t rval = rightConst->getSExtValue();
        // Use unsigned arithmetic for +, -, * to avoid signed overflow UB.
        // The unsigned result, when reinterpreted as signed, gives the correct
        // two's-complement wrapping behavior that matches LLVM's add/sub/mul.
        auto ulval = static_cast<uint64_t>(lval);
        auto urval = static_cast<uint64_t>(rval);

        if (expr->op == "+") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, ulval + urval));
        } else if (expr->op == "-") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, ulval - urval));
        } else if (expr->op == "*") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, ulval * urval));
        } else if (expr->op == "/") {
            if (rval != 0 && (lval != INT64_MIN || rval != -1)) {
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval / rval));
            }
        } else if (expr->op == "%") {
            if (rval != 0 && (lval != INT64_MIN || rval != -1)) {
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
        } else if (expr->op == "&&") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, (lval != 0 && rval != 0) ? 1 : 0));
        } else if (expr->op == "||") {
            return llvm::ConstantInt::get(*context, llvm::APInt(64, (lval != 0 || rval != 0) ? 1 : 0));
        } else if (expr->op == "<<") {
            if (rval >= 0 && rval < 64)
                // Use unsigned shift to avoid UB when lval is negative.
                return llvm::ConstantInt::get(*context, llvm::APInt(64, ulval << static_cast<unsigned>(rval)));
        } else if (expr->op == ">>") {
            if (rval >= 0 && rval < 64)
                // Arithmetic (signed) right shift: sign-extending, matching
                // LLVM's AShr semantics.  C++ guarantees arithmetic shift for
                // signed types since C++20; pre-C++20 it is implementation-
                // defined but every supported compiler uses arithmetic shift.
                return llvm::ConstantInt::get(*context, llvm::APInt(64, lval >> rval));
        } else if (expr->op == "**") {
            if (rval >= 0) {
                int64_t result = 1;
                bool overflow = false;
                for (int64_t i = 0; i < rval; i++) {
                    if (lval != 0 && lval != 1 && lval != -1) {
                        const uint64_t ab = (lval < 0) ? static_cast<uint64_t>(-static_cast<uint64_t>(lval))
                                                 : static_cast<uint64_t>(lval);
                        const uint64_t ar = (result < 0) ? static_cast<uint64_t>(-static_cast<uint64_t>(result))
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
                // Negative exponent: base**(-n) = 1 / base**n in integer math.
                // |base| > 1 → truncates to 0; base=1 → 1; base=-1 → ±1.
                if (lval == 1)
                    return llvm::ConstantInt::get(*context, llvm::APInt(64, 1));
                if (lval == -1)
                    return llvm::ConstantInt::get(*context, llvm::APInt(64, (rval & 1) ? -1 : 1));
                return llvm::ConstantInt::get(*context, llvm::APInt(64, 0));
            }
        }
    }

    // Algebraic identity optimizations — when one operand is a known constant,
    // many operations can be simplified without emitting any instruction.
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
        const int64_t rv = ci->getSExtValue();
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
        if (rv == -1 && expr->op == "/")
            return builder->CreateNeg(left, "negtmp"); // x/(-1) → -x
        // x & -1 (all ones) → x
        if (rv == -1 && expr->op == "&")
            return left;
        // x | -1 (all ones) → -1
        if (rv == -1 && expr->op == "|")
            return llvm::ConstantInt::get(getDefaultType(), -1);
    }
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
        const int64_t lv = ci->getSExtValue();
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
        if (lv == -1 && expr->op == "**") {
            // (-1)**x → 1 if x is even, -1 if x is odd
            llvm::Value* bit = builder->CreateAnd(right, llvm::ConstantInt::get(getDefaultType(), 1), "pow.bit");
            llvm::Value* isOdd = builder->CreateICmpNE(bit, llvm::ConstantInt::get(getDefaultType(), 0), "pow.isodd");
            return builder->CreateSelect(isOdd, llvm::ConstantInt::get(getDefaultType(), -1),
                                         llvm::ConstantInt::get(getDefaultType(), 1), "pow.negone");
        }
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
    // Integer arithmetic generally uses wrapping (no NSW/NUW flags) because
    // NSW/NUW tell LLVM that overflow is undefined behavior, which can cause
    // miscompilation when overflow actually occurs.
    //
    // Exception: when BOTH operands are provably non-negative (tracked via
    // nonNegValues_), we set both nsw and nuw on addition.  For two non-negative
    // i64 values, signed overflow cannot occur unless the result exceeds
    // INT64_MAX (nsw), and unsigned overflow cannot occur either: the sum of
    // two values in [0, INT64_MAX] is at most 2·INT64_MAX = 2^64−2, which is
    // strictly less than UINT64_MAX = 2^64−1 (nuw).  The nuw flag additionally
    // enables LLVM's loop unroller to propagate non-negativity to unrolled
    // copies and allows SCEV to use unsigned induction variable ranges.
    if (expr->op == "+") {
        const bool leftNonNeg = nonNegValues_.count(left);
        const bool rightNonNeg = nonNegValues_.count(right);
        const bool bothOperandsNonNeg = leftNonNeg && rightNonNeg;
        // Also detect non-negative constant operands for NSW/NUW/tracking.
        bool constNonNeg = false;
        if (!bothOperandsNonNeg) {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right))
                constNonNeg = leftNonNeg && !ci->isNegative();
            else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left))
                constNonNeg = rightNonNeg && !ci->isNegative();
        }
        // Both nsw and nuw are safe: for non-negative operands, signed
        // overflow cannot occur (each value fits in [0, INT64_MAX]), and
        // their sum is at most 2·INT64_MAX = 2^64−2 < UINT64_MAX so unsigned
        // overflow cannot occur either.
        const bool canNSWNUW = bothOperandsNonNeg || constNonNeg;
        auto* result = canNSWNUW
            ? builder->CreateAdd(left, right, "addtmp", /*HasNUW=*/true, /*HasNSW=*/true)
            : builder->CreateAdd(left, right, "addtmp");
        // Track non-negativity: if both operands are known non-negative
        // (including constant operands), the result is non-negative
        // (assuming no overflow, which is true for typical loop counter
        // arithmetic).
        if (canNSWNUW)
            nonNegValues_.insert(result);
        return result;
    } else if (expr->op == "-") {
        // When both operands are non-negative, the subtraction of two
        // non-negative values cannot produce signed overflow: the result
        // is in the range [-(2^63-1), 2^63-1], which is representable
        // in i64 without wrapping.  The nsw flag enables SCEV to compute
        // tighter trip counts for countdown loops and proves induction
        // variable monotonicity.
        const bool leftNonNeg = nonNegValues_.count(left);
        const bool rightNonNeg = nonNegValues_.count(right);
        const bool bothNonNeg = leftNonNeg && rightNonNeg;
        // Also detect constant non-negative operands — mirrors the
        // addition logic.  Subtracting a non-negative constant from a
        // non-negative value (or vice versa) cannot overflow i64.
        bool constNSW = false;
        if (!bothNonNeg) {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right))
                constNSW = leftNonNeg && !ci->isNegative();
            else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left))
                constNSW = rightNonNeg && !ci->isNegative();
        }
        const bool canNSW = bothNonNeg || constNSW;
        return canNSW ? builder->CreateNSWSub(left, right, "subtmp")
                      : builder->CreateSub(left, right, "subtmp");
    } else if (expr->op == "*") {
        // Strength reduction: multiply by power of 2 → left shift
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            const int s = log2IfPowerOf2(ci->getSExtValue());
            if (s >= 0) {
                const bool leftNonNeg = nonNegValues_.count(left);
                // For non-negative base × positive power-of-2: emit NSWMul so
                // LLVM's SCEV can track the result range (shift loses flags).
                if (leftNonNeg) {
                    auto* result = builder->CreateNSWMul(left, right, "multmp");
                    nonNegValues_.insert(result);
                    return result;
                }
                return builder->CreateShl(left, llvm::ConstantInt::get(getDefaultType(), s), "shltmp");
            }
        }
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
            const int s = log2IfPowerOf2(ci->getSExtValue());
            if (s >= 0) {
                const bool rightNonNeg = nonNegValues_.count(right);
                if (rightNonNeg) {
                    auto* result = builder->CreateNSWMul(right, left, "multmp");
                    nonNegValues_.insert(result);
                    return result;
                }
                return builder->CreateShl(right, llvm::ConstantInt::get(getDefaultType(), s), "shltmp");
            }
        }
        // Strength reduction: multiply by small non-power-of-2 constants
        // to shift+add/sub sequences (faster on many microarchitectures).
        // n*3 → (n<<1)+n, n*5 → (n<<2)+n, n*7 → (n<<3)-n, n*9 → (n<<3)+n
        // n*10 → (n<<3)+(n<<1), n*15 → (n<<4)-n, n*17 → (n<<4)+n
        // When baseNonNeg is true, the base operand is provably non-negative
        // so intermediate add/sub operations set nsw — this enables SCEV
        // to compute tighter ranges for induction variable analysis.
        auto emitShiftAdd = [&](llvm::Value* base, int64_t multiplier, bool baseNonNeg = false) -> llvm::Value* {
            const bool nf = false;  // nuw: never set on these (could wrap unsigned)
            const bool ns = baseNonNeg;  // nsw: safe when base >= 0
            switch (multiplier) {
            case 3: {
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul3.shl");
                return builder->CreateAdd(shl, base, "mul3", nf, ns);
            }
            case 5: {
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul5.shl");
                return builder->CreateAdd(shl, base, "mul5", nf, ns);
            }
            case 7: {
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul7.shl");
                return builder->CreateSub(shl, base, "mul7", nf, ns);
            }
            case 9: {
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul9.shl");
                return builder->CreateAdd(shl, base, "mul9", nf, ns);
            }
            case 10: {
                // n*10 → (n<<3) + (n<<1)
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul10.shl3");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul10.shl1");
                return builder->CreateAdd(shl3, shl1, "mul10", nf, ns);
            }
            case 15: {
                // n*15 → (n<<4) - n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul15.shl");
                return builder->CreateSub(shl, base, "mul15", nf, ns);
            }
            case 17: {
                // n*17 → (n<<4) + n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul17.shl");
                return builder->CreateAdd(shl, base, "mul17", nf, ns);
            }
            case 6: {
                // n*6 → (n<<2) + (n<<1)  (= n*4 + n*2)
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul6.shl2");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul6.shl1");
                return builder->CreateAdd(shl2, shl1, "mul6", nf, ns);
            }
            case 12: {
                // n*12 → (n<<3) + (n<<2)  (= n*8 + n*4)
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul12.shl3");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul12.shl2");
                return builder->CreateAdd(shl3, shl2, "mul12", nf, ns);
            }
            case 24: {
                // n*24 → (n<<5) - (n<<3)  (= n*32 - n*8)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul24.shl5");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul24.shl3");
                return builder->CreateSub(shl5, shl3, "mul24", nf, ns);
            }
            case 25: {
                // n*25 → ((n<<5) - (n<<3)) + n  (= (n*32 - n*8) + n = n*24 + n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul25.shl5");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul25.shl3");
                auto* t = builder->CreateSub(shl5, shl3, "mul25.t", nf, ns);
                return builder->CreateAdd(t, base, "mul25", nf, ns);
            }
            case 31: {
                // n*31 → (n<<5) - n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul31.shl");
                return builder->CreateSub(shl, base, "mul31", nf, ns);
            }
            case 33: {
                // n*33 → (n<<5) + n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul33.shl");
                return builder->CreateAdd(shl, base, "mul33", nf, ns);
            }
            case 63: {
                // n*63 → (n<<6) - n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 6), "mul63.shl");
                return builder->CreateSub(shl, base, "mul63", nf, ns);
            }
            case 65: {
                // n*65 → (n<<6) + n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 6), "mul65.shl");
                return builder->CreateAdd(shl, base, "mul65", nf, ns);
            }
            case 127: {
                // n*127 → (n<<7) - n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 7), "mul127.shl");
                return builder->CreateSub(shl, base, "mul127", nf, ns);
            }
            case 255: {
                // n*255 → (n<<8) - n
                auto* shl = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 8), "mul255.shl");
                return builder->CreateSub(shl, base, "mul255", nf, ns);
            }
            case 1000: {
                // n*1000 → (n<<10) - (n<<5) + (n<<3)  (= 1024n - 32n + 8n)
                auto* shl10 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 10), "mul1000.shl10");
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul1000.shl5");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul1000.shl3");
                auto* t = builder->CreateSub(shl10, shl5, "mul1000.t", nf, ns);
                return builder->CreateAdd(t, shl3, "mul1000", nf, ns);
            }
            case 100: {
                // n*100 → (n<<7) - (n<<5) + (n<<2)  (= 128n - 32n + 4n)
                auto* shl7 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 7), "mul100.shl7");
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul100.shl5");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul100.shl2");
                auto* t = builder->CreateSub(shl7, shl5, "mul100.t", nf, ns);
                return builder->CreateAdd(t, shl2, "mul100", nf, ns);
            }
            case 11: {
                // n*11 → (n<<3) + (n<<1) + n  (= 8n + 2n + n)
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul11.shl3");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul11.shl1");
                auto* t = builder->CreateAdd(shl3, shl1, "mul11.t", nf, ns);
                return builder->CreateAdd(t, base, "mul11", nf, ns);
            }
            case 13: {
                // n*13 → (n<<4) - (n<<1) - n  (= 16n - 2n - n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul13.shl4");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul13.shl1");
                auto* t = builder->CreateSub(shl4, shl1, "mul13.t", nf, ns);
                return builder->CreateSub(t, base, "mul13", nf, ns);
            }
            case 20: {
                // n*20 → (n<<4) + (n<<2)  (= 16n + 4n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul20.shl4");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul20.shl2");
                return builder->CreateAdd(shl4, shl2, "mul20", nf, ns);
            }
            case 21: {
                // n*21 → (n<<4) + (n<<2) + n  (= 16n + 4n + n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul21.shl4");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul21.shl2");
                auto* t = builder->CreateAdd(shl4, shl2, "mul21.t", nf, ns);
                return builder->CreateAdd(t, base, "mul21", nf, ns);
            }
            case 14: {
                // n*14 → (n<<4) - (n<<1)  (= 16n - 2n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul14.shl4");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul14.shl1");
                return builder->CreateSub(shl4, shl1, "mul14", nf, ns);
            }
            case 28: {
                // n*28 → (n<<5) - (n<<2)  (= 32n - 4n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul28.shl5");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul28.shl2");
                return builder->CreateSub(shl5, shl2, "mul28", nf, ns);
            }
            case 60: {
                // n*60 → (n<<6) - (n<<2)  (= 64n - 4n)
                auto* shl6 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 6), "mul60.shl6");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul60.shl2");
                return builder->CreateSub(shl6, shl2, "mul60", nf, ns);
            }
            case 96: {
                // n*96 → (n<<7) - (n<<5)  (= 128n - 32n)
                auto* shl7 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 7), "mul96.shl7");
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul96.shl5");
                return builder->CreateSub(shl7, shl5, "mul96", nf, ns);
            }
            case 120: {
                // n*120 → (n<<7) - (n<<3)  (= 128n - 8n)
                auto* shl7 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 7), "mul120.shl7");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul120.shl3");
                return builder->CreateSub(shl7, shl3, "mul120", nf, ns);
            }
            case 18: {
                // n*18 → (n<<4) + (n<<1)  (= 16n + 2n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul18.shl4");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul18.shl1");
                return builder->CreateAdd(shl4, shl1, "mul18", nf, ns);
            }
            case 36: {
                // n*36 → (n<<5) + (n<<2)  (= 32n + 4n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul36.shl5");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul36.shl2");
                return builder->CreateAdd(shl5, shl2, "mul36", nf, ns);
            }
            case 37: {
                // n*37 → (n<<5) + (n<<2) + n  (= 32n + 4n + n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul37.shl5");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul37.shl2");
                auto* t = builder->CreateAdd(shl5, shl2, "mul37.t", nf, ns);
                return builder->CreateAdd(t, base, "mul37", nf, ns);
            }
            case 40: {
                // n*40 → (n<<5) + (n<<3)  (= 32n + 8n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul40.shl5");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul40.shl3");
                return builder->CreateAdd(shl5, shl3, "mul40", nf, ns);
            }
            case 41: {
                // n*41 → (n<<5) + (n<<3) + n  (= 32n + 8n + n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul41.shl5");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul41.shl3");
                auto* t = builder->CreateAdd(shl5, shl3, "mul41.t", nf, ns);
                return builder->CreateAdd(t, base, "mul41", nf, ns);
            }
            case 48: {
                // n*48 → (n<<5) + (n<<4)  (= 32n + 16n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul48.shl5");
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul48.shl4");
                return builder->CreateAdd(shl5, shl4, "mul48", nf, ns);
            }
            case 49: {
                // n*49 → (n<<5) + (n<<4) + n  (= 32n + 16n + n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul49.shl5");
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul49.shl4");
                auto* t = builder->CreateAdd(shl5, shl4, "mul49.t", nf, ns);
                return builder->CreateAdd(t, base, "mul49", nf, ns);
            }
            case 50: {
                // n*50 → (n<<6) - (n<<4) + (n<<1)  (= 64n - 16n + 2n)
                auto* shl6 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 6), "mul50.shl6");
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul50.shl4");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul50.shl1");
                auto* t = builder->CreateSub(shl6, shl4, "mul50.t", nf, ns);
                return builder->CreateAdd(t, shl1, "mul50", nf, ns);
            }
            case 200: {
                // n*200 → (n<<8) - (n<<6) + (n<<3)  (= 256n - 64n + 8n)
                auto* shl8 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 8), "mul200.shl8");
                auto* shl6 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 6), "mul200.shl6");
                auto* shl3 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 3), "mul200.shl3");
                auto* t = builder->CreateSub(shl8, shl6, "mul200.t", nf, ns);
                return builder->CreateAdd(t, shl3, "mul200", nf, ns);
            }
            case 19: {
                // n*19 → (n<<4) + (n<<1) + n  (= 16n + 2n + n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul19.shl4");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul19.shl1");
                auto* t = builder->CreateAdd(shl4, shl1, "mul19.t", nf, ns);
                return builder->CreateAdd(t, base, "mul19", nf, ns);
            }
            case 22: {
                // n*22 → (n<<4) + (n<<2) + (n<<1)  (= 16n + 4n + 2n)
                auto* shl4 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 4), "mul22.shl4");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul22.shl2");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul22.shl1");
                auto* t = builder->CreateAdd(shl4, shl2, "mul22.t", nf, ns);
                return builder->CreateAdd(t, shl1, "mul22", nf, ns);
            }
            case 26: {
                // n*26 → (n<<5) - (n<<2) - (n<<1)  (= 32n - 4n - 2n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul26.shl5");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul26.shl2");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul26.shl1");
                auto* t = builder->CreateSub(shl5, shl2, "mul26.t", nf, ns);
                return builder->CreateSub(t, shl1, "mul26", nf, ns);
            }
            case 27: {
                // n*27 → (n<<5) - (n<<2) - n  (= 32n - 4n - n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul27.shl5");
                auto* shl2 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 2), "mul27.shl2");
                auto* t = builder->CreateSub(shl5, shl2, "mul27.t", nf, ns);
                return builder->CreateSub(t, base, "mul27", nf, ns);
            }
            case 30: {
                // n*30 → (n<<5) - (n<<1)  (= 32n - 2n)
                auto* shl5 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 5), "mul30.shl5");
                auto* shl1 = builder->CreateShl(base, llvm::ConstantInt::get(getDefaultType(), 1), "mul30.shl1");
                return builder->CreateSub(shl5, shl1, "mul30", nf, ns);
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
            const bool leftNonNeg = nonNegValues_.count(left);
            const int64_t rv = ci->getSExtValue();
            // For non-negative base × positive constant: emit mul nsw directly.
            // emitShiftAdd produces (x<<a)+x patterns, but LLVM's InstCombine
            // converts these back to mul and DROPS the nsw flag, preventing
            // SCEV from proving non-negativity in subsequent analysis passes
            // (e.g. loop phi variables in Collatz-like while-loops lose the
            // mul nsw that enables accumulation interleaving and other opts).
            // Emitting mul nsw directly preserves the flag through all passes
            // since it is placed on the multiply itself, not on an intermediate
            // add.  The backend produces the same lea/shift instructions anyway.
            if (leftNonNeg && rv > 0) {
                auto* result = builder->CreateNSWMul(left, right, "multmp");
                nonNegValues_.insert(result);
                return result;
            }
            if (auto* result = emitShiftAdd(left, rv, false)) {
                return result;
            }
            // Negative constant strength reduction: n * (-K) → neg(n * K).
            // This leverages the existing shift+add patterns for the absolute
            // value, then negates the result.  A single neg (sub 0, x) is far
            // cheaper than a hardware multiply.
            if (rv < -1) {
                if (auto* posResult = emitShiftAdd(left, -rv, false)) {
                    return builder->CreateNeg(posResult, "mulneg");
                }
            }
        }
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
            const bool rightNonNeg = nonNegValues_.count(right);
            const int64_t lv = ci->getSExtValue();
            // Same as above: emit mul nsw when base is non-negative so the
            // nsw flag survives InstCombine's shift+add → mul canonicalization.
            if (rightNonNeg && lv > 0) {
                auto* result = builder->CreateNSWMul(right, left, "multmp");
                nonNegValues_.insert(result);
                return result;
            }
            if (auto* result = emitShiftAdd(right, lv, false)) {
                return result;
            }
            if (lv < -1) {
                if (auto* posResult = emitShiftAdd(right, -lv, false)) {
                    return builder->CreateNeg(posResult, "mulneg");
                }
            }
        }
        // When both operands are known non-negative, set nsw to enable
        // SCEV range analysis and strength reduction of derived expressions.
        // Also set nsw when one operand is a positive constant and the other
        // is non-negative — the product of two non-negative values cannot
        // overflow into the negative range for typical program values.
        if (nonNegValues_.count(left) && nonNegValues_.count(right)) {
            auto* result = builder->CreateNSWMul(left, right, "multmp");
            nonNegValues_.insert(result);
            return result;
        }
        if (nonNegValues_.count(left)) {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
                if (!ci->isNegative() && !ci->isZero()) {
                    auto* result = builder->CreateNSWMul(left, right, "multmp");
                    nonNegValues_.insert(result);
                    return result;
                }
            }
        }
        if (nonNegValues_.count(right)) {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left)) {
                if (!ci->isNegative() && !ci->isZero()) {
                    auto* result = builder->CreateNSWMul(left, right, "multmp");
                    nonNegValues_.insert(result);
                    return result;
                }
            }
        }
        // Squaring detection: x*x is always non-negative regardless of
        // the sign of x.  This is crucial for loops like `(i*i) % 101`
        // where the non-negativity of the product enables urem instead
        // of the more expensive srem.
        // Detect squaring at the AST level: both children are the same
        // identifier expression.
        {
            if (expr->left->type == ASTNodeType::IDENTIFIER_EXPR && expr->right->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* leftIdent = static_cast<IdentifierExpr*>(expr->left.get());
                auto* rightIdent = static_cast<IdentifierExpr*>(expr->right.get());
                if (leftIdent->name == rightIdent->name) {
                    auto* result = builder->CreateMul(left, right, "sqtmp");
                    nonNegValues_.insert(result);
                    return result;
                }
            }
        }
        if (left == right) {
            auto* result = builder->CreateMul(left, right, "sqtmp");
            nonNegValues_.insert(result);
            return result;
        }
        return builder->CreateMul(left, right, "multmp");
    } else if (expr->op == "/" || expr->op == "%") {
        const bool isDivision = expr->op == "/";

        // Strength reduction: unsigned-compatible division/modulo by power of 2.
        // For signed division by a positive power of 2, we can use an
        // arithmetic right shift with sign-correction for correct rounding
        // toward zero (C/OmScript semantics).  For modulo, we use
        // AND with (divisor - 1) after similar sign correction.
        //
        // When the dividend is provably non-negative, skip the sign correction
        // entirely: a simple logical right shift (div) or AND mask (mod) suffices,
        // saving 3 instructions per operation.
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            const int64_t rv = ci->getSExtValue();
            if (rv > 0) {
                const int s = log2IfPowerOf2(rv);
                if (s > 0) {
                    bool leftNonNeg = nonNegValues_.count(left) > 0;
                    if (!leftNonNeg) {
                        llvm::KnownBits KB = llvm::computeKnownBits(
                            left, module->getDataLayout());
                        leftNonNeg = KB.isNonNegative();
                    }
                    if (leftNonNeg) {
                        // Non-negative dividend: no sign correction needed.
                        // div: x >> s (logical shift — high bits are 0)
                        // mod: x & (2^s - 1)
                        if (isDivision) {
                            auto* result = builder->CreateLShr(left,
                                llvm::ConstantInt::get(getDefaultType(), s), "div.lshr");
                            nonNegValues_.insert(result);
                            return result;
                        } else {
                            auto* result = builder->CreateAnd(left,
                                llvm::ConstantInt::get(getDefaultType(), (1LL << s) - 1), "mod.and");
                            nonNegValues_.insert(result);
                            return result;
                        }
                    }
                    // Dividend sign unknown: emit sdiv/srem instead of the
                    // multi-instruction signed shift expansion.  LLVM's
                    // CorrelatedValuePropagation (CVP) pass can often prove
                    // non-negativity through value-range analysis on PHI nodes
                    // (e.g. Collatz-sequence variables that are always > 0) and
                    // convert sdiv/srem → udiv/urem, which InstCombine then
                    // lowers to lshr/and — a single instruction instead of four.
                    // The inline shift expansion we used previously baked in the
                    // sign-correction at IR-generation time, before LLVM's
                    // analyses could run, preventing this optimisation.
                    return isDivision
                        ? builder->CreateSDiv(left, right, "divtmp")
                        : builder->CreateSRem(left, right, "modtmp");
                }
            }
        }

        // Division/modulo by a non-zero constant: emit unsigned operations
        // when the dividend is provably non-negative.  The unsigned path
        // avoids the sign-correction fixup in the magic-number multiply
        // sequence, saving ~2 instructions per operation.  This is critical
        // for vectorization: when LLVM's loop vectorizer creates vector
        // copies, it preserves urem/udiv directly, whereas srem/sdiv would
        // require sign-correction in each vector lane.
        //
        // For signed (negative-possible) dividends, emit srem/sdiv and
        // let LLVM's CorrelatedValuePropagation convert them when it can
        // prove non-negativity through its own analysis.
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            if (!ci->isZero()) {
                if (ci->getSExtValue() > 0) {
                    // Positive constant divisor: check if dividend is non-negative
                    // using codegen-level tracking or LLVM's KnownBits analysis.
                    bool leftNonNeg = nonNegValues_.count(left) > 0;
                    if (!leftNonNeg) {
                        llvm::KnownBits KB = llvm::computeKnownBits(
                            left, module->getDataLayout());
                        leftNonNeg = KB.isNonNegative();
                    }
                    if (leftNonNeg) {
                        auto* result = isDivision
                            ? builder->CreateUDiv(left, right, "udivtmp")
                            : builder->CreateURem(left, right, "uremtmp");
                        // urem/udiv result is always non-negative
                        nonNegValues_.insert(result);
                        return result;
                    }
                }
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
        auto* result = builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        nonNegValues_.insert(result);
        return result;
    } else if (expr->op == "!=") {
        llvm::Value* cmp = builder->CreateICmpNE(left, right, "cmptmp");
        auto* result = builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        nonNegValues_.insert(result);
        return result;
    } else if (expr->op == "<") {
        // When both operands are provably non-negative, use unsigned comparison.
        // Unsigned comparisons have simpler codegen (no sign-extension) and
        // enable better vectorization — LLVM's loop vectorizer can widen
        // unsigned comparisons without sign-correction in each lane.
        bool bothNonNeg = nonNegValues_.count(left) && nonNegValues_.count(right);
        if (!bothNonNeg) {
            // Also check for non-negative constant operands.
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right))
                bothNonNeg = nonNegValues_.count(left) && !ci->isNegative();
            else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left))
                bothNonNeg = nonNegValues_.count(right) && !ci->isNegative();
        }
        llvm::Value* cmp = bothNonNeg
            ? builder->CreateICmpULT(left, right, "cmptmp")
            : builder->CreateICmpSLT(left, right, "cmptmp");
        auto* result = builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        nonNegValues_.insert(result);
        return result;
    } else if (expr->op == "<=") {
        bool bothNonNeg = nonNegValues_.count(left) && nonNegValues_.count(right);
        if (!bothNonNeg) {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right))
                bothNonNeg = nonNegValues_.count(left) && !ci->isNegative();
            else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left))
                bothNonNeg = nonNegValues_.count(right) && !ci->isNegative();
        }
        llvm::Value* cmp = bothNonNeg
            ? builder->CreateICmpULE(left, right, "cmptmp")
            : builder->CreateICmpSLE(left, right, "cmptmp");
        auto* result = builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        nonNegValues_.insert(result);
        return result;
    } else if (expr->op == ">") {
        bool bothNonNeg = nonNegValues_.count(left) && nonNegValues_.count(right);
        if (!bothNonNeg) {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right))
                bothNonNeg = nonNegValues_.count(left) && !ci->isNegative();
            else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left))
                bothNonNeg = nonNegValues_.count(right) && !ci->isNegative();
        }
        llvm::Value* cmp = bothNonNeg
            ? builder->CreateICmpUGT(left, right, "cmptmp")
            : builder->CreateICmpSGT(left, right, "cmptmp");
        auto* result = builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        nonNegValues_.insert(result);
        return result;
    } else if (expr->op == ">=") {
        bool bothNonNeg = nonNegValues_.count(left) && nonNegValues_.count(right);
        if (!bothNonNeg) {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right))
                bothNonNeg = nonNegValues_.count(left) && !ci->isNegative();
            else if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(left))
                bothNonNeg = nonNegValues_.count(right) && !ci->isNegative();
        }
        llvm::Value* cmp = bothNonNeg
            ? builder->CreateICmpUGE(left, right, "cmptmp")
            : builder->CreateICmpSGE(left, right, "cmptmp");
        auto* result = builder->CreateZExt(cmp, getDefaultType(), "booltmp");
        nonNegValues_.insert(result);
        return result;
    } else if (expr->op == "&") {
        auto* result = builder->CreateAnd(left, right, "andtmp");
        // AND with a non-negative value always produces a non-negative result
        if (nonNegValues_.count(left) || nonNegValues_.count(right))
            nonNegValues_.insert(result);
        return result;
    } else if (expr->op == "|") {
        auto* result = builder->CreateOr(left, right, "ortmp");
        // OR of two non-negative values is non-negative
        if (nonNegValues_.count(left) && nonNegValues_.count(right))
            nonNegValues_.insert(result);
        return result;
    } else if (expr->op == "^") {
        auto* result = builder->CreateXor(left, right, "xortmp");
        // XOR of two non-negative values is non-negative (sign bit stays 0)
        if (nonNegValues_.count(left) && nonNegValues_.count(right))
            nonNegValues_.insert(result);
        return result;
    } else if (expr->op == "<<") {
        // For constant shift amounts already in [0, 63], skip the mask.
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            const int64_t sv = ci->getSExtValue();
            if (sv >= 0 && sv < 64)
                return builder->CreateShl(left, right, "shltmp");
        }
        // Mask shift amount to [0, 63] to prevent undefined behavior
        llvm::Value* mask = llvm::ConstantInt::get(getDefaultType(), 63);
        llvm::Value* safeShift = builder->CreateAnd(right, mask, "shlmask");
        // Do NOT set nsw/nuw: left-shifts on arbitrary values can overflow,
        // and marking nsw would let LLVM treat overflowing shifts as poison.
        return builder->CreateShl(left, safeShift, "shltmp");
    } else if (expr->op == ">>") {
        // For constant shift amounts already in [0, 63], skip the mask.
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            const int64_t sv = ci->getSExtValue();
            if (sv >= 0 && sv < 64)
                return builder->CreateLShr(left, right, "lshrtmp");
        }
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
        //
        // When the base is provably non-negative and the exponent is positive,
        // we set nsw on all intermediate multiplications.  This enables SCEV
        // to compute tighter value ranges for the result, improving downstream
        // loop optimization and vectorization.
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(right)) {
            const int64_t exp = ci->getSExtValue();
            const bool baseNonNeg = nonNegValues_.count(left);
            if (exp == 2) {
                // x**2 → x*x  (1 mul) — result is always non-negative
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow2")
                    : builder->CreateMul(left, left, "pow2");
                nonNegValues_.insert(result);  // x*x >= 0 always
                return result;
            }
            if (exp == 3) {
                // x**3 → x*x*x  (2 muls)
                auto* sq = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow3.sq")
                    : builder->CreateMul(left, left, "pow3.sq");
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(sq, left, "pow3")
                    : builder->CreateMul(sq, left, "pow3");
                if (baseNonNeg) nonNegValues_.insert(result);
                return result;
            }
            if (exp == 4) {
                // x**4 → t=x*x; t*t  (2 muls) — result is always non-negative
                auto* sq = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow4.sq")
                    : builder->CreateMul(left, left, "pow4.sq");
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(sq, sq, "pow4")
                    : builder->CreateMul(sq, sq, "pow4");
                nonNegValues_.insert(result);  // (x*x)*(x*x) >= 0 always
                return result;
            }
            if (exp == 5) {
                // x**5 → t=x*x; t*t*x  (3 muls)
                auto* sq = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow5.sq")
                    : builder->CreateMul(left, left, "pow5.sq");
                auto* q4 = baseNonNeg
                    ? builder->CreateNSWMul(sq, sq, "pow5.q4")
                    : builder->CreateMul(sq, sq, "pow5.q4");
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(q4, left, "pow5")
                    : builder->CreateMul(q4, left, "pow5");
                if (baseNonNeg) nonNegValues_.insert(result);
                return result;
            }
            if (exp == 6) {
                // x**6 → t=x*x; u=t*t; u*t  (3 muls) — result is always non-negative
                auto* sq = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow6.sq")
                    : builder->CreateMul(left, left, "pow6.sq");
                auto* q4 = baseNonNeg
                    ? builder->CreateNSWMul(sq, sq, "pow6.q4")
                    : builder->CreateMul(sq, sq, "pow6.q4");
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(q4, sq, "pow6")
                    : builder->CreateMul(q4, sq, "pow6");
                nonNegValues_.insert(result);  // even exponent → always non-neg
                return result;
            }
            if (exp == 7) {
                // x**7 → t=x*x; u=t*t; u*t*x  (4 muls)
                auto* sq = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow7.sq")
                    : builder->CreateMul(left, left, "pow7.sq");
                auto* q4 = baseNonNeg
                    ? builder->CreateNSWMul(sq, sq, "pow7.q4")
                    : builder->CreateMul(sq, sq, "pow7.q4");
                auto* q6 = baseNonNeg
                    ? builder->CreateNSWMul(q4, sq, "pow7.q6")
                    : builder->CreateMul(q4, sq, "pow7.q6");
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(q6, left, "pow7")
                    : builder->CreateMul(q6, left, "pow7");
                if (baseNonNeg) nonNegValues_.insert(result);
                return result;
            }
            if (exp == 8) {
                // x**8 → t=x*x; u=t*t; u*u  (3 muls) — result is always non-negative
                auto* sq = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow8.sq")
                    : builder->CreateMul(left, left, "pow8.sq");
                auto* q4 = baseNonNeg
                    ? builder->CreateNSWMul(sq, sq, "pow8.q4")
                    : builder->CreateMul(sq, sq, "pow8.q4");
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(q4, q4, "pow8")
                    : builder->CreateMul(q4, q4, "pow8");
                nonNegValues_.insert(result);  // even exponent → always non-neg
                return result;
            }
            if (exp == 9) {
                // x**9 → t=x*x; u=t*t; v=u*u; v*x  (4 muls)
                auto* sq = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow9.sq")
                    : builder->CreateMul(left, left, "pow9.sq");
                auto* q4 = baseNonNeg
                    ? builder->CreateNSWMul(sq, sq, "pow9.q4")
                    : builder->CreateMul(sq, sq, "pow9.q4");
                auto* q8 = baseNonNeg
                    ? builder->CreateNSWMul(q4, q4, "pow9.q8")
                    : builder->CreateMul(q4, q4, "pow9.q8");
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(q8, left, "pow9")
                    : builder->CreateMul(q8, left, "pow9");
                if (baseNonNeg) nonNegValues_.insert(result);
                return result;
            }
            if (exp == 10) {
                // x**10 → t=x*x; u=t*t; v=u*u; v*t  (4 muls) — result is always non-negative
                auto* sq = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow10.sq")
                    : builder->CreateMul(left, left, "pow10.sq");
                auto* q4 = baseNonNeg
                    ? builder->CreateNSWMul(sq, sq, "pow10.q4")
                    : builder->CreateMul(sq, sq, "pow10.q4");
                auto* q8 = baseNonNeg
                    ? builder->CreateNSWMul(q4, q4, "pow10.q8")
                    : builder->CreateMul(q4, q4, "pow10.q8");
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(q8, sq, "pow10")
                    : builder->CreateMul(q8, sq, "pow10");
                nonNegValues_.insert(result);  // even exponent → always non-neg
                return result;
            }
            if (exp == 11) {
                // x**11 → t=x*x; u=t*t; v=u*u; v*t*x  (5 muls)
                auto* sq = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow11.sq")
                    : builder->CreateMul(left, left, "pow11.sq");
                auto* q4 = baseNonNeg
                    ? builder->CreateNSWMul(sq, sq, "pow11.q4")
                    : builder->CreateMul(sq, sq, "pow11.q4");
                auto* q8 = baseNonNeg
                    ? builder->CreateNSWMul(q4, q4, "pow11.q8")
                    : builder->CreateMul(q4, q4, "pow11.q8");
                auto* q10 = baseNonNeg
                    ? builder->CreateNSWMul(q8, sq, "pow11.q10")
                    : builder->CreateMul(q8, sq, "pow11.q10");
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(q10, left, "pow11")
                    : builder->CreateMul(q10, left, "pow11");
                if (baseNonNeg) nonNegValues_.insert(result);
                return result;
            }
            if (exp == 12) {
                // x**12 → t=x*x; u=t*t; v=u*u; v*u  (4 muls) — even exponent
                auto* sq = baseNonNeg
                    ? builder->CreateNSWMul(left, left, "pow12.sq")
                    : builder->CreateMul(left, left, "pow12.sq");
                auto* q4 = baseNonNeg
                    ? builder->CreateNSWMul(sq, sq, "pow12.q4")
                    : builder->CreateMul(sq, sq, "pow12.q4");
                auto* q8 = baseNonNeg
                    ? builder->CreateNSWMul(q4, q4, "pow12.q8")
                    : builder->CreateMul(q4, q4, "pow12.q8");
                auto* result = baseNonNeg
                    ? builder->CreateNSWMul(q8, q4, "pow12")
                    : builder->CreateMul(q8, q4, "pow12");
                nonNegValues_.insert(result);  // even exponent → always non-neg
                return result;
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

        // Negative exponent: base**(-n) = 1 / base**n in integer math.
        // |base| > 1 → truncates to 0; base=1 → 1; base=-1 → ±1.
        builder->SetInsertPoint(negExpBB);
        llvm::Value* negOne = llvm::ConstantInt::get(getDefaultType(), -1, true);
        llvm::Value* isBaseOne = builder->CreateICmpEQ(left, one, "pow.isone");
        llvm::Value* isBaseNegOne = builder->CreateICmpEQ(left, negOne, "pow.isnegone");
        // For base=-1: result is -1 if exponent is odd, 1 if even.
        // exponent is negative here, but parity of -n equals parity of n:
        // negate, then check the low bit.
        llvm::Value* absExp = builder->CreateNeg(right, "pow.absexp");
        llvm::Value* expLowBit = builder->CreateAnd(absExp, one, "pow.lowbit");
        llvm::Value* isExpOdd = builder->CreateICmpNE(expLowBit, zero, "pow.expodd");
        llvm::Value* negOneResult = builder->CreateSelect(isExpOdd, negOne, one, "pow.negoneres");
        // Pick result: base==1 → 1, base==-1 → ±1, else → 0.
        llvm::Value* negResult = builder->CreateSelect(isBaseNegOne, negOneResult, zero, "pow.negsel");
        negResult = builder->CreateSelect(isBaseOne, one, negResult, "pow.negfinal");
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
        auto* powBackBr = builder->CreateBr(loopBB);
        if (optimizationLevel >= OptimizationLevel::O1) {
            llvm::SmallVector<llvm::Metadata*, 2> mds;
            mds.push_back(nullptr);
            mds.push_back(llvm::MDNode::get(*context,
                {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
            llvm::MDNode* md = llvm::MDNode::get(*context, mds);
            md->replaceOperandWith(0, md);
            powBackBr->setMetadata(llvm::LLVMContext::MD_loop, md);
        }

        builder->SetInsertPoint(doneBB);
        llvm::PHINode* finalResult = builder->CreatePHI(getDefaultType(), 2, "pow.final");
        finalResult->addIncoming(negResult, negExpBB);
        finalResult->addIncoming(result, loopBB);
        return finalResult;
    }

    codegenError("Unknown binary operator: " + expr->op, expr);
}

llvm::Value* CodeGenerator::generateUnary(UnaryExpr* expr) {
    // Double negation elimination — detect op(op(x)) patterns at the AST level
    // and short-circuit to just x: -(-x) → x, ~(~x) → x, !(!x) → x.
    if (expr->operand->type == ASTNodeType::UNARY_EXPR) {
        auto* inner = static_cast<UnaryExpr*>(expr->operand.get());
        if (inner->op == expr->op && (expr->op == "-" || expr->op == "~" || expr->op == "!")) {
            return generateExpression(inner->operand.get());
        }
    }

    llvm::Value* operand = generateExpression(expr->operand.get());

    // Constant folding for unary operations
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(operand)) {
        const int64_t val = ci->getSExtValue();
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
    } else if (expr->op == "&") {
        // Address-of operator: in OmScript's value semantics, this is a
        // pass-through — the operand is already the loaded value (not a
        // pointer).  The `&` is syntactic sugar for borrow declarations
        // (e.g., `borrow var j:&i32 = &x;`).
        return operand;
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

    builder->CreateAlignedStore(value, it->second, llvm::MaybeAlign(8));
    // Update non-negativity tracking on assignment.
    // When the assigned value is provably non-negative, mark the alloca so
    // subsequent loads can benefit from unsigned operations and NSW flags.
    // When the value might be negative, remove the alloca from nonNegValues_
    // to prevent unsound optimizations.
    if (alloca && alloca->getAllocatedType()->isIntegerTy()) {
        bool valNonNeg = nonNegValues_.count(value) > 0;
        if (!valNonNeg) {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(value))
                valNonNeg = !ci->isNegative();
        }
        if (valNonNeg)
            nonNegValues_.insert(it->second);
        else
            nonNegValues_.erase(it->second);
    }
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
    if (operandExpr->type != ASTNodeType::INDEX_EXPR && operandExpr->type != ASTNodeType::IDENTIFIER_EXPR) {
        codegenError("Increment/decrement operators require an lvalue operand", errorNode);
    }
    IndexExpr* indexExpr = (operandExpr->type == ASTNodeType::INDEX_EXPR)
        ? static_cast<IndexExpr*>(operandExpr) : nullptr;
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

        // Bounds check — elided in OPTMAX functions where compile-time
        // ownership analysis guarantees safety (zero-cost abstraction).
        // Also elided in @hot functions at O2+ (user asserts performance-critical
        // code with safe indices) and when the index is a provably-safe loop iterator.
        bool boundsCheckElidedID = inOptMaxFunction
            || (currentFuncHintHot_ && optimizationLevel >= OptimizationLevel::O2);

        if (!boundsCheckElidedID && optimizationLevel >= OptimizationLevel::O1) {
            if (indexExpr->index->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* idxIdent = static_cast<IdentifierExpr*>(indexExpr->index.get());
                if (safeIndexVars_.count(idxIdent->name)) {
                    auto it = loopIterEndBound_.find(idxIdent->name);
                    if (it != loopIterEndBound_.end()) {
                        llvm::Value* endBound = it->second;
                        auto* lenLoadE = builder->CreateLoad(getDefaultType(), arrPtr, "incdec.len.elim");
                        lenLoadE->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
                        lenLoadE->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
                        llvm::Value* lenVal = lenLoadE;
                        if (endBound == lenVal) {
                            boundsCheckElidedID = true;
                        }
                        if (!boundsCheckElidedID) {
                            if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endBound)) {
                                if (auto* lenCI = llvm::dyn_cast<llvm::ConstantInt>(lenVal)) {
                                    if (endCI->getSExtValue() <= lenCI->getSExtValue()) {
                                        boundsCheckElidedID = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!boundsCheckElidedID) {
            auto* lenLoadID = builder->CreateLoad(getDefaultType(), arrPtr, "incdec.len");
            lenLoadID->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
            lenLoadID->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            llvm::Value* lenVal = lenLoadID;
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
        }
        llvm::Value* dataPtr = builder->CreateInBoundsGEP(getDefaultType(), arrPtr,
            llvm::ConstantInt::get(getDefaultType(), 1), "incdec.data");
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), dataPtr, idxVal, "incdec.elem.ptr");
        llvm::Value* current = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "incdec.elem");

        llvm::Value* delta = llvm::ConstantInt::get(getDefaultType(), 1, true);
        llvm::Value* updated =
            (op == "++") ? builder->CreateAdd(current, delta, "inc") : builder->CreateSub(current, delta, "dec");
        builder->CreateAlignedStore(updated, elemPtr, llvm::MaybeAlign(8));
        return isPostfix ? current : updated;
    }

    // Handle simple variable increment/decrement
    if (operandExpr->type != ASTNodeType::IDENTIFIER_EXPR) {
        codegenError("Increment/decrement operators require an lvalue operand", errorNode);
    }
    auto* identifier = static_cast<IdentifierExpr*>(operandExpr);

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

    // Branchless select optimization: when both arms are simple expressions
    // (literals or identifiers with no side effects), emit a single `select`
    // instruction instead of a branch+PHI diamond.  This eliminates branch
    // misprediction entirely and enables the backend to use conditional-move
    // (cmov) instructions.
    auto isSimpleExpr = [](Expression* e) -> bool {
        return e->type == ASTNodeType::LITERAL_EXPR || e->type == ASTNodeType::IDENTIFIER_EXPR;
    };
    if (isSimpleExpr(expr->thenExpr.get()) && isSimpleExpr(expr->elseExpr.get())) {
        llvm::Value* thenVal = generateExpression(expr->thenExpr.get());
        llvm::Value* elseVal = generateExpression(expr->elseExpr.get());
        // Ensure matching types for select.
        if (thenVal->getType() != elseVal->getType()) {
            if (thenVal->getType()->isDoubleTy() || elseVal->getType()->isDoubleTy()) {
                if (!thenVal->getType()->isDoubleTy())
                    thenVal = ensureFloat(thenVal);
                if (!elseVal->getType()->isDoubleTy())
                    elseVal = ensureFloat(elseVal);
            }
        }
        llvm::Value* sel = builder->CreateSelect(condBool, thenVal, elseVal, "ternsel");
        // Propagate non-negativity: if both arms are non-negative, the result is.
        bool tNonNeg = nonNegValues_.count(thenVal) > 0;
        if (!tNonNeg)
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(thenVal))
                tNonNeg = !ci->isNegative();
        bool eNonNeg = nonNegValues_.count(elseVal) > 0;
        if (!eNonNeg)
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(elseVal))
                eNonNeg = !ci->isNegative();
        if (tNonNeg && eNonNeg)
            nonNegValues_.insert(sel);
        return sel;
    }

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

    // Propagate non-negativity through the PHI: if both arms are provably
    // non-negative the merged result is also non-negative.  Without this,
    // patterns like x = (x%2==0) ? (x/2) : (3*x+1) lose non-neg tracking
    // after the first iteration, causing srem/sdiv instead of urem/lshr.
    bool thenNonNeg = nonNegValues_.count(thenVal) > 0;
    if (!thenNonNeg)
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(thenVal))
            thenNonNeg = !ci->isNegative();
    bool elseNonNeg = nonNegValues_.count(elseVal) > 0;
    if (!elseNonNeg)
        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(elseVal))
            elseNonNeg = !ci->isNegative();
    if (thenNonNeg && elseNonNeg)
        nonNegValues_.insert(phi);

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
        const size_t numElements = expr->elements.size();
        const size_t totalSlots = 1 + numElements;

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
    evalElems.reserve(expr->elements.size());

    for (const auto& elem : expr->elements) {
        if (elem->type == ASTNodeType::SPREAD_EXPR) {
            auto* spread = static_cast<SpreadExpr*>(elem.get());
            llvm::Value* arrVal = generateExpression(spread->operand.get());
            arrVal = toDefaultType(arrVal);
            llvm::Value* arrPtr =
                builder->CreateIntToPtr(arrVal, llvm::PointerType::getUnqual(*context), "spread.arrptr");
            auto* spreadLenLoad = builder->CreateLoad(getDefaultType(), arrPtr, "spread.len");
            spreadLenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
            spreadLenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            llvm::Value* arrLen = spreadLenLoad;
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
            llvm::Value* srcElemPtr = builder->CreateInBoundsGEP(getDefaultType(), srcPtr, srcIdx, "spread.srcelem");
            llvm::Value* elem = builder->CreateLoad(getDefaultType(), srcElemPtr, "spread.elem");
            // Store in dest: buf[writeIdx + 1]
            llvm::Value* curIdx = builder->CreateLoad(getDefaultType(), writeIdx, "spread.curidx");
            llvm::Value* dstIdx = builder->CreateAdd(curIdx, one, "spread.dstidx");
            llvm::Value* dstPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, dstIdx, "spread.dstptr");
            builder->CreateStore(elem, dstPtr);
            // Increment write index
            llvm::Value* newIdx = builder->CreateAdd(curIdx, one, "spread.newidx");
            builder->CreateStore(newIdx, writeIdx);
            // Increment loop counter
            llvm::Value* nextI = builder->CreateAdd(i, one, "spread.nexti");
            i->addIncoming(nextI, bodyBB);
            auto* spreadBackBr = builder->CreateBr(loopBB);
            if (optimizationLevel >= OptimizationLevel::O1) {
                llvm::SmallVector<llvm::Metadata*, 2> mds;
                mds.push_back(nullptr);
                mds.push_back(llvm::MDNode::get(*context,
                    {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));
                llvm::MDNode* md = llvm::MDNode::get(*context, mds);
                md->replaceOperandWith(0, md);
                spreadBackBr->setMetadata(llvm::LLVMContext::MD_loop, md);
            }

            builder->SetInsertPoint(doneBB);
        } else {
            // Single element: store at buf[writeIdx + 1]
            llvm::Value* curIdx = builder->CreateLoad(getDefaultType(), writeIdx, "spread.curidx");
            llvm::Value* dstIdx = builder->CreateAdd(curIdx, one, "spread.dstidx");
            llvm::Value* dstPtr = builder->CreateInBoundsGEP(getDefaultType(), buf, dstIdx, "spread.dstptr");
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

    // SIMD vector element extraction: v[i] → extractelement
    if (arrVal->getType()->isVectorTy()) {
        // Index must be i32 for extractelement.
        if (idxVal->getType() != llvm::Type::getInt32Ty(*context))
            idxVal = builder->CreateIntCast(idxVal, llvm::Type::getInt32Ty(*context), true, "simd.idx");
        llvm::Value* elem = builder->CreateExtractElement(arrVal, idxVal, "simd.extract");
        // Widen f32 → f64 and narrow integer types → i64 so the extracted
        // value integrates with OmScript's standard type system (i64 / double).
        if (elem->getType()->isFloatTy())
            elem = builder->CreateFPExt(elem, getFloatType(), "simd.ext.f64");
        else if (elem->getType()->isIntegerTy() && elem->getType() != getDefaultType())
            elem = builder->CreateSExt(elem, getDefaultType(), "simd.ext.i64");
        return elem;
    }

    idxVal = toDefaultType(idxVal);

    // Detect whether the base expression is a string.  Strings are raw char
    // pointers without a length header; arrays have the layout [length, e0,
    // e1, ...] and each element is 8 bytes (i64).
    const bool isStr = arrVal->getType()->isPointerTy() || isStringExpr(expr->array.get());

    // Convert i64 → pointer (strings may arrive as i64 via ptrtoint)
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    llvm::Value* basePtr =
        arrVal->getType()->isPointerTy() ? arrVal : builder->CreateIntToPtr(arrVal, ptrTy, "idx.baseptr");

    // Bounds check — elided in OPTMAX functions where compile-time
    // ownership analysis guarantees safety (zero-cost abstraction).
    //
    // Also elided when static analysis can prove the index is within bounds:
    // for ascending for-loops starting from a non-negative constant, the
    // iterator is always in [start, endVal).  If the index is a known-safe
    // loop iterator AND we can verify endVal <= array length, the bounds
    // check is provably unnecessary (zero-cost abstraction).
    //
    // @hot functions at O2+ also skip bounds checks: the user asserts the
    // function is performance-critical with safe array access patterns.
    bool boundsCheckElided = inOptMaxFunction
        || (currentFuncHintHot_ && !isStr && optimizationLevel >= OptimizationLevel::O2);

    // Ownership-aware optimization: borrowed arrays cannot be resized,
    // so their length is invariant.  Mark the length load with !invariant.load
    // so LLVM can hoist/CSE it across the loop.
    bool arrayIsBorrowed = false;
    if (!isStr) {
        if (expr->array->type == ASTNodeType::IDENTIFIER_EXPR) {
            arrayIsBorrowed = isVariableBorrowed(static_cast<IdentifierExpr*>(expr->array.get())->name);
        }
    }

    if (!boundsCheckElided && !isStr && optimizationLevel >= OptimizationLevel::O1) {
        // ── Known-array-size optimization ────────────────────────────
        // When the array was created via array_fill(N, val) with constant
        // N, and the loop iterator is bounded by a constant <= N, we can
        // prove bounds safety without loading the length header.
        if (expr->array->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto* arrIdent = static_cast<IdentifierExpr*>(expr->array.get());
            auto sizeIt = knownArraySizes_.find(arrIdent->name);
            if (sizeIt != knownArraySizes_.end()) {
                if (expr->index->type == ASTNodeType::IDENTIFIER_EXPR) {
                    auto* idxIdent2 = static_cast<IdentifierExpr*>(expr->index.get());
                    if (safeIndexVars_.count(idxIdent2->name)) {
                        auto endIt = loopIterEndBound_.find(idxIdent2->name);
                        if (endIt != loopIterEndBound_.end()) {
                            auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endIt->second);
                            auto* sizeCI = llvm::dyn_cast<llvm::ConstantInt>(sizeIt->second);
                            if (endCI && sizeCI &&
                                endCI->getSExtValue() <= sizeCI->getSExtValue()) {
                                boundsCheckElided = true;
                            }
                        }
                    }
                }
            }
        }

        // Check if the index value is a safe loop iterator (non-negative,
        // ascending, bounded by loop end).
        IdentifierExpr* idxIdent = (expr->index->type == ASTNodeType::IDENTIFIER_EXPR)
            ? static_cast<IdentifierExpr*>(expr->index.get()) : nullptr;
        if (idxIdent && safeIndexVars_.count(idxIdent->name)) {
            auto it = loopIterEndBound_.find(idxIdent->name);
            if (it != loopIterEndBound_.end()) {
                llvm::Value* endBound = it->second;
                // Load the array length from slot 0.
                auto* lenLoadElim = builder->CreateLoad(getDefaultType(), basePtr, "idx.len.elim");
                lenLoadElim->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
                lenLoadElim->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
                llvm::Value* lenVal = lenLoadElim;

                // Case 1: end bound and length are the same SSA value (e.g.,
                // both come from len(arr) or same variable).
                if (endBound == lenVal) {
                    boundsCheckElided = true;
                }

                // Case 2: both are compile-time constants — compare directly.
                if (!boundsCheckElided) {
                    if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endBound)) {
                        if (auto* lenCI = llvm::dyn_cast<llvm::ConstantInt>(lenVal)) {
                            if (endCI->getSExtValue() <= lenCI->getSExtValue()) {
                                boundsCheckElided = true;
                            }
                        }
                    }
                }

                // Case 3 (fallback): emit llvm.assume(endBound <= len) so LLVM
                // can fold the bounds check branch via CorrelatedValuePropagation.
                if (!boundsCheckElided && !dynamicCompilation_
                    && optimizationLevel >= OptimizationLevel::O2) {
                    llvm::Value* endLELen = builder->CreateICmpSLE(endBound, lenVal, "idx.endlelen");
                    llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
                        module.get(), llvm::Intrinsic::assume, {});
                    builder->CreateCall(assumeFn, {endLELen});
                }
            }
        }

        // ── Enhanced bounds elision: arithmetic index expressions ─────
        // For arr[i + const] or arr[i - const] where i is a safe iterator,
        // verify that the adjusted range [start+const, end+const) still
        // falls within [0, len).  This handles common patterns like:
        //   for (i in 1...n) { arr[i - 1] }     // lookback
        //   for (i in 0...n-1) { arr[i + 1] }   // lookahead
        if (!boundsCheckElided && !idxIdent) {
            if (expr->index->type == ASTNodeType::BINARY_EXPR) {
                auto* idxBinary = static_cast<BinaryExpr*>(expr->index.get());
                if (idxBinary->op == "+" || idxBinary->op == "-") {
                    IdentifierExpr* iterIdent = (idxBinary->left->type == ASTNodeType::IDENTIFIER_EXPR)
                        ? static_cast<IdentifierExpr*>(idxBinary->left.get()) : nullptr;
                    LiteralExpr* offsetLit = (idxBinary->right->type == ASTNodeType::LITERAL_EXPR)
                        ? static_cast<LiteralExpr*>(idxBinary->right.get()) : nullptr;
                    if (iterIdent && offsetLit && safeIndexVars_.count(iterIdent->name)) {
                        auto endIt = loopIterEndBound_.find(iterIdent->name);
                        if (endIt != loopIterEndBound_.end()) {
                            // We know iterator is in [0, end).  For i+c the access
                            // range is [c, end+c).  Safe if end+c <= len and c >= 0.
                            // For i-c the range is [-c, end-c).  Safe if c <= start
                            // (i.e., c <= 0 after adjustment) and end-c <= len.
                            // We only handle constant offsets for compile-time proof.
                            if (offsetLit->type == ASTNodeType::LITERAL_EXPR) {
                                // Retrieve the constant offset from the literal
                                int64_t offset = 0;
                                bool offsetKnown = false;
                                if (offsetLit->literalType == LiteralExpr::LiteralType::INTEGER) {
                                    offset = offsetLit->intValue;
                                    offsetKnown = true;
                                }
                                if (offsetKnown) {
                                    int64_t effectiveOffset = (idxBinary->op == "-") ? -offset : offset;
                                    // ── Positive effective offset: arr[i + C] ─────
                                    // Range is [start+C, end+C). Safe if end+C <= len.
                                    if (effectiveOffset >= 0) {
                                        auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endIt->second);
                                        auto* arithLenLoad = builder->CreateLoad(
                                            getDefaultType(), basePtr, "idx.len.arith");
                                        arithLenLoad->setMetadata(
                                            llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
                                        arithLenLoad->setMetadata(
                                            llvm::LLVMContext::MD_range, arrayLenRangeMD_);

                                        // When both end bound and length are
                                        // compile-time constants, prove statically.
                                        if (endCI) {
                                            if (auto* lenCI = llvm::dyn_cast<llvm::ConstantInt>(
                                                    endIt->second)) {
                                                // If the end bound itself satisfies
                                                // end + offset <= len, elide.
                                                if (endCI->getSExtValue() + effectiveOffset
                                                    <= lenCI->getSExtValue()) {
                                                    boundsCheckElided = true;
                                                }
                                            }
                                        }
                                        // Emit assume hint for LLVM's CVP pass
                                        if (!boundsCheckElided && !dynamicCompilation_
                                            && optimizationLevel >= OptimizationLevel::O2) {
                                            llvm::Value* adjustedEnd = builder->CreateAdd(
                                                endIt->second,
                                                llvm::ConstantInt::get(getDefaultType(), effectiveOffset),
                                                "idx.adjend");
                                            llvm::Value* cmp = builder->CreateICmpSLE(
                                                adjustedEnd, arithLenLoad, "idx.arith.safe");
                                            llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
                                                module.get(), llvm::Intrinsic::assume, {});
                                            builder->CreateCall(assumeFn, {cmp});
                                        }
                                    }

                                    // ── Negative effective offset: arr[i - K] ────
                                    // Range is [start-K, end-K). Safe if:
                                    //   (a) start - K >= 0  (lower bound non-negative)
                                    //   (b) end - K <= len  (upper bound within array)
                                    // Use loopIterStartBound_ to prove (a) at compile
                                    // time.  For (b), since i is a safe index variable,
                                    // we know end <= len(arr).  Since K > 0, we have
                                    // end - K < end <= len, so (b) is automatically true.
                                    // This handles the common lookback pattern:
                                    //   for (i in K...n) { arr[i - K] }
                                    if (effectiveOffset < 0 && !boundsCheckElided) {
                                        int64_t absOffset = -effectiveOffset;
                                        auto startIt = loopIterStartBound_.find(iterIdent->name);
                                        if (startIt != loopIterStartBound_.end()) {
                                            // Check (a): start >= absOffset
                                            auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startIt->second);
                                            if (startCI && startCI->getSExtValue() >= absOffset) {
                                                // Lower bound safe: start - K >= 0.
                                                // Upper bound is automatically safe since
                                                // K > 0 and end <= len (safe index var
                                                // invariant), so end - K < end <= len.
                                                boundsCheckElided = true;
                                            }

                                            // Fallback: emit assume hints for LLVM CVP
                                            if (!boundsCheckElided && !dynamicCompilation_
                                                && optimizationLevel >= OptimizationLevel::O2) {
                                                auto* arithLenLoad = builder->CreateLoad(
                                                    getDefaultType(), basePtr, "idx.len.arith.neg");
                                                arithLenLoad->setMetadata(
                                                    llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
                                                arithLenLoad->setMetadata(
                                                    llvm::LLVMContext::MD_range, arrayLenRangeMD_);
                                                // Assume: start - absOffset >= 0
                                                llvm::Value* adjustedStart = builder->CreateSub(
                                                    startIt->second,
                                                    llvm::ConstantInt::get(getDefaultType(), absOffset),
                                                    "idx.adjstart");
                                                llvm::Value* geZero = builder->CreateICmpSGE(
                                                    adjustedStart,
                                                    llvm::ConstantInt::get(getDefaultType(), 0),
                                                    "idx.negoff.ge0");
                                                // Assume: end - absOffset <= len
                                                llvm::Value* adjustedEnd = builder->CreateSub(
                                                    endIt->second,
                                                    llvm::ConstantInt::get(getDefaultType(), absOffset),
                                                    "idx.adjend.neg");
                                                llvm::Value* leLen = builder->CreateICmpSLE(
                                                    adjustedEnd, arithLenLoad, "idx.negoff.safe");
                                                llvm::Value* bothSafe = builder->CreateAnd(
                                                    geZero, leLen, "idx.negoff.both");
                                                llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
                                                    module.get(), llvm::Intrinsic::assume, {});
                                                builder->CreateCall(assumeFn, {bothSafe});
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (!boundsCheckElided) {
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "idx.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "idx.fail", function);

        llvm::Value* lenVal;
        if (isStr) {
            // String: use strlen to get length — no length header.
            lenVal = builder->CreateCall(getOrDeclareStrlen(), {basePtr}, "idx.strlen");
        } else {
            // Array: length is stored in slot 0 of the buffer.
            auto* lenLoad = builder->CreateLoad(getDefaultType(), basePtr, "idx.len");
            lenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
            lenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            // Borrowed arrays cannot resize — mark length as invariant so
            // LLVM can hoist it out of loops and CSE multiple loads.
            if (arrayIsBorrowed) {
                lenLoad->setMetadata(llvm::LLVMContext::MD_invariant_load,
                                     llvm::MDNode::get(*context, {}));
            }
            lenVal = lenLoad;
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
    }

    if (isStr) {
        // Load single byte at offset index, zero-extend to i64
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), basePtr, idxVal, "idx.charptr");
        llvm::Value* charVal = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "idx.char");
        return builder->CreateZExt(charVal, getDefaultType(), "idx.charext");
    }
    // Array: element is at slot (index + 1) in the i64 buffer.
    // Compute data pointer (base + 1 slot) then GEP by just the index.
    // This allows LLVM to hoist the data-pointer computation out of loops.
    //
    // OmScript arrays are guaranteed contiguous in memory: the layout is
    // [length, e0, e1, ..., e(n-1)] as a single malloc/calloc allocation.
    // We communicate this to LLVM via:
    //   - 8-byte aligned loads (elements are i64)
    //   - !nontemporal on large sequential scans (when @prefetch is active)
    //   - inbounds GEP (the index is within the allocated region)
    // This enables LLVM to apply vectorization, prefetching, and stride
    // analysis optimizations that require contiguous memory guarantees.
    llvm::Value* dataPtr = builder->CreateInBoundsGEP(getDefaultType(), basePtr,
        llvm::ConstantInt::get(getDefaultType(), 1), "idx.data");
    llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), dataPtr, idxVal, "idx.elem.ptr");
    auto* elemLoad = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "idx.elem");
    elemLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
    // When inside a parallel loop, attach the access group metadata so
    // LLVM's vectorizer and Polly know this load is iteration-independent.
    if (currentLoopAccessGroup_)
        elemLoad->setMetadata(llvm::LLVMContext::MD_access_group, currentLoopAccessGroup_);
    return elemLoad;
}

llvm::Value* CodeGenerator::generateIndexAssign(IndexAssignExpr* expr) {
    // SIMD vector element insertion: v[i] = x → insertelement.
    // We must handle this before generating the array expression because
    // SIMD vectors are values (not pointers) — we load the vector from its
    // alloca, insert the new element, and store the updated vector back.
    if (expr->array->type == ASTNodeType::IDENTIFIER_EXPR) {
        auto* idExpr = static_cast<IdentifierExpr*>(expr->array.get());
        auto it = namedValues.find(idExpr->name);
        if (it != namedValues.end()) {
            auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(it->second);
            if (alloca && alloca->getAllocatedType()->isVectorTy()) {
                auto* vecTy = alloca->getAllocatedType();
                llvm::Value* vec = builder->CreateLoad(vecTy, alloca, "simd.load");
                llvm::Value* idxVal = generateExpression(expr->index.get());
                llvm::Value* newVal = generateExpression(expr->value.get());
                // Convert index to i32
                if (idxVal->getType() != llvm::Type::getInt32Ty(*context))
                    idxVal = builder->CreateIntCast(idxVal, llvm::Type::getInt32Ty(*context), true, "simd.idx");
                // Convert value to match vector element type
                auto* elemTy = llvm::cast<llvm::FixedVectorType>(vecTy)->getElementType();
                newVal = convertToVectorElement(newVal, elemTy);
                llvm::Value* updated = builder->CreateInsertElement(vec, newVal, idxVal, "simd.insert");
                builder->CreateStore(updated, alloca);
                return updated;
            }
        }
    }

    llvm::Value* arrVal = generateExpression(expr->array.get());
    llvm::Value* idxVal = generateExpression(expr->index.get());
    llvm::Value* newVal = generateExpression(expr->value.get());
    newVal = toDefaultType(newVal);
    idxVal = toDefaultType(idxVal);

    // Detect whether the base is a string (raw char* without a length header).
    const bool isStr = arrVal->getType()->isPointerTy() || isStringExpr(expr->array.get());

    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    llvm::Value* basePtr =
        arrVal->getType()->isPointerTy() ? arrVal : builder->CreateIntToPtr(arrVal, ptrTy, "idxa.baseptr");

    // Bounds check — elided in OPTMAX functions where compile-time
    // ownership analysis guarantees safety (zero-cost abstraction).
    // Also elided in @hot functions at O2+ and when the index is a
    // provably-safe for-loop iterator.
    bool boundsCheckElidedA = inOptMaxFunction
        || (currentFuncHintHot_ && !isStr && optimizationLevel >= OptimizationLevel::O2);

    // Ownership-aware: borrowed arrays cannot resize, so length is stable.
    bool arrayIsBorrowedA = false;
    if (!isStr) {
        if (expr->array->type == ASTNodeType::IDENTIFIER_EXPR)
            arrayIsBorrowedA = isVariableBorrowed(static_cast<IdentifierExpr*>(expr->array.get())->name);
    }

    if (!boundsCheckElidedA && !isStr && optimizationLevel >= OptimizationLevel::O1) {
        IdentifierExpr* idxIdent = (expr->index->type == ASTNodeType::IDENTIFIER_EXPR)
            ? static_cast<IdentifierExpr*>(expr->index.get()) : nullptr;
        if (idxIdent && safeIndexVars_.count(idxIdent->name)) {
            auto it = loopIterEndBound_.find(idxIdent->name);
            if (it != loopIterEndBound_.end()) {
                llvm::Value* endBound = it->second;
                auto* lenLoadAElim = builder->CreateLoad(getDefaultType(), basePtr, "idxa.len.elim");
                lenLoadAElim->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
                lenLoadAElim->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
                llvm::Value* lenVal = lenLoadAElim;
                if (endBound == lenVal) {
                    boundsCheckElidedA = true;
                }
                if (!boundsCheckElidedA) {
                    if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endBound)) {
                        if (auto* lenCI = llvm::dyn_cast<llvm::ConstantInt>(lenVal)) {
                            if (endCI->getSExtValue() <= lenCI->getSExtValue()) {
                                boundsCheckElidedA = true;
                            }
                        }
                    }
                }
                if (!boundsCheckElidedA && !dynamicCompilation_
                    && optimizationLevel >= OptimizationLevel::O2) {
                    llvm::Value* endLELen = builder->CreateICmpSLE(endBound, lenVal, "idxa.endlelen");
                    llvm::Function* assumeFn = OMSC_GET_INTRINSIC(
                        module.get(), llvm::Intrinsic::assume, {});
                    builder->CreateCall(assumeFn, {endLELen});
                }
            }
        }
    }

    if (!boundsCheckElidedA) {
        llvm::Function* function = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* okBB = llvm::BasicBlock::Create(*context, "idxa.ok", function);
        llvm::BasicBlock* failBB = llvm::BasicBlock::Create(*context, "idxa.fail", function);

        llvm::Value* lenVal;
        if (isStr) {
            lenVal = builder->CreateCall(getOrDeclareStrlen(), {basePtr}, "idxa.strlen");
        } else {
            auto* lenLoad = builder->CreateLoad(getDefaultType(), basePtr, "idxa.len");
            lenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
            lenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            // Borrowed arrays cannot resize — length is invariant.
            if (arrayIsBorrowedA) {
                lenLoad->setMetadata(llvm::LLVMContext::MD_invariant_load,
                                     llvm::MDNode::get(*context, {}));
            }
            lenVal = lenLoad;
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
    }

    if (isStr) {
        // Truncate to i8 and store at byte offset index
        llvm::Value* byteVal = builder->CreateTrunc(newVal, llvm::Type::getInt8Ty(*context), "idxa.byte");
        llvm::Value* charPtr = builder->CreateGEP(llvm::Type::getInt8Ty(*context), basePtr, idxVal, "idxa.charptr");
        builder->CreateStore(byteVal, charPtr);
    } else {
        // Array: store i64 element at slot (index + 1)
        // Use two-step GEP so LLVM can hoist the data-pointer out of loops.
        // inbounds GEP: OmScript arrays are contiguous heap allocations,
        // so the element pointer is always within the allocated region.
        llvm::Value* dataPtr = builder->CreateInBoundsGEP(getDefaultType(), basePtr,
            llvm::ConstantInt::get(getDefaultType(), 1), "idxa.data");
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), dataPtr, idxVal, "idxa.elem.ptr");
        auto* elemStore = builder->CreateAlignedStore(newVal, elemPtr, llvm::MaybeAlign(8));
        elemStore->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
        // When inside a parallel loop, attach the access group metadata so
        // LLVM's vectorizer and Polly know this store is iteration-independent.
        if (currentLoopAccessGroup_)
            elemStore->setMetadata(llvm::LLVMContext::MD_access_group, currentLoopAccessGroup_);
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
    const size_t numFields = fields.size();

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
    const std::string structType = resolveStructType(expr->object.get());
    const size_t fieldIdx = resolveFieldIndex(structType, expr->fieldName, expr);

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
        // noalias → !alias.scope + !noalias metadata pair.
        // The scope includes a reference to the domain so LLVM's
        // ScopedNoAliasAA can reason about aliasing across struct fields.
        // The scope name includes the struct type and field name to
        // distinguish different field accesses for better alias analysis.
        if (attrs->noalias) {
            llvm::MDNode* domain = llvm::MDNode::getDistinct(*context,
                {llvm::MDString::get(*context, "struct.noalias.domain")});
            domain->replaceOperandWith(0, domain);
            std::string scopeName = "struct.noalias." + structType + "." + expr->fieldName;
            llvm::MDNode* scope = llvm::MDNode::getDistinct(*context,
                {nullptr, domain, llvm::MDString::get(*context, scopeName)});
            scope->replaceOperandWith(0, scope);
            llvm::MDNode* scopeList = llvm::MDNode::get(*context, {scope});
            load->setMetadata(llvm::LLVMContext::MD_alias_scope, scopeList);
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
    const std::string structType = resolveStructType(expr->object.get());
    const size_t fieldIdx = resolveFieldIndex(structType, expr->fieldName, expr);

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
