#include "diagnostic.h"
#include "lexer.h"
#include "opt_context.h"
#include "parser.h"
#include "rlc_pass.h"
#include <gtest/gtest.h>
#include <stdexcept>

using namespace omscript;

// Helper: parse source into a Program
static std::unique_ptr<Program> parseSource(const std::string& source) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    return parser.parse();
}

// ===========================================================================
// RLC pass — coalescing tests
// ===========================================================================

/// Two sequential regions with disjoint lifetimes: R2 should be coalesced into R1.
TEST(RLCPassTest, CoalesceTwoSequentialRegions) {
    const std::string src = R"(
fn foo() {
    var r1 = newRegion();
    alloc(r1, 128);
    invalidate r1;
    var r2 = newRegion();
    alloc(r2, 256);
    invalidate r2;
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    RLCStats stats = runRLCPass(program.get(), /*verbose=*/false);
    EXPECT_EQ(stats.regionsCoalesced, 1u);
    EXPECT_GT(stats.invalidatesRemoved, 0u);
    // After coalescing, there should only be one region variable in the block.
    auto& stmts = program->functions[0]->body->statements;
    unsigned newRegionCalls = 0;
    for (const auto& s : stmts) {
        if (!s || s->type != ASTNodeType::VAR_DECL) continue;
        const auto* vd = static_cast<const VarDecl*>(s.get());
        if (vd->initializer && vd->initializer->type == ASTNodeType::CALL_EXPR) {
            const auto* c = static_cast<const CallExpr*>(vd->initializer.get());
            if (c->callee == "newRegion") ++newRegionCalls;
        }
    }
    EXPECT_EQ(newRegionCalls, 1u);
}

/// Region not invalidated → E013.
TEST(RLCPassTest, E013RegionNotInvalidated) {
    const std::string src = R"(
fn bar() {
    var r = newRegion();
    alloc(r, 64);
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    EXPECT_THROW(runRLCPass(program.get()), DiagnosticError);
}

/// Verify E013 carries the right error code.
TEST(RLCPassTest, E013ErrorCode) {
    const std::string src = R"(
fn baz() {
    var r = newRegion();
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    try {
        runRLCPass(program.get());
        FAIL() << "Expected DiagnosticError";
    } catch (const DiagnosticError& e) {
        EXPECT_EQ(e.diagnostic().code, ErrorCode::E013_REGION_NOT_INVALIDATED);
    }
}

/// Region used after invalidate → E014.
TEST(RLCPassTest, E014UseAfterInvalidate) {
    const std::string src = R"(
fn qux() {
    var r = newRegion();
    invalidate r;
    alloc(r, 32);
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    try {
        runRLCPass(program.get());
        FAIL() << "Expected DiagnosticError for E014";
    } catch (const DiagnosticError& e) {
        EXPECT_EQ(e.diagnostic().code, ErrorCode::E014_REGION_USE_AFTER_INVALIDATE);
    }
}

/// Global region variable exempt from E013.
TEST(RLCPassTest, GlobalRegionExemptFromE013) {
    const std::string src = R"(
fn withGlobal() {
    global var r = newRegion();
    alloc(r, 64);
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    // Should NOT throw E013 because the region is declared global.
    EXPECT_NO_THROW(runRLCPass(program.get()));
}

/// Three sequential regions: R2 coalesced into R1, then R3 into R1 (transitive).
TEST(RLCPassTest, ThreeRegionsTransitiveCoalescing) {
    const std::string src = R"(
fn triple() {
    var r1 = newRegion();
    alloc(r1, 100);
    invalidate r1;
    var r2 = newRegion();
    alloc(r2, 200);
    invalidate r2;
    var r3 = newRegion();
    alloc(r3, 300);
    invalidate r3;
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    RLCStats stats = runRLCPass(program.get(), /*verbose=*/false);
    // Two pairs coalesced: (r1,r2) and then (r1,r3).
    EXPECT_GE(stats.regionsCoalesced, 2u);
}

/// No coalescing when regions overlap (R1 not yet invalidated when R2 is created).
TEST(RLCPassTest, NoCoalescingWhenOverlapping) {
    const std::string src = R"(
fn overlap() {
    var r1 = newRegion();
    var r2 = newRegion();
    alloc(r1, 64);
    alloc(r2, 64);
    invalidate r1;
    invalidate r2;
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    RLCStats stats = runRLCPass(program.get(), /*verbose=*/false);
    EXPECT_EQ(stats.regionsCoalesced, 0u);
}

// ===========================================================================
// `global var` parsing tests
// ===========================================================================

/// Top-level global var declaration is placed in program->globals.
TEST(GlobalVarTest, TopLevelGlobalParsed) {
    const std::string src = R"(
global var counter = 0;
fn main() {
    counter = 42;
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    EXPECT_EQ(program->globals.size(), 1u);
    EXPECT_EQ(program->globals[0]->name, "counter");
    EXPECT_TRUE(program->globals[0]->isGlobal);
}

/// Global const declaration.
TEST(GlobalVarTest, TopLevelGlobalConstParsed) {
    const std::string src = R"(
global const MAX = 100;
fn getMax() {
    return MAX;
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->globals.size(), 1u);
    EXPECT_EQ(program->globals[0]->name, "MAX");
    EXPECT_TRUE(program->globals[0]->isConst);
    EXPECT_TRUE(program->globals[0]->isGlobal);
}

/// Multiple top-level globals.
TEST(GlobalVarTest, MultipleTopLevelGlobals) {
    const std::string src = R"(
global var x = 1;
global var y = 2;
global var z = 3;
fn sum() {
    return x + y + z;
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    EXPECT_EQ(program->globals.size(), 3u);
}

/// Global var inside function body is a VarDecl with isGlobal=true in the AST.
TEST(GlobalVarTest, InFunctionBodyGlobalParsed) {
    const std::string src = R"(
fn init() {
    global var gState = 0;
}
)";
    auto program = parseSource(src);
    ASSERT_NE(program, nullptr);
    ASSERT_FALSE(program->functions.empty());
    const auto& body = program->functions[0]->body->statements;
    ASSERT_FALSE(body.empty());
    const auto* vd = static_cast<const VarDecl*>(body[0].get());
    EXPECT_EQ(vd->name, "gState");
    EXPECT_TRUE(vd->isGlobal);
}

// ===========================================================================
// newRegion / alloc in isStdlibFunction
// ===========================================================================

TEST(RLCPassTest, NewRegionAndAllocAreStdlib) {
    // Verify parsing succeeds.
    const std::string src = R"(
fn useRegion() {
    var r = newRegion();
    var p = alloc(r, 64);
    invalidate r;
}
)";
    EXPECT_NO_THROW({
        auto program = parseSource(src);
        ASSERT_NE(program, nullptr);
    });

    // Verify newRegion and alloc are registered with writesMemory=true in
    // the canonical BuiltinEffectTable.
    const BuiltinEffects& newRegionFx = BuiltinEffectTable::get("newRegion");
    EXPECT_TRUE(newRegionFx.writesMemory);
    EXPECT_FALSE(newRegionFx.hasIO);

    const BuiltinEffects& allocFx = BuiltinEffectTable::get("alloc");
    EXPECT_TRUE(allocFx.writesMemory);
    EXPECT_FALSE(allocFx.hasIO);
}
