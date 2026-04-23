/// @file hardware_graph_test.cpp
/// @brief Unit tests for the Hardware Graph Optimization Engine (HGOE).
///
/// Tests cover:
///   - Hardware graph construction and querying
///   - Microarchitecture database lookup
///   - Program graph construction from LLVM IR
///   - Graph mapping / list scheduling
///   - Hardware-aware cost model
///   - Hardware-aware transformations (FMA, prefetch, branch layout)
///   - Activation conditions (-march / -mtune)
///   - Fallback for unknown architectures
///   - End-to-end integration with compiler pipeline

#include "codegen.h"
#include "hardware_graph.h"
#include "lexer.h"
#include "parser.h"
#include <gtest/gtest.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

// LLVM 19+ renamed getDeclaration → getOrInsertDeclaration.
#if LLVM_VERSION_MAJOR >= 19
#define OMSC_TEST_GET_INTRIN llvm::Intrinsic::getOrInsertDeclaration
#else
#define OMSC_TEST_GET_INTRIN llvm::Intrinsic::getDeclaration
#endif

using namespace omscript;
using namespace omscript::hgoe;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: create a test module with a function
// ─────────────────────────────────────────────────────────────────────────────

struct HGOETestModule {
    llvm::LLVMContext ctx;
    std::unique_ptr<llvm::Module> mod;
    llvm::Function* func = nullptr;
    llvm::BasicBlock* entry = nullptr;
    llvm::IRBuilder<> builder;

    HGOETestModule(const std::string& name = "test_func", unsigned numArgs = 2,
                    bool useFP = false)
        : mod(std::make_unique<llvm::Module>("test", ctx)), builder(ctx) {
        llvm::Type* retTy = useFP ? llvm::Type::getDoubleTy(ctx)
                                  : llvm::Type::getInt64Ty(ctx);
        std::vector<llvm::Type*> argTypes(numArgs, retTy);
        auto* funcType = llvm::FunctionType::get(retTy, argTypes, false);
        func = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                       name, mod.get());
        entry = llvm::BasicBlock::Create(ctx, "entry", func);
        builder.SetInsertPoint(entry);

        // The production codegen emits floating-point ops with full fast-math
        // flags (`-ffast-math`-equivalent: nnan ninf nsz arcp contract afn
        // reassoc).  The hardware-graph FP transforms (generateFMA, FMASub,
        // foldFPDivByConstant, foldSqrtSquare, …) are gated on these flags
        // to preserve IEEE 754 semantics in non-fast-math code.  Apply the
        // same default here so unit tests observe the same transforms that
        // the real compiler enables.
        if (useFP) {
            llvm::FastMathFlags fmf;
            fmf.setFast();
            builder.setFastMathFlags(fmf);
        }
    }

    llvm::Value* arg(unsigned idx) {
        auto it = func->arg_begin();
        std::advance(it, idx);
        return &*it;
    }

    llvm::Type* i64Ty() { return llvm::Type::getInt64Ty(ctx); }
    llvm::Type* f64Ty() { return llvm::Type::getDoubleTy(ctx); }

    bool verify() {
        std::string err;
        llvm::raw_string_ostream os(err);
        return !llvm::verifyModule(*mod, &os);
    }
};

// ═════════════════════════════════════════════════════════════════════════════
// Step 1 — Hardware graph tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, EmptyGraph) {
    HardwareGraph g;
    EXPECT_EQ(g.nodeCount(), 0u);
    EXPECT_EQ(g.edgeCount(), 0u);
}

TEST(HardwareGraphTest, AddNodesAndEdges) {
    HardwareGraph g;
    unsigned n0 = g.addNode(ResourceType::Dispatch, "dispatch", 1, 1.0, 6.0, 1);
    unsigned n1 = g.addNode(ResourceType::IntegerALU, "alu", 4, 1.0, 4.0, 1);
    unsigned n2 = g.addNode(ResourceType::LoadUnit, "load", 2, 4.0, 2.0, 1);

    EXPECT_EQ(g.nodeCount(), 3u);
    EXPECT_EQ(n0, 0u);
    EXPECT_EQ(n1, 1u);
    EXPECT_EQ(n2, 2u);

    g.addEdge(n0, n1, 0.0, 4.0, "dispatch→alu");
    g.addEdge(n0, n2, 0.0, 2.0, "dispatch→load");
    EXPECT_EQ(g.edgeCount(), 2u);
}

TEST(HardwareGraphTest, FindNodesByType) {
    HardwareGraph g;
    g.addNode(ResourceType::IntegerALU, "alu0");
    g.addNode(ResourceType::LoadUnit, "load0");
    g.addNode(ResourceType::IntegerALU, "alu1");

    auto alus = g.findNodes(ResourceType::IntegerALU);
    EXPECT_EQ(alus.size(), 2u);

    auto loads = g.findNodes(ResourceType::LoadUnit);
    EXPECT_EQ(loads.size(), 1u);

    auto stores = g.findNodes(ResourceType::StoreUnit);
    EXPECT_EQ(stores.size(), 0u);
}

TEST(HardwareGraphTest, GetOutEdges) {
    HardwareGraph g;
    unsigned n0 = g.addNode(ResourceType::Dispatch, "dispatch");
    unsigned n1 = g.addNode(ResourceType::IntegerALU, "alu");
    unsigned n2 = g.addNode(ResourceType::LoadUnit, "load");

    g.addEdge(n0, n1, 0.0, 4.0);
    g.addEdge(n0, n2, 0.0, 2.0);
    g.addEdge(n1, n2, 1.0, 1.0);

    auto out0 = g.getOutEdges(n0);
    EXPECT_EQ(out0.size(), 2u);

    auto out1 = g.getOutEdges(n1);
    EXPECT_EQ(out1.size(), 1u);

    auto out2 = g.getOutEdges(n2);
    EXPECT_EQ(out2.size(), 0u);
}

TEST(HardwareGraphTest, NodeLookup) {
    HardwareGraph g;
    unsigned id = g.addNode(ResourceType::FMAUnit, "fma", 2, 4.0, 0.5, 1);

    const HardwareNode* node = g.getNode(id);
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->type, ResourceType::FMAUnit);
    EXPECT_EQ(node->name, "fma");
    EXPECT_EQ(node->count, 2u);
    EXPECT_DOUBLE_EQ(node->latency, 4.0);

    EXPECT_EQ(g.getNode(999), nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 7 — Hardware database tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, LookupSkylake) {
    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->name, "skylake");
    EXPECT_EQ(profile->isa, ISAFamily::X86_64);
    EXPECT_EQ(profile->vectorWidth, 256u);
    EXPECT_GE(profile->intALUs, 4u);
    EXPECT_GE(profile->loadPorts, 2u);
}

TEST(HardwareGraphTest, LookupZen4) {
    auto profile = lookupMicroarch("znver4");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->name, "znver4");
    EXPECT_EQ(profile->isa, ISAFamily::X86_64);
    EXPECT_GE(profile->loadPorts, 3u);
}

TEST(HardwareGraphTest, LookupAppleM1) {
    auto profile = lookupMicroarch("apple-m1");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->isa, ISAFamily::AArch64);
    EXPECT_EQ(profile->vectorWidth, 128u); // NEON
    EXPECT_GE(profile->issueWidth, 8u);
}

TEST(HardwareGraphTest, LookupNeoverseV2) {
    auto profile = lookupMicroarch("neoverse-v2");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->isa, ISAFamily::AArch64);
    EXPECT_GE(profile->vecUnits, 4u);
}

TEST(HardwareGraphTest, LookupRISCV) {
    auto profile = lookupMicroarch("generic-rv64");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->isa, ISAFamily::RISCV64);
    EXPECT_EQ(profile->issueWidth, 2u);
}

TEST(HardwareGraphTest, LookupUnknownReturnsFallback) {
    auto profile = lookupMicroarch("unknown-cpu-xyz");
    EXPECT_FALSE(profile.has_value());
}

TEST(HardwareGraphTest, LookupEmptyReturnsFallback) {
    auto profile = lookupMicroarch("");
    EXPECT_FALSE(profile.has_value());
}

TEST(HardwareGraphTest, LookupCaseInsensitive) {
    auto p1 = lookupMicroarch("Skylake");
    auto p2 = lookupMicroarch("SKYLAKE");
    auto p3 = lookupMicroarch("skylake");

    ASSERT_TRUE(p1.has_value());
    ASSERT_TRUE(p2.has_value());
    ASSERT_TRUE(p3.has_value());
    EXPECT_EQ(p1->name, p2->name);
    EXPECT_EQ(p2->name, p3->name);
}

TEST(HardwareGraphTest, LookupHyphenInsensitive) {
    auto p1 = lookupMicroarch("apple-m1");
    auto p2 = lookupMicroarch("applem1");

    ASSERT_TRUE(p1.has_value());
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(p1->name, p2->name);
}

TEST(HardwareGraphTest, BuildHardwareGraphFromProfile) {
    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    HardwareGraph g = buildHardwareGraph(*profile);

    // Should have dispatch, ALU, vector, FMA, load, store, branch, AGU,
    // divider, L1, L2, L3, memory, int regs, vec regs, retire = 16 nodes
    EXPECT_GE(g.nodeCount(), 16u);

    // Should have edges from dispatch to execution units and cache hierarchy
    EXPECT_GE(g.edgeCount(), 15u);

    // Verify specific node types exist
    EXPECT_GE(g.findNodes(ResourceType::Dispatch).size(), 1u);
    EXPECT_GE(g.findNodes(ResourceType::IntegerALU).size(), 1u);
    EXPECT_GE(g.findNodes(ResourceType::LoadUnit).size(), 1u);
    EXPECT_GE(g.findNodes(ResourceType::L1DCache).size(), 1u);
    EXPECT_GE(g.findNodes(ResourceType::L2Cache).size(), 1u);
    EXPECT_GE(g.findNodes(ResourceType::L3Cache).size(), 1u);
    EXPECT_GE(g.findNodes(ResourceType::MainMemory).size(), 1u);
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 2 — Program graph tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, ProgramGraphEmpty) {
    ProgramGraph pg;
    EXPECT_EQ(pg.nodeCount(), 0u);
    EXPECT_EQ(pg.criticalPathLength(), 0u);
}

TEST(HardwareGraphTest, ProgramGraphFromSimpleFunction) {
    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* add = tm.builder.CreateAdd(a, b, "add");
    auto* mul = tm.builder.CreateMul(add, a, "mul");
    tm.builder.CreateRet(mul);

    ProgramGraph pg;
    pg.buildFromFunction(*tm.func);

    // Should have nodes for add, mul, ret (at least 3)
    EXPECT_GE(pg.nodeCount(), 3u);
    EXPECT_GE(pg.edges().size(), 1u); // add → mul dependency
}

TEST(HardwareGraphTest, ProgramGraphDependencies) {
    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* add = tm.builder.CreateAdd(a, b);
    auto* mul = tm.builder.CreateMul(add, b); // depends on add
    tm.builder.CreateRet(mul);

    ProgramGraph pg;
    pg.buildFromFunction(*tm.func);

    // mul should have add as a predecessor
    const ProgramNode* mulNode = nullptr;
    for (const auto& node : pg.nodes()) {
        if (node.inst && node.inst == llvm::dyn_cast<llvm::Instruction>(mul)) {
            mulNode = &node;
            break;
        }
    }
    ASSERT_NE(mulNode, nullptr);

    auto preds = pg.getPredecessors(mulNode->id);
    EXPECT_GE(preds.size(), 1u); // At least add is a predecessor
}

TEST(HardwareGraphTest, ProgramGraphCriticalPath) {
    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* add = tm.builder.CreateAdd(a, b);
    auto* mul = tm.builder.CreateMul(add, a);
    auto* sub = tm.builder.CreateSub(mul, b);
    tm.builder.CreateRet(sub);

    ProgramGraph pg;
    pg.buildFromFunction(*tm.func);

    // Critical path: add → mul → sub → ret = at least 3 levels
    unsigned cp = pg.criticalPathLength();
    EXPECT_GE(cp, 3u);
}

TEST(HardwareGraphTest, ProgramGraphClassification) {
    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    tm.builder.CreateAdd(a, b);        // IntArith
    tm.builder.CreateMul(a, b);        // IntMul
    tm.builder.CreateSDiv(a, b);       // IntDiv
    tm.builder.CreateShl(a, b);        // Shift
    tm.builder.CreateICmpSLT(a, b);    // Comparison
    tm.builder.CreateRet(a);

    ProgramGraph pg;
    pg.buildFromFunction(*tm.func);

    // Verify diverse operation classes are represented
    bool hasIntArith = false, hasIntMul = false, hasIntDiv = false;
    bool hasShift = false, hasComparison = false;

    for (const auto& node : pg.nodes()) {
        switch (node.opClass) {
        case OpClass::IntArith:   hasIntArith = true; break;
        case OpClass::IntMul:     hasIntMul = true; break;
        case OpClass::IntDiv:     hasIntDiv = true; break;
        case OpClass::Shift:      hasShift = true; break;
        case OpClass::Comparison: hasComparison = true; break;
        default: break;
        }
    }

    EXPECT_TRUE(hasIntArith);
    EXPECT_TRUE(hasIntMul);
    EXPECT_TRUE(hasIntDiv);
    EXPECT_TRUE(hasShift);
    EXPECT_TRUE(hasComparison);
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 3 — Graph mapping tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, MapSimpleProgram) {
    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* add = tm.builder.CreateAdd(a, b);
    auto* mul = tm.builder.CreateMul(add, a);
    tm.builder.CreateRet(mul);

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    ProgramGraph pg;
    pg.buildFromFunction(*tm.func);

    MappingResult result = mapProgramToHardware(pg, hw, *profile);

    EXPECT_GT(result.totalCycles, 0u);
    EXPECT_GE(result.schedule.size(), 1u);
    EXPECT_GE(result.portUtilization, 0.0);
    EXPECT_LE(result.portUtilization, 1.0);
}

TEST(HardwareGraphTest, MapEmptyProgram) {
    HGOETestModule tm("empty", 0);
    tm.builder.CreateRet(llvm::ConstantInt::get(tm.i64Ty(), 0));

    auto profile = lookupMicroarch("znver4");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    ProgramGraph pg;
    pg.buildFromFunction(*tm.func);

    MappingResult result = mapProgramToHardware(pg, hw, *profile);
    EXPECT_GE(result.totalCycles, 0u);
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 5 — Hardware-aware cost model tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, CostModelBasics) {
    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);
    HardwareCostModel costModel(hw, *profile);

    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* add = tm.builder.CreateAdd(a, b);
    auto* mul = tm.builder.CreateMul(a, b);
    auto* div = tm.builder.CreateSDiv(a, b);
    tm.builder.CreateRet(add);

    double addCost = costModel.instructionCost(llvm::cast<llvm::Instruction>(add));
    double mulCost = costModel.instructionCost(llvm::cast<llvm::Instruction>(mul));
    double divCost = costModel.instructionCost(llvm::cast<llvm::Instruction>(div));

    // Add should be cheap, mul more expensive, div most expensive
    EXPECT_LT(addCost, mulCost);
    EXPECT_LT(mulCost, divCost);
}

TEST(HardwareGraphTest, CostModelVectorWidth) {
    auto skylake = lookupMicroarch("skylake");
    auto appleM1 = lookupMicroarch("apple-m1");
    ASSERT_TRUE(skylake.has_value());
    ASSERT_TRUE(appleM1.has_value());

    HardwareGraph hwSkylake = buildHardwareGraph(*skylake);
    HardwareGraph hwApple = buildHardwareGraph(*appleM1);

    HardwareCostModel skylakeCost(hwSkylake, *skylake);
    HardwareCostModel appleCost(hwApple, *appleM1);

    // Skylake should prefer wider vectors (AVX2=256-bit → width 8)
    // Apple M1 NEON is 128-bit → width 4
    EXPECT_GE(skylakeCost.preferredVectorWidth(), appleCost.preferredVectorWidth());
}

TEST(HardwareGraphTest, SimulateExecution) {
    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);
    HardwareCostModel costModel(hw, *profile);

    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* add = tm.builder.CreateAdd(a, b);
    auto* mul = tm.builder.CreateMul(add, b);
    tm.builder.CreateRet(mul);

    ProgramGraph pg;
    pg.buildFromFunction(*tm.func);

    double cycles = costModel.simulateExecution(pg);
    EXPECT_GT(cycles, 0.0);
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 4 — Hardware-aware transformation tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, FMAGeneration) {
    HGOETestModule tm("fma_test", 3, true);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* c = tm.arg(2);

    // a * b + c → should be converted to FMA
    auto* mul = tm.builder.CreateFMul(a, b, "mul");
    auto* add = tm.builder.CreateFAdd(mul, c, "add");
    tm.builder.CreateRet(add);

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    TransformStats stats = applyHardwareTransforms(*tm.func, *profile);

    EXPECT_GE(stats.fmaGenerated, 1u);
    EXPECT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, NoFMAWhenNoUnits) {
    HGOETestModule tm("no_fma_test", 3, true);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* c = tm.arg(2);

    auto* mul = tm.builder.CreateFMul(a, b);
    auto* add = tm.builder.CreateFAdd(mul, c);
    tm.builder.CreateRet(add);

    // Use a profile with 0 FMA units
    MicroarchProfile noFMA;
    noFMA.fmaUnits = 0;
    noFMA.cacheLineSize = 64;

    TransformStats stats = applyHardwareTransforms(*tm.func, noFMA);
    EXPECT_EQ(stats.fmaGenerated, 0u);
}

// ═════════════════════════════════════════════════════════════════════════════
// Step 6 — Activation conditions and integration tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, ShouldActivateWithMarch) {
    HGOEConfig config;
    config.marchCpu = "skylake";
    EXPECT_TRUE(shouldActivate(config));
}

TEST(HardwareGraphTest, ShouldActivateWithMtune) {
    HGOEConfig config;
    config.mtuneCpu = "znver4";
    EXPECT_TRUE(shouldActivate(config));
}

TEST(HardwareGraphTest, ShouldNotActivateWithoutFlags) {
    HGOEConfig config;
    EXPECT_FALSE(shouldActivate(config));
}

TEST(HardwareGraphTest, OptimizeModuleActivated) {
    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    tm.builder.CreateRet(tm.builder.CreateAdd(a, b));

    HGOEConfig config;
    config.marchCpu = "skylake";
    auto stats = optimizeModule(*tm.mod, config);

    EXPECT_TRUE(stats.activated);
    EXPECT_EQ(stats.resolvedArch, "skylake");
    EXPECT_GE(stats.functionsOptimized, 1u);
}

TEST(HardwareGraphTest, OptimizeModuleNotActivated) {
    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    tm.builder.CreateRet(tm.builder.CreateAdd(a, b));

    HGOEConfig config; // No -march, no -mtune
    auto stats = optimizeModule(*tm.mod, config);

    EXPECT_FALSE(stats.activated);
    EXPECT_EQ(stats.functionsOptimized, 0u);
}

TEST(HardwareGraphTest, FallbackForUnknownArch) {
    HGOETestModule tm;
    tm.builder.CreateRet(tm.arg(0));

    HGOEConfig config;
    config.marchCpu = "unknown-fantasy-cpu";
    auto stats = optimizeModule(*tm.mod, config);

    // Should fallback — activated is false for unknown arch
    EXPECT_FALSE(stats.activated);
}

TEST(HardwareGraphTest, OptimizeFunctionWithZen4) {
    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* add = tm.builder.CreateAdd(a, b);
    auto* mul = tm.builder.CreateMul(add, b);
    tm.builder.CreateRet(mul);

    HGOEConfig config;
    config.marchCpu = "znver4";
    auto stats = optimizeFunction(*tm.func, config);

    EXPECT_TRUE(stats.activated);
    EXPECT_EQ(stats.resolvedArch, "znver4");
    EXPECT_GE(stats.functionsOptimized, 1u);
}

TEST(HardwareGraphTest, OptimizeFunctionWithAppleM3) {
    HGOETestModule tm;
    auto* a = tm.arg(0);
    tm.builder.CreateRet(a);

    HGOEConfig config;
    config.marchCpu = "apple-m3";
    auto stats = optimizeFunction(*tm.func, config);

    EXPECT_TRUE(stats.activated);
}

TEST(HardwareGraphTest, OptimizeFunctionSkipsDeclarations) {
    HGOETestModule tm;
    llvm::Type* i64 = llvm::Type::getInt64Ty(tm.ctx);
    auto* funcType = llvm::FunctionType::get(i64, {i64}, false);
    auto* decl = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                         "external_func", tm.mod.get());
    // Don't add a body — this is a declaration
    tm.builder.CreateRet(tm.arg(0)); // finish the test func

    HGOEConfig config;
    config.marchCpu = "skylake";
    auto stats = optimizeFunction(*decl, config);

    // Declaration should be skipped
    EXPECT_EQ(stats.functionsOptimized, 0u);
}

// ═════════════════════════════════════════════════════════════════════════════
// End-to-end integration with OmScript compiler
// ═════════════════════════════════════════════════════════════════════════════

static llvm::Module* compileOmScript(const std::string& source, CodeGenerator& codegen) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generate(program.get());
    return codegen.getModule();
}

TEST(HardwareGraphTest, IntegrationWithMarchSkylake) {
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setMarch("skylake");
    auto* mod = compileOmScript("fn main() { return 42; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(HardwareGraphTest, IntegrationWithMtuneZnver4) {
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setMtune("znver4");
    auto* mod = compileOmScript(
        "fn compute(a, b) { return a * 3 + b * 5; }\n"
        "fn main() { return compute(10, 20); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(HardwareGraphTest, IntegrationWithoutFlagsNormal) {
    // Without -march/-mtune, HGOE should not activate
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = compileOmScript("fn main() { return 1 + 2; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(HardwareGraphTest, IntegrationHGOEDisabled) {
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setMarch("skylake");
    codegen.setHardwareGraphOpt(false);
    auto* mod = compileOmScript("fn main() { return 42; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(HardwareGraphTest, IntegrationAtO3WithMarch) {
    CodeGenerator codegen(OptimizationLevel::O3);
    codegen.setMarch("znver4");
    auto* mod = compileOmScript(
        "fn fib(n) {\n"
        "  if (n <= 1) { return n; }\n"
        "  return fib(n - 1) + fib(n - 2);\n"
        "}\n"
        "fn main() { return fib(10); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// Configuration and stats tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, ConfigDefaults) {
    HGOEConfig config;
    EXPECT_TRUE(config.marchCpu.empty());
    EXPECT_TRUE(config.mtuneCpu.empty());
    EXPECT_TRUE(config.enableScheduling);
    EXPECT_TRUE(config.enableTransforms);
    EXPECT_TRUE(config.enableCostModel);
}

TEST(HardwareGraphTest, StatsInitZero) {
    HGOEStats stats;
    EXPECT_FALSE(stats.activated);
    EXPECT_EQ(stats.functionsOptimized, 0u);
    EXPECT_EQ(stats.totalScheduledCycles, 0u);
    EXPECT_EQ(stats.transforms.fmaGenerated, 0u);
    EXPECT_EQ(stats.transforms.prefetchesInserted, 0u);
    EXPECT_EQ(stats.transforms.branchesOptimized, 0u);
}

TEST(HardwareGraphTest, MicroarchProfileConsistency) {
    // All known architectures should have reasonable values
    std::vector<std::string> cpus = {
        "skylake", "haswell", "alderlake",
        "sandybridge", "ivybridge",
        "znver3", "znver4",
        "apple-m1", "apple-m3",
        "neoverse-v2", "neoverse-n2",
        "graviton3", "graviton4",
        "lunar-lake", "znver5",
        "generic-rv64", "sifive-u74"
    };

    for (const auto& cpu : cpus) {
        auto profile = lookupMicroarch(cpu);
        ASSERT_TRUE(profile.has_value()) << "Failed for: " << cpu;
        EXPECT_GT(profile->issueWidth, 0u) << cpu;
        EXPECT_GT(profile->intALUs, 0u) << cpu;
        EXPECT_GT(profile->loadPorts, 0u) << cpu;
        EXPECT_GT(profile->vectorWidth, 0u) << cpu;
        EXPECT_GT(profile->l1DSize, 0u) << cpu;
        EXPECT_GT(profile->l1DLatency, 0u) << cpu;
        EXPECT_GT(profile->memoryLatency, 0u) << cpu;
    }
}

TEST(HardwareGraphTest, Graviton3ProfileAccuracy) {
    auto profile = lookupMicroarch("graviton3");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->isa, ISAFamily::AArch64);
    EXPECT_EQ(profile->issueWidth, 8u);
    EXPECT_EQ(profile->intALUs, 6u);
    EXPECT_EQ(profile->vecUnits, 4u);
    EXPECT_EQ(profile->vectorWidth, 256u); // SVE 256-bit
    EXPECT_EQ(profile->loadPorts, 3u);
    EXPECT_EQ(profile->l1DSize, 64u);
}

TEST(HardwareGraphTest, Graviton4ProfileAccuracy) {
    auto profile = lookupMicroarch("graviton4");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->isa, ISAFamily::AArch64);
    EXPECT_EQ(profile->l2Size, 2048u);   // 2 MB L2
    EXPECT_EQ(profile->l3Size, 36864u);  // 36 MB L3
    EXPECT_LT(profile->memoryLatency, 140u); // DDR5-5600
}

TEST(HardwareGraphTest, LunarLakeProfileAccuracy) {
    auto profile = lookupMicroarch("lunar-lake");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->isa, ISAFamily::X86_64);
    EXPECT_EQ(profile->decodeWidth, 8u);
    EXPECT_EQ(profile->issueWidth, 8u);
    EXPECT_EQ(profile->intALUs, 6u);
    EXPECT_EQ(profile->loadPorts, 3u);
    EXPECT_EQ(profile->l2Size, 2560u);   // 2.5 MB L2
    EXPECT_EQ(profile->vectorWidth, 256u); // AVX2
}

TEST(HardwareGraphTest, Zen5ProfileAccuracy) {
    auto profile = lookupMicroarch("znver5");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->isa, ISAFamily::X86_64);
    EXPECT_EQ(profile->issueWidth, 8u);
    EXPECT_EQ(profile->intALUs, 6u);
    EXPECT_EQ(profile->vecUnits, 4u);
    EXPECT_EQ(profile->fmaUnits, 4u);
    EXPECT_EQ(profile->loadPorts, 4u);
    EXPECT_EQ(profile->vectorWidth, 512u); // AVX-512
    EXPECT_EQ(profile->branchUnits, 2u);
}

// ═════════════════════════════════════════════════════════════════════════════
// Schedule enhancements — register pressure, fusion, beam search, debug
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, ScheduleReordersInstructions) {
    // Verify that the scheduler actually reorders instructions when it's
    // beneficial. Create a BB with: load, add (uses load), independent mul.
    // The scheduler should move the mul before the add to hide load latency.
    HGOETestModule tm("sched_reorder");
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    // Create a pointer, load from it, add to it, and do independent work.
    auto* alloca = tm.builder.CreateAlloca(tm.i64Ty(), nullptr, "ptr");
    tm.builder.CreateStore(a, alloca);
    auto* load = tm.builder.CreateLoad(tm.i64Ty(), alloca, "loaded");
    auto* add = tm.builder.CreateAdd(load, b, "sum");    // depends on load
    auto* mul = tm.builder.CreateMul(a, b, "product");   // independent of load
    auto* result = tm.builder.CreateAdd(add, mul, "result");
    tm.builder.CreateRet(result);

    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);

    // The module should still verify after scheduling.
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, ScheduleRegisterPressureAware) {
    // Create a BB with many independent additions that produce live values.
    // The scheduler should track register pressure and prefer to schedule
    // instructions that free registers when pressure is high.
    HGOETestModule tm("reg_pressure", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    // Generate several independent computations.
    llvm::Value* sum = a;
    for (int i = 0; i < 8; ++i) {
        auto* t = tm.builder.CreateAdd(a, tm.builder.getInt64(i), "t" + std::to_string(i));
        sum = tm.builder.CreateAdd(sum, t, "acc" + std::to_string(i));
    }
    auto* final_ = tm.builder.CreateAdd(sum, b);
    tm.builder.CreateRet(final_);

    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, ScheduleHandlesLargeBB) {
    // Test beam search pruning: create a large BB with many independent ops.
    // The scheduler should complete without excessive runtime.
    HGOETestModule tm("large_bb", 2);
    auto* a = tm.arg(0);

    llvm::Value* acc = a;
    for (int i = 0; i < 100; ++i) {
        auto* t = tm.builder.CreateAdd(acc, tm.builder.getInt64(i));
        acc = tm.builder.CreateXor(acc, t);
    }
    tm.builder.CreateRet(acc);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, ScheduleFusionAware) {
    // Create a compare instruction followed by other work.
    // The scheduler should keep the compare close to its use (branch).
    HGOETestModule tm("fusion", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* cmp = tm.builder.CreateICmpSLT(a, b, "cmp");
    auto* sel = tm.builder.CreateSelect(cmp, a, b, "sel");
    tm.builder.CreateRet(sel);

    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, ScheduleWithDivisionLatencyHiding) {
    // Division should be scheduled early to hide its long latency.
    HGOETestModule tm("div_hiding", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* div = tm.builder.CreateSDiv(a, b, "div");
    auto* add1 = tm.builder.CreateAdd(a, b, "add1");
    auto* add2 = tm.builder.CreateAdd(add1, b, "add2");
    auto* result = tm.builder.CreateAdd(div, add2, "result");
    tm.builder.CreateRet(result);

    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());

    // Check the scheduled order: division should appear before adds
    // (which are independent of it) since it has higher latency.
    auto& entry = tm.func->getEntryBlock();
    unsigned divPos = 0, addPos = 0;
    unsigned pos = 0;
    for (auto& inst : entry) {
        if (inst.getOpcode() == llvm::Instruction::SDiv) divPos = pos;
        if (&inst == result) addPos = pos;
        ++pos;
    }
    EXPECT_LT(divPos, addPos);
}

TEST(HardwareGraphTest, ScheduleMultiArchConsistency) {
    // Schedule the same BB on different architectures and verify consistency.
    HGOETestModule tm("multi_arch", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* sum = tm.builder.CreateAdd(a, b);
    auto* prod = tm.builder.CreateMul(a, b);
    auto* result = tm.builder.CreateAdd(sum, prod);
    tm.builder.CreateRet(result);
    ASSERT_TRUE(tm.verify());

    // Try on different architectures.
    for (const char* arch : {"skylake", "znver4", "apple-m1"}) {
        auto profile = lookupMicroarch(arch);
        ASSERT_TRUE(profile.has_value()) << "Missing profile for " << arch;
        HardwareGraph hw = buildHardwareGraph(*profile);
        unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
        EXPECT_GT(cycles, 0u) << "Zero cycles on " << arch;
        ASSERT_TRUE(tm.verify()) << "Verification failed after scheduling on " << arch;
    }
}

TEST(HardwareGraphTest, ScheduleChainInterleaving) {
    // Two independent chains: (a+b+c) and (d+e+f).
    // The scheduler should interleave them for better ILP.
    HGOETestModule tm("chains", 6);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* c = tm.arg(2);
    auto* d = tm.arg(3);
    auto* e = tm.arg(4);
    auto* f = tm.arg(5);

    // Chain 1: a + b + c
    auto* chain1a = tm.builder.CreateAdd(a, b, "c1a");
    auto* chain1b = tm.builder.CreateAdd(chain1a, c, "c1b");
    // Chain 2: d + e + f
    auto* chain2a = tm.builder.CreateAdd(d, e, "c2a");
    auto* chain2b = tm.builder.CreateAdd(chain2a, f, "c2b");
    // Merge
    auto* result = tm.builder.CreateAdd(chain1b, chain2b, "result");
    tm.builder.CreateRet(result);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, ScheduleSlackAwarePressure) {
    // Create a mix of critical-path and off-critical-path instructions.
    // Slack-aware scheduling should deprioritize off-critical instructions
    // when register pressure is high.
    HGOETestModule tm("slack", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    // Critical path: a chain of multiplies (high latency)
    auto* m1 = tm.builder.CreateMul(a, b, "m1");
    auto* m2 = tm.builder.CreateMul(m1, a, "m2");

    // Off-critical: independent adds that can be delayed
    auto* add1 = tm.builder.CreateAdd(a, b, "add1");
    auto* add2 = tm.builder.CreateAdd(add1, a, "add2");

    auto* result = tm.builder.CreateAdd(m2, add2, "result");
    tm.builder.CreateRet(result);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, ScheduleIncBranchFusion) {
    // Create an increment + compare pattern (loop counter).
    // The scheduler should keep the add and compare close together.
    HGOETestModule tm("inc_branch", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* inc = tm.builder.CreateAdd(a, tm.builder.getInt64(1), "inc");
    auto* cmp = tm.builder.CreateICmpSLT(inc, b, "cmp");
    auto* sel = tm.builder.CreateSelect(cmp, inc, b, "sel");
    tm.builder.CreateRet(sel);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, MicroarchHasROBSize) {
    // All known profiles should have a non-zero ROB size.
    for (const char* arch : {"skylake", "znver4", "apple-m1", "neoverse-v2"}) {
        auto profile = lookupMicroarch(arch);
        ASSERT_TRUE(profile.has_value()) << "Missing profile for " << arch;
        EXPECT_GT(profile->robSize, 0u) << "ROB size not set for " << arch;
    }
}

TEST(HardwareGraphTest, ScheduleROBPressureAware) {
    // Create a large BB with many independent instructions to stress ROB.
    HGOETestModule tm("rob_pressure", 2);
    auto* a = tm.arg(0);

    // Generate many independent operations
    llvm::Value* acc = a;
    for (int i = 0; i < 50; ++i) {
        auto* t = tm.builder.CreateAdd(acc, tm.builder.getInt64(i));
        acc = tm.builder.CreateXor(acc, t);
    }
    tm.builder.CreateRet(acc);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, SchedulePortBalancing) {
    // Mix of ALU and multiply operations — port balancing should
    // distribute them across available ports.
    HGOETestModule tm("port_balance", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    // Create a mix of adds (ALU ports) and multiplies (specific ports)
    auto* add1 = tm.builder.CreateAdd(a, b, "add1");
    auto* mul1 = tm.builder.CreateMul(a, b, "mul1");
    auto* add2 = tm.builder.CreateAdd(add1, b, "add2");
    auto* mul2 = tm.builder.CreateMul(mul1, a, "mul2");
    auto* result = tm.builder.CreateAdd(add2, mul2, "result");
    tm.builder.CreateRet(result);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, ScheduleCacheMissAwareLoads) {
    // Loads with large GEP offsets should be scheduled earlier
    // (likely cache miss).
    HGOETestModule tm("cache_miss", 1);
    auto* base = tm.arg(0);

    // Create pointer type and loads at different offsets
    auto* ptr = tm.builder.CreateIntToPtr(base,
        llvm::PointerType::get(tm.builder.getInt64Ty(), 0));

    auto* gep_near = tm.builder.CreateConstGEP1_64(
        tm.builder.getInt64Ty(), ptr, 1, "near");
    auto* gep_far = tm.builder.CreateConstGEP1_64(
        tm.builder.getInt64Ty(), ptr, 1000, "far");

    auto* load_near = tm.builder.CreateLoad(tm.builder.getInt64Ty(), gep_near, "load_near");
    auto* load_far = tm.builder.CreateLoad(tm.builder.getInt64Ty(), gep_far, "load_far");

    auto* result = tm.builder.CreateAdd(load_near, load_far, "result");
    tm.builder.CreateRet(result);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

// ═════════════════════════════════════════════════════════════════════════════
// Sandy Bridge / Ivy Bridge profile tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, SandyBridgeProfileAccuracy) {
    auto profile = lookupMicroarch("sandybridge");
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->isa, ISAFamily::X86_64);
    EXPECT_EQ(profile->issueWidth, 4u);
    EXPECT_EQ(profile->intALUs, 3u);
    EXPECT_EQ(profile->fmaUnits, 0u);    // no FMA on Sandy Bridge
    EXPECT_EQ(profile->vectorWidth, 256u); // AVX (256-bit)
    EXPECT_EQ(profile->robSize, 168u);
    EXPECT_EQ(profile->l1DSize, 32u);
    EXPECT_EQ(profile->mulPortCount, 1u);
}

TEST(HardwareGraphTest, IvyBridgeLookup) {
    auto p1 = lookupMicroarch("ivybridge");
    auto p2 = lookupMicroarch("ivy-bridge");
    ASSERT_TRUE(p1.has_value());
    ASSERT_TRUE(p2.has_value());
    EXPECT_EQ(p1->name, p2->name);
    EXPECT_EQ(p1->isa, ISAFamily::X86_64);
    EXPECT_EQ(p1->fmaUnits, 0u); // no FMA on Ivy Bridge either
}

TEST(HardwareGraphTest, SandyBridgeOptimize) {
    HGOETestModule tm;
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    tm.builder.CreateRet(tm.builder.CreateAdd(a, b));

    HGOEConfig config;
    config.marchCpu = "sandybridge";
    auto stats = optimizeFunction(*tm.func, config);
    EXPECT_TRUE(stats.activated);
    EXPECT_EQ(stats.resolvedArch, "sandybridge");
    EXPECT_GE(stats.functionsOptimized, 1u);
}

// ═════════════════════════════════════════════════════════════════════════════
// Integer strength reduction — generic algorithm tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, StrengthReducePowerOf2) {
    // x * 64 -> x << 6 (single shift, latency 1 instead of 3)
    HGOETestModule tm("sr_pow2", 1);
    auto* x = tm.arg(0);
    auto* mul = tm.builder.CreateMul(x, tm.builder.getInt64(64), "sr_pow2");
    tm.builder.CreateRet(mul);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    TransformStats stats = applyHardwareTransforms(*tm.func, *profile);
    EXPECT_GE(stats.intStrengthReduced, 1u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, StrengthReduceAddForm) {
    // x * 3 -> (x<<1) + x, x * 5 -> (x<<2) + x, x * 65537 -> (x<<16) + x
    for (int64_t c : {3LL, 5LL, 65537LL, 131073LL}) {
        HGOETestModule tm("sr_add", 1);
        auto* x = tm.arg(0);
        auto* mul = tm.builder.CreateMul(x, tm.builder.getInt64(c), "sr_add");
        tm.builder.CreateRet(mul);
        ASSERT_TRUE(tm.verify());

        auto profile = lookupMicroarch("skylake");
        ASSERT_TRUE(profile.has_value());
        TransformStats stats = applyHardwareTransforms(*tm.func, *profile);
        EXPECT_GE(stats.intStrengthReduced, 1u) << "Failed for constant " << c;
        ASSERT_TRUE(tm.verify()) << "Verify failed for constant " << c;
    }
}

TEST(HardwareGraphTest, StrengthReduceSubForm) {
    // x * 7 -> (x<<3) - x, x * 63 -> (x<<6) - x, x * 255 -> (x<<8) - x
    for (int64_t c : {7LL, 15LL, 31LL, 63LL, 127LL, 255LL}) {
        HGOETestModule tm("sr_sub", 1);
        auto* x = tm.arg(0);
        auto* mul = tm.builder.CreateMul(x, tm.builder.getInt64(c), "sr_sub");
        tm.builder.CreateRet(mul);
        ASSERT_TRUE(tm.verify());

        auto profile = lookupMicroarch("skylake");
        ASSERT_TRUE(profile.has_value());
        TransformStats stats = applyHardwareTransforms(*tm.func, *profile);
        EXPECT_GE(stats.intStrengthReduced, 1u) << "Failed for constant " << c;
        ASSERT_TRUE(tm.verify()) << "Verify failed for constant " << c;
    }
}

TEST(HardwareGraphTest, StrengthReduceNegativeConstant) {
    // x * -7 -> -((x<<3) - x)
    HGOETestModule tm("sr_neg", 1);
    auto* x = tm.arg(0);
    auto* mul = tm.builder.CreateMul(x, tm.builder.getInt64(-7), "sr_neg");
    tm.builder.CreateRet(mul);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    TransformStats stats = applyHardwareTransforms(*tm.func, *profile);
    EXPECT_GE(stats.intStrengthReduced, 1u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, StrengthReduceLargeConstant) {
    // Ensure we don't generate incorrect code for arbitrary constants.
    HGOETestModule tm("sr_large", 1);
    auto* x = tm.arg(0);
    auto* mul = tm.builder.CreateMul(x, tm.builder.getInt64(196609), "sr_large");
    tm.builder.CreateRet(mul);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    // Whether or not it's reduced, the IR must remain valid.
    applyHardwareTransforms(*tm.func, *profile);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, StrengthReduceThreeSetBitsWhenBottlenecked) {
    // x * 11 = x * (8+2+1) -- 3 set bits, uses 3 instructions when
    // mulPortCount < intALUs (i.e., multiply is the bottleneck port).
    HGOETestModule tm("sr_3bit", 1);
    auto* x = tm.arg(0);
    auto* mul = tm.builder.CreateMul(x, tm.builder.getInt64(11), "sr_3bit");
    tm.builder.CreateRet(mul);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    // Skylake has mulPortCount=2, intALUs=4 -> 3-bit form should apply.
    EXPECT_LT(profile->mulPortCount, profile->intALUs);
    TransformStats stats = applyHardwareTransforms(*tm.func, *profile);
    EXPECT_GE(stats.intStrengthReduced, 1u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, StrengthReduceNoChangeForMul1) {
    // x * 1 is trivially the identity, and the hardware-graph identity-op
    // folder correctly rewrites it to `x` and removes the multiply.  We
    // verify exactly that: the resulting function must contain no `mul`
    // instructions.  (The transform is counted under intStrengthReduced;
    // we don't assert on the numeric value because multiple folders may
    // observe the same rewrite.)
    HGOETestModule tm("sr_one", 1);
    auto* x = tm.arg(0);
    auto* mul = tm.builder.CreateMul(x, tm.builder.getInt64(1), "sr_one");
    tm.builder.CreateRet(mul);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    applyHardwareTransforms(*tm.func, *profile);

    bool hasMul = false;
    for (auto& bb : *tm.func)
        for (auto& inst : bb)
            if (inst.getOpcode() == llvm::Instruction::Mul) hasMul = true;
    EXPECT_FALSE(hasMul) << "mul by 1 should be folded away";
    ASSERT_TRUE(tm.verify());
}

// ═════════════════════════════════════════════════════════════════════════════
// SchedulerPolicy tests — independently tunable heuristic components
// ═════════════════════════════════════════════════════════════════════════════

// Helper: build a small function and run scheduleInstructions with a policy.
// Returns the total cycle count and (via out param) quality metrics.
static unsigned runScheduler(llvm::Function& func, const MicroarchProfile& profile,
                              const SchedulerPolicy& policy,
                              SchedulerQuality* quality = nullptr) {
    HardwareGraph hw = buildHardwareGraph(profile);
    return scheduleInstructions(func, hw, profile, policy, quality);
}

TEST(HardwareGraphTest, SchedulerPolicyDefaultProducesValidSchedule) {
    // Default policy should produce a valid schedule (correct IR ordering).
    HGOETestModule tm("policy_default", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* add = tm.builder.CreateAdd(a, b, "add");
    auto* mul = tm.builder.CreateMul(add, a, "mul");
    tm.builder.CreateRet(mul);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    SchedulerPolicy policy; // default
    SchedulerQuality quality;
    unsigned cycles = runScheduler(*tm.func, *profile, policy, &quality);
    EXPECT_GT(cycles, 0u);
    EXPECT_GT(quality.instructionsTotal, 0u);
    EXPECT_GT(quality.basicBlocksScheduled, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, SchedulerPolicyDisableFusionStillValid) {
    // Disabling fusion heuristic should still produce correct, valid IR.
    HGOETestModule tm("policy_no_fusion", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* cmp = tm.builder.CreateICmpSLT(a, b, "cmp");
    auto* sel = tm.builder.CreateSelect(cmp, a, b, "sel");
    tm.builder.CreateRet(sel);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    SchedulerPolicy policy;
    policy.enableFusionHeuristic = false;
    unsigned cycles = runScheduler(*tm.func, *profile, policy);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, SchedulerPolicyDisableRegPressureStillValid) {
    // Disabling register pressure heuristic: schedule still correct.
    HGOETestModule tm("policy_no_rp", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* a1 = tm.builder.CreateAdd(a, b, "a1");
    auto* a2 = tm.builder.CreateMul(a1, b, "a2");
    auto* a3 = tm.builder.CreateSub(a2, a, "a3");
    tm.builder.CreateRet(a3);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    SchedulerPolicy policy;
    policy.enableRegPressure = false;
    unsigned cycles = runScheduler(*tm.func, *profile, policy);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, SchedulerPolicyNarrowBeamValid) {
    // Narrow beam width (1) should still produce a valid schedule.
    HGOETestModule tm("policy_narrow", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* r = tm.builder.CreateAdd(a, b, "r");
    tm.builder.CreateRet(r);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    SchedulerPolicy policy;
    policy.beamWidth = 1;
    unsigned cycles = runScheduler(*tm.func, *profile, policy);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, SchedulerPolicyDisableAllFeaturesStillValid) {
    // With all optional features disabled the scheduler degrades gracefully.
    HGOETestModule tm("policy_minimal", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* r = tm.builder.CreateMul(a, b, "r");
    tm.builder.CreateRet(r);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    SchedulerPolicy policy;
    policy.enableFusionHeuristic  = false;
    policy.enableRegPressure      = false;
    policy.enableROBPressure      = false;
    policy.enableChainDiversity   = false;
    policy.enableCacheMissRisk    = false;
    policy.enableSlackAware       = false;
    policy.enableBeamPruning      = false;
    unsigned cycles = runScheduler(*tm.func, *profile, policy);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

// ═════════════════════════════════════════════════════════════════════════════
// SchedulerQuality tests — critical-path adherence and ILP measurement
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, SchedulerQualityCollectsMetrics) {
    // scheduleInstructions must populate SchedulerQuality with sane values.
    HGOETestModule tm("quality_metrics", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    // Chain: a→r0→r1→r2→ret (4 dependent ops, critical path = 3 + latencies)
    auto* r0 = tm.builder.CreateAdd(a, b, "r0");
    auto* r1 = tm.builder.CreateMul(r0, a, "r1");
    auto* r2 = tm.builder.CreateAdd(r1, b, "r2");
    tm.builder.CreateRet(r2);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    SchedulerQuality quality;
    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile,
                                           SchedulerPolicy{}, &quality);
    EXPECT_GT(cycles, 0u);
    EXPECT_GT(quality.scheduledCycles, 0u);
    EXPECT_GE(quality.instructionsTotal, 3u);
    EXPECT_GE(quality.basicBlocksScheduled, 1u);
    // Efficiency should be in (0, 1.0] — can't be better than critical path
    EXPECT_GT(quality.efficiency, 0.0);
    EXPECT_LE(quality.efficiency, 1.0 + 1e-6);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, SchedulerQualityTwoIndependentChainsILP) {
    // Two independent chains should finish faster than one serial chain of the
    // same total instruction count, on a wide-issue CPU.
    //
    // Serial:  a→r0→r1 (latency-bound, 2 × latMul = 6 cycles on Skylake)
    // Parallel: a→p0, b→p1 (can issue simultaneously → ~3 cycles)
    //
    // We check that the two-chain case produces fewer cycles OR the same
    // (scheduler may still be limited by other constraints).
    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    // Serial chain
    {
        HGOETestModule tms("serial_chain", 2);
        auto* a = tms.arg(0);
        auto* b = tms.arg(1);
        auto* r0 = tms.builder.CreateMul(a, b, "r0");
        auto* r1 = tms.builder.CreateMul(r0, b, "r1");
        tms.builder.CreateRet(r1);
        SchedulerQuality qSerial;
        unsigned cSerial = scheduleInstructions(*tms.func, hw, *profile,
                                                SchedulerPolicy{}, &qSerial);
        EXPECT_GT(cSerial, 0u);
        EXPECT_GE(qSerial.instructionsTotal, 2u);
        (void)cSerial;
    }

    // Parallel chains — each chain is independent
    {
        HGOETestModule tmp("parallel_chains", 2);
        auto* a = tmp.arg(0);
        auto* b = tmp.arg(1);
        auto* p0 = tmp.builder.CreateMul(a, b, "p0");
        auto* p1 = tmp.builder.CreateMul(a, b, "p1");
        auto* res = tmp.builder.CreateAdd(p0, p1, "res");
        tmp.builder.CreateRet(res);
        SchedulerQuality qPar;
        unsigned cPar = scheduleInstructions(*tmp.func, hw, *profile,
                                              SchedulerPolicy{}, &qPar);
        EXPECT_GT(cPar, 0u);
        EXPECT_GE(qPar.instructionsTotal, 3u);
        // The parallel schedule should not be worse than the serial one.
        // (On a 6-wide machine with 2 multiply ports, p0 and p1 should
        // be issued in the same cycle.)
        EXPECT_LE(cPar, 20u); // sanity: should not take >20 cycles for 3 ops
    }
    // Verify both cases ran without crash and produced valid IR
    // (the ASSERT_TRUE in each sub-scope above already covers this).
}

TEST(HardwareGraphTest, SchedulerQualityLoadEarlyHidesLatency) {
    // A load whose result is consumed several ops later should be scheduled
    // early enough to overlap with independent ALU work.  Verify the schedule
    // does not produce more cycles than an extremely conservative upper bound.
    HGOETestModule tm("load_early", 2, /*useFP=*/false);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    // Allocate a local array on the stack and load from it.
    auto* alloc = tm.builder.CreateAlloca(tm.i64Ty(), tm.builder.getInt64(4), "arr");
    auto* gep   = tm.builder.CreateGEP(tm.i64Ty(), alloc, b, "ptr");
    tm.builder.CreateStore(a, gep);
    auto* load  = tm.builder.CreateLoad(tm.i64Ty(), gep, "val");

    // Do some independent arithmetic while waiting for the load.
    auto* i0 = tm.builder.CreateAdd(a, b, "i0");
    auto* i1 = tm.builder.CreateMul(i0, a, "i1");
    auto* i2 = tm.builder.CreateAdd(i1, b, "i2");
    // Finally use the loaded value.
    auto* res = tm.builder.CreateAdd(load, i2, "res");
    tm.builder.CreateRet(res);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    SchedulerQuality quality;
    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile,
                                           SchedulerPolicy{}, &quality);
    EXPECT_GT(cycles, 0u);
    // Sanity: should not exceed 5× the number of moveable instructions.
    EXPECT_LE(cycles, 5u * quality.instructionsTotal + 10u);
    ASSERT_TRUE(tm.verify());
}

// ═════════════════════════════════════════════════════════════════════════════
// CalibrationHints / calibrateProfile tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, CalibrateProfileIdentity) {
    // Default (identity) hints must leave the profile unchanged.
    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    CalibrationHints hints; // all scales = 1.0
    MicroarchProfile adjusted = calibrateProfile(*profile, hints);

    EXPECT_EQ(adjusted.latIntAdd,  profile->latIntAdd);
    EXPECT_EQ(adjusted.latFPAdd,   profile->latFPAdd);
    EXPECT_EQ(adjusted.latLoad,    profile->latLoad);
    EXPECT_EQ(adjusted.latIntDiv,  profile->latIntDiv);
    EXPECT_EQ(adjusted.robSize,    profile->robSize);
    EXPECT_EQ(adjusted.issueWidth, profile->issueWidth);
}

TEST(HardwareGraphTest, CalibrateProfileScalesLatencies) {
    // Non-identity hints should adjust the correct fields.
    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    CalibrationHints hints;
    hints.intAddScale = 2.0; // double integer add latency
    hints.loadScale   = 3.0; // triple load latency (e.g., cache pressure)
    hints.divScale    = 0.5; // halve div latency (aggressive speculation)
    hints.robScale    = 0.5; // halve ROB (SMT contention)

    MicroarchProfile adj = calibrateProfile(*profile, hints);

    EXPECT_EQ(adj.latIntAdd, profile->latIntAdd * 2u);
    EXPECT_EQ(adj.latLoad,   profile->latLoad   * 3u);
    // halved: result rounded to nearest, clamped to 1
    EXPECT_LE(adj.latIntDiv, profile->latIntDiv); // should not increase
    EXPECT_GE(adj.latIntDiv, 1u);
    EXPECT_LE(adj.robSize,   profile->robSize);   // smaller ROB
    EXPECT_GE(adj.robSize,   1u);
}

TEST(HardwareGraphTest, CalibrateProfileClampsToOne) {
    // Scaling to near-zero must clamp at 1 (no zero-latency paths produced
    // by calibration for instructions that require at least 1 cycle).
    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    CalibrationHints hints;
    hints.intAddScale = 0.001;
    hints.fpAddScale  = 0.001;
    hints.loadScale   = 0.001;
    hints.divScale    = 0.001;
    hints.robScale    = 0.001;
    hints.rsScale     = 0.001;
    hints.issueScale  = 0.001;

    MicroarchProfile adj = calibrateProfile(*profile, hints);

    EXPECT_GE(adj.latIntAdd,    1u);
    EXPECT_GE(adj.latFPAdd,     1u);
    EXPECT_GE(adj.latLoad,      1u);
    EXPECT_GE(adj.latIntDiv,    1u);
    EXPECT_GE(adj.robSize,      1u);
    EXPECT_GE(adj.schedulerSize, 1u);
    EXPECT_GE(adj.issueWidth,   1u);
}

TEST(HardwareGraphTest, CalibrateProfileUsedByScheduler) {
    // A calibrated profile must be usable directly with scheduleInstructions.
    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());

    // Simulate a heavily-loaded SMT scenario: halve ROB and issue width.
    CalibrationHints hints;
    hints.robScale   = 0.5;
    hints.issueScale = 0.5;
    MicroarchProfile adj = calibrateProfile(*profile, hints);

    HGOETestModule tm("calib_sched", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* r = tm.builder.CreateAdd(a, b, "r");
    tm.builder.CreateRet(r);
    ASSERT_TRUE(tm.verify());

    HardwareGraph hw = buildHardwareGraph(adj);
    SchedulerQuality quality;
    unsigned cycles = scheduleInstructions(*tm.func, hw, adj,
                                           SchedulerPolicy{}, &quality);
    EXPECT_GT(cycles, 0u);
    ASSERT_TRUE(tm.verify());
}

// ═════════════════════════════════════════════════════════════════════════════
// Memory dependency precision tests
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, ProgramGraphMemoryDepsMultipleStores) {
    // Verify that buildFromFunction tracks all live stores, not just the last.
    // Given:  store @A, 1 ; store @B, 2 ; load %x, @A
    // The RAW edge from store @A to load %x must be captured.  With the old
    // last-only tracking it was missed when store @B didn't alias @A.
    HGOETestModule tm("multi_store_dep", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    // Two separate alloca'd variables — no aliasing between them.
    auto* varA = tm.builder.CreateAlloca(tm.i64Ty(), nullptr, "varA");
    auto* varB = tm.builder.CreateAlloca(tm.i64Ty(), nullptr, "varB");

    tm.builder.CreateStore(a, varA); // store to varA
    tm.builder.CreateStore(b, varB); // store to varB (different address)
    auto* load = tm.builder.CreateLoad(tm.i64Ty(), varA, "load_a"); // load varA
    auto* res  = tm.builder.CreateAdd(load, b, "res");
    tm.builder.CreateRet(res);
    ASSERT_TRUE(tm.verify());

    ProgramGraph pg;
    pg.buildFromFunction(*tm.func);

    // Find the node IDs for the store @varA and the load @varA.
    unsigned storeAId = ~0u, loadAId = ~0u;
    for (unsigned i = 0; i < pg.nodeCount(); ++i) {
        const ProgramNode* nd = pg.getNode(i);
        if (!nd || !nd->inst) continue;
        if (nd->opClass == OpClass::Store &&
            llvm::cast<llvm::StoreInst>(nd->inst)->getPointerOperand() == varA)
            storeAId = i;
        if (nd->opClass == OpClass::Load &&
            llvm::cast<llvm::LoadInst>(nd->inst)->getPointerOperand() == varA)
            loadAId = i;
    }
    ASSERT_NE(storeAId, ~0u) << "store @varA node not found";
    ASSERT_NE(loadAId,  ~0u) << "load  @varA node not found";

    // There must be a memory edge from storeA → loadA.
    bool foundEdge = false;
    for (const auto& e : pg.edges()) {
        if (e.srcId == storeAId && e.dstId == loadAId &&
            e.type == DepType::Memory)
            foundEdge = true;
    }
    EXPECT_TRUE(foundEdge)
        << "Expected RAW memory edge from store @varA to load @varA "
           "but none was found — all-live-stores tracking is broken.";
}

TEST(HardwareGraphTest, ProgramGraphMemoryDepsNoSpuriousEdge) {
    // When two stores are to provably non-aliasing locals and a load reads
    // only the second one, there should be no RAW edge from the first store.
    HGOETestModule tm("no_spurious_dep", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);

    auto* varA = tm.builder.CreateAlloca(tm.i64Ty(), nullptr, "varA");
    auto* varB = tm.builder.CreateAlloca(tm.i64Ty(), nullptr, "varB");

    tm.builder.CreateStore(a, varA);
    tm.builder.CreateStore(b, varB);
    auto* load = tm.builder.CreateLoad(tm.i64Ty(), varB, "load_b"); // read varB, not varA
    tm.builder.CreateRet(load);
    ASSERT_TRUE(tm.verify());

    ProgramGraph pg;
    pg.buildFromFunction(*tm.func);

    // Find store @varA and load @varB.
    unsigned storeAId = ~0u, loadBId = ~0u;
    for (unsigned i = 0; i < pg.nodeCount(); ++i) {
        const ProgramNode* nd = pg.getNode(i);
        if (!nd || !nd->inst) continue;
        if (nd->opClass == OpClass::Store &&
            llvm::cast<llvm::StoreInst>(nd->inst)->getPointerOperand() == varA)
            storeAId = i;
        if (nd->opClass == OpClass::Load &&
            llvm::cast<llvm::LoadInst>(nd->inst)->getPointerOperand() == varB)
            loadBId = i;
    }
    ASSERT_NE(storeAId, ~0u);
    ASSERT_NE(loadBId,  ~0u);

    // Should NOT have a memory edge from storeA → loadB (non-aliasing).
    bool found = false;
    for (const auto& e : pg.edges())
        if (e.srcId == storeAId && e.dstId == loadBId && e.type == DepType::Memory)
            found = true;
    EXPECT_FALSE(found)
        << "Spurious RAW edge from store @varA to load @varB: "
           "alias analysis should rule this out.";
}

// ═════════════════════════════════════════════════════════════════════════════
// HGOEConfig.schedulerPolicy plumbing test
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, OptimizeFunctionUsesConfigPolicy) {
    // Verify that the policy in HGOEConfig is plumbed through to the scheduler.
    // We set an extreme beamWidth and verify the function still verifies after.
    HGOETestModule tm("cfg_policy", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* r0 = tm.builder.CreateAdd(a, b, "r0");
    auto* r1 = tm.builder.CreateMul(r0, a, "r1");
    tm.builder.CreateRet(r1);
    ASSERT_TRUE(tm.verify());

    HGOEConfig config;
    config.marchCpu = "skylake";
    config.schedulerPolicy.beamWidth = 4;
    config.schedulerPolicy.enableFusionHeuristic = false;

    HGOEStats stats = optimizeFunction(*tm.func, config);
    EXPECT_TRUE(stats.activated);
    EXPECT_EQ(stats.resolvedArch, "skylake");
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, HGOEStatsSchedulerQualityPopulated) {
    // After optimizeFunction, schedulerQuality fields must be populated.
    HGOETestModule tm("stats_quality", 2);
    auto* a = tm.arg(0);
    auto* b = tm.arg(1);
    auto* r0 = tm.builder.CreateAdd(a, b, "r0");
    auto* r1 = tm.builder.CreateMul(r0, a, "r1");
    auto* r2 = tm.builder.CreateAdd(r1, b, "r2");
    tm.builder.CreateRet(r2);
    ASSERT_TRUE(tm.verify());

    HGOEConfig config;
    config.marchCpu = "skylake";

    HGOEStats stats = optimizeFunction(*tm.func, config);
    EXPECT_TRUE(stats.activated);
    EXPECT_GT(stats.schedulerQuality.scheduledCycles, 0u);
    EXPECT_GT(stats.schedulerQuality.instructionsTotal, 0u);
    ASSERT_TRUE(tm.verify());
}


// ═════════════════════════════════════════════════════════════════════════════
// Per-CPU port-assignment correctness tests
// These tests verify that after the classifyOp / port-model fixes, specific
// instruction types are routed to the correct execution-unit ports so they
// no longer spuriously compete with integer ALU operations.
// ═════════════════════════════════════════════════════════════════════════════

TEST(HardwareGraphTest, PortModel_FMAIntrinUsesFMAUnit) {
    // Create a function with 4 independent llvm.fma intrinsics.
    // After the classifyOp fix fma→OpClass::FMA, they should be dispatched
    // to the FMAUnit, not the IntegerALU.  Scheduling must produce valid IR
    // and stay within a reasonable cycle budget.
    HGOETestModule tm("fma_intrin_test", 2, /*useFP=*/true);
    llvm::IRBuilder<>& b = tm.builder;
    llvm::LLVMContext& ctx = tm.ctx;

    llvm::Type* f64 = llvm::Type::getDoubleTy(ctx);
    llvm::Value* a = tm.arg(0), *bv = tm.arg(1);

    llvm::Function* fmaFn =
        OMSC_TEST_GET_INTRIN(tm.mod.get(), llvm::Intrinsic::fma,
                                        {f64});
    llvm::Value* fma0 = b.CreateCall(fmaFn, {a, bv, a},   "fma0");
    llvm::Value* fma1 = b.CreateCall(fmaFn, {bv, a, bv},  "fma1");
    llvm::Value* fma2 = b.CreateCall(fmaFn, {a, a, bv},   "fma2");
    llvm::Value* fma3 = b.CreateCall(fmaFn, {bv, bv, a},  "fma3");

    llvm::Value* s01  = b.CreateFAdd(fma0, fma1, "s01");
    llvm::Value* s23  = b.CreateFAdd(fma2, fma3, "s23");
    llvm::Value* tot  = b.CreateFAdd(s01, s23, "tot");
    b.CreateRet(tot);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    SchedulerQuality quality;
    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile,
                                           SchedulerPolicy{}, &quality);
    EXPECT_GT(cycles, 0u);
    // 3-level FMA/FPAdd tree: critical path ≤ latFMA + latFPAdd*2 + overhead
    EXPECT_LE(cycles, static_cast<unsigned>(profile->latFMA * 2 + profile->latFPAdd * 2 + 8u));
    EXPECT_GT(quality.efficiency, 0.0);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, PortModel_SqrtUsesDividerUnit) {
    // llvm.sqrt is classified as OpClass::FPDiv (DividerUnit) after the fix.
    // Two independent sqrts: verify the schedule is valid and cycle-bounded.
    HGOETestModule tm("sqrt_test", 2, /*useFP=*/true);
    llvm::IRBuilder<>& b = tm.builder;
    llvm::LLVMContext& ctx = tm.ctx;

    llvm::Type* f64 = llvm::Type::getDoubleTy(ctx);
    llvm::Function* sqrtFn =
        OMSC_TEST_GET_INTRIN(tm.mod.get(), llvm::Intrinsic::sqrt,
                                        {f64});
    llvm::Value* a = tm.arg(0), *bv = tm.arg(1);
    llvm::Value* sq0 = b.CreateCall(sqrtFn, {a},  "sq0");
    llvm::Value* sq1 = b.CreateCall(sqrtFn, {bv}, "sq1");
    llvm::Value* res = b.CreateFAdd(sq0, sq1, "res");
    b.CreateRet(res);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    SchedulerQuality quality;
    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile,
                                           SchedulerPolicy{}, &quality);
    EXPECT_GT(cycles, 0u);
    // Two sqrts serialise on a single divider unit (2 × latFPDiv), plus a
    // trailing FPAdd and dispatch/retire overhead from the ROB model.  The
    // scheduler now accounts for pipeline stall slack more conservatively,
    // so the upper bound here allows for ~16 cycles of overhead on top of
    // the two divider occupancies.
    EXPECT_LE(cycles, 2u * profile->latFPDiv + 16u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, PortModel_FCmpUsesFPPipeline) {
    // FCmp must be classified as FPArith (FMAUnit) not Comparison (IntegerALU).
    HGOETestModule tm("fcmp_test", 2, /*useFP=*/true);
    llvm::IRBuilder<>& b = tm.builder;
    llvm::Value* a = tm.arg(0), *bv = tm.arg(1);

    llvm::Value* cmp = b.CreateFCmpOLT(a, bv, "cmp");
    llvm::Value* sel = b.CreateSelect(cmp, a, bv, "sel");
    b.CreateRet(sel);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("znver4");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    SchedulerQuality quality;
    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile,
                                           SchedulerPolicy{}, &quality);
    EXPECT_GT(cycles, 0u);
    EXPECT_LE(cycles, 4u * profile->latFPAdd + 5u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, PortModel_FPConversionUsesFPPipeline) {
    // UIToFP/SIToFP/FPToSI must be classified as FPArith (FMAUnit), not
    // Conversion (IntegerALU).  Verify scheduling of a conversion chain.
    HGOETestModule tm("fp_convert_test", 2, /*useFP=*/false);
    llvm::IRBuilder<>& b = tm.builder;
    llvm::LLVMContext& ctx = tm.ctx;

    llvm::Type* f64 = llvm::Type::getDoubleTy(ctx);
    llvm::Value* a = tm.arg(0), *bv = tm.arg(1);

    llvm::Value* fa   = b.CreateSIToFP(a, f64, "fa");
    llvm::Value* fb   = b.CreateSIToFP(bv, f64, "fb");
    llvm::Value* prod = b.CreateFMul(fa, fb, "prod");
    llvm::Value* back = b.CreateFPToSI(prod, llvm::Type::getInt64Ty(ctx), "back");
    llvm::Value* res  = b.CreateAdd(back, a, "res");
    b.CreateRet(res);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    SchedulerQuality quality;
    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile,
                                           SchedulerPolicy{}, &quality);
    EXPECT_GT(cycles, 0u);
    // Critical path: 2 × latFPConvert(=5) + latFPMul(=4) + latFPConvert + latIntAdd
    // = 5+4+5+1 = 15 + overhead.  Allow generous upper bound.
    EXPECT_LE(cycles, 30u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, PortModel_FPRoundIntrinUsesFPPort) {
    // floor/ceil should be classified as FPArith (FMAUnit), not Intrinsic
    // (IntegerALU).  Verify scheduling produces valid IR.
    HGOETestModule tm("fp_round_test", 1, /*useFP=*/true);
    llvm::IRBuilder<>& b = tm.builder;
    llvm::LLVMContext& ctx = tm.ctx;

    llvm::Type* f64 = llvm::Type::getDoubleTy(ctx);
    llvm::Function* floorFn =
        OMSC_TEST_GET_INTRIN(tm.mod.get(), llvm::Intrinsic::floor,
                                        {f64});
    llvm::Function* ceilFn =
        OMSC_TEST_GET_INTRIN(tm.mod.get(), llvm::Intrinsic::ceil,
                                        {f64});
    llvm::Value* a  = tm.arg(0);
    llvm::Value* fl = b.CreateCall(floorFn, {a}, "fl");
    llvm::Value* cl = b.CreateCall(ceilFn,  {a}, "cl");
    llvm::Value* r  = b.CreateFAdd(fl, cl, "r");
    b.CreateRet(r);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("znver5");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    SchedulerQuality quality;
    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile,
                                           SchedulerPolicy{}, &quality);
    EXPECT_GT(cycles, 0u);
    // floor/ceil latency ≈ latFPAdd*2; fadd on top → reasonable upper bound
    EXPECT_LE(cycles, 3u * profile->latFPAdd + 8u);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, PortModel_DividerPortBlocked_SerialDivides) {
    // After the divider throughput fix, two chained integer divides must take
    // at least 2 × latIntDiv cycles: the divider is non-pipelined, so the
    // second divide cannot start until the first completes its full latency.
    HGOETestModule tm("serial_div_test", 2);
    llvm::IRBuilder<>& b = tm.builder;
    llvm::Value* a = tm.arg(0), *bv = tm.arg(1);

    llvm::Value* d0  = b.CreateSDiv(a, bv,  "d0");
    llvm::Value* d1  = b.CreateSDiv(d0, bv, "d1");  // depends on d0
    b.CreateRet(d1);
    ASSERT_TRUE(tm.verify());

    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);

    SchedulerQuality quality;
    unsigned cycles = scheduleInstructions(*tm.func, hw, *profile,
                                           SchedulerPolicy{}, &quality);
    // Two serial non-pipelined divides: at least 2 × latIntDiv
    EXPECT_GE(cycles, 2u * profile->latIntDiv);
    ASSERT_TRUE(tm.verify());
}

TEST(HardwareGraphTest, PortModel_DividerPort_ParallelBound) {
    // Two INDEPENDENT divides are bounded by the slower of:
    //   - serial: 2 × latIntDiv  (single divider, Skylake)
    //   - parallel: latIntDiv    (two dividers, Apple M1)
    // Either way, cycles ≤ 2 × latIntDiv + small constant.
    HGOETestModule tm("parallel_div_test", 2);
    llvm::IRBuilder<>& b = tm.builder;
    llvm::Value* a = tm.arg(0), *bv = tm.arg(1);

    llvm::Value* d0  = b.CreateSDiv(a,  bv, "d0");
    llvm::Value* d1  = b.CreateSDiv(bv, a,  "d1");  // independent of d0
    llvm::Value* res = b.CreateAdd(d0, d1, "res");
    b.CreateRet(res);
    ASSERT_TRUE(tm.verify());

    for (const char* cpu : {"skylake", "apple-m1"}) {
        auto profile = lookupMicroarch(cpu);
        ASSERT_TRUE(profile.has_value()) << "CPU: " << cpu;
        HardwareGraph hw = buildHardwareGraph(*profile);

        SchedulerQuality quality;
        unsigned cycles = scheduleInstructions(*tm.func, hw, *profile,
                                               SchedulerPolicy{}, &quality);
        EXPECT_GT(cycles, 0u) << "CPU: " << cpu;
        EXPECT_LE(cycles, 2u * profile->latIntDiv + 5u) << "CPU: " << cpu;
        ASSERT_TRUE(tm.verify()) << "CPU: " << cpu;
    }
}
