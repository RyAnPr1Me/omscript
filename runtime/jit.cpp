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
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/GVN.h>
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
    if (offset + 2 > code.size())
        return 0;
    return static_cast<uint16_t>(code[offset]) | (static_cast<uint16_t>(code[offset + 1]) << 8);
}

static int64_t peekInt(const std::vector<uint8_t>& code, size_t offset) {
    if (offset + 8 > code.size())
        return 0;
    uint64_t raw = 0;
    for (int i = 0; i < 8; i++) {
        raw |= static_cast<uint64_t>(code[offset + i]) << (i * 8);
    }
    int64_t value;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

static double peekFloat(const std::vector<uint8_t>& code, size_t offset) {
    if (offset + 8 > code.size())
        return 0.0;
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
    if (it != callCounts_.end())
        return it->second;
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
        if (oldIntPtr)
            compiled_[func.name] = oldIntPtr;
        if (oldFloatPtr)
            compiledFloat_[func.name] = oldFloatPtr;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// compile() — translate bytecode → LLVM IR → native code
// ---------------------------------------------------------------------------

bool BytecodeJIT::compile(const BytecodeFunction& func, JITSpecialization spec) {
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
            ip += 1 + 8; // rd + value
            break;
        case OpCode::POP:
        case OpCode::DUP:
            break;
        case OpCode::ADD:
        case OpCode::SUB:
        case OpCode::MUL:
        case OpCode::DIV:
        case OpCode::MOD:
        case OpCode::EQ:
        case OpCode::NE:
        case OpCode::LT:
        case OpCode::LE:
        case OpCode::GT:
        case OpCode::GE:
        case OpCode::AND:
        case OpCode::OR:
        case OpCode::BIT_AND:
        case OpCode::BIT_OR:
        case OpCode::BIT_XOR:
        case OpCode::SHL:
        case OpCode::SHR:
            ip += 3; // rd, rs1, rs2
            break;
        case OpCode::NEG:
        case OpCode::NOT:
        case OpCode::BIT_NOT:
        case OpCode::MOV:
            ip += 2; // rd, rs
            break;
        case OpCode::LOAD_LOCAL: {
            ip++; // rd
            uint8_t idx = code[ip];
            ip++; // idx
            if (idx > maxLocalIdx)
                maxLocalIdx = idx;
            break;
        }
        case OpCode::STORE_LOCAL: {
            uint8_t idx = code[ip];
            ip++; // idx
            ip++; // rs
            if (idx > maxLocalIdx)
                maxLocalIdx = idx;
            break;
        }
        case OpCode::JUMP: {
            uint16_t target = peekShort(code, ip);
            ip += 2;
            if (target > code.size()) {
                failedCompilations_.insert(func.name);
                return false;
            }
            blockStarts.insert(target);
            blockStarts.insert(ip);
            break;
        }
        case OpCode::JUMP_IF_FALSE: {
            ip++; // rs
            uint16_t target = peekShort(code, ip);
            ip += 2;
            if (target > code.size()) {
                failedCompilations_.insert(func.name);
                return false;
            }
            blockStarts.insert(target);
            blockStarts.insert(ip);
            break;
        }
        case OpCode::RETURN:
            ip++; // rs
            blockStarts.insert(ip);
            break;
        case OpCode::PRINT:
            ip++; // rs
            // fall through to unsupported
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

    auto* int64Ty = llvm::Type::getInt64Ty(*ctx);
    auto* int32Ty = llvm::Type::getInt32Ty(*ctx);
    auto* int64PtrTy = llvm::PointerType::getUnqual(int64Ty);

    llvm::FunctionType* fnType = llvm::FunctionType::get(int64Ty, {int64PtrTy, int32Ty}, false);

    std::string jitName = "jit_" + func.name;
    llvm::Function* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, jitName, mod.get());

    llvm::Argument* argsPtr = fn->arg_begin();
    argsPtr->setName("args");

    // --- Create basic blocks ---
    std::map<size_t, llvm::BasicBlock*> blocks;
    for (size_t start : blockStarts) {
        if (start <= code.size()) {
            blocks[start] = llvm::BasicBlock::Create(*ctx, "bb_" + std::to_string(start), fn);
        }
    }

    // --- Entry block: allocate locals & copy arguments ---
    llvm::BasicBlock* entryBB = llvm::BasicBlock::Create(*ctx, "entry", fn, blocks.begin()->second);
    builder.SetInsertPoint(entryBB);

    size_t numLocals = maxLocalIdx + 1;
    std::vector<llvm::AllocaInst*> locals(numLocals);
    for (size_t i = 0; i < numLocals; i++) {
        locals[i] = builder.CreateAlloca(int64Ty, nullptr, "local_" + std::to_string(i));
    }

    for (size_t i = 0; i < func.arity; i++) {
        auto* idx = llvm::ConstantInt::get(int32Ty, i);
        auto* ptr = builder.CreateGEP(int64Ty, argsPtr, idx, "arg_ptr_" + std::to_string(i));
        auto* val = builder.CreateLoad(int64Ty, ptr, "arg_" + std::to_string(i));
        builder.CreateStore(val, locals[i]);
    }
    for (size_t i = func.arity; i < numLocals; i++) {
        builder.CreateStore(llvm::ConstantInt::get(int64Ty, 0), locals[i]);
    }
    builder.CreateBr(blocks.begin()->second);

    // Declare abort() for division-by-zero guards.
    llvm::FunctionType* abortType = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx), false);
    llvm::FunctionCallee abortFn = mod->getOrInsertFunction("abort", abortType);

    // ------------------------------------------------------------------
    // Phase 3: Translate each basic block (register-based)
    // ------------------------------------------------------------------
    // Allocate alloca-based register storage. LLVM mem2reg will promote to SSA.
    static constexpr size_t kJITMaxRegs = 256;
    std::vector<llvm::AllocaInst*> regAlloc(kJITMaxRegs);
    {
        llvm::IRBuilder<> allocBuilder(entryBB, entryBB->begin());
        for (size_t i = 0; i < kJITMaxRegs; i++) {
            regAlloc[i] = allocBuilder.CreateAlloca(int64Ty, nullptr, "r" + std::to_string(i));
            allocBuilder.CreateStore(llvm::ConstantInt::get(int64Ty, 0), regAlloc[i]);
        }
    }

    for (auto blockIt = blocks.begin(); blockIt != blocks.end(); ++blockIt) {
        size_t blockStart = blockIt->first;
        llvm::BasicBlock* bb = blockIt->second;
        if (blockStart >= code.size())
            continue;

        auto nextIt = std::next(blockIt);
        size_t blockEnd = (nextIt != blocks.end()) ? nextIt->first : code.size();

        builder.SetInsertPoint(bb);
        llvm::BasicBlock* currentBB = bb;

        ip = blockStart;
        bool terminated = false;

        while (ip < blockEnd && !terminated) {
            auto op = static_cast<OpCode>(code[ip]);
            ip++;

            switch (op) {
            case OpCode::PUSH_INT: {
                uint8_t rd = code[ip++];
                int64_t val = peekInt(code, ip);
                ip += 8;
                builder.CreateStore(llvm::ConstantInt::get(int64Ty, val), regAlloc[rd]);
                break;
            }
            case OpCode::POP:
            case OpCode::DUP:
                break;
            case OpCode::MOV: {
                uint8_t rd = code[ip++];
                uint8_t rs = code[ip++];
                auto* val = builder.CreateLoad(int64Ty, regAlloc[rs], "mov");
                builder.CreateStore(val, regAlloc[rd]);
                break;
            }

#define JIT_REG_BINOP(NAME, CREATE)                                                                                    \
    case OpCode::NAME: {                                                                                               \
        uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];                                                   \
        auto* a = builder.CreateLoad(int64Ty, regAlloc[rs1], "a");                                                     \
        auto* b = builder.CreateLoad(int64Ty, regAlloc[rs2], "b");                                                     \
        builder.CreateStore(builder.CREATE(a, b, #NAME), regAlloc[rd]);                                                \
        break;                                                                                                         \
    }
                JIT_REG_BINOP(ADD, CreateNSWAdd)
                JIT_REG_BINOP(SUB, CreateNSWSub)
                JIT_REG_BINOP(MUL, CreateNSWMul)
#undef JIT_REG_BINOP

            case OpCode::DIV: {
                uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];
                auto* a = builder.CreateLoad(int64Ty, regAlloc[rs1], "a");
                auto* b = builder.CreateLoad(int64Ty, regAlloc[rs2], "b");
                auto* isZero = builder.CreateICmpEQ(b, llvm::ConstantInt::get(int64Ty, 0), "divzero");
                auto* errBB = llvm::BasicBlock::Create(*ctx, "diverr", fn);
                auto* okBB = llvm::BasicBlock::Create(*ctx, "divok", fn);
                builder.CreateCondBr(isZero, errBB, okBB);
                builder.SetInsertPoint(errBB);
                builder.CreateCall(abortFn);
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBB);
                currentBB = okBB;
                builder.CreateStore(builder.CreateSDiv(a, b, "div"), regAlloc[rd]);
                break;
            }
            case OpCode::MOD: {
                uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];
                auto* a = builder.CreateLoad(int64Ty, regAlloc[rs1], "a");
                auto* b = builder.CreateLoad(int64Ty, regAlloc[rs2], "b");
                auto* isZero = builder.CreateICmpEQ(b, llvm::ConstantInt::get(int64Ty, 0), "modzero");
                auto* errBB = llvm::BasicBlock::Create(*ctx, "moderr", fn);
                auto* okBB = llvm::BasicBlock::Create(*ctx, "modok", fn);
                builder.CreateCondBr(isZero, errBB, okBB);
                builder.SetInsertPoint(errBB);
                builder.CreateCall(abortFn);
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBB);
                currentBB = okBB;
                builder.CreateStore(builder.CreateSRem(a, b, "mod"), regAlloc[rd]);
                break;
            }
            case OpCode::NEG: {
                uint8_t rd = code[ip++], rs = code[ip++];
                auto* a = builder.CreateLoad(int64Ty, regAlloc[rs], "a");
                builder.CreateStore(builder.CreateNeg(a, "neg"), regAlloc[rd]);
                break;
            }

#define JIT_CMP_REG(NAME, PRED)                                                                                        \
    case OpCode::NAME: {                                                                                               \
        uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];                                                   \
        auto* a = builder.CreateLoad(int64Ty, regAlloc[rs1], "a");                                                     \
        auto* b = builder.CreateLoad(int64Ty, regAlloc[rs2], "b");                                                     \
        auto* cmp = builder.CreateICmp(llvm::CmpInst::PRED, a, b, #NAME);                                              \
        builder.CreateStore(builder.CreateZExt(cmp, int64Ty, #NAME "_i64"), regAlloc[rd]);                             \
        break;                                                                                                         \
    }
                JIT_CMP_REG(EQ, ICMP_EQ)
                JIT_CMP_REG(NE, ICMP_NE)
                JIT_CMP_REG(LT, ICMP_SLT)
                JIT_CMP_REG(LE, ICMP_SLE)
                JIT_CMP_REG(GT, ICMP_SGT)
                JIT_CMP_REG(GE, ICMP_SGE)
#undef JIT_CMP_REG

            case OpCode::AND: {
                uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];
                auto* a = builder.CreateLoad(int64Ty, regAlloc[rs1], "a");
                auto* b = builder.CreateLoad(int64Ty, regAlloc[rs2], "b");
                auto* boolA = builder.CreateICmpNE(a, llvm::ConstantInt::get(int64Ty, 0));
                auto* boolB = builder.CreateICmpNE(b, llvm::ConstantInt::get(int64Ty, 0));
                auto* r = builder.CreateAnd(boolA, boolB, "and");
                builder.CreateStore(builder.CreateZExt(r, int64Ty, "and_i64"), regAlloc[rd]);
                break;
            }
            case OpCode::OR: {
                uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];
                auto* a = builder.CreateLoad(int64Ty, regAlloc[rs1], "a");
                auto* b = builder.CreateLoad(int64Ty, regAlloc[rs2], "b");
                auto* boolA = builder.CreateICmpNE(a, llvm::ConstantInt::get(int64Ty, 0));
                auto* boolB = builder.CreateICmpNE(b, llvm::ConstantInt::get(int64Ty, 0));
                auto* r = builder.CreateOr(boolA, boolB, "or");
                builder.CreateStore(builder.CreateZExt(r, int64Ty, "or_i64"), regAlloc[rd]);
                break;
            }
            case OpCode::NOT: {
                uint8_t rd = code[ip++], rs = code[ip++];
                auto* a = builder.CreateLoad(int64Ty, regAlloc[rs], "a");
                auto* r = builder.CreateICmpEQ(a, llvm::ConstantInt::get(int64Ty, 0), "not");
                builder.CreateStore(builder.CreateZExt(r, int64Ty, "not_i64"), regAlloc[rd]);
                break;
            }

#define JIT_BIT_REG(NAME, CREATE)                                                                                      \
    case OpCode::NAME: {                                                                                               \
        uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];                                                   \
        auto* a = builder.CreateLoad(int64Ty, regAlloc[rs1], "a");                                                     \
        auto* b = builder.CreateLoad(int64Ty, regAlloc[rs2], "b");                                                     \
        builder.CreateStore(builder.CREATE(a, b, #NAME), regAlloc[rd]);                                                \
        break;                                                                                                         \
    }
                JIT_BIT_REG(BIT_AND, CreateAnd)
                JIT_BIT_REG(BIT_OR, CreateOr)
                JIT_BIT_REG(BIT_XOR, CreateXor)
                JIT_BIT_REG(SHL, CreateShl)
                JIT_BIT_REG(SHR, CreateAShr)
#undef JIT_BIT_REG

            case OpCode::BIT_NOT: {
                uint8_t rd = code[ip++], rs = code[ip++];
                auto* a = builder.CreateLoad(int64Ty, regAlloc[rs], "a");
                builder.CreateStore(builder.CreateNot(a, "bnot"), regAlloc[rd]);
                break;
            }

            case OpCode::LOAD_LOCAL: {
                uint8_t rd = code[ip++];
                uint8_t idx = code[ip++];
                if (idx >= numLocals) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                auto* val = builder.CreateLoad(int64Ty, locals[idx], "ld_" + std::to_string(idx));
                builder.CreateStore(val, regAlloc[rd]);
                break;
            }
            case OpCode::STORE_LOCAL: {
                uint8_t idx = code[ip++];
                uint8_t rs = code[ip++];
                if (idx >= numLocals) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                auto* val = builder.CreateLoad(int64Ty, regAlloc[rs], "st_val");
                builder.CreateStore(val, locals[idx]);
                break;
            }

            case OpCode::JUMP: {
                uint16_t target = peekShort(code, ip);
                ip += 2;
                auto it = blocks.find(target);
                if (it == blocks.end()) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                builder.CreateBr(it->second);
                terminated = true;
                break;
            }
            case OpCode::JUMP_IF_FALSE: {
                uint8_t rs = code[ip++];
                uint16_t target = peekShort(code, ip);
                ip += 2;
                auto* cond = builder.CreateLoad(int64Ty, regAlloc[rs], "cond_val");
                auto* condBool = builder.CreateICmpNE(cond, llvm::ConstantInt::get(int64Ty, 0), "cond");
                auto targetIt = blocks.find(target);
                if (targetIt == blocks.end()) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                auto fallIt = blocks.find(ip);
                if (fallIt == blocks.end()) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                builder.CreateCondBr(condBool, fallIt->second, targetIt->second);
                terminated = true;
                break;
            }
            case OpCode::RETURN: {
                uint8_t rs = code[ip++];
                auto* retVal = builder.CreateLoad(int64Ty, regAlloc[rs], "ret_val");
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
                builder.CreateBr(nextBBIt->second);
            } else {
                builder.CreateRet(llvm::ConstantInt::get(int64Ty, 0));
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
        errStream.flush();
        llvm::errs() << "JIT int verification failed for " << func.name << ": " << errStr << "\n";
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
    modules_.push_back({std::move(ctx), std::unique_ptr<llvm::ExecutionEngine>(engine)});
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
            ip += 1 + 8; // rd + value
            break;
        case OpCode::POP:
        case OpCode::DUP:
            break;
        case OpCode::ADD:
        case OpCode::SUB:
        case OpCode::MUL:
        case OpCode::DIV:
        case OpCode::MOD:
        case OpCode::EQ:
        case OpCode::NE:
        case OpCode::LT:
        case OpCode::LE:
        case OpCode::GT:
        case OpCode::GE:
            ip += 3; // rd, rs1, rs2
            break;
        case OpCode::NEG:
        case OpCode::NOT:
        case OpCode::MOV:
            ip += 2; // rd, rs
            break;
        case OpCode::LOAD_LOCAL: {
            ip++; // rd
            uint8_t idx = code[ip];
            ip++;
            if (idx > maxLocalIdx)
                maxLocalIdx = idx;
            break;
        }
        case OpCode::STORE_LOCAL: {
            uint8_t idx = code[ip];
            ip++;
            ip++; // rs
            if (idx > maxLocalIdx)
                maxLocalIdx = idx;
            break;
        }
        case OpCode::JUMP: {
            uint16_t target = peekShort(code, ip);
            ip += 2;
            if (target > code.size()) {
                failedCompilations_.insert(func.name);
                return false;
            }
            blockStarts.insert(target);
            blockStarts.insert(ip);
            break;
        }
        case OpCode::JUMP_IF_FALSE: {
            ip++; // rs
            uint16_t target = peekShort(code, ip);
            ip += 2;
            if (target > code.size()) {
                failedCompilations_.insert(func.name);
                return false;
            }
            blockStarts.insert(target);
            blockStarts.insert(ip);
            break;
        }
        case OpCode::RETURN:
            ip++; // rs
            blockStarts.insert(ip);
            break;
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

    auto* doubleTy = llvm::Type::getDoubleTy(*ctx);
    auto* int32Ty = llvm::Type::getInt32Ty(*ctx);
    auto* doublePtrTy = llvm::PointerType::getUnqual(doubleTy);

    llvm::FunctionType* fnType = llvm::FunctionType::get(doubleTy, {doublePtrTy, int32Ty}, false);

    std::string jitName = "jit_" + func.name + "_float";
    llvm::Function* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage, jitName, mod.get());

    llvm::Argument* argsPtr = fn->arg_begin();
    argsPtr->setName("args");

    // --- Create basic blocks ---
    std::map<size_t, llvm::BasicBlock*> blocks;
    for (size_t start : blockStarts) {
        if (start <= code.size()) {
            blocks[start] = llvm::BasicBlock::Create(*ctx, "bb_" + std::to_string(start), fn);
        }
    }

    // --- Entry block: allocate locals & copy arguments ---
    llvm::BasicBlock* entryBB = llvm::BasicBlock::Create(*ctx, "entry", fn, blocks.begin()->second);
    builder.SetInsertPoint(entryBB);

    size_t numLocals = maxLocalIdx + 1;
    std::vector<llvm::AllocaInst*> locals(numLocals);
    for (size_t i = 0; i < numLocals; i++) {
        locals[i] = builder.CreateAlloca(doubleTy, nullptr, "local_" + std::to_string(i));
    }

    for (size_t i = 0; i < func.arity; i++) {
        auto* idx = llvm::ConstantInt::get(int32Ty, i);
        auto* ptr = builder.CreateGEP(doubleTy, argsPtr, idx, "arg_ptr_" + std::to_string(i));
        auto* val = builder.CreateLoad(doubleTy, ptr, "arg_" + std::to_string(i));
        builder.CreateStore(val, locals[i]);
    }
    for (size_t i = func.arity; i < numLocals; i++) {
        builder.CreateStore(llvm::ConstantFP::get(doubleTy, 0.0), locals[i]);
    }
    builder.CreateBr(blocks.begin()->second);

    // Declare abort() for division-by-zero guards.
    llvm::FunctionType* abortType = llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx), false);
    llvm::FunctionCallee abortFn = mod->getOrInsertFunction("abort", abortType);

    // ------------------------------------------------------------------
    // Phase 3: Translate each basic block (double-typed arithmetic)
    // ------------------------------------------------------------------
    // Allocate alloca-based register storage for float JIT
    static constexpr size_t kJITMaxRegsF = 256;
    std::vector<llvm::AllocaInst*> regAlloc(kJITMaxRegsF);
    {
        llvm::IRBuilder<> allocBuilder(entryBB, entryBB->begin());
        for (size_t i = 0; i < kJITMaxRegsF; i++) {
            regAlloc[i] = allocBuilder.CreateAlloca(doubleTy, nullptr, "fr" + std::to_string(i));
            allocBuilder.CreateStore(llvm::ConstantFP::get(doubleTy, 0.0), regAlloc[i]);
        }
    }

    for (auto blockIt = blocks.begin(); blockIt != blocks.end(); ++blockIt) {
        size_t blockStart = blockIt->first;
        llvm::BasicBlock* bb = blockIt->second;
        if (blockStart >= code.size())
            continue;

        auto nextIt = std::next(blockIt);
        size_t blockEnd = (nextIt != blocks.end()) ? nextIt->first : code.size();

        builder.SetInsertPoint(bb);
        llvm::BasicBlock* currentBB = bb;

        ip = blockStart;
        bool terminated = false;

        while (ip < blockEnd && !terminated) {
            auto op = static_cast<OpCode>(code[ip]);
            ip++;

            switch (op) {
            case OpCode::PUSH_INT: {
                uint8_t rd = code[ip++];
                int64_t val = peekInt(code, ip);
                ip += 8;
                builder.CreateStore(llvm::ConstantFP::get(doubleTy, static_cast<double>(val)), regAlloc[rd]);
                break;
            }
            case OpCode::PUSH_FLOAT: {
                uint8_t rd = code[ip++];
                double val = peekFloat(code, ip);
                ip += 8;
                builder.CreateStore(llvm::ConstantFP::get(doubleTy, val), regAlloc[rd]);
                break;
            }
            case OpCode::POP:
            case OpCode::DUP:
                break;
            case OpCode::MOV: {
                uint8_t rd = code[ip++], rs = code[ip++];
                auto* val = builder.CreateLoad(doubleTy, regAlloc[rs], "fmov");
                builder.CreateStore(val, regAlloc[rd]);
                break;
            }

#define FJIT_BINOP(NAME, CREATE)                                                                                       \
    case OpCode::NAME: {                                                                                               \
        uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];                                                   \
        auto* a = builder.CreateLoad(doubleTy, regAlloc[rs1], "a");                                                    \
        auto* b = builder.CreateLoad(doubleTy, regAlloc[rs2], "b");                                                    \
        builder.CreateStore(builder.CREATE(a, b, "f" #NAME), regAlloc[rd]);                                            \
        break;                                                                                                         \
    }
                FJIT_BINOP(ADD, CreateFAdd)
                FJIT_BINOP(SUB, CreateFSub)
                FJIT_BINOP(MUL, CreateFMul)
#undef FJIT_BINOP

            case OpCode::DIV: {
                uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];
                auto* a = builder.CreateLoad(doubleTy, regAlloc[rs1], "a");
                auto* b = builder.CreateLoad(doubleTy, regAlloc[rs2], "b");
                auto* isZero = builder.CreateFCmpOEQ(b, llvm::ConstantFP::get(doubleTy, 0.0), "fdivzero");
                auto* errBB = llvm::BasicBlock::Create(*ctx, "fdiverr", fn);
                auto* okBB = llvm::BasicBlock::Create(*ctx, "fdivok", fn);
                builder.CreateCondBr(isZero, errBB, okBB);
                builder.SetInsertPoint(errBB);
                builder.CreateCall(abortFn);
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBB);
                currentBB = okBB;
                builder.CreateStore(builder.CreateFDiv(a, b, "fdiv"), regAlloc[rd]);
                break;
            }
            case OpCode::MOD: {
                uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];
                auto* a = builder.CreateLoad(doubleTy, regAlloc[rs1], "a");
                auto* b = builder.CreateLoad(doubleTy, regAlloc[rs2], "b");
                auto* isZero = builder.CreateFCmpOEQ(b, llvm::ConstantFP::get(doubleTy, 0.0), "fmodzero");
                auto* errBB = llvm::BasicBlock::Create(*ctx, "fmoderr", fn);
                auto* okBB = llvm::BasicBlock::Create(*ctx, "fmodok", fn);
                builder.CreateCondBr(isZero, errBB, okBB);
                builder.SetInsertPoint(errBB);
                builder.CreateCall(abortFn);
                builder.CreateUnreachable();
                builder.SetInsertPoint(okBB);
                currentBB = okBB;
                builder.CreateStore(builder.CreateFRem(a, b, "fmod"), regAlloc[rd]);
                break;
            }
            case OpCode::NEG: {
                uint8_t rd = code[ip++], rs = code[ip++];
                auto* a = builder.CreateLoad(doubleTy, regAlloc[rs], "a");
                builder.CreateStore(builder.CreateFNeg(a, "fneg"), regAlloc[rd]);
                break;
            }

#define FJIT_CMP(NAME, PRED)                                                                                           \
    case OpCode::NAME: {                                                                                               \
        uint8_t rd = code[ip++], rs1 = code[ip++], rs2 = code[ip++];                                                   \
        auto* a = builder.CreateLoad(doubleTy, regAlloc[rs1], "a");                                                    \
        auto* b = builder.CreateLoad(doubleTy, regAlloc[rs2], "b");                                                    \
        auto* cmp = builder.CreateFCmp(llvm::CmpInst::PRED, a, b, "f" #NAME);                                          \
        builder.CreateStore(builder.CreateUIToFP(cmp, doubleTy, "f" #NAME "_d"), regAlloc[rd]);                        \
        break;                                                                                                         \
    }
                FJIT_CMP(EQ, FCMP_OEQ)
                FJIT_CMP(NE, FCMP_ONE)
                FJIT_CMP(LT, FCMP_OLT)
                FJIT_CMP(LE, FCMP_OLE)
                FJIT_CMP(GT, FCMP_OGT)
                FJIT_CMP(GE, FCMP_OGE)
#undef FJIT_CMP

            case OpCode::NOT: {
                uint8_t rd = code[ip++], rs = code[ip++];
                auto* a = builder.CreateLoad(doubleTy, regAlloc[rs], "a");
                auto* isZero = builder.CreateFCmpOEQ(a, llvm::ConstantFP::get(doubleTy, 0.0), "fnot");
                builder.CreateStore(builder.CreateUIToFP(isZero, doubleTy, "fnot_d"), regAlloc[rd]);
                break;
            }

            case OpCode::LOAD_LOCAL: {
                uint8_t rd = code[ip++];
                uint8_t idx = code[ip++];
                if (idx >= numLocals) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                auto* val = builder.CreateLoad(doubleTy, locals[idx], "fld_" + std::to_string(idx));
                builder.CreateStore(val, regAlloc[rd]);
                break;
            }
            case OpCode::STORE_LOCAL: {
                uint8_t idx = code[ip++];
                uint8_t rs = code[ip++];
                if (idx >= numLocals) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                auto* val = builder.CreateLoad(doubleTy, regAlloc[rs], "fst_val");
                builder.CreateStore(val, locals[idx]);
                break;
            }

            case OpCode::JUMP: {
                uint16_t target = peekShort(code, ip);
                ip += 2;
                auto it = blocks.find(target);
                if (it == blocks.end()) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                builder.CreateBr(it->second);
                terminated = true;
                break;
            }
            case OpCode::JUMP_IF_FALSE: {
                uint8_t rs = code[ip++];
                uint16_t target = peekShort(code, ip);
                ip += 2;
                auto* cond = builder.CreateLoad(doubleTy, regAlloc[rs], "fcond_val");
                auto* condBool = builder.CreateFCmpONE(cond, llvm::ConstantFP::get(doubleTy, 0.0), "fcond");
                auto targetIt = blocks.find(target);
                if (targetIt == blocks.end()) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                auto fallIt = blocks.find(ip);
                if (fallIt == blocks.end()) {
                    failedCompilations_.insert(func.name);
                    return false;
                }
                builder.CreateCondBr(condBool, fallIt->second, targetIt->second);
                terminated = true;
                break;
            }
            case OpCode::RETURN: {
                uint8_t rs = code[ip++];
                auto* retVal = builder.CreateLoad(doubleTy, regAlloc[rs], "fret_val");
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
                builder.CreateBr(nextBBIt->second);
            } else {
                builder.CreateRet(llvm::ConstantFP::get(doubleTy, 0.0));
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
        errStream.flush();
        llvm::errs() << "JIT float verification failed for " << func.name << ": " << errStr << "\n";
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
    modules_.push_back({std::move(ctx), std::unique_ptr<llvm::ExecutionEngine>(engine)});
    return true;
}

} // namespace omscript
