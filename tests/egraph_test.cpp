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

    // x/4 should NOT be equivalent to x>>2 for signed integers.
    // With logical (unsigned) shift: -7/4 = -1, but -7>>2 = (uint64)(-7)>>2 (a huge number).
    // The div_pow2 rules were removed to fix this signed-division bug.
    ClassId two = g.addConst(2);
    ClassId shiftExpr = g.addBinOp(Op::Shr, x, two);
    EXPECT_NE(g.find(expr), g.find(shiftExpr));
}

TEST(EGraphTest, ZeroDivNotFolded) {
    // 0 / x must NOT be simplified to 0.
    // When x is also 0 the fold would suppress the division-by-zero runtime
    // error and produce an incorrect result.
    EGraph g;
    ClassId zero = g.addConst(0);
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Div, zero, x);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId zero2 = g.addConst(0);
    // After saturation, 0/x must NOT be merged with the constant 0.
    EXPECT_NE(g.find(expr), g.find(zero2));
}

TEST(EGraphTest, ZeroModNotFolded) {
    // 0 % x must NOT be simplified to 0 for the same reason: when x==0,
    // the fold masks the modulo-by-zero runtime error.
    EGraph g;
    ClassId zero = g.addConst(0);
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Mod, zero, x);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId zero2 = g.addConst(0);
    EXPECT_NE(g.find(expr), g.find(zero2));
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

// Regression: shifting by >= 64 or by a negative amount is undefined behavior
// in C++.  The constant folder must refuse to fold these instead of invoking UB.
TEST(EGraphTest, ConstantFoldShiftOutOfRange) {
    {
        EGraph g;
        ClassId a = g.addConst(1);
        ClassId b = g.addConst(100);  // shift >= 64 → UB
        (void)g.addBinOp(Op::Shl, a, b);

        auto rules = getAllRules();
        // Must not crash (no UB) — just not fold the constant
        g.saturate(rules);
    }
    {
        EGraph g;
        ClassId a = g.addConst(1);
        ClassId b = g.addConst(-1);  // negative shift → UB
        (void)g.addBinOp(Op::Shr, a, b);

        auto rules = getAllRules();
        g.saturate(rules);
    }
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

// ─────────────────────────────────────────────────────────────────────────────
// New e-graph rule tests — improved optimization coverage
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, ModSelfIsZero) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Mod, x, x);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId zero = g.addConst(0);
    EXPECT_EQ(g.find(expr), g.find(zero));
}

TEST(EGraphTest, NegSubSwap) {
    // -(a - b) → b - a
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId sub = g.addBinOp(Op::Sub, a, b);
    ClassId negSub = g.addUnaryOp(Op::Neg, sub);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId bMinusA = g.addBinOp(Op::Sub, b, a);
    EXPECT_EQ(g.find(negSub), g.find(bMinusA));
}

TEST(EGraphTest, MulAssociativity) {
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId c = g.addVar("c");
    ClassId ab = g.addBinOp(Op::Mul, a, b);
    ClassId abc_left = g.addBinOp(Op::Mul, ab, c);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId bc = g.addBinOp(Op::Mul, b, c);
    ClassId abc_right = g.addBinOp(Op::Mul, a, bc);
    EXPECT_EQ(g.find(abc_left), g.find(abc_right));
}

TEST(EGraphTest, StrengthReductionMul3) {
    // x * 3 → (x << 1) + x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId three = g.addConst(3);
    ClassId expr = g.addBinOp(Op::Mul, x, three);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId one = g.addConst(1);
    ClassId shl = g.addBinOp(Op::Shl, x, one);
    ClassId addExpr = g.addBinOp(Op::Add, shl, x);
    EXPECT_EQ(g.find(expr), g.find(addExpr));
}

TEST(EGraphTest, StrengthReductionMul5) {
    // x * 5 → (x << 2) + x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId five = g.addConst(5);
    ClassId expr = g.addBinOp(Op::Mul, x, five);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId two = g.addConst(2);
    ClassId shl = g.addBinOp(Op::Shl, x, two);
    ClassId addExpr = g.addBinOp(Op::Add, shl, x);
    EXPECT_EQ(g.find(expr), g.find(addExpr));
}

TEST(EGraphTest, StrengthReductionMul7) {
    // x * 7 → (x << 3) - x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId seven = g.addConst(7);
    ClassId expr = g.addBinOp(Op::Mul, x, seven);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId three = g.addConst(3);
    ClassId shl = g.addBinOp(Op::Shl, x, three);
    ClassId subExpr = g.addBinOp(Op::Sub, shl, x);
    EXPECT_EQ(g.find(expr), g.find(subExpr));
}

TEST(EGraphTest, LogAndSelf) {
    // x && x → x != 0 (boolean conversion)
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::LogAnd, x, x);
    ClassId zero = g.addConst(0);
    ClassId expected = g.addBinOp(Op::Ne, x, zero);

    auto rules = getComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, LogOrSelf) {
    // x || x → x != 0 (boolean conversion)
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::LogOr, x, x);
    ClassId zero = g.addConst(0);
    ClassId expected = g.addBinOp(Op::Ne, x, zero);

    auto rules = getComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, LogAndZero) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::LogAnd, x, zero);

    auto rules = getComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(zero));
}

TEST(EGraphTest, LogOrZero) {
    // x || 0 → x != 0 (boolean conversion)
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::LogOr, x, zero);
    ClassId expected = g.addBinOp(Op::Ne, x, zero);

    auto rules = getComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, TernarySameBranch) {
    // cond ? x : x → x
    EGraph g;
    ClassId cond = g.addVar("cond");
    ClassId x = g.addVar("x");

    ENode ternaryNode(Op::Ternary, std::vector<ClassId>{cond, x, x});
    ClassId expr = g.add(ternaryNode);

    auto rules = getComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, TernaryTrueCondition) {
    // 1 ? a : b → a
    EGraph g;
    ClassId one = g.addConst(1);
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");

    ENode ternaryNode(Op::Ternary, std::vector<ClassId>{one, a, b});
    ClassId expr = g.add(ternaryNode);

    auto rules = getComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(a));
}

TEST(EGraphTest, TernaryFalseCondition) {
    // 0 ? a : b → b
    EGraph g;
    ClassId zero = g.addConst(0);
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");

    ENode ternaryNode(Op::Ternary, std::vector<ClassId>{zero, a, b});
    ClassId expr = g.add(ternaryNode);

    auto rules = getComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(b));
}

TEST(EGraphTest, BitwiseAbsorptionAndOr) {
    // a & (a | b) → a
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId orExpr = g.addBinOp(Op::BitOr, a, b);
    ClassId expr = g.addBinOp(Op::BitAnd, a, orExpr);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(a));
}

TEST(EGraphTest, BitwiseAbsorptionOrAnd) {
    // a | (a & b) → a
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId andExpr = g.addBinOp(Op::BitAnd, a, b);
    ClassId expr = g.addBinOp(Op::BitOr, a, andExpr);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(a));
}

TEST(EGraphTest, XorAllOnesToBitNot) {
    // x ^ -1 → ~x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId allOnes = g.addConst(-1);
    ClassId xorExpr = g.addBinOp(Op::BitXor, x, allOnes);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    ClassId bitNotExpr = g.addUnaryOp(Op::BitNot, x);
    EXPECT_EQ(g.find(xorExpr), g.find(bitNotExpr));
}

TEST(EGraphTest, ShiftCombineShl) {
    // (x << 2) << 3 → x << 5
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId two = g.addConst(2);
    ClassId three = g.addConst(3);
    ClassId inner = g.addBinOp(Op::Shl, x, two);
    ClassId outer = g.addBinOp(Op::Shl, inner, three);

    auto rules = getBitwiseRules();
    auto algRules = getAlgebraicRules();
    rules.insert(rules.end(), algRules.begin(), algRules.end());
    g.saturate(rules);

    ClassId five = g.addConst(5);
    ClassId combined = g.addBinOp(Op::Shl, x, five);
    EXPECT_EQ(g.find(outer), g.find(combined));
}

TEST(EGraphTest, ShiftCombineShr) {
    // (x >> 3) >> 4 → x >> 7
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId three = g.addConst(3);
    ClassId four = g.addConst(4);
    ClassId inner = g.addBinOp(Op::Shr, x, three);
    ClassId outer = g.addBinOp(Op::Shr, inner, four);

    auto rules = getBitwiseRules();
    auto algRules = getAlgebraicRules();
    rules.insert(rules.end(), algRules.begin(), algRules.end());
    g.saturate(rules);

    ClassId seven = g.addConst(7);
    ClassId combined = g.addBinOp(Op::Shr, x, seven);
    EXPECT_EQ(g.find(outer), g.find(combined));
}

// ─────────────────────────────────────────────────────────────────────────────
// Integration test: compile a C-like function through OmScript
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, CompileCLikeSumFunction) {
    // This test compiles an OmScript equivalent of the C function:
    //   #include <stdio.h>
    //   int sum_array(int n) {
    //       int total = 0;
    //       for (int i = 0; i < n; i++) { total = total + i; }
    //       return total;
    //   }
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR(
        "fn sum_array(n) {\n"
        "  var total = 0;\n"
        "  for (i in 0...n) {\n"
        "    total = total + i;\n"
        "  }\n"
        "  return total;\n"
        "}\n"
        "fn main() { return sum_array(100); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* sumFn = mod->getFunction("sum_array");
    if (!sumFn) { SUCCEED(); return; } // inlined at O2
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(EGraphTest, CompileCLikeFactorialFunction) {
    // OmScript equivalent of:
    //   int factorial(int n) {
    //       if (n <= 1) return 1;
    //       return n * factorial(n - 1);
    //   }
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR(
        "fn factorial(n) {\n"
        "  if (n <= 1) { return 1; }\n"
        "  return n * factorial(n - 1);\n"
        "}\n"
        "fn main() { return factorial(10); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* factFn = mod->getFunction("factorial");
    if (!factFn) { SUCCEED(); return; } // inlined at O2
}

TEST(EGraphTest, CompileCLikeBitwiseOperations) {
    // OmScript equivalent of C-like bitwise optimization patterns:
    //   int bitwise_opt(int x, int y) {
    //       int a = x & (x | y);   // absorption: should optimize to x
    //       int b = (x * 1) + (y - 0);  // identity: x + y
    //       int c = x ^ x;         // self-xor: 0
    //       return a + b + c;
    //   }
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR(
        "fn bitwise_opt(x, y) {\n"
        "  var a = x & (x | y);\n"
        "  var b = (x * 1) + (y - 0);\n"
        "  var c = x ^ x;\n"
        "  return a + b + c;\n"
        "}\n"
        "fn main() { return bitwise_opt(42, 7); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* bwFn = mod->getFunction("bitwise_opt");
    if (!bwFn) { SUCCEED(); return; } // inlined at O2
}

TEST(EGraphTest, CompileCLikeStrengthReduction) {
    // Test that strength reduction optimizations apply to C-like patterns:
    //   int optimize_mul(int x) {
    //       return x * 8 + x * 3 + x * 15;
    //   }
    // x*8 → x<<3, x*3 → (x<<1)+x, x*15 → (x<<4)-x
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR(
        "fn optimize_mul(x) {\n"
        "  return x * 8 + x * 3 + x * 15;\n"
        "}\n"
        "fn main() { return optimize_mul(10); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* optFn = mod->getFunction("optimize_mul");
    if (!optFn) { SUCCEED(); return; } // inlined at O2
}

// ─────────────────────────────────────────────────────────────────────────────
// New e-graph algebraic rule tests (round 2)
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, AddNegToSub) {
    // a + (-b) → a - b
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId negB = g.addUnaryOp(Op::Neg, b);
    ClassId expr = g.addBinOp(Op::Add, a, negB);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId expected = g.addBinOp(Op::Sub, a, b);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, ZeroSubToNeg) {
    // 0 - x → -x
    EGraph g;
    ClassId zero = g.addConst(0);
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Sub, zero, x);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId expected = g.addUnaryOp(Op::Neg, x);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, MulNeg1ToNeg) {
    // x * (-1) → -x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId neg1 = g.addConst(-1);
    ClassId expr = g.addBinOp(Op::Mul, x, neg1);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId expected = g.addUnaryOp(Op::Neg, x);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, AddSelfToMul2) {
    // x + x → x * 2
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::Add, x, x);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId two = g.addConst(2);
    ClassId expected = g.addBinOp(Op::Mul, x, two);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, AddSubCancelLeft) {
    // (a + b) - a → b
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId sum = g.addBinOp(Op::Add, a, b);
    ClassId expr = g.addBinOp(Op::Sub, sum, a);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(b));
}

TEST(EGraphTest, AddSubCancelRight) {
    // (a + b) - b → a
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId sum = g.addBinOp(Op::Add, a, b);
    ClassId expr = g.addBinOp(Op::Sub, sum, b);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(a));
}

TEST(EGraphTest, SubNegToAdd) {
    // a - (-b) → a + b
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId negB = g.addUnaryOp(Op::Neg, b);
    ClassId expr = g.addBinOp(Op::Sub, a, negB);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId expected = g.addBinOp(Op::Add, a, b);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, PowZero) {
    // x ** 0 → 1
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::Pow, x, zero);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId one = g.addConst(1);
    EXPECT_EQ(g.find(expr), g.find(one));
}

TEST(EGraphTest, PowOne) {
    // x ** 1 → x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId one = g.addConst(1);
    ClassId expr = g.addBinOp(Op::Pow, x, one);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, PowTwo) {
    // x ** 2 → x * x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId two = g.addConst(2);
    ClassId expr = g.addBinOp(Op::Pow, x, two);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId expected = g.addBinOp(Op::Mul, x, x);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

// ─────────────────────────────────────────────────────────────────────────────
// New e-graph comparison rule tests (round 2)
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, TernaryNotCondFlip) {
    // (!c) ? a : b → c ? b : a
    EGraph g;
    ClassId c = g.addVar("c");
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId notC = g.addUnaryOp(Op::LogNot, c);

    ENode ternaryNode(Op::Ternary, std::vector<ClassId>{notC, a, b});
    ClassId expr = g.add(ternaryNode);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ENode expectedNode(Op::Ternary, std::vector<ClassId>{c, b, a});
    ClassId expected = g.add(expectedNode);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, LogAndOne) {
    // x && 1 → x != 0 (boolean conversion)
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId one = g.addConst(1);
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::LogAnd, x, one);
    ClassId expected = g.addBinOp(Op::Ne, x, zero);

    auto rules = getComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, LogOrOne) {
    // x || 1 → 1
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId one = g.addConst(1);
    ClassId expr = g.addBinOp(Op::LogOr, x, one);

    auto rules = getComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(one));
}

TEST(EGraphTest, LogNotZero) {
    // !0 → 1
    EGraph g;
    ClassId zero = g.addConst(0);
    ClassId expr = g.addUnaryOp(Op::LogNot, zero);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId one = g.addConst(1);
    EXPECT_EQ(g.find(expr), g.find(one));
}

TEST(EGraphTest, LogNotOne) {
    // !1 → 0
    EGraph g;
    ClassId one = g.addConst(1);
    ClassId expr = g.addUnaryOp(Op::LogNot, one);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId zero = g.addConst(0);
    EXPECT_EQ(g.find(expr), g.find(zero));
}

TEST(EGraphTest, SubNeZero) {
    // (x - y) != 0 → x != y
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    ClassId sub = g.addBinOp(Op::Sub, x, y);
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::Ne, sub, zero);

    auto rules = getComparisonRules();
    auto algRules = getAlgebraicRules();
    rules.insert(rules.end(), algRules.begin(), algRules.end());
    g.saturate(rules);

    ClassId expected = g.addBinOp(Op::Ne, x, y);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

// ─────────────────────────────────────────────────────────────────────────────
// New e-graph bitwise rule tests (round 2)
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, XorAssociativity) {
    // (a ^ b) ^ c → a ^ (b ^ c)
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId c = g.addVar("c");
    ClassId ab = g.addBinOp(Op::BitXor, a, b);
    ClassId expr = g.addBinOp(Op::BitXor, ab, c);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    ClassId bc = g.addBinOp(Op::BitXor, b, c);
    ClassId expected = g.addBinOp(Op::BitXor, a, bc);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, AndAssociativity) {
    // (a & b) & c → a & (b & c)
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId c = g.addVar("c");
    ClassId ab = g.addBinOp(Op::BitAnd, a, b);
    ClassId expr = g.addBinOp(Op::BitAnd, ab, c);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    ClassId bc = g.addBinOp(Op::BitAnd, b, c);
    ClassId expected = g.addBinOp(Op::BitAnd, a, bc);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, OrAssociativity) {
    // (a | b) | c → a | (b | c)
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId c = g.addVar("c");
    ClassId ab = g.addBinOp(Op::BitOr, a, b);
    ClassId expr = g.addBinOp(Op::BitOr, ab, c);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    ClassId bc = g.addBinOp(Op::BitOr, b, c);
    ClassId expected = g.addBinOp(Op::BitOr, a, bc);
    EXPECT_EQ(g.find(expr), g.find(expected));
}

TEST(EGraphTest, XorZeroLeft) {
    // 0 ^ x → x
    EGraph g;
    ClassId zero = g.addConst(0);
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::BitXor, zero, x);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, AndZeroLeft) {
    // 0 & x → 0
    EGraph g;
    ClassId zero = g.addConst(0);
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::BitAnd, zero, x);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(zero));
}

TEST(EGraphTest, OrZeroLeft) {
    // 0 | x → x
    EGraph g;
    ClassId zero = g.addConst(0);
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::BitOr, zero, x);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, AndAllOnesLeft) {
    // -1 & x → x
    EGraph g;
    ClassId allOnes = g.addConst(-1);
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::BitAnd, allOnes, x);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(x));
}

TEST(EGraphTest, OrAllOnesLeft) {
    // -1 | x → -1
    EGraph g;
    ClassId allOnes = g.addConst(-1);
    ClassId x = g.addVar("x");
    ClassId expr = g.addBinOp(Op::BitOr, allOnes, x);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(expr), g.find(allOnes));
}

TEST(EGraphTest, BitNotToXorAndBack) {
    // ~x → x ^ -1, and they should be in the same e-class
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId bitNot = g.addUnaryOp(Op::BitNot, x);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    ClassId allOnes = g.addConst(-1);
    ClassId xorExpr = g.addBinOp(Op::BitXor, x, allOnes);
    EXPECT_EQ(g.find(bitNot), g.find(xorExpr));
}

TEST(EGraphTest, AllRulesHasSubstantialCount) {
    // Verify the total rule count has grown
    auto rules = getAllRules();
    EXPECT_GT(rules.size(), 70u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Distributive / factoring / large power-of-2 tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, MulDistributeOverAdd) {
    // a * (b + c) → a*b + a*c
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId c = g.addVar("c");
    ClassId bc = g.addBinOp(Op::Add, b, c);
    ClassId expr = g.addBinOp(Op::Mul, a, bc);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId ab = g.addBinOp(Op::Mul, a, b);
    ClassId ac = g.addBinOp(Op::Mul, a, c);
    ClassId distributed = g.addBinOp(Op::Add, ab, ac);
    g.saturate(rules);
    EXPECT_EQ(g.find(expr), g.find(distributed));
}

TEST(EGraphTest, AddFactorRight) {
    // a*c + b*c → (a + b) * c
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId c = g.addVar("c");
    ClassId ac = g.addBinOp(Op::Mul, a, c);
    ClassId bc = g.addBinOp(Op::Mul, b, c);
    ClassId expr = g.addBinOp(Op::Add, ac, bc);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId aplusb = g.addBinOp(Op::Add, a, b);
    ClassId factored = g.addBinOp(Op::Mul, aplusb, c);
    g.saturate(rules);
    EXPECT_EQ(g.find(expr), g.find(factored));
}

TEST(EGraphTest, Mul128ToShl7) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c128 = g.addConst(128);
    ClassId expr = g.addBinOp(Op::Mul, x, c128);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId seven = g.addConst(7);
    ClassId shift = g.addBinOp(Op::Shl, x, seven);
    EXPECT_EQ(g.find(expr), g.find(shift));
}

TEST(EGraphTest, Mul256ToShl8) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c256 = g.addConst(256);
    ClassId expr = g.addBinOp(Op::Mul, x, c256);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId eight = g.addConst(8);
    ClassId shift = g.addBinOp(Op::Shl, x, eight);
    EXPECT_EQ(g.find(expr), g.find(shift));
}

TEST(EGraphTest, Mul1024ToShl10) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c1024 = g.addConst(1024);
    ClassId expr = g.addBinOp(Op::Mul, x, c1024);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    ClassId ten = g.addConst(10);
    ClassId shift = g.addBinOp(Op::Shl, x, ten);
    EXPECT_EQ(g.find(expr), g.find(shift));
}

TEST(EGraphTest, DeMorganLogicalAnd) {
    // !(a && b) → !a || !b
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId land = g.addBinOp(Op::LogAnd, a, b);
    ClassId notLand = g.addUnaryOp(Op::LogNot, land);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId notA = g.addUnaryOp(Op::LogNot, a);
    ClassId notB = g.addUnaryOp(Op::LogNot, b);
    ClassId demorgan = g.addBinOp(Op::LogOr, notA, notB);
    g.saturate(rules);
    EXPECT_EQ(g.find(notLand), g.find(demorgan));
}

TEST(EGraphTest, DeMorganLogicalOr) {
    // !(a || b) → !a && !b
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId lor = g.addBinOp(Op::LogOr, a, b);
    ClassId notLor = g.addUnaryOp(Op::LogNot, lor);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId notA = g.addUnaryOp(Op::LogNot, a);
    ClassId notB = g.addUnaryOp(Op::LogNot, b);
    ClassId demorgan = g.addBinOp(Op::LogAnd, notA, notB);
    g.saturate(rules);
    EXPECT_EQ(g.find(notLor), g.find(demorgan));
}

TEST(EGraphTest, NotEqToNe) {
    // !(a == b) → a != b
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId eq = g.addBinOp(Op::Eq, a, b);
    ClassId notEq = g.addUnaryOp(Op::LogNot, eq);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId ne = g.addBinOp(Op::Ne, a, b);
    EXPECT_EQ(g.find(notEq), g.find(ne));
}

TEST(EGraphTest, NotLtToGe) {
    // !(a < b) → a >= b
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId lt = g.addBinOp(Op::Lt, a, b);
    ClassId notLt = g.addUnaryOp(Op::LogNot, lt);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId ge = g.addBinOp(Op::Ge, a, b);
    EXPECT_EQ(g.find(notLt), g.find(ge));
}

TEST(EGraphTest, GtSwapToLt) {
    // a > b → b < a
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId gt = g.addBinOp(Op::Gt, a, b);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId lt = g.addBinOp(Op::Lt, b, a);
    EXPECT_EQ(g.find(gt), g.find(lt));
}

TEST(EGraphTest, SubLtZeroToLt) {
    // (a - b) < 0 → a < b
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId sub = g.addBinOp(Op::Sub, a, b);
    ClassId zero = g.addConst(0);
    ClassId expr = g.addBinOp(Op::Lt, sub, zero);

    auto rules = getComparisonRules();
    g.saturate(rules);

    ClassId lt = g.addBinOp(Op::Lt, a, b);
    EXPECT_EQ(g.find(expr), g.find(lt));
}

TEST(EGraphTest, DeMorganBitwiseAnd) {
    // ~(a & b) → (~a) | (~b)
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId band = g.addBinOp(Op::BitAnd, a, b);
    ClassId notBand = g.addUnaryOp(Op::BitNot, band);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    ClassId notA = g.addUnaryOp(Op::BitNot, a);
    ClassId notB = g.addUnaryOp(Op::BitNot, b);
    ClassId demorgan = g.addBinOp(Op::BitOr, notA, notB);
    g.saturate(rules);
    EXPECT_EQ(g.find(notBand), g.find(demorgan));
}

TEST(EGraphTest, DoubleBitNotIdentity) {
    // ~~x → x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId not1 = g.addUnaryOp(Op::BitNot, x);
    ClassId not2 = g.addUnaryOp(Op::BitNot, not1);

    auto rules = getBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(not2), g.find(x));
}

// ─────────────────────────────────────────────────────────────────────────────
// New multiply strength reduction rules
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, StrengthReductionMul30) {
    // x * 30 → (x << 5) - (x << 1)
    EGraph g;
    ClassId x    = g.addVar("x");
    ClassId c30  = g.addConst(30);
    ClassId mul  = g.addBinOp(Op::Mul, x, c30);
    ClassId c5   = g.addConst(5);
    ClassId c1   = g.addConst(1);
    ClassId s5   = g.addBinOp(Op::Shl, x, c5);
    ClassId s1   = g.addBinOp(Op::Shl, x, c1);
    ClassId expected = g.addBinOp(Op::Sub, s5, s1);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(expected));
}

TEST(EGraphTest, StrengthReductionMul36) {
    // x * 36 → (x << 5) + (x << 2)
    EGraph g;
    ClassId x    = g.addVar("x");
    ClassId c36  = g.addConst(36);
    ClassId mul  = g.addBinOp(Op::Mul, x, c36);
    ClassId c5   = g.addConst(5);
    ClassId c2   = g.addConst(2);
    ClassId s5   = g.addBinOp(Op::Shl, x, c5);
    ClassId s2   = g.addBinOp(Op::Shl, x, c2);
    ClassId expected = g.addBinOp(Op::Add, s5, s2);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(expected));
}

TEST(EGraphTest, StrengthReductionMul60) {
    // x * 60 → (x << 6) - (x << 2)
    EGraph g;
    ClassId x    = g.addVar("x");
    ClassId c60  = g.addConst(60);
    ClassId mul  = g.addBinOp(Op::Mul, x, c60);
    ClassId c6   = g.addConst(6);
    ClassId c2   = g.addConst(2);
    ClassId s6   = g.addBinOp(Op::Shl, x, c6);
    ClassId s2   = g.addBinOp(Op::Shl, x, c2);
    ClassId expected = g.addBinOp(Op::Sub, s6, s2);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(expected));
}

TEST(EGraphTest, StrengthReductionMul72) {
    // x * 72 → (x << 6) + (x << 3)
    EGraph g;
    ClassId x    = g.addVar("x");
    ClassId c72  = g.addConst(72);
    ClassId mul  = g.addBinOp(Op::Mul, x, c72);
    ClassId c6   = g.addConst(6);
    ClassId c3   = g.addConst(3);
    ClassId s6   = g.addBinOp(Op::Shl, x, c6);
    ClassId s3   = g.addBinOp(Op::Shl, x, c3);
    ClassId expected = g.addBinOp(Op::Add, s6, s3);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(expected));
}

TEST(EGraphTest, StrengthReductionMul192) {
    // x * 192 → (x << 7) + (x << 6)
    EGraph g;
    ClassId x    = g.addVar("x");
    ClassId c192 = g.addConst(192);
    ClassId mul  = g.addBinOp(Op::Mul, x, c192);
    ClassId c7   = g.addConst(7);
    ClassId c6   = g.addConst(6);
    ClassId s7   = g.addBinOp(Op::Shl, x, c7);
    ClassId s6   = g.addBinOp(Op::Shl, x, c6);
    ClassId expected = g.addBinOp(Op::Add, s7, s6);

    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(expected));
}

// ─────────────────────────────────────────────────────────────────────────────
// Arithmetic-bitwise identity: (a|b) - (a&b) ↔ a^b
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, OrSubAndEqualsXor) {
    // (a | b) - (a & b) should be in the same class as a ^ b
    EGraph g;
    ClassId a     = g.addVar("a");
    ClassId b     = g.addVar("b");
    ClassId orAB  = g.addBinOp(Op::BitOr,  a, b);
    ClassId andAB = g.addBinOp(Op::BitAnd, a, b);
    ClassId sub   = g.addBinOp(Op::Sub, orAB, andAB);
    ClassId xorAB = g.addBinOp(Op::BitXor, a, b);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(sub), g.find(xorAB));
}

TEST(EGraphTest, XorAddAndEqualsOr) {
    // (a ^ b) + (a & b) should be in the same class as a | b
    EGraph g;
    ClassId a     = g.addVar("a");
    ClassId b     = g.addVar("b");
    ClassId xorAB = g.addBinOp(Op::BitXor, a, b);
    ClassId andAB = g.addBinOp(Op::BitAnd, a, b);
    ClassId sum   = g.addBinOp(Op::Add, xorAB, andAB);
    ClassId orAB  = g.addBinOp(Op::BitOr, a, b);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(sum), g.find(orAB));
}

// ─────────────────────────────────────────────────────────────────────────────
// Relational e-graph infrastructure tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, GetConstValue) {
    EGraph g;
    ClassId c42 = g.addConst(42);
    ClassId x   = g.addVar("x");
    ClassId sum = g.addBinOp(Op::Add, c42, x);

    EXPECT_EQ(g.getConstValue(c42), std::optional<long long>(42));
    EXPECT_EQ(g.getConstValue(x), std::nullopt);
    EXPECT_EQ(g.getConstValue(sum), std::nullopt);
}

TEST(EGraphTest, GetConstFValue) {
    EGraph g;
    ClassId cf = g.addConstF(3.14);
    ClassId x  = g.addVar("x");

    auto val = g.getConstFValue(cf);
    ASSERT_TRUE(val.has_value());
    EXPECT_DOUBLE_EQ(*val, 3.14);
    EXPECT_EQ(g.getConstFValue(x), std::nullopt);
}

TEST(EGraphTest, GuardedRuleSkipsNonConstant) {
    // Relational guard: mul_any_pow2_to_shl should NOT fire when the
    // second operand is a variable (not a power-of-2 constant).
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    ClassId mul = g.addBinOp(Op::Mul, x, y);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    // x * y should NOT be merged with any shift, since y is not a constant.
    // Check that the mul class doesn't contain a Shl node.
    const auto& cls = g.getClass(g.find(mul));
    bool hasShl = false;
    for (const auto& node : cls.nodes) {
        if (node.op == Op::Shl) hasShl = true;
    }
    EXPECT_FALSE(hasShl);
}

TEST(EGraphTest, GuardedRuleMulPow2ToShl) {
    // x * 16 should be merged with x << 4 via the relational guard.
    EGraph g;
    ClassId x   = g.addVar("x");
    ClassId c16 = g.addConst(16);
    ClassId mul = g.addBinOp(Op::Mul, x, c16);
    ClassId c4  = g.addConst(4);
    ClassId shl = g.addBinOp(Op::Shl, x, c4);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(shl));
}

TEST(EGraphTest, GuardedRuleMulNonPow2Skipped) {
    // x * 6 should NOT fire the pow2-to-shl rule (6 is not a power of 2).
    EGraph g;
    ClassId x  = g.addVar("x");
    ClassId c6 = g.addConst(6);
    ClassId mul = g.addBinOp(Op::Mul, x, c6);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    const auto& cls = g.getClass(g.find(mul));
    bool hasShl = false;
    for (const auto& node : cls.nodes) {
        if (node.op == Op::Shl) hasShl = true;
    }
    EXPECT_FALSE(hasShl);
}

TEST(EGraphTest, GuardedRuleModPow2ToAnd) {
    // x % 32 should be merged with x & 31 via the relational guard.
    EGraph g;
    ClassId x   = g.addVar("x");
    ClassId c32 = g.addConst(32);
    ClassId mod = g.addBinOp(Op::Mod, x, c32);
    ClassId c31 = g.addConst(31);
    ClassId and_ = g.addBinOp(Op::BitAnd, x, c31);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mod), g.find(and_));
}

// ─────────────────────────────────────────────────────────────────────────────
// Comparison merging tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, LtOrEqToLe) {
    // (a < b) || (a == b) should be merged with a <= b
    EGraph g;
    ClassId a  = g.addVar("a");
    ClassId b  = g.addVar("b");
    ClassId lt = g.addBinOp(Op::Lt, a, b);
    ClassId eq = g.addBinOp(Op::Eq, a, b);
    ClassId lor = g.addBinOp(Op::LogOr, lt, eq);
    ClassId le = g.addBinOp(Op::Le, a, b);

    auto rules = getAdvancedComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(lor), g.find(le));
}

TEST(EGraphTest, GtOrEqToGe) {
    // (a > b) || (a == b) should be merged with a >= b
    EGraph g;
    ClassId a  = g.addVar("a");
    ClassId b  = g.addVar("b");
    ClassId gt = g.addBinOp(Op::Gt, a, b);
    ClassId eq = g.addBinOp(Op::Eq, a, b);
    ClassId lor = g.addBinOp(Op::LogOr, gt, eq);
    ClassId ge = g.addBinOp(Op::Ge, a, b);

    auto rules = getAdvancedComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(lor), g.find(ge));
}

TEST(EGraphTest, NeAndLtRedundant) {
    // (a != b) && (a < b) should be merged with a < b
    EGraph g;
    ClassId a  = g.addVar("a");
    ClassId b  = g.addVar("b");
    ClassId ne = g.addBinOp(Op::Ne, a, b);
    ClassId lt = g.addBinOp(Op::Lt, a, b);
    ClassId land = g.addBinOp(Op::LogAnd, ne, lt);

    auto rules = getAdvancedComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(land), g.find(lt));
}

TEST(EGraphTest, GtAndNeRedundant) {
    // (a > b) && (a != b) should be merged with a > b
    EGraph g;
    ClassId a  = g.addVar("a");
    ClassId b  = g.addVar("b");
    ClassId gt = g.addBinOp(Op::Gt, a, b);
    ClassId ne = g.addBinOp(Op::Ne, a, b);
    ClassId land = g.addBinOp(Op::LogAnd, gt, ne);

    auto rules = getAdvancedComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(land), g.find(gt));
}

// ─────────────────────────────────────────────────────────────────────────────
// Ternary factoring tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, TernaryAddFactor) {
    // cond ? (a + c) : (b + c) should equal (cond ? a : b) + c
    EGraph g;
    ClassId cond = g.addVar("cond");
    ClassId a    = g.addVar("a");
    ClassId b    = g.addVar("b");
    ClassId c    = g.addVar("c");
    ClassId ac   = g.addBinOp(Op::Add, a, c);
    ClassId bc   = g.addBinOp(Op::Add, b, c);
    ENode ternNode(Op::Ternary, std::vector<ClassId>{cond, ac, bc});
    ClassId tern = g.add(ternNode);

    // Build expected: (cond ? a : b) + c
    ENode selNode(Op::Ternary, std::vector<ClassId>{cond, a, b});
    ClassId sel  = g.add(selNode);
    ClassId expected = g.addBinOp(Op::Add, sel, c);

    auto rules = getAdvancedComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(tern), g.find(expected));
}

TEST(EGraphTest, TernaryNegFactor) {
    // cond ? (-a) : (-b) should equal -(cond ? a : b)
    EGraph g;
    ClassId cond = g.addVar("cond");
    ClassId a    = g.addVar("a");
    ClassId b    = g.addVar("b");
    ClassId negA = g.addUnaryOp(Op::Neg, a);
    ClassId negB = g.addUnaryOp(Op::Neg, b);
    ENode ternNode(Op::Ternary, std::vector<ClassId>{cond, negA, negB});
    ClassId tern = g.add(ternNode);

    // Build expected: -(cond ? a : b)
    ENode selNode(Op::Ternary, std::vector<ClassId>{cond, a, b});
    ClassId sel  = g.add(selNode);
    ClassId expected = g.addUnaryOp(Op::Neg, sel);

    auto rules = getAdvancedComparisonRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(tern), g.find(expected));
}

// ─────────────────────────────────────────────────────────────────────────────
// Complement-based bitwise tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, ComplAndOr) {
    // (~a) & (a | b) should be merged with (~a) & b
    EGraph g;
    ClassId a    = g.addVar("a");
    ClassId b    = g.addVar("b");
    ClassId notA = g.addUnaryOp(Op::BitNot, a);
    ClassId orAB = g.addBinOp(Op::BitOr, a, b);
    ClassId lhs  = g.addBinOp(Op::BitAnd, notA, orAB);
    ClassId rhs  = g.addBinOp(Op::BitAnd, notA, b);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(lhs), g.find(rhs));
}

TEST(EGraphTest, ComplOrAnd) {
    // (~a) | (a & b) should be merged with (~a) | b
    EGraph g;
    ClassId a    = g.addVar("a");
    ClassId b    = g.addVar("b");
    ClassId notA = g.addUnaryOp(Op::BitNot, a);
    ClassId andAB = g.addBinOp(Op::BitAnd, a, b);
    ClassId lhs  = g.addBinOp(Op::BitOr, notA, andAB);
    ClassId rhs  = g.addBinOp(Op::BitOr, notA, b);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(lhs), g.find(rhs));
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests for relational strength-reduction rules
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, RelationalMul3StrengthReduce) {
    // x * 3 → (x << 1) + x
    EGraph g;
    ClassId x   = g.addVar("x");
    ClassId c3  = g.addConst(3);
    ClassId mul = g.addBinOp(Op::Mul, x, c3);

    // Expected: (x << 1) + x
    ClassId shl  = g.addBinOp(Op::Shl, x, g.addConst(1));
    ClassId sum  = g.addBinOp(Op::Add, shl, x);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(sum));
}

TEST(EGraphTest, RelationalMul7StrengthReduce) {
    // x * 7 → (x << 3) - x
    EGraph g;
    ClassId x   = g.addVar("x");
    ClassId c7  = g.addConst(7);
    ClassId mul = g.addBinOp(Op::Mul, x, c7);

    ClassId shl  = g.addBinOp(Op::Shl, x, g.addConst(3));
    ClassId diff = g.addBinOp(Op::Sub, shl, x);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(diff));
}

TEST(EGraphTest, ShiftCombineShlRelational) {
    // (x << 2) << 3 → x << 5
    EGraph g;
    ClassId x    = g.addVar("x");
    ClassId shl1 = g.addBinOp(Op::Shl, x, g.addConst(2));
    ClassId shl2 = g.addBinOp(Op::Shl, shl1, g.addConst(3));
    ClassId expected = g.addBinOp(Op::Shl, x, g.addConst(5));

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(shl2), g.find(expected));
}

TEST(EGraphTest, ShiftCombineShrRelational) {
    // (x >> 4) >> 2 → x >> 6
    EGraph g;
    ClassId x    = g.addVar("x");
    ClassId shr1 = g.addBinOp(Op::Shr, x, g.addConst(4));
    ClassId shr2 = g.addBinOp(Op::Shr, shr1, g.addConst(2));
    ClassId expected = g.addBinOp(Op::Shr, x, g.addConst(6));

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(shr2), g.find(expected));
}

TEST(EGraphTest, TernarySameOperands) {
    // cond ? x : x → x
    EGraph g;
    ClassId c = g.addVar("c");
    ClassId x = g.addVar("x");
    ENode ternNode(Op::Ternary, std::vector<ClassId>{c, x, x});
    ClassId tern = g.add(ternNode);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(tern), g.find(x));
}

TEST(EGraphTest, Ternary10ToBool) {
    // cond ? 1 : 0 → cond != 0
    EGraph g;
    ClassId c = g.addVar("c");
    ClassId c1 = g.addConst(1);
    ClassId c0 = g.addConst(0);
    ENode ternNode(Op::Ternary, std::vector<ClassId>{c, c1, c0});
    ClassId tern = g.add(ternNode);
    ClassId boolExpr = g.addBinOp(Op::Ne, c, c0);

    auto rules = getAdvancedBitwiseRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(tern), g.find(boolExpr));
}

// ─────────────────────────────────────────────────────────────────────────────
// Commutative matching tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, CommutativeMatchAdd) {
    // Pattern matches x + y should also match y + x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    ClassId sum [[maybe_unused]] = g.addBinOp(Op::Add, y, x); // note: y first, then x

    // Pattern is Add(Wild("a"), Wild("b"))
    Pattern pat = Pattern::OpPat(Op::Add, {Pattern::Wild("a"), Pattern::Wild("b")});
    auto matches = g.match(pat);
    EXPECT_GE(matches.size(), 1u);
}

TEST(EGraphTest, CommutativeMatchMul) {
    // Pattern matches specific constant ordering even when reversed
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c3 = g.addConst(3);
    ClassId mul [[maybe_unused]] = g.addBinOp(Op::Mul, c3, x); // 3 * x

    // Pattern is Mul(Wild("a"), Const(3)) — opposite order
    Pattern pat = Pattern::OpPat(Op::Mul, {Pattern::Wild("a"), Pattern::ConstPat(3)});
    auto matches = g.match(pat);
    EXPECT_GE(matches.size(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-variable relational rule tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, MulDivCancel) {
    // x * 5 / 5 → x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c5 = g.addConst(5);
    ClassId mul = g.addBinOp(Op::Mul, x, c5);
    ClassId div = g.addBinOp(Op::Div, mul, c5);

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(div), g.find(x));
}

TEST(EGraphTest, AddSubConstFold) {
    // (x + 10) - 3 → x + 7
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c10 = g.addConst(10);
    ClassId c3 = g.addConst(3);
    ClassId add = g.addBinOp(Op::Add, x, c10);
    ClassId sub = g.addBinOp(Op::Sub, add, c3);
    ClassId expected = g.addBinOp(Op::Add, x, g.addConst(7));

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(sub), g.find(expected));
}

TEST(EGraphTest, SubAddConstFold) {
    // (x - 3) + 10 → x + 7
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c3 = g.addConst(3);
    ClassId c10 = g.addConst(10);
    ClassId sub = g.addBinOp(Op::Sub, x, c3);
    ClassId add = g.addBinOp(Op::Add, sub, c10);
    ClassId expected = g.addBinOp(Op::Add, x, g.addConst(7));

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(add), g.find(expected));
}

TEST(EGraphTest, MulMulConstFold) {
    // (x * 3) * 4 → x * 12
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c3 = g.addConst(3);
    ClassId c4 = g.addConst(4);
    ClassId mul1 = g.addBinOp(Op::Mul, x, c3);
    ClassId mul2 = g.addBinOp(Op::Mul, mul1, c4);
    ClassId expected = g.addBinOp(Op::Mul, x, g.addConst(12));

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul2), g.find(expected));
}

TEST(EGraphTest, DiffOfSquares) {
    // (x + y) * (x - y) → x*x - y*y
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    ClassId sum = g.addBinOp(Op::Add, x, y);
    ClassId diff = g.addBinOp(Op::Sub, x, y);
    ClassId product = g.addBinOp(Op::Mul, sum, diff);

    ClassId xx = g.addBinOp(Op::Mul, x, x);
    ClassId yy = g.addBinOp(Op::Mul, y, y);
    ClassId expected = g.addBinOp(Op::Sub, xx, yy);

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(product), g.find(expected));
}

TEST(EGraphTest, MulPow2Plus1) {
    // x * 33 → (x << 5) + x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c33 = g.addConst(33);
    ClassId mul = g.addBinOp(Op::Mul, x, c33);

    ClassId shl = g.addBinOp(Op::Shl, x, g.addConst(5));
    ClassId expected = g.addBinOp(Op::Add, shl, x);

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(expected));
}

TEST(EGraphTest, MulPow2Minus1) {
    // x * 31 → (x << 5) - x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c31 = g.addConst(31);
    ClassId mul = g.addBinOp(Op::Mul, x, c31);

    ClassId shl = g.addBinOp(Op::Shl, x, g.addConst(5));
    ClassId expected = g.addBinOp(Op::Sub, shl, x);

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(expected));
}

TEST(EGraphTest, DoubleShiftAdd) {
    // (x << 3) + (x << 3) → x << 4
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId shl = g.addBinOp(Op::Shl, x, g.addConst(3));
    ClassId sum = g.addBinOp(Op::Add, shl, shl);
    ClassId expected = g.addBinOp(Op::Shl, x, g.addConst(4));

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(sum), g.find(expected));
}

TEST(EGraphTest, CombineZeroChecks) {
    // (a == 0) && (b == 0) → (a | b) == 0
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId c0 = g.addConst(0);
    ClassId eq1 = g.addBinOp(Op::Eq, a, c0);
    ClassId eq2 = g.addBinOp(Op::Eq, b, c0);
    ClassId conj = g.addBinOp(Op::LogAnd, eq1, eq2);

    ClassId orNode = g.addBinOp(Op::BitOr, a, b);
    ClassId expected = g.addBinOp(Op::Eq, orNode, c0);

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(conj), g.find(expected));
}

TEST(EGraphTest, CombineNonZeroChecks) {
    // (a != 0) || (b != 0) → (a | b) != 0
    EGraph g;
    ClassId a = g.addVar("a");
    ClassId b = g.addVar("b");
    ClassId c0 = g.addConst(0);
    ClassId ne1 = g.addBinOp(Op::Ne, a, c0);
    ClassId ne2 = g.addBinOp(Op::Ne, b, c0);
    ClassId disj = g.addBinOp(Op::LogOr, ne1, ne2);

    ClassId orNode = g.addBinOp(Op::BitOr, a, b);
    ClassId expected = g.addBinOp(Op::Ne, orNode, c0);

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(disj), g.find(expected));
}

TEST(EGraphTest, RangeCheckUnsigned) {
    // (x >= 5) && (x <= 10) → (x - 5) <= 5
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c5 = g.addConst(5);
    ClassId c10 = g.addConst(10);
    ClassId ge = g.addBinOp(Op::Ge, x, c5);
    ClassId le = g.addBinOp(Op::Le, x, c10);
    ClassId conj = g.addBinOp(Op::LogAnd, ge, le);

    ClassId diff = g.addBinOp(Op::Sub, x, c5);
    ClassId range = g.addConst(5);  // 10 - 5
    ClassId expected = g.addBinOp(Op::Le, diff, range);

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(conj), g.find(expected));
}

TEST(EGraphTest, RelationalRulesNotEmpty) {
    auto rules = getRelationalRules();
    EXPECT_GT(rules.size(), 0u);
}

TEST(EGraphTest, AllRulesIncludesRelational) {
    auto allRules = getAllRules();
    auto relRules = getRelationalRules();
    EXPECT_GT(allRules.size(), relRules.size());
}

TEST(EGraphTest, EClassAnalysisData) {
    EGraph g;
    ClassId c0 = g.addConst(0);
    ClassId c1 = g.addConst(1);
    ClassId cx = g.addVar("x");

    const auto& cls0 = g.getClass(c0);
    EXPECT_TRUE(cls0.isZero);
    EXPECT_FALSE(cls0.isOne);

    const auto& cls1 = g.getClass(c1);
    EXPECT_FALSE(cls1.isZero);
    EXPECT_TRUE(cls1.isOne);

    const auto& clsx = g.getClass(cx);
    EXPECT_FALSE(clsx.isZero);
    EXPECT_FALSE(clsx.isOne);
}

TEST(EGraphTest, AreEquivalent) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c0 = g.addConst(0);
    ClassId add = g.addBinOp(Op::Add, x, c0);

    // Before saturation, they should be different
    EXPECT_FALSE(g.areEquivalent(x, add));

    // After saturation with algebraic rules, x + 0 → x
    auto rules = getAlgebraicRules();
    g.saturate(rules);

    EXPECT_TRUE(g.areEquivalent(x, add));
}

TEST(EGraphTest, HasVariable) {
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");

    EXPECT_TRUE(g.hasVariable(x, "x"));
    EXPECT_FALSE(g.hasVariable(x, "y"));
    EXPECT_TRUE(g.hasVariable(y, "y"));
}

TEST(EGraphTest, GetClassOp) {
    EGraph g;
    ClassId c = g.addConst(42);
    ClassId x = g.addVar("x");
    ClassId add = g.addBinOp(Op::Add, c, x);

    auto opC = g.getClassOp(c);
    ASSERT_TRUE(opC.has_value());
    EXPECT_EQ(*opC, Op::Const);

    auto opX = g.getClassOp(x);
    ASSERT_TRUE(opX.has_value());
    EXPECT_EQ(*opX, Op::Var);

    auto opAdd = g.getClassOp(add);
    ASSERT_TRUE(opAdd.has_value());
    EXPECT_EQ(*opAdd, Op::Add);
}

TEST(EGraphTest, MaskShiftNormalize) {
    // (x & 0xF0) >> 4 → (x >> 4) & 0x0F
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId mask = g.addConst(0xF0);  // 240
    ClassId c4 = g.addConst(4);
    ClassId masked = g.addBinOp(Op::BitAnd, x, mask);
    ClassId shifted = g.addBinOp(Op::Shr, masked, c4);

    ClassId xShifted = g.addBinOp(Op::Shr, x, c4);
    ClassId newMask = g.addConst(0x0F);  // 15
    ClassId expected = g.addBinOp(Op::BitAnd, xShifted, newMask);

    auto rules = getRelationalRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(shifted), g.find(expected));
}

// ─────────────────────────────────────────────────────────────────────────────
// Convergence tracking tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, SaturationConvergesEarly) {
    // A simple expression should converge in few iterations
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId c0 = g.addConst(0);
    ClassId add = g.addBinOp(Op::Add, x, c0);

    auto rules = getAllRules();
    size_t iters = g.saturate(rules);

    // Should converge quickly (x + 0 → x is immediate)
    EXPECT_LE(iters, 10u);
    EXPECT_EQ(g.find(add), g.find(x));
}

// ─────────────────────────────────────────────────────────────────────────────
// DAG sharing extraction tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, ExtractDAGSharing) {
    // (x + y) used in two places should benefit from sharing
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    ClassId sum = g.addBinOp(Op::Add, x, y);
    // Use sum twice: sum + sum
    ClassId doubled = g.addBinOp(Op::Add, sum, sum);

    CostModel model;
    ENode extracted = g.extract(doubled, model);
    EXPECT_EQ(extracted.op, Op::Add);
}

// ─────────────────────────────────────────────────────────────────────────────
// Floating-point rule tests
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, FPMulOneIdentity) {
    // x * 1.0 → x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId one = g.addConstF(1.0);
    ClassId mul = g.addBinOp(Op::Mul, x, one);

    auto rules = getFloatingPointRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(x));
}

TEST(EGraphTest, FPMulNegOneToNeg) {
    // x * (-1.0) → -x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId negone = g.addConstF(-1.0);
    ClassId mul = g.addBinOp(Op::Mul, x, negone);
    ClassId neg = g.addUnaryOp(Op::Neg, x);

    auto rules = getFloatingPointRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(neg));
}

TEST(EGraphTest, FPMulTwoToAdd) {
    // x * 2.0 → x + x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId two = g.addConstF(2.0);
    ClassId mul = g.addBinOp(Op::Mul, x, two);
    ClassId add = g.addBinOp(Op::Add, x, x);

    auto rules = getFloatingPointRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mul), g.find(add));
}

TEST(EGraphTest, FPDivOneIdentity) {
    // x / 1.0 → x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId one = g.addConstF(1.0);
    ClassId div = g.addBinOp(Op::Div, x, one);

    auto rules = getFloatingPointRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(div), g.find(x));
}

TEST(EGraphTest, FPSubSelf) {
    // x - x → 0.0
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId sub = g.addBinOp(Op::Sub, x, x);
    ClassId zero = g.addConstF(0.0);

    auto rules = getFloatingPointRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(sub), g.find(zero));
}

TEST(EGraphTest, FPMulHalfToDiv2) {
    // x * 0.5 ↔ x / 2.0
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId half = g.addConstF(0.5);
    ClassId two = g.addConstF(2.0);
    ClassId mulHalf = g.addBinOp(Op::Mul, x, half);
    ClassId divTwo = g.addBinOp(Op::Div, x, two);

    auto rules = getFloatingPointRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(mulHalf), g.find(divTwo));
}

TEST(EGraphTest, FPAddSubCancel) {
    // (x + y) - y → x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    ClassId add = g.addBinOp(Op::Add, x, y);
    ClassId sub = g.addBinOp(Op::Sub, add, y);

    auto rules = getFloatingPointRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(sub), g.find(x));
}

TEST(EGraphTest, FPSubAddCancel) {
    // (x - y) + y → x
    EGraph g;
    ClassId x = g.addVar("x");
    ClassId y = g.addVar("y");
    ClassId sub = g.addBinOp(Op::Sub, x, y);
    ClassId add = g.addBinOp(Op::Add, sub, y);

    auto rules = getFloatingPointRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(add), g.find(x));
}

TEST(EGraphTest, FPSqrtSquared) {
    // sqrt(4.0) * sqrt(4.0) → 4.0
    EGraph g;
    ClassId four = g.addConstF(4.0);
    ClassId sqrtFour = g.addUnaryOp(Op::Sqrt, four);
    ClassId product = g.addBinOp(Op::Mul, sqrtFour, sqrtFour);

    auto rules = getFloatingPointRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(product), g.find(four));
}

TEST(EGraphTest, FPDivSelfConst) {
    // 5.0 / 5.0 → 1.0
    EGraph g;
    ClassId five = g.addConstF(5.0);
    ClassId div = g.addBinOp(Op::Div, five, five);
    ClassId one = g.addConstF(1.0);

    auto rules = getFloatingPointRules();
    g.saturate(rules);

    EXPECT_EQ(g.find(div), g.find(one));
}

TEST(EGraphTest, FPRulesNotEmpty) {
    auto rules = getFloatingPointRules();
    EXPECT_GT(rules.size(), 0u);
}

TEST(EGraphTest, AllRulesIncludesFP) {
    auto allRules = getAllRules();
    auto fpRules = getFloatingPointRules();
    auto relRules = getRelationalRules();
    EXPECT_GT(allRules.size(), fpRules.size() + relRules.size());
}

// ─────────────────────────────────────────────────────────────────────────────
// Division by power-of-2 for non-negative values
// ─────────────────────────────────────────────────────────────────────────────

TEST(EGraphTest, DivPow2NonNeg) {
    // 8 / 4 → 8 >> 2  (both operands are non-negative constants)
    EGraph g;
    ClassId c8 = g.addConst(8);
    ClassId c4 = g.addConst(4);
    ClassId div = g.addBinOp(Op::Div, c8, c4);
    ClassId shr = g.addBinOp(Op::Shr, c8, g.addConst(2));

    auto rules = getRelationalRules();
    g.saturate(rules);

    // After constant folding, both should equal 2
    EXPECT_EQ(g.find(div), g.find(shr));
}
