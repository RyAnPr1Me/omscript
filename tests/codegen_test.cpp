#include <gtest/gtest.h>
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "vm.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <llvm/Support/TargetSelect.h>

using namespace omscript;

// Ensure LLVM targets are initialized once for the entire test binary.
namespace {
struct LLVMInit {
    LLVMInit() {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmParser();
        llvm::InitializeNativeTargetAsmPrinter();
    }
};
static LLVMInit llvmInit;
} // namespace

// Helper: parse source into a Program
static std::unique_ptr<Program> parseSource(const std::string& source) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    return parser.parse();
}

// Helper: generate IR module from source
static llvm::Module* generateIR(const std::string& source, CodeGenerator& codegen) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generate(program.get());
    return codegen.getModule();
}

// ===========================================================================
// isStdlibFunction
// ===========================================================================

TEST(CodegenTest, IsStdlibFunctionKnown) {
    EXPECT_TRUE(isStdlibFunction("print"));
    EXPECT_TRUE(isStdlibFunction("print_char"));
    EXPECT_TRUE(isStdlibFunction("abs"));
    EXPECT_TRUE(isStdlibFunction("min"));
    EXPECT_TRUE(isStdlibFunction("max"));
    EXPECT_TRUE(isStdlibFunction("pow"));
    EXPECT_TRUE(isStdlibFunction("sqrt"));
    EXPECT_TRUE(isStdlibFunction("sign"));
    EXPECT_TRUE(isStdlibFunction("clamp"));
    EXPECT_TRUE(isStdlibFunction("is_even"));
    EXPECT_TRUE(isStdlibFunction("is_odd"));
    EXPECT_TRUE(isStdlibFunction("is_alpha"));
    EXPECT_TRUE(isStdlibFunction("is_digit"));
    EXPECT_TRUE(isStdlibFunction("to_char"));
    EXPECT_TRUE(isStdlibFunction("len"));
    EXPECT_TRUE(isStdlibFunction("input"));
    EXPECT_TRUE(isStdlibFunction("sum"));
    EXPECT_TRUE(isStdlibFunction("swap"));
    EXPECT_TRUE(isStdlibFunction("reverse"));
}

TEST(CodegenTest, IsStdlibFunctionUnknown) {
    EXPECT_FALSE(isStdlibFunction("myFunction"));
    EXPECT_FALSE(isStdlibFunction(""));
    EXPECT_FALSE(isStdlibFunction("PRINT"));
}

// ===========================================================================
// Basic IR generation
// ===========================================================================

TEST(CodegenTest, EmptyMainFunction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFunc = mod->getFunction("main");
    ASSERT_NE(mainFunc, nullptr);
    EXPECT_FALSE(mainFunc->empty());
}

TEST(CodegenTest, FunctionWithParameters) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn add(a, b) { return a; } fn main() { return add(1, 2); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* addFunc = mod->getFunction("add");
    ASSERT_NE(addFunc, nullptr);
    EXPECT_EQ(addFunc->arg_size(), 2u);
}

TEST(CodegenTest, MultipleFunctions) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn foo() { return 1; } fn bar() { return 2; } fn main() { return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("foo"), nullptr);
    EXPECT_NE(mod->getFunction("bar"), nullptr);
    EXPECT_NE(mod->getFunction("main"), nullptr);
}

// ===========================================================================
// Optimization levels
// ===========================================================================

TEST(CodegenTest, OptimizationO0) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 1 + 2; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptimizationO1) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR("fn main() { return 1 + 2; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptimizationO2) {
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn main() { return 1 + 2; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptimizationO3) {
    CodeGenerator codegen(OptimizationLevel::O3);
    auto* mod = generateIR("fn main() { return 1 + 2; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Control flow IR generation
// ===========================================================================

TEST(CodegenTest, IfStatement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { if (1) { return 1; } return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, IfElseStatement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { if (1) { return 1; } else { return 0; } }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, WhileLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 0; while (x < 10) { x = x + 1; } return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, DoWhileLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 0; do { x = x + 1; } while (x < 10); return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ForLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s = 0; for (i in 0...10) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ForLoopWithStep) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s = 0; for (i in 0...10...2) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Expression IR generation
// ===========================================================================

TEST(CodegenTest, BinaryArithmetic) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 2 + 3 * 4 - 1; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, UnaryNegation) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return -5; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, TernaryExpression) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 1 ? 10 : 20; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatExpression) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 3.14; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StringExpression) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s = \"hello\"; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArrayExpression) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = [1, 2, 3]; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Variable operations
// ===========================================================================

TEST(CodegenTest, VariableDecl) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 42; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ConstDecl) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { const c = 42; return c; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, CompoundAssignment) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 10; x += 5; x -= 2; x *= 3; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Postfix / prefix
// ===========================================================================

TEST(CodegenTest, PostfixIncrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 5; x++; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PrefixDecrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 5; --x; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Stdlib call generation
// ===========================================================================

TEST(CodegenTest, PrintCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { print(42); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, AbsCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return abs(-5); }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Scoping
// ===========================================================================

TEST(CodegenTest, BlockScope) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 1; { var x = 2; } return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Break / continue
// ===========================================================================

TEST(CodegenTest, BreakInLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 0; while (1) { x = x + 1; if (x == 5) { break; } } return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ContinueInLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s = 0; for (i in 0...10) { if (i == 5) { continue; } s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Forward references
// ===========================================================================

TEST(CodegenTest, ForwardFunctionReference) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return helper(); } fn helper() { return 42; }", codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("helper"), nullptr);
}

// ===========================================================================
// OPTMAX
// ===========================================================================

TEST(CodegenTest, OptmaxFunction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { return x; } OPTMAX!: fn main() { return opt(5); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Bitwise operators
// ===========================================================================

TEST(CodegenTest, BitwiseOperators) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return (5 & 3) | (2 ^ 1) | (~0) | (1 << 2) | (8 >> 1); }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Comparison operators
// ===========================================================================

TEST(CodegenTest, ComparisonOperators) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 1 < 2; var b = 1 <= 2; var c = 2 > 1; var d = 2 >= 1; var e = 1 == 1; var f = 1 != 2; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Logical operators
// ===========================================================================

TEST(CodegenTest, LogicalOperators) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return (1 && 1) || (0 && 1); }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Dynamic compilation flag
// ===========================================================================

TEST(CodegenTest, DynamicCompilationFlag) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_FALSE(codegen.isDynamicCompilation());
    codegen.setDynamicCompilation(true);
    EXPECT_TRUE(codegen.isDynamicCompilation());
    codegen.setDynamicCompilation(false);
    EXPECT_FALSE(codegen.isDynamicCompilation());
}

// ===========================================================================
// SetOptimizationLevel
// ===========================================================================

TEST(CodegenTest, SetOptimizationLevel) {
    CodeGenerator codegen(OptimizationLevel::O0);
    codegen.setOptimizationLevel(OptimizationLevel::O3);
    // Just verify it doesn't crash
    auto* mod = generateIR("fn main() { return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// OPTMAX constant folding (covers lines 53-218 of codegen.cpp)
// ===========================================================================

TEST(CodegenTest, OptmaxConstantFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { return 2 + 3; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxUnaryFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { return -5; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxBitwiseFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { return ~0; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxLogicalFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { return !0; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxBinaryAllOps) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) {"
        "  var a: int = 1 + 2;"
        "  var b: int = 5 - 3;"
        "  var c: int = 2 * 3;"
        "  var d: int = 10 / 2;"
        "  var e: int = 10 % 3;"
        "  var f: int = 1 == 1;"
        "  var g: int = 1 != 2;"
        "  var h: int = 1 < 2;"
        "  var i: int = 1 <= 2;"
        "  var j: int = 2 > 1;"
        "  var k: int = 2 >= 1;"
        "  var l: int = 1 && 1;"
        "  var m: int = 0 || 1;"
        "  var n: int = 1 & 3;"
        "  var o: int = 1 | 2;"
        "  var p: int = 1 ^ 3;"
        "  var q: int = 1 << 2;"
        "  var r: int = 8 >> 1;"
        "  return a + b;"
        "} OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxFloatFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { return -3.14; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxFloatNotFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { return !3.14; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxFloatBinaryFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) {"
        "  var a: int = 1.5 + 2.5;"
        "  var b: int = 5.0 - 3.0;"
        "  var c: int = 2.0 * 3.0;"
        "  var d: int = 10.0 / 2.0;"
        "  return 0;"
        "} OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxIdentitySimplification) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) {"
        "  var a: int = 0 + x;"
        "  var b: int = x + 0;"
        "  var c: int = x - 0;"
        "  var d: int = 1 * x;"
        "  var e: int = x * 1;"
        "  var f: int = x / 1;"
        "  return a;"
        "} OPTMAX!: fn main() { return opt(5); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxBlockOptimization) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) {"
        "  if (1) { return 2 + 3; }"
        "  while (0) { var y: int = 1; }"
        "  do { var z: int = 2; } while (0);"
        "  for (i: int in 0...10) { var w: int = i; }"
        "  return 0;"
        "} OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxTernaryFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { return 1 ? 10 : 20; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxAssignmentOptimization) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { var a: int = 0; a = 2 + 3; return a; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Float expressions in codegen
// ===========================================================================

TEST(CodegenTest, FloatArithmetic) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 1.5 + 2.5; var y = 3.0 - 1.0; var z = 2.0 * 3.0; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatDivision) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 10.0 / 3.0; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatComparison) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 1.0 < 2.0; var b = 1.0 <= 2.0; var c = 2.0 > 1.0; var d = 2.0 >= 1.0; var e = 1.0 == 1.0; var f = 1.0 != 2.0; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, MixedIntFloatArithmetic) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 1 + 2.5; var y = 3.0 - 1; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// More stdlib calls
// ===========================================================================

TEST(CodegenTest, MinCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return min(3, 5); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, MaxCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return max(3, 5); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PowCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return pow(2, 10); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SqrtCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return sqrt(16); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SignCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return sign(-5); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ClampCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return clamp(15, 0, 10); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, IsEvenCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return is_even(4); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, IsOddCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return is_odd(3); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, IsAlphaCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return is_alpha(65); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, IsDigitCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return is_digit(48); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ToCharCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { print_char(to_char(65)); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LenCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return len(\"hello\"); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SumCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var arr = [1, 2, 3]; return sum(arr); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SwapCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var arr = [1, 2, 3]; swap(arr, 0, 1); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ReverseCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var arr = [1, 2, 3]; reverse(arr); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PrintCharCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { print_char(65); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// String expression codegen
// ===========================================================================

TEST(CodegenTest, StringLiteralReturn) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s = \"hello world\"; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PrintStringCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { print(\"hello\"); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Division and modulo codegen
// ===========================================================================

TEST(CodegenTest, IntegerDivision) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 10 / 3; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, IntegerModulo) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 10 % 3; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Logical operators with short-circuit
// ===========================================================================

TEST(CodegenTest, LogicalAndShortCircuit) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 0 && 1; var y = 1 && 1; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LogicalOrShortCircuit) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 0 || 1; var y = 1 || 0; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Postfix/Prefix on identifiers
// ===========================================================================

TEST(CodegenTest, PostfixDecrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 5; x--; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PrefixIncrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 5; ++x; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Ternary with false condition
// ===========================================================================

TEST(CodegenTest, TernaryFalseCondition) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 0 ? 10 : 20; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// For loop with descending range
// ===========================================================================

TEST(CodegenTest, ForLoopDescending) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s = 0; for (i in 10...0...-1) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Const modification error
// ===========================================================================

TEST(CodegenTest, ConstModificationError) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { const c = 5; c = 10; return c; }", codegen), std::runtime_error);
}

// ===========================================================================
// Undefined variable error
// ===========================================================================

TEST(CodegenTest, UndefinedVariableError) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return x; }", codegen), std::runtime_error);
}

// ===========================================================================
// Array expression
// ===========================================================================

TEST(CodegenTest, ArrayExpressionError) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = [1, 2, 3]; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Dynamic compilation / Bytecode generation
// ===========================================================================

TEST(CodegenTest, BytecodeGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 42; return x; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeArithmetic) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 1 + 2 * 3 - 4 / 2; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeControlFlow) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { if (1) { return 1; } return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeWhileLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 0; while (x < 10) { x = x + 1; } return x; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeComparison) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var a = 1 == 1; var b = 1 != 2; var c = 1 < 2; var d = 1 <= 2; var e = 2 > 1; var f = 2 >= 1; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeLogical) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var a = 1 && 1; var b = 0 || 1; var c = !0; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeString) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var s = \"hello\"; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeFloat) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 3.14; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn add(a, b) { return a; } fn main() { return add(1, 2); }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeDoWhile) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 0; do { x = x + 1; } while (x < 5); return x; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeUnaryNeg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return -5; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeUnaryNot) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return !0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeTernary) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 1 ? 10 : 20; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeModulo) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 10 % 3; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeVarNoInit) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeIfElse) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { if (1) { return 1; } else { return 0; } }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    EXPECT_GT(emitter.getCode().size(), 0u);
}

// ===========================================================================
// Float-specific stdlib functions
// ===========================================================================

TEST(CodegenTest, FloatAbsCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 3.14; return abs(-5); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatSqrtCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = sqrt(16); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatSignCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return sign(-5); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatClampCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return clamp(15, 0, 10); }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Break/continue outside loop errors
// ===========================================================================

TEST(CodegenTest, BreakOutsideLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { break; return 0; }", codegen), std::runtime_error);
}

TEST(CodegenTest, ContinueOutsideLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { continue; return 0; }", codegen), std::runtime_error);
}

// ===========================================================================
// Postfix/Prefix on non-identifier error
// ===========================================================================

TEST(CodegenTest, PostfixOnNonIdentifier) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { 5++; return 0; }", codegen), std::runtime_error);
}

TEST(CodegenTest, PrefixOnNonIdentifier) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { ++5; return 0; }", codegen), std::runtime_error);
}

// ===========================================================================
// Var with no init
// ===========================================================================

TEST(CodegenTest, VarDeclNoInit) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Nested scoping
// ===========================================================================

TEST(CodegenTest, NestedScoping) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 1; { var x = 2; { var x = 3; } } return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Const postfix/prefix/compound-assign errors
// ===========================================================================

TEST(CodegenTest, ConstPostfixError) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { const c = 5; c++; return c; }", codegen), std::runtime_error);
}

TEST(CodegenTest, ConstPrefixError) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { const c = 5; ++c; return c; }", codegen), std::runtime_error);
}

TEST(CodegenTest, ConstCompoundAssignError) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { const c = 5; c += 1; return c; }", codegen), std::runtime_error);
}

// ===========================================================================
// Wrong argument count errors
// ===========================================================================

TEST(CodegenTest, PrintWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { print(); return 0; }", codegen), std::runtime_error);
}

TEST(CodegenTest, AbsWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return abs(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, MinWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return min(1); }", codegen), std::runtime_error);
}

// ===========================================================================
// Bytecode for-loop generation (covers lines 2362-2406)
// ===========================================================================

TEST(CodegenTest, BytecodeForLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var s = 0; for (i in 0...10) { s = s + i; } return s; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeForLoopWithStep) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var s = 0; for (i in 0...10...2) { s = s + i; } return s; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

// ===========================================================================
// Bytecode break/continue
// ===========================================================================

TEST(CodegenTest, BytecodeBreak) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 0; while (1) { x = x + 1; if (x == 5) { break; } } return x; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    EXPECT_THROW(codegen.generateBytecode(program.get()), std::runtime_error);
}

TEST(CodegenTest, BytecodeContinue) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var s = 0; var x = 0; while (x < 10) { x = x + 1; if (x == 5) { continue; } s = s + x; } return s; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    EXPECT_THROW(codegen.generateBytecode(program.get()), std::runtime_error);
}

// ===========================================================================
// Bytecode postfix/prefix/assignment
// ===========================================================================

TEST(CodegenTest, BytecodePostfixIncrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 0; x++; return x; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

TEST(CodegenTest, BytecodePrefixDecrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 5; --x; return x; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

TEST(CodegenTest, BytecodeAssignment) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 0; x = 42; return x; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

// ===========================================================================
// Float operations in LLVM codegen
// ===========================================================================

TEST(CodegenTest, FloatDivisionIR) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 10.0; var b = 3.0; var c = a / b; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatComparisonAllOps) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var a = 1.0; var b = 2.0;"
        "  var lt = a < b; var le = a <= b;"
        "  var gt = b > a; var ge = b >= a;"
        "  var eq = a == a; var ne = a != b;"
        "  return 0;"
        "}", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatUnaryNeg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 3.14; var y = -x; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// OPTMAX with non-literal expressions and all binary ops
// ===========================================================================

TEST(CodegenTest, OptmaxNonLiteralExpr) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int, y: int) { return x + y; } OPTMAX!: fn main() { return opt(1, 2); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxCallOptimization) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { return abs(x); } OPTMAX!: fn main() { return opt(-5); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxFloatBinaryAllOps) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) {"
        "  var a: int = 1.5 + 2.5;"
        "  var b: int = 5.0 - 3.0;"
        "  var c: int = 2.0 * 3.0;"
        "  var d: int = 10.0 / 2.0;"
        "  var e: int = 1.0 == 1.0;"
        "  var f: int = 1.0 != 2.0;"
        "  var g: int = 1.0 < 2.0;"
        "  var h: int = 1.0 <= 2.0;"
        "  var i: int = 2.0 > 1.0;"
        "  var j: int = 2.0 >= 1.0;"
        "  return 0;"
        "} OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxArrayExpr) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { var a: int = [1, 2, 3]; return 0; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxIfElseOptimization) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { if (1) { return 1; } else { return 0; } } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxDoWhileOptimization) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { var a: int = 0; do { a = a + 1; } while (0); return a; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxPostfixOptimization) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { var a: int = 0; a++; return a; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxPrefixOptimization) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { var a: int = 0; ++a; return a; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Stdlib wrong argument count errors
// ===========================================================================

TEST(CodegenTest, MaxWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return max(1); }", codegen), std::runtime_error);
}

TEST(CodegenTest, PowWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return pow(1); }", codegen), std::runtime_error);
}

TEST(CodegenTest, SqrtWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return sqrt(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, SignWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return sign(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, ClampWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return clamp(1, 2); }", codegen), std::runtime_error);
}

TEST(CodegenTest, IsEvenWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return is_even(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, IsOddWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return is_odd(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, IsAlphaWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return is_alpha(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, IsDigitWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return is_digit(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, ToCharWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return to_char(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, LenWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return len(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, PrintCharWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { print_char(); return 0; }", codegen), std::runtime_error);
}

TEST(CodegenTest, SumWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return sum(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, SwapWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return swap(1); }", codegen), std::runtime_error);
}

TEST(CodegenTest, ReverseWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return reverse(); }", codegen), std::runtime_error);
}

// ===========================================================================
// Undefined function call
// ===========================================================================

TEST(CodegenTest, UndefinedFunctionCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return undefined_func(1); }", codegen), std::runtime_error);
}

// ===========================================================================
// WriteObjectFile test
// ===========================================================================

TEST(CodegenTest, WriteObjectFile) {
    CodeGenerator codegen(OptimizationLevel::O0);
    generateIR("fn main() { return 42; }", codegen);
    std::string filename = (std::filesystem::temp_directory_path() / "test_output.o").string();
    codegen.writeObjectFile(filename);
    std::ifstream f(filename);
    EXPECT_TRUE(f.good());
    f.close();
    EXPECT_EQ(std::remove(filename.c_str()), 0);
}

// ===========================================================================
// Optimization with actual optimization level
// ===========================================================================

TEST(CodegenTest, OptimizationPassesO2) {
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR(
        "fn main() { var x = 10; var y = x + 5; return y; }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// String operations
// ===========================================================================

TEST(CodegenTest, StringConcatenation) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = \"hello \"; var b = \"world\"; var c = a + b; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StringComparison) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = \"abc\"; var b = \"def\"; var eq = a == b; var ne = a != b; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Index expression codegen
// ===========================================================================

TEST(CodegenTest, IndexExpressionCodegen) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = [1, 2, 3]; var x = a[0]; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Ternary with variable condition
// ===========================================================================

TEST(CodegenTest, TernaryWithVariable) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var c = 1; return c ? 10 : 20; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Modulo and Shift operators
// ===========================================================================

TEST(CodegenTest, ModuloExpression) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 17 % 5; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ShiftExpressions) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 1 << 4; var b = 16 >> 2; return a + b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Input function
// ===========================================================================

TEST(CodegenTest, InputCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* inputMod = generateIR("fn foo() { var x = input(); return x; } fn main() { return 0; }", codegen);
    ASSERT_NE(inputMod, nullptr);
}

// ===========================================================================
// For loop in codegen with complex body
// ===========================================================================

TEST(CodegenTest, ForLoopWithBreak) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s = 0; for (i in 0...100) { if (i == 50) { break; } s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ForLoopWithContinue) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s = 0; for (i in 0...10) { if (i == 5) { continue; } s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Do-while with break
// ===========================================================================

TEST(CodegenTest, DoWhileBreak) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 0; do { x = x + 1; if (x == 3) { break; } } while (x < 10); return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Nested functions calling each other
// ===========================================================================

TEST(CodegenTest, NestedFunctionCalls) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn add(a, b) { return a + b; }"
        "fn mul(a, b) { return a * b; }"
        "fn main() { return add(mul(2, 3), mul(4, 5)); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Bytecode nested if-else
// ===========================================================================

TEST(CodegenTest, BytecodeNestedIfElse) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 5; if (x > 0) { if (x > 10) { return 2; } else { return 1; } } else { return 0; } }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

// ===========================================================================
// Multiple compound assignments
// ===========================================================================

TEST(CodegenTest, AllCompoundAssignments) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var x = 100; x += 10; x -= 5; x *= 2; x /= 3; x %= 7; return x; }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Bytecode block statement
// ===========================================================================

TEST(CodegenTest, BytecodeBlock) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { { var x = 1; } return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

// ===========================================================================
// Float-specific stdlib calls
// ===========================================================================

TEST(CodegenTest, PrintFloatArg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 3.14; print(x); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, AbsFloatArg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = -3.14; var y = abs(x); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, MinFloatArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 1.5; var b = 2.5; var c = min(a, b); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, MaxFloatArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 1.5; var b = 2.5; var c = max(a, b); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SignFloatArg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = -3.14; var y = sign(x); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ClampFloatArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 15.0; var y = clamp(x, 0.0, 10.0); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PrintCharFloatArg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 65.0; print_char(x); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Input function wrong args
// ===========================================================================

TEST(CodegenTest, InputWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { var x = input(1); return 0; }", codegen), std::runtime_error);
}

// ===========================================================================
// OPTMAX calling non-stdlib function
// ===========================================================================

TEST(CodegenTest, OptmaxCallNonStdlibError) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR(
        "fn helper() { return 1; }"
        "OPTMAX=: fn opt(x: int) { return helper(); } OPTMAX!:"
        "fn main() { return opt(1); }",
        codegen), std::runtime_error);
}

// ===========================================================================
// User function wrong argument count
// ===========================================================================

TEST(CodegenTest, WrongArgCountUserFunction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn add(a, b) { return a + b; } fn main() { return add(1); }", codegen), std::runtime_error);
}

// ===========================================================================
// Ternary with float branches
// ===========================================================================

TEST(CodegenTest, TernaryFloatBranches) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 1 ? 1.5 : 2.5; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, TernaryMixedTypes) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 1 ? 1.5 : 2; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Unary NOT on value
// ===========================================================================

TEST(CodegenTest, UnaryNotOnVar) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 0; return !x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Unary tilde on var
// ===========================================================================

TEST(CodegenTest, UnaryTildeOnVar) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 5; return ~x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Bitwise operators on variables
// ===========================================================================

TEST(CodegenTest, BitwiseAndOnVars) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 5; var b = 3; return a & b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, BitwiseOrOnVars) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 5; var b = 3; return a | b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, BitwiseXorOnVars) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 5; var b = 3; return a ^ b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ShiftOnVars) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 1; var b = 4; return a << b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArithShiftRight) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a = 16; var b = 2; return a >> b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// OPTMAX with float logical ops and for step
// ===========================================================================

TEST(CodegenTest, OptmaxFloatLogicalOps) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) {"
        "  var a: int = 1.0 && 2.0;"
        "  var b: int = 0.0 || 3.0;"
        "  return 0;"
        "} OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxForLoopWithStepOpt) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) {"
        "  var s: int = 0;"
        "  for (i: int in 0...10...2) { s = s + i; }"
        "  return s;"
        "} OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Return void
// ===========================================================================

TEST(CodegenTest, ReturnVoid) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn foo() { return; } fn main() { foo(); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Multiple if branches with merging
// ===========================================================================

TEST(CodegenTest, IfElseMerging) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var x = 0; if (1) { x = 1; } else { x = 2; } return x; }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Float postfix/prefix
// ===========================================================================

TEST(CodegenTest, FloatPostfixIncrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 1.5; x++; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatPrefixDecrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 3.14; --x; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Bytecode for-loop variable load/store
// ===========================================================================

TEST(CodegenTest, BytecodeForLoopVariableOps) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var total = 0; for (i in 0...5) { total = total + i; } return total; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

// ===========================================================================
// Bytecode break/continue in while loops
// ===========================================================================

TEST(CodegenTest, BytecodeWhileBreak) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 0; while (x < 100) { x = x + 1; if (x == 10) { break; } } return x; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    EXPECT_THROW(codegen.generateBytecode(program.get()), std::runtime_error);
}

TEST(CodegenTest, BytecodeWhileContinue) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var s = 0; var x = 0; while (x < 10) { x = x + 1; if (x == 5) { continue; } s = s + x; } return s; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    EXPECT_THROW(codegen.generateBytecode(program.get()), std::runtime_error);
}

// ===========================================================================
// Bytecode expression statement
// ===========================================================================

TEST(CodegenTest, BytecodeExpressionStatement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { 1 + 2; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

// ===========================================================================
// Bytecode identifier load
// ===========================================================================

TEST(CodegenTest, BytecodeIdentifier) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 10; var y = x + 1; return y; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

// ===========================================================================
// Bytecode postfix decrement and prefix increment
// ===========================================================================

TEST(CodegenTest, BytecodePostfixDecrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 5; x--; return x; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

TEST(CodegenTest, BytecodePrefixIncrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = 5; ++x; return x; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

// ===========================================================================
// Bytecode return void
// ===========================================================================

TEST(CodegenTest, BytecodeReturnVoid) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

// ===========================================================================
// Bytecode unsupported unary (~) error
// ===========================================================================

TEST(CodegenTest, BytecodeUnsupportedUnary) {
    // The ~ operator is now supported in bytecode via the BIT_NOT opcode.
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return ~5; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());

    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), ~static_cast<int64_t>(5));
}

// ===========================================================================
// Bytecode stdlib function error
// ===========================================================================

TEST(CodegenTest, BytecodeStdlibError) {
    // print() is now supported in bytecode via the PRINT opcode.
    // Other stdlib functions should still throw.
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x = abs(5); return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    EXPECT_THROW(codegen.generateBytecode(program.get()), std::runtime_error);
}

// ===========================================================================
// Bytecode variable declaration without init
// ===========================================================================

TEST(CodegenTest, BytecodeVarDeclNoInit) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { var x; return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    EXPECT_GT(codegen.getBytecodeEmitter().getCode().size(), 0u);
}

// ===========================================================================
// Optimization O1 (tests legacy pass manager path)
// ===========================================================================

TEST(CodegenTest, OptimizationO1FunctionPasses) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn add(a, b) { return a + b; } fn main() { return add(1, 2); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Tilde on float variable (converted before bitwise not)
// ===========================================================================

TEST(CodegenTest, TildeOnFloatVar) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 3.14; return ~x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// String concat with lazy function init (covers malloc/strcpy/strcat)
// ===========================================================================

TEST(CodegenTest, StringConcatLazyInit) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var a = \"hello\";"
        "  var b = \" world\";"
        "  var c = a + b;"
        "  print(c);"
        "  return 0;"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Input function codegen (covers scanf setup)
// ===========================================================================

TEST(CodegenTest, InputFunctionCodegen) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn foo() { var x = input(); return x; }"
        "fn main() { return 0; }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Print_char codegen (covers putchar setup)
// ===========================================================================

TEST(CodegenTest, PrintCharCodegen) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { print_char(65); return 0; }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// OPTMAX with index expression (covers index optimization path)
// ===========================================================================

TEST(CodegenTest, OptmaxIndexExpr) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { var a: int = [1, 2]; var b: int = a[0]; return b; } OPTMAX!: fn main() { return opt(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// optimizeFunction: per-function optimization
// ===========================================================================

TEST(CodegenTest, OptimizeFunctionPerFunction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn add(a, b) { return a + b; } fn main() { return add(1, 2); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    // Find the 'add' function and run per-function optimization on it
    llvm::Function* addFn = mod->getFunction("add");
    ASSERT_NE(addFn, nullptr);
    codegen.optimizeFunction(addFn);
    // The function should still be valid after optimization
    EXPECT_FALSE(addFn->empty());
}

// ===========================================================================
// O0 level: no optimizations applied
// ===========================================================================

TEST(CodegenTest, OptimizationPassesO0) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var x = 10; var y = x + 5; return y; }",
        codegen);
    ASSERT_NE(mod, nullptr);
    // Module should be valid even with O0 (no optimizations)
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    EXPECT_FALSE(mainFn->empty());
}

// ===========================================================================
// Switch statement codegen
// ===========================================================================

TEST(CodegenTest, SwitchBasic) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn classify(x) {"
        "  switch (x) {"
        "    case 1: return 10;"
        "    case 2: return 20;"
        "    default: return 0;"
        "  }"
        "}"
        "fn main() { return classify(2); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    llvm::Function* fn = mod->getFunction("classify");
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(fn->empty());
}

TEST(CodegenTest, SwitchNoDefault) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var x = 5;"
        "  switch (x) {"
        "    case 1: return 10;"
        "    case 2: return 20;"
        "  }"
        "  return 0;"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// typeof and assert codegen
// ===========================================================================

TEST(CodegenTest, TypeofCodegen) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var t = typeof(42); return t; }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, AssertCodegen) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { assert(1); return 0; }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, AssertWrongArgCount) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(
        generateIR("fn main() { assert(1, 2); return 0; }", codegen),
        std::runtime_error);
}

TEST(CodegenTest, TypeofWrongArgCount) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(
        generateIR("fn main() { typeof(); return 0; }", codegen),
        std::runtime_error);
}

// ===========================================================================
// Break inside switch (no enclosing loop)
// ===========================================================================

TEST(CodegenTest, BreakInsideSwitch) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var r = 0;"
        "  switch (1) {"
        "    case 1: r = 10; break;"
        "    case 2: r = 20; break;"
        "    default: r = 99; break;"
        "  }"
        "  return r;"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
    llvm::Function* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(fn->empty());
}

// ===========================================================================
// Switch inside OPTMAX function (constant folding)
// ===========================================================================

TEST(CodegenTest, SwitchInOptmax) {
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR(
        "OPTMAX=:"
        "fn classify(x: int) {"
        "  var base: int = 2 + 3;"  // should be folded to 5
        "  switch (x) {"
        "    case 1: return base;"
        "    default: return 0;"
        "  }"
        "}"
        "OPTMAX!:"
        "fn main() { return classify(1); }",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Array element assignment: arr[i] = value
// ===========================================================================

TEST(CodegenTest, ArrayIndexAssignment) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var arr = [10, 20, 30];"
        "  arr[0] = 99;"
        "  return arr[0];"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
    llvm::Function* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(fn->empty());
}

TEST(CodegenTest, ArrayIndexAssignmentMultiple) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var arr = [1, 2, 3];"
        "  arr[0] = 10;"
        "  arr[1] = 20;"
        "  arr[2] = 30;"
        "  return arr[0] + arr[1] + arr[2];"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// For-each loop codegen
// ===========================================================================

TEST(CodegenTest, ForEachBasic) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var arr = [10, 20, 30];"
        "  var total = 0;"
        "  for (x in arr) {"
        "    total = total + x;"
        "  }"
        "  return total;"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
    llvm::Function* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(fn->empty());
}

TEST(CodegenTest, ForEachWithBreak) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var arr = [1, 2, 3, 4, 5];"
        "  var total = 0;"
        "  for (x in arr) {"
        "    if (x == 4) { break; }"
        "    total = total + x;"
        "  }"
        "  return total;"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ForEachWithContinue) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var arr = [1, 2, 3, 4, 5];"
        "  var total = 0;"
        "  for (x in arr) {"
        "    if (x % 2 == 0) { continue; }"
        "    total = total + x;"
        "  }"
        "  return total;"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// String functions: str_len and char_at
// ===========================================================================

TEST(CodegenTest, StrLen) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var s = \"Hello\";"
        "  return str_len(s);"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
    llvm::Function* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(fn->empty());
}

TEST(CodegenTest, CharAt) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var s = \"ABC\";"
        "  return char_at(s, 0);"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrLenWrongArity) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR(
        "fn main() { return str_len(); }",
        codegen), std::runtime_error);
}

TEST(CodegenTest, CharAtWrongArity) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR(
        "fn main() { return char_at(\"a\"); }",
        codegen), std::runtime_error);
}

// ===========================================================================
// Multi-variable declarations
// ===========================================================================

TEST(CodegenTest, MultiVarDeclaration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var a = 10, b = 20, c = 30;"
        "  return a + b + c;"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
    llvm::Function* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(fn->empty());
}

TEST(CodegenTest, MultiConstDeclaration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  const x = 5, y = 10;"
        "  return x * y;"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// str_eq string comparison
// ===========================================================================

TEST(CodegenTest, StrEqFunction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() {"
        "  var a = \"hello\";"
        "  var b = \"hello\";"
        "  return str_eq(a, b);"
        "}",
        codegen);
    ASSERT_NE(mod, nullptr);
    llvm::Function* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(fn->empty());
}

TEST(CodegenTest, StrEqWrongArgCount) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(
        generateIR("fn main() { return str_eq(\"a\"); }", codegen),
        std::runtime_error);
}

// ===========================================================================
// Bytecode bitwise operators
// ===========================================================================

TEST(CodegenTest, BytecodeBitwiseAnd) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 255 & 15; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());

    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), 15);
}

TEST(CodegenTest, BytecodeBitwiseOr) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 240 | 15; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());

    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), 255);
}

TEST(CodegenTest, BytecodeBitwiseXor) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 255 ^ 15; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());

    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), 240);
}

TEST(CodegenTest, BytecodeShiftLeft) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 1 << 4; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());

    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), 16);
}

TEST(CodegenTest, BytecodeShiftRight) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 16 >> 4; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());

    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(CodegenTest, BytecodeBitwiseNot) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return ~0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());

    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), ~static_cast<int64_t>(0));
}

// ===========================================================================
// Bytecode print support
// ===========================================================================

TEST(CodegenTest, BytecodePrint) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { print(42); return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());

    VM vm;
    testing::internal::CaptureStdout();
    vm.execute(codegen.getBytecodeEmitter().getCode());
    std::string output = testing::internal::GetCapturedStdout();
    EXPECT_EQ(output, "42\n");
    EXPECT_EQ(vm.getLastReturn().asInt(), 0);
}

// ===========================================================================
// New pass manager IPO optimizations (inlining, IPSCCP, GlobalDCE)
// ===========================================================================

TEST(CodegenTest, InliningAtO2) {
    // At O2, the new pass manager should inline small functions.
    // After inlining square(5) + square(3), IPSCCP should fold to 34.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR(
        "fn square(x) { return x * x; }"
        "fn main() { return square(5) + square(3); }",
        codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // After inlining + constant propagation, main should have no call to square
    bool hasCallToSquare = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (call->getCalledFunction() &&
                    call->getCalledFunction()->getName() == "square") {
                    hasCallToSquare = true;
                }
            }
        }
    }
    EXPECT_FALSE(hasCallToSquare) << "square() should be inlined at O2";
}

TEST(CodegenTest, InliningAtO3) {
    CodeGenerator codegen(OptimizationLevel::O3);
    auto* mod = generateIR(
        "fn double_it(n) { return n + n; }"
        "fn main() { return double_it(21); }",
        codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasCall = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (call->getCalledFunction() &&
                    call->getCalledFunction()->getName() == "double_it") {
                    hasCall = true;
                }
            }
        }
    }
    EXPECT_FALSE(hasCall) << "double_it() should be inlined at O3";
}

TEST(CodegenTest, NoInliningAtO1) {
    // O1 uses the lightweight legacy pipeline which doesn't include inlining.
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn helper(x) { return x * 2; }"
        "fn main() { return helper(10); }",
        codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasCall = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (call->getCalledFunction() &&
                    call->getCalledFunction()->getName() == "helper") {
                    hasCall = true;
                }
            }
        }
    }
    EXPECT_TRUE(hasCall) << "helper() should NOT be inlined at O1";
}

TEST(CodegenTest, ConstantPropagationThroughInline) {
    // After inlining + IPSCCP, the entire program should fold to a constant
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR(
        "fn add(a, b) { return a + b; }"
        "fn mul(a, b) { return a * b; }"
        "fn main() { return add(mul(3, 4), mul(5, 6)); }",
        codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // Check that main has a single block with just a ret instruction
    ASSERT_EQ(mainFn->size(), 1u);
    auto& entry = mainFn->getEntryBlock();
    // First (and only) instruction should be ret i64 42 (3*4 + 5*6)
    auto& firstInst = entry.front();
    auto* retInst = llvm::dyn_cast<llvm::ReturnInst>(&firstInst);
    ASSERT_NE(retInst, nullptr);
    auto* constVal = llvm::dyn_cast<llvm::ConstantInt>(retInst->getReturnValue());
    ASSERT_NE(constVal, nullptr);
    EXPECT_EQ(constVal->getSExtValue(), 42);
}

// ===========================================================================
// OPTMAX mixed-type constant folding
// ===========================================================================

TEST(CodegenTest, OptmaxMixedTypeConstantFolding) {
    // OPTMAX should fold 5 + 1.0 into 6.0 at the AST level
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn main() { return 5 + 1.0; } OPTMAX!:",
        codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // The result of mixed int+float should be folded to a constant float
    // then converted back to i64 for the return. Check that no add instruction exists.
    bool hasAddInst = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::FAdd ||
                I.getOpcode() == llvm::Instruction::Add) {
                hasAddInst = true;
            }
        }
    }
    EXPECT_FALSE(hasAddInst) << "Mixed int+float should be folded at compile time in OPTMAX";
}

TEST(CodegenTest, OptmaxMixedTypeMul) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn main() { return 3 * 2.5; } OPTMAX!:",
        codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasMulInst = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::FMul ||
                I.getOpcode() == llvm::Instruction::Mul) {
                hasMulInst = true;
            }
        }
    }
    EXPECT_FALSE(hasMulInst) << "Mixed int*float should be folded at compile time in OPTMAX";
}

// ===========================================================================
// Boolean and null literals
// ===========================================================================

TEST(CodegenTest, TrueLiteral) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return true; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    EXPECT_FALSE(mainFn->empty());
}

TEST(CodegenTest, FalseLiteral) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return false; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    EXPECT_FALSE(mainFn->empty());
}

TEST(CodegenTest, NullLiteral) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = null; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    EXPECT_FALSE(mainFn->empty());
}

TEST(CodegenTest, TrueInCondition) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { if (true) { return 1; } return 0; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

// ===========================================================================
// Bitwise compound assignment
// ===========================================================================

TEST(CodegenTest, BitwiseAndAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 15; x &= 6; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, BitwiseOrAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 3; x |= 12; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, BitwiseXorAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 15; x ^= 9; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, ShiftLeftAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 1; x <<= 3; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, ShiftRightAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x = 32; x >>= 2; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

// ===========================================================================
// Array compound assignment
// ===========================================================================

TEST(CodegenTest, ArrayCompoundAssignPlus) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var arr = [10, 20]; arr[0] += 5; return arr[0]; }",
        codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, ArrayCompoundAssignMinus) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var arr = [10, 20]; arr[1] -= 3; return arr[1]; }",
        codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, ArrayCompoundAssignMul) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var arr = [10]; arr[0] *= 3; return arr[0]; }",
        codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

// ===========================================================================
// Semantic validation: missing main, duplicate functions, duplicate params
// ===========================================================================

TEST(CodegenTest, NoMainFunction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW({
        generateIR("fn foo() { return 42; }", codegen);
    }, std::runtime_error);
}

TEST(CodegenTest, NoMainFunctionMessage) {
    CodeGenerator codegen(OptimizationLevel::O0);
    try {
        generateIR("fn foo() { return 42; }", codegen);
        FAIL() << "Expected exception";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("No 'main' function defined"), std::string::npos);
    }
}

TEST(CodegenTest, DuplicateFunction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW({
        generateIR("fn add(a, b) { return a + b; }\n"
                    "fn add(x, y) { return x + y; }\n"
                    "fn main() { return add(1, 2); }", codegen);
    }, std::runtime_error);
}

TEST(CodegenTest, DuplicateFunctionMessage) {
    CodeGenerator codegen(OptimizationLevel::O0);
    try {
        generateIR("fn add(a, b) { return a + b; }\n"
                    "fn add(x, y) { return x + y; }\n"
                    "fn main() { return add(1, 2); }", codegen);
        FAIL() << "Expected exception";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("Duplicate function definition"), std::string::npos);
    }
}

TEST(CodegenTest, DuplicateParameter) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW({
        generateIR("fn add(a, a) { return a + a; }\n"
                    "fn main() { return add(1, 2); }", codegen);
    }, std::runtime_error);
}

TEST(CodegenTest, DuplicateParameterMessage) {
    CodeGenerator codegen(OptimizationLevel::O0);
    try {
        generateIR("fn add(a, a) { return a + a; }\n"
                    "fn main() { return add(1, 2); }", codegen);
        FAIL() << "Expected exception";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("Duplicate parameter name"), std::string::npos);
    }
}

TEST(CodegenTest, ValidProgramAccepted) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn add(a, b) { return a + b; }\n"
        "fn main() { return add(1, 2); }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    auto* addFn = mod->getFunction("add");
    ASSERT_NE(addFn, nullptr);
}

// ===========================================================================
// Bytecode constant folding
// ===========================================================================

TEST(CodegenTest, BytecodeConstantFoldingAdd) {
    // 10 + 20 should be folded to PUSH_INT 30 (not PUSH_INT 10, PUSH_INT 20, ADD)
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 10 + 20; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    auto& emitter = codegen.getBytecodeEmitter();
    const auto& code = emitter.getCode();
    // Execute and verify correct result
    VM vm;
    vm.execute(code);
    EXPECT_EQ(vm.getLastReturn().asInt(), 30);
    // The bytecode should be smaller with folding (no ADD opcode needed).
    // Without folding: PUSH_INT(1+8) + PUSH_INT(1+8) + ADD(1) + RETURN(1) + HALT(1) = 21
    // With folding:    PUSH_INT(1+8) + RETURN(1) + HALT(1) = 11
    EXPECT_LT(code.size(), 21u);
}

TEST(CodegenTest, BytecodeConstantFoldingMul) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 6 * 7; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), 42);
}

TEST(CodegenTest, BytecodeConstantFoldingComparison) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return 5 < 10; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(CodegenTest, BytecodeConstantFoldingUnaryNeg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return -42; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), -42);
}

TEST(CodegenTest, BytecodeConstantFoldingLogicalNot) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return !0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), 1);
}

TEST(CodegenTest, BytecodeConstantFoldingBitwiseNot) {
    CodeGenerator codegen(OptimizationLevel::O0);
    Lexer lexer("fn main() { return ~0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateBytecode(program.get());
    VM vm;
    vm.execute(codegen.getBytecodeEmitter().getCode());
    EXPECT_EQ(vm.getLastReturn().asInt(), -1);
}

// ===========================================================================
// IR-level constant comparison folding
// ===========================================================================

TEST(CodegenTest, IRConstantComparisonFolding) {
    // When both operands are constants, comparisons should be folded
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 5 == 5; }", codegen);
    auto* mainFunc = mod->getFunction("main");
    ASSERT_NE(mainFunc, nullptr);
    // At O0, constant folding in generateBinary still occurs
    // The function should exist and have code
    EXPECT_FALSE(mainFunc->empty());
}

// ===========================================================================
// Strength reduction for division by power of 2
// ===========================================================================

TEST(CodegenTest, DivisionStrengthReduction) {
    // n / 4 should be converted to n >> 2 (arithmetic shift right)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn divide_by_four(n) { return n / 4; }\n"
        "fn main() { return divide_by_four(16); }", codegen);
    auto* func = mod->getFunction("divide_by_four");
    ASSERT_NE(func, nullptr);
    EXPECT_FALSE(func->empty());
    // Verify the function contains an ashr instruction (strength reduction)
    bool hasAShr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::AShr) {
                hasAShr = true;
            }
        }
    }
    EXPECT_TRUE(hasAShr);
}

// ===========================================================================
// Execution-tier classification
// ===========================================================================

TEST(CodegenTest, ClassifyMainAsAOT) {
    std::string src = "fn main() { return 0; }";
    CodeGenerator codegen;
    generateIR(src, codegen);
    EXPECT_EQ(codegen.getFunctionTier("main"), ExecutionTier::AOT);
}

TEST(CodegenTest, ClassifyOptMaxAsAOT) {
    std::string src = R"(
        OPTMAX=: fn optimized(x: int) { return x * 2; } OPTMAX!:
        fn main() { return optimized(5); }
    )";
    CodeGenerator codegen;
    generateIR(src, codegen);
    EXPECT_EQ(codegen.getFunctionTier("optimized"), ExecutionTier::AOT);
}

TEST(CodegenTest, ClassifyFullyAnnotatedAsAOT) {
    std::string src = R"(
        fn add(a: int, b: int) { return a + b; }
        fn main() { return add(1, 2); }
    )";
    CodeGenerator codegen;
    generateIR(src, codegen);
    EXPECT_EQ(codegen.getFunctionTier("add"), ExecutionTier::AOT);
}

TEST(CodegenTest, ClassifyUnannotatedAsInterpreted) {
    std::string src = R"(
        fn compute(x, y) { return x + y; }
        fn main() { return compute(3, 4); }
    )";
    CodeGenerator codegen;
    generateIR(src, codegen);
    EXPECT_EQ(codegen.getFunctionTier("compute"), ExecutionTier::Interpreted);
}

TEST(CodegenTest, ClassifyPartiallyAnnotatedAsInterpreted) {
    std::string src = R"(
        fn mixed(a: int, b) { return a + b; }
        fn main() { return mixed(1, 2); }
    )";
    CodeGenerator codegen;
    generateIR(src, codegen);
    EXPECT_EQ(codegen.getFunctionTier("mixed"), ExecutionTier::Interpreted);
}

TEST(CodegenTest, ClassifyNoParamsAsAOT) {
    // Functions with no parameters are trivially fully-annotated.
    std::string src = R"(
        fn zero() { return 0; }
        fn main() { return zero(); }
    )";
    CodeGenerator codegen;
    generateIR(src, codegen);
    EXPECT_EQ(codegen.getFunctionTier("zero"), ExecutionTier::AOT);
}

TEST(CodegenTest, ExecutionTierNameStrings) {
    EXPECT_STREQ(executionTierName(ExecutionTier::AOT), "AOT");
    EXPECT_STREQ(executionTierName(ExecutionTier::Interpreted), "Interpreted");
    EXPECT_STREQ(executionTierName(ExecutionTier::JIT), "JIT");
}

TEST(CodegenTest, FunctionTierMapContainsAllFunctions) {
    std::string src = R"(
        fn helper(x) { return x; }
        fn main() { return helper(0); }
    )";
    CodeGenerator codegen;
    generateIR(src, codegen);
    auto& tiers = codegen.getFunctionTiers();
    EXPECT_NE(tiers.find("helper"), tiers.end());
    EXPECT_NE(tiers.find("main"), tiers.end());
}

TEST(CodegenTest, HasFullTypeAnnotationsHelper) {
    auto prog = parseSource(R"(
        fn typed(a: int, b: int) { return a + b; }
        fn untyped(a, b) { return a + b; }
        fn main() { return 0; }
    )");
    bool foundTyped = false, foundUntyped = false;
    for (auto& fn : prog->functions) {
        if (fn->name == "typed") {
            EXPECT_TRUE(fn->hasFullTypeAnnotations());
            foundTyped = true;
        }
        if (fn->name == "untyped") {
            EXPECT_FALSE(fn->hasFullTypeAnnotations());
            foundUntyped = true;
        }
    }
    EXPECT_TRUE(foundTyped);
    EXPECT_TRUE(foundUntyped);
}

// ===========================================================================
// Hybrid compilation
// ===========================================================================

// Helper: run hybrid generation and return the codegen
static void generateHybridIR(const std::string& source, CodeGenerator& codegen) {
    Lexer lexer(source);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    auto program = parser.parse();
    codegen.generateHybrid(program.get());
}

TEST(CodegenTest, HybridEmitsBytecodeForUntypedFunction) {
    std::string src = R"(
        fn compute(x, y) { return x + y; }
        fn main() { return compute(3, 4); }
    )";
    CodeGenerator codegen;
    generateHybridIR(src, codegen);

    // compute is Interpreted, main is AOT
    EXPECT_EQ(codegen.getFunctionTier("compute"), ExecutionTier::Interpreted);
    EXPECT_EQ(codegen.getFunctionTier("main"), ExecutionTier::AOT);

    // Hybrid should produce bytecode for the untyped function
    EXPECT_TRUE(codegen.hasHybridBytecodeFunctions());
    auto& bcFuncs = codegen.getBytecodeFunctions();
    ASSERT_EQ(bcFuncs.size(), 1u);
    EXPECT_EQ(bcFuncs[0].name, "compute");
    EXPECT_EQ(bcFuncs[0].arity, 2);
    EXPECT_FALSE(bcFuncs[0].bytecode.empty());
}

TEST(CodegenTest, HybridNoBytecodeForFullyTyped) {
    std::string src = R"(
        fn add(a: int, b: int) { return a + b; }
        fn main() { return add(1, 2); }
    )";
    CodeGenerator codegen;
    generateHybridIR(src, codegen);

    // Both functions are AOT — no bytecode should be emitted
    EXPECT_EQ(codegen.getFunctionTier("add"), ExecutionTier::AOT);
    EXPECT_EQ(codegen.getFunctionTier("main"), ExecutionTier::AOT);
    EXPECT_FALSE(codegen.hasHybridBytecodeFunctions());
}

TEST(CodegenTest, HybridMultipleUntypedFunctions) {
    std::string src = R"(
        fn helper(x) { return x * 2; }
        fn dynamic_add(a, b) { return a + b; }
        fn main() { return helper(5) + dynamic_add(1, 2); }
    )";
    CodeGenerator codegen;
    generateHybridIR(src, codegen);

    EXPECT_EQ(codegen.getFunctionTier("helper"), ExecutionTier::Interpreted);
    EXPECT_EQ(codegen.getFunctionTier("dynamic_add"), ExecutionTier::Interpreted);
    EXPECT_EQ(codegen.getFunctionTier("main"), ExecutionTier::AOT);

    auto& bcFuncs = codegen.getBytecodeFunctions();
    ASSERT_EQ(bcFuncs.size(), 2u);

    // Verify both functions have valid bytecode and correct arity
    std::unordered_map<std::string, uint8_t> foundFuncs;
    for (auto& f : bcFuncs) {
        foundFuncs[f.name] = f.arity;
        EXPECT_FALSE(f.bytecode.empty());
    }
    EXPECT_EQ(foundFuncs["helper"], 1);
    EXPECT_EQ(foundFuncs["dynamic_add"], 2);
}

TEST(CodegenTest, HybridMixedTiersWithOptMax) {
    std::string src = R"(
        OPTMAX=: fn fast(x: int) { return x * x; } OPTMAX!:
        fn dynamic(x) { return x + 1; }
        fn main() { return fast(3) + dynamic(2); }
    )";
    CodeGenerator codegen;
    generateHybridIR(src, codegen);

    EXPECT_EQ(codegen.getFunctionTier("fast"), ExecutionTier::AOT);
    EXPECT_EQ(codegen.getFunctionTier("dynamic"), ExecutionTier::Interpreted);
    EXPECT_EQ(codegen.getFunctionTier("main"), ExecutionTier::AOT);

    auto& bcFuncs = codegen.getBytecodeFunctions();
    ASSERT_EQ(bcFuncs.size(), 1u);
    EXPECT_EQ(bcFuncs[0].name, "dynamic");
    EXPECT_EQ(bcFuncs[0].arity, 1);
}

TEST(CodegenTest, HybridPreservesLLVMIR) {
    std::string src = R"(
        fn dynamic_fn(x) { return x; }
        fn main() { return dynamic_fn(42); }
    )";
    CodeGenerator codegen;
    generateHybridIR(src, codegen);

    // The LLVM module should still have both functions as IR
    auto* mod = codegen.getModule();
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("main"), nullptr);
    EXPECT_NE(mod->getFunction("dynamic_fn"), nullptr);
}

TEST(CodegenTest, HybridBytecodeContainsReturn) {
    std::string src = R"(
        fn identity(x) { return x; }
        fn main() { return identity(5); }
    )";
    CodeGenerator codegen;
    generateHybridIR(src, codegen);

    auto& bcFuncs = codegen.getBytecodeFunctions();
    ASSERT_EQ(bcFuncs.size(), 1u);

    // The bytecode should contain at least one RETURN opcode
    auto& code = bcFuncs[0].bytecode;
    bool hasReturn = false;
    for (uint8_t byte : code) {
        if (byte == static_cast<uint8_t>(OpCode::RETURN)) {
            hasReturn = true;
            break;
        }
    }
    EXPECT_TRUE(hasReturn);
}
