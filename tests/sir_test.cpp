/// @file sir_test.cpp
/// @brief Unit tests for the Semantic IR (SIR) builder.
///
/// Tests that buildSIR correctly extracts:
///   - SIRFunction / SIRParam for function declarations
///   - SIRVarFacts for local variables (type, const, range)
///   - SIRLoopInfo for for-loops (static bounds, trip count)
///   - SIRCallSite for function calls
///   - SIRModule call graph

#include "sir.h"
#include "lexer.h"
#include "parser.h"
#include "opt_context.h"
#include <gtest/gtest.h>

using namespace omscript;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::unique_ptr<Program> parseSource(const std::string& src) {
    Lexer lexer(src);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    return parser.parse();
}

static SIRModule buildFromSource(const std::string& src) {
    auto program = parseSource(src);
    OptimizationContext ctx;
    return buildSIR(*program, ctx, {}, {}, {}, {}, {});
}

// ─────────────────────────────────────────────────────────────────────────────
// totalFunctions
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, FunctionCount) {
    auto sir = buildFromSource("fn foo() { } fn bar() { } fn baz() { }");
    EXPECT_EQ(sir.totalFunctions, 3u);
    EXPECT_NE(sir.getFunction("foo"), nullptr);
    EXPECT_NE(sir.getFunction("bar"), nullptr);
    EXPECT_NE(sir.getFunction("baz"), nullptr);
    EXPECT_EQ(sir.getFunction("qux"), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Return type inference
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, ReturnTypeVoid) {
    auto sir = buildFromSource("fn f() { }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    // No return-type annotation → unknown/void
    EXPECT_TRUE(fn->returnType.kind == SIRType::BaseKind::Void ||
                fn->returnType.kind == SIRType::BaseKind::Unknown);
}

TEST(SIRBuilder, ReturnTypeInt) {
    auto sir = buildFromSource("fn f() -> int { return 0; }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->returnType.kind, SIRType::BaseKind::Int);
    EXPECT_EQ(fn->returnType.bitWidth, 64);
}

TEST(SIRBuilder, ReturnTypeI32) {
    auto sir = buildFromSource("fn f() -> i32 { return 0; }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->returnType.kind, SIRType::BaseKind::Int);
    EXPECT_EQ(fn->returnType.bitWidth, 32);
}

TEST(SIRBuilder, ReturnTypeString) {
    auto sir = buildFromSource(R"(fn f() -> string { return "hi"; })");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->returnType.kind, SIRType::BaseKind::String);
}

// ─────────────────────────────────────────────────────────────────────────────
// Parameters
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, Parameters) {
    auto sir = buildFromSource("fn add(x: i32, y: i64) -> i64 { return x + y; }");
    const auto* fn = sir.getFunction("add");
    ASSERT_NE(fn, nullptr);
    ASSERT_EQ(fn->params.size(), 2u);
    EXPECT_EQ(fn->params[0].name, "x");
    EXPECT_EQ(fn->params[0].type.kind, SIRType::BaseKind::Int);
    EXPECT_EQ(fn->params[0].type.bitWidth, 32);
    EXPECT_EQ(fn->params[1].name, "y");
    EXPECT_EQ(fn->params[1].type.bitWidth, 64);
}

TEST(SIRBuilder, ParamVarFacts) {
    auto sir = buildFromSource("fn f(n: int) -> int { return n; }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    const auto* vf = sir.getVarFacts("f", "n");
    ASSERT_NE(vf, nullptr);
    EXPECT_EQ(vf->type.kind, SIRType::BaseKind::Int);
}

// ─────────────────────────────────────────────────────────────────────────────
// Annotation hints
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, InlineHint) {
    auto sir = buildFromSource("@inline fn f() { }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->forceInline);
    EXPECT_FALSE(fn->neverInline);
}

TEST(SIRBuilder, NoInlineHint) {
    auto sir = buildFromSource("@noinline fn f() { }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->neverInline);
    EXPECT_FALSE(fn->forceInline);
}

TEST(SIRBuilder, HotColdHints) {
    auto sir = buildFromSource("@hot fn h() { } @cold fn c() { }");
    ASSERT_NE(sir.getFunction("h"), nullptr);
    ASSERT_NE(sir.getFunction("c"), nullptr);
    EXPECT_TRUE(sir.getFunction("h")->isHot);
    EXPECT_FALSE(sir.getFunction("h")->isCold);
    EXPECT_TRUE(sir.getFunction("c")->isCold);
    EXPECT_FALSE(sir.getFunction("c")->isHot);
}

// ─────────────────────────────────────────────────────────────────────────────
// Entry point detection
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, EntryPointMain) {
    auto sir = buildFromSource("fn main() { }");
    const auto* fn = sir.getFunction("main");
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->isEntry);
    EXPECT_TRUE(sir.entryPoints.count("main") > 0);
}

TEST(SIRBuilder, NonEntryPoint) {
    auto sir = buildFromSource("fn helper() { }");
    ASSERT_NE(sir.getFunction("helper"), nullptr);
    EXPECT_FALSE(sir.getFunction("helper")->isEntry);
    EXPECT_EQ(sir.entryPoints.size(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// estimatedBodySize
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, EmptyBodySize) {
    auto sir = buildFromSource("fn f() { }");
    EXPECT_EQ(sir.getFunction("f")->estimatedBodySize, 0u);
}

TEST(SIRBuilder, SmallBodySize) {
    auto sir = buildFromSource("fn f(n: int) -> int { var x: int = n * 2; return x; }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    EXPECT_GE(fn->estimatedBodySize, 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// VarFacts: type and const
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, VarFactsIntType) {
    auto sir = buildFromSource("fn f() { var x: int = 42; }");
    const auto* vf = sir.getVarFacts("f", "x");
    ASSERT_NE(vf, nullptr);
    EXPECT_EQ(vf->type.kind, SIRType::BaseKind::Int);
}

TEST(SIRBuilder, VarFactsConstVar) {
    auto sir = buildFromSource("fn f() { const x: int = 7; }");
    const auto* vf = sir.getVarFacts("f", "x");
    ASSERT_NE(vf, nullptr);
    EXPECT_TRUE(vf->isImmutable);
    EXPECT_TRUE(vf->type.isConst);
}

TEST(SIRBuilder, VarFactsLiteralConstInt) {
    auto sir = buildFromSource("fn f() { var x: int = 99; }");
    const auto* vf = sir.getVarFacts("f", "x");
    ASSERT_NE(vf, nullptr);
    ASSERT_TRUE(vf->constIntVal.has_value());
    EXPECT_EQ(*vf->constIntVal, 99);
}

TEST(SIRBuilder, VarFactsStringLiteral) {
    auto sir = buildFromSource(R"(fn f() { var s: string = "hello"; })");
    const auto* vf = sir.getVarFacts("f", "s");
    ASSERT_NE(vf, nullptr);
    ASSERT_TRUE(vf->constStrVal.has_value());
    EXPECT_EQ(*vf->constStrVal, "hello");
}

TEST(SIRBuilder, VarFactsMiss) {
    auto sir = buildFromSource("fn f() { }");
    EXPECT_EQ(sir.getVarFacts("f", "nonexistent"), nullptr);
    EXPECT_EQ(sir.getVarFacts("noFunc", "x"), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Loop info: for-loop bounds
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, ForLoopStaticBounds) {
    auto sir = buildFromSource("fn f() { for (i in 0...10) { } }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    ASSERT_GE(fn->loops.size(), 1u);
    const auto& li = fn->loops[0];
    EXPECT_EQ(li.iterVar, "i");
    EXPECT_TRUE(li.hasConstBounds);
    ASSERT_TRUE(li.staticStart.has_value());
    ASSERT_TRUE(li.staticEnd.has_value());
    EXPECT_EQ(*li.staticStart, 0);
    EXPECT_EQ(*li.staticEnd, 10);
    ASSERT_TRUE(li.tripCount.has_value());
    EXPECT_EQ(*li.tripCount, 10);
    EXPECT_TRUE(li.isCountable);
    EXPECT_EQ(li.nestingDepth, 0);
}

TEST(SIRBuilder, ForLoopNesting) {
    auto sir = buildFromSource("fn f() { for (i in 0...5) { for (j in 0...3) { } } }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    ASSERT_EQ(fn->loops.size(), 2u);
    EXPECT_EQ(fn->loops[0].nestingDepth, 0);
    EXPECT_EQ(fn->loops[1].nestingDepth, 1);
}

TEST(SIRBuilder, ForLoopTripCount) {
    auto sir = buildFromSource("fn f() { for (i in 2...8) { } }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    ASSERT_FALSE(fn->loops.empty());
    ASSERT_TRUE(fn->loops[0].tripCount.has_value());
    EXPECT_EQ(*fn->loops[0].tripCount, 6);
}

TEST(SIRBuilder, ForLoopInductionVar) {
    auto sir = buildFromSource("fn f() { for (i in 0...5) { } }");
    const auto* vf = sir.getVarFacts("f", "i");
    ASSERT_NE(vf, nullptr);
    EXPECT_EQ(vf->type.kind, SIRType::BaseKind::Int);
}

// ─────────────────────────────────────────────────────────────────────────────
// Call sites
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, CallSiteCallee) {
    auto sir = buildFromSource("fn helper() { } fn f() { helper(); }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    ASSERT_FALSE(fn->callSites.empty());
    EXPECT_EQ(fn->callSites[0].callee, "helper");
}

TEST(SIRBuilder, CallSiteConstIntArg) {
    auto sir = buildFromSource("fn sink(n: int) { } fn f() { sink(42); }");
    const auto* fn = sir.getFunction("f");
    ASSERT_NE(fn, nullptr);
    ASSERT_FALSE(fn->callSites.empty());
    const auto& cs = fn->callSites[0];
    ASSERT_FALSE(cs.constIntArgs.empty());
    ASSERT_TRUE(cs.constIntArgs[0].has_value());
    EXPECT_EQ(*cs.constIntArgs[0], 42);
}

// ─────────────────────────────────────────────────────────────────────────────
// Call graph
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, CallGraphDirect) {
    auto sir = buildFromSource("fn a() { } fn b() { a(); } fn c() { a(); b(); }");
    EXPECT_TRUE(sir.functions.at("a").directCallees.empty());
    EXPECT_EQ(sir.functions.at("b").directCallees.count("a"), 1u);
    EXPECT_EQ(sir.functions.at("c").directCallees.count("a"), 1u);
    EXPECT_EQ(sir.functions.at("c").directCallees.count("b"), 1u);
}

TEST(SIRBuilder, CallerGraph) {
    auto sir = buildFromSource("fn a() { } fn b() { a(); } fn c() { a(); }");
    EXPECT_TRUE(sir.callerGraph.count("a") > 0);
    EXPECT_TRUE(sir.callerGraph.at("a").count("b") > 0);
    EXPECT_TRUE(sir.callerGraph.at("a").count("c") > 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// isLeaf / isRecursive
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, LeafFunction) {
    auto sir = buildFromSource("fn leaf() -> int { return 1; }");
    ASSERT_NE(sir.getFunction("leaf"), nullptr);
    EXPECT_TRUE(sir.getFunction("leaf")->isLeaf);
    EXPECT_FALSE(sir.getFunction("leaf")->isRecursive);
}

TEST(SIRBuilder, RecursiveFunction) {
    auto sir = buildFromSource("fn fact(n: int) -> int { if (n <= 1) { return 1; } return n * fact(n - 1); }");
    const auto* fn = sir.getFunction("fact");
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->isRecursive);
    EXPECT_FALSE(fn->isLeaf);
}

// ─────────────────────────────────────────────────────────────────────────────
// SIRType::str
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRType, StringRepresentation) {
    EXPECT_EQ(SIRType::makeVoid().str(),        "void");
    EXPECT_EQ(SIRType::makeBool().str(),        "bool");
    EXPECT_EQ(SIRType::makeInt(64).str(),       "i64");
    EXPECT_EQ(SIRType::makeInt(32).str(),       "i32");
    EXPECT_EQ(SIRType::makeUInt(64).str(),      "u64");
    EXPECT_EQ(SIRType::makeFloat(32).str(),     "f32");
    EXPECT_EQ(SIRType::makeString().str(),      "string");
    EXPECT_EQ(SIRType::makeArray(SIRType::makeInt()).str(), "i64[]");
    EXPECT_EQ(SIRType::makeStruct("Point").str(), "struct Point");
}

// ─────────────────────────────────────────────────────────────────────────────
// estimatedTotalSize
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, EstimatedTotalSize) {
    auto sir = buildFromSource("fn f() { var x: int = 1; var y: int = 2; } fn g() { }");
    EXPECT_GE(sir.estimatedTotalSize, 0u);
    EXPECT_GE(sir.functions.at("f").estimatedBodySize, 2u);
    EXPECT_EQ(sir.functions.at("g").estimatedBodySize, 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// getFunction / hasSIR queries via OptimizationContext
// ─────────────────────────────────────────────────────────────────────────────

TEST(SIRBuilder, OptCtxHasSIR) {
    OptimizationContext ctx;
    EXPECT_FALSE(ctx.hasSIR());
    auto program = parseSource("fn f() { }");
    auto sir = std::make_unique<SIRModule>(buildSIR(*program, ctx, {}, {}, {}, {}, {}));
    ctx.setSIR<SIRModule>(std::move(sir));
    EXPECT_TRUE(ctx.hasSIR());
    const auto* s = ctx.sirTyped<SIRModule>();
    ASSERT_NE(s, nullptr);
    EXPECT_NE(s->getFunction("f"), nullptr);
}
