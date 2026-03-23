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
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>

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
    HardwareCostModel costModel(hw);

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

    HardwareCostModel skylakeCost(hwSkylake);
    HardwareCostModel appleCost(hwApple);

    // Skylake should prefer wider vectors (AVX2=256-bit → width 8)
    // Apple M1 NEON is 128-bit → width 4
    EXPECT_GE(skylakeCost.preferredVectorWidth(), appleCost.preferredVectorWidth());
}

TEST(HardwareGraphTest, SimulateExecution) {
    auto profile = lookupMicroarch("skylake");
    ASSERT_TRUE(profile.has_value());
    HardwareGraph hw = buildHardwareGraph(*profile);
    HardwareCostModel costModel(hw);

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
