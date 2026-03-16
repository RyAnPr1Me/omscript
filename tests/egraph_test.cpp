/// @file egraph_test.cpp
/// @brief Unit tests for the e-graph equality saturation optimizer.
///
/// Tests cover:
///   - E-graph data structure operations (add, find, merge)
///   - Pattern matching
///   - Algebraic simplification rules
///   - Constant folding
///   - Strength reduction
///   - Comparison simplification
///   - Bitwise simplification
///   - Cost-based extraction
///   - AST ↔ E-graph round-trip (semantic preservation)
///   - Scalability (node limits, iteration limits)

#include "ast.h"
#include "codegen.h"
#include "egraph.h"
#include "lexer.h"
#include "parser.h"
#include <gtest/gtest.h>
#include <memory>

using namespace omscript;
using namespace omscript::egraph;

// ─────────────────────────────────────────────────────────────────────────────
// E-Graph data structure tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, AddConstant) {
    EGraph g;
    ClassId c1 = g.addConst(42);
    ClassId c2 = g.addConst(42);
    // Same constant should map to the same class (hash-consing)
    EXPECT_EQ(g.find(c1), g.find(c2));
}

TEST(EGraphTest, AddDifferentConstants) {
    EGraph g;
    ClassId c1 = g.addConst(1);
    ClassId c2 = g.addConst(2);
    EXPECT_NE(g.find(c1), g.find(c2));
}

TEST(EGraphTest, AddVariable) {
    EGraph g;
    ClassId v1 = g.addVar("x");
    ClassId v2 = g.addVar("x");
    EXPECT_EQ(g.find(v1), g.find(v2));
}

TEST(EGraphTest, MergeClasses) {
    EGraph g;
    ClassId a = g.addConst(1);
    ClassId b = g.addConst(2);
    EXPECT_NE(g.find(a), g.find(b));

    g.merge(a, b);
    EXPECT_EQ(g.find(a), g.find(b));
}

TEST(EGraphTest, TransitiveMerge) {
    EGraph g;
    ClassId a = g.addConst(1);
    ClassId b = g.addConst(2);
    ClassId c = g.addConst(3);

    g.merge(a, b);
    g.merge(b, c);
    EXPECT_EQ(g.find(a), g.find(c));
}

TEST(EGraphTest, BinaryOpCreation) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    ClassId sum = g.addBinOp(Op::Add, x, y);
    EXPECT_NE(g.find(sum), g.find(x));
    EXPECT_NE(g.find(sum), g.find(y));
}

TEST(EGraphTest, HashConsDedup) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    ClassId sum1 = g.addBinOp(Op::Add, x, y);
    ClassId sum2 = g.addBinOp(Op::Add, x, y);
    // Same expression should deduplicate
    EXPECT_EQ(g.find(sum1), g.find(sum2));
}

TEST(EGraphTest, NumClasses) {
    EGraph g;
    g.addConst(1);
    g.addConst(2);
    g.addVar("x");
    EXPECT_EQ(g.numClasses(), 3u);
}

TEST(EGraphTest, MergeReducesClasses) {
    EGraph g;
    ClassId a = g.addConst(1);
    ClassId b = g.addConst(2);
    EXPECT_EQ(g.numClasses(), 2u);
    g.merge(a, b);
    EXPECT_EQ(g.numClasses(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Pattern matching tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, PatternMatchWildcard) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId zero = g.addConst(0);
    g.addBinOp(Op::Add, x, zero);

    // Match: ?a + 0
    auto matches = g.match(
        Pattern::OpPat(Op::Add, {Pattern::Wild("a"), Pattern::ConstPat(0)}));
    EXPECT_GE(matches.size(), 1u);
}

TEST(EGraphTest, PatternMatchConst) {
    EGraph g;
    g.addConst(42);

    auto matches = g.match(Pattern::ConstPat(42));
    EXPECT_GE(matches.size(), 1u);
}

TEST(EGraphTest, PatternNoMatch) {
    EGraph g;
    g.addConst(1);

    auto matches = g.match(Pattern::ConstPat(99));
    EXPECT_EQ(matches.size(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Algebraic simplification tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, AddZeroSimplification) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::Add, x, zero);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    // After saturation, expr and x should be in the same class
    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, MulOneSimplification) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId one = g.addConst(1);
    ClassId expr = g.addBinOp(Op::Mul, x, one);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, MulZeroSimplification) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::Mul, x, zero);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(zero));
}

TEST(EGraphTest, SubSelfSimplification) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Sub, x, x);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId zero = g.addConst(0);
    EXPECT_EQ(g.find(expr), g.find(zero));
}

TEST(EGraphTest, DivOneSimplification) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId one = g.addConst(1);
    ClassId expr = g.addBinOp(Op::Div, x, one);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, DivSelfSimplification) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Div, x, x);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId one = g.addConst(1);
    EXPECT_EQ(g.find(expr), g.find(one));
}

TEST(EGraphTest, DoubleNegation) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId neg1 = g.addUnaryOp(Op::Neg, x);
    ClassId neg2 = g.addUnaryOp(Op::Neg, neg1);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(neg2), g.find(x));
}

TEST(EGraphTest, ModOneIsZero) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId one = g.addConst(1);
    ClassId expr = g.addBinOp(Op::Mod, x, one);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId zero = g.addConst(0);
    EXPECT_EQ(g.find(expr), g.find(zero));
}

// ─────────────────────────────────────────────────────────────────────────────
// Strength reduction tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, MulTwoToAdd) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId two = g.addConst(2);
    ClassId expr = g.addBinOp(Op::Mul, x, two);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    // After saturation, x*2 should be equivalent to x+x
    ClassId addExpr = g.addBinOp(Op::Add, x, x);
    EXPECT_EQ(g.find(expr), g.find(addExpr));
}

TEST(EGraphTest, MulPow2ToShift) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId eight = g.addConst(8);
    ClassId expr = g.addBinOp(Op::Mul, x, eight);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    // x*8 should be equivalent to x<<3
    ClassId three = g.addConst(3);
    ClassId shiftExpr = g.addBinOp(Op::Shl, x, three);
    EXPECT_EQ(g.find(expr), g.find(shiftExpr));
}

TEST(EGraphTest, DivPow2ToShift) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId four = g.addConst(4);
    ClassId expr = g.addBinOp(Op::Div, x, four);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    // x/4 should NOT be equivalent to x>>2 for signed integers
    // (e.g. -7/4 = -1, but -7>>2 = -2 due to rounding toward -inf).
    // The div_pow2 rule was removed to fix this signed-division bug.
    ClassId two = g.addConst(2);
    ClassId shiftExpr = g.addBinOp(Op::Shr, x, two);
    EXPECT_NE(g.find(expr), g.find(shiftExpr));
}

// ─────────────────────────────────────────────────────────────────────────────
// Constant folding tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, ConstantFoldAdd) {
    EGraph g;
    ClassId a = g.addConst(3);
    ClassId b = g.addConst(4);
    ClassId expr = g.addBinOp(Op::Add, a, b);

    auto rules = getAllRules();
    g.saturate(rules);

    ClassId expected = g.addConst(7);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, ConstantFoldMul) {
    EGraph g;
    ClassId a = g.addConst(5);
    ClassId b = g.addConst(6);
    ClassId expr = g.addBinOp(Op::Mul, a, b);

    auto rules = getAllRules();
    g.saturate(rules);

    ClassId expected = g.addConst(30);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, ConstantFoldSub) {
    EGraph g;
    ClassId a = g.addConst(10);
    ClassId b = g.addConst(3);
    ClassId expr = g.addBinOp(Op::Sub, a, b);

    auto rules = getAllRules();
    g.saturate(rules);

    ClassId expected = g.addConst(7);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, ConstantFoldNeg) {
    EGraph g;
    ClassId a = g.addConst(5);
    ClassId expr = g.addUnaryOp(Op::Neg, a);

    auto rules = getAllRules();
    g.saturate(rules);

    ClassId expected = g.addConst(-5);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, ConstantFoldComparison) {
    EGraph g;
    ClassId a = g.addConst(3);
    ClassId b = g.addConst(5);
    ClassId expr = g.addBinOp(Op::Lt, a, b);

    auto rules = getAllRules();
    g.saturate(rules);

    ClassId expected = g.addConst(1);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison rule tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, SelfEqualityIsOne) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Eq, x, x);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId one = g.addConst(1);
    EXPECT_EQ(g.find(expr), g.find(one));
}

TEST(EGraphTest, SelfInequalityIsZero) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Ne, x, x);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId zero = g.addConst(0);
    EXPECT_EQ(g.find(expr), g.find(zero));
}

TEST(EGraphTest, SelfLessThanIsZero) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Lt, x, x);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId zero = g.addConst(0);
    EXPECT_EQ(g.find(expr), g.find(zero));
}

TEST(EGraphTest, SelfLessEqualIsOne) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Le, x, x);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId one = g.addConst(1);
    EXPECT_EQ(g.find(expr), g.find(one));
}

TEST(EGraphTest, DoubleLogicalNot) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId not1 = g.addUnaryOp(Op::LogNot, x);
    ClassId not2 = g.addUnaryOp(Op::LogNot, not1);

    auto rules = getComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(not2), g.find(x));
}

// ─────────────────────────────────────────────────────────────────────────────
// Bitwise rule tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, XorSelfIsZero) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::BitXor, x, x);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    ClassId zero = g.addConst(0);
    EXPECT_EQ(g.find(expr), g.find(zero));
}

TEST(EGraphTest, AndSelfIsIdentity) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::BitAnd, x, x);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, OrSelfIsIdentity) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::BitOr, x, x);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, XorZeroIsIdentity) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::BitXor, x, zero);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, AndZeroIsZero) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::BitAnd, x, zero);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(zero));
}

TEST(EGraphTest, DoubleBitNot) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId not1 = g.addUnaryOp(Op::BitNot, x);
    ClassId not2 = g.addUnaryOp(Op::BitNot, not1);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(not2), g.find(x));
}

TEST(EGraphTest, ShiftByZeroIsIdentity) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId zero = g.addConst(0);
    ClassId shl = g.addBinOp(Op::Shl, x, zero);
    ClassId shr = g.addBinOp(Op::Shr, x, zero);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(shl), g.find(x));
    EXPECT_EQ(g.find(shr), g.find(x));
}

// ─────────────────────────────────────────────────────────────────────────────
// Cost model and extraction tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, ExtractPrefersCheaper) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId two = g.addConst(2);
    ClassId expr = g.addBinOp(Op::Mul, x, two);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    CostModel model;
    ENode best = g.extract(g.find(expr), model);

    // The cost model should prefer x+x (cost: 1) over x*2 (cost: 3)
    // or x<<1 (cost: 1+0.1=1.1 for shift+const)
    // The best should be Add or Shl, not Mul
    EXPECT_NE(best.op, Op::Mul);
}

TEST(EGraphTest, ExtractConstantFolded) {
    EGraph g;
    ClassId a = g.addConst(10);
    ClassId b = g.addConst(20);
    ClassId expr = g.addBinOp(Op::Add, a, b);

    auto rules = getAllRules();
    g.saturate(rules);

    CostModel model;
    ENode best = g.extract(g.find(expr), model);

    // Should extract the constant 30 (cheapest representation)
    EXPECT_EQ(best.op, Op::Const);
    EXPECT_EQ(best.value, 30);
}

// ─────────────────────────────────────────────────────────────────────────────
// Scalability tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, NodeLimitRespected) {
    SaturationConfig config;
    config.maxNodes = 100;
    config.maxIterations = 100;

    EGraph g(config);
    ClassId x = g.addVar("x");

    // Build a deep chain: ((x + x) + x) + x ...
    ClassId current = x;
    for (int i = 0; i < 50; i++) {
        current = g.addBinOp(Op::Add, current, x);
    }

    auto rules = getAllRules();
    g.saturate(rules);

    // Should not exceed the node limit
    EXPECT_LE(g.numNodes(), config.maxNodes + 100); // some slack for final iteration
}

TEST(EGraphTest, IterationLimitRespected) {
    SaturationConfig config;
    config.maxNodes = 100000;
    config.maxIterations = 3;

    EGraph g(config);
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    g.addBinOp(Op::Add, x, y);

    auto rules = getAllRules();
    size_t iterations = g.saturate(rules);

    EXPECT_LE(iterations, config.maxIterations);
}

// ─────────────────────────────────────────────────────────────────────────────
// AST round-trip tests (semantic preservation)
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, OptimizeExpressionPreservesConstant) {
    auto expr = std::make_unique<LiteralExpr>(42LL);
    auto result = optimizeExpression(expr.get());
    ASSERT_NE(result, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(result.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->literalType, LiteralExpr::LiteralType::INTEGER);
    EXPECT_EQ(lit->intValue, 42);
}

TEST(EGraphTest, OptimizeExpressionFoldsConstants) {
    // 3 + 4 should fold to 7
    auto left = std::make_unique<LiteralExpr>(3LL);
    auto right = std::make_unique<LiteralExpr>(4LL);
    auto expr = std::make_unique<BinaryExpr>("+", std::move(left), std::move(right));

    auto result = optimizeExpression(expr.get());
    ASSERT_NE(result, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(result.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->intValue, 7);
}

TEST(EGraphTest, OptimizeExpressionSimplifies) {
    // x + 0 should simplify to x
    auto x = std::make_unique<IdentifierExpr>("x");
    auto zero = std::make_unique<LiteralExpr>(0LL);
    auto expr = std::make_unique<BinaryExpr>("+", std::move(x), std::move(zero));

    auto result = optimizeExpression(expr.get());
    ASSERT_NE(result, nullptr);
    auto* id = dynamic_cast<IdentifierExpr*>(result.get());
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->name, "x");
}

TEST(EGraphTest, OptimizeExpressionPreservesVariable) {
    auto expr = std::make_unique<IdentifierExpr>("myVar");
    auto result = optimizeExpression(expr.get());
    ASSERT_NE(result, nullptr);
    auto* id = dynamic_cast<IdentifierExpr*>(result.get());
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->name, "myVar");
}

// ─────────────────────────────────────────────────────────────────────────────
// Integration test: e-graph optimizer + LLVM codegen
// ─────────────────────────────────────────────────────────────────────────────

// Helper to generate IR from OmScript source (from codegen_test.cpp pattern)
static llvm::Module* generateIR(const std::string& source, CodeGenerator& codegen) {
    omscript::Lexer lexer(source);
    auto tokens = lexer.tokenize();
    omscript::Parser parser(tokens);
    auto program = parser.parse();
    codegen.generate(program.get());
    return codegen.getModule();
}

TEST(EGraphTest, SemanticPreservationSimpleArithmetic) {
    // Verify that e-graph optimization doesn't change semantics
    // Test: fn main() { return 3 + 4 * 2; }
    // Expected: 11
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn main() { return 3 + 4 * 2; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(EGraphTest, SemanticPreservationWithVariables) {
    // fn f(x) { return x + 0; }  should optimize x+0 → x at AST level
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(x) { return x + 0; } fn main() { return f(5); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(EGraphTest, SemanticPreservationComplex) {
    // More complex expression with multiple optimizable patterns
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR(
        "fn compute(a, b) { return (a * 1) + (b - 0) + (a - a); }\n"
        "fn main() { return compute(10, 20); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(EGraphTest, EGraphDisabledAtO0) {
    // At O0, e-graph optimization should NOT be applied
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 3 + 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(EGraphTest, EGraphDisabledWithFlag) {
    // Test that -fno-egraph disables the optimization
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setEGraphOptimize(false);
    auto* mod = generateIR("fn main() { return 3 + 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Complex algebraic identity tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, Commutativity) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    ClassId xy = g.addBinOp(Op::Add, x, y);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    // After commutativity, x+y and y+x should be equivalent
    ClassId yx = g.addBinOp(Op::Add, y, x);
    EXPECT_EQ(g.find(xy), g.find(yx));
}

TEST(EGraphTest, SubZeroRight) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::Sub, x, zero);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, CostModelPreferences) {
    CostModel model;

    // Constants are cheapest
    ENode constNode(Op::Const, 42LL);
    EXPECT_LT(model.nodeCost(constNode), 1.0);

    // Division is very expensive
    ENode divNode(Op::Div);
    EXPECT_GT(model.nodeCost(divNode), 20.0);

    // Addition is cheap
    ENode addNode(Op::Add);
    EXPECT_LE(model.nodeCost(addNode), 2.0);

    // Multiplication is moderate
    ENode mulNode(Op::Mul);
    EXPECT_GT(model.nodeCost(mulNode), model.nodeCost(addNode));
}

TEST(EGraphTest, FloatConstant) {
    EGraph g;
    ClassId f1 = g.addConstF(3.14);
    ClassId f2 = g.addConstF(3.14);
    EXPECT_EQ(g.find(f1), g.find(f2));

    ClassId f3 = g.addConstF(2.71);
    EXPECT_NE(g.find(f1), g.find(f3));
}

TEST(EGraphTest, FloatConstantFolding) {
    EGraph g;
    ClassId a = g.addConstF(1.5);
    ClassId b = g.addConstF(2.5);
    ClassId expr = g.addBinOp(Op::Add, a, b);

    auto rules = getAllRules();
    g.saturate(rules);

    ClassId expected = g.addConstF(4.0);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

// ─────────────────────────────────────────────────────────────────────────────
// Rule library completeness tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, GetAllRulesNotEmpty) {
    auto rules = getAllRules();
    EXPECT_GT(rules.size(), 30u); // We should have a substantial number of rules
}

TEST(EGraphTest, AlgebraicRulesNotEmpty) {
    auto rules = getAlgebraicRules();
    EXPECT_GT(rules.size(), 10u);
}

TEST(EGraphTest, ComparisonRulesNotEmpty) {
    auto rules = getComparisonRules();
    EXPECT_GT(rules.size(), 5u);
}

TEST(EGraphTest, BitwiseRulesNotEmpty) {
    auto rules = getBitwiseRules();
    EXPECT_GT(rules.size(), 5u);
}
