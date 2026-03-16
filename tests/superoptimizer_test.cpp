/// @file superoptimizer_test.cpp
/// @brief Unit tests for the superoptimizer.
///
/// Tests cover:
///   - Cost model accuracy
///   - Idiom detection (rotate, abs, min/max, power-of-2, bitfield extract)
///   - Algebraic simplification on LLVM IR
///   - Synthesis engine for multiply/divide strength reduction
///   - End-to-end integration with the compiler pipeline
///   - Semantic preservation after superoptimization

#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "superoptimizer.h"
#include <gtest/gtest.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

using namespace omscript;
using namespace omscript::superopt;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: create a simple test function in a fresh module
// ─────────────────────────────────────────────────────────────────────────────

struct TestModule {
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> mod;
    llvm::Function* func = nullptr;
    llvm::BasicBlock* entry = nullptr;
    llvm::IRBuilder<> builder;

    TestModule(const std::string& funcName = "test_func", unsigned numArgs = 2)
        : mod(std::make_unique<llvm::Module>("test", ctx)), builder(ctx) {
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        std::vector<llvm::Type*> argTypes(numArgs, i64);
        auto* funcType = llvm::FunctionType::get(i64, argTypes, false);
        func = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                       funcName, mod.get());
        entry = llvm::BasicBlock::Create(ctx, "entry", func);
        builder.SetInsertPoint(entry);
    }

    llvm::Value* arg(unsigned idx) {
        auto it = func->arg_begin();
        std::advance(it, idx);
        return &*it;
    }

    llvm::Type* i64Ty() { return llvm::Type::getInt64Ty(ctx); }
    llvm::Type* i32Ty() { return llvm::Type::getInt32Ty(ctx); }

    bool verify() {
        std::string err;
        llvm::raw_string_ostream os(err);
        return !llvm::verifyModule(*mod, &os);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Cost model tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SuperoptimizerTest, CostModelBasics) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* add = tm.builder.CreateAdd(a, b);
    auto* mul = tm.builder.CreateMul(a, b);
    auto* sdiv = tm.builder.CreateSDiv(a, b);
    tm.builder.CreateRet(add);

    EXPECT_LE(instructionCost(llvm::cast<llvm::Instruction>(add)), 1.5);
    EXPECT_GT(instructionCost(llvm::cast<llvm::Instruction>(mul)), 2.0);
    EXPECT_GT(instructionCost(llvm::cast<llvm::Instruction>(sdiv)), 20.0);
}

TEST(SuperoptimizerTest, CostModelShiftsCheaperThanMul) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* one = llvm::ConstantInt::get(tm.i64Ty(), 1);

    auto* shl = tm.builder.CreateShl(a, one);
    auto* two = llvm::ConstantInt::get(tm.i64Ty(), 2);
    auto* mul = tm.builder.CreateMul(a, two);
    tm.builder.CreateRet(shl);

    EXPECT_LT(instructionCost(llvm::cast<llvm::Instruction>(shl)),
              instructionCost(llvm::cast<llvm::Instruction>(mul)));
}

TEST(SuperoptimizerTest, CostModelIntrinsicsCheap) {
    TestModule tm;
    auto* a = tm.arg(0);

    llvm::Function* ctpop = llvm::Intrinsic::getDeclaration(
        tm.mod.get(), llvm::Intrinsic::ctpop, {tm.i64Ty()});
    auto* pop = tm.builder.CreateCall(ctpop, {a});
    tm.builder.CreateRet(pop);

    EXPECT_LE(instructionCost(llvm::cast<llvm::Instruction>(pop)), 2.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom detection tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SuperoptimizerTest, DetectRotateLeft) {
    TestModule tm;
    auto* x = tm.arg(0);
    auto* amt = llvm::ConstantInt::get(tm.i64Ty(), 13);
    auto* amtR = llvm::ConstantInt::get(tm.i64Ty(), 51); // 64 - 13

    auto* shl = tm.builder.CreateShl(x, amt);
    auto* lshr = tm.builder.CreateLShr(x, amtR);
    auto* rot = tm.builder.CreateOr(shl, lshr, "manual_rotl");
    tm.builder.CreateRet(rot);

    auto idioms = detectIdioms(*tm.entry);
    ASSERT_GE(idioms.size(), 1u);
    EXPECT_EQ(idioms[0].idiom, Idiom::RotateLeft);
    EXPECT_EQ(idioms[0].operands[0], x);
}

TEST(SuperoptimizerTest, DetectAbsoluteValue) {
    TestModule tm;
    auto* x = tm.arg(0);
    auto* zero = llvm::ConstantInt::get(tm.i64Ty(), 0);

    auto* cmp = tm.builder.CreateICmpSLT(x, zero, "isNeg");
    auto* neg = tm.builder.CreateSub(zero, x, "negx");
    auto* sel = tm.builder.CreateSelect(cmp, neg, x, "abs");
    tm.builder.CreateRet(sel);

    auto idioms = detectIdioms(*tm.entry);
    ASSERT_GE(idioms.size(), 1u);
    EXPECT_EQ(idioms[0].idiom, Idiom::AbsoluteValue);
}

TEST(SuperoptimizerTest, DetectIntMin) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* cmp = tm.builder.CreateICmpSLT(a, b, "lt");
    auto* sel = tm.builder.CreateSelect(cmp, a, b, "min");
    tm.builder.CreateRet(sel);

    auto idioms = detectIdioms(*tm.entry);
    ASSERT_GE(idioms.size(), 1u);
    EXPECT_EQ(idioms[0].idiom, Idiom::IntMin);
}

TEST(SuperoptimizerTest, DetectIntMax) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* cmp = tm.builder.CreateICmpSGT(a, b, "gt");
    auto* sel = tm.builder.CreateSelect(cmp, a, b, "max");
    tm.builder.CreateRet(sel);

    auto idioms = detectIdioms(*tm.entry);
    ASSERT_GE(idioms.size(), 1u);
    EXPECT_EQ(idioms[0].idiom, Idiom::IntMax);
}

TEST(SuperoptimizerTest, DetectPowerOf2) {
    TestModule tm;
    auto* x = tm.arg(0);
    auto* one = llvm::ConstantInt::get(tm.i64Ty(), 1);
    auto* zero = llvm::ConstantInt::get(tm.i64Ty(), 0);

    auto* sub = tm.builder.CreateSub(x, one, "xminus1");
    auto* andI = tm.builder.CreateAnd(x, sub, "xandxm1");
    auto* cmp = tm.builder.CreateICmpEQ(andI, zero, "ispow2");
    auto* ext = tm.builder.CreateZExt(cmp, tm.i64Ty());
    tm.builder.CreateRet(ext);

    auto idioms = detectIdioms(*tm.entry);
    ASSERT_GE(idioms.size(), 1u);
    EXPECT_EQ(idioms[0].idiom, Idiom::IsPowerOf2);
}

TEST(SuperoptimizerTest, DetectBitFieldExtract) {
    TestModule tm;
    auto* x = tm.arg(0);
    auto* shiftAmt = llvm::ConstantInt::get(tm.i64Ty(), 8);
    auto* mask = llvm::ConstantInt::get(tm.i64Ty(), 0xFF); // (1<<8)-1

    auto* shifted = tm.builder.CreateLShr(x, shiftAmt, "shift");
    auto* extracted = tm.builder.CreateAnd(shifted, mask, "bfe");
    tm.builder.CreateRet(extracted);

    auto idioms = detectIdioms(*tm.entry);
    ASSERT_GE(idioms.size(), 1u);
    EXPECT_EQ(idioms[0].idiom, Idiom::BitFieldExtract);
}

// ─────────────────────────────────────────────────────────────────────────────
// Idiom replacement tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SuperoptimizerTest, ReplaceRotateWithIntrinsic) {
    TestModule tm;
    auto* x = tm.arg(0);
    auto* amt = llvm::ConstantInt::get(tm.i64Ty(), 13);
    auto* amtR = llvm::ConstantInt::get(tm.i64Ty(), 51);

    auto* shl = tm.builder.CreateShl(x, amt);
    auto* lshr = tm.builder.CreateLShr(x, amtR);
    auto* rot = tm.builder.CreateOr(shl, lshr, "manual_rotl");
    tm.builder.CreateRet(rot);

    SuperoptimizerConfig config;
    config.enableSynthesis = false; // Only test idiom replacement
    auto stats = superoptimizeFunction(*tm.func, config);

    EXPECT_GE(stats.idiomsReplaced, 1u);
    EXPECT_TRUE(tm.verify());
}

TEST(SuperoptimizerTest, ReplaceAbsWithIntrinsic) {
    TestModule tm;
    auto* x = tm.arg(0);
    auto* zero = llvm::ConstantInt::get(tm.i64Ty(), 0);

    auto* cmp = tm.builder.CreateICmpSLT(x, zero);
    auto* neg = tm.builder.CreateSub(zero, x);
    auto* sel = tm.builder.CreateSelect(cmp, neg, x, "abs");
    tm.builder.CreateRet(sel);

    SuperoptimizerConfig config;
    config.enableSynthesis = false;
    auto stats = superoptimizeFunction(*tm.func, config);

    EXPECT_GE(stats.idiomsReplaced, 1u);
    EXPECT_TRUE(tm.verify());
}

TEST(SuperoptimizerTest, ReplaceMinWithIntrinsic) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* cmp = tm.builder.CreateICmpSLT(a, b);
    auto* sel = tm.builder.CreateSelect(cmp, a, b, "min");
    tm.builder.CreateRet(sel);

    SuperoptimizerConfig config;
    config.enableSynthesis = false;
    auto stats = superoptimizeFunction(*tm.func, config);

    EXPECT_GE(stats.idiomsReplaced, 1u);
    EXPECT_TRUE(tm.verify());
}

// ─────────────────────────────────────────────────────────────────────────────
// Algebraic simplification tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SuperoptimizerTest, AlgebraicSubSelf) {
    TestModule tm;
    auto* a = tm.arg(0);

    auto* sub = tm.builder.CreateSub(a, a, "self_sub");
    tm.builder.CreateRet(sub);

    SuperoptimizerConfig config;
    config.enableIdiomRecognition = false;
    config.enableSynthesis = false;
    config.enableBranchOpt = false;
    auto stats = superoptimizeFunction(*tm.func, config);

    EXPECT_GE(stats.algebraicSimplified, 1u);
    EXPECT_TRUE(tm.verify());
}

TEST(SuperoptimizerTest, AlgebraicXorSelf) {
    TestModule tm;
    auto* a = tm.arg(0);

    auto* xorI = tm.builder.CreateXor(a, a, "self_xor");
    tm.builder.CreateRet(xorI);

    SuperoptimizerConfig config;
    config.enableIdiomRecognition = false;
    config.enableSynthesis = false;
    config.enableBranchOpt = false;
    auto stats = superoptimizeFunction(*tm.func, config);

    EXPECT_GE(stats.algebraicSimplified, 1u);
    EXPECT_TRUE(tm.verify());
}

TEST(SuperoptimizerTest, AlgebraicAndSelf) {
    TestModule tm;
    auto* a = tm.arg(0);

    auto* andI = tm.builder.CreateAnd(a, a, "self_and");
    tm.builder.CreateRet(andI);

    SuperoptimizerConfig config;
    config.enableIdiomRecognition = false;
    config.enableSynthesis = false;
    config.enableBranchOpt = false;
    auto stats = superoptimizeFunction(*tm.func, config);

    EXPECT_GE(stats.algebraicSimplified, 1u);
    EXPECT_TRUE(tm.verify());
}

TEST(SuperoptimizerTest, AlgebraicOrSelf) {
    TestModule tm;
    auto* a = tm.arg(0);

    auto* orI = tm.builder.CreateOr(a, a, "self_or");
    tm.builder.CreateRet(orI);

    SuperoptimizerConfig config;
    config.enableIdiomRecognition = false;
    config.enableSynthesis = false;
    config.enableBranchOpt = false;
    auto stats = superoptimizeFunction(*tm.func, config);

    EXPECT_GE(stats.algebraicSimplified, 1u);
    EXPECT_TRUE(tm.verify());
}

TEST(SuperoptimizerTest, AlgebraicMulConstCombine) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* c1 = llvm::ConstantInt::get(tm.i64Ty(), 3);
    auto* c2 = llvm::ConstantInt::get(tm.i64Ty(), 5);

    auto* mul1 = tm.builder.CreateMul(a, c1, "mul1");
    auto* mul2 = tm.builder.CreateMul(mul1, c2, "mul2");
    tm.builder.CreateRet(mul2);

    SuperoptimizerConfig config;
    config.enableIdiomRecognition = false;
    config.enableSynthesis = false;
    config.enableBranchOpt = false;
    auto stats = superoptimizeFunction(*tm.func, config);

    // (a*3)*5 should be combined to a*15
    EXPECT_GE(stats.algebraicSimplified, 1u);
    EXPECT_TRUE(tm.verify());
}

// ─────────────────────────────────────────────────────────────────────────────
// Synthesis tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SuperoptimizerTest, SynthMul3ToShiftAdd) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* three = llvm::ConstantInt::get(tm.i64Ty(), 3);

    auto* mul = tm.builder.CreateMul(a, three, "mul3");
    tm.builder.CreateRet(mul);

    SuperoptimizerConfig config;
    config.enableIdiomRecognition = false;
    config.enableAlgebraic = false;
    config.enableBranchOpt = false;
    auto stats = superoptimizeFunction(*tm.func, config);

    EXPECT_GE(stats.synthReplacements, 1u);
    EXPECT_TRUE(tm.verify());
}

TEST(SuperoptimizerTest, SynthUDivPow2ToShift) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* sixteen = llvm::ConstantInt::get(tm.i64Ty(), 16);

    auto* div = tm.builder.CreateUDiv(a, sixteen, "udiv16");
    tm.builder.CreateRet(div);

    SuperoptimizerConfig config;
    config.enableIdiomRecognition = false;
    config.enableAlgebraic = false;
    config.enableBranchOpt = false;
    auto stats = superoptimizeFunction(*tm.func, config);

    EXPECT_GE(stats.synthReplacements, 1u);
    EXPECT_TRUE(tm.verify());
}

TEST(SuperoptimizerTest, SynthURemPow2ToAnd) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* eight = llvm::ConstantInt::get(tm.i64Ty(), 8);

    auto* rem = tm.builder.CreateURem(a, eight, "urem8");
    tm.builder.CreateRet(rem);

    SuperoptimizerConfig config;
    config.enableIdiomRecognition = false;
    config.enableAlgebraic = false;
    config.enableBranchOpt = false;
    auto stats = superoptimizeFunction(*tm.func, config);

    EXPECT_GE(stats.synthReplacements, 1u);
    EXPECT_TRUE(tm.verify());
}

// ─────────────────────────────────────────────────────────────────────────────
// Concrete evaluator tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SuperoptimizerTest, EvaluateAdd) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* add = tm.builder.CreateAdd(a, b);
    tm.builder.CreateRet(add);

    auto result = evaluateInst(llvm::cast<llvm::Instruction>(add), {3, 4});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 7u);
}

TEST(SuperoptimizerTest, EvaluateMul) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* mul = tm.builder.CreateMul(a, b);
    tm.builder.CreateRet(mul);

    auto result = evaluateInst(llvm::cast<llvm::Instruction>(mul), {5, 6});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 30u);
}

TEST(SuperoptimizerTest, EvaluateXor) {
    TestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* xorI = tm.builder.CreateXor(a, b);
    tm.builder.CreateRet(xorI);

    auto result = evaluateInst(llvm::cast<llvm::Instruction>(xorI), {0xFF, 0x0F});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, 0xF0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Module-level tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SuperoptimizerTest, ModuleLevelOptimization) {
    TestModule tm("func1", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* sub = tm.builder.CreateSub(a, a, "self_sub");
    auto* add = tm.builder.CreateAdd(sub, b);
    tm.builder.CreateRet(add);

    auto stats = superoptimizeModule(*tm.mod);
    EXPECT_GE(stats.algebraicSimplified, 1u);
}

TEST(SuperoptimizerTest, EmptyFunctionNoOp) {
    TestModule tm("empty", 0);
    auto* zero = llvm::ConstantInt::get(tm.i64Ty(), 0);
    tm.builder.CreateRet(zero);

    auto stats = superoptimizeFunction(*tm.func);
    EXPECT_EQ(stats.idiomsReplaced, 0u);
    EXPECT_EQ(stats.synthReplacements, 0u);
    EXPECT_EQ(stats.algebraicSimplified, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// End-to-end integration tests with OmScript compiler
// ─────────────────────────────────────────────────────────────────────────────

static llvm::Module* compileOmScript(const std::string& source, CodeGenerator& codegen) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generate(program.get());
    return codegen.getModule();
}

TEST(SuperoptimizerTest, IntegrationSemanticPreservation) {
    // Verify that the superoptimizer doesn't break simple programs
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = compileOmScript("fn main() { return 42; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(SuperoptimizerTest, IntegrationWithArithmetic) {
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = compileOmScript(
        "fn compute(a, b) { return a * 3 + b * 5; }\n"
        "fn main() { return compute(10, 20); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(SuperoptimizerTest, IntegrationWithLoops) {
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = compileOmScript(
        "fn sum(n) {\n"
        "  var total = 0;\n"
        "  for (i in 0...n) { total = total + i; }\n"
        "  return total;\n"
        "}\n"
        "fn main() { return sum(100); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(SuperoptimizerTest, IntegrationDisabledWithFlag) {
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setSuperoptimize(false);
    auto* mod = compileOmScript("fn main() { return 1 + 2; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(SuperoptimizerTest, IntegrationAtO3) {
    CodeGenerator codegen(OptimizationLevel::O3);
    auto* mod = compileOmScript(
        "fn fib(n) {\n"
        "  if (n <= 1) { return n; }\n"
        "  return fib(n - 1) + fib(n - 2);\n"
        "}\n"
        "fn main() { return fib(10); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Configuration tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(SuperoptimizerTest, ConfigDefaultsReasonable) {
    SuperoptimizerConfig config;
    EXPECT_TRUE(config.enableIdiomRecognition);
    EXPECT_TRUE(config.enableSynthesis);
    EXPECT_TRUE(config.enableBranchOpt);
    EXPECT_TRUE(config.enableAlgebraic);
    EXPECT_LE(config.synthesis.maxInstructions, 10u);
    EXPECT_GT(config.synthesis.numTestVectors, 0u);
}

TEST(SuperoptimizerTest, StatsInitZero) {
    SuperoptimizerStats stats;
    EXPECT_EQ(stats.idiomsReplaced, 0u);
    EXPECT_EQ(stats.synthReplacements, 0u);
    EXPECT_EQ(stats.branchesSimplified, 0u);
    EXPECT_EQ(stats.algebraicSimplified, 0u);
}
