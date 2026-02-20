#include "jit.h"
#include "../include/bytecode.h"
#include "vm.h"

#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Utils.h>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <set>

namespace omscript {

// Static member definition.
TypeProfile BytecodeJIT::emptyProfile_;

// ---------------------------------------------------------------------------
// Helpers for reading bytecode without advancing an instruction pointer.
// ---------------------------------------------------------------------------

static uint16_t peekShort(const std::vector<uint8_t>& code, size_t offset) {
    return static_cast<uint16_t>(code[offset]) |
           (static_cast<uint16_t>(code[offset + 1]) << 8);
}

static int64_t peekInt(const std::vector<uint8_t>& code, size_t offset) {
    uint64_t raw = 0;
    for (int i = 0; i < 8; i++) {
        raw |= static_cast<uint64_t>(code[offset + i]) << (i * 8);
    }
    int64_t value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

static double peekFloat(const std::vector<uint8_t>& code, size_t offset) {
    uint64_t raw = 0;
    for (int i = 0; i < 8; i++) {
        raw |= static_cast<uint64_t>(code[offset + i]) << (i * 8);
    }
    double value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

// ---------------------------------------------------------------------------
// BytecodeJIT
// ---------------------------------------------------------------------------

BytecodeJIT::BytecodeJIT() = default;
BytecodeJIT::~BytecodeJIT() = default;

void BytecodeJIT::ensureInitialized() {
    if (!initialized_) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        initialized_ = true;
    }
}

bool BytecodeJIT::isCompiled(const std::string& name) const {
    return compiled_.count(name) > 0 || compiledFloat_.count(name) > 0;
}

BytecodeJIT::JITFnPtr BytecodeJIT::getCompiled(const std::string& name) const {
    auto it = compiled_.find(name);
    return it != compiled_.end() ? it->second : nullptr;
}

BytecodeJIT::JITFloatFnPtr BytecodeJIT::getCompiledFloat(const std::string& name) const {
    auto it = compiledFloat_.find(name);
    return it != compiledFloat_.end() ? it->second : nullptr;
}

JITSpecialization BytecodeJIT::getSpecialization(const std::string& name) const {
    auto it = specializations_.find(name);
    return it != specializations_.end() ? it->second : JITSpecialization::Unknown;
}

bool BytecodeJIT::recordCall(const std::string& name) {
    if (compiled_.count(name) || failedCompilations_.count(name))
        return false;
    auto& count = callCounts_[name];
    ++count;
    return count == kJITThreshold;
}

bool BytecodeJIT::recordPostJITCall(const std::string& name) {
    if (!compiled_.count(name) || recompiled_.count(name))
        return false;
    auto& count = postJITCallCounts_[name];
    ++count;
    return count == kRecompileThreshold;
}

size_t BytecodeJIT::getCallCount(const std::string& name) const {
    auto it = callCounts_.find(name);
    if (it != callCounts_.end()) return it->second;
    return 0;
}

void BytecodeJIT::recordTypes(const std::string& name, bool allInt, bool allFloat) {
    auto& profile = typeProfiles_[name];
    if (allInt)
        profile.intCalls++;
    else if (allFloat)
        profile.floatCalls++;
    else
        profile.mixedCalls++;
}

const TypeProfile& BytecodeJIT::getTypeProfile(const std::string& name) const {
    auto it = typeProfiles_.find(name);
    return it != typeProfiles_.end() ? it->second : emptyProfile_;
}

bool BytecodeJIT::recompile(const BytecodeFunction& func) {
    // Mark as recompiled so we only attempt once per function lifetime.
    recompiled_.insert(func.name);

    ensureInitialized();

    // Determine the best specialization from the type profile.
    auto spec = getTypeProfile(func.name).bestSpecialization();
    if (spec == JITSpecialization::Unknown || spec == JITSpecialization::Mixed)
        spec = JITSpecialization::IntOnly;

    // If already compiled with the same specialization, skip.
    auto curSpec = getSpecialization(func.name);
    if (curSpec == spec)
        return true;

    // Save old pointers so we can restore on failure.
    auto oldIntIt = compiled_.find(func.name);
    auto oldFloatIt = compiledFloat_.find(func.name);
    JITFnPtr oldIntPtr = oldIntIt != compiled_.end() ? oldIntIt->second : nullptr;
    JITFloatFnPtr oldFloatPtr = oldFloatIt != compiledFloat_.end() ? oldFloatIt->second : nullptr;

    // Remove old entries so compile() can proceed.
    compiled_.erase(func.name);
    compiledFloat_.erase(func.name);
    specializations_.erase(func.name);
    failedCompilations_.erase(func.name);

    bool ok = compile(func, spec);
    if (!ok) {
        // Restore old pointers if recompilation failed.
        if (oldIntPtr) compiled_[func.name] = oldIntPtr;
        if (oldFloatPtr) compiledFloat_[func.name] = oldFloatPtr;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// compile() — translate bytecode → LLVM IR → native code
// ---------------------------------------------------------------------------

bool BytecodeJIT::compile(const BytecodeFunction& func,
                         JITSpecialization spec) {
    ensureInitialized();

    const auto& code = func.bytecode;
    if (code.empty() || code.size() < kMinBytecodeSize) {
        failedCompilations_.insert(func.name);
        return false;
    }

    // Use the type profile to determine specialization if Unknown.
    if (spec == JITSpecialization::Unknown) {
        spec = getTypeProfile(func.name).bestSpecialization();
        if (spec == JITSpecialization::Unknown || spec == JITSpecialization::Mixed)
            spec = JITSpecialization::IntOnly;
    }

    bool ok = false;
    if (spec == JITSpecialization::FloatOnly)
        ok = compileFloat(func);
    else
        ok = compileInt(func);

    if (ok)
        specializations_[func.name] = spec;
    return ok;
}

// ---------------------------------------------------------------------------
// compileInt() — integer-specialized JIT (original path)
// ---------------------------------------------------------------------------

bool BytecodeJIT::compileInt(const BytecodeFunction& func) {
    const auto& code = func.bytecode;

    // ------------------------------------------------------------------
    // Phase 1: Pre-scan — check JIT eligibility & find basic-block starts
    // ------------------------------------------------------------------
    std::set<size_t> blockStarts;
    blockStarts.insert(0);

    size_t maxLocalIdx = 0;
    if (func.arity > 0)
        maxLocalIdx = func.arity - 1;

    size_t ip = 0;
    while (ip < code.size()) {
        auto op = static_cast<OpCode>(code[ip]);
        ip++;

        switch (op) {
        case OpCode::PUSH_INT:
            ip += 8;
            break;
        case OpCode::POP:
        case OpCode::ADD:
        case OpCode::SUB:
        case OpCode::MUL:
        case OpCode::DIV:
        case OpCode::MOD:
        case OpCode::NEG:
        case OpCode::EQ:
        case OpCode::NE:
        case OpCode::LT:
        case OpCode::LE:
        case OpCode::GT:
        case OpCode::GE:
        case OpCode::AND:
        case OpCode::OR:
        case OpCode::NOT:
        case OpCode::BIT_AND:
        case OpCode::BIT_OR:
        case OpCode::BIT_XOR:
        case OpCode::BIT_NOT:
        case OpCode::SHL:
        case OpCode::SHR:
        case OpCode::DUP:
            break;
        case OpCode::LOAD_LOCAL:
        case OpCode::STORE_LOCAL: {
            uint8_t idx = code[ip];
            ip++;
            if (idx > maxLocalIdx)
                maxLocalIdx = idx;
            break;
        }
        case OpCode::JUMP: {
            uint16_t target = peekShort(code, ip);
            ip += 2;
            blockStarts.insert(target);
            blockStarts.insert(ip);
            break;
        }
        case OpCode::JUMP_IF_FALSE: {
            uint16_t target = peekShort(code, ip);
            ip += 2;
            blockStarts.insert(target);
            blockStarts.insert(ip);
            break;
        }
        case OpCode::RETURN:
            blockStarts.insert(ip);
            break;

        // Unsupported opcodes — bail out to interpretation.
        case OpCode::PUSH_FLOAT:
        case OpCode::PUSH_STRING:
        case OpCode::LOAD_VAR:
        case OpCode::STORE_VAR:
        case OpCode::CALL:
        case OpCode::PRINT:
        case OpCode::HALT:
        default:
            failedCompilations_.insert(func.name);
            return false;
        }
    }

    // ------------------------------------------------------------------
    // Phase 2: Build LLVM module & function
    // ------------------------------------------------------------------
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("jit_" + func.name, *ctx);
    llvm::IRBuilder<> builder(*ctx);

    auto* int64Ty  = llvm::Type::getInt64Ty(*ctx);
    auto* int32Ty  = llvm::Type::getInt32Ty(*ctx);
    auto* int64PtrTy = llvm::PointerType::getUnqual(int64Ty);

    llvm::FunctionType* fnType =
        llvm::FunctionType::get(int64Ty, {int64PtrTy, int32Ty}, false);

    std::string jitName = "jit_" + func.name;
    llvm::Function* fn = llvm::Function::Create(
        fnType, llvm::Function::ExternalLinkage, jitName, mod.get());

    llvm::Argument* argsPtr  = fn->arg_begin();
    argsPtr->setName("args");

    // --- Create basic blocks ---
    std::map<size_t, llvm::BasicBlock*> blocks;
    for (size_t start : blockStarts) {
        if (start <= code.size()) {
            blocks[start] = llvm::BasicBlock::Create(
                *ctx, "bb_" + std::to_string(start), fn);
        }
    }

    // --- Entry block: allocate locals & copy arguments ---
    llvm::BasicBlock* entryBB =
        llvm::BasicBlock::Create(*ctx, "entry", fn, blocks.begin()->second);
    builder.SetInsertPoint(entryBB);

    size_t numLocals = maxLocalIdx + 1;
    std::vector<llvm::AllocaInst*> locals(numLocals);
    for (size_t i = 0; i < numLocals; i++) {
        locals[i] = builder.CreateAlloca(
            int64Ty, nullptr, "local_" + std::to_string(i));
    }

    for (size_t i = 0; i < func.arity; i++) {
        auto* idx = llvm::ConstantInt::get(int32Ty, i);
        auto* ptr = builder.CreateGEP(int64Ty, argsPtr, idx,
                                      "arg_ptr_" + std::to_string(i));
        auto* val = builder.CreateLoad(int64Ty, ptr,
                                       "arg_" + std::to_string(i));
        builder.CreateStore(val, locals[i]);
    }
    for (size_t i = func.arity; i < numLocals; i++) {
        builder.CreateStore(llvm::ConstantInt::get(int64Ty, 0), locals[i]);
    }
    builder.CreateBr(blocks.begin()->second);

    // Declare abort() for division-by-zero guards.
    llvm::FunctionType* abortType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx), false);
    llvm::FunctionCallee abortFn =
        mod->getOrInsertFunction("abort", abortType);

    // ------------------------------------------------------------------
    // Phase 3: Translate each basic block
    // ------------------------------------------------------------------
    for (auto blockIt = blocks.begin(); blockIt != blocks.end(); ++blockIt) {
        size_t blockStart = blockIt->first;
        llvm::BasicBlock* bb = blockIt->second;
        if (blockStart >= code.size())
            continue;

        auto nextIt = std::next(blockIt);
        size_t blockEnd = (nextIt != blocks.end()) ? nextIt->first : code.size();

        builder.SetInsertPoint(bb);
        llvm::BasicBlock* currentBB = bb;

        // Compile-time operand stack (SSA values).
        std::vector<llvm::Value*> cstack;

        ip = blockStart;
        bool terminated = false;

        while (ip < blockEnd && !terminated) {
            auto op = static_cast<OpCode>(code[ip]);
            ip++;

            switch (op) {
            case OpCode::PUSH_INT: {
                int64_t val = peekInt(code, ip);
                ip += 8;
                cstack.push_back(llvm::ConstantInt::get(int64Ty, val));
                break;
            }
            case OpCode::POP:
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                cstack.pop_back();
                break;
            case OpCode::DUP:
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                cstack.push_back(cstack.back());
                break;

            // ---- arithmetic ----
            case OpCode::ADD: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateNSWAdd(a, b, "add"));
                break;
            }
            case OpCode::SUB: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateNSWSub(a, b, "sub"));
                break;
            }
            case OpCode::MUL: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateNSWMul(a, b, "mul"));
                break;
            }
            case OpCode::DIV: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                auto* isZero = builder.CreateICmpEQ(
                    b, llvm::ConstantInt::get(int64Ty, 0), "divzero");
                auto* errBB = llvm::BasicBlock::Create(*ctx, "diverr", fn);
                auto* okBB  = llvm::BasicBlock::Create(*ctx, "divok", fn);
                builder.CreateCondBr(isZero, errBB, okBB);
                builder.SetInsertPoint(errBB);
                builder.CreateCall(abortFn);
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBB);
                currentBB = okBB;
                cstack.push_back(builder.CreateSDiv(a, b, "div"));
                break;
            }
            case OpCode::MOD: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                auto* isZero = builder.CreateICmpEQ(
                    b, llvm::ConstantInt::get(int64Ty, 0), "modzero");
                auto* errBB = llvm::BasicBlock::Create(*ctx, "moderr", fn);
                auto* okBB  = llvm::BasicBlock::Create(*ctx, "modok", fn);
                builder.CreateCondBr(isZero, errBB, okBB);
                builder.SetInsertPoint(errBB);
                builder.CreateCall(abortFn);
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBB);
                currentBB = okBB;
                cstack.push_back(builder.CreateSRem(a, b, "mod"));
                break;
            }
            case OpCode::NEG: {
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateNeg(a, "neg"));
                break;
            }

            // ---- comparisons (result is 0 or 1 as i64) ----
#define JIT_CMP(NAME, PRED) \
            case OpCode::NAME: { \
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; } \
                auto* b = cstack.back(); cstack.pop_back(); \
                auto* a = cstack.back(); cstack.pop_back(); \
                auto* cmp = builder.CreateICmp(llvm::CmpInst::PRED, a, b, #NAME); \
                cstack.push_back(builder.CreateZExt(cmp, int64Ty, #NAME "_i64")); \
                break; \
            }
            JIT_CMP(EQ, ICMP_EQ)
            JIT_CMP(NE, ICMP_NE)
            JIT_CMP(LT, ICMP_SLT)
            JIT_CMP(LE, ICMP_SLE)
            JIT_CMP(GT, ICMP_SGT)
            JIT_CMP(GE, ICMP_SGE)
#undef JIT_CMP

            // ---- logical ----
            case OpCode::AND: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                auto* boolA = builder.CreateICmpNE(a, llvm::ConstantInt::get(int64Ty, 0));
                auto* boolB = builder.CreateICmpNE(b, llvm::ConstantInt::get(int64Ty, 0));
                auto* r = builder.CreateAnd(boolA, boolB, "and");
                cstack.push_back(builder.CreateZExt(r, int64Ty, "and_i64"));
                break;
            }
            case OpCode::OR: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                auto* boolA = builder.CreateICmpNE(a, llvm::ConstantInt::get(int64Ty, 0));
                auto* boolB = builder.CreateICmpNE(b, llvm::ConstantInt::get(int64Ty, 0));
                auto* r = builder.CreateOr(boolA, boolB, "or");
                cstack.push_back(builder.CreateZExt(r, int64Ty, "or_i64"));
                break;
            }
            case OpCode::NOT: {
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                auto* a = cstack.back(); cstack.pop_back();
                auto* r = builder.CreateICmpEQ(
                    a, llvm::ConstantInt::get(int64Ty, 0), "not");
                cstack.push_back(builder.CreateZExt(r, int64Ty, "not_i64"));
                break;
            }

            // ---- bitwise ----
            case OpCode::BIT_AND: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateAnd(a, b, "band"));
                break;
            }
            case OpCode::BIT_OR: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateOr(a, b, "bor"));
                break;
            }
            case OpCode::BIT_XOR: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateXor(a, b, "bxor"));
                break;
            }
            case OpCode::BIT_NOT: {
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateNot(a, "bnot"));
                break;
            }
            case OpCode::SHL: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateShl(a, b, "shl"));
                break;
            }
            case OpCode::SHR: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateAShr(a, b, "shr"));
                break;
            }

            // ---- locals ----
            case OpCode::LOAD_LOCAL: {
                uint8_t idx = code[ip++];
                if (idx >= numLocals) { failedCompilations_.insert(func.name); return false; }
                cstack.push_back(builder.CreateLoad(
                    int64Ty, locals[idx], "ld_" + std::to_string(idx)));
                break;
            }
            case OpCode::STORE_LOCAL: {
                uint8_t idx = code[ip++];
                if (idx >= numLocals || cstack.empty()) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                // STORE_LOCAL peeks (does not pop) in the interpreter.
                builder.CreateStore(cstack.back(), locals[idx]);
                break;
            }

            // ---- control flow ----
            case OpCode::JUMP: {
                uint16_t target = peekShort(code, ip);
                ip += 2;
                if (!cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                auto it = blocks.find(target);
                if (it == blocks.end()) { failedCompilations_.insert(func.name); return false; }
                builder.CreateBr(it->second);
                terminated = true;
                break;
            }
            case OpCode::JUMP_IF_FALSE: {
                uint16_t target = peekShort(code, ip);
                ip += 2;
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                auto* cond = cstack.back(); cstack.pop_back();
                if (!cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                auto* condBool = builder.CreateICmpNE(
                    cond, llvm::ConstantInt::get(int64Ty, 0), "cond");
                auto targetIt = blocks.find(target);
                if (targetIt == blocks.end()) { failedCompilations_.insert(func.name); return false; }
                auto fallIt = blocks.find(ip);
                if (fallIt == blocks.end()) { failedCompilations_.insert(func.name); return false; }
                builder.CreateCondBr(condBool, fallIt->second, targetIt->second);
                terminated = true;
                break;
            }
            case OpCode::RETURN: {
                llvm::Value* retVal;
                if (cstack.empty()) {
                    retVal = llvm::ConstantInt::get(int64Ty, 0);
                } else {
                    retVal = cstack.back();
                    cstack.pop_back();
                }
                builder.CreateRet(retVal);
                terminated = true;
                break;
            }
            default:
                failedCompilations_.insert(func.name);
                return false;
            }
        }

        // If the block fell through without a terminator, branch to the next.
        if (!terminated && currentBB->getTerminator() == nullptr) {
            auto nextBBIt = blocks.find(blockEnd);
            if (nextBBIt != blocks.end()) {
                if (!cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                builder.CreateBr(nextBBIt->second);
            } else {
                llvm::Value* rv = cstack.empty()
                    ? llvm::ConstantInt::get(int64Ty, 0)
                    : cstack.back();
                builder.CreateRet(rv);
            }
        }
    }

    // Add terminators to any unreachable blocks (e.g. code after RETURN).
    for (auto& [start, bb] : blocks) {
        if (!bb->getTerminator()) {
            builder.SetInsertPoint(bb);
            builder.CreateRet(llvm::ConstantInt::get(int64Ty, 0));
        }
    }

    // ------------------------------------------------------------------
    // Phase 4: Verify, optimize & JIT-compile
    // ------------------------------------------------------------------
    std::string errStr;
    llvm::raw_string_ostream errStream(errStr);
    if (llvm::verifyFunction(*fn, &errStream)) {
        failedCompilations_.insert(func.name);
        return false;
    }

    // Run lightweight optimization passes on the JIT module to reduce
    // redundant loads/stores and simplify control flow before compiling
    // to native code.
    {
        llvm::legacy::FunctionPassManager fpm(mod.get());
        fpm.add(llvm::createPromoteMemoryToRegisterPass());
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createReassociatePass());
        fpm.add(llvm::createGVNPass());
        fpm.add(llvm::createCFGSimplificationPass());
        fpm.doInitialization();
        fpm.run(*fn);
        fpm.doFinalization();
    }

    std::string engineError;
    llvm::EngineBuilder engineBuilder(std::move(mod));
    engineBuilder.setErrorStr(&engineError);
    engineBuilder.setEngineKind(llvm::EngineKind::JIT);
    engineBuilder.setOptLevel(llvm::CodeGenOptLevel::Aggressive);
    llvm::ExecutionEngine* engine = engineBuilder.create();
    if (!engine) {
        failedCompilations_.insert(func.name);
        return false;
    }

    engine->finalizeObject();
    uint64_t fnAddr = engine->getFunctionAddress(jitName);
    if (!fnAddr) {
        delete engine;
        failedCompilations_.insert(func.name);
        return false;
    }

    compiled_[func.name] = reinterpret_cast<JITFnPtr>(fnAddr);
    modules_.push_back({std::move(ctx),
                        std::unique_ptr<llvm::ExecutionEngine>(engine)});
    return true;
}

// ---------------------------------------------------------------------------
// compileFloat() — float-specialized JIT compilation
//
// Generates the same structure as compileInt but with double-typed IR.
// Signature: double jit_<name>_float(double* args, int argCount)
// ---------------------------------------------------------------------------

bool BytecodeJIT::compileFloat(const BytecodeFunction& func) {
    const auto& code = func.bytecode;

    // ------------------------------------------------------------------
    // Phase 1: Pre-scan — check eligibility & find basic-block starts
    // ------------------------------------------------------------------
    std::set<size_t> blockStarts;
    blockStarts.insert(0);

    size_t maxLocalIdx = 0;
    if (func.arity > 0)
        maxLocalIdx = func.arity - 1;

    size_t ip = 0;
    while (ip < code.size()) {
        auto op = static_cast<OpCode>(code[ip]);
        ip++;

        switch (op) {
        case OpCode::PUSH_INT:
        case OpCode::PUSH_FLOAT:
            ip += 8;
            break;
        case OpCode::POP:
        case OpCode::ADD: case OpCode::SUB: case OpCode::MUL:
        case OpCode::DIV: case OpCode::MOD: case OpCode::NEG:
        case OpCode::EQ: case OpCode::NE: case OpCode::LT:
        case OpCode::LE: case OpCode::GT: case OpCode::GE:
        case OpCode::NOT: case OpCode::DUP:
            break;
        case OpCode::LOAD_LOCAL:
        case OpCode::STORE_LOCAL: {
            uint8_t idx = code[ip]; ip++;
            if (idx > maxLocalIdx) maxLocalIdx = idx;
            break;
        }
        case OpCode::JUMP: {
            uint16_t target = peekShort(code, ip); ip += 2;
            blockStarts.insert(target);
            blockStarts.insert(ip);
            break;
        }
        case OpCode::JUMP_IF_FALSE: {
            uint16_t target = peekShort(code, ip); ip += 2;
            blockStarts.insert(target);
            blockStarts.insert(ip);
            break;
        }
        case OpCode::RETURN:
            blockStarts.insert(ip);
            break;
        // Float JIT does not support strings, globals, CALL, PRINT,
        // or bitwise ops (which only make sense for integers).
        default:
            failedCompilations_.insert(func.name);
            return false;
        }
    }

    // ------------------------------------------------------------------
    // Phase 2: Build LLVM module & function (double-typed)
    // ------------------------------------------------------------------
    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto mod = std::make_unique<llvm::Module>("jitf_" + func.name, *ctx);
    llvm::IRBuilder<> builder(*ctx);

    auto* doubleTy  = llvm::Type::getDoubleTy(*ctx);
    auto* int32Ty   = llvm::Type::getInt32Ty(*ctx);
    auto* doublePtrTy = llvm::PointerType::getUnqual(doubleTy);

    llvm::FunctionType* fnType =
        llvm::FunctionType::get(doubleTy, {doublePtrTy, int32Ty}, false);

    std::string jitName = "jit_" + func.name + "_float";
    llvm::Function* fn = llvm::Function::Create(
        fnType, llvm::Function::ExternalLinkage, jitName, mod.get());

    llvm::Argument* argsPtr = fn->arg_begin();
    argsPtr->setName("args");

    // --- Create basic blocks ---
    std::map<size_t, llvm::BasicBlock*> blocks;
    for (size_t start : blockStarts) {
        if (start <= code.size()) {
            blocks[start] = llvm::BasicBlock::Create(
                *ctx, "bb_" + std::to_string(start), fn);
        }
    }

    // --- Entry block: allocate locals & copy arguments ---
    llvm::BasicBlock* entryBB =
        llvm::BasicBlock::Create(*ctx, "entry", fn, blocks.begin()->second);
    builder.SetInsertPoint(entryBB);

    size_t numLocals = maxLocalIdx + 1;
    std::vector<llvm::AllocaInst*> locals(numLocals);
    for (size_t i = 0; i < numLocals; i++) {
        locals[i] = builder.CreateAlloca(
            doubleTy, nullptr, "local_" + std::to_string(i));
    }

    for (size_t i = 0; i < func.arity; i++) {
        auto* idx = llvm::ConstantInt::get(int32Ty, i);
        auto* ptr = builder.CreateGEP(doubleTy, argsPtr, idx,
                                      "arg_ptr_" + std::to_string(i));
        auto* val = builder.CreateLoad(doubleTy, ptr,
                                       "arg_" + std::to_string(i));
        builder.CreateStore(val, locals[i]);
    }
    for (size_t i = func.arity; i < numLocals; i++) {
        builder.CreateStore(llvm::ConstantFP::get(doubleTy, 0.0), locals[i]);
    }
    builder.CreateBr(blocks.begin()->second);

    // Declare abort() for division-by-zero guards.
    llvm::FunctionType* abortType =
        llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx), false);
    llvm::FunctionCallee abortFn =
        mod->getOrInsertFunction("abort", abortType);

    // ------------------------------------------------------------------
    // Phase 3: Translate each basic block (double-typed arithmetic)
    // ------------------------------------------------------------------
    for (auto blockIt = blocks.begin(); blockIt != blocks.end(); ++blockIt) {
        size_t blockStart = blockIt->first;
        llvm::BasicBlock* bb = blockIt->second;
        if (blockStart >= code.size()) continue;

        auto nextIt = std::next(blockIt);
        size_t blockEnd = (nextIt != blocks.end()) ? nextIt->first : code.size();

        builder.SetInsertPoint(bb);
        llvm::BasicBlock* currentBB = bb;
        std::vector<llvm::Value*> cstack;

        ip = blockStart;
        bool terminated = false;

        while (ip < blockEnd && !terminated) {
            auto op = static_cast<OpCode>(code[ip]); ip++;

            switch (op) {
            case OpCode::PUSH_INT: {
                // Promote integer constants to double in float specialization.
                int64_t val = peekInt(code, ip); ip += 8;
                cstack.push_back(llvm::ConstantFP::get(doubleTy, static_cast<double>(val)));
                break;
            }
            case OpCode::PUSH_FLOAT: {
                double val = peekFloat(code, ip);
                ip += 8;
                cstack.push_back(llvm::ConstantFP::get(doubleTy, val));
                break;
            }
            case OpCode::POP:
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                cstack.pop_back();
                break;
            case OpCode::DUP:
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                cstack.push_back(cstack.back());
                break;

            // ---- float arithmetic ----
            case OpCode::ADD: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateFAdd(a, b, "fadd"));
                break;
            }
            case OpCode::SUB: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateFSub(a, b, "fsub"));
                break;
            }
            case OpCode::MUL: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateFMul(a, b, "fmul"));
                break;
            }
            case OpCode::DIV: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                auto* isZero = builder.CreateFCmpOEQ(
                    b, llvm::ConstantFP::get(doubleTy, 0.0), "fdivzero");
                auto* errBB = llvm::BasicBlock::Create(*ctx, "fdiverr", fn);
                auto* okBB  = llvm::BasicBlock::Create(*ctx, "fdivok", fn);
                builder.CreateCondBr(isZero, errBB, okBB);
                builder.SetInsertPoint(errBB);
                builder.CreateCall(abortFn);
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBB);
                currentBB = okBB;
                cstack.push_back(builder.CreateFDiv(a, b, "fdiv"));
                break;
            }
            case OpCode::MOD: {
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; }
                auto* b = cstack.back(); cstack.pop_back();
                auto* a = cstack.back(); cstack.pop_back();
                auto* isZero = builder.CreateFCmpOEQ(
                    b, llvm::ConstantFP::get(doubleTy, 0.0), "fmodzero");
                auto* errBB = llvm::BasicBlock::Create(*ctx, "fmoderr", fn);
                auto* okBB  = llvm::BasicBlock::Create(*ctx, "fmodok", fn);
                builder.CreateCondBr(isZero, errBB, okBB);
                builder.SetInsertPoint(errBB);
                builder.CreateCall(abortFn);
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBB);
                currentBB = okBB;
                cstack.push_back(builder.CreateFRem(a, b, "fmod"));
                break;
            }
            case OpCode::NEG: {
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                auto* a = cstack.back(); cstack.pop_back();
                cstack.push_back(builder.CreateFNeg(a, "fneg"));
                break;
            }

            // ---- float comparisons (result is 0.0 or 1.0) ----
#define FLOAT_CMP(NAME, PRED) \
            case OpCode::NAME: { \
                if (cstack.size() < 2) { failedCompilations_.insert(func.name); return false; } \
                auto* b = cstack.back(); cstack.pop_back(); \
                auto* a = cstack.back(); cstack.pop_back(); \
                auto* cmp = builder.CreateFCmp(llvm::CmpInst::PRED, a, b, "f" #NAME); \
                cstack.push_back(builder.CreateUIToFP(cmp, doubleTy, "f" #NAME "_d")); \
                break; \
            }
            FLOAT_CMP(EQ, FCMP_OEQ)
            FLOAT_CMP(NE, FCMP_ONE)
            FLOAT_CMP(LT, FCMP_OLT)
            FLOAT_CMP(LE, FCMP_OLE)
            FLOAT_CMP(GT, FCMP_OGT)
            FLOAT_CMP(GE, FCMP_OGE)
#undef FLOAT_CMP

            case OpCode::NOT: {
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                auto* a = cstack.back(); cstack.pop_back();
                auto* isZero = builder.CreateFCmpOEQ(
                    a, llvm::ConstantFP::get(doubleTy, 0.0), "fnot");
                cstack.push_back(builder.CreateUIToFP(isZero, doubleTy, "fnot_d"));
                break;
            }

            // ---- locals ----
            case OpCode::LOAD_LOCAL: {
                uint8_t idx = code[ip++];
                if (idx >= numLocals) { failedCompilations_.insert(func.name); return false; }
                cstack.push_back(builder.CreateLoad(
                    doubleTy, locals[idx], "fld_" + std::to_string(idx)));
                break;
            }
            case OpCode::STORE_LOCAL: {
                uint8_t idx = code[ip++];
                if (idx >= numLocals || cstack.empty()) {
                    failedCompilations_.insert(func.name); return false;
                }
                builder.CreateStore(cstack.back(), locals[idx]);
                break;
            }

            // ---- control flow ----
            case OpCode::JUMP: {
                uint16_t target = peekShort(code, ip); ip += 2;
                if (!cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                auto it = blocks.find(target);
                if (it == blocks.end()) { failedCompilations_.insert(func.name); return false; }
                builder.CreateBr(it->second);
                terminated = true;
                break;
            }
            case OpCode::JUMP_IF_FALSE: {
                uint16_t target = peekShort(code, ip); ip += 2;
                if (cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                auto* cond = cstack.back(); cstack.pop_back();
                if (!cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                // Compare double != 0.0 for truthiness
                auto* condBool = builder.CreateFCmpONE(
                    cond, llvm::ConstantFP::get(doubleTy, 0.0), "fcond");
                auto targetIt = blocks.find(target);
                if (targetIt == blocks.end()) { failedCompilations_.insert(func.name); return false; }
                auto fallIt = blocks.find(ip);
                if (fallIt == blocks.end()) { failedCompilations_.insert(func.name); return false; }
                builder.CreateCondBr(condBool, fallIt->second, targetIt->second);
                terminated = true;
                break;
            }
            case OpCode::RETURN: {
                llvm::Value* retVal;
                if (cstack.empty()) {
                    retVal = llvm::ConstantFP::get(doubleTy, 0.0);
                } else {
                    retVal = cstack.back();
                    cstack.pop_back();
                }
                builder.CreateRet(retVal);
                terminated = true;
                break;
            }
            default:
                failedCompilations_.insert(func.name);
                return false;
            }
        }

        if (!terminated && currentBB->getTerminator() == nullptr) {
            auto nextBBIt = blocks.find(blockEnd);
            if (nextBBIt != blocks.end()) {
                if (!cstack.empty()) { failedCompilations_.insert(func.name); return false; }
                builder.CreateBr(nextBBIt->second);
            } else {
                llvm::Value* rv = cstack.empty()
                    ? llvm::ConstantFP::get(doubleTy, 0.0)
                    : cstack.back();
                builder.CreateRet(rv);
            }
        }
    }

    // Terminate unreachable blocks.
    for (auto& [start, bb] : blocks) {
        if (!bb->getTerminator()) {
            builder.SetInsertPoint(bb);
            builder.CreateRet(llvm::ConstantFP::get(doubleTy, 0.0));
        }
    }

    // ------------------------------------------------------------------
    // Phase 4: Verify, optimize & JIT-compile
    // ------------------------------------------------------------------
    std::string errStr;
    llvm::raw_string_ostream errStream(errStr);
    if (llvm::verifyFunction(*fn, &errStream)) {
        failedCompilations_.insert(func.name);
        return false;
    }

    {
        llvm::legacy::FunctionPassManager fpm(mod.get());
        fpm.add(llvm::createPromoteMemoryToRegisterPass());
        fpm.add(llvm::createInstructionCombiningPass());
        fpm.add(llvm::createReassociatePass());
        fpm.add(llvm::createGVNPass());
        fpm.add(llvm::createCFGSimplificationPass());
        fpm.doInitialization();
        fpm.run(*fn);
        fpm.doFinalization();
    }

    std::string engineError;
    llvm::EngineBuilder engineBuilder(std::move(mod));
    engineBuilder.setErrorStr(&engineError);
    engineBuilder.setEngineKind(llvm::EngineKind::JIT);
    engineBuilder.setOptLevel(llvm::CodeGenOptLevel::Aggressive);
    llvm::ExecutionEngine* engine = engineBuilder.create();
    if (!engine) {
        failedCompilations_.insert(func.name);
        return false;
    }

    engine->finalizeObject();
    uint64_t fnAddr = engine->getFunctionAddress(jitName);
    if (!fnAddr) {
        delete engine;
        failedCompilations_.insert(func.name);
        return false;
    }

    compiledFloat_[func.name] = reinterpret_cast<JITFloatFnPtr>(fnAddr);
    modules_.push_back({std::move(ctx),
                        std::unique_ptr<llvm::ExecutionEngine>(engine)});
    return true;
}

} // namespace omscript
