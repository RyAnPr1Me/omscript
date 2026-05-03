#include "codegen.h"
#include "diagnostic.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/Support/KnownBits.h>
#include <set>
#include <stdexcept>
#include <unordered_set>

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

    // Codegen safety-net: a VarDecl that escaped parser checks is allowed
    if (stmt->typeName.empty() && !stmt->initializer && !stmt->isCompilerGenerated) {
        codegenError("Variable '" + stmt->name + "' has no type annotation and no initializer. "
                     "All user-declared variables require an explicit type "
                     "(e.g., 'var " + stmt->name + ":i64 = ...') unless an "
                     "initializer is present from which the type can be inferred.", stmt);
    }

    // When the variable is declared global (inside a function), create a
    // module-level GlobalVariable instead of a stack alloca.
    if (stmt->isGlobal) {
        // Always use the original unqualified name; no mangling.
        const std::string llvmName = stmt->name;
        // If already created (e.g., by generateGlobals), just record it.
        if (auto* existing = module->getGlobalVariable(llvmName)) {
            namedValues[stmt->name] = existing;
            globalVars_[llvmName] = existing;
            return;
        }
        llvm::Type* ty = stmt->typeName.empty() ? getDefaultType() : resolveAnnotatedType(stmt->typeName);
        auto* gv = new llvm::GlobalVariable(
            *module, ty,
            stmt->isConst,
            llvm::GlobalValue::ExternalLinkage,
            llvm::Constant::getNullValue(ty),
            llvmName);
        gv->setAlignment(llvm::MaybeAlign(8));
        globalVars_[llvmName] = gv;
        namedValues[stmt->name] = gv;
        // If there is an initializer, generate it and store into the global.
        if (stmt->initializer) {
            llvm::Value* initVal = generateExpression(stmt->initializer.get());
            if (initVal->getType() != ty)
                initVal = convertTo(initVal, ty);
            builder->CreateStore(initVal, gv);
        }
        return;
    }

    // ───────────────────────────────────────────────────────────────────────
    if (!stmt->typeName.empty() && stmt->typeName[0] == '&' &&
        stmt->initializer &&
        stmt->initializer->type == ASTNodeType::BORROW_EXPR) {
        auto* bw = static_cast<BorrowExpr*>(stmt->initializer.get());
        // Require explicit `&` on the initializer for the reference form.
        if (!bw->source ||
            bw->source->type != ASTNodeType::UNARY_EXPR ||
            static_cast<UnaryExpr*>(bw->source.get())->op != "&") {
            codegenError("Reference borrow '" + stmt->name +
                         "' requires '&' on the initializer (e.g. `borrow var " +
                         stmt->name + ":" + stmt->typeName + " = &x;`)", stmt);
        }
        // generateBorrowExpr validates borrow state for the source variable
        llvm::Value* ptrVal = generateBorrowExpr(bw);
        if (!ptrVal->getType()->isPointerTy()) {
            codegenError("Reference borrow source for '" + stmt->name +
                         "' did not produce a pointer", stmt);
        }
        // Allocate a pointer slot for the reference variable itself.
        llvm::Type* refStorageTy = llvm::PointerType::getUnqual(*context);
        llvm::AllocaInst* refAlloca =
            createEntryBlockAlloca(function, stmt->name, refStorageTy);
        builder->CreateAlignedStore(ptrVal, refAlloca, llvm::MaybeAlign(8));
        // Strip the leading '&' to get the element annotation (e.g. "i64").
        const std::string elemAnnot = stmt->typeName.substr(1);
        refVarElemTypes_[stmt->name] = elemAnnot;
        // Bind the variable so subsequent identifier lookups find it.  The
        bindVariableAnnotated(stmt->name, refAlloca, stmt->typeName, stmt->isConst);
        // Register the borrow alias (releases the source's borrow count when
        // the scope ends).  pendingSrcVar was set by generateBorrowExpr.
        if (!bw->pendingSrcVar.empty()) {
            if (bw->isMut) {
                markVariableMutBorrowed(stmt->name, bw->pendingSrcVar);
            } else {
                markVariableBorrowed(stmt->name, bw->pendingSrcVar);
            }
            bw->pendingSrcVar.clear();
        }
        return;
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
            // Check if this array literal is eligible for stack allocation.
            bool useStackAlloc = false;
            bool useReadOnlyGlobal = false;
            if (stmt->initializer->type == ASTNodeType::ARRAY_EXPR &&
                optimizationLevel >= OptimizationLevel::O1) {
                auto* arrExpr = static_cast<ArrayExpr*>(stmt->initializer.get());
                bool hasSpreadElem = false;
                bool allIntLiterals = true;
                for (const auto& elem : arrExpr->elements) {
                    if (elem->type == ASTNodeType::SPREAD_EXPR) {
                        hasSpreadElem = true;
                        break;
                    }
                    if (elem->type == ASTNodeType::LITERAL_EXPR &&
                        static_cast<LiteralExpr*>(elem.get())->literalType ==
                            LiteralExpr::LiteralType::INTEGER) {
                        continue;  // OK
                    }
                    // Negative integer literal: unary `-` on integer.
                    if (elem->type == ASTNodeType::UNARY_EXPR) {
                        auto* un = static_cast<UnaryExpr*>(elem.get());
                        if (un->op == "-" && un->operand &&
                            un->operand->type == ASTNodeType::LITERAL_EXPR &&
                            static_cast<LiteralExpr*>(un->operand.get())
                                    ->literalType ==
                                LiteralExpr::LiteralType::INTEGER) {
                            continue;
                        }
                    }
                    allIntLiterals = false;
                }
                const size_t n = arrExpr->elements.size();
                if (!hasSpreadElem) {
                    // Strongest form: bind the var to a private global
                    if (allIntLiterals && n >= 2 &&
                        optimizationLevel >= OptimizationLevel::O2 &&
                        doesVarHaveOnlyReadOnlyUses(stmt->name)) {
                        useReadOnlyGlobal = true;
                    } else if (stmt->isConst && n <= kMaxStackArrayElements) {
                        useStackAlloc = true;
                    } else if (!stmt->isConst && allIntLiterals && n <= 16 &&
                               !doesVarEscapeCurrentScope(stmt->name)) {
                        useStackAlloc = true;
                    }
                }
                if (useReadOnlyGlobal) {
                    pendingArrayReadOnlyGlobal_ = true;
                } else if (useStackAlloc) {
                    pendingArrayStackAlloc_ = true;
                }
            }
            // ── array_fill(N, V) → ro-global LUT ────────────────────────────
            else if (stmt->initializer->type == ASTNodeType::CALL_EXPR &&
                     optimizationLevel >= OptimizationLevel::O2) {
                auto* call = static_cast<CallExpr*>(stmt->initializer.get());
                if (call->callee == "array_fill" &&
                    call->arguments.size() == 2) {
                    auto getConstInt = [](const Expression* e,
                                          int64_t& out) -> bool {
                        if (!e) return false;
                        if (e->type == ASTNodeType::LITERAL_EXPR) {
                            auto* lit = static_cast<const LiteralExpr*>(e);
                            if (lit->literalType == LiteralExpr::LiteralType::INTEGER) {
                                out = lit->intValue;
                                return true;
                            }
                        }
                        if (e->type == ASTNodeType::UNARY_EXPR) {
                            auto* un = static_cast<const UnaryExpr*>(e);
                            if (un->op == "-" && un->operand &&
                                un->operand->type == ASTNodeType::LITERAL_EXPR) {
                                auto* lit = static_cast<const LiteralExpr*>(un->operand.get());
                                if (lit->literalType == LiteralExpr::LiteralType::INTEGER) {
                                    out = -lit->intValue;
                                    return true;
                                }
                            }
                        }
                        return false;
                    };
                    int64_t nVal = 0, vVal = 0;
                    if (getConstInt(call->arguments[0].get(), nVal) &&
                        getConstInt(call->arguments[1].get(), vVal) &&
                        nVal >= 2 && nVal <= 1024 &&
                        doesVarHaveOnlyReadOnlyUses(stmt->name)) {
                        useReadOnlyGlobal = true;
                        pendingArrayReadOnlyGlobal_ = true;
                    }
                }
            }
            initValue = generateExpression(stmt->initializer.get());
            if (useReadOnlyGlobal) {
                pendingArrayReadOnlyGlobal_ = false;
                readOnlyGlobalArrays_.insert(stmt->name);
                // Treat ro-global arrays the same as stack-allocated arrays
                stackAllocatedArrays_.insert(stmt->name);
                optStats_.roGlobalArrays++;
            } else if (useStackAlloc) {
                pendingArrayStackAlloc_ = false;
                stackAllocatedArrays_.insert(stmt->name);
                optStats_.escapeStackAllocs++;
            }
        }

        // When no annotation is present, infer the type from the initializer.
        if (stmt->typeName.empty())
            allocaType = initValue->getType();

        // When a string literal is assigned to a mutable string variable,
        if (!stmt->isConst &&
            stmt->initializer->type == ASTNodeType::LITERAL_EXPR &&
            static_cast<LiteralExpr*>(stmt->initializer.get())->literalType ==
                LiteralExpr::LiteralType::STRING) {
            initValue = builder->CreateCall(getOrDeclareStrdup(), {initValue}, "strdup.init");
            allocaType = initValue->getType();
        }

        // Convert the initializer to match the declared type when an annotation
        if (!stmt->typeName.empty() && !isSimdType)
            initValue = convertTo(initValue, allocaType);
    }

    llvm::AllocaInst* alloca = createEntryBlockAlloca(function, stmt->name, allocaType);

    // Register keyword: force the variable into a CPU register.
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

    // Track dict variables so dict["key"] routes to map_get IR.
    {
        bool isDict = false;
        if (!stmt->typeName.empty() &&
            (stmt->typeName == "dict" || stmt->typeName.rfind("dict[", 0) == 0)) {
            isDict = true;
        }
        if (!isDict && stmt->initializer) {
            if (stmt->initializer->type == ASTNodeType::DICT_EXPR) {
                isDict = true;
            } else if (stmt->initializer->type == ASTNodeType::CALL_EXPR) {
                auto* call = static_cast<CallExpr*>(stmt->initializer.get());
                if (call->callee == "map_new" || call->callee == "map_set" ||
                    call->callee == "map_remove") {
                    isDict = true;
                }
            }
        }
        if (isDict)
            dictVarNames_.insert(stmt->name);
    }

    // Track `ptr` / `ptr<T>` variables so isStringExpr() can exclude them
    {
        const std::string& tn = stmt->typeName;
        const bool isPtr = (tn == "ptr" || tn.rfind("ptr<", 0) == 0);
        if (isPtr) {
            ptrVarNames_.insert(stmt->name);
            if (tn.rfind("ptr<", 0) == 0 && tn.back() == '>') {
                // Extract inner element type: ptr<T> → T
                ptrElemTypes_[stmt->name] = tn.substr(4, tn.size() - 5);
            } else {
                // Untyped `ptr`: record as "ptr" sentinel so callers know this
                ptrElemTypes_[stmt->name] = "ptr";
            }
            // ── Element-addressed &x inference ─────────────────────────────
            if (stmt->initializer &&
                stmt->initializer->type == ASTNodeType::UNARY_EXPR) {
                const auto* ue = static_cast<const UnaryExpr*>(stmt->initializer.get());
                if (ue->op == "&" && ue->operand &&
                    ue->operand->type == ASTNodeType::IDENTIFIER_EXPR) {
                    const std::string& srcName =
                        static_cast<const IdentifierExpr*>(ue->operand.get())->name;
                    // Only override if we don't already have an explicit element type
                    // (i.e., the annotation was just bare `ptr`, not `ptr<T>`).
                    const bool hasExplicitElemType =
                        (tn.rfind("ptr<", 0) == 0 && tn.back() == '>');
                    if (!hasExplicitElemType) {
                        auto tit = varTypeAnnotations_.find(srcName);
                        if (tit != varTypeAnnotations_.end() && !tit->second.empty()) {
                            ptrElemTypes_[stmt->name] = tit->second;
                        }
                    }
                }
            }
            // Determine heap vs stack origin for invalidate:
            if (stmt->initializer) {
                if (lastStackAllocBacking_) {
                    // Stack allocation: record the backing alloca for lifetime.end.
                    stackPtrBackingAlloca_[stmt->name] = lastStackAllocBacking_;
                    heapPtrVarNames_.erase(stmt->name);
                    lastStackAllocBacking_ = nullptr;
                } else if (stmt->initializer->type == ASTNodeType::CALL_EXPR) {
                    auto* call = static_cast<CallExpr*>(stmt->initializer.get());
                    const bool isHeapAlloc =
                        call->callee == "malloc" || call->callee == "realloc" ||
                        call->callee == "calloc" ||
                        (call->callee.rfind("alloc<", 0) == 0 && call->callee.back() == '>');
                    if (isHeapAlloc) {
                        heapPtrVarNames_.insert(stmt->name);
                        stackPtrBackingAlloca_.erase(stmt->name);
                    } else {
                        heapPtrVarNames_.erase(stmt->name);
                        stackPtrBackingAlloca_.erase(stmt->name);
                    }
                } else {
                    heapPtrVarNames_.erase(stmt->name);
                    stackPtrBackingAlloca_.erase(stmt->name);
                }
            }
        } else {
            ptrVarNames_.erase(stmt->name);
            ptrElemTypes_.erase(stmt->name);
            heapPtrVarNames_.erase(stmt->name);
            stackPtrBackingAlloca_.erase(stmt->name);
        }
    }

    // Track array variables so isStringExpr() can distinguish array pointers
    // from string pointers (both now use pointer-typed allocas).
    {
        bool isArray = false;
        if (!stmt->typeName.empty() &&
            stmt->typeName.size() >= 2 &&
            stmt->typeName.compare(stmt->typeName.size() - 2, 2, "[]") == 0) {
            isArray = true;
        }
        if (!isArray && stmt->initializer) {
            if (stmt->initializer->type == ASTNodeType::ARRAY_EXPR) {
                isArray = true;
            } else if (stmt->initializer->type == ASTNodeType::CALL_EXPR) {
                auto* call = static_cast<CallExpr*>(stmt->initializer.get());
                if (call->callee == "array_fill" || call->callee == "array_concat" ||
                    call->callee == "array_copy" || call->callee == "array_map" ||
                    call->callee == "array_filter" || call->callee == "array_slice" ||
                    call->callee == "push" || call->callee == "pop" ||
                    call->callee == "shift" || call->callee == "unshift" ||
                    call->callee == "sort" || call->callee == "reverse" ||
                    call->callee == "array_remove" || call->callee == "array_reduce" ||
                    call->callee == "str_split" || call->callee == "str_chars" ||
                    arrayReturningFunctions_.count(call->callee)) {
                    isArray = true;
                }
            }
        }
        if (isArray)
            arrayVars_.insert(stmt->name);
        else
            arrayVars_.erase(stmt->name);
    }

    bindVariableAnnotated(stmt->name, alloca, stmt->typeName, stmt->isConst);

    // If the initializer was a borrow expression, register the alias mapping
    if (stmt->initializer && stmt->initializer->type == ASTNodeType::BORROW_EXPR) {
        auto* bw = static_cast<BorrowExpr*>(stmt->initializer.get());
        if (!bw->pendingSrcVar.empty()) {
            if (bw->isMut) {
                markVariableMutBorrowed(stmt->name, bw->pendingSrcVar);
            } else {
                markVariableBorrowed(stmt->name, bw->pendingSrcVar);
            }
            bw->pendingSrcVar.clear(); // consumed
        }
    }

    // If the initializer was a reborrow expression, register the alias mapping.
    if (stmt->initializer && stmt->initializer->type == ASTNodeType::REBORROW_EXPR) {
        auto* rb = static_cast<ReborrowExpr*>(stmt->initializer.get());
        if (!rb->pendingSrcVar.empty()) {
            if (rb->isMut) {
                markVariableMutBorrowed(stmt->name, rb->pendingSrcVar);
            } else {
                markVariableBorrowed(stmt->name, rb->pendingSrcVar);
            }
            rb->pendingSrcVar.clear(); // consumed
        }
    }

    if (initValue) {
        builder->CreateStore(initValue, alloca);
        // Track non-negativity: if a variable is initialized with a
        if (allocaType->isIntegerTy()) {
            bool initNonNeg = nonNegValues_.count(initValue) > 0;
            if (!initNonNeg) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(initValue))
                    initNonNeg = !ci->isNegative();
            }
            // KnownBits fallback: detect non-negativity through LLVM's
            if (!initNonNeg && optimizationLevel >= OptimizationLevel::O1) {
                llvm::KnownBits kb = llvm::computeKnownBits(
                    initValue, module->getDataLayout());
                initNonNeg = kb.isNonNegative();
            }
            if (initNonNeg)
                nonNegValues_.insert(alloca);
            // Propagate tight upper bound for modular arithmetic.
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
            // Track constant integer values for `const` variables and
            bool isComptimeInit = stmt->initializer &&
                                  stmt->initializer->type == ASTNodeType::COMPTIME_EXPR;
            if (stmt->isConst || isComptimeInit) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(initValue))
                    constIntFolds_[stmt->name] = ci->getSExtValue();
            }
        }
        // Track const float values for compile-time float expression folding.
        if (stmt->isConst && initValue->getType()->isDoubleTy()) {
            if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(initValue))
                constFloatFolds_[stmt->name] = cf->getValueAPF().convertToDouble();
        }
        // Track const / comptime string initializers for compile-time string
        if (stmt->initializer) {
            bool isComptimeInit2 = stmt->initializer->type == ASTNodeType::COMPTIME_EXPR;
            if (stmt->isConst || isComptimeInit2) {
                std::string foldedStr;
                if (tryFoldStringConcat(stmt->initializer.get(), foldedStr))
                    constStringFolds_[stmt->name] = std::move(foldedStr);
            }
        }
        // Track const / comptime array initializers for compile-time array
        // indexing: `const arr = [1,2,3]; var x = arr[1];` folds to 2.
        if (stmt->initializer) {
            bool hasComptimeInit = stmt->initializer->type == ASTNodeType::COMPTIME_EXPR;
            if (stmt->isConst || hasComptimeInit) {
                auto foldedArr = tryFoldExprToConst(stmt->initializer.get());
                if (foldedArr && foldedArr->kind == ConstValue::Kind::Array)
                    constArrayFolds_[stmt->name] = std::move(foldedArr->arrVal);
            }
        }
        // ── CF-CTRE scope propagation ──────────────────────────────────────
        if (ctEngine_) {
            bool hasComptimeInit3 = stmt->initializer &&
                                    stmt->initializer->type == ASTNodeType::COMPTIME_EXPR;
            if (hasComptimeInit3 && lastComptimeCtResult_) {
                // Case 1 — consume the stashed CT result.
                const CTValue& ctv = *lastComptimeCtResult_;
                // Back-fill legacy fold maps if the old evaluator missed them.
                if (ctv.isInt()) {
                    if (!constIntFolds_.count(stmt->name))
                        constIntFolds_[stmt->name] = ctv.asI64();
                } else if (ctv.isString()) {
                    if (!constStringFolds_.count(stmt->name))
                        constStringFolds_[stmt->name] = ctv.asStr();
                } else if (ctv.isArray() && !constArrayFolds_.count(stmt->name)) {
                    auto cv = ctValueToConstValue(ctv);
                    if (cv.kind == ConstValue::Kind::Array)
                        constArrayFolds_[stmt->name] = std::move(cv.arrVal);
                }
                lastComptimeCtResult_.reset();
            } else if (!hasComptimeInit3 && initValue) {
                // Case 2 — non-comptime init folded to a scalar constant.
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(initValue)) {
                    if (!scopeComptimeInts_.count(stmt->name))
                        scopeComptimeInts_[stmt->name] = ci->getSExtValue();
                }
            }
        }
        // Propagate integer constant from a zero-param constant-returning fn:
        if (stmt->isConst && stmt->initializer &&
            stmt->initializer->type == ASTNodeType::CALL_EXPR) {
            auto* callInit = static_cast<CallExpr*>(stmt->initializer.get());
            if (callInit->arguments.empty() && optCtx_) {
                auto v = optCtx_->constIntReturn(callInit->callee);
                if (v) constIntFolds_.try_emplace(stmt->name, *v);
            }
        }
        // Track whether this variable holds a string value so that print(),
        if (isStringExpr(stmt->initializer.get()))
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
        if (stmt->initializer->type == ASTNodeType::CALL_EXPR) {
            auto* callExpr = static_cast<CallExpr*>(stmt->initializer.get());
            if (callExpr->callee == "array_fill" && callExpr->arguments.size() == 2) {
                // The initValue is the result of array_fill, but sizeArg
                auto* sizeExpr = callExpr->arguments[0].get();
                if (sizeExpr->type == ASTNodeType::LITERAL_EXPR) {
                    auto* lit = static_cast<LiteralExpr*>(sizeExpr);
                    if (lit->literalType == LiteralExpr::LiteralType::INTEGER && lit->intValue > 0) {
                        knownArraySizes_[stmt->name] =
                            llvm::ConstantInt::get(getDefaultType(), lit->intValue);
                    }
                } else if (sizeExpr->type == ASTNodeType::IDENTIFIER_EXPR) {
                    // Variable-size array_fill(varName, val): track the alloca of
                    auto* varIdent = static_cast<IdentifierExpr*>(sizeExpr);
                    auto varIt = namedValues.find(varIdent->name);
                    if (varIt != namedValues.end()) {
                        if (auto* sizeAlloca = llvm::dyn_cast<llvm::AllocaInst>(varIt->second)) {
                            knownArraySizeAllocas_[stmt->name] = sizeAlloca;
                        }
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
        if (isStringExpr(stmt->value.get())) {
            if (builder->GetInsertBlock() && builder->GetInsertBlock()->getParent())
                stringReturningFunctions_.insert(builder->GetInsertBlock()->getParent()->getName());
        }
        // Convert return value to match the function's declared return type.
        llvm::Function* currentFn = builder->GetInsertBlock()->getParent();
        llvm::Type* retTy = currentFn->getReturnType();
        retValue = convertTo(retValue, retTy);

        // Tail call optimization: if the return value is a direct function
        if (optimizationLevel >= OptimizationLevel::O1) {
            if (auto* callInst = llvm::dyn_cast<llvm::CallInst>(retValue)) {
                // Self-recursive tail calls get musttail: this GUARANTEES the
                llvm::Function* calledFn = callInst->getCalledFunction();
                if (calledFn && calledFn == currentFn &&
                    calledFn->getReturnType() == currentFn->getReturnType() &&
                    calledFn->arg_size() == callInst->arg_size()) {
                    // Verify all parameter types match (required by musttail).
                    bool typesMatch = true;
                    for (unsigned i = 0; i < calledFn->arg_size(); ++i) {
                        if (callInst->getArgOperand(i)->getType() !=
                            calledFn->getFunctionType()->getParamType(i)) {
                            typesMatch = false;
                            break;
                        }
                    }
                    if (typesMatch) {
                        callInst->setTailCallKind(llvm::CallInst::TCK_MustTail);
                    } else {
                        callInst->setTailCallKind(llvm::CallInst::TCK_Tail);
                    }
                } else {
                    callInst->setTailCallKind(llvm::CallInst::TCK_Tail);
                }
            }
        }

        // @prefetch enforcement: determine which prefetched parameter (if any)
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
        if (retTy->isVoidTy())
            builder->CreateRetVoid();
        else
            builder->CreateRet(llvm::Constant::getNullValue(retTy));
    }
}

void CodeGenerator::generateIf(IfStmt* stmt) {
    // ── CF-CTRE abstract-interpretation dead-branch elimination (Q2) ─────
    if (optCtx_) {
        if (optCtx_->isThenBranchDead(stmt)) {
            // Condition is always false — only generate else branch.
            if (stmt->elseBranch) generateStatement(stmt->elseBranch.get());
            return;
        }
        if (optCtx_->isElseBranchDead(stmt)) {
            // Condition is always true — only generate then branch.
            generateStatement(stmt->thenBranch.get());
            return;
        }
    }

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

    // ── If-else to select conversion ─────────────────────────────────
    if (optimizationLevel >= OptimizationLevel::O2 && stmt->elseBranch) {
        // Extract the single assignment from each branch.
        auto extractSingleAssign = [](Statement* branch) -> AssignExpr* {
            // Direct expression statement
            if (branch->type == ASTNodeType::EXPR_STMT) {
                auto* exprStmt = static_cast<ExprStmt*>(branch);
                if (exprStmt->expression->type == ASTNodeType::ASSIGN_EXPR)
                    return static_cast<AssignExpr*>(exprStmt->expression.get());
            }
            // Block with a single statement
            if (branch->type == ASTNodeType::BLOCK) {
                auto* block = static_cast<BlockStmt*>(branch);
                if (block->statements.size() == 1 &&
                    block->statements[0]->type == ASTNodeType::EXPR_STMT) {
                    auto* exprStmt = static_cast<ExprStmt*>(block->statements[0].get());
                    if (exprStmt->expression->type == ASTNodeType::ASSIGN_EXPR)
                        return static_cast<AssignExpr*>(exprStmt->expression.get());
                }
            }
            return nullptr;
        };

        auto* thenAssign = extractSingleAssign(stmt->thenBranch.get());
        auto* elseAssign = extractSingleAssign(stmt->elseBranch.get());

        if (thenAssign && elseAssign && thenAssign->name == elseAssign->name) {
            // Check that both RHS values are simple side-effect-free expressions.
            auto isSimpleValue = [](Expression* e) -> bool {
                if (e->type == ASTNodeType::LITERAL_EXPR) return true;
                if (e->type == ASTNodeType::IDENTIFIER_EXPR) return true;
                if (e->type == ASTNodeType::UNARY_EXPR) {
                    auto* u = static_cast<UnaryExpr*>(e);
                    return (u->op == "-" || u->op == "~" || u->op == "!") &&
                           (u->operand->type == ASTNodeType::LITERAL_EXPR ||
                            u->operand->type == ASTNodeType::IDENTIFIER_EXPR);
                }
                if (e->type == ASTNodeType::BINARY_EXPR) {
                    auto* b = static_cast<BinaryExpr*>(e);
                    const bool lhsSimple = b->left->type == ASTNodeType::IDENTIFIER_EXPR ||
                                           b->left->type == ASTNodeType::LITERAL_EXPR;
                    const bool rhsSimple = b->right->type == ASTNodeType::IDENTIFIER_EXPR ||
                                           b->right->type == ASTNodeType::LITERAL_EXPR;
                    const std::string& op = b->op;
                    const bool safeOp = (op == "+" || op == "-" || op == "*" ||
                                         op == "&" || op == "|" || op == "^" ||
                                         op == "<<" || op == ">>");
                    return lhsSimple && rhsSimple && safeOp;
                }
                return false;
            };

            if (isSimpleValue(thenAssign->value.get()) &&
                isSimpleValue(elseAssign->value.get())) {
                // Emit both values eagerly and use select.
                llvm::Value* thenVal = generateExpression(thenAssign->value.get());
                llvm::Value* elseVal = generateExpression(elseAssign->value.get());
                // Type-match for select.
                if (thenVal->getType() != elseVal->getType()) {
                    if (thenVal->getType()->isDoubleTy() || elseVal->getType()->isDoubleTy()) {
                        if (!thenVal->getType()->isDoubleTy())
                            thenVal = ensureFloat(thenVal);
                        if (!elseVal->getType()->isDoubleTy())
                            elseVal = ensureFloat(elseVal);
                    } else {
                        thenVal = toDefaultType(thenVal);
                        elseVal = toDefaultType(elseVal);
                    }
                }
                llvm::Value* sel = builder->CreateSelect(condBool, thenVal, elseVal, "ifsel");
                // Store to the target variable.
                auto it = namedValues.find(thenAssign->name);
                if (it != namedValues.end()) {
                    builder->CreateStore(sel, it->second);
                    // Track non-negativity of the result.
                    bool tNonNeg = nonNegValues_.count(thenVal) > 0;
                    bool eNonNeg = nonNegValues_.count(elseVal) > 0;
                    if (!tNonNeg) {
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(thenVal))
                            tNonNeg = !ci->isNegative();
                    }
                    if (!eNonNeg) {
                        if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(elseVal))
                            eNonNeg = !ci->isNegative();
                    }
                    if (tNonNeg && eNonNeg)
                        nonNegValues_.insert(it->second);
                    else
                        nonNegValues_.erase(it->second);
                    return;
                }
            }
        }
    }

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
    if (stmt->hintLikely || stmt->hintUnlikely) {
        const uint32_t thenWeight = stmt->hintLikely ? 2000 : 1;
        const uint32_t elseWeight = stmt->hintLikely ? 1 : 2000;
        llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(thenWeight, elseWeight);
        br->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
    } else if (optimizationLevel >= OptimizationLevel::O2) {
        // Infer branch probability from zero/null equality patterns.
        [&]() {
            // Peel outermost icmp ne %x, 0 (from toBool)
            auto* outerNE = llvm::dyn_cast<llvm::ICmpInst>(condBool);
            if (!outerNE || outerNE->getPredicate() != llvm::ICmpInst::ICMP_NE) return;
            auto* outerRHS = llvm::dyn_cast<llvm::ConstantInt>(outerNE->getOperand(1));
            if (!outerRHS || !outerRHS->isZero()) return;
            // Peel optional zext
            llvm::Value* inner = outerNE->getOperand(0);
            if (auto* z = llvm::dyn_cast<llvm::ZExtInst>(inner))
                inner = z->getOperand(0);
            // Must be an icmp eq/ne against zero
            auto* innerCmp = llvm::dyn_cast<llvm::ICmpInst>(inner);
            if (!innerCmp) return;
            llvm::ConstantInt* zeroC = nullptr;
            llvm::Value* tested = nullptr;
            if ((zeroC = llvm::dyn_cast<llvm::ConstantInt>(innerCmp->getOperand(1))))
                tested = innerCmp->getOperand(0);
            else if ((zeroC = llvm::dyn_cast<llvm::ConstantInt>(innerCmp->getOperand(0))))
                tested = innerCmp->getOperand(1);
            if (!zeroC || !zeroC->isZero() || !tested) return;
            const bool isEqZero = (innerCmp->getPredicate() == llvm::ICmpInst::ICMP_EQ);
            const bool isNeZero = (innerCmp->getPredicate() == llvm::ICmpInst::ICMP_NE);
            if (!isEqZero && !isNeZero) return;
            // eq 0 → then-branch (zero path) is rare; ne 0 → then-branch is common.
            const uint32_t thenW = isEqZero ? 1u : 99u;
            const uint32_t elseW = isEqZero ? 99u : 1u;
            llvm::MDNode* w = llvm::MDBuilder(*context).createBranchWeights(thenW, elseW);
            br->setMetadata(llvm::LLVMContext::MD_prof, w);
        }();
    }

    // Save pre-if non-neg state so each branch starts from the same baseline.
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
        for (llvm::Value* v : preIfNonNeg) {
            if (!thenNonNeg.count(v))
                nonNegValues_.erase(v);
        }
    }

    // Merge block — if it has no predecessors (both then and else terminated
    builder->SetInsertPoint(mergeBB);
    if (llvm::pred_empty(mergeBB))
        builder->CreateUnreachable();
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
        auto savedLenCacheWI = std::move(loopArrayLenCache_);
        loopArrayLenCache_.clear();
        generateStatement(stmt->body.get());
        loopArrayLenCache_ = std::move(savedLenCacheWI);
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
    if (optimizationLevel >= OptimizationLevel::O2) {
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

    auto savedLenCacheW = std::move(loopArrayLenCache_);
    loopArrayLenCache_.clear();

    // @independent for while loops: pre-create access group before body.
    llvm::MDNode* whileIndependentAccessGroup = nullptr;
    llvm::MDNode* savedWhileAccessGroup = currentLoopAccessGroup_;
    if (stmt->loopHints.independent) {
        whileIndependentAccessGroup = llvm::MDNode::getDistinct(*context, {});
        currentLoopAccessGroup_ = whileIndependentAccessGroup;
        optStats_.independentLoops++;
    }

    generateStatement(stmt->body.get());

    if (stmt->loopHints.independent) {
        currentLoopAccessGroup_ = savedWhileAccessGroup;
    }

    loopArrayLenCache_ = std::move(savedLenCacheW);
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        auto* backBrWhile = builder->CreateBr(condBB);
        // hints: mustprogress for loop-idiom recognition.
        llvm::MDNode* mustProgress =
            llvm::MDNode::get(*context, {llvm::MDString::get(*context, "llvm.loop.mustprogress")});
        llvm::SmallVector<llvm::Metadata*, 4> loopMDs;
        loopMDs.push_back(nullptr);
        loopMDs.push_back(mustProgress);
        // While-loop unrolling: when truly nested inside another loop
        if (!inOptMaxFunction && !currentFuncHintUnroll_ && loopNestDepth_ > 1 && optimizationLevel >= OptimizationLevel::O2) {
            // Emit unroll.count=2 as a suggestion instead of unroll.disable.
            loopMDs.push_back(llvm::MDNode::get(
                *context,
                {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                     llvm::Type::getInt32Ty(*context), 2u))}));
        }
        // @vectorize / @novectorize: per-function loop vectorization hints.
        const bool whileBodyHasNonPow2ModVal = bodyHasNonPow2ModuloValue_;
        if (currentFuncHintNoVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        } else if (!whileBodyHasNonPow2ModVal
                   && (currentFuncHintVectorize_
                       || (currentFuncHintHot_ && optimizationLevel >= OptimizationLevel::O2
                           && loopNestDepth_ <= 1))) {
            // @hot at O2+: auto-enable vectorization for top-level while-loops.
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        if (currentFuncHintHot_ && currentFuncHintVectorize_
            && !whileBodyHasNonPow2ModVal
            && optimizationLevel >= OptimizationLevel::O2
            && !currentFuncHintNoVectorize_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.interleave.count"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 4))}));
        }
        // parallel_accesses: suppress when the body has non-pow2 modulo
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

        // Apply per-loop @loop hints for WhileStmt
        const LoopConfig& whileLoopHints = (stmt->loopHints.unrollCount > 0 || stmt->loopHints.vectorize || stmt->loopHints.noVectorize || stmt->loopHints.parallel || stmt->loopHints.independent || stmt->loopHints.fuse)
            ? stmt->loopHints
            : currentOptMaxConfig_.loop;
        if (whileLoopHints.unrollCount > 0) {
            loopMDs.push_back(llvm::MDNode::get(
                *context,
                {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                     llvm::Type::getInt32Ty(*context), (unsigned)whileLoopHints.unrollCount))}));
        }
        if (whileLoopHints.vectorize) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        if (whileLoopHints.noVectorize) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        }
        if (whileLoopHints.parallel) {
            llvm::MDNode* accessGroup2 = llvm::MDNode::getDistinct(*context, {});
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.parallel_accesses"),
                           accessGroup2}));
        }
        // @independent for while loops
        if (stmt->loopHints.independent && whileIndependentAccessGroup) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.parallel_accesses"),
                           whileIndependentAccessGroup}));
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
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "dowhilecond", function);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(*context, "dowhileend", function);

    // Jump directly to body (execute at least once)
    builder->CreateBr(bodyBB);

    // Body block
    builder->SetInsertPoint(bodyBB);
    loopStack.push_back({endBB, condBB});
    auto savedLenCacheDW = std::move(loopArrayLenCache_);
    loopArrayLenCache_.clear();
    generateStatement(stmt->body.get());
    loopArrayLenCache_ = std::move(savedLenCacheDW);
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(condBB);
    }

    // Condition block
    builder->SetInsertPoint(condBB);
    llvm::Value* condition = generateExpression(stmt->condition.get());

    // Constant condition elimination for do-while: the body always executes
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
                   || (currentFuncHintHot_ && optimizationLevel >= OptimizationLevel::O2
                       && loopNestDepth_ <= 1)) {
            // Mirror while-loop: @hot+O2+ for top-level do-while loops.
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        if (currentFuncHintHot_ && currentFuncHintVectorize_
            && optimizationLevel >= OptimizationLevel::O2
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
    // Save/restore per-loop array dependency tracking sets.
    auto savedLoopWrittenArrays = std::move(loopWrittenArrays_);
    loopWrittenArrays_.clear();
    auto savedLoopBackwardReadArrays = std::move(loopBackwardReadArrays_);
    loopBackwardReadArrays_.clear();
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

    // Early non-negativity tracking: mark the iterator alloca as non-negative
    {
        bool startNonNeg = false;
        if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal))
            startNonNeg = startCI->getSExtValue() >= 0;
        if (!startNonNeg)
            startNonNeg = nonNegValues_.count(startVal) > 0;
        if (startNonNeg)
            nonNegValues_.insert(iterAlloca);
    }

    // Get end value
    llvm::Value* endVal = generateExpression(stmt->end.get());
    endVal = convertTo(endVal, iterType);

    // Empty range elimination: when start and end are compile-time constants
    if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
        if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endVal)) {
            if (startCI->getSExtValue() == endCI->getSExtValue()) {
                return;
            }
        }
    }

    // Get step value.  When not specified, default to +1 for ascending ranges
    llvm::Value* stepVal;
    bool stepKnownPositive = false;
    bool stepKnownNonZero = false;
    if (stmt->step) {
        stepVal = generateExpression(stmt->step.get());
        stepVal = convertTo(stepVal, iterType);
        if (auto* stepCI = llvm::dyn_cast<llvm::ConstantInt>(stepVal)) {
            stepKnownPositive = stepCI->getSExtValue() > 0;
            stepKnownNonZero  = stepCI->getSExtValue() != 0;

            // Empty-loop elimination for constant wrong-direction step:
            if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
                if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endVal)) {
                    int64_t sv = startCI->getSExtValue();
                    int64_t ev = endCI->getSExtValue();
                    int64_t step = stepCI->getSExtValue();
                    // Ascending step but start > end → condition (i < end) fails immediately.
                    // Descending step but start < end → condition (i > end) fails immediately.
                    if ((step > 0 && sv > ev) || (step < 0 && sv < ev)) {
                        return;
                    }
                }
            }
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

    // Tight upper-bound tracking for body iterator loads.
    if (stepKnownPositive) {
        const bool startNonNegConst = [&]() -> bool {
            if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(startVal))
                return ci->getSExtValue() >= 0;
            return nonNegValues_.count(startVal) > 0;
        }();
        if (startNonNegConst) {
            if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endVal)) {
                int64_t ev = endCI->getSExtValue();
                if (ev > 0 && iterType->isIntegerTy(64)) {
                    // Set tight upper bound on the iterator alloca so body loads
                    // get !range [0, ev) — much tighter than [0, INT64_MAX).
                    allocaUpperBound_[iterAlloca] = ev;
                }
            }
        }
    }

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
        // Zero step is a programming error — heavily favour the non-zero path.
        auto* stepW = llvm::MDBuilder(*context).createBranchWeights(1000, 1);
        builder->CreateCondBr(stepNonZero, condBB, stepFailBB, stepW);

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
    llvm::Value* curVal = builder->CreateAlignedLoad(iterType, iterAlloca, llvm::MaybeAlign(8), stmt->iteratorVar.c_str());
    // !range on the condition block load: when the loop has constant bounds
    if (stepKnownPositive && iterType->isIntegerTy(64)) {
        auto bit = allocaUpperBound_.find(iterAlloca);
        if (bit != allocaUpperBound_.end()) {
            // Check if step is compile-time 1 (safe to use end+1 as upper bound).
            const bool stepIsOne = [&]() -> bool {
                if (auto* stepCI = llvm::dyn_cast<llvm::ConstantInt>(stepVal))
                    return stepCI->getSExtValue() == 1;
                return false;
            }();
            if (stepIsOne && bit->second > 0 && bit->second < llvm::APInt::getSignedMaxValue(64).getSExtValue()) {
                // curVal ∈ [0, end] = [0, end+1) exclusive.
                llvm::MDBuilder mdB(*context);
                auto* curLoad = llvm::cast<llvm::LoadInst>(curVal);
                curLoad->setMetadata(llvm::LLVMContext::MD_range,
                    mdB.createRange(llvm::APInt(64, 0),
                                    llvm::APInt(64, bit->second + 1)));
            } else if (!stepIsOne) {
                // For step > 1, use the generic non-negative range.
                auto* curLoad = llvm::cast<llvm::LoadInst>(curVal);
                curLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            }
        } else if (nonNegValues_.count(iterAlloca)) {
            auto* curLoad = llvm::cast<llvm::LoadInst>(curVal);
            curLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        }
    }

    llvm::Value* continueCond;
    if (stepKnownPositive) {
        // Fast path: known ascending loop.
        const bool iterNonNeg = nonNegValues_.count(iterAlloca) > 0;
        const auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endVal);
        bool endNonNeg  = nonNegValues_.count(endVal) > 0
            || (endCI && !endCI->isNegative());
        // KnownBits fallback: detect non-negativity of end value through
        if (iterNonNeg && !endNonNeg) {
            llvm::KnownBits endKB = llvm::computeKnownBits(
                endVal, module->getDataLayout());
            endNonNeg = endKB.isNonNegative();
        }
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
        uint32_t bodyWeight = 2000;
        if (stepKnownPositive) {
            if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
                if (auto* endCI = llvm::dyn_cast<llvm::ConstantInt>(endVal)) {
                    int64_t sv = startCI->getSExtValue();
                    int64_t ev = endCI->getSExtValue();
                    if (ev > sv && (ev - sv) < 100000) {
                        bodyWeight = static_cast<uint32_t>(ev - sv);
                    }
                }
            }
        }
        llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(bodyWeight, 1);
        forCondBr->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
    }

    // Body block
    builder->SetInsertPoint(bodyBB);

    // OmScript-specific optimization: emit @llvm.assume(iter >= 0) for
    if (optimizationLevel >= OptimizationLevel::O2 && stepKnownPositive) {
        // Check if start is known non-negative (common: for (i in 0...n))
        bool startNonNeg = false;
        if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
            startNonNeg = startCI->getSExtValue() >= 0;
        }
        if (startNonNeg) {
            // Load the iterator once and reuse for all assume conditions.
            llvm::Value* iterVal = builder->CreateAlignedLoad(iterType, iterAlloca, llvm::MaybeAlign(8), "iter.assume");
            llvm::Function* assumeFn = OMSC_GET_INTRINSIC_STMT(
                module.get(), llvm::Intrinsic::assume, {});
            // Lower bound: assume iter >= 0 (always true for non-negative starts).
            llvm::Value* isNonNeg = builder->CreateICmpSGE(
                iterVal, llvm::ConstantInt::get(iterType, 0), "iter.nonneg");
            builder->CreateCall(assumeFn, {isNonNeg});
            // Tighter lower bound: when start > 0, also assume iter >= start.
            if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
                if (startCI->getSExtValue() > 0) {
                    llvm::Value* isGeStart = builder->CreateICmpSGE(
                        iterVal, startVal, "iter.ge.start");
                    builder->CreateCall(assumeFn, {isGeStart});
                }
            }
            // Upper bound: assume iter < end.  OmScript's for-loop semantics
            llvm::Value* isLtEnd = builder->CreateICmpSLT(
                iterVal, endVal, "iter.lt.end");
            builder->CreateCall(assumeFn, {isLtEnd});
            // Track the alloca as producing non-negative values so that
            nonNegValues_.insert(iterAlloca);

            // ── CF-CTRE Phase 9 IV range refinement ────────────────────
            if (optCtx_) {
                CTInterval ivRange = optCtx_->getExitRange(
                    builder->GetInsertBlock()->getParent()->getName().str(),
                    stmt->iteratorVar);
                // Only inject if the abstract range is tighter than what we
                // already know from the start/end IR values.
                if (ivRange.isRange() && !ivRange.isTop() && ivRange.lo >= 0) {
                    llvm::ConstantInt* cLo = llvm::dyn_cast<llvm::ConstantInt>(startVal);
                    llvm::ConstantInt* cHi = llvm::dyn_cast<llvm::ConstantInt>(endVal);
                    const bool knowLo = cLo && cLo->getSExtValue() == ivRange.lo;
                    const bool knowHi = cHi && cHi->getSExtValue() == ivRange.hi + 1;
                    if (!knowLo || !knowHi) {
                        // Reload the IV (same block — GVN will dedup the load).
                        llvm::Value* iv2 = builder->CreateAlignedLoad(
                            iterType, iterAlloca, llvm::MaybeAlign(8), "iter.absi");
                        if (!knowLo) {
                            llvm::Value* c = llvm::ConstantInt::get(iterType, ivRange.lo);
                            llvm::Value* cond = builder->CreateICmpSGE(iv2, c, "iter.absi.lo");
                            builder->CreateCall(assumeFn, {cond});
                        }
                        if (!knowHi) {
                            llvm::Value* c = llvm::ConstantInt::get(iterType, ivRange.hi + 1);
                            llvm::Value* cond = builder->CreateICmpSLT(iv2, c, "iter.absi.hi");
                            builder->CreateCall(assumeFn, {cond});
                        }
                    }
                }
            }
        }
    }

    // Compile-time bounds check elimination: for ascending for-loops starting
    if (stepKnownPositive && optimizationLevel >= OptimizationLevel::O1) {
        bool startNonNegForElim = false;
        if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
            startNonNegForElim = startCI->getSExtValue() >= 0;
        }
        if (startNonNegForElim) {
            safeIndexVars_.insert(stmt->iteratorVar);
            loopIterEndBound_[stmt->iteratorVar] = endVal;
            // Track loop start bound for negative-offset bounds check
            loopIterStartBound_[stmt->iteratorVar] = startVal;
            // Zero-cost abstraction: detect for(i in 0...len(arr)) pattern.
            if (stmt->end->type == ASTNodeType::CALL_EXPR) {
                auto* callEnd = static_cast<CallExpr*>(stmt->end.get());
                if (callEnd->callee == "len" && callEnd->arguments.size() == 1 &&
                    callEnd->arguments[0]->type == ASTNodeType::IDENTIFIER_EXPR) {
                    auto* arrIdent = static_cast<IdentifierExpr*>(callEnd->arguments[0].get());
                    loopIterEndArray_[stmt->iteratorVar] = arrIdent->name;
                }
            }
        }
    }

    // ── Range-to-pointer-arithmetic: loop preamble ───────────────────────────
    llvm::StringMap<llvm::Value*> savedPtrModeDataPtrs;
    llvm::StringMap<llvm::Value*> savedPtrModeLens;
    std::string savedPtrModeIterVar;

    const bool doPtrMode = stepKnownPositive
        && optimizationLevel >= OptimizationLevel::O2
        && [&]() {
               if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(startVal))
                   return ci->getSExtValue() >= 0;
               return false;
           }();

    if (doPtrMode) {
        // Scan the loop body AST (runs at compile time, not emitted as IR).
        auto arrAccesses = preScanLoopArrayAccesses(stmt->body.get(), stmt->iteratorVar);

        // Save outer loop's pointer-mode state so we can restore it after.
        savedPtrModeDataPtrs = std::move(loopPtrModeDataPtrs_);
        savedPtrModeLens     = std::move(loopPtrModeLens_);
        savedPtrModeIterVar  = loopPtrModeIterVar_;
        loopPtrModeDataPtrs_.clear();
        loopPtrModeLens_.clear();
        loopPtrModeIterVar_ = stmt->iteratorVar;

        auto* ptrTy = llvm::PointerType::getUnqual(*context);
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC_STMT(
            module.get(), llvm::Intrinsic::assume, {});

        for (auto& [arrName, isWritten] : arrAccesses) {
            // Only handle arrays that are local named variables.
            auto it = namedValues.find(arrName);
            if (it == namedValues.end()) continue;
            // Must be an alloca (local variable), not a parameter forwarded
            // as an integer (those come via function arguments).
            if (!llvm::isa<llvm::AllocaInst>(it->second)) continue;

            // Load the array pointer from the alloca (once, before the loop).
            llvm::Value* arrRaw = builder->CreateAlignedLoad(
                getDefaultType(), it->second, llvm::MaybeAlign(8),
                arrName + ".prl.raw");
            llvm::Value* basePtr =
                arrRaw->getType()->isPointerTy()
                    ? arrRaw
                    : builder->CreateIntToPtr(arrRaw, ptrTy, arrName + ".prl.base");

            // Load the length header once, before the loop.
            auto* lenLoad = builder->CreateAlignedLoad(
                getDefaultType(), basePtr, llvm::MaybeAlign(8),
                arrName + ".prl.len");
            lenLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayLen_);
            lenLoad->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            // If the array is read-only in the loop body, its length is
            // invariant across the loop — mark so LICM can hoist further.
            if (!isWritten)
                lenLoad->setMetadata(llvm::LLVMContext::MD_invariant_load,
                                     llvm::MDNode::get(*context, {}));

            // Pre-compute data pointer: &arr[0] = base + 1 element.
            llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                getDefaultType(), basePtr,
                llvm::ConstantInt::get(getDefaultType(), 1),
                arrName + ".prl.data");

            // Emit a SINGLE pre-loop range assertion: end <= len(arr).
            llvm::Value* endLELen = builder->CreateICmpULE(
                endVal, lenLoad, arrName + ".prl.safe");
            builder->CreateCall(assumeFn, {endLELen});
            // Also assert start >= 0 (already proven by doPtrMode gate, but
            // make it explicit for LLVM's range tracking).
            llvm::Value* startGE0 = builder->CreateICmpSGE(
                startVal, llvm::ConstantInt::get(getDefaultType(), 0),
                arrName + ".prl.start.ge0");
            builder->CreateCall(assumeFn, {startGE0});

            // Cache the pre-computed values for use inside the loop body.
            loopPtrModeDataPtrs_[arrName] = dataPtr;
            loopPtrModeLens_[arrName]     = lenLoad;
            // Also populate the loop-scope length cache so `canElideBoundsCheck`
            loopArrayLenCache_[basePtr] = lenLoad;
        }
    }

    loopStack.push_back({endBB, incBB});
    // Clear per-iteration array length cache so inner-body bounds checks
    // get fresh values.  Save outer cache for nested loop restore.
    auto savedLenCache = std::move(loopArrayLenCache_);
    loopArrayLenCache_.clear();
    // Re-seed the cache with the pointer-mode lengths we just computed so
    // they survive the clear above and remain visible during body generation.
    for (auto& kv : loopPtrModeLens_) {
        auto arrIt = namedValues.find(kv.first());
        if (arrIt == namedValues.end()) continue;
        // Look up the base ptr from the data ptr (data - 1) — but it's simpler
        (void)kv;  // entries were already populated in preamble before the save
    }

    // @independent: pre-create access group so all loads/stores in the
    llvm::MDNode* independentAccessGroup = nullptr;
    llvm::MDNode* savedAccessGroup = currentLoopAccessGroup_;
    if (stmt->loopHints.independent) {
        independentAccessGroup = llvm::MDNode::getDistinct(*context, {});
        currentLoopAccessGroup_ = independentAccessGroup;
        optStats_.independentLoops++;
    }

    generateStatement(stmt->body.get());

    // Restore the outer loop's access group (or null).
    if (stmt->loopHints.independent) {
        currentLoopAccessGroup_ = savedAccessGroup;
    }

    loopArrayLenCache_ = std::move(savedLenCache);
    loopStack.pop_back();

    // Clean up: iterator no longer has guaranteed bounds outside the loop.
    safeIndexVars_.erase(stmt->iteratorVar);
    loopIterEndBound_.erase(stmt->iteratorVar);
    loopIterStartBound_.erase(stmt->iteratorVar);
    loopIterEndArray_.erase(stmt->iteratorVar);

    // Restore pointer-mode state for the enclosing loop (or clear if none).
    if (doPtrMode) {
        loopPtrModeDataPtrs_ = std::move(savedPtrModeDataPtrs);
        loopPtrModeLens_     = std::move(savedPtrModeLens);
        loopPtrModeIterVar_  = std::move(savedPtrModeIterVar);
    }

    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(incBB);
    }

    // Increment block
    builder->SetInsertPoint(incBB);
    llvm::Value* nextVal = builder->CreateAlignedLoad(iterType, iterAlloca, llvm::MaybeAlign(8), stmt->iteratorVar.c_str());
    // !range on the increment block load: we reach incBB only after the
    if (stepKnownPositive && iterType->isIntegerTy(64)) {
        auto bit = allocaUpperBound_.find(iterAlloca);
        if (bit != allocaUpperBound_.end()) {
            llvm::MDBuilder mdB(*context);
            llvm::cast<llvm::LoadInst>(nextVal)->setMetadata(
                llvm::LLVMContext::MD_range,
                mdB.createRange(llvm::APInt(64, 0),
                                llvm::APInt(64, bit->second)));
        } else if (nonNegValues_.count(iterAlloca)) {
            llvm::cast<llvm::LoadInst>(nextVal)->setMetadata(
                llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        }
    }

    // OmScript advantage: for ascending loops starting from a non-negative
    llvm::Value* incVal;
    bool startNonNegForInc = false;
    if (stepKnownPositive) {
        if (auto* startCI = llvm::dyn_cast<llvm::ConstantInt>(startVal)) {
            startNonNegForInc = startCI->getSExtValue() >= 0;
        }
        // Also check nonNegValues_ for non-constant start values (e.g., function
        // args known to be non-negative from caller context).
        if (!startNonNegForInc && nonNegValues_.count(startVal) > 0) {
            startNonNegForInc = true;
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
    {
        // Loop metadata: mustprogress enables loop-idiom recognition (auto
        llvm::MDNode* mustProgress =
            llvm::MDNode::get(*context, {llvm::MDString::get(*context, "llvm.loop.mustprogress")});
        llvm::SmallVector<llvm::Metadata*, 6> loopMDs;
        loopMDs.push_back(nullptr); // self-reference placeholder (fixed below)
        loopMDs.push_back(mustProgress);
        // NOTE: interleave.count and vectorize.width are intentionally NOT
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
                    // Trip count multiple hint: when the trip count is a multiple
                    if (optimizationLevel >= OptimizationLevel::O3
                            && enableVectorize_ && tripCount > 8) {
                        // Find the largest power-of-2 that divides tripCount.
                        uint64_t multiple = 1;
                        for (uint64_t p = 64; p > 1; p >>= 1) {
                            if (tripCount % p == 0) { multiple = p; break; }
                        }
                        if (multiple >= 2) {
                            loopMDs.push_back(llvm::MDNode::get(
                                *context,
                                {llvm::MDString::get(*context, "llvm.loop.trip.count.multiple"),
                                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                                     llvm::Type::getInt32Ty(*context),
                                     static_cast<uint32_t>(multiple)))}));
                        }
                    }
                }
            }
        }
        // For variable-trip-count for-loops at O3, trust LLVM's cost-model-
        const bool deeplyNested = loopNestDepth_ >= 2;
        const bool vectorizePreferred = currentFuncHintVectorize_ && !bodyHasInnerLoop_;
        const bool suppressUnrollHint = deeplyNested || vectorizePreferred;
        // Comparison-context non-pow2 modulo loops (e.g. i%3==0): suppress
        const bool cmpModuloLoop = bodyHasNonPow2Modulo_ && !bodyHasNonPow2ModuloValue_;

        if (currentFuncHintNoUnroll_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.unroll.disable")}));
        } else if (bodyHasInnerLoop_) {
            // When the body contains an inner loop (while/for), disable
            if (!currentFuncHintUnroll_) {
                // @hot outer loop at O3 with inner loop: emit unroll.count=8 to
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
             const bool modArrayStore = bodyHasNonPow2ModuloArrayStore_
                 && optimizationLevel >= OptimizationLevel::O3;
             // Backward array refs (arr[i] += arr[i-1]): serial loop-carried dependency.
             const bool suppressUnrollForBackward = bodyHasBackwardArrayRef_
                 && optimizationLevel >= OptimizationLevel::O3;
             if (!modArrayStore && !suppressUnrollForBackward) {
                 static constexpr unsigned kOptMaxUnrollCount = 16;
                 static constexpr unsigned kOptMaxCmpModuloUnrollCount = 8;
                 static constexpr unsigned kDefaultUnrollCount = 4;
                 // For comparison-context non-pow2 modulo loops (e.g. i%3==0 branch),
                 const unsigned unrollCount =
                     inOptMaxFunction
                     ? (cmpModuloLoop ? kOptMaxCmpModuloUnrollCount : kOptMaxUnrollCount)
                     : (cmpModuloLoop ? kOptMaxCmpModuloUnrollCount : kDefaultUnrollCount);
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
        if (currentFuncHintNoVectorize_) {
            // @novectorize is the user's explicit request — always honour it.
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        } else if (currentFuncHintVectorize_
                   || (optimizationLevel >= OptimizationLevel::O3
                       && !bodyHasInnerLoop_ && stepKnownPositive)) {
            // Force-enable vectorization at O3 for hot loops.
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
            // vectorize.predicate.enable: allow predicated (masked) vectorization
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.predicate.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        // Interleaving: @hot+@vectorize at O3 → 4 iterations; plain O3
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

        // Apply per-loop @loop hints (override/augment function-level) for ForStmt
        const LoopConfig& loopHintsCfg = (stmt->loopHints.unrollCount > 0 || stmt->loopHints.vectorize || stmt->loopHints.noVectorize || stmt->loopHints.parallel || stmt->loopHints.independent || stmt->loopHints.fuse)
            ? stmt->loopHints
            : currentOptMaxConfig_.loop;
        if (loopHintsCfg.unrollCount > 0) {
            loopMDs.push_back(llvm::MDNode::get(
                *context,
                {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                     llvm::Type::getInt32Ty(*context), (unsigned)loopHintsCfg.unrollCount))}));
        }
        if (loopHintsCfg.vectorize) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 1))}));
        }
        if (loopHintsCfg.noVectorize) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
                           llvm::ConstantAsMetadata::get(
                               llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), 0))}));
        }
        if (loopHintsCfg.parallel) {
            llvm::MDNode* accessGroup2 = llvm::MDNode::getDistinct(*context, {});
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.parallel_accesses"),
                           accessGroup2}));
        }
        // @independent: emit parallel_accesses with the pre-created access group.
        // The access group was already attached to all loads/stores in the body.
        if (loopHintsCfg.independent && independentAccessGroup) {
            loopMDs.push_back(llvm::MDNode::get(
                *context, {llvm::MDString::get(*context, "llvm.loop.parallel_accesses"),
                           independentAccessGroup}));
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
    loopWrittenArrays_ = std::move(savedLoopWrittenArrays);
    loopBackwardReadArrays_ = std::move(savedLoopBackwardReadArrays);
}

void CodeGenerator::generateForEach(ForEachStmt* stmt) {
    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        codegenError("For-each loop outside of function", stmt);
    }

    // ── Range / range_step fast path ─────────────────────────────────────
    if (stmt->collection->type == ASTNodeType::CALL_EXPR &&
        optimizationLevel >= OptimizationLevel::O1) {
        auto* call = static_cast<CallExpr*>(stmt->collection.get());
        const bool isRange     = (call->callee == "range"      && call->arguments.size() == 2);
        const bool isRangeStep = (call->callee == "range_step" && call->arguments.size() == 3);
        if (isRange || isRangeStep) {
            const ScopeGuard rangeScope(*this);

            // Evaluate start, end (and step) exactly once to preserve
            // side-effect order with the unfused builtin path.
            llvm::Value* startV = generateExpression(call->arguments[0].get());
            llvm::Value* endV   = generateExpression(call->arguments[1].get());
            startV = toDefaultType(startV);
            endV   = toDefaultType(endV);

            llvm::Value* zeroC = llvm::ConstantInt::get(getDefaultType(), 0);
            llvm::Value* oneC  = llvm::ConstantInt::get(getDefaultType(), 1);

            llvm::Value* stepV = nullptr; // nullptr → unit step (no mul needed)
            llvm::Value* count = nullptr;
            if (isRange) {
                // count = max(end - start, 0).  Loop runs `count` times and
                // binds x to (start + i) for i = 0..count-1.
                llvm::Value* diff  = builder->CreateSub(endV, startV, "frng.diff");
                llvm::Value* isPos = builder->CreateICmpSGT(diff, zeroC, "frng.ispos");
                count = builder->CreateSelect(isPos, diff, zeroC, "frng.count");
            } else {
                // range_step: evaluate step once, then preserve the
                // runtime step==0 abort that the builtin would emit.
                stepV = generateExpression(call->arguments[2].get());
                stepV = toDefaultType(stepV);

                llvm::Value* stepIsZero = builder->CreateICmpEQ(stepV, zeroC, "frng.stepzero");
                llvm::BasicBlock* stepOkBB   = llvm::BasicBlock::Create(*context, "frng.stepok",   function);
                llvm::BasicBlock* stepFailBB = llvm::BasicBlock::Create(*context, "frng.stepfail", function);
                builder->CreateCondBr(stepIsZero, stepFailBB, stepOkBB);

                builder->SetInsertPoint(stepFailBB);
                {
                    std::string msg = call->line > 0
                        ? std::string("Runtime error: range step cannot be zero at line ") + std::to_string(call->line) + "\n"
                        : "Runtime error: range step cannot be zero\n";
                    builder->CreateCall(getPrintfFunction(),
                        {builder->CreateGlobalString(msg, "frng_zero_msg")});
                }
                builder->CreateCall(getOrDeclareAbort());
                builder->CreateUnreachable();

                builder->SetInsertPoint(stepOkBB);

                // count = max(0, (end - start + step - sign) / step).  This
                llvm::Value* diff    = builder->CreateSub(endV, startV, "frng.diff");
                llvm::Value* stepM1  = builder->CreateSub(stepV, oneC,  "frng.stepm1");
                llvm::Value* adjDiff = builder->CreateAdd(diff, stepM1, "frng.adjdiff");
                llvm::Value* rawCnt  = builder->CreateSDiv(adjDiff, stepV, "frng.rawcount");
                llvm::Value* isPos   = builder->CreateICmpSGT(rawCnt, zeroC, "frng.ispos");
                count = builder->CreateSelect(isPos, rawCnt, zeroC, "frng.count");
            }

            // Hidden index alloca + iterator alloca, mirroring the array path.
            llvm::AllocaInst* idxAllocaR = createEntryBlockAlloca(function, "_foreach_idx");
            builder->CreateStore(zeroC, idxAllocaR);
            nonNegValues_.insert(idxAllocaR);

            llvm::AllocaInst* iterAllocaR = createEntryBlockAlloca(function, stmt->iteratorVar);
            bindVariable(stmt->iteratorVar, iterAllocaR);

            llvm::BasicBlock* condBBR = llvm::BasicBlock::Create(*context, "frng.cond", function);
            llvm::BasicBlock* bodyBBR = llvm::BasicBlock::Create(*context, "frng.body", function);
            llvm::BasicBlock* incBBR  = llvm::BasicBlock::Create(*context, "frng.inc",  function);
            llvm::BasicBlock* endBBR  = llvm::BasicBlock::Create(*context, "frng.end",  function);

            builder->CreateBr(condBBR);

            // Condition: idx < count (unsigned: idx and count are both ≥ 0).
            builder->SetInsertPoint(condBBR);
            llvm::Value* curIdxR = builder->CreateAlignedLoad(
                getDefaultType(), idxAllocaR, llvm::MaybeAlign(8), "frng.idx");
            if (optimizationLevel >= OptimizationLevel::O1) {
                llvm::cast<llvm::LoadInst>(curIdxR)->setMetadata(
                    llvm::LLVMContext::MD_range, arrayLenRangeMD_);
            }
            llvm::Value* condR = builder->CreateICmpULT(curIdxR, count, "frng.cmp");
            auto* condBrR = builder->CreateCondBr(condR, bodyBBR, endBBR);
            if (optimizationLevel >= OptimizationLevel::O2) {
                llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(2000, 1);
                condBrR->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
            }

            // Body: bind x = start + idx [* step], then execute user body.
            builder->SetInsertPoint(bodyBBR);
            if (optimizationLevel >= OptimizationLevel::O2) {
                llvm::Value* nn = builder->CreateICmpSGE(
                    curIdxR, zeroC, "frng.nonneg");
                llvm::Function* assumeFn = OMSC_GET_INTRINSIC_STMT(
                    module.get(), llvm::Intrinsic::assume, {});
                builder->CreateCall(assumeFn, {nn});
            }
            // For range:      x = start + idx       (nsw safe; both fit in i64)
            llvm::Value* offset = stepV
                ? builder->CreateMul(curIdxR, stepV, "frng.off")
                : curIdxR;
            llvm::Value* iterVal = builder->CreateAdd(
                startV, offset, "frng.val",
                /*HasNUW=*/false, /*HasNSW=*/stepV == nullptr);
            builder->CreateStore(iterVal, iterAllocaR);

            loopStack.push_back({endBBR, incBBR});
            auto savedLenCacheR = std::move(loopArrayLenCache_);
            loopArrayLenCache_.clear();
            generateStatement(stmt->body.get());
            loopArrayLenCache_ = std::move(savedLenCacheR);
            loopStack.pop_back();
            if (!builder->GetInsertBlock()->getTerminator()) {
                builder->CreateBr(incBBR);
            }

            // Increment hidden idx and back-edge.
            builder->SetInsertPoint(incBBR);
            llvm::Value* nextIdxR = builder->CreateAdd(
                curIdxR, oneC, "frng.next", /*HasNUW=*/true, /*HasNSW=*/true);
            builder->CreateStore(nextIdxR, idxAllocaR);
            auto* backBrR = builder->CreateBr(condBBR);

            // Loop metadata: same hints as the array foreach path so the
            // vectorizer/unroller treat both consistently.
            if (optimizationLevel >= OptimizationLevel::O1) {
                llvm::MDNode* mustProgress = llvm::MDNode::get(
                    *context, {llvm::MDString::get(*context, "llvm.loop.mustprogress")});
                llvm::SmallVector<llvm::Metadata*, 4> loopMDs;
                loopMDs.push_back(nullptr);
                loopMDs.push_back(mustProgress);
                if (!inOptMaxFunction && optimizationLevel >= OptimizationLevel::O3 &&
                    enableUnrollLoops_) {
                    loopMDs.push_back(llvm::MDNode::get(
                        *context,
                        {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                         llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                             llvm::Type::getInt32Ty(*context), 4))}));
                }
                llvm::MDNode* loopMD = llvm::MDNode::get(*context, loopMDs);
                loopMD->replaceOperandWith(0, loopMD);
                backBrR->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
            }

            builder->SetInsertPoint(endBBR);
            optStats_.foreachRangeFused++;
            return;
        }
    }

    const ScopeGuard scope(*this);

    // Evaluate the collection (array or string)
    llvm::Value* collVal = generateExpression(stmt->collection.get());

    // Detect whether the collection is a string.  Strings are raw char
    const bool isStr = isStringExpr(stmt->collection.get());
    // Detect whether this is an array whose elements are string pointers.
    const bool isStrArray = !isStr && isStringArrayExpr(stmt->collection.get());

    // Convert to pointer for element access.  When the collection value is
    auto* ptrTy = llvm::PointerType::getUnqual(*context);
    llvm::Value* basePtr;
    if (collVal->getType()->isPointerTy()) {
        basePtr = collVal;
    } else {
        collVal = toDefaultType(collVal);
        basePtr = builder->CreateIntToPtr(collVal, ptrTy, "foreach.baseptr");
    }

    // Get the collection length
    llvm::Value* lenVal;
    if (isStr) {
        // String: strlen (no length header)
        auto* strlenCall = builder->CreateCall(getOrDeclareStrlen(), {basePtr}, "foreach.strlen");
        // !range [0, INT64_MAX): strlen always returns a non-negative value.
        strlenCall->setMetadata(llvm::LLVMContext::MD_range, arrayLenRangeMD_);
        lenVal = strlenCall;
    } else {
        // Array: length stored in slot 0
                llvm::Value* lenLoad = emitLoadArrayLen(basePtr, "foreach.len");
        lenVal = lenLoad;
    }

    // Allocate hidden index variable and the user's iterator variable
    llvm::AllocaInst* idxAlloca = createEntryBlockAlloca(function, "_foreach_idx");
    builder->CreateStore(llvm::ConstantInt::get(getDefaultType(), 0), idxAlloca);
    // The foreach hidden index starts at 0 and only increments, so it is
    nonNegValues_.insert(idxAlloca);

    llvm::AllocaInst* iterAlloca = createEntryBlockAlloca(function, stmt->iteratorVar);
    bindVariable(stmt->iteratorVar, iterAlloca);
    // If the collection is a string array, mark the iterator as a string
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
    llvm::Value* curIdx = builder->CreateAlignedLoad(getDefaultType(), idxAlloca, llvm::MaybeAlign(8), "foreach.idx");
    // !range [0, INT64_MAX): the foreach index is always non-negative (starts
    if (optimizationLevel >= OptimizationLevel::O1) {
        llvm::cast<llvm::LoadInst>(curIdx)->setMetadata(
            llvm::LLVMContext::MD_range, arrayLenRangeMD_);
    }

    llvm::Value* cond = builder->CreateICmpULT(curIdx, lenVal, "foreach.cmp");
    auto* foreachCondBr = builder->CreateCondBr(cond, bodyBB, endBB);
    // Hint the back-edge (body) as likely-taken.
    if (optimizationLevel >= OptimizationLevel::O2) {
        llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(2000, 1);
        foreachCondBr->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
    }

    // Body: load current element into iterator variable, then execute body
    builder->SetInsertPoint(bodyBB);
    // Reuse curIdx from condBB — condBB dominates bodyBB and the hidden index
    llvm::Value* bodyIdx = curIdx;

    // Emit @llvm.assume(idx >= 0) so LLVM's CorrelatedValuePropagation and
    if (optimizationLevel >= OptimizationLevel::O2) {
        llvm::Value* isNonNeg = builder->CreateICmpSGE(
            bodyIdx, llvm::ConstantInt::get(getDefaultType(), 0), "foreach.nonneg");
        llvm::Function* assumeFn = OMSC_GET_INTRINSIC_STMT(
            module.get(), llvm::Intrinsic::assume, {});
        builder->CreateCall(assumeFn, {isNonNeg});
    }

    llvm::Value* elemVal;
    if (isStr) {
        // String: load single byte at offset bodyIdx, zero-extend to i64
        llvm::Value* charPtr = builder->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context), basePtr, bodyIdx, "foreach.charptr");
        auto* charByte = builder->CreateLoad(llvm::Type::getInt8Ty(*context), charPtr, "foreach.char");
        // TBAA: string character loads are in the string-data type set,
        charByte->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaStringData_);
        // OmScript strings are always initialized (C NUL-terminated), so
        // character loads within bounds are always defined.
        if (optimizationLevel >= OptimizationLevel::O1)
            charByte->setMetadata(llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
        elemVal = builder->CreateZExt(charByte, getDefaultType(), "foreach.charext");
    } else {
        // Array: element is at slot (bodyIdx + 1).
        llvm::Value* offset =
            builder->CreateAdd(bodyIdx, llvm::ConstantInt::get(getDefaultType(), 1), "foreach.offset",
                               /*HasNUW=*/true, /*HasNSW=*/true);
        llvm::Value* elemPtr = builder->CreateInBoundsGEP(getDefaultType(), basePtr, offset, "foreach.elem.ptr");
        elemVal = builder->CreateAlignedLoad(getDefaultType(), elemPtr, llvm::MaybeAlign(8), "foreach.elem");
        // TBAA: foreach element loads (slots 1+) never alias the array length
        if (auto* elemLoad = llvm::dyn_cast<llvm::LoadInst>(elemVal)) {
            elemLoad->setMetadata(llvm::LLVMContext::MD_tbaa, tbaaArrayElem_);
            // OmScript arrays are always initialized, so element loads are !noundef.
            if (optimizationLevel >= OptimizationLevel::O1)
                elemLoad->setMetadata(llvm::LLVMContext::MD_noundef, llvm::MDNode::get(*context, {}));
        }
    }
    builder->CreateStore(elemVal, iterAlloca);

    loopStack.push_back({endBB, incBB});
    auto savedLenCacheFE = std::move(loopArrayLenCache_);
    loopArrayLenCache_.clear();
    generateStatement(stmt->body.get());
    loopArrayLenCache_ = std::move(savedLenCacheFE);
    loopStack.pop_back();
    if (!builder->GetInsertBlock()->getTerminator()) {
        builder->CreateBr(incBB);
    }

    // Increment hidden index.
    builder->SetInsertPoint(incBB);
    // Reuse curIdx from condBB — the hidden index alloca is only modified in
    llvm::Value* incIdx = builder->CreateAdd(curIdx, llvm::ConstantInt::get(getDefaultType(), 1), "foreach.next",
                                             /*HasNUW=*/true, /*HasNSW=*/true);
    builder->CreateStore(incIdx, idxAlloca);
    auto* backBr = builder->CreateBr(condBB);

    // Attach loop metadata to the back-edge branch so LLVM's loop optimizer,
    if (optimizationLevel >= OptimizationLevel::O1) {
        llvm::MDNode* mustProgress =
            llvm::MDNode::get(*context, {llvm::MDString::get(*context, "llvm.loop.mustprogress")});
        llvm::SmallVector<llvm::Metadata*, 6> loopMDs;
        loopMDs.push_back(nullptr); // self-reference placeholder
        loopMDs.push_back(mustProgress);
        // At O3 with unrolling enabled, hint the unroller with a moderate
        if (!inOptMaxFunction && optimizationLevel >= OptimizationLevel::O3 && enableUnrollLoops_) {
            loopMDs.push_back(llvm::MDNode::get(
                *context,
                {llvm::MDString::get(*context, "llvm.loop.unroll.count"),
                 llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                     llvm::Type::getInt32Ty(*context), 2))}));
        }
        // @vectorize / @novectorize: per-function loop vectorization hints.
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
        if ((stmt->loopHints.parallel)
            || (optimizationLevel >= OptimizationLevel::O3
                && enableParallelize_ && !currentFuncHintNoParallelize_
                && loopNestDepth_ == 0
                && (currentFuncHintParallelize_ || currentFuncHintHot_
                    || optimizationLevel >= OptimizationLevel::O3))) {
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
    deferStack.emplace_back(); // Push new defer scope

    for (auto& statement : stmt->statements) {
        if (builder->GetInsertBlock()->getTerminator()) {
            // After a terminator (e.g. throw/return), skip unreachable code,
            if (statement->type != ASTNodeType::CATCH_STMT)
                continue;
        }
        if (statement->type == ASTNodeType::DEFER_STMT) {
            // Collect deferred statements for later execution
            auto* deferStmt = static_cast<DeferStmt*>(statement.get());
            deferStack.back().push_back(deferStmt->body.get());
        } else {
            generateStatement(statement.get());
        }
    }

    // Execute deferred statements in LIFO order (reverse of defer order)
    auto& defers = deferStack.back();
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
        if (builder->GetInsertBlock()->getTerminator()) {
            break; // Don't generate after a terminator
        }
        generateStatement(*it);
    }
    deferStack.pop_back(); // Pop defer scope
}

void CodeGenerator::generateExprStmt(ExprStmt* stmt) {
    generateExpression(stmt->expression.get());
}

void CodeGenerator::generateSwitch(SwitchStmt* stmt) {
    llvm::Value* condVal = generateExpression(stmt->condition.get());
    condVal = toDefaultType(condVal);

    llvm::Function* function = builder->GetInsertBlock()->getParent();

    // Constant switch condition elimination: when the condition is a known
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

    // ── Small switch → if-else chain optimization (O2+) ───────────────
    if (optimizationLevel >= OptimizationLevel::O2) {
        // Count total case values (including multi-value cases).
        unsigned totalCaseValues = 0;
        unsigned nonDefaultCases = 0;
        for (auto& sc : stmt->cases) {
            if (!sc.isDefault) {
                ++nonDefaultCases;
                totalCaseValues += 1 + static_cast<unsigned>(sc.values.size());
            }
        }

        if (nonDefaultCases > 0 && totalCaseValues <= 2) {
            // Validate all case values (float check, constant check, duplicate check)
            // before emitting any IR, matching the regular switch path's semantics.
            std::set<int64_t> seenSmall;
            for (auto& sc : stmt->cases) {
                if (sc.isDefault) continue;
                auto checkVal = [&](Expression* expr) {
                    llvm::Value* v = generateExpression(expr);
                    if (v->getType()->isDoubleTy())
                        codegenError("case value must be an integer constant, not a float", expr);
                    v = toDefaultType(v);
                    auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v);
                    if (!ci)
                        codegenError("case value must be a compile-time integer constant", expr);
                    int64_t cv = ci->getSExtValue();
                    if (!seenSmall.insert(cv).second)
                        codegenError("duplicate case value " + std::to_string(cv) + " in switch statement", expr);
                };
                checkVal(sc.value.get());
                for (auto& ev : sc.values) checkVal(ev.get());
            }

            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "switch.end", function);
            loopStack.push_back({mergeBB, nullptr});

            // Find the default case.
            SwitchCase* defaultCase = nullptr;
            for (auto& sc : stmt->cases) {
                if (sc.isDefault) { defaultCase = &sc; break; }
            }

            // Chain: for each non-default case, emit cmp + condbr.
            // Last false-branch falls through to default (or merge if no default).
            llvm::BasicBlock* falseBB = nullptr;
            for (auto& sc : stmt->cases) {
                if (sc.isDefault) continue;

                // Collect all case values for this arm (already validated above).
                std::vector<llvm::ConstantInt*> caseConstants;
                auto addVal = [&](Expression* expr) {
                    llvm::Value* v = generateExpression(expr);
                    v = toDefaultType(v);
                    auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v);
                    caseConstants.push_back(ci);
                };
                addVal(sc.value.get());
                for (auto& ev : sc.values) addVal(ev.get());

                // Build the comparison: condVal == c1 || condVal == c2 || ...
                llvm::Value* cmp = builder->CreateICmpEQ(condVal, caseConstants[0], "switch.cmp");
                for (unsigned i = 1; i < caseConstants.size(); ++i) {
                    llvm::Value* cmp2 = builder->CreateICmpEQ(condVal, caseConstants[i], "switch.cmp");
                    cmp = builder->CreateOr(cmp, cmp2, "switch.or");
                }

                llvm::BasicBlock* caseBB = llvm::BasicBlock::Create(*context, "switch.case", function, mergeBB);
                falseBB = llvm::BasicBlock::Create(*context, "switch.next", function, mergeBB);

                auto* br = builder->CreateCondBr(cmp, caseBB, falseBB);
                // Uniform weights: each explicit case is equally likely to match.
                llvm::MDNode* brW = llvm::MDBuilder(*context).createBranchWeights(1, 1);
                br->setMetadata(llvm::LLVMContext::MD_prof, brW);

                // Generate case body.
                builder->SetInsertPoint(caseBB);
                {
                    const ScopeGuard scope(*this);
                    for (auto& s : sc.body) {
                        generateStatement(s.get());
                        if (builder->GetInsertBlock()->getTerminator()) break;
                    }
                }
                if (!builder->GetInsertBlock()->getTerminator())
                    builder->CreateBr(mergeBB);

                builder->SetInsertPoint(falseBB);
            }

            // Generate default (or fall through to merge).
            if (defaultCase) {
                const ScopeGuard scope(*this);
                for (auto& s : defaultCase->body) {
                    generateStatement(s.get());
                    if (builder->GetInsertBlock()->getTerminator()) break;
                }
            }
            if (!builder->GetInsertBlock()->getTerminator())
                builder->CreateBr(mergeBB);

            loopStack.pop_back();
            builder->SetInsertPoint(mergeBB);
            return;
        }
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
    if (optimizationLevel >= OptimizationLevel::O2) {
        const unsigned numCases = switchInst->getNumCases();
        const bool hasDefault = (defaultBB != mergeBB);
        const unsigned totalSuccessors = numCases + (hasDefault ? 1 : 0);
        if (totalSuccessors > 0) {
            llvm::SmallVector<uint32_t, 16> weights;
            // Default case: weight it as cold when there are many explicit cases
            uint32_t defaultWeight = 0;
            if (hasDefault) {
                defaultWeight = (numCases >= 4) ? 1u : numCases;
            }
            weights.push_back(defaultWeight);
            // Each explicit case: equal weight of numCases so the total
            // probability mass for all explicit cases >> default.
            const uint32_t caseWeight = numCases;
            for (unsigned i = 0; i < numCases; ++i) {
                weights.push_back(caseWeight);
            }
            llvm::MDNode* brWeights = llvm::MDBuilder(*context).createBranchWeights(weights);
            switchInst->setMetadata(llvm::LLVMContext::MD_prof, brWeights);
        }
    }

    builder->SetInsertPoint(mergeBB);
}

// ---------------------------------------------------------------------------

int64_t CodeGenerator::getCatchStringId(const std::string& s) {
    auto it = catchStringIds_.find(s);
    if (it != catchStringIds_.end())
        return it->second;
    int64_t id = nextCatchStringId_++;
    catchStringIds_[s] = id;
    return id;
}

void CodeGenerator::buildCatchTable(
        const std::vector<std::unique_ptr<Statement>>& stmts,
        llvm::Function* fn) {
    // Walk the top-level statements of a function body and register every
    for (const auto& s : stmts) {
        if (s->type != ASTNodeType::CATCH_STMT) continue;
        auto* cs = static_cast<CatchStmt*>(s.get());
        int64_t key = cs->isString ? getCatchStringId(cs->strCode) : cs->intCode;
        if (catchTable_.count(key)) {
            codegenError("Duplicate catch(" +
                (cs->isString ? ("\"" + cs->strCode + "\"") : std::to_string(cs->intCode)) +
                ") block in the same function", cs);
        }
        llvm::BasicBlock* bb = llvm::BasicBlock::Create(
            *context,
            cs->isString ? ("catch.str." + cs->strCode) : ("catch." + std::to_string(key)),
            fn);
        catchTable_[key] = bb;
    }
    // Create the default (unmatched throw) block once per function.
    if (!catchTable_.empty()) {
        catchDefaultBB_ = llvm::BasicBlock::Create(*context, "catch.unmatched", fn);
    }
}

/// Emit the handler body for a catch(code) block.
void CodeGenerator::generateCatch(CatchStmt* stmt) {
    int64_t key = stmt->isString ? getCatchStringId(stmt->strCode) : stmt->intCode;
    auto it = catchTable_.find(key);
    if (it == catchTable_.end()) {
        // Should not happen if buildCatchTable ran correctly.
        codegenError("Internal error: catch block not in table", stmt);
    }
    llvm::BasicBlock* handlerBB = it->second;
    llvm::Function* fn = builder->GetInsertBlock()->getParent();

    // If the previous block doesn't have a terminator, it's the normal
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(*context, "catch.merge", fn);

    if (!builder->GetInsertBlock()->getTerminator())
        builder->CreateBr(mergeBB);

    // Switch to the handler BasicBlock and generate the body.
    builder->SetInsertPoint(handlerBB);
    {
        const ScopeGuard scope(*this);
        for (auto& s : stmt->body->statements) {
            if (builder->GetInsertBlock()->getTerminator()) break;
            generateStatement(s.get());
        }
    }
    if (!builder->GetInsertBlock()->getTerminator())
        builder->CreateBr(mergeBB);

    // Emit the unmatched-throw abort here if this is the last catch in the
    if (catchDefaultBB_ && !catchDefaultBB_->getTerminator()) {
        // Check if all catch entries point to a non-null, terminated BB; if not
        bool allTerminated = true;
        for (auto& [k, bb] : catchTable_) {
            if (!bb->getTerminator()) { allTerminated = false; break; }
        }
        if (allTerminated) {
            builder->SetInsertPoint(catchDefaultBB_);
            // Unmatched throw: print a message and abort.
            std::string errText = "Runtime error: unmatched throw\n";
            builder->CreateCall(getPrintfFunction(),
                {builder->CreateGlobalString(errText, "catch_unmatched_msg")});
            builder->CreateCall(getOrDeclareAbort());
            builder->CreateUnreachable();
        }
    }

    // Continue at the merge point.
    builder->SetInsertPoint(mergeBB);
}

/// throw expr; — compile to a switch over the catch table.
void CodeGenerator::generateThrow(ThrowStmt* stmt) {
    llvm::Value* val = generateExpression(stmt->value.get());
    val = toDefaultType(val);

    if (catchTable_.empty()) {
        // No catch blocks in this function — abort with a clear error.
        std::string errText = stmt->line > 0
            ? std::string("Runtime error: unhandled throw at line ") + std::to_string(stmt->line) + "\n"
            : "Runtime error: unhandled throw\n";
        builder->CreateCall(getPrintfFunction(),
            {builder->CreateGlobalString(errText, "throw_unhandled_msg")});
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();
        return;
    }

    // Emit a switch: switch(val) { case key: br handlerBB, ... default: br catchDefaultBB_ }
    llvm::BasicBlock* defaultBB = catchDefaultBB_;
    if (!defaultBB) {
        // Safety: create an inline abort block if catchDefaultBB_ is null.
        defaultBB = llvm::BasicBlock::Create(*context, "throw.nodefault",
                                             builder->GetInsertBlock()->getParent());
        builder->SetInsertPoint(defaultBB);
        builder->CreateCall(getOrDeclareAbort());
        builder->CreateUnreachable();
        builder->SetInsertPoint(builder->GetInsertBlock());
    }

    // Re-point to the block before the switch instruction.
    llvm::BasicBlock* curBB = builder->GetInsertBlock();
    builder->SetInsertPoint(curBB);

    llvm::SwitchInst* sw = builder->CreateSwitch(val, defaultBB,
                                                  static_cast<unsigned>(catchTable_.size()));
    for (auto& [key, handlerBB] : catchTable_) {
        auto* caseVal = llvm::ConstantInt::getSigned(
            llvm::cast<llvm::IntegerType>(getDefaultType()), key);
        sw->addCase(caseVal, handlerBB);
    }
}

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
    const std::string& name = stmt->varName;

    // ── Heap-free heap-allocated variables ────────────────────────────────
    const bool isHeapString = stringVars_.count(name) > 0;
    const bool isHeapArray  = arrayVars_.count(name) > 0 &&
                               !stackAllocatedArrays_.count(name);
    const bool isHeapDict   = dictVarNames_.count(name) > 0;
    // ptr variables whose value was heap-allocated (malloc / heap alloc<T>).
    const bool isHeapPtr    = heapPtrVarNames_.count(name) > 0;
    // struct variables are heap-allocated opaque pointers.
    const bool isHeapStruct = structVars_.count(name) > 0;
    // bigint variables are heap-allocated arbitrary-precision integers.
    bool isHeapBigint = false;
    {
        auto tit = varTypeAnnotations_.find(name);
        isHeapBigint = (tit != varTypeAnnotations_.end() && tit->second == "bigint");
    }

    if (isHeapString || isHeapArray || isHeapDict || isHeapPtr ||
        isHeapStruct || isHeapBigint) {
        // Load the heap pointer from the alloca (i64 stored as int, ptr cast needed).
        auto* allocaInst2 = llvm::dyn_cast<llvm::AllocaInst>(alloca);
        if (allocaInst2) {
            auto* ptrTy = llvm::PointerType::getUnqual(*context);
            llvm::Value* heapPtr = builder->CreateLoad(
                allocaInst2->getAllocatedType(), allocaInst2, name + ".ptr");
            // Cast i64 → ptr if necessary (OmScript stores heap pointers as i64)
            if (!heapPtr->getType()->isPointerTy()) {
                heapPtr = builder->CreateIntToPtr(heapPtr, ptrTy, name + ".heapptr");
            }
            // Emit free().  The compiler already knows free() is
            // InaccessibleOrArgMemOnly so this is safe to CSE/hoist.
            builder->CreateCall(getOrDeclareFree(), {heapPtr});
        }
    }

    // For stack-allocated alloc<T> pointers: end the lifetime of the backing
    // alloca so LLVM knows the stack slot is dead and can reuse it.
    {
        auto backingIt = stackPtrBackingAlloca_.find(name);
        if (backingIt != stackPtrBackingAlloca_.end()) {
            auto* backingAlloca = backingIt->second;
            if (backingAlloca) {
                const uint64_t backingSz =
                    module->getDataLayout().getTypeAllocSize(
                        backingAlloca->getAllocatedType());
                auto* backingSzVal = llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(*context), backingSz);
                auto* lifetimeEnd = OMSC_GET_INTRINSIC_STMT(
                    module.get(), llvm::Intrinsic::lifetime_end,
                    {llvm::PointerType::getUnqual(*context)});
                builder->CreateCall(lifetimeEnd, {backingSzVal, backingAlloca});
            }
            stackPtrBackingAlloca_.erase(backingIt);
        }
    }

    // Emit llvm.lifetime.end to mark the alloca slot as dead.
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

    // ── Tracking-set cleanup ───────────────────────────────────────────────
    constIntFolds_.erase(name);
    constFloatFolds_.erase(name);
    constStringFolds_.erase(name);
    varTypeAnnotations_.erase(name);
    stringVars_.erase(name);
    arrayVars_.erase(name);
    dictVarNames_.erase(name);
    ptrVarNames_.erase(name);
    ptrElemTypes_.erase(name);
    heapPtrVarNames_.erase(name);
    structVars_.erase(name);
    simdVars_.erase(name);
    registerVars_.erase(name);
    stackAllocatedArrays_.erase(name);
    readOnlyGlobalArrays_.erase(name);
    frozenVars_.erase(name);

    // Mark the variable as dead for use-after-invalidate detection.
    deadVars_.insert(name);
    deadVarReason_[name] = "invalidated";
    getBorrowState(name).invalidated = true;
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
    bindVariableAnnotated(stmt->name, alloca, stmt->typeName);

    if (initValue) {
        builder->CreateStore(initValue, alloca);

        // If the source is an identifier, mark the source as dead (emit
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

llvm::Value* CodeGenerator::generateRangeAnnot(RangeAnnotExpr* expr) {
    // ── Compile-time bounds check ────────────────────────────────────────
    if (auto cv = tryFoldInt(expr->inner.get())) {
        const int64_t v = *cv;
        if (v < expr->lo || v > expr->hi) {
            codegenError(
                "@range[" + std::to_string(expr->lo) + ", " + std::to_string(expr->hi) +
                "] violated: inner expression folds to " + std::to_string(v) +
                " which is outside the declared range", expr);
        }
    }

    // ── Generate the inner value ─────────────────────────────────────────
    llvm::Value* val = generateExpression(expr->inner.get());
    if (!val) return val;

    // Range hints only apply to integer values.  Non-integer results
    if (!val->getType()->isIntegerTy()) return val;

    // i1 booleans don't have a meaningful integer range; skip the hint.
    if (val->getType()->isIntegerTy(1)) return val;

    llvm::IntegerType* intTy = llvm::cast<llvm::IntegerType>(val->getType());
    const unsigned bits = intTy->getBitWidth();

    // Sanity-clamp: if the declared range exceeds what the underlying
    int64_t lo = expr->lo;
    int64_t hi = expr->hi;
    if (bits < 64) {
        const int64_t typeMin = -(int64_t{1} << (bits - 1));
        const int64_t typeMax =  (int64_t{1} << (bits - 1)) - 1;
        if (lo < typeMin) lo = typeMin;
        if (hi > typeMax) hi = typeMax;
        if (lo > hi) return val; // nothing to assert
    }

    llvm::Constant* loC = llvm::ConstantInt::get(intTy, lo, /*IsSigned=*/true);
    llvm::Constant* hiC = llvm::ConstantInt::get(intTy, hi, /*IsSigned=*/true);

    // ── !range metadata on load/call results ─────────────────────────────
    if (auto* inst = llvm::dyn_cast<llvm::Instruction>(val)) {
        bool canAttach = llvm::isa<llvm::LoadInst>(inst) ||
                         llvm::isa<llvm::CallInst>(inst) ||
                         llvm::isa<llvm::InvokeInst>(inst);
        if (canAttach && hi < std::numeric_limits<int64_t>::max()) {
            // Don't overwrite a tighter pre-existing !range; intersect
            if (!inst->getMetadata(llvm::LLVMContext::MD_range)) {
                llvm::Metadata* mdOps[] = {
                    llvm::ConstantAsMetadata::get(loC),
                    llvm::ConstantAsMetadata::get(
                        llvm::ConstantInt::get(intTy, hi + 1, /*IsSigned=*/true))};
                inst->setMetadata(llvm::LLVMContext::MD_range,
                                  llvm::MDNode::get(*context, mdOps));
            }
        }
    }

    // ── llvm.assume(val >= lo && val <= hi) ─────────────────────────────
    llvm::Function* assumeFn = OMSC_GET_INTRINSIC_STMT(
        module.get(), llvm::Intrinsic::assume, {});
    llvm::Value* geLo = builder->CreateICmpSGE(val, loC, "rangeannot.gelo");
    builder->CreateCall(assumeFn, {geLo});
    llvm::Value* leHi = builder->CreateICmpSLE(val, hiC, "rangeannot.lehi");
    builder->CreateCall(assumeFn, {leHi});

    // ── Non-negativity bookkeeping ──────────────────────────────────────
    if (lo >= 0) {
        nonNegValues_.insert(val);
    }

    return val;
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
    // Generate the source value first.
    llvm::Value* val = generateExpression(expr->source.get());

    // The borrow variable name isn't known here (it's set by the surrounding
    Expression* srcForIdent = expr->source.get();
    if (auto* ue = dynamic_cast<UnaryExpr*>(srcForIdent); ue && ue->op == "&" && ue->operand)
        srcForIdent = ue->operand.get();
    if (auto* srcIdent = dynamic_cast<IdentifierExpr*>(srcForIdent)) {
        const std::string& srcName = srcIdent->name;

        if (expr->isMut) {
            // ---------------------------------------------------------------
            auto* bs = getBorrowStateOpt(srcName);
            if (bs) {
                if (bs->immutBorrowCount > 0) {
                    codegenError("Cannot create mutable borrow of '" + srcName +
                                 "' — it already has " +
                                 std::to_string(bs->immutBorrowCount) +
                                 " active immutable borrow(s)", expr);
                }
                if (bs->mutBorrowed) {
                    codegenError("Cannot create mutable borrow of '" + srcName +
                                 "' — it already has an active mutable borrow", expr);
                }
                if (bs->frozen) {
                    codegenError("Cannot create mutable borrow of frozen variable '" + srcName + "'", expr);
                }
                if (bs->moved || bs->invalidated) {
                    codegenError("Cannot borrow dead variable '" + srcName + "'", expr);
                }
            }
            // Lock source.  Ref name not known yet; use a sentinel — the
            expr->pendingSrcVar = srcName; // stash for VarDecl to consume
        } else {
            // ---------------------------------------------------------------
            auto* bs = getBorrowStateOpt(srcName);
            if (bs) {
                if (bs->mutBorrowed) {
                    codegenError("Cannot create immutable borrow of '" + srcName +
                                 "' — it already has an active mutable borrow", expr);
                }
                if (bs->moved || bs->invalidated) {
                    codegenError("Cannot borrow dead variable '" + srcName + "'", expr);
                }
            }
            expr->pendingSrcVar = srcName;
        }
    }

    // -----------------------------------------------------------------------
    if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(val)) {
        const std::string suffix = expr->isMut ? "mut" : "ref";
        std::string scopeName = "omscript.borrow." + suffix;
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
        loadInst->setMetadata(llvm::LLVMContext::MD_alias_scope, scopeList);
        if (!expr->isMut) {
            // Immutable borrow: the load does NOT alias any other mutable access.
            loadInst->setMetadata(llvm::LLVMContext::MD_noalias, scopeList);
            // !invariant.load: value will not change while the borrow is alive.
            loadInst->setMetadata(llvm::LLVMContext::MD_invariant_load,
                                  llvm::MDNode::get(*context, {}));
        }
    }

    return val;
}

llvm::Value* CodeGenerator::generateReborrowExpr(ReborrowExpr* expr) {
    // Find source variable name from the source expression
    std::string srcName;
    if (auto* srcId = dynamic_cast<IdentifierExpr*>(expr->source.get())) {
        srcName = srcId->name;
    }

    if (!srcName.empty()) {
        // Check borrow validity — source must be readable/writable
        auto* bs = getBorrowStateOpt(srcName);
        if (bs) {
            if (bs->moved || bs->invalidated) {
                codegenError("Cannot reborrow dead variable '" + srcName + "'", expr);
            }
            if (expr->isMut && bs->mutBorrowed) {
                codegenError("Cannot create mutable reborrow of '" + srcName +
                             "' — it already has an active mutable borrow", expr);
            }
            if (expr->isMut && bs->immutBorrowCount > 0) {
                codegenError("Cannot create mutable reborrow of '" + srcName +
                             "' — it already has active immutable borrows", expr);
            }
            if (expr->isMut && bs->frozen) {
                codegenError("Cannot create mutable reborrow of frozen variable '" + srcName + "'", expr);
            }
        }

        // Resolve the original source (chase borrow chain once)
        std::string realSrc = srcName;
        auto bmit = borrowMap_.find(srcName);
        if (bmit != borrowMap_.end()) {
            realSrc = bmit->second.srcVar;
        }
        expr->pendingSrcVar = realSrc;
    }

    // Generate the value — for partial borrows compute a GEP pointer
    llvm::Value* val = generateExpression(expr->source.get());

    // Partial borrow: field access → use GEP on struct
    if (!expr->fieldName.empty()) {
        if (auto* srcId = dynamic_cast<IdentifierExpr*>(expr->source.get())) {
            auto stit = structVars_.find(srcId->name);
            if (stit != structVars_.end()) {
                const std::string& stType = stit->second;
                auto fit = structDefs_.find(stType);
                if (fit != structDefs_.end()) {
                    const auto& fields = fit->second;
                    for (size_t fi = 0; fi < fields.size(); ++fi) {
                        if (fields[fi] == expr->fieldName) {
                            auto it2 = namedValues.find(srcId->name);
                            if (it2 != namedValues.end()) {
                                auto* srcAlloca = llvm::dyn_cast<llvm::AllocaInst>(it2->second);
                                if (srcAlloca) {
                                    llvm::StructType* sty = llvm::dyn_cast<llvm::StructType>(
                                        srcAlloca->getAllocatedType());
                                    if (sty) {
                                        val = builder->CreateStructGEP(sty, srcAlloca,
                                                                       static_cast<unsigned>(fi),
                                                                       "reborrow.field");
                                    }
                                }
                            }
                            break;
                        }
                    }
                }
            }
        }
    } else if (expr->indexExpr) {
        // Partial borrow: array element → GEP to element slot
        llvm::Value* arrPtr = generateExpression(expr->source.get());
        llvm::Value* idx = generateExpression(expr->indexExpr.get());
        // Arrays: [len, e0, e1, ...]  — element i is at slot i+1
        llvm::Value* elemIdx = builder->CreateAdd(
            idx, llvm::ConstantInt::get(getDefaultType(), 1), "reborrow.idx");
        val = builder->CreateGEP(getDefaultType(), arrPtr, elemIdx, "reborrow.elem");
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
            bool isLargeType = elemTy->isArrayTy() || elemTy->isStructTy();
            if (auto* arraySize = allocaInst->getArraySize()) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(arraySize)) {
                    if (ci->getZExtValue() > 1) isLargeType = true;
                }
            }

            if (isLargeType) {
                // Memory-resident prefetch: for large types (structs, arrays)
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

                // Offset prefetch for large types: if the user specified
                if (stmt->offsetBytes > 0) {
                    llvm::Value* aheadPtr = builder->CreateInBoundsGEP(
                        builder->getInt8Ty(), alloca,
                        llvm::ConstantInt::get(getDefaultType(), stmt->offsetBytes),
                        varName + ".pf.ahead");
                    auto* pfAhead = builder->CreateCall(prefetchFn, {
                        aheadPtr,
                        builder->getInt32(0),
                        builder->getInt32(locality),
                        builder->getInt32(1)
                    });
                    pfAhead->setMetadata("omscript.memory_prefetch",
                        llvm::MDNode::get(*context, {}));
                }

                // For multi-slot arrays (structs), also prefetch subsequent
                if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(elemTy)) {
                    const uint64_t numSlots = arrTy->getNumElements();
                    const uint64_t slotSize = module->getDataLayout().getTypeAllocSize(
                        arrTy->getElementType());
                    const uint64_t totalBytes = numSlots * slotSize;
                    // Prefetch every 64 bytes (one cache line) beyond the first.
                    for (uint64_t offset = 64; offset < totalBytes; offset += 64) {
                        llvm::Value* gepIdx = llvm::ConstantInt::get(
                            getDefaultType(), offset / slotSize);
                        llvm::Value* nextPtr = builder->CreateInBoundsGEP(
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

                    // Offset prefetch: prefetch+N brings the cache line at
                    if (stmt->offsetBytes > 0) {
                        llvm::Value* offsetPtr = builder->CreateInBoundsGEP(
                            builder->getInt8Ty(), ptr,
                            llvm::ConstantInt::get(getDefaultType(), stmt->offsetBytes),
                            varName + ".pf.ahead");
                        auto* pfAhead = builder->CreateCall(prefetchFn, {
                            offsetPtr,
                            builder->getInt32(0),
                            builder->getInt32(locality),
                            builder->getInt32(1)
                        });
                        pfAhead->setMetadata("omscript.memory_prefetch",
                            llvm::MDNode::get(*context, {}));
                    }
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
    getBorrowState(varName).moved = true;
}

VarBorrowState& CodeGenerator::getBorrowState(const std::string& varName) {
    return varBorrowStates_[varName];
}

const VarBorrowState* CodeGenerator::getBorrowStateOpt(const std::string& varName) const {
    auto it = varBorrowStates_.find(varName);
    return it == varBorrowStates_.end() ? nullptr : &it->second;
}

void CodeGenerator::markVariableBorrowed(const std::string& refVar, const std::string& srcVar) {
    getBorrowState(srcVar).immutBorrowCount++;
    BorrowInfo info{refVar, srcVar, false};
    borrowMap_[refVar] = info;
    // Register in the current borrow scope so endScope releases it automatically.
    if (!borrowScopeStack_.empty()) {
        borrowScopeStack_.back().push_back(info);
    }
}

void CodeGenerator::markVariableMutBorrowed(const std::string& refVar, const std::string& srcVar) {
    getBorrowState(srcVar).mutBorrowed = true;
    BorrowInfo info{refVar, srcVar, true};
    borrowMap_[refVar] = info;
    if (!borrowScopeStack_.empty()) {
        borrowScopeStack_.back().push_back(info);
    }
}

void CodeGenerator::releaseBorrow(const std::string& refVar) {
    auto mapIt = borrowMap_.find(refVar);
    if (mapIt == borrowMap_.end()) return;
    const BorrowInfo& info = mapIt->second;
    auto stateIt = varBorrowStates_.find(info.srcVar);
    if (stateIt != varBorrowStates_.end()) {
        if (info.isMut) {
            stateIt->second.mutBorrowed = false;
        } else {
            if (stateIt->second.immutBorrowCount > 0)
                stateIt->second.immutBorrowCount--;
        }
    }
    borrowMap_.erase(mapIt);
}

void CodeGenerator::markVariableFrozen(const std::string& varName) {
    frozenVars_.insert(varName);
    constValues[varName] = true; // prevent writes via checkConstModification
    getBorrowState(varName).frozen = true;
}

bool CodeGenerator::isVariableBorrowed(const std::string& varName) const {
    auto* s = getBorrowStateOpt(varName);
    return s && s->immutBorrowCount > 0;
}

bool CodeGenerator::isVariableFrozen(const std::string& varName) const {
    return frozenVars_.count(varName) > 0;
}

OwnershipState CodeGenerator::getOwnershipState(const std::string& varName) const {
    auto* s = getBorrowStateOpt(varName);
    if (!s) return OwnershipState::Owned;
    return s->state();
}

void CodeGenerator::checkVariableReadable(const std::string& varName, ASTNode* site) {
    // Dead check (moved / invalidated)
    auto deadIt = deadVars_.find(varName);
    if (deadIt != deadVars_.end()) {
        auto reasonIt = deadVarReason_.find(varName);
        const std::string reason = (reasonIt != deadVarReason_.end()) ? reasonIt->second : "moved or invalidated";
        codegenError("Use of " + reason + " variable '" + varName + "'", site);
    }
    // Mutably borrowed: source is completely locked — no reads allowed.
    auto* s = getBorrowStateOpt(varName);
    if (s && s->mutBorrowed) {
        codegenError("Cannot read variable '" + varName +
                     "' — it has an active mutable borrow (the mutable alias must"
                     " go out of scope before the source can be read)", site);
    }
}

void CodeGenerator::generateFreeze(FreezeStmt* stmt) {
    const std::string& name = stmt->varName;

    // Validate: variable must exist
    auto it = namedValues.find(name);
    if (it == namedValues.end()) {
        codegenError("Unknown variable '" + name + "' in freeze statement", stmt);
    }

    // Validate: cannot freeze a moved or invalidated variable
    auto state = getOwnershipState(name);
    if (state == OwnershipState::Moved) {
        codegenError("Cannot freeze moved variable '" + name + "'", stmt);
    }
    if (state == OwnershipState::Invalidated) {
        codegenError("Cannot freeze invalidated variable '" + name + "'", stmt);
    }
    if (state == OwnershipState::MutBorrowed) {
        codegenError("Cannot freeze variable '" + name + "' while it has an active mutable borrow", stmt);
    }

    // Mark the variable as frozen in the ownership lattice
    markVariableFrozen(name);
    optStats_.borrowsFrozen++;

    // ── Alias propagation: freeze x; also freezes the borrow source of x ──
    // If x is a borrow alias (borrowMap_["x"].srcVar == "y"), freeze y too.
    {
        auto bmit = borrowMap_.find(name);
        if (bmit != borrowMap_.end()) {
            const std::string& srcVar = bmit->second.srcVar;
            if (!isVariableFrozen(srcVar)) {
                markVariableFrozen(srcVar);
            }
        }
    }
    // Also freeze all aliases that point to this variable.
    for (auto& kv : borrowMap_) {
        if (kv.second.srcVar == name && !isVariableFrozen(kv.first)) {
            markVariableFrozen(kv.first);
        }
    }

    // ── Emit LLVM freeze instruction ──────────────────────────────────────
    llvm::Value* alloca = it->second;
    if (auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(alloca)) {
        llvm::Type* elemTy = allocaInst->getAllocatedType();
        if (elemTy->isIntegerTy() || elemTy->isFloatingPointTy()) {
            llvm::Value* loadedVal = builder->CreateAlignedLoad(
                elemTy, allocaInst, llvm::MaybeAlign(8), (name + ".freeze.load").c_str());
            llvm::Value* frozenVal = builder->CreateFreeze(loadedVal, (name + ".frozen").c_str());
            builder->CreateAlignedStore(frozenVal, allocaInst, llvm::MaybeAlign(8));
        }

        // Emit llvm.invariant.start to tell LLVM the memory backing this variable
        // will not change from this point forward.
        const uint64_t sz = module->getDataLayout().getTypeAllocSize(elemTy);
        auto* szVal = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), sz);
#if LLVM_VERSION_MAJOR >= 19
        auto* invariantStart = llvm::Intrinsic::getOrInsertDeclaration(
#else
        auto* invariantStart = llvm::Intrinsic::getDeclaration(
#endif
            module.get(), llvm::Intrinsic::invariant_start,
            {llvm::PointerType::getUnqual(*context)});
        builder->CreateCall(invariantStart, {szVal, allocaInst});
    }
}

// generatePipeline: desugar a pipeline { stage ... } block into a for-loop

// Helper: collect all unique array-identifier bases from a statement subtree.
// Used by generatePipeline to know which arrays to auto-prefetch.
static void collectArrayBases(const Statement* s, std::vector<std::string>& out) {
    if (!s) return;
    std::unordered_set<std::string> seen(out.begin(), out.end());
    std::function<void(const Expression*)> scanExpr = [&](const Expression* e) {
        if (!e) return;
        if (e->type == ASTNodeType::INDEX_EXPR) {
            const auto* ie = static_cast<const IndexExpr*>(e);
            if (ie->array && ie->array->type == ASTNodeType::IDENTIFIER_EXPR) {
                const std::string& name =
                    static_cast<const IdentifierExpr*>(ie->array.get())->name;
                if (seen.insert(name).second)
                    out.push_back(name);
            }
            scanExpr(ie->array.get());
            scanExpr(ie->index.get());
            return;
        }
        if (e->type == ASTNodeType::BINARY_EXPR) {
            const auto* b = static_cast<const BinaryExpr*>(e);
            scanExpr(b->left.get()); scanExpr(b->right.get());
        } else if (e->type == ASTNodeType::UNARY_EXPR) {
            scanExpr(static_cast<const UnaryExpr*>(e)->operand.get());
        } else if (e->type == ASTNodeType::CALL_EXPR) {
            for (const auto& a : static_cast<const CallExpr*>(e)->arguments)
                scanExpr(a.get());
        } else if (e->type == ASTNodeType::TERNARY_EXPR) {
            const auto* t = static_cast<const TernaryExpr*>(e);
            scanExpr(t->condition.get()); scanExpr(t->thenExpr.get()); scanExpr(t->elseExpr.get());
        } else if (e->type == ASTNodeType::ASSIGN_EXPR) {
            scanExpr(static_cast<const AssignExpr*>(e)->value.get());
        } else if (e->type == ASTNodeType::INDEX_ASSIGN_EXPR) {
            const auto* ia = static_cast<const IndexAssignExpr*>(e);
            scanExpr(ia->array.get()); scanExpr(ia->index.get()); scanExpr(ia->value.get());
        } else if (e->type == ASTNodeType::ARRAY_EXPR) {
            for (const auto& el : static_cast<const ArrayExpr*>(e)->elements)
                scanExpr(el.get());
        } else if (e->type == ASTNodeType::PIPE_EXPR) {
            const auto* pe = static_cast<const PipeExpr*>(e);
            scanExpr(pe->left.get());
        } else if (e->type == ASTNodeType::POSTFIX_EXPR) {
            scanExpr(static_cast<const PostfixExpr*>(e)->operand.get());
        } else if (e->type == ASTNodeType::PREFIX_EXPR) {
            scanExpr(static_cast<const PrefixExpr*>(e)->operand.get());
        }
    };
    std::function<void(const Statement*)> scanStmt = [&](const Statement* s) {
        if (!s) return;
        switch (s->type) {
        case ASTNodeType::EXPR_STMT:
            scanExpr(static_cast<const ExprStmt*>(s)->expression.get()); break;
        case ASTNodeType::VAR_DECL:
            scanExpr(static_cast<const VarDecl*>(s)->initializer.get()); break;
        case ASTNodeType::MOVE_DECL:
            scanExpr(static_cast<const MoveDecl*>(s)->initializer.get()); break;
        case ASTNodeType::RETURN_STMT:
            scanExpr(static_cast<const ReturnStmt*>(s)->value.get()); break;
        case ASTNodeType::BLOCK:
            for (const auto& sub : static_cast<const BlockStmt*>(s)->statements)
                scanStmt(sub.get());
            break;
        case ASTNodeType::IF_STMT: {
            const auto* ifs = static_cast<const IfStmt*>(s);
            scanExpr(ifs->condition.get());
            scanStmt(ifs->thenBranch.get());
            scanStmt(ifs->elseBranch.get());
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            const auto* ws = static_cast<const WhileStmt*>(s);
            scanExpr(ws->condition.get());
            scanStmt(ws->body.get());
            break;
        }
        case ASTNodeType::FOR_STMT: {
            const auto* fs = static_cast<const ForStmt*>(s);
            scanExpr(fs->start.get()); scanExpr(fs->end.get());
            scanExpr(fs->step.get()); scanStmt(fs->body.get());
            break;
        }
        default: break;
        }
    };
    scanStmt(s);
}

// ─────────────────────────────────────────────────────────────────────────────

void CodeGenerator::generatePipeline(PipelineStmt* stmt) {
    static constexpr const char* kPipelineIter = "__pipeline_i";

    llvm::Function* function = builder->GetInsertBlock()->getParent();
    if (!function) {
        codegenError("pipeline statement outside of function", stmt);
    }

    // Stages execute strictly in order.  `throw` inside a stage routes to
    // the function-level catch handler via the jump-table mechanism.
    const int nStages = static_cast<int>(stmt->stages.size());

    // ── One-shot form ────────────────────────────────────────────────────────
    if (!stmt->count) {
        const ScopeGuard scope(*this);
        for (int si = 0; si < nStages; ++si) {
            if (builder->GetInsertBlock()->getTerminator()) break;
            const ScopeGuard stageScope(*this);
            generateBlock(stmt->stages[si].body.get());
        }
        return;
    }

    // ── Collect array bases across all stages for prefetch insertion ─────────
    std::vector<std::string> prefetchBases;
    for (const auto& stage : stmt->stages)
        collectArrayBases(stage.body.get(), prefetchBases);

    const int64_t prefetchDist = std::max<int64_t>(8, 2LL * nStages);

    // ── Count-based form ─────────────────────────────────────────────────────
    const ScopeGuard scope(*this);

    llvm::Type* iterTy = getDefaultType(); // i64

    // Evaluate count once before the loop.
    llvm::Value* countVal = generateExpression(stmt->count.get());
    countVal = convertTo(countVal, iterTy);

    // Zero-count: no-op.
    if (auto* cv = llvm::dyn_cast<llvm::ConstantInt>(countVal)) {
        if (cv->getSExtValue() <= 0) return;
    }

    // Hidden iterator alloca: __pipeline_i = 0
    llvm::AllocaInst* iterAlloca =
        createEntryBlockAlloca(function, kPipelineIter, iterTy);
    bindVariable(kPipelineIter, iterAlloca);
    loopIterVars_.insert(kPipelineIter);
    builder->CreateStore(llvm::ConstantInt::get(iterTy, 0), iterAlloca);

    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(*context, "pipeline.cond", function);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(*context, "pipeline.body", function);
    llvm::BasicBlock* exitBB = llvm::BasicBlock::Create(*context, "pipeline.exit", function);

    auto savedArrayLenCache = std::move(loopArrayLenCache_);
    loopArrayLenCache_.clear();
    loopStack.push_back({exitBB, condBB});

    builder->CreateBr(condBB);

    // ── Condition: __pipeline_i < count ─────────────────────────────────────
    builder->SetInsertPoint(condBB);
    llvm::Value* iCond = builder->CreateAlignedLoad(
        iterTy, iterAlloca, llvm::MaybeAlign(8), "__pipeline_i.cond");
    llvm::Value* cond = builder->CreateICmpSLT(iCond, countVal, "pipeline.cond.check");
    builder->CreateCondBr(cond, bodyBB, exitBB);

    // ── Body ─────────────────────────────────────────────────────────────────
    builder->SetInsertPoint(bodyBB);

    // ① Prefetch array elements D iterations ahead.
    if (optimizationLevel >= OptimizationLevel::O1 && !prefetchBases.empty()) {
        llvm::Value* iBody = builder->CreateAlignedLoad(
            iterTy, iterAlloca, llvm::MaybeAlign(8), "__pipeline_i.pf");
        llvm::Value* pfDist  = llvm::ConstantInt::get(iterTy,
            static_cast<uint64_t>(prefetchDist), /*isSigned=*/true);
        llvm::Value* pfIndex = builder->CreateAdd(iBody, pfDist,
            "__pipeline_i.pf.idx", /*HasNUW=*/false, /*HasNSW=*/true);

        llvm::Function* prefetchFn = OMSC_GET_INTRINSIC_STMT(
            module.get(), llvm::Intrinsic::prefetch,
            {llvm::PointerType::getUnqual(*context)});
        auto* ptrTy = llvm::PointerType::getUnqual(*context);

        for (const auto& baseName : prefetchBases) {
            auto it = namedValues.find(baseName);
            if (it == namedValues.end()) continue;

            llvm::Value* rawPtr = builder->CreateAlignedLoad(
                iterTy, it->second, llvm::MaybeAlign(8), baseName + ".pf.raw");
            llvm::Value* basePtr =
                rawPtr->getType()->isPointerTy()
                    ? rawPtr
                    : builder->CreateIntToPtr(rawPtr, ptrTy, baseName + ".pf.ptr");

            // Skip length header (GEP + 1)
            llvm::Value* dataPtr = builder->CreateInBoundsGEP(
                getDefaultType(), basePtr,
                llvm::ConstantInt::get(getDefaultType(), 1),
                baseName + ".pf.data");

            llvm::Value* elemPtr = builder->CreateInBoundsGEP(
                getDefaultType(), dataPtr, pfIndex, baseName + ".pf.elemptr");

            auto* pfCall = builder->CreateCall(prefetchFn, {
                elemPtr,
                builder->getInt32(0),   // read
                builder->getInt32(3),   // high temporal locality
                builder->getInt32(1)    // data cache
            });
            pfCall->setMetadata("omscript.pipeline_prefetch",
                llvm::MDNode::get(*context, {}));
        }
    }

    // ② Execute stage bodies sequentially.

    {
        const ScopeGuard stageScope(*this);
        for (int si = 0; si < nStages; ++si) {
            if (builder->GetInsertBlock()->getTerminator()) break;
            const ScopeGuard innerScope(*this);
            generateBlock(stmt->stages[si].body.get());
        }
    }

    // ③ __pipeline_i++
    llvm::Value* iStep = builder->CreateAlignedLoad(
        iterTy, iterAlloca, llvm::MaybeAlign(8), "__pipeline_i.step");
    llvm::Value* iNext = builder->CreateAdd(
        iStep, llvm::ConstantInt::get(iterTy, 1),
        "__pipeline_i.next", /*HasNUW=*/true, /*HasNSW=*/true);
    builder->CreateStore(iNext, iterAlloca);

    // ④ Back-edge with pipeline loop metadata.
    {
        llvm::BranchInst* backEdge =
            llvm::cast<llvm::BranchInst>(builder->CreateBr(condBB));

        llvm::MDBuilder mdb(*context);
        llvm::SmallVector<llvm::Metadata*, 8> mds;
        mds.push_back(nullptr);

        mds.push_back(llvm::MDNode::get(*context,
            {llvm::MDString::get(*context, "llvm.loop.mustprogress")}));

        mds.push_back(llvm::MDNode::get(*context, {
            llvm::MDString::get(*context, "llvm.loop.vectorize.enable"),
            mdb.createConstant(llvm::ConstantInt::getTrue(*context))
        }));

        mds.push_back(llvm::MDNode::get(*context, {
            llvm::MDString::get(*context, "llvm.loop.interleave.count"),
            mdb.createConstant(llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(*context), std::max(2, nStages)))
        }));

        mds.push_back(llvm::MDNode::get(*context, {
            llvm::MDString::get(*context, "llvm.loop.pipeline.initiationinterval"),
            mdb.createConstant(llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(*context), 1))
        }));

        llvm::MDNode* loopMD = llvm::MDNode::get(*context, mds);
        loopMD->replaceOperandWith(0, loopMD);
        backEdge->setMetadata(llvm::LLVMContext::MD_loop, loopMD);
    }

    loopStack.pop_back();
    loopArrayLenCache_ = std::move(savedArrayLenCache);
    loopIterVars_.erase(kPipelineIter);

    builder->SetInsertPoint(exitBB);
}

// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Recursively scan an expression for array accesses using iterVar.
void scanExprForArrayAccesses(
    const Expression* expr,
    const std::string& iterVar,
    std::unordered_map<std::string, bool>& goodArrays,
    std::unordered_set<std::string>& badArrays)
{
    if (!expr) return;
    switch (expr->type) {
    case ASTNodeType::INDEX_EXPR: {
        const auto* ie = static_cast<const IndexExpr*>(expr);
        // Scan the index expression itself first (it may use other arrays).
        scanExprForArrayAccesses(ie->index.get(), iterVar, goodArrays, badArrays);
        // Check whether the index is exactly iterVar.
        bool simpleIndex =
            ie->index->type == ASTNodeType::IDENTIFIER_EXPR &&
            static_cast<const IdentifierExpr*>(ie->index.get())->name == iterVar;
        if (ie->array->type == ASTNodeType::IDENTIFIER_EXPR) {
            const std::string& arrName =
                static_cast<const IdentifierExpr*>(ie->array.get())->name;
            if (!simpleIndex || badArrays.count(arrName)) {
                badArrays.insert(arrName);
                goodArrays.erase(arrName);
            } else {
                if (!goodArrays.count(arrName) && !badArrays.count(arrName))
                    goodArrays[arrName] = false;  // initially read-only
            }
        } else {
            // Non-identifier array base (e.g., nested index, call result):
            // we can't track it, so recurse on the base as a plain expression.
            scanExprForArrayAccesses(ie->array.get(), iterVar, goodArrays, badArrays);
        }
        return;
    }
    case ASTNodeType::INDEX_ASSIGN_EXPR: {
        const auto* iae = static_cast<const IndexAssignExpr*>(expr);
        scanExprForArrayAccesses(iae->index.get(), iterVar, goodArrays, badArrays);
        scanExprForArrayAccesses(iae->value.get(), iterVar, goodArrays, badArrays);
        bool simpleIndex =
            iae->index->type == ASTNodeType::IDENTIFIER_EXPR &&
            static_cast<const IdentifierExpr*>(iae->index.get())->name == iterVar;
        if (iae->array->type == ASTNodeType::IDENTIFIER_EXPR) {
            const std::string& arrName =
                static_cast<const IdentifierExpr*>(iae->array.get())->name;
            if (!simpleIndex || badArrays.count(arrName)) {
                badArrays.insert(arrName);
                goodArrays.erase(arrName);
            } else {
                // Mark as written (value = true).
                auto it = goodArrays.find(arrName);
                if (it != goodArrays.end())
                    it->second = true;
                else if (!badArrays.count(arrName))
                    goodArrays[arrName] = true;
            }
        } else {
            scanExprForArrayAccesses(iae->array.get(), iterVar, goodArrays, badArrays);
        }
        return;
    }
    case ASTNodeType::CALL_EXPR: {
        const auto* ce = static_cast<const CallExpr*>(expr);
        // Length-modifying builtins: disqualify the first argument array.
        static const std::unordered_set<std::string> kLenModifiers = {
            "push", "pop", "array_remove", "array_insert",
            "array_clear", "array_resize"
        };
        if (kLenModifiers.count(ce->callee) && !ce->arguments.empty()) {
            const auto* arg0 = ce->arguments[0].get();
            if (arg0->type == ASTNodeType::IDENTIFIER_EXPR) {
                const std::string& arrName =
                    static_cast<const IdentifierExpr*>(arg0)->name;
                badArrays.insert(arrName);
                goodArrays.erase(arrName);
            }
        }
        for (const auto& arg : ce->arguments)
            scanExprForArrayAccesses(arg.get(), iterVar, goodArrays, badArrays);
        return;
    }
    // For all other expression types, recursively scan children.
    case ASTNodeType::BINARY_EXPR: {
        const auto* be = static_cast<const BinaryExpr*>(expr);
        scanExprForArrayAccesses(be->left.get(),  iterVar, goodArrays, badArrays);
        scanExprForArrayAccesses(be->right.get(), iterVar, goodArrays, badArrays);
        return;
    }
    case ASTNodeType::UNARY_EXPR: {
        const auto* ue = static_cast<const UnaryExpr*>(expr);
        scanExprForArrayAccesses(ue->operand.get(), iterVar, goodArrays, badArrays);
        return;
    }
    case ASTNodeType::TERNARY_EXPR: {
        const auto* te = static_cast<const TernaryExpr*>(expr);
        scanExprForArrayAccesses(te->condition.get(), iterVar, goodArrays, badArrays);
        scanExprForArrayAccesses(te->thenExpr.get(),  iterVar, goodArrays, badArrays);
        scanExprForArrayAccesses(te->elseExpr.get(),  iterVar, goodArrays, badArrays);
        return;
    }
    case ASTNodeType::ASSIGN_EXPR: {
        const auto* ae = static_cast<const AssignExpr*>(expr);
        scanExprForArrayAccesses(ae->value.get(), iterVar, goodArrays, badArrays);
        return;
    }
    default:
        return;  // literals, identifiers, etc. — no array accesses
    }
}

// Recursively scan a statement tree for array accesses using iterVar.
void scanStmtForArrayAccesses(
    const Statement* stmt,
    const std::string& iterVar,
    std::unordered_map<std::string, bool>& goodArrays,
    std::unordered_set<std::string>& badArrays,
    unsigned depth = 0)
{
    if (!stmt) return;
    // Limit recursion into nested for-loops: inner loops may re-use the same
    if (depth > 8) return;

    switch (stmt->type) {
    case ASTNodeType::BLOCK: {
        const auto* blk = static_cast<const BlockStmt*>(stmt);
        for (const auto& s : blk->statements)
            scanStmtForArrayAccesses(s.get(), iterVar, goodArrays, badArrays, depth);
        return;
    }
    case ASTNodeType::EXPR_STMT: {
        scanExprForArrayAccesses(
            static_cast<const ExprStmt*>(stmt)->expression.get(),
            iterVar, goodArrays, badArrays);
        return;
    }
    case ASTNodeType::VAR_DECL: {
        const auto* vd = static_cast<const VarDecl*>(stmt);
        if (vd->initializer)
            scanExprForArrayAccesses(vd->initializer.get(), iterVar, goodArrays, badArrays);
        return;
    }
    case ASTNodeType::RETURN_STMT: {
        const auto* rs = static_cast<const ReturnStmt*>(stmt);
        if (rs->value)
            scanExprForArrayAccesses(rs->value.get(), iterVar, goodArrays, badArrays);
        return;
    }
    case ASTNodeType::IF_STMT: {
        const auto* is = static_cast<const IfStmt*>(stmt);
        scanExprForArrayAccesses(is->condition.get(), iterVar, goodArrays, badArrays);
        scanStmtForArrayAccesses(is->thenBranch.get(), iterVar, goodArrays, badArrays, depth);
        if (is->elseBranch)
            scanStmtForArrayAccesses(is->elseBranch.get(), iterVar, goodArrays, badArrays, depth);
        return;
    }
    case ASTNodeType::WHILE_STMT: {
        const auto* ws = static_cast<const WhileStmt*>(stmt);
        scanExprForArrayAccesses(ws->condition.get(), iterVar, goodArrays, badArrays);
        scanStmtForArrayAccesses(ws->body.get(), iterVar, goodArrays, badArrays, depth + 1);
        return;
    }
    case ASTNodeType::FOR_STMT: {
        // For nested for-loops: only recurse if the INNER iterator is different
        const auto* fs = static_cast<const ForStmt*>(stmt);
        if (fs->iteratorVar != iterVar)
            scanStmtForArrayAccesses(fs->body.get(), iterVar, goodArrays, badArrays, depth + 1);
        return;
    }
    case ASTNodeType::FOR_EACH_STMT: {
        const auto* fes = static_cast<const ForEachStmt*>(stmt);
        // If the collection is a named array, check if iterVar accesses it.
        if (fes->collection->type == ASTNodeType::IDENTIFIER_EXPR) {
            const std::string& cn =
                static_cast<const IdentifierExpr*>(fes->collection.get())->name;
            // for-each iterates the whole array — length changes would be visible.
            // Conservatively disqualify.
            if (goodArrays.count(cn) || badArrays.count(cn)) {
                badArrays.insert(cn);
                goodArrays.erase(cn);
            }
        }
        scanStmtForArrayAccesses(fes->body.get(), iterVar, goodArrays, badArrays, depth + 1);
        return;
    }
    default:
        return;
    }
}
} // anonymous namespace

std::unordered_map<std::string, bool>
CodeGenerator::preScanLoopArrayAccesses(const Statement* body,
                                         const std::string& iterVar) {
    std::unordered_map<std::string, bool> goodArrays;
    std::unordered_set<std::string> badArrays;
    scanStmtForArrayAccesses(body, iterVar, goodArrays, badArrays);
    return goodArrays;
}

} // namespace omscript
