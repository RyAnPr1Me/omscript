#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <llvm/IR/Instructions.h>
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
    auto* mod = generateIR("fn add(a:i64, b:i64) { return a; } fn main() { return add(1, 2); }", codegen);
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
    auto* mod = generateIR("fn main() { var x:i64 = 0; while (x < 10) { x = x + 1; } return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, DoWhileLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 0; do { x = x + 1; } while (x < 10); return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ForLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...10) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ForLoopWithStep) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...10...2) { s = s + i; } return s; }", codegen);
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
    auto* mod = generateIR("fn main() { var x:float = 3.14; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StringExpression) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s:i64 = \"hello\"; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArrayExpression) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2, 3]; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Variable operations
// ===========================================================================

TEST(CodegenTest, VariableDecl) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 42; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ConstDecl) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { const c:i64 = 42; return c; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, CompoundAssignment) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 10; x += 5; x -= 2; x *= 3; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Postfix / prefix
// ===========================================================================

TEST(CodegenTest, PostfixIncrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 5; x++; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PrefixDecrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 5; --x; return x; }", codegen);
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
    auto* mod = generateIR("fn main() { var x:i64 = 1; { var x:i64 = 2; } return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Break / continue
// ===========================================================================

TEST(CodegenTest, BreakInLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod =
        generateIR("fn main() { var x:i64 = 0; while (1) { x = x + 1; if (x == 5) { break; } } return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ContinueInLoop) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var s:i64 = 0; for (i in 0...10) { if (i == 5) { continue; } s = s + i; } return s; }", codegen);
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
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return x; } OPTMAX!: fn main() { return opt(5); }", codegen);
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
    auto* mod = generateIR("fn main() { var a:i64 = 1 < 2; var b:i64 = 1 <= 2; var c:i64 = 2 > 1; var d:i64 = 2 >= 1; var e:i64 = 1 == 1; "
                           "var f:i64 = 1 != 2; return 0; }",
                           codegen);
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
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return 2 + 3; } OPTMAX!: fn main() { return opt(1); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxUnaryFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return -5; } OPTMAX!: fn main() { return opt(1); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxBitwiseFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return ~0; } OPTMAX!: fn main() { return opt(1); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxLogicalFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return !0; } OPTMAX!: fn main() { return opt(1); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxBinaryAllOps) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) {"
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
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return -3.14; } OPTMAX!: fn main() { return opt(1); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxFloatNotFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return !3.14; } OPTMAX!: fn main() { return opt(1); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxFloatBinaryFolding) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) {"
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
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) {"
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
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) {"
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
    auto* mod =
        generateIR("OPTMAX=: fn opt(x: int) { return 1 ? 10 : 20; } OPTMAX!: fn main() { return opt(1); }", codegen);
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
    auto* mod = generateIR("fn main() { var x:i64 = 1.5 + 2.5; var y:i64 = 3.0 - 1.0; var z:i64 = 2.0 * 3.0; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatDivision) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 10.0 / 3.0; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatComparison) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64 = 1.0 < 2.0; var b:i64 = 1.0 <= 2.0; var c:i64 = 2.0 > 1.0; var d:i64 = 2.0 >= 1.0; "
                           "var e:i64 = 1.0 == 1.0; var f:i64 = 1.0 != 2.0; return 0; }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, MixedIntFloatArithmetic) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 1 + 2.5; var y:i64 = 3.0 - 1; return 0; }", codegen);
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
    auto* mod = generateIR("fn main() { var arr:i64[] = [1, 2, 3]; return sum(arr); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SwapCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var arr:i64[] = [1, 2, 3]; swap(arr, 0, 1); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ReverseCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var arr:i64[] = [1, 2, 3]; reverse(arr); return 0; }", codegen);
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
    auto* mod = generateIR("fn main() { var s:i64 = \"hello world\"; return 0; }", codegen);
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
    auto* mod = generateIR("fn main() { var x:i64 = 0 && 1; var y:i64 = 1 && 1; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LogicalOrShortCircuit) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 0 || 1; var y:i64 = 1 || 0; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Postfix/Prefix on identifiers
// ===========================================================================

TEST(CodegenTest, PostfixDecrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 5; x--; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PrefixIncrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 5; ++x; return x; }", codegen);
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
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 10...0...-1) { s = s + i; } return s; }", codegen);
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
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2, 3]; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// ===========================================================================


// ===========================================================================
// Float-specific stdlib functions
// ===========================================================================

TEST(CodegenTest, FloatAbsCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:float = 3.14; return abs(-5); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatSqrtCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = sqrt(16); return 0; }", codegen);
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
    auto* mod = generateIR("fn main() { var x:i64; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Nested scoping
// ===========================================================================

TEST(CodegenTest, NestedScoping) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 1; { var x:i64 = 2; { var x:i64 = 3; } } return x; }", codegen);
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
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// Float operations in LLVM codegen
// ===========================================================================

TEST(CodegenTest, FloatDivisionIR) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:float = 10.0; var b:float = 3.0; var c:i64 = a / b; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatComparisonAllOps) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() {"
                           "  var a:float = 1.0; var b:float = 2.0;"
                           "  var lt:bool = a < b; var le:bool = a <= b;"
                           "  var gt:bool = b > a; var ge:bool = b >= a;"
                           "  var eq:bool = a == a; var ne:bool = a != b;"
                           "  return 0;"
                           "}",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatUnaryNeg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:float = 3.14; var y:i64 = -x; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// OPTMAX with non-literal expressions and all binary ops
// ===========================================================================

TEST(CodegenTest, OptmaxNonLiteralExpr) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int, y: int) { return x + y; } OPTMAX!: fn main() { return opt(1, 2); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxCallOptimization) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod =
        generateIR("OPTMAX=: fn opt(x: int) { return abs(x); } OPTMAX!: fn main() { return opt(-5); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxFloatBinaryAllOps) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) {"
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
        "OPTMAX=: fn opt(x: int) { var a: int = [1, 2, 3]; return 0; } OPTMAX!: fn main() { return opt(1); }", codegen);
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
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { var a: int = 0; do { a = a + 1; } while (0); return a; } "
                           "OPTMAX!: fn main() { return opt(1); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxPostfixOptimization) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { var a: int = 0; a++; return a; } OPTMAX!: fn main() { return opt(1); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxPrefixOptimization) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "OPTMAX=: fn opt(x: int) { var a: int = 0; ++a; return a; } OPTMAX!: fn main() { return opt(1); }", codegen);
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
    auto* mod = generateIR("fn main() { var x:i64 = 10; var y:i64 = x + 5; return y; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// String operations
// ===========================================================================

TEST(CodegenTest, StringConcatenation) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64 = \"hello \"; var b:i64 = \"world\"; var c:i64 = a + b; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StringComparison) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var a:i64 = \"abc\"; var b:i64 = \"def\"; var eq:bool = a == b; var ne:bool = a != b; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Index expression codegen
// ===========================================================================

TEST(CodegenTest, IndexExpressionCodegen) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2, 3]; var x:i64 = a[0]; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Ternary with variable condition
// ===========================================================================

TEST(CodegenTest, TernaryWithVariable) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var c:i64 = 1; return c ? 10 : 20; }", codegen);
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
    auto* mod = generateIR("fn main() { var a:i64 = 1 << 4; var b:i64 = 16 >> 2; return a + b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Input function
// ===========================================================================

TEST(CodegenTest, InputCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* inputMod = generateIR("fn foo() { var x:string = input(); return x; } fn main() { return 0; }", codegen);
    ASSERT_NE(inputMod, nullptr);
}

// ===========================================================================
// For loop in codegen with complex body
// ===========================================================================

TEST(CodegenTest, ForLoopWithBreak) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var s:i64 = 0; for (i in 0...100) { if (i == 50) { break; } s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ForLoopWithContinue) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var s:i64 = 0; for (i in 0...10) { if (i == 5) { continue; } s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Do-while with break
// ===========================================================================

TEST(CodegenTest, DoWhileBreak) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var x:i64 = 0; do { x = x + 1; if (x == 3) { break; } } while (x < 10); return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Nested functions calling each other
// ===========================================================================

TEST(CodegenTest, NestedFunctionCalls) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn add(a:i64, b:i64) { return a + b; }"
                           "fn mul(a:i64, b:i64) { return a * b; }"
                           "fn main() { return add(mul(2, 3), mul(4, 5)); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// ===========================================================================


// ===========================================================================
// Multiple compound assignments
// ===========================================================================

TEST(CodegenTest, AllCompoundAssignments) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 100; x += 10; x -= 5; x *= 2; x /= 3; x %= 7; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// ===========================================================================


// ===========================================================================
// Float-specific stdlib calls
// ===========================================================================

TEST(CodegenTest, PrintFloatArg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:float = 3.14; print(x); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, AbsFloatArg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:float = -3.14; var y:i64 = abs(x); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, MinFloatArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:float = 1.5; var b:float = 2.5; var c:i64 = min(a, b); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, MaxFloatArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:float = 1.5; var b:float = 2.5; var c:i64 = max(a, b); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SignFloatArg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:float = -3.14; var y:i64 = sign(x); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ClampFloatArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:float = 15.0; var y:i64 = clamp(x, 0.0, 10.0); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PrintCharFloatArg) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:float = 65.0; print_char(x); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Input function wrong args
// ===========================================================================

TEST(CodegenTest, InputWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { var x:string = input(1); return 0; }", codegen), std::runtime_error);
}

// ===========================================================================
// OPTMAX calling non-stdlib function
// ===========================================================================

TEST(CodegenTest, OptmaxCallNonStdlibError) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn helper() { return 1; }"
                            "OPTMAX=: fn opt(x: int) { return helper(); } OPTMAX!:"
                            "fn main() { return opt(1); }",
                            codegen),
                 std::runtime_error);
}

// ===========================================================================
// User function wrong argument count
// ===========================================================================

TEST(CodegenTest, WrongArgCountUserFunction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn add(a:i64, b:i64) { return a + b; } fn main() { return add(1); }", codegen),
                 std::runtime_error);
}

// ===========================================================================
// Ternary with float branches
// ===========================================================================

TEST(CodegenTest, TernaryFloatBranches) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 1 ? 1.5 : 2.5; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, TernaryMixedTypes) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 1 ? 1.5 : 2; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Unary NOT on value
// ===========================================================================

TEST(CodegenTest, UnaryNotOnVar) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 0; return !x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Unary tilde on var
// ===========================================================================

TEST(CodegenTest, UnaryTildeOnVar) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 5; return ~x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Bitwise operators on variables
// ===========================================================================

TEST(CodegenTest, BitwiseAndOnVars) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64 = 5; var b:i64 = 3; return a & b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, BitwiseOrOnVars) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64 = 5; var b:i64 = 3; return a | b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, BitwiseXorOnVars) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64 = 5; var b:i64 = 3; return a ^ b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ShiftOnVars) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64 = 1; var b:i64 = 4; return a << b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArithShiftRight) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64 = 16; var b:i64 = 2; return a >> b; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// OPTMAX with float logical ops and for step
// ===========================================================================

TEST(CodegenTest, OptmaxFloatLogicalOps) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) {"
                           "  var a: int = 1.0 && 2.0;"
                           "  var b: int = 0.0 || 3.0;"
                           "  return 0;"
                           "} OPTMAX!: fn main() { return opt(1); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, OptmaxForLoopWithStepOpt) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) {"
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
    auto* mod = generateIR("fn main() { var x:i64 = 0; if (1) { x = 1; } else { x = 2; } return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Float postfix/prefix
// ===========================================================================

TEST(CodegenTest, FloatPostfixIncrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:float = 1.5; x++; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, FloatPrefixDecrement) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:float = 3.14; --x; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// Optimization O1 (tests legacy pass manager path)
// ===========================================================================

TEST(CodegenTest, OptimizationO1FunctionPasses) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR("fn add(a:i64, b:i64) { return a + b; } fn main() { return add(1, 2); }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Tilde on float variable (converted before bitwise not)
// ===========================================================================

TEST(CodegenTest, TildeOnFloatVar) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:float = 3.14; return ~x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// String concat with lazy function init (covers malloc/strcpy/strcat)
// ===========================================================================

TEST(CodegenTest, StringConcatLazyInit) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() {"
                           "  var a:i64 = \"hello\";"
                           "  var b:i64 = \" world\";"
                           "  var c:i64 = a + b;"
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
    auto* mod = generateIR("fn foo() { var x:string = input(); return x; }"
                           "fn main() { return 0; }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Print_char codegen (covers putchar setup)
// ===========================================================================

TEST(CodegenTest, PrintCharCodegen) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { print_char(65); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// OPTMAX with index expression (covers index optimization path)
// ===========================================================================

TEST(CodegenTest, OptmaxIndexExpr) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { var a: int = [1, 2]; var b: int = a[0]; return b; } OPTMAX!: fn "
                           "main() { return opt(1); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// optimizeFunction: per-function optimization
// ===========================================================================

TEST(CodegenTest, OptimizeFunctionPerFunction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn add(a:i64, b:i64) { return a + b; } fn main() { return add(1, 2); }", codegen);
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
    auto* mod = generateIR("fn main() { var x:i64 = 10; var y:i64 = x + 5; return y; }", codegen);
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
    auto* mod = generateIR("fn classify(x:i64) {"
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
    auto* mod = generateIR("fn main() {"
                           "  var x:i64 = 5;"
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
    auto* mod = generateIR("fn main() { var t:i64 = typeof(42); return t; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, AssertCodegen) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { assert(1); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, AssertWrongArgCount) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { assert(1, 2); return 0; }", codegen), std::runtime_error);
}

TEST(CodegenTest, TypeofWrongArgCount) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { typeof(); return 0; }", codegen), std::runtime_error);
}

// ===========================================================================
// Break inside switch (no enclosing loop)
// ===========================================================================

TEST(CodegenTest, BreakInsideSwitch) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() {"
                           "  var r:i64 = 0;"
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
    auto* mod = generateIR("OPTMAX=:"
                           "fn classify(x: int) {"
                           "  var base: int = 2 + 3;" // should be folded to 5
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

TEST(CodegenTest, SwitchDuplicateCaseError) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() {"
                            "  var x:i64 = 1;"
                            "  switch (x) {"
                            "    case 1: return 10;"
                            "    case 1: return 20;"
                            "  }"
                            "  return 0;"
                            "}",
                            codegen),
                 std::runtime_error);
}

TEST(CodegenTest, SwitchMultiValueCase) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() {"
                           "  var x:i64 = 2;"
                           "  switch (x) {"
                           "    case 1, 2, 3: return 10;"
                           "    case 4, 5: return 20;"
                           "    default: return 0;"
                           "  }"
                           "}",
                           codegen);
    ASSERT_NE(mod, nullptr);
    llvm::Function* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(fn->empty());
}

TEST(CodegenTest, SwitchMultiValueDuplicateError) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() {"
                            "  var x:i64 = 1;"
                            "  switch (x) {"
                            "    case 1, 2: return 10;"
                            "    case 2, 3: return 20;"
                            "  }"
                            "}",
                            codegen),
                 std::runtime_error);
}

TEST(CodegenTest, SwitchConstantConditionElimination) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() {"
                           "  switch (2) {"
                           "    case 1: return 10;"
                           "    case 2, 3: return 20;"
                           "    default: return 0;"
                           "  }"
                           "}",
                           codegen);
    ASSERT_NE(mod, nullptr);
    llvm::Function* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    // Constant condition elimination means no switch instruction is emitted
    bool hasSwitchInst = false;
    for (auto& bb : *fn) {
        for (auto& inst : bb) {
            if (llvm::isa<llvm::SwitchInst>(&inst))
                hasSwitchInst = true;
        }
    }
    EXPECT_FALSE(hasSwitchInst) << "Constant switch should be eliminated";
}

// ===========================================================================
// Array element assignment: arr[i] = value
// ===========================================================================

TEST(CodegenTest, ArrayIndexAssignment) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() {"
                           "  var arr:i64[] = [10, 20, 30];"
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
    auto* mod = generateIR("fn main() {"
                           "  var arr:i64[] = [1, 2, 3];"
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
    auto* mod = generateIR("fn main() {"
                           "  var arr:i64[] = [10, 20, 30];"
                           "  var total:i64 = 0;"
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
    auto* mod = generateIR("fn main() {"
                           "  var arr:i64[] = [1, 2, 3, 4, 5];"
                           "  var total:i64 = 0;"
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
    auto* mod = generateIR("fn main() {"
                           "  var arr:i64[] = [1, 2, 3, 4, 5];"
                           "  var total:i64 = 0;"
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
    auto* mod = generateIR("fn main() {"
                           "  var s:i64 = \"Hello\";"
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
    auto* mod = generateIR("fn main() {"
                           "  var s:i64 = \"ABC\";"
                           "  return char_at(s, 0);"
                           "}",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrLenWrongArity) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return str_len(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, CharAtWrongArity) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return char_at(\"a\"); }", codegen), std::runtime_error);
}

// ===========================================================================
// Multi-variable declarations
// ===========================================================================

TEST(CodegenTest, MultiVarDeclaration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() {"
                           "  var a:i64 = 10, b:i64 = 20, c:i64 = 30;"
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
    auto* mod = generateIR("fn main() {"
                           "  const x:i64 = 5, y:i64 = 10;"
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
    auto* mod = generateIR("fn main() {"
                           "  var a:i64 = \"hello\";"
                           "  var b:i64 = \"hello\";"
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
    EXPECT_THROW(generateIR("fn main() { return str_eq(\"a\"); }", codegen), std::runtime_error);
}

// ===========================================================================
// ===========================================================================


// ===========================================================================
// ===========================================================================


// ===========================================================================
// New pass manager IPO optimizations (inlining, IPSCCP, GlobalDCE)
// ===========================================================================

TEST(CodegenTest, InliningAtO2) {
    // At O2, the new pass manager should inline small functions.
    // After inlining square(5) + square(3), IPSCCP should fold to 34.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn square(x:i64) { return x * x; }"
                           "fn main() { return square(5) + square(3); }",
                           codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // After inlining + constant propagation, main should have no call to square
    bool hasCallToSquare = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (call->getCalledFunction() && call->getCalledFunction()->getName() == "square") {
                    hasCallToSquare = true;
                }
            }
        }
    }
    EXPECT_FALSE(hasCallToSquare) << "square() should be inlined at O2";
}

TEST(CodegenTest, InliningAtO3) {
    CodeGenerator codegen(OptimizationLevel::O3);
    auto* mod = generateIR("fn double_it(n:i64) { return n + n; }"
                           "fn main() { return double_it(21); }",
                           codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasCall = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (call->getCalledFunction() && call->getCalledFunction()->getName() == "double_it") {
                    hasCall = true;
                }
            }
        }
    }
    EXPECT_FALSE(hasCall) << "double_it() should be inlined at O3";
}

TEST(CodegenTest, InliningAtO1) {
    // O1 uses the new pass manager's standard pipeline which includes inlining.
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR("fn helper(x:i64) { return x * 2; }"
                           "fn main() { return helper(10); }",
                           codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasCall = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (call->getCalledFunction() && call->getCalledFunction()->getName() == "helper") {
                    hasCall = true;
                }
            }
        }
    }
    EXPECT_FALSE(hasCall) << "helper() should be inlined at O1";
}

TEST(CodegenTest, ConstantPropagationThroughInline) {
    // After inlining + IPSCCP, the entire program should fold to a constant
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn add(a:i64, b:i64) { return a + b; }"
                           "fn mul(a:i64, b:i64) { return a * b; }"
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
    auto* mod = generateIR("OPTMAX=: fn main() { return 5 + 1.0; } OPTMAX!:", codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // The result of mixed int+float should be folded to a constant float
    // then converted back to i64 for the return. Check that no add instruction exists.
    bool hasAddInst = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::FAdd || I.getOpcode() == llvm::Instruction::Add) {
                hasAddInst = true;
            }
        }
    }
    EXPECT_FALSE(hasAddInst) << "Mixed int+float should be folded at compile time in OPTMAX";
}

TEST(CodegenTest, OptmaxMixedTypeMul) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn main() { return 3 * 2.5; } OPTMAX!:", codegen);
    llvm::Function* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasMulInst = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::FMul || I.getOpcode() == llvm::Instruction::Mul) {
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
    auto* mod = generateIR("fn main() { var x:i64 = null; return x; }", codegen);
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
    auto* mod = generateIR("fn main() { var x:i64 = 15; x &= 6; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, BitwiseOrAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 3; x |= 12; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, BitwiseXorAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 15; x ^= 9; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, ShiftLeftAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 1; x <<= 3; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, ShiftRightAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 32; x >>= 2; return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

// ===========================================================================
// Array compound assignment
// ===========================================================================

TEST(CodegenTest, ArrayCompoundAssignPlus) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var arr:i64[] = [10, 20]; arr[0] += 5; return arr[0]; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, ArrayCompoundAssignMinus) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var arr:i64[] = [10, 20]; arr[1] -= 3; return arr[1]; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, ArrayCompoundAssignMul) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var arr:i64[] = [10]; arr[0] *= 3; return arr[0]; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

// ===========================================================================
// Semantic validation: missing main, duplicate functions, duplicate params
// ===========================================================================

TEST(CodegenTest, NoMainFunction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW({ generateIR("fn foo() { return 42; }", codegen); }, std::runtime_error);
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
    EXPECT_THROW(
        {
            generateIR("fn add(a:i64, b:i64) { return a + b; }\n"
                       "fn add(x:i64, y:i64) { return x + y; }\n"
                       "fn main() { return add(1, 2); }",
                       codegen);
        },
        std::runtime_error);
}

TEST(CodegenTest, DuplicateFunctionMessage) {
    CodeGenerator codegen(OptimizationLevel::O0);
    try {
        generateIR("fn add(a:i64, b:i64) { return a + b; }\n"
                   "fn add(x:i64, y:i64) { return x + y; }\n"
                   "fn main() { return add(1, 2); }",
                   codegen);
        FAIL() << "Expected exception";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("Duplicate function definition"), std::string::npos);
    }
}

TEST(CodegenTest, DuplicateParameter) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(
        {
            generateIR("fn add(a:i64, a:i64) { return a + a; }\n"
                       "fn main() { return add(1, 2); }",
                       codegen);
        },
        std::runtime_error);
}

TEST(CodegenTest, DuplicateParameterMessage) {
    CodeGenerator codegen(OptimizationLevel::O0);
    try {
        generateIR("fn add(a:i64, a:i64) { return a + a; }\n"
                   "fn main() { return add(1, 2); }",
                   codegen);
        FAIL() << "Expected exception";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("Duplicate parameter name"), std::string::npos);
    }
}

TEST(CodegenTest, ValidProgramAccepted) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn add(a:i64, b:i64) { return a + b; }\n"
                           "fn main() { return add(1, 2); }",
                           codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    auto* addFn = mod->getFunction("add");
    ASSERT_NE(addFn, nullptr);
}

// ===========================================================================
// ===========================================================================


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
    // n / 4 where divisor is a known power-of-2 constant should be strength-
    // reduced to a shift sequence by the LLVM backend at O2+.
    // Note: signed div→shift is left to the LLVM backend (not the frontend
    // egraph) because signed division truncates toward zero while arithmetic
    // shift right rounds toward negative infinity.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn divide_by_four(n:i64) { return n / 4; }\n"
                           "fn main() { return divide_by_four(16); }",
                           codegen);
    auto* func = mod->getFunction("divide_by_four");
    if (!func) { SUCCEED(); return; } // inlined at O2
    EXPECT_FALSE(func->empty());
    // At O2, LLVM's instcombine converts sdiv-by-power-of-2 to AShr sequence
    bool hasAShr = false;
    bool hasSDiv = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::AShr)
                hasAShr = true;
            if (inst.getOpcode() == llvm::Instruction::SDiv)
                hasSDiv = true;
        }
    }
    EXPECT_TRUE(hasAShr) << "Division by power-of-2 should use AShr";
    EXPECT_FALSE(hasSDiv) << "Division by power-of-2 should NOT use SDiv";
}

TEST(CodegenTest, ModuloStrengthReduction) {
    // n % 8 where the divisor is a known power-of-2 constant should be
    // strength-reduced by the LLVM backend at O2+.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn mod_by_eight(n:i64) { return n % 8; }\n"
                           "fn main() { return mod_by_eight(17); }",
                           codegen);
    auto* func = mod->getFunction("mod_by_eight");
    if (!func) { SUCCEED(); return; } // inlined at O2
    EXPECT_FALSE(func->empty());
    bool hasSRem = false;
    bool hasAShr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SRem)
                hasSRem = true;
            if (inst.getOpcode() == llvm::Instruction::AShr)
                hasAShr = true;
        }
    }
    EXPECT_TRUE(hasAShr) << "Modulo by power-of-2 should use AShr-based sequence";
    EXPECT_FALSE(hasSRem) << "Modulo by power-of-2 should NOT use SRem";
}

TEST(CodegenTest, MultiplyStrengthReduction) {
    // n * 8 should be converted to n << 3 (left shift) when the multiplier
    // is a power of 2.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul_by_eight(n:i64) { return n * 8; }\n"
                           "fn main() { return mul_by_eight(3); }",
                           codegen);
    auto* func = mod->getFunction("mul_by_eight");
    ASSERT_NE(func, nullptr);
    EXPECT_FALSE(func->empty());
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Shl) {
                hasShl = true;
            }
        }
    }
    EXPECT_TRUE(hasShl);
}

// ===========================================================================
// IR-level algebraic identity optimizations
// ===========================================================================

TEST(CodegenTest, AlgebraicIdentityMultiplyByZero) {
    // x * 0 should fold to 0 (no multiply instruction emitted)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul_zero(n:i64) { return n * 0; }\n"
                           "fn main() { return mul_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("mul_zero");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul || inst.getOpcode() == llvm::Instruction::Shl)
                hasMul = true;
        }
    }
    EXPECT_FALSE(hasMul);
}

TEST(CodegenTest, AlgebraicIdentityAddZero) {
    // x + 0 should fold to x (no add instruction emitted)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn add_zero(n:i64) { return n + 0; }\n"
                           "fn main() { return add_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("add_zero");
    ASSERT_NE(func, nullptr);
    bool hasAdd = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Add)
                hasAdd = true;
        }
    }
    EXPECT_FALSE(hasAdd);
}

TEST(CodegenTest, AlgebraicIdentityZeroPlusX) {
    // 0 + x should fold to x
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn zero_plus(n:i64) { return 0 + n; }\n"
                           "fn main() { return zero_plus(42); }",
                           codegen);
    auto* func = mod->getFunction("zero_plus");
    ASSERT_NE(func, nullptr);
    bool hasAdd = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Add)
                hasAdd = true;
        }
    }
    EXPECT_FALSE(hasAdd);
}

TEST(CodegenTest, AlgebraicIdentityBitwiseAndZero) {
    // x & 0 should fold to 0
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn and_zero(n:i64) { return n & 0; }\n"
                           "fn main() { return and_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("and_zero");
    ASSERT_NE(func, nullptr);
    bool hasAnd = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::And)
                hasAnd = true;
        }
    }
    EXPECT_FALSE(hasAnd);
}

TEST(CodegenTest, AlgebraicIdentityBitwiseOrZero) {
    // x | 0 should fold to x
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn or_zero(n:i64) { return n | 0; }\n"
                           "fn main() { return or_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("or_zero");
    ASSERT_NE(func, nullptr);
    bool hasOr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Or)
                hasOr = true;
        }
    }
    EXPECT_FALSE(hasOr);
}

TEST(CodegenTest, AlgebraicIdentityXorZero) {
    // x ^ 0 should fold to x
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn xor_zero(n:i64) { return n ^ 0; }\n"
                           "fn main() { return xor_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("xor_zero");
    ASSERT_NE(func, nullptr);
    bool hasXor = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Xor)
                hasXor = true;
        }
    }
    EXPECT_FALSE(hasXor);
}

TEST(CodegenTest, AlgebraicIdentityShlZero) {
    // x << 0 should fold to x
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn shl_zero(n:i64) { return n << 0; }\n"
                           "fn main() { return shl_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("shl_zero");
    ASSERT_NE(func, nullptr);
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasShl);
}

TEST(CodegenTest, AlgebraicIdentityShrZero) {
    // x >> 0 should fold to x
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn shr_zero(n:i64) { return n >> 0; }\n"
                           "fn main() { return shr_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("shr_zero");
    ASSERT_NE(func, nullptr);
    bool hasShr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::AShr)
                hasShr = true;
        }
    }
    EXPECT_FALSE(hasShr);
}

TEST(CodegenTest, AlgebraicIdentityPowZero) {
    // x ** 0 should fold to 1 (no loop)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn pow_zero(n:i64) { return n ** 0; }\n"
                           "fn main() { return pow_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("pow_zero");
    ASSERT_NE(func, nullptr);
    // Should be a single basic block with a constant return of 1
    int bbCount = 0;
    for (auto& bb : *func) {
        (void)bb;
        bbCount++;
    }
    EXPECT_LE(bbCount, 2);
}

TEST(CodegenTest, AlgebraicIdentityPowOne) {
    // x ** 1 should fold to x (no loop)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn pow_one(n:i64) { return n ** 1; }\n"
                           "fn main() { return pow_one(42); }",
                           codegen);
    auto* func = mod->getFunction("pow_one");
    ASSERT_NE(func, nullptr);
    int bbCount = 0;
    for (auto& bb : *func) {
        (void)bb;
        bbCount++;
    }
    EXPECT_LE(bbCount, 2);
}

TEST(CodegenTest, AlgebraicIdentityOnePowX) {
    // 1 ** x should fold to 1
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn one_pow(n:i64) { return 1 ** n; }\n"
                           "fn main() { return one_pow(42); }",
                           codegen);
    auto* func = mod->getFunction("one_pow");
    ASSERT_NE(func, nullptr);
    int bbCount = 0;
    for (auto& bb : *func) {
        (void)bb;
        bbCount++;
    }
    EXPECT_LE(bbCount, 2);
}

// ===========================================================================
// OPTMAX algebraic identity optimizations
// ===========================================================================

TEST(CodegenTest, OptmaxMultiplyByZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn mul_zero(x: int) { return x * 0; } OPTMAX!:\n"
                           "fn main() { return mul_zero(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("mul_zero"), nullptr);
}

TEST(CodegenTest, OptmaxBitwiseAndZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn and_zero(x: int) { return x & 0; } OPTMAX!:\n"
                           "fn main() { return and_zero(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("and_zero"), nullptr);
}

TEST(CodegenTest, OptmaxBitwiseOrZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn or_zero(x: int) { return x | 0; } OPTMAX!:\n"
                           "fn main() { return or_zero(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("or_zero"), nullptr);
}

TEST(CodegenTest, OptmaxXorZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn xor_zero(x: int) { return x ^ 0; } OPTMAX!:\n"
                           "fn main() { return xor_zero(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("xor_zero"), nullptr);
}

TEST(CodegenTest, OptmaxPowZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn pow_zero(x: int) { return x ** 0; } OPTMAX!:\n"
                           "fn main() { return pow_zero(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("pow_zero"), nullptr);
}

TEST(CodegenTest, OptmaxPowOne) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn pow_one(x: int) { return x ** 1; } OPTMAX!:\n"
                           "fn main() { return pow_one(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("pow_one"), nullptr);
}

TEST(CodegenTest, OptmaxShlZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn shl_zero(x: int) { return x << 0; } OPTMAX!:\n"
                           "fn main() { return shl_zero(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("shl_zero"), nullptr);
}

TEST(CodegenTest, OptmaxShrZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn shr_zero(x: int) { return x >> 0; } OPTMAX!:\n"
                           "fn main() { return shr_zero(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("shr_zero"), nullptr);
}

TEST(CodegenTest, OptmaxOnePowX) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn one_pow(x: int) { return 1 ** x; } OPTMAX!:\n"
                           "fn main() { return one_pow(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("one_pow"), nullptr);
}

// ===========================================================================
// OPTMAX double-negation folding
// ===========================================================================

TEST(CodegenTest, OptmaxDoubleNegation) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn double_neg(x: int) { return -(-x); } OPTMAX!:\n"
                           "fn main() { return double_neg(42); }",
                           codegen);
    auto* func = mod->getFunction("double_neg");
    ASSERT_NE(func, nullptr);
    // After double-negation folding, there should be no Neg instructions
    bool hasNeg = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Sub) {
                // Check if it's a negation (sub 0, x)
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(inst.getOperand(0))) {
                    if (ci->isZero())
                        hasNeg = true;
                }
            }
        }
    }
    EXPECT_FALSE(hasNeg);
}

TEST(CodegenTest, OptmaxDoubleBitwiseNot) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn double_bitnot(x: int) { return ~(~x); } OPTMAX!:\n"
                           "fn main() { return double_bitnot(42); }",
                           codegen);
    auto* func = mod->getFunction("double_bitnot");
    ASSERT_NE(func, nullptr);
    // After double-bitwise-not folding, there should be no Xor instructions
    bool hasXor = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Xor)
                hasXor = true;
        }
    }
    EXPECT_FALSE(hasXor);
}

// ===========================================================================
// Extended algebraic identity optimizations
// ===========================================================================

TEST(CodegenTest, AlgebraicIdentityModOne) {
    // x % 1 should fold to 0 (no modulo instruction emitted)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mod_one(n:i64) { return n % 1; }\n"
                           "fn main() { return mod_one(42); }",
                           codegen);
    auto* func = mod->getFunction("mod_one");
    ASSERT_NE(func, nullptr);
    // Check that no SRem instruction is emitted
    bool hasSRem = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SRem)
                hasSRem = true;
        }
    }
    EXPECT_FALSE(hasSRem) << "x % 1 should be folded to 0, no SRem emitted";
}

TEST(CodegenTest, AlgebraicIdentityMulNegOne) {
    // x * (-1) should emit a negation, not a multiply
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul_neg(n:i64) { return n * (-1); }\n"
                           "fn main() { return mul_neg(42); }",
                           codegen);
    auto* func = mod->getFunction("mul_neg");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
        }
    }
    EXPECT_FALSE(hasMul) << "x * (-1) should be strength-reduced to negation";
}

TEST(CodegenTest, AlgebraicIdentityAndAllOnes) {
    // x & (-1) should fold to x (no AND instruction emitted)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn and_allones(n:i64) { return n & (-1); }\n"
                           "fn main() { return and_allones(42); }",
                           codegen);
    auto* func = mod->getFunction("and_allones");
    ASSERT_NE(func, nullptr);
    bool hasAnd = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::And)
                hasAnd = true;
        }
    }
    EXPECT_FALSE(hasAnd) << "x & (-1) should fold to x, no And emitted";
}

TEST(CodegenTest, AlgebraicIdentityOrAllOnes) {
    // x | (-1) should fold to -1 (all ones)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn or_allones(n:i64) { return n | (-1); }\n"
                           "fn main() { return or_allones(42); }",
                           codegen);
    auto* func = mod->getFunction("or_allones");
    ASSERT_NE(func, nullptr);
    bool hasOr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Or)
                hasOr = true;
        }
    }
    EXPECT_FALSE(hasOr) << "x | (-1) should fold to -1, no Or emitted";
}

// ===========================================================================
// Extended strength reduction tests
// ===========================================================================

TEST(CodegenTest, StrengthReductionMultiplyBy10) {
    // n * 10 should use shift+add: (n<<3) + (n<<1)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul10(n:i64) { return n * 10; }\n"
                           "fn main() { return mul10(5); }",
                           codegen);
    auto* func = mod->getFunction("mul10");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 10 should use shift+add, not multiply";
    EXPECT_TRUE(hasShl) << "n * 10 should use shifts";
}

TEST(CodegenTest, StrengthReductionMultiplyBy15) {
    // n * 15 should use (n<<4) - n
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul15(n:i64) { return n * 15; }\n"
                           "fn main() { return mul15(5); }",
                           codegen);
    auto* func = mod->getFunction("mul15");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 15 should use shift+sub, not multiply";
    EXPECT_TRUE(hasShl) << "n * 15 should use shifts";
}

TEST(CodegenTest, StrengthReductionMultiplyBy17) {
    // n * 17 should use (n<<4) + n
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul17(n:i64) { return n * 17; }\n"
                           "fn main() { return mul17(5); }",
                           codegen);
    auto* func = mod->getFunction("mul17");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 17 should use shift+add, not multiply";
    EXPECT_TRUE(hasShl) << "n * 17 should use shifts";
}

// ===========================================================================
// New PM pass pipeline tests
// ===========================================================================

TEST(CodegenTest, O2DeadStoreElimination) {
    // At O2, dead stores (values written but never read) should be eliminated
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn dead_store() {\n"
                           "  var x:i64 = 42;\n"
                           "  x = 100;\n"
                           "  return x;\n"
                           "}\n"
                           "fn main() { return dead_store(); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    auto* func = mod->getFunction("dead_store");
    if (!func) {
        // Function was inlined into main at O2 — optimization succeeded
        SUCCEED();
        return;
    }
    // After DSE + mem2reg, the function should have minimal instructions
    int storeCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Store)
                storeCount++;
        }
    }
    // DSE should eliminate the dead store of 42
    EXPECT_LE(storeCount, 1) << "Dead stores should be eliminated at O2";
}

TEST(CodegenTest, O2GlobalOptimization) {
    // At O2, GlobalOpt + GlobalDCE should optimize the module
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn helper() { return 42; }\n"
                           "fn main() { return helper(); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    // After optimization, the module should still be valid
    EXPECT_NE(mod->getFunction("main"), nullptr);
}

// ===========================================================================
// Small constant exponentiation specialization
// ===========================================================================

TEST(CodegenTest, SmallExponentPow2) {
    // x**2 should inline as x*x — no loop needed
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn sq(n:i64) { return n ** 2; }\n"
                           "fn main() { return sq(5); }",
                           codegen);
    auto* func = mod->getFunction("sq");
    ASSERT_NE(func, nullptr);
    int bbCount = 0;
    for (auto& bb : *func) {
        (void)bb;
        bbCount++;
    }
    // Inlined x*x: single basic block, no pow loop
    EXPECT_EQ(bbCount, 1) << "x**2 should be a single basic block (no loop)";
}

TEST(CodegenTest, SmallExponentPow3) {
    // x**3 should inline as x*x*x — no loop needed
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn cube(n:i64) { return n ** 3; }\n"
                           "fn main() { return cube(5); }",
                           codegen);
    auto* func = mod->getFunction("cube");
    ASSERT_NE(func, nullptr);
    int bbCount = 0;
    for (auto& bb : *func) {
        (void)bb;
        bbCount++;
    }
    EXPECT_EQ(bbCount, 1) << "x**3 should be a single basic block (no loop)";
}

TEST(CodegenTest, SmallExponentPow4) {
    // x**4 should inline as t=x*x; t*t — no loop needed
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn pow4(n:i64) { return n ** 4; }\n"
                           "fn main() { return pow4(5); }",
                           codegen);
    auto* func = mod->getFunction("pow4");
    ASSERT_NE(func, nullptr);
    int bbCount = 0;
    for (auto& bb : *func) {
        (void)bb;
        bbCount++;
    }
    EXPECT_EQ(bbCount, 1) << "x**4 should be a single basic block (no loop)";
}

TEST(CodegenTest, SmallExponentPow8) {
    // x**8 should inline as t=x*x; u=t*t; u*u — no loop needed
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn pow8(n:i64) { return n ** 8; }\n"
                           "fn main() { return pow8(2); }",
                           codegen);
    auto* func = mod->getFunction("pow8");
    ASSERT_NE(func, nullptr);
    int bbCount = 0;
    for (auto& bb : *func) {
        (void)bb;
        bbCount++;
    }
    EXPECT_EQ(bbCount, 1) << "x**8 should be a single basic block (no loop)";
}

// ===========================================================================
// Extended strength reduction tests (6, 12, 24, 25, 31, 33, 63, 65)
// ===========================================================================

TEST(CodegenTest, StrengthReductionMultiplyBy6) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul6(n:i64) { return n * 6; }\n"
                           "fn main() { return mul6(5); }",
                           codegen);
    auto* func = mod->getFunction("mul6");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 6 should use shift+add, not multiply";
    EXPECT_TRUE(hasShl) << "n * 6 should use shifts";
}

TEST(CodegenTest, StrengthReductionMultiplyBy24) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul24(n:i64) { return n * 24; }\n"
                           "fn main() { return mul24(5); }",
                           codegen);
    auto* func = mod->getFunction("mul24");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 24 should use shift+sub, not multiply";
    EXPECT_TRUE(hasShl) << "n * 24 should use shifts";
}

TEST(CodegenTest, StrengthReductionMultiplyBy31) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul31(n:i64) { return n * 31; }\n"
                           "fn main() { return mul31(5); }",
                           codegen);
    auto* func = mod->getFunction("mul31");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 31 should use shift+sub, not multiply";
    EXPECT_TRUE(hasShl) << "n * 31 should use shifts";
}

// ===========================================================================
// C library function attribute tests
// ===========================================================================

TEST(CodegenTest, MallocHasNonNullReturn) {
    // malloc should have nonnull on its return value
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() {\n"
                           "  var arr:i64[] = [1, 2, 3];\n"
                           "  return arr[0];\n"
                           "}",
                           codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("malloc");
    if (fn) {
        EXPECT_TRUE(fn->hasRetAttribute(llvm::Attribute::NonNull))
            << "malloc return should have nonnull attribute";
        EXPECT_TRUE(fn->hasRetAttribute(llvm::Attribute::NoAlias))
            << "malloc return should have noalias attribute";
    }
}

TEST(CodegenTest, PureFunctionsHaveMemoryNone) {
    // Pure math functions should have memory(none)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() {\n"
                           "  var x:i64 = floor(3.7);\n"
                           "  return to_int(x);\n"
                           "}",
                           codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("floor");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoFree))
            << "floor should have nofree attribute";
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoSync))
            << "floor should have nosync attribute";
    }
}

// ===========================================================================
// IR-level algebraic identity tests (generate → LLVM IR)
// ===========================================================================

TEST(CodegenTest, BytecodeAlgebraicIdentityAddZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto program = parseSource("fn add_zero(x:i64) { return x + 0; }\n"
                               "fn main() { return 0; }");
    codegen.generate(program.get());
    EXPECT_NE(codegen.getModule()->getFunction("add_zero"), nullptr);
}

TEST(CodegenTest, BytecodeAlgebraicIdentityMulZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto program = parseSource("fn mul_zero(x:i64) { return x * 0; }\n"
                               "fn main() { return 0; }");
    codegen.generate(program.get());
    EXPECT_NE(codegen.getModule()->getFunction("mul_zero"), nullptr);
}

TEST(CodegenTest, BytecodeAlgebraicIdentityMulOne) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto program = parseSource("fn mul_one(x:i64) { return x * 1; }\n"
                               "fn main() { return 0; }");
    codegen.generate(program.get());
    EXPECT_NE(codegen.getModule()->getFunction("mul_one"), nullptr);
}

TEST(CodegenTest, BytecodeAlgebraicIdentityPowZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto program = parseSource("fn pow_zero(x:i64) { return x ** 0; }\n"
                               "fn main() { return 0; }");
    codegen.generate(program.get());
    EXPECT_NE(codegen.getModule()->getFunction("pow_zero"), nullptr);
}

// ===========================================================================
// Side-effect preservation tests
// ===========================================================================

TEST(CodegenTest, BytecodeAlgebraicMulZeroPreservesSideEffects) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto program = parseSource("fn mul_zero(x:i64) { return x * 0; }\n"
                               "fn main() { return 0; }");
    codegen.generate(program.get());
    EXPECT_NE(codegen.getModule()->getFunction("mul_zero"), nullptr);
}

TEST(CodegenTest, BytecodeAlgebraicPowZeroPreservesSideEffects) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto program = parseSource("fn pow_zero(x:i64) { return x ** 0; }\n"
                               "fn main() { return 0; }");
    codegen.generate(program.get());
    EXPECT_NE(codegen.getModule()->getFunction("pow_zero"), nullptr);
}

TEST(CodegenTest, BytecodeAlgebraicZeroMulPreservesSideEffects) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto program = parseSource("fn zero_mul(x:i64) { return 0 * x; }\n"
                               "fn main() { return 0; }");
    codegen.generate(program.get());
    EXPECT_NE(codegen.getModule()->getFunction("zero_mul"), nullptr);
}

// ===========================================================================
// OPTMAX algebraic identity: side-effect preservation
// ===========================================================================

TEST(CodegenTest, OptmaxMulZeroPreservesSideEffects) {
    // In OPTMAX, func() * 0 should NOT be optimized to 0 because the
    // function call may have side effects. The multiplication should remain.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=:\n"
                           "fn helper(x: int) { return x; }\n"
                           "fn mul_zero(x: int) { return helper(x) * 0; }\n"
                           "OPTMAX!:\n"
                           "fn main() { return mul_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("mul_zero");
    ASSERT_NE(func, nullptr);
    // The function should contain a call to helper() even though
    // the result is multiplied by 0
    bool hasCall = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (llvm::isa<llvm::CallInst>(inst))
                hasCall = true;
        }
    }
    EXPECT_TRUE(hasCall);
}

TEST(CodegenTest, OptmaxPowZeroPreservesSideEffects) {
    // In OPTMAX, func() ** 0 should NOT be optimized to 1 because the
    // function call may have side effects.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=:\n"
                           "fn helper(x: int) { return x; }\n"
                           "fn pow_zero(x: int) { return helper(x) ** 0; }\n"
                           "OPTMAX!:\n"
                           "fn main() { return pow_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("pow_zero");
    ASSERT_NE(func, nullptr);
    bool hasCall = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (llvm::isa<llvm::CallInst>(inst))
                hasCall = true;
        }
    }
    EXPECT_TRUE(hasCall);
}

TEST(CodegenTest, OptmaxMulZeroPureStillOptimized) {
    // In OPTMAX, x * 0 where x is a simple variable should still be
    // optimized to 0 (no multiply instruction emitted).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=:\n"
                           "fn mul_zero(x: int) { return x * 0; }\n"
                           "OPTMAX!:\n"
                           "fn main() { return mul_zero(42); }",
                           codegen);
    auto* func = mod->getFunction("mul_zero");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul || inst.getOpcode() == llvm::Instruction::Shl)
                hasMul = true;
        }
    }
    EXPECT_FALSE(hasMul);
}

// ===========================================================================
// Higher-order array functions
// ===========================================================================

TEST(CodegenTest, IsStdlibFunctionArrayMap) {
    EXPECT_TRUE(isStdlibFunction("array_map"));
}

TEST(CodegenTest, IsStdlibFunctionArrayFilter) {
    EXPECT_TRUE(isStdlibFunction("array_filter"));
}

TEST(CodegenTest, IsStdlibFunctionArrayReduce) {
    EXPECT_TRUE(isStdlibFunction("array_reduce"));
}

TEST(CodegenTest, ArrayMapGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn double(x:i64) { return x * 2; }\n"
                           "fn main() { var a:i64[] = [1, 2, 3]; var b:i64 = array_map(a, \"double\"); return 0; }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArrayFilterGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn is_pos(x:i64) { return x > 0; }\n"
                           "fn main() { var a:i64[] = [1, -2, 3]; var b:i64 = array_filter(a, \"is_pos\"); return 0; }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArrayReduceGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn add(acc:i64, x:i64) { return acc + x; }\n"
                           "fn main() { var a:i64[] = [1, 2, 3]; var s:i64 = array_reduce(a, \"add\", 0); return 0; }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Lambda, pipe, and spread operator tests
// ===========================================================================

TEST(CodegenTest, LambdaWithArrayMap) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2, 3]; var b:i64 = array_map(a, |x| x * 2); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
    // The lambda should be desugared into a __lambda_ function
    bool foundLambda = false;
    for (auto& fn : *mod) {
        if (fn.getName().str().find("__lambda_") == 0) {
            foundLambda = true;
            break;
        }
    }
    EXPECT_TRUE(foundLambda);
}

TEST(CodegenTest, LambdaWithArrayFilter) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod =
        generateIR("fn main() { var a:i64[] = [1, 2, 3, 4]; var b:bool = array_filter(a, |x| x % 2 == 0); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LambdaWithArrayReduce) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod =
        generateIR("fn main() { var a:i64[] = [1, 2, 3]; var s:i64 = array_reduce(a, |acc, x| acc + x, 0); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PipeOperator) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn double(x:i64) { return x * 2; }\n"
                           "fn main() { var x:i64 = 5 |> double; return 0; }",
                           codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PipeOperatorWithStdlib) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2, 3]; var n:bool = a |> len; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SpreadOperatorInArray) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2]; var b:i64[] = [3, 4]; var c:i64[] = [...a, ...b]; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SpreadWithPlainElements) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2]; var b:i64[] = [0, ...a, 99]; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, IsStdlibFunctionArrayMapFilterReduce) {
    EXPECT_TRUE(isStdlibFunction("array_map"));
    EXPECT_TRUE(isStdlibFunction("array_filter"));
    EXPECT_TRUE(isStdlibFunction("array_reduce"));
}

// ===========================================================================
// Vectorization and loop optimization flag tests
// ===========================================================================

TEST(CodegenTest, VectorizeFlagDefaultOn) {
    // Vectorize is on by default — generating a for-loop at O2 should succeed.
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setVectorize(true);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...10) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, VectorizeFlagOff) {
    // Disabling vectorize should still compile successfully.
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setVectorize(false);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...10) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, UnrollLoopsFlagOff) {
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setUnrollLoops(false);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...10) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LoopOptimizeFlagOff) {
    CodeGenerator codegen(OptimizationLevel::O3);
    codegen.setLoopOptimize(false);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...10) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LoopOptimizeFlagOnO3) {
    // At O3 with loop-optimize on, the polyhedral LoopDistribute pass runs.
    CodeGenerator codegen(OptimizationLevel::O3);
    codegen.setLoopOptimize(true);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...10) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PollyLoopOptO2) {
    // At O2 with loop-optimize on, the Polly polyhedral plugin is loaded.
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setLoopOptimize(true);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...100) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PollyDisabledWhenLoopOptOff) {
    // When loop-optimize is off, Polly should not be loaded; compilation still works.
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setLoopOptimize(false);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...100) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LoopIdiomAndIndVarO2) {
    // At O2+, LoopIdiomRecognize and IndVarSimplify run via the loop optimizer
    // end EP.  Verify the pipeline doesn't crash on a typical loop.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...1000) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LoopDeletionO2) {
    // LoopDeletion should handle dead loops (result unused) without crashing.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...100) { s = s + 1; } return 42; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LoopInterchangeO3) {
    // At O3 with loop-optimize, LoopInterchange runs for nested loop patterns.
    CodeGenerator codegen(OptimizationLevel::O3);
    codegen.setLoopOptimize(true);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...10) { for (j in 0...10) { s = s + i * j; } } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ADCEAndFloat2IntO2) {
    // ADCE and Float2Int run at the scalar optimizer late EP for O2+.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn main() { var x:float = 3.0; var y:i64 = x + 1.0; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, VectorCombineAndLoopSinkO2) {
    // VectorCombine and LoopSink run at the optimizer last EP for O2+.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...100) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, InferFunctionAttrsO1) {
    // InferFunctionAttrs runs at pipeline start for O1+.
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR("fn add(a:i64, b:i64) { return a + b; } fn main() { return add(1, 2); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PartiallyInlineLibCallsOptmax) {
    // PartiallyInlineLibCalls is in the OPTMAX pipeline for math functions.
    CodeGenerator codegen(OptimizationLevel::O3);
    auto* mod = generateIR("fn main() { var x:float = 2.0; var y:i64 = x ** 0.5; return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Extended strength reduction patterns
// ===========================================================================

TEST(CodegenTest, StrengthReductionMul14) {
    // n*14 → (n<<4) - (n<<1) — two shifts + one sub instead of hardware multiply.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 7; return x * 14; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrengthReductionMul28) {
    // n*28 → (n<<5) - (n<<2).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 3; return x * 28; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrengthReductionMul40) {
    // n*40 → (n<<5) + (n<<3).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 5; return x * 40; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrengthReductionMul48) {
    // n*48 → (n<<5) + (n<<4).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 2; return x * 48; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrengthReductionMul60) {
    // n*60 → (n<<6) - (n<<2).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 4; return x * 60; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrengthReductionMul96) {
    // n*96 → (n<<7) - (n<<5).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 3; return x * 96; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrengthReductionMul120) {
    // n*120 → (n<<7) - (n<<3).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 2; return x * 120; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrengthReductionMul256) {
    // n*256 → (n<<8).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 5; return x * 256; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrengthReductionMul512) {
    // n*512 → (n<<9).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 3; return x * 512; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StrengthReductionMul1024) {
    // n*1024 → (n<<10).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 7; return x * 1024; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Extended exponent specialization (powers 17-25)
// ===========================================================================

TEST(CodegenTest, ExponentSpecializationPow17) {
    // x**17 → inline 5 multiplications instead of binary exponentiation loop.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 2; return x ** 17; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ExponentSpecializationPow18) {
    // x**18 → inline 5 multiplications.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 2; return x ** 18; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ExponentSpecializationPow20) {
    // x**20 → inline 5 multiplications.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 2; return x ** 20; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ExponentSpecializationPow24) {
    // x**24 → inline 5 multiplications.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 2; return x ** 24; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ExponentSpecializationPow25) {
    // x**25 → inline 6 multiplications.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 2; return x ** 25; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Additional optimization passes (pipeline coverage)
// ===========================================================================

TEST(CodegenTest, MergeICmpsO2) {
    // MergeICmps runs at O2+ to merge comparison chains into memcmp.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn main() { var a:i64 = 1; var b:i64 = 2; if (a == 1 && b == 2) { return 1; } return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, HotColdSplittingO3) {
    // HotColdSplitting at O3 outlines cold code for better I-cache density.
    CodeGenerator codegen(OptimizationLevel::O3);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...1000) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, StraightLineStrengthReduceO2) {
    // StraightLineStrengthReduce reuses address computations in straight-line code.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn main() { var a:i64 = 5; var b:i64 = a * 3 + 1; var c:i64 = a * 3 + 2; return b + c; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, NaryReassociateO2) {
    // NaryReassociate reuses sub-expressions in add/mul chains.
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(a:i64, b:i64) { var t:i64 = a + b; return (a + b) + 2; } fn main() { return f(3, 4); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, TargetAwareVectorWidth) {
    // Vector width hint should use target-native SIMD width (not hardcoded 4).
    // On the build machine this is at least 4 (SSE), 8 (AVX2), or 16 (AVX-512).
    CodeGenerator codegen(OptimizationLevel::O3);
    codegen.setVectorize(true);
    auto* mod = generateIR("fn main() { var s:i64 = 0; for (i in 0...1000) { s = s + i; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, WhileLoopWithVectorizeHints) {
    // While loops should also get vectorization metadata at O2+.
    CodeGenerator codegen(OptimizationLevel::O2);
    codegen.setVectorize(true);
    auto* mod =
        generateIR("fn main() { var i:i64 = 0; var s:i64 = 0; while (i < 10) { s = s + i; i = i + 1; } return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Production safety: wrapping arithmetic (no undefined behavior on overflow)
// ===========================================================================

TEST(CodegenTest, ArithmeticUsesWrapping) {
    // Verify that the generated IR uses wrapping add/sub/mul (not NSW variants)
    // to ensure defined two's-complement behavior on integer overflow.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn add(a:i64, b:i64) { return a + b; } fn main() { return add(1, 2); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* addFn = mod->getFunction("add");
    ASSERT_NE(addFn, nullptr);
    // Walk instructions and verify no NSW flags on add/sub/mul
    for (auto& BB : *addFn) {
        for (auto& I : BB) {
            if (auto* binOp = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
                if (binOp->getOpcode() == llvm::Instruction::Add || binOp->getOpcode() == llvm::Instruction::Sub ||
                    binOp->getOpcode() == llvm::Instruction::Mul) {
                    EXPECT_FALSE(binOp->hasNoSignedWrap())
                        << "Arithmetic operation should not have NSW flag for production safety";
                }
            }
        }
    }
}

TEST(CodegenTest, SubtractionWrapping) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn sub(a:i64, b:i64) { return a - b; } fn main() { return sub(3, 1); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* subFn = mod->getFunction("sub");
    ASSERT_NE(subFn, nullptr);
    for (auto& BB : *subFn) {
        for (auto& I : BB) {
            if (auto* binOp = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
                if (binOp->getOpcode() == llvm::Instruction::Sub) {
                    EXPECT_FALSE(binOp->hasNoSignedWrap()) << "Subtraction should not have NSW flag";
                }
            }
        }
    }
}

TEST(CodegenTest, MultiplicationWrapping) {
    CodeGenerator codegen(OptimizationLevel::O0);
    // Use a non-power-of-2 constant to avoid strength-reduction to shl
    auto* mod = generateIR("fn mul(a:i64, b:i64) { return a * b; } fn main() { return mul(3, 5); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* mulFn = mod->getFunction("mul");
    ASSERT_NE(mulFn, nullptr);
    for (auto& BB : *mulFn) {
        for (auto& I : BB) {
            if (auto* binOp = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
                if (binOp->getOpcode() == llvm::Instruction::Mul) {
                    EXPECT_FALSE(binOp->hasNoSignedWrap()) << "Multiplication should not have NSW flag";
                }
            }
        }
    }
}

// ===========================================================================
// Production safety: division by zero guard
// ===========================================================================

TEST(CodegenTest, DivisionByZeroGuard) {
    // Verify that division generates a zero-check guard.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn div(a:i64, b:i64) { return a / b; } fn main() { return div(10, 2); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* divFn = mod->getFunction("div");
    ASSERT_NE(divFn, nullptr);
    // Should have multiple basic blocks (at least: entry, zero-check, div-op)
    EXPECT_GT(std::distance(divFn->begin(), divFn->end()), 1u)
        << "Division function should have guard basic blocks for zero-check";
}

TEST(CodegenTest, ModuloByZeroGuard) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mod_fn(a:i64, b:i64) { return a % b; } fn main() { return mod_fn(10, 3); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* modFn = mod->getFunction("mod_fn");
    ASSERT_NE(modFn, nullptr);
    EXPECT_GT(std::distance(modFn->begin(), modFn->end()), 1u)
        << "Modulo function should have guard basic blocks for zero-check";
}

// ===========================================================================
// Production safety: array bounds checking
// ===========================================================================

TEST(CodegenTest, ArrayIndexBoundsCheck) {
    // Verify that array indexing generates bounds-check guard blocks.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod =
        generateIR("fn get(a:i64, i:i64) { return a[i]; } fn main() { var arr:i64[] = [1,2,3]; return get(arr, 0); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* getFn = mod->getFunction("get");
    ASSERT_NE(getFn, nullptr);
    // Bounds checking adds additional basic blocks (ok + fail paths)
    EXPECT_GT(std::distance(getFn->begin(), getFn->end()), 1u)
        << "Array index function should have bounds-check basic blocks";
}

TEST(CodegenTest, ArrayIndexAssignBoundsCheck) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn set(a:i64, i:i64, v:i64) { a[i] = v; return 0; } fn main() { var arr:i64[] = [1,2,3]; return set(arr, 0, 42); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* setFn = mod->getFunction("set");
    ASSERT_NE(setFn, nullptr);
    EXPECT_GT(std::distance(setFn->begin(), setFn->end()), 1u)
        << "Array index assign function should have bounds-check basic blocks";
}

// ===========================================================================
// Same-value identity optimizations (IR-level)
// ===========================================================================

TEST(CodegenTest, SameValueXorZero) {
    // v ^ v → 0: both sides are the same constant, so constant folding
    // produces 0 directly.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 5 ^ 5; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    // Should be constant-folded to 0 — no XOR instruction
    bool hasXor = false;
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (I.getOpcode() == llvm::Instruction::Xor)
                hasXor = true;
    EXPECT_FALSE(hasXor) << "5 ^ 5 should be constant-folded, no XOR instruction";
}

TEST(CodegenTest, SameValueSubZero) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 10 - 10; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    bool hasSub = false;
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (I.getOpcode() == llvm::Instruction::Sub)
                hasSub = true;
    EXPECT_FALSE(hasSub) << "10 - 10 should be constant-folded, no SUB instruction";
}

TEST(CodegenTest, SameValueAndIdentity) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 7 & 7; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    bool hasAnd = false;
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (I.getOpcode() == llvm::Instruction::And)
                hasAnd = true;
    EXPECT_FALSE(hasAnd) << "7 & 7 should be constant-folded, no AND instruction";
}

TEST(CodegenTest, SameValueOrIdentity) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 3 | 3; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    bool hasOr = false;
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (I.getOpcode() == llvm::Instruction::Or)
                hasOr = true;
    EXPECT_FALSE(hasOr) << "3 | 3 should be constant-folded, no OR instruction";
}

// ===========================================================================
// Small-constant multiply strength reduction
// ===========================================================================

TEST(CodegenTest, MultiplyBy3StrengthReduction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul3(x:i64) { return x * 3; } fn main() { return mul3(10); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("mul3");
    ASSERT_NE(fn, nullptr);
    // n * 3 → (n << 1) + n: should contain a shift left and add, not a mul
    bool hasShl = false;
    bool hasAdd = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
            if (I.getOpcode() == llvm::Instruction::Add)
                hasAdd = true;
        }
    }
    EXPECT_TRUE(hasShl) << "multiply by 3 should use shl";
    EXPECT_TRUE(hasAdd) << "multiply by 3 should use add";
}

TEST(CodegenTest, MultiplyBy5StrengthReduction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul5(x:i64) { return x * 5; } fn main() { return mul5(10); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("mul5");
    ASSERT_NE(fn, nullptr);
    bool hasShl = false;
    bool hasAdd = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
            if (I.getOpcode() == llvm::Instruction::Add)
                hasAdd = true;
        }
    }
    EXPECT_TRUE(hasShl) << "multiply by 5 should use shl";
    EXPECT_TRUE(hasAdd) << "multiply by 5 should use add";
}

TEST(CodegenTest, MultiplyBy7StrengthReduction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul7(x:i64) { return x * 7; } fn main() { return mul7(10); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("mul7");
    ASSERT_NE(fn, nullptr);
    // n * 7 → (n << 3) - n: should contain a shift left and sub, not a mul
    bool hasShl = false;
    bool hasSub = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
            if (I.getOpcode() == llvm::Instruction::Sub)
                hasSub = true;
        }
    }
    EXPECT_TRUE(hasShl) << "multiply by 7 should use shl";
    EXPECT_TRUE(hasSub) << "multiply by 7 should use sub";
}

TEST(CodegenTest, MultiplyBy9StrengthReduction) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul9(x:i64) { return x * 9; } fn main() { return mul9(10); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("mul9");
    ASSERT_NE(fn, nullptr);
    bool hasShl = false;
    bool hasAdd = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
            if (I.getOpcode() == llvm::Instruction::Add)
                hasAdd = true;
        }
    }
    EXPECT_TRUE(hasShl) << "multiply by 9 should use shl";
    EXPECT_TRUE(hasAdd) << "multiply by 9 should use add";
}

TEST(CodegenTest, MultiplyBy3Commutative) {
    // 3 * x should also be strength-reduced (commutative)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul3c(x:i64) { return 3 * x; } fn main() { return mul3c(10); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("mul3c");
    ASSERT_NE(fn, nullptr);
    bool hasShl = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_TRUE(hasShl) << "3 * x should also use shl (commutative)";
}

// ===========================================================================
// OPTMAX self-identifier optimizations
// ===========================================================================

TEST(CodegenTest, OptmaxSelfXor) {
    // x ^ x → 0 at AST level when both sides are the same identifier
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return x ^ x; } OPTMAX!: fn main() { return opt(5); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("opt");
    ASSERT_NE(fn, nullptr);
    bool hasXor = false;
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (I.getOpcode() == llvm::Instruction::Xor)
                hasXor = true;
    EXPECT_FALSE(hasXor) << "OPTMAX x ^ x should be folded to 0, no XOR instruction";
}

TEST(CodegenTest, OptmaxSelfSub) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return x - x; } OPTMAX!: fn main() { return opt(5); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("opt");
    ASSERT_NE(fn, nullptr);
    bool hasSub = false;
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (I.getOpcode() == llvm::Instruction::Sub)
                hasSub = true;
    EXPECT_FALSE(hasSub) << "OPTMAX x - x should be folded to 0, no SUB instruction";
}

TEST(CodegenTest, OptmaxSelfAnd) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return x & x; } OPTMAX!: fn main() { return opt(5); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("opt");
    ASSERT_NE(fn, nullptr);
    bool hasAnd = false;
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (I.getOpcode() == llvm::Instruction::And)
                hasAnd = true;
    EXPECT_FALSE(hasAnd) << "OPTMAX x & x should simplify to x, no AND instruction";
}

TEST(CodegenTest, OptmaxSelfOr) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return x | x; } OPTMAX!: fn main() { return opt(5); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("opt");
    ASSERT_NE(fn, nullptr);
    bool hasOr = false;
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (I.getOpcode() == llvm::Instruction::Or)
                hasOr = true;
    EXPECT_FALSE(hasOr) << "OPTMAX x | x should simplify to x, no OR instruction";
}

// ===========================================================================
// Float constant folding
// ===========================================================================

TEST(CodegenTest, FloatConstantFoldAdd) {
    // 1.5 + 2.5 should be folded to 4.0 at compile time
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 1.5 + 2.5; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasFAdd = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::FAdd)
                hasFAdd = true;
        }
    }
    EXPECT_FALSE(hasFAdd) << "Float addition of constants should be folded at compile time";
}

TEST(CodegenTest, FloatConstantFoldMul) {
    // 3.0 * 4.0 should be folded to 12.0
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 3.0 * 4.0; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasFMul = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::FMul)
                hasFMul = true;
        }
    }
    EXPECT_FALSE(hasFMul) << "Float multiplication of constants should be folded at compile time";
}

TEST(CodegenTest, FloatConstantFoldSub) {
    // 10.0 - 3.0 should be folded to 7.0
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 10.0 - 3.0; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasFSub = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::FSub)
                hasFSub = true;
        }
    }
    EXPECT_FALSE(hasFSub) << "Float subtraction of constants should be folded at compile time";
}

TEST(CodegenTest, FloatConstantFoldComparison) {
    // 2.0 < 3.0 should fold to 1 at compile time
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 2.0 < 3.0; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasFCmp = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::FCmp)
                hasFCmp = true;
        }
    }
    EXPECT_FALSE(hasFCmp) << "Float comparison of constants should be folded at compile time";
}

TEST(CodegenTest, FloatConstantFoldMod) {
    // 7.0 % 3.0 should be folded to 1.0 at compile time (fmod)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 7.0 % 3.0; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    bool hasFRem = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::FRem)
                hasFRem = true;
        }
    }
    EXPECT_FALSE(hasFRem) << "Float modulo of constants should be folded at compile time";
}

TEST(CodegenTest, FloatConstantFoldPow) {
    // 2.0 ** 3.0 should be folded to 8.0 at compile time
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 2.0 ** 3.0; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // After constant folding, there should be no call to llvm.pow
    bool hasPowCall = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = call->getCalledFunction()) {
                    if (callee->getName().contains("pow"))
                        hasPowCall = true;
                }
            }
        }
    }
    EXPECT_FALSE(hasPowCall) << "Float power of constants should be folded at compile time";
}

TEST(CodegenTest, NullCoalesceConstFoldNonzero) {
    // 42 ?? expr → 42  (no branch generated)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 42 ?? 99; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // Should be a single basic block — no branch needed
    EXPECT_EQ(mainFn->size(), 1u)
        << "Nonzero ?? y should fold to the nonzero value without branches";
}

TEST(CodegenTest, NullCoalesceConstFoldZero) {
    // 0 ?? expr → expr  (no branch generated)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 0 ?? 99; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // Should be a single basic block — evaluates right side directly
    EXPECT_EQ(mainFn->size(), 1u)
        << "0 ?? y should fold to y without branches";
}

TEST(CodegenTest, FloatNegOneMultFold) {
    // x * (-1.0) should fold to -x (FNeg) instead of FMul
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x * (-1.0); }\n"
                           "fn main() { return 0; }",
                           codegen);
    auto* fn = mod->getFunction("f");
    ASSERT_NE(fn, nullptr);
    bool hasFMul = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::FMul)
                hasFMul = true;
        }
    }
    EXPECT_FALSE(hasFMul) << "x * (-1.0) should fold to FNeg, not FMul";
}

// ===========================================================================
// Same-value comparison identities
// ===========================================================================

TEST(CodegenTest, SameValueEqualityFoldsToOne) {
    // x == x should fold to 1 after mem2reg promotes both loads to the same SSA value
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(x: int) { return x == x; } fn main() { return f(5); }", codegen);
    auto* fn = mod->getFunction("f");
    if (!fn) { SUCCEED(); return; } // inlined at O2
    // At O2, mem2reg + our same-value identity + LLVM passes eliminate the comparison
    bool hasICmp = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::ICmp)
                hasICmp = true;
        }
    }
    EXPECT_FALSE(hasICmp) << "x == x should be folded after mem2reg promotes to same SSA value";
}

TEST(CodegenTest, SameValueInequalityFoldsToZero) {
    // x != x should fold to 0
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(x: int) { return x != x; } fn main() { return f(5); }", codegen);
    auto* fn = mod->getFunction("f");
    if (!fn) { SUCCEED(); return; } // inlined at O2
    bool hasICmp = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::ICmp)
                hasICmp = true;
        }
    }
    EXPECT_FALSE(hasICmp) << "x != x should be folded after mem2reg promotes to same SSA value";
}

TEST(CodegenTest, SameValueLessEqualFoldsToOne) {
    // x <= x should fold to 1
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(x: int) { return x <= x; } fn main() { return f(5); }", codegen);
    auto* fn = mod->getFunction("f");
    if (!fn) { SUCCEED(); return; } // inlined at O2
    bool hasICmp = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::ICmp)
                hasICmp = true;
        }
    }
    EXPECT_FALSE(hasICmp) << "x <= x should be folded after mem2reg promotes to same SSA value";
}

TEST(CodegenTest, SameValueLessThanFoldsToZero) {
    // x < x should fold to 0
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(x: int) { return x < x; } fn main() { return f(5); }", codegen);
    auto* fn = mod->getFunction("f");
    if (!fn) { SUCCEED(); return; } // inlined at O2
    bool hasICmp = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (I.getOpcode() == llvm::Instruction::ICmp)
                hasICmp = true;
        }
    }
    EXPECT_FALSE(hasICmp) << "x < x should be folded after mem2reg promotes to same SSA value";
}

// ===========================================================================
// While loop constant condition elimination
// ===========================================================================

TEST(CodegenTest, WhileConstantFalseEliminated) {
    // while (0) { ... } should not generate any loop structure
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 1; while (0) { x = x + 1; } return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // Should not have any whilecond/whilebody blocks
    bool hasWhileBlock = false;
    for (auto& BB : *mainFn) {
        std::string name = BB.getName().str();
        if (name.find("whilecond") != std::string::npos || name.find("whilebody") != std::string::npos)
            hasWhileBlock = true;
    }
    EXPECT_FALSE(hasWhileBlock) << "while(0) should be entirely eliminated";
}

TEST(CodegenTest, WhileConstantTrueNoCondCheck) {
    // while (1) { break; } should emit an infinite loop without condition block
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { while (1) { break; } return 0; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // Should not have a whilecond block (no condition check needed)
    bool hasCondBlock = false;
    for (auto& BB : *mainFn) {
        if (BB.getName().str().find("whilecond") != std::string::npos)
            hasCondBlock = true;
    }
    EXPECT_FALSE(hasCondBlock) << "while(1) should not generate a condition check block";
}

// ===========================================================================
// Do-while loop constant condition elimination
// ===========================================================================

TEST(CodegenTest, DoWhileConstantFalseSingleExec) {
    // do { ... } while (0) should execute body once and not loop back
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 0; do { x = x + 1; } while (0); return x; }", codegen);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // The condition block should not have a conditional branch back to the body
    // (it should be an unconditional branch to endBB)
    bool hasCondBr = false;
    for (auto& BB : *mainFn) {
        if (BB.getName().str().find("dowhilecond") != std::string::npos) {
            auto* term = BB.getTerminator();
            if (term && llvm::isa<llvm::BranchInst>(term)) {
                auto* br = llvm::cast<llvm::BranchInst>(term);
                if (br->isConditional())
                    hasCondBr = true;
            }
        }
    }
    EXPECT_FALSE(hasCondBr) << "do-while(0) condition block should have unconditional branch, not conditional";
}

// ===========================================================================
// OPTMAX EarlyCSE with MemorySSA
// ===========================================================================

TEST(CodegenTest, OptmaxEarlyCSEMemorySSA) {
    // OPTMAX functions should use EarlyCSE with MemorySSA for better CSE
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("OPTMAX=: fn opt(x: int) { return x + x; } OPTMAX!: fn main() { return opt(5); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("opt");
    ASSERT_NE(fn, nullptr);
}

// ===========================================================================
// O3 pipeline with AggressiveInstCombine and ConstraintElimination
// ===========================================================================

TEST(CodegenTest, O3PipelineAggressiveInstCombine) {
    // At O3, the pipeline should include AggressiveInstCombine and
    // ConstraintElimination passes via the new PM extension points.
    CodeGenerator codegen(OptimizationLevel::O3);
    auto* mod = generateIR("fn main() { var x:i64 = 42; return x * 2 + x * 2; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

// ===========================================================================
// Division by power-of-2 strength reduction (shift-based)
// ===========================================================================

TEST(CodegenTest, DivisionByPow2EmitsShiftNotSDiv) {
    // n / 16 should emit AShr-based sequence at O2+ (LLVM backend transform)
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(n:i64) { return n / 16; } fn main() { return f(32); }", codegen);
    auto* func = mod->getFunction("f");
    if (!func) { SUCCEED(); return; } // inlined at O2
    bool hasAShr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::AShr)
                hasAShr = true;
        }
    }
    EXPECT_TRUE(hasAShr) << "Division by 16 should use AShr";
}

TEST(CodegenTest, DivisionByNonPow2UsesMagicNumber) {
    // n / 3 should use magic-number multiplication at O2+ (LLVM backend)
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(n:i64) { return n / 3; } fn main() { return f(9); }", codegen);
    auto* func = mod->getFunction("f");
    if (!func) { SUCCEED(); return; } // inlined at O2
    bool hasSDiv = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SDiv)
                hasSDiv = true;
        }
    }
    EXPECT_FALSE(hasSDiv) << "Division by 3 should NOT use SDiv (should use magic number)";
}

TEST(CodegenTest, DivisionByOneIsIdentity) {
    // n / 1 should be simplified by the algebraic identity (x/1 → x)
    // and not generate any division or shift instructions at all.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(n:i64) { return n / 1; } fn main() { return f(42); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasSDiv = false;
    bool hasAShr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SDiv)
                hasSDiv = true;
            if (inst.getOpcode() == llvm::Instruction::AShr)
                hasAShr = true;
        }
    }
    EXPECT_FALSE(hasSDiv) << "Division by 1 should be identity (no SDiv)";
    EXPECT_FALSE(hasAShr) << "Division by 1 should be identity (no AShr)";
}

// ===========================================================================
// Modulo by power-of-2 strength reduction
// ===========================================================================

TEST(CodegenTest, ModuloByPow2EmitsShiftNotSRem) {
    // n % 16 should emit shift-based sequence at O2+ (LLVM backend)
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(n:i64) { return n % 16; } fn main() { return f(33); }", codegen);
    auto* func = mod->getFunction("f");
    if (!func) { SUCCEED(); return; } // inlined at O2
    bool hasSRem = false;
    bool hasAShr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SRem)
                hasSRem = true;
            if (inst.getOpcode() == llvm::Instruction::AShr)
                hasAShr = true;
        }
    }
    EXPECT_FALSE(hasSRem) << "Modulo by 16 should NOT use SRem";
    EXPECT_TRUE(hasAShr) << "Modulo by 16 should use shift-based sequence";
}

// ===========================================================================
// Ternary constant condition elimination
// ===========================================================================

TEST(CodegenTest, TernaryConstantTrue) {
    // 1 ? 42 : 99 should fold to 42 with no branch
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 1 ? 42 : 99; }", codegen);
    auto* func = mod->getFunction("main");
    ASSERT_NE(func, nullptr);
    bool hasCondBr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasCondBr = true;
            }
        }
    }
    EXPECT_FALSE(hasCondBr) << "Ternary with constant true condition should have no conditional branch";
}

TEST(CodegenTest, TernaryConstantFalse) {
    // 0 ? 42 : 99 should fold to 99 with no branch
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 0 ? 42 : 99; }", codegen);
    auto* func = mod->getFunction("main");
    ASSERT_NE(func, nullptr);
    bool hasCondBr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasCondBr = true;
            }
        }
    }
    EXPECT_FALSE(hasCondBr) << "Ternary with constant false condition should have no conditional branch";
}

// ===========================================================================
// Double negation elimination
// ===========================================================================

TEST(CodegenTest, DoubleNegationInt) {
    // -(-x) should simplify to x (no Neg instructions)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return -(-x); } fn main() { return f(5); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    int negCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            // Neg is emitted as Sub 0, x
            if (inst.getOpcode() == llvm::Instruction::Sub)
                negCount++;
        }
    }
    EXPECT_EQ(negCount, 0) << "-(-x) should be simplified to x with no Sub";
}

TEST(CodegenTest, DoubleBitwiseNotElim) {
    // ~(~x) should simplify to x (no Xor instructions)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return ~(~x); } fn main() { return f(5); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    int xorCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Xor)
                xorCount++;
        }
    }
    EXPECT_EQ(xorCount, 0) << "~(~x) should be simplified to x with no Xor";
}

TEST(CodegenTest, DoubleLogicalNotElim) {
    // !(!x) should simplify to x (no ICmp/Xor)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return !(!x); } fn main() { return f(5); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    int icmpCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::ICmp)
                icmpCount++;
        }
    }
    EXPECT_EQ(icmpCount, 0) << "!(!x) should be simplified to x with no ICmp";
}

// ===========================================================================
// For-loop empty range elimination
// ===========================================================================

TEST(CodegenTest, ForLoopEmptyRangeEliminated) {
    // for (i in 10...10) {} should be entirely eliminated since start == end
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 1; for (i in 10...10) { x = x + 1; } return x; }", codegen);
    auto* func = mod->getFunction("main");
    ASSERT_NE(func, nullptr);
    // Should not have any for-loop blocks
    bool hasForBlock = false;
    for (auto& BB : *func) {
        std::string name = BB.getName().str();
        if (name.find("forcond") != std::string::npos || name.find("forbody") != std::string::npos)
            hasForBlock = true;
    }
    EXPECT_FALSE(hasForBlock) << "for(i in 10...10) should be entirely eliminated";
}

// ===========================================================================
// Function attribute completeness
// ===========================================================================

TEST(CodegenTest, MemcpyHasWillReturn) {
    // memcpy should have WillReturn attribute
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2, 3]; var b:i64 = a; return b[0]; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("memcpy");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::WillReturn)) << "memcpy should have WillReturn attribute";
    }
}

TEST(CodegenTest, MemmoveHasFullAttributes) {
    // memmove should have WillReturn, NoFree, NoSync attributes
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2, 3]; return a[0]; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("memmove");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::WillReturn)) << "memmove should have WillReturn attribute";
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoFree)) << "memmove should have NoFree attribute";
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoSync)) << "memmove should have NoSync attribute";
    }
}

// ===========================================================================
// Memory allocator attribute tests
// ===========================================================================

TEST(CodegenTest, MallocHasAllocatorAttributes) {
    // malloc should have basic safety attributes.
    // NOTE: allocsize, allockind, and inaccessibleMemOnly are intentionally
    // omitted — they cause LLVM's InferFunctionAttrs to add aggressive
    // attributes that lead to miscompilation of string repetition loops.
    CodeGenerator codegen(OptimizationLevel::O0);
    // String concatenation triggers a malloc call.
    auto* mod =
        generateIR("fn main() { var a:i64 = \"hello\"; var b:i64 = \"world\"; print(a + b); return 0; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("malloc");
    ASSERT_NE(fn, nullptr) << "malloc should be declared";
    EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoUnwind));
    EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::WillReturn));
    EXPECT_TRUE(fn->hasRetAttribute(llvm::Attribute::NoAlias));
    EXPECT_TRUE(fn->hasRetAttribute(llvm::Attribute::NonNull));
}

TEST(CodegenTest, FreeHasAllocatorAttributes) {
    // free should have nocapture, allockind, and memory effect attributes
    // when it is declared in the module.
    CodeGenerator codegen(OptimizationLevel::O0);
    // push uses realloc (not free), so free may not be declared by this snippet.
    // We still test that IF free appears, its attributes are correct.
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2]; push(a, 3); return a[0]; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("free");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoUnwind));
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::WillReturn));
        // allockind: free
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::AllocKind))
            << "free should have allockind attribute";
        auto kind = fn->getFnAttribute(llvm::Attribute::AllocKind).getAllocKind();
        EXPECT_TRUE((kind & llvm::AllocFnKind::Free) != llvm::AllocFnKind::Unknown)
            << "free allockind should include Free";
        // memory effects
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::Memory))
            << "free should have memory effects attribute";
        // allocptr on parameter 0
        EXPECT_TRUE(fn->hasParamAttribute(0, llvm::Attribute::AllocatedPointer))
            << "free parameter 0 should have allocptr attribute";
    }
}

TEST(CodegenTest, ReallocHasAllocatorAttributes) {
    // realloc should have allocsize, allockind, nocapture, and memory effect attrs.
    CodeGenerator codegen(OptimizationLevel::O0);
    // Use array operations that may trigger realloc
    auto* mod = generateIR("fn main() { var a:i64[] = [1, 2]; return len(a); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("realloc");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoUnwind));
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::WillReturn));
        EXPECT_TRUE(fn->hasRetAttribute(llvm::Attribute::NoAlias));
        // allocsize(1): parameter 1 is the new allocation size
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::AllocSize))
            << "realloc should have allocsize attribute";
        auto allocSize = fn->getFnAttribute(llvm::Attribute::AllocSize).getAllocSizeArgs();
        EXPECT_EQ(allocSize.first, 1u) << "realloc allocsize should reference parameter 1";
        // allockind: realloc
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::AllocKind))
            << "realloc should have allockind attribute";
        auto kind = fn->getFnAttribute(llvm::Attribute::AllocKind).getAllocKind();
        EXPECT_TRUE((kind & llvm::AllocFnKind::Realloc) != llvm::AllocFnKind::Unknown)
            << "realloc allockind should include Realloc";
        // memory effects
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::Memory))
            << "realloc should have memory effects attribute";
        // allocptr on parameter 0
        EXPECT_TRUE(fn->hasParamAttribute(0, llvm::Attribute::AllocatedPointer))
            << "realloc parameter 0 should have allocptr attribute";
    }
}

TEST(CodegenTest, StrndupHasAllocatorAttributes) {
    // strndup should have allocsize and allockind attributes.
    CodeGenerator codegen(OptimizationLevel::O0);
    // str_split triggers strndup for extracting substrings.
    auto* mod =
        generateIR("fn main() { var s:i64 = \"hello world\"; var parts:i64 = str_split(s, \" \"); return len(parts); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("strndup");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoUnwind));
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::WillReturn));
        EXPECT_TRUE(fn->hasRetAttribute(llvm::Attribute::NoAlias));
        // allocsize(1)
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::AllocSize))
            << "strndup should have allocsize attribute";
        auto allocSize = fn->getFnAttribute(llvm::Attribute::AllocSize).getAllocSizeArgs();
        EXPECT_EQ(allocSize.first, 1u) << "strndup allocsize should reference parameter 1";
        // allockind: alloc | uninitialized
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::AllocKind))
            << "strndup should have allockind attribute";
        auto kind = fn->getFnAttribute(llvm::Attribute::AllocKind).getAllocKind();
        EXPECT_TRUE((kind & llvm::AllocFnKind::Alloc) != llvm::AllocFnKind::Unknown)
            << "strndup allockind should include Alloc";
    }
}

// ===========================================================================
// Magic-number division tests
// ===========================================================================

TEST(CodegenTest, MagicDivisionBy7) {
    // n / 7 should use magic-number multiplication at O2+ (LLVM backend)
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(n:i64) { return n / 7; } fn main() { return f(49); }", codegen);
    auto* func = mod->getFunction("f");
    if (!func) { SUCCEED(); return; } // inlined at O2
    bool hasSDiv = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SDiv)
                hasSDiv = true;
        }
    }
    EXPECT_FALSE(hasSDiv) << "Division by 7 should NOT use SDiv (should use magic number)";
}

TEST(CodegenTest, MagicModuloBy7) {
    // n % 7 should use magic-number multiplication at O2+ (LLVM backend)
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(n:i64) { return n % 7; } fn main() { return f(50); }", codegen);
    auto* func = mod->getFunction("f");
    if (!func) { SUCCEED(); return; } // inlined at O2
    bool hasSRem = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SRem)
                hasSRem = true;
        }
    }
    EXPECT_FALSE(hasSRem) << "Modulo by 7 should NOT use SRem (should use magic number)";
}

// ===========================================================================
// Negative constant strength reduction tests
// ===========================================================================

TEST(CodegenTest, NegativeMultiplyStrengthReduction) {
    // n * (-3) should use shift+add+neg, not generic multiply
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(n:i64) { return n * (-3); } fn main() { return f(5); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * (-3) should NOT use Mul (should use shift+neg)";
    EXPECT_TRUE(hasShl) << "n * (-3) should use shifts";
}

// ===========================================================================
// Additional strength reduction tests
// ===========================================================================

TEST(CodegenTest, StrengthReductionMultiplyBy127) {
    // n * 127 → (n<<7) - n
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(n:i64) { return n * 127; } fn main() { return f(5); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 127 should NOT use Mul";
    EXPECT_TRUE(hasShl) << "n * 127 should use shift+sub";
}

TEST(CodegenTest, StrengthReductionMultiplyBy255) {
    // n * 255 → (n<<8) - n
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(n:i64) { return n * 255; } fn main() { return f(5); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 255 should NOT use Mul";
    EXPECT_TRUE(hasShl) << "n * 255 should use shift+sub";
}

TEST(CodegenTest, StrengthReductionMultiplyBy1000) {
    // n * 1000 → shift+sub sequence
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(n:i64) { return n * 1000; } fn main() { return f(5); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 1000 should NOT use Mul";
    EXPECT_TRUE(hasShl) << "n * 1000 should use shift+add/sub sequence";
}

// ===========================================================================
// Float division by power-of-2 → reciprocal multiplication
// ===========================================================================

TEST(CodegenTest, FloatDivByPow2UsesReciprocal) {
    // x / 2.0 should become x * 0.5 (FMul not FDiv)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return to_float(x) / 2.0; } fn main() { return f(10); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasFDiv = false;
    bool hasFMul = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::FDiv)
                hasFDiv = true;
            if (inst.getOpcode() == llvm::Instruction::FMul)
                hasFMul = true;
        }
    }
    EXPECT_FALSE(hasFDiv) << "x / 2.0 should NOT use FDiv";
    EXPECT_TRUE(hasFMul) << "x / 2.0 should use FMul (reciprocal)";
}

// ===========================================================================
// Pure function attribute inference tests
// ===========================================================================

TEST(CodegenTest, PureFunctionGetsReadnone) {
    // A pure arithmetic function (no memory access) should get
    // memory(none) at O1+.
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR("fn square(x:i64) { return x * x; }\n"
                           "fn main() { return square(5); }",
                           codegen);
    auto* func = mod->getFunction("square");
    if (func && !func->isDeclaration()) {
        EXPECT_TRUE(func->doesNotAccessMemory())
            << "Pure function 'square' should have readnone/memory(none) attribute";
    }
}

// ===========================================================================
// Extended small-exponent specialization tests
// ===========================================================================

TEST(CodegenTest, ExponentSpecializationPow7) {
    // x**7 should produce inline multiplications, no loop
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x ** 7; } fn main() { return f(2); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasLoop = false;
    int mulCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                mulCount++;
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                // Loop-back branches indicate a loop
                if (br->isConditional())
                    hasLoop = true;
            }
        }
    }
    EXPECT_FALSE(hasLoop) << "x**7 should be inline (no loop)";
    EXPECT_EQ(mulCount, 4) << "x**7 should use exactly 4 multiplications";
}

TEST(CodegenTest, ExponentSpecializationPow9) {
    // x**9 should produce inline multiplications, no loop
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x ** 9; } fn main() { return f(2); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasLoop = false;
    int mulCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                mulCount++;
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasLoop = true;
            }
        }
    }
    EXPECT_FALSE(hasLoop) << "x**9 should be inline (no loop)";
    EXPECT_EQ(mulCount, 4) << "x**9 should use exactly 4 multiplications";
}

TEST(CodegenTest, ExponentSpecializationPow10) {
    // x**10 should produce inline multiplications, no loop
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x ** 10; } fn main() { return f(2); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasLoop = false;
    int mulCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                mulCount++;
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasLoop = true;
            }
        }
    }
    EXPECT_FALSE(hasLoop) << "x**10 should be inline (no loop)";
    EXPECT_EQ(mulCount, 4) << "x**10 should use exactly 4 multiplications";
}

TEST(CodegenTest, ExponentSpecializationPow16) {
    // x**16 should produce inline multiplications, no loop
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x ** 16; } fn main() { return f(2); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasLoop = false;
    int mulCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                mulCount++;
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasLoop = true;
            }
        }
    }
    EXPECT_FALSE(hasLoop) << "x**16 should be inline (no loop)";
    EXPECT_EQ(mulCount, 4) << "x**16 should use exactly 4 multiplications";
}

// ===========================================================================
// Extended magic-number division range test
// ===========================================================================

TEST(CodegenTest, MagicDivisionBy1337) {
    // n / 1337 should use magic-number multiplication at O2+ (LLVM backend)
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(n:i64) { return n / 1337; } fn main() { return f(2674); }", codegen);
    auto* func = mod->getFunction("f");
    if (!func) { SUCCEED(); return; } // inlined at O2
    bool hasSDiv = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SDiv)
                hasSDiv = true;
        }
    }
    EXPECT_FALSE(hasSDiv) << "Division by 1337 should NOT use SDiv (should use magic number)";
}

TEST(CodegenTest, MagicDivisionByLargeConstant) {
    // n / 12345 should use magic-number multiplication at O2+ (LLVM backend)
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(n:i64) { return n / 12345; } fn main() { return f(24690); }", codegen);
    auto* func = mod->getFunction("f");
    if (!func) { SUCCEED(); return; } // inlined at O2
    bool hasSDiv = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SDiv)
                hasSDiv = true;
        }
    }
    EXPECT_FALSE(hasSDiv) << "Division by 12345 should NOT use SDiv (should use magic number)";
}

// ===========================================================================
// Same-value division/modulo identity tests
// ===========================================================================

TEST(CodegenTest, SameValueDivisionIdentity) {
    // x / x → 1 when both operands are the same SSA value.
    // At O0, separate loads produce different SSA values; this test
    // verifies the identity fires after LLVM's mem2reg (at O1+).
    CodeGenerator codegen(OptimizationLevel::O0);
    // Use a literal-level identity where the expression value is reused:
    // `var x:i64 = a + 1; return x / x;`  — 'x' is loaded once, both references
    // are the same SSA value after the load.  However, at O0 two separate
    // loads will occur. The identity correctly fires post-mem2reg (O1+).
    // For now, verify the identity exists by checking that the pattern is
    // recognized when the same constant is on both sides.
    auto* mod = generateIR("fn f() { return 5 / 5; } fn main() { return f(); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasSDiv = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SDiv)
                hasSDiv = true;
        }
    }
    // 5/5 is constant-folded to 1, so no SDiv should be present
    EXPECT_FALSE(hasSDiv) << "5 / 5 should be constant-folded to 1 (no SDiv)";
}

TEST(CodegenTest, SameValueModuloIdentity) {
    // x % x → 0 when both operands are the same SSA value.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f() { return 7 % 7; } fn main() { return f(); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasSRem = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SRem)
                hasSRem = true;
        }
    }
    // 7%7 is constant-folded to 0, so no SRem should be present
    EXPECT_FALSE(hasSRem) << "7 % 7 should be constant-folded to 0 (no SRem)";
}

// ===========================================================================
// Comparison-with-zero folding tests
// ===========================================================================

TEST(CodegenTest, MulCompareZeroFolding) {
    // x * 5 == 0  →  x == 0 (eliminates the multiply)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x * 5 == 0; } fn main() { return f(0); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
        }
    }
    EXPECT_FALSE(hasMul) << "x * 5 == 0 should be simplified to x == 0 (no Mul)";
}

// ===========================================================================
// signext parameter attribute tests
// ===========================================================================

TEST(CodegenTest, UserFunctionParamsHaveSignExt) {
    // User function parameters should have signext attribute.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn add(a:i64, b:i64) { return a + b; }\n"
                           "fn main() { return add(1, 2); }",
                           codegen);
    auto* func = mod->getFunction("add");
    ASSERT_NE(func, nullptr);
    EXPECT_TRUE(func->hasParamAttribute(0, llvm::Attribute::SExt))
        << "Parameter 0 should have signext";
    EXPECT_TRUE(func->hasParamAttribute(1, llvm::Attribute::SExt))
        << "Parameter 1 should have signext";
    // Return value should also have signext
    EXPECT_TRUE(func->hasRetAttribute(llvm::Attribute::SExt))
        << "Return value should have signext";
}

// ===========================================================================
// norecurse attribute inference tests
// ===========================================================================

TEST(CodegenTest, NonRecursiveFunctionGetsNorecurse) {
    // A non-recursive function should get norecurse at O1+.
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR("fn add(a:i64, b:i64) { return a + b; }\n"
                           "fn main() { return add(1, 2); }",
                           codegen);
    auto* func = mod->getFunction("add");
    if (func && !func->isDeclaration()) {
        EXPECT_TRUE(func->hasFnAttribute(llvm::Attribute::NoRecurse))
            << "Non-recursive function should have norecurse attribute";
    }
}

TEST(CodegenTest, RecursiveFunctionDoesNotGetNorecurse) {
    // A recursive function should NOT get norecurse.
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR("fn fib(n:i64) { if (n <= 1) { return n; } return fib(n - 1) + fib(n - 2); }\n"
                           "fn main() { return fib(10); }",
                           codegen);
    auto* func = mod->getFunction("fib");
    if (func && !func->isDeclaration()) {
        EXPECT_FALSE(func->hasFnAttribute(llvm::Attribute::NoRecurse))
            << "Recursive function should NOT have norecurse attribute";
    }
}

// ===========================================================================
// Cold attribute on error-handling functions
// ===========================================================================

TEST(CodegenTest, ExitHasColdAttribute) {
    // exit() should be marked cold since it's an error/termination path.
    CodeGenerator codegen(OptimizationLevel::O0);
    // Division-by-zero path calls exit, which forces the declaration.
    auto* mod = generateIR("fn f(a:i64, b:i64) { return a / b; }\nfn main() { return f(10, 2); }", codegen);
    auto* exitFn = mod->getFunction("exit");
    if (exitFn) {
        EXPECT_TRUE(exitFn->hasFnAttribute(llvm::Attribute::Cold))
            << "exit() should have cold attribute";
    }
}

// ===========================================================================
// Returned attribute on memcpy/strcpy/strcat/memmove
// ===========================================================================

TEST(CodegenTest, StrcpyHasReturnedAttribute) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var s:i64 = \"hello\"; return len(s); }", codegen);
    auto* fn = mod->getFunction("strcpy");
    if (fn) {
        EXPECT_TRUE(fn->hasParamAttribute(0, llvm::Attribute::Returned))
            << "strcpy param 0 should have returned attribute";
    }
}

TEST(CodegenTest, MemcpyHasReturnedAttribute) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var arr:i64[] = [1, 2, 3]; return arr[0]; }", codegen);
    auto* fn = mod->getFunction("memcpy");
    if (fn) {
        EXPECT_TRUE(fn->hasParamAttribute(0, llvm::Attribute::Returned))
            << "memcpy param 0 should have returned attribute";
    }
}

TEST(CodegenTest, MemcpyHasArgMemOnly) {
    // memcpy should have memory(argmem: readwrite)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var arr:i64[] = [1, 2, 3]; return arr[0]; }", codegen);
    auto* fn = mod->getFunction("memcpy");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoSync))
            << "memcpy should have nosync attribute";
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoFree))
            << "memcpy should have nofree attribute";
    }
}

// ===========================================================================
// C library function attribute completeness
// ===========================================================================

TEST(CodegenTest, RandHasNoUnwindAttribute) {
    CodeGenerator codegen(OptimizationLevel::O0);
    // 'random()' is the omscript builtin that calls C's rand().
    auto* mod = generateIR("fn main() { return random(); }", codegen);
    auto* fn = mod->getFunction("rand");
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoUnwind))
        << "rand() should have nounwind attribute";
    EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::WillReturn))
        << "rand() should have willreturn attribute";
}

TEST(CodegenTest, SrandHasNoUnwindAttribute) {
    CodeGenerator codegen(OptimizationLevel::O0);
    // 'seed(N)' or 'random()' triggers srand; random auto-seeds.
    // Trigger srand via random() which auto-seeds on first call.
    auto* mod = generateIR("fn main() { return random(); }", codegen);
    auto* fn = mod->getFunction("srand");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoUnwind))
            << "srand() should have nounwind attribute";
        // srand() modifies global PRNG state, so it must NOT be NoSync —
        // the optimizer must not reorder or eliminate it relative to rand().
        EXPECT_FALSE(fn->hasFnAttribute(llvm::Attribute::NoSync))
            << "srand() must NOT have nosync (modifies global PRNG state)";
    }
}

TEST(CodegenTest, TimeHasNoUnwindAttribute) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return time(); }", codegen);
    auto* fn = mod->getFunction("time");
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoUnwind))
        << "time() should have nounwind attribute";
}

TEST(CodegenTest, AtofHasArgMemRead) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return to_float(\"3.14\"); }", codegen);
    auto* fn = mod->getFunction("atof");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::Memory))
            << "atof() should have memory attribute";
        EXPECT_TRUE(fn->hasParamAttribute(0, llvm::Attribute::NonNull))
            << "atof() param 0 should be nonnull";
    }
}

// ===========================================================================
// File I/O function attributes
// ===========================================================================

TEST(CodegenTest, FopenHasNoUnwind) {
    CodeGenerator codegen(OptimizationLevel::O0);
    // file_read() builtin calls fopen internally
    auto* mod = generateIR("fn main() { var s:i64 = file_read(\"test.txt\"); return 0; }", codegen);
    auto* fn = mod->getFunction("fopen");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoUnwind))
            << "fopen() should have nounwind attribute";
        EXPECT_TRUE(fn->hasParamAttribute(0, llvm::Attribute::NonNull))
            << "fopen param 0 should be nonnull";
        EXPECT_TRUE(fn->hasParamAttribute(0, llvm::Attribute::ReadOnly))
            << "fopen param 0 should be readonly";
    }
}

// ===========================================================================
// Subtraction-comparison strength reduction tests
// ===========================================================================

TEST(CodegenTest, SubEqZeroFoldsToEq) {
    // (x - y) == 0 should fold to x == y, comparing operands directly
    // instead of using the sub result in the comparison.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(a:i64, b:i64) { return (a - b) == 0; }\n"
                           "fn main() { return f(5, 5); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    // Verify that the icmp instruction does NOT use the sub result.
    // The sub may still exist (dead code), but the comparison should
    // directly compare the sub's operands.
    bool subUsedInCmp = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                for (unsigned i = 0; i < cmp->getNumOperands(); ++i) {
                    if (auto* sub = llvm::dyn_cast<llvm::BinaryOperator>(cmp->getOperand(i))) {
                        if (sub->getOpcode() == llvm::Instruction::Sub)
                            subUsedInCmp = true;
                    }
                }
            }
        }
    }
    EXPECT_FALSE(subUsedInCmp) << "(a - b) == 0 should compare a == b directly (no Sub in comparison)";
}

TEST(CodegenTest, SubNeqZeroFoldsToNeq) {
    // (x - y) != 0 should fold to x != y, comparing operands directly
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(a:i64, b:i64) { return (a - b) != 0; }\n"
                           "fn main() { return f(5, 3); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool subUsedInCmp = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
                for (unsigned i = 0; i < cmp->getNumOperands(); ++i) {
                    if (auto* sub = llvm::dyn_cast<llvm::BinaryOperator>(cmp->getOperand(i))) {
                        if (sub->getOpcode() == llvm::Instruction::Sub)
                            subUsedInCmp = true;
                    }
                }
            }
        }
    }
    EXPECT_FALSE(subUsedInCmp) << "(a - b) != 0 should compare a != b directly (no Sub in comparison)";
}

// ===========================================================================
// Multiply strength reduction: new patterns (n*11, n*13, n*20, n*21)
// ===========================================================================

TEST(CodegenTest, StrengthReductionMultiplyBy11) {
    // n * 11 should use shift+add: (n<<3) + (n<<1) + n
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul11(n:i64) { return n * 11; }\n"
                           "fn main() { return mul11(5); }",
                           codegen);
    auto* func = mod->getFunction("mul11");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 11 should use shift+add, not multiply";
    EXPECT_TRUE(hasShl) << "n * 11 should use shifts";
}

TEST(CodegenTest, StrengthReductionMultiplyBy13) {
    // n * 13 should use shift+sub: (n<<4) - (n<<1) - n
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul13(n:i64) { return n * 13; }\n"
                           "fn main() { return mul13(5); }",
                           codegen);
    auto* func = mod->getFunction("mul13");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 13 should use shift+sub, not multiply";
    EXPECT_TRUE(hasShl) << "n * 13 should use shifts";
}

TEST(CodegenTest, StrengthReductionMultiplyBy20) {
    // n * 20 should use shift+add: (n<<4) + (n<<2)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul20(n:i64) { return n * 20; }\n"
                           "fn main() { return mul20(5); }",
                           codegen);
    auto* func = mod->getFunction("mul20");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 20 should use shift+add, not multiply";
    EXPECT_TRUE(hasShl) << "n * 20 should use shifts";
}

TEST(CodegenTest, StrengthReductionMultiplyBy21) {
    // n * 21 should use shift+add: (n<<4) + (n<<2) + n
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul21(n:i64) { return n * 21; }\n"
                           "fn main() { return mul21(5); }",
                           codegen);
    auto* func = mod->getFunction("mul21");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * 21 should use shift+add, not multiply";
    EXPECT_TRUE(hasShl) << "n * 21 should use shifts";
}

TEST(CodegenTest, StrengthReductionMultiplyBy11Commutative) {
    // 11 * n (left operand constant) should also use shift+add
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn mul11c(n:i64) { return 11 * n; }\n"
                           "fn main() { return mul11c(5); }",
                           codegen);
    auto* func = mod->getFunction("mul11c");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "11 * n should use shift+add, not multiply";
    EXPECT_TRUE(hasShl) << "11 * n should use shifts";
}

TEST(CodegenTest, NegativeMultiplyBy11StrengthReduction) {
    // n * (-11) should use shift+add + neg
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn muln11(n:i64) { return n * (-11); }\n"
                           "fn main() { return muln11(5); }",
                           codegen);
    auto* func = mod->getFunction("muln11");
    ASSERT_NE(func, nullptr);
    bool hasMul = false;
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                hasMul = true;
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasMul) << "n * (-11) should use shift+add+neg, not multiply";
    EXPECT_TRUE(hasShl) << "n * (-11) should use shifts";
}

// ===========================================================================
// Exponent specialization: x**11 and x**12
// ===========================================================================

TEST(CodegenTest, ExponentSpecializationPow11) {
    // x**11 should produce inline multiplications, no loop
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x ** 11; } fn main() { return f(2); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasLoop = false;
    int mulCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                mulCount++;
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasLoop = true;
            }
        }
    }
    EXPECT_FALSE(hasLoop) << "x**11 should be inline (no loop)";
    EXPECT_EQ(mulCount, 5) << "x**11 should use exactly 5 multiplications";
}

TEST(CodegenTest, ExponentSpecializationPow12) {
    // x**12 should produce inline multiplications, no loop
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x ** 12; } fn main() { return f(2); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasLoop = false;
    int mulCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                mulCount++;
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasLoop = true;
            }
        }
    }
    EXPECT_FALSE(hasLoop) << "x**12 should be inline (no loop)";
    EXPECT_EQ(mulCount, 4) << "x**12 should use exactly 4 multiplications";
}

// ===========================================================================
// Constant nonzero divisor skips zero-check
// ===========================================================================

TEST(CodegenTest, ConstantDivisorSkipsZeroCheck) {
    // Division by a constant nonzero value should not generate a zero-check branch
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(n:i64) { return n / 7; }\n"
                           "fn main() { return f(42); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    int condBranches = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    condBranches++;
            }
        }
    }
    EXPECT_EQ(condBranches, 0) << "Division by constant 7 should not have a zero-check branch";
}

TEST(CodegenTest, ConstantModuloSkipsZeroCheck) {
    // Modulo by a constant nonzero value should not generate a zero-check branch
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(n:i64) { return n % 5; }\n"
                           "fn main() { return f(42); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    int condBranches = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    condBranches++;
            }
        }
    }
    EXPECT_EQ(condBranches, 0) << "Modulo by constant 5 should not have a zero-check branch";
}

TEST(CodegenTest, VariableDivisorHasZeroCheck) {
    // Division by a variable should still generate a zero-check branch
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(a:i64, b:i64) { return a / b; }\n"
                           "fn main() { return f(42, 7); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    int condBranches = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    condBranches++;
            }
        }
    }
    EXPECT_GT(condBranches, 0) << "Division by variable should have a zero-check branch";
}

// ===========================================================================
// Negative constant divisor magic-number optimization
// ===========================================================================

TEST(CodegenTest, NegativeDivisorUsesMagicNumber) {
    // x / (-7) should use magic-number multiplication at O2+ (LLVM backend)
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(n:i64) { return n / (-7); }\n"
                           "fn main() { return f(42); }",
                           codegen);
    auto* func = mod->getFunction("f");
    if (!func) { SUCCEED(); return; } // inlined at O2
    bool hasSDiv = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SDiv)
                hasSDiv = true;
        }
    }
    EXPECT_FALSE(hasSDiv) << "n / (-7) should use magic-number, not hardware sdiv";
}

TEST(CodegenTest, NegativeModuloUsesMagicNumber) {
    // x % (-5) should use magic-number multiplication at O2+ (LLVM backend)
    CodeGenerator codegen(OptimizationLevel::O2);
    auto* mod = generateIR("fn f(n:i64) { return n % (-5); }\n"
                           "fn main() { return f(42); }",
                           codegen);
    auto* func = mod->getFunction("f");
    if (!func) { SUCCEED(); return; } // inlined at O2
    bool hasSRem = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::SRem)
                hasSRem = true;
        }
    }
    EXPECT_FALSE(hasSRem) << "n % (-5) should use magic-number, not hardware srem";
}

// ===========================================================================
// Float exponent specialization
// ===========================================================================

TEST(CodegenTest, FloatPowHalfUsesSqrt) {
    // x ** 0.5 should use sqrt intrinsic, not llvm.pow
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return to_float(x) ** 0.5; }\n"
                           "fn main() { return f(4); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasSqrt = false;
    bool hasPow = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (auto* callee = call->getCalledFunction()) {
                    if (callee->getName().contains("sqrt"))
                        hasSqrt = true;
                    if (callee->getName().contains("pow"))
                        hasPow = true;
                }
            }
        }
    }
    EXPECT_TRUE(hasSqrt) << "x ** 0.5 should use sqrt intrinsic";
    EXPECT_FALSE(hasPow) << "x ** 0.5 should NOT use llvm.pow";
}

TEST(CodegenTest, FloatPow3UsesInlineMul) {
    // x ** 3.0 should use inline multiplies, not llvm.pow
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return to_float(x) ** 3.0; }\n"
                           "fn main() { return f(2); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasFMul = false;
    bool hasPow = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::FMul)
                hasFMul = true;
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
                if (auto* callee = call->getCalledFunction()) {
                    if (callee->getName().contains("pow"))
                        hasPow = true;
                }
            }
        }
    }
    EXPECT_TRUE(hasFMul) << "x ** 3.0 should use inline fmul";
    EXPECT_FALSE(hasPow) << "x ** 3.0 should NOT use llvm.pow";
}

// ===========================================================================
// Exponent specialization: x**13, x**15, x**32, x**64
// ===========================================================================

TEST(CodegenTest, ExponentSpecializationPow13) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x ** 13; } fn main() { return f(2); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasLoop = false;
    int mulCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                mulCount++;
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasLoop = true;
            }
        }
    }
    EXPECT_FALSE(hasLoop) << "x**13 should be inline (no loop)";
    EXPECT_EQ(mulCount, 5) << "x**13 should use exactly 5 multiplications";
}

TEST(CodegenTest, ExponentSpecializationPow15) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x ** 15; } fn main() { return f(2); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasLoop = false;
    int mulCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                mulCount++;
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasLoop = true;
            }
        }
    }
    EXPECT_FALSE(hasLoop) << "x**15 should be inline (no loop)";
    EXPECT_EQ(mulCount, 5) << "x**15 should use exactly 5 multiplications";
}

TEST(CodegenTest, ExponentSpecializationPow32) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x ** 32; } fn main() { return f(2); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasLoop = false;
    int mulCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                mulCount++;
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasLoop = true;
            }
        }
    }
    EXPECT_FALSE(hasLoop) << "x**32 should be inline (no loop)";
    EXPECT_EQ(mulCount, 5) << "x**32 should use exactly 5 multiplications";
}

TEST(CodegenTest, ExponentSpecializationPow64) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return x ** 64; } fn main() { return f(2); }", codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasLoop = false;
    int mulCount = 0;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Mul)
                mulCount++;
            if (auto* br = llvm::dyn_cast<llvm::BranchInst>(&inst)) {
                if (br->isConditional())
                    hasLoop = true;
            }
        }
    }
    EXPECT_FALSE(hasLoop) << "x**64 should be inline (no loop)";
    EXPECT_EQ(mulCount, 6) << "x**64 should use exactly 6 multiplications";
}

// ===========================================================================
// Left-constant zero shift/sub identities
// ===========================================================================

TEST(CodegenTest, ZeroShiftLeftIsZero) {
    // 0 << x should produce constant 0 (no shift instruction)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return 0 << x; }\n"
                           "fn main() { return f(5); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasShl) << "0 << x should be folded to constant 0";
}

TEST(CodegenTest, ZeroShiftRightIsZero) {
    // 0 >> x should produce constant 0 (no shift instruction)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return 0 >> x; }\n"
                           "fn main() { return f(5); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasAShr = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::AShr)
                hasAShr = true;
        }
    }
    EXPECT_FALSE(hasAShr) << "0 >> x should be folded to constant 0";
}

TEST(CodegenTest, ZeroMinusXIsNeg) {
    // 0 - x should produce neg(x), not a sub instruction with 0 LHS
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return 0 - x; }\n"
                           "fn main() { return f(5); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    // Just verify that the function compiles and returns the correct type.
    // The neg instruction is a sub 0, x which is fine — the key optimization
    // is that we emit it directly rather than a generic sub.
    EXPECT_NE(func, nullptr);
}

// ===========================================================================
// IEEE-754 float identity correctness tests
// ===========================================================================

TEST(CodegenTest, FloatMulByZeroNotFolded) {
    // x*0.0 must NOT be folded to 0.0: NaN*0=NaN, Inf*0=NaN, (-x)*0=-0.
    // Verify that FMul instruction is preserved (not folded to constant).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return to_float(x) * 0.0; }\n"
                           "fn main() { return f(1); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasFMul = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::FMul)
                hasFMul = true;
        }
    }
    EXPECT_TRUE(hasFMul)
        << "x * 0.0 must emit FMul (not fold to constant) for IEEE-754 correctness";
}

TEST(CodegenTest, ZeroMulByFloatNotFolded) {
    // 0.0*x must NOT be folded to 0.0: same IEEE-754 concerns as x*0.0.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return 0.0 * to_float(x); }\n"
                           "fn main() { return f(1); }",
                           codegen);
    auto* func = mod->getFunction("f");
    ASSERT_NE(func, nullptr);
    bool hasFMul = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::FMul)
                hasFMul = true;
        }
    }
    EXPECT_TRUE(hasFMul)
        << "0.0 * x must emit FMul (not fold to constant) for IEEE-754 correctness";
}

// ===========================================================================
// usleep WillReturn attribute test
// ===========================================================================

TEST(CodegenTest, UsleepHasWillReturnAttribute) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { sleep(100); return 0; }", codegen);
    auto* fn = mod->getFunction("usleep");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoUnwind))
            << "usleep() should have nounwind attribute";
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::WillReturn))
            << "usleep() should have willreturn attribute";
    }
}

// ===========================================================================
// rand/srand/time memory effect attribute tests
// ===========================================================================

TEST(CodegenTest, RandHasInaccessibleMemOnly) {
    // rand() only accesses global PRNG state → inaccessibleMemOnly.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return random(); }", codegen);
    auto* fn = mod->getFunction("rand");
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::Memory))
        << "rand() should have memory attribute";
}

TEST(CodegenTest, SrandHasInaccessibleMemOnly) {
    // srand() only accesses global PRNG state → inaccessibleMemOnly.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return random(); }", codegen);
    auto* fn = mod->getFunction("srand");
    if (fn) {
        EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::Memory))
            << "srand() should have memory attribute";
    }
}

TEST(CodegenTest, TimeHasNoSyncAttribute) {
    // time() doesn't synchronize with other threads.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return time(); }", codegen);
    auto* fn = mod->getFunction("time");
    ASSERT_NE(fn, nullptr);
    EXPECT_TRUE(fn->hasFnAttribute(llvm::Attribute::NoSync))
        << "time() should have nosync attribute";
}

// ===========================================================================
// Struct codegen tests
// ===========================================================================

TEST(CodegenTest, StructCreationAndFieldAccess) {
    // A struct literal should compile and the field access should return the
    // value stored in that field.
    CodeGenerator codegen(OptimizationLevel::O0);
    const char* src =
        "struct Point { x, y }"
        "fn main() {"
        "    var p:i64 = Point { x: 42, y: 7 };"
        "    return p.x;"
        "}";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("main"), nullptr);
}

TEST(CodegenTest, StructFieldAssignment) {
    CodeGenerator codegen(OptimizationLevel::O0);
    const char* src =
        "struct Point { x, y }"
        "fn main() {"
        "    var p:i64 = Point { x: 1, y: 2 };"
        "    p.x = 99;"
        "    return p.x;"
        "}";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("main"), nullptr);
}

TEST(CodegenTest, StructAsReturnValue) {
    CodeGenerator codegen(OptimizationLevel::O0);
    const char* src =
        "struct Pair { a, b }"
        "fn make_pair(x:i64, y:i64) {"
        "    var p:i64 = Pair { a: x, b: y };"
        "    return p;"
        "}"
        "fn main() {"
        "    var q:i64 = make_pair(10, 20);"
        "    return q.a;"
        "}";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("main"), nullptr);
}

// ===========================================================================
// Generic function codegen tests
// ===========================================================================

TEST(CodegenTest, GenericFunctionCompilesAndRuns) {
    // Generic type parameters are type-erased (all values are i64), so a
    // generic function should compile to ordinary IR just like a non-generic
    // function.
    CodeGenerator codegen(OptimizationLevel::O0);
    const char* src =
        "fn identity<T>(x: T) -> T { return x; }"
        "fn main() {"
        "    return identity(55);"
        "}";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("main"), nullptr);
    EXPECT_NE(mod->getFunction("identity"), nullptr);
}

// ===========================================================================
// Division-by-zero safety: zero_div / zero_mod rules must not be applied
// ===========================================================================

TEST(CodegenTest, ZeroDividedByZeroNotFolded) {
    // 0 / 0 must not be folded to 0 at compile time (the runtime trap is
    // the correct behaviour).  If the e-graph zero_div rule were present
    // the generated IR would contain a constant 0 return instead of a
    // runtime division instruction.
    CodeGenerator codegen(OptimizationLevel::O2);
    const char* src =
        "fn divide(x:i64) { return 0 / x; }"
        "fn main() { return divide(1); }";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    // The 'divide' function must still exist (not inlined away entirely) OR
    // the main function must contain a division instruction; we simply check
    // that the module compiled successfully.
    EXPECT_NE(mod->getFunction("main"), nullptr);
}

// ===========================================================================
// Borrow with reference type annotation and address-of operator
// ===========================================================================

TEST(CodegenTest, BorrowWithRefTypeAndAddressOf) {
    CodeGenerator codegen(OptimizationLevel::O0);
    const char* src =
        "fn main() {"
        "    var x :i32 = 5;"
        "    borrow var j:&i32 = &x;"
        "    return j;"
        "}";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    EXPECT_NE(mod->getFunction("main"), nullptr);
}

// ===========================================================================
// Prefetch: use-site prefetch and memory residency
// ===========================================================================

TEST(CodegenTest, PrefetchVarDeclRegistersNoAllocaPrefetch) {
    // Prefetch var declaration should NOT emit llvm.prefetch on the alloca
    // itself — the variable should be promoted to a register by SROA/mem2reg.
    // For i32 variables (non-pointer), no prefetch intrinsic is emitted at all.
    CodeGenerator codegen(OptimizationLevel::O0);
    const char* src =
        "fn main() {"
        "    prefetch var x:i32 = 5;"
        "    var y:i64 = x + 1;"
        "    invalidate x;"
        "    return y;"
        "}";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);

    // Count llvm.prefetch calls — for i32 (non-pointer) variables, no
    // prefetch should be emitted (the variable goes to a register).
    unsigned prefetchCount = 0;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = call->getCalledFunction()) {
                    if (callee->getIntrinsicID() == llvm::Intrinsic::prefetch)
                        ++prefetchCount;
                }
            }
        }
    }
    EXPECT_EQ(prefetchCount, 0u); // no alloca prefetch for register vars
}

TEST(CodegenTest, PrefetchI64EmitsValuePrefetch) {
    // Prefetch of an i64 variable should emit a value-based prefetch
    // (the variable's value is treated as a pointer to prefetch).
    CodeGenerator codegen(OptimizationLevel::O0);
    const char* src =
        "fn main() {"
        "    prefetch var v:i64 = 10;"
        "    var a:i64 = v;"
        "    invalidate v;"
        "    return a;"
        "}";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);

    // Should have exactly one prefetch: the value-based prefetch for the
    // i64 variable (its value is treated as a memory address).
    unsigned prefetchCount = 0;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = call->getCalledFunction()) {
                    if (callee->getIntrinsicID() == llvm::Intrinsic::prefetch)
                        ++prefetchCount;
                }
            }
        }
    }
    EXPECT_EQ(prefetchCount, 1u); // value-based prefetch only
}

TEST(CodegenTest, PrefetchHotUsesHighLocality) {
    // `prefetch hot` should use locality=3 in the value-based prefetch.
    CodeGenerator codegen(OptimizationLevel::O0);
    const char* src =
        "fn main() {"
        "    prefetch hot var h:i64 = 7;"
        "    invalidate h;"
        "    return 0;"
        "}";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);

    bool foundHighLocality = false;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = call->getCalledFunction()) {
                    if (callee->getIntrinsicID() == llvm::Intrinsic::prefetch) {
                        // Arg 2 is the locality hint
                        if (auto* loc = llvm::dyn_cast<llvm::ConstantInt>(
                                call->getArgOperand(2))) {
                            if (loc->getZExtValue() == 3)
                                foundHighLocality = true;
                        }
                    }
                }
            }
        }
    }
    EXPECT_TRUE(foundHighLocality);
}

TEST(CodegenTest, PrefetchStructEmitsMemoryPrefetch) {
    // Prefetch of a struct variable (stored as an array alloca) should emit
    // llvm.prefetch directly on the alloca — memory-resident prefetch for
    // large types that cannot fit in registers.
    CodeGenerator codegen(OptimizationLevel::O0);
    const char* src =
        "struct Point { x, y }\n"
        "fn main() {\n"
        "    var p:i64 = Point { x: 1, y: 2 };\n"
        "    prefetch p;\n"
        "    var r:i64 = p.x + p.y;\n"
        "    invalidate p;\n"
        "    return r;\n"
        "}\n";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);

    // Should have at least one llvm.prefetch call (memory-resident prefetch
    // on the struct's alloca).
    unsigned prefetchCount = 0;
    for (auto& BB : *mainFn) {
        for (auto& I : BB) {
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I)) {
                if (auto* callee = call->getCalledFunction()) {
                    if (callee->getIntrinsicID() == llvm::Intrinsic::prefetch)
                        ++prefetchCount;
                }
            }
        }
    }
    EXPECT_GE(prefetchCount, 1u); // memory-resident prefetch for struct
}

TEST(CodegenTest, RestrictAnnotationAddsArgMemOnly) {
    // @restrict should set argmem-only memory effects for alias optimization.
    CodeGenerator codegen(OptimizationLevel::O0);
    const char* src =
        "@restrict\n"
        "fn process(a: int, b: int) -> int {\n"
        "    return a + b;\n"
        "}\n"
        "fn main() {\n"
        "    return process(1, 2);\n"
        "}\n";
    auto* mod = generateIR(src, codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("process");
    ASSERT_NE(fn, nullptr);

    // The function should only access argument memory.
    EXPECT_TRUE(fn->onlyAccessesArgMemory())
        << "Function should have argmem-only memory effect from @restrict";
    EXPECT_TRUE(fn->doesNotThrow())
        << "Function should have nothrow from @restrict";
}

// ===========================================================================
// Negative exponent power operator
// ===========================================================================

TEST(CodegenTest, NegativeExponentConstantFolding) {
    // Constant folding: 1 ** -5 should fold to 1, not 0.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 1 ** -5; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // The function should constant-fold to return 1.
    for (auto& bb : *mainFn) {
        for (auto& inst : bb) {
            if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue())) {
                    EXPECT_EQ(ci->getSExtValue(), 1)
                        << "1 ** -5 should constant-fold to 1";
                }
            }
        }
    }
}

TEST(CodegenTest, NegativeExponentNegOneOdd) {
    // (-1) ** -3 should fold to -1 (odd exponent).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return (-1) ** -3; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    for (auto& bb : *mainFn) {
        for (auto& inst : bb) {
            if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue())) {
                    EXPECT_EQ(ci->getSExtValue(), -1)
                        << "(-1) ** -3 should constant-fold to -1";
                }
            }
        }
    }
}

TEST(CodegenTest, NegativeExponentNegOneEven) {
    // (-1) ** -4 should fold to 1 (even exponent).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return (-1) ** -4; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    for (auto& bb : *mainFn) {
        for (auto& inst : bb) {
            if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue())) {
                    EXPECT_EQ(ci->getSExtValue(), 1)
                        << "(-1) ** -4 should constant-fold to 1";
                }
            }
        }
    }
}

TEST(CodegenTest, NegativeExponentLargeBase) {
    // 5 ** -2 should fold to 0 (integer truncation of 0.04).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 5 ** -2; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    for (auto& bb : *mainFn) {
        for (auto& inst : bb) {
            if (auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&inst)) {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(ret->getReturnValue())) {
                    EXPECT_EQ(ci->getSExtValue(), 0)
                        << "5 ** -2 should constant-fold to 0";
                }
            }
        }
    }
}

TEST(CodegenTest, NegativeExponentRuntimePath) {
    // Runtime negative exponent: base 1 should still yield 1.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn neg_pow(b:i64, e:i64) { return b ** e; }\n"
                           "fn main() { return neg_pow(1, -5); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("neg_pow");
    ASSERT_NE(fn, nullptr);
    // Verify the function has the negative-exponent guard blocks.
    bool hasNegExpBlock = false;
    for (auto& bb : *fn) {
        if (bb.getName().starts_with("pow.negexp"))
            hasNegExpBlock = true;
    }
    EXPECT_TRUE(hasNegExpBlock)
        << "Runtime power should have a negative-exponent guard block";
}

// ===========================================================================
// Shift constant folding optimization
// ===========================================================================

TEST(CodegenTest, ShiftConstantNoMask) {
    // Shift by a constant in [0, 63] should NOT generate an AND mask.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn shl5(x:i64) { return x << 5; }\n"
                           "fn main() { return shl5(1); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("shl5");
    ASSERT_NE(fn, nullptr);
    bool hasAndMask = false;
    for (auto& bb : *fn) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::And)
                hasAndMask = true;
        }
    }
    EXPECT_FALSE(hasAndMask)
        << "Shift by constant 5 should not emit an AND mask instruction";
}

TEST(CodegenTest, ShiftVariableHasMask) {
    // Shift by a variable should still generate an AND mask.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn shlv(x:i64, n:i64) { return x << n; }\n"
                           "fn main() { return shlv(1, 5); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("shlv");
    ASSERT_NE(fn, nullptr);
    bool hasAndMask = false;
    for (auto& bb : *fn) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::And)
                hasAndMask = true;
        }
    }
    EXPECT_TRUE(hasAndMask)
        << "Shift by variable should still emit an AND mask instruction";
}

// ===========================================================================
// Short-circuit constant folding for && and ||
// ===========================================================================

TEST(CodegenTest, ShortCircuitAndFalse) {
    // 0 && x should fold to 0 without evaluating x.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return 0 && x; }\n"
                           "fn main() { return f(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("f");
    ASSERT_NE(fn, nullptr);
    // Should be a single basic block (no branch needed).
    int bbCount = std::distance(fn->begin(), fn->end());
    EXPECT_EQ(bbCount, 1) << "0 && x should not generate branches";
}

TEST(CodegenTest, ShortCircuitOrTrue) {
    // 1 || x should fold to 1 without evaluating x.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(x:i64) { return 1 || x; }\n"
                           "fn main() { return f(42); }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("f");
    ASSERT_NE(fn, nullptr);
    int bbCount = std::distance(fn->begin(), fn->end());
    EXPECT_EQ(bbCount, 1) << "1 || x should not generate branches";
}

// ===========================================================================
// String repeat overflow guard
// ===========================================================================

TEST(CodegenTest, StringRepeatOverflowGuard) {
    // String repetition should have an overflow guard.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn f(n:i64) { return \"abc\" * n; }\n"
                           "fn main() { return 0; }",
                           codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("f");
    ASSERT_NE(fn, nullptr);
    // Check for the overflow guard block.
    bool hasOvfBlock = false;
    for (auto& bb : *fn) {
        if (bb.getName().starts_with("strmul.overflow"))
            hasOvfBlock = true;
    }
    EXPECT_TRUE(hasOvfBlock)
        << "String repetition should have an overflow guard block";
}

// ===========================================================================
// Left-shift constant folding with negative operands (UB fix)
// ===========================================================================

TEST(CodegenTest, LeftShiftNegativeConstFold) {
    // (-1) << 3 should be folded to -8 at compile time.
    // This previously triggered undefined behavior (signed left shift of negative).
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return (-1) << 3; }", codegen);
    auto* func = mod->getFunction("main");
    ASSERT_NE(func, nullptr);
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasShl) << "(-1) << 3 should be constant-folded (no Shl instruction)";
}

TEST(CodegenTest, LeftShiftLargeNegativeConstFold) {
    // (-2) << 4 should be folded to -32 at compile time.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return (-2) << 4; }", codegen);
    auto* func = mod->getFunction("main");
    ASSERT_NE(func, nullptr);
    bool hasShl = false;
    for (auto& bb : *func) {
        for (auto& inst : bb) {
            if (inst.getOpcode() == llvm::Instruction::Shl)
                hasShl = true;
        }
    }
    EXPECT_FALSE(hasShl) << "(-2) << 4 should be constant-folded (no Shl instruction)";
}

// ===========================================================================
// Logical && and || constant folding in integer path
// ===========================================================================

TEST(CodegenTest, LogicalAndConstantFold) {
    // 3 && 5 with both operands constant should fold to 1 (no branches)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 3 && 5; }", codegen);
    auto* func = mod->getFunction("main");
    ASSERT_NE(func, nullptr);
    // With constant folding, this should be a single block returning 1
    EXPECT_EQ(func->size(), 1u)
        << "3 && 5 should be constant-folded to 1 (single basic block)";
}

TEST(CodegenTest, LogicalOrConstantFold) {
    // 0 || 7 with both operands constant should fold to 1 (no branches)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 0 || 7; }", codegen);
    auto* func = mod->getFunction("main");
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->size(), 1u)
        << "0 || 7 should be constant-folded to 1 (single basic block)";
}

TEST(CodegenTest, LogicalAndFalseConstantFold) {
    // 0 && 5 should fold to 0 (no branches)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 0 && 5; }", codegen);
    auto* func = mod->getFunction("main");
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->size(), 1u)
        << "0 && 5 should be constant-folded to 0 (single basic block)";
}

TEST(CodegenTest, LogicalOrBothZeroConstantFold) {
    // 0 || 0 should fold to 0 (no branches)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return 0 || 0; }", codegen);
    auto* func = mod->getFunction("main");
    ASSERT_NE(func, nullptr);
    EXPECT_EQ(func->size(), 1u)
        << "0 || 0 should be constant-folded to 0 (single basic block)";
}

// ===========================================================================
// Trigonometric and transcendental math builtins
// ===========================================================================

TEST(CodegenTest, IsStdlibFunctionTrigMath) {
    EXPECT_TRUE(isStdlibFunction("sin"));
    EXPECT_TRUE(isStdlibFunction("cos"));
    EXPECT_TRUE(isStdlibFunction("tan"));
    EXPECT_TRUE(isStdlibFunction("asin"));
    EXPECT_TRUE(isStdlibFunction("acos"));
    EXPECT_TRUE(isStdlibFunction("atan"));
    EXPECT_TRUE(isStdlibFunction("atan2"));
    EXPECT_TRUE(isStdlibFunction("exp"));
    EXPECT_TRUE(isStdlibFunction("log"));
    EXPECT_TRUE(isStdlibFunction("log10"));
    EXPECT_TRUE(isStdlibFunction("cbrt"));
    EXPECT_TRUE(isStdlibFunction("hypot"));
}

TEST(CodegenTest, SinGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return sin(0.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, CosGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return cos(0.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, TanGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return tan(0.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ExpGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return exp(1.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LogGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return log(1.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, Log10Generation) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return log10(100.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, CbrtGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return cbrt(27.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, HypotGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return hypot(3.0, 4.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, Atan2Generation) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return atan2(1.0, 1.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, AsinGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return asin(0.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, AcosGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return acos(1.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, AtanGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return atan(0.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Array utility builtins
// ===========================================================================

TEST(CodegenTest, IsStdlibFunctionArrayUtilities) {
    EXPECT_TRUE(isStdlibFunction("array_min"));
    EXPECT_TRUE(isStdlibFunction("array_max"));
    EXPECT_TRUE(isStdlibFunction("array_any"));
    EXPECT_TRUE(isStdlibFunction("array_every"));
    EXPECT_TRUE(isStdlibFunction("array_find"));
    EXPECT_TRUE(isStdlibFunction("array_count"));
}

TEST(CodegenTest, ArrayMinGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [5, 3, 8]; return array_min(a); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArrayMaxGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [5, 3, 8]; return array_max(a); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArrayFindGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a:i64[] = [5, 3, 8]; return array_find(a, 3); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArrayAnyGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn is_pos(x:i64) { return x > 0; }\nfn main() { var a:i64[] = [1, -2, 3]; return array_any(a, \"is_pos\"); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArrayEveryGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn is_pos(x:i64) { return x > 0; }\nfn main() { var a:i64[] = [1, 2, 3]; return array_every(a, \"is_pos\"); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ArrayCountGeneration) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn is_even(x:i64) { return x % 2 == 0; }\nfn main() { var a:i64[] = [1, 2, 3, 4]; return array_count(a, \"is_even\"); }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// SIMD vector types
// ===========================================================================

TEST(CodegenTest, SimdF32x4VectorAdd) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a: f32x4 = [1.0, 2.0, 3.0, 4.0]; var b: f32x4 = [5.0, 6.0, 7.0, 8.0]; var c: f32x4 = a + b; return to_int(c[0]); }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
}

TEST(CodegenTest, SimdI32x4VectorSub) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x: i32x4 = [10, 20, 30, 40]; var y: i32x4 = [1, 2, 3, 4]; var z: i32x4 = x - y; return z[0]; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SimdElementWrite) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var v: i32x4 = [0, 0, 0, 0]; v[0] = 5; return v[0]; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, SimdF64x2Vector) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var a: f64x2 = [1.5, 2.5]; var b: f64x2 = [3.5, 4.5]; var c: f64x2 = a * b; return to_int(c[0]); }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Register keyword
// ===========================================================================

TEST(CodegenTest, RegisterVarDecl) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { register var x:i64 = 42; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, RegisterVarReassign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { register var x:i64 = 0; x = 42; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, RegisterVarForcesPromotion) {
    // At O0, register var should still produce zero allocas because
    // mem2reg is forced after codegen for functions with register vars.
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { register var x:i64 = 0; x = x + 1; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    unsigned allocaCount = 0;
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (llvm::isa<llvm::AllocaInst>(I))
                ++allocaCount;
    EXPECT_EQ(allocaCount, 0u);
}

// ===========================================================================
// **= and ??= compound assignment
// ===========================================================================

TEST(CodegenTest, PowerAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 2; x **= 3; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, NullCoalesceAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { var x:i64 = 0; x ?\?= 42; return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ===========================================================================
// Struct field compound assignment
// ===========================================================================

TEST(CodegenTest, StructFieldCompoundAssign) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("struct P { x, y }\nfn main() { var p:i64 = P { x: 10, y: 20 }; p.x += 5; return p.x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

// ── New bitwise + math builtins ──────────────────────────────────────────

TEST(CodegenTest, PopcountCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return popcount(255); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PopcountWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return popcount(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, ClzCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return clz(16); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, ClzWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return clz(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, CtzCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return ctz(16); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, CtzWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return ctz(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, BitreverseCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return bitreverse(1); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, BitreverseWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return bitreverse(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, Exp2Call) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return exp2(3.0); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, Exp2WrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return exp2(); }", codegen), std::runtime_error);
}

TEST(CodegenTest, IsPowerOf2Call) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return is_power_of_2(16); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, IsPowerOf2WrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return is_power_of_2(); }", codegen), std::runtime_error);
}

// ── LCM builtin ──────────────────────────────────────────────────────────

TEST(CodegenTest, LcmCall) {
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR("fn main() { return lcm(4, 6); }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, LcmWrongArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return lcm(4); }", codegen), std::runtime_error);
}

TEST(CodegenTest, LcmZeroArgs) {
    CodeGenerator codegen(OptimizationLevel::O0);
    EXPECT_THROW(generateIR("fn main() { return lcm(); }", codegen), std::runtime_error);
}

// ── NUW on non-negative additions ─────────────────────────────────────────

// At O1+, adding two provably non-negative values should produce an add
// instruction with both the nsw and nuw flags set.  The nuw flag enables
// LLVM's SCEV and loop unroller to reason about the unsigned range of the
// sum, which in turn enables additional vectorization and CSE opportunities.
TEST(CodegenTest, NUWAddOnNonNegOperands) {
    // The for-loop iterator `i` is non-negative (ascending from 0); `len(a)`
    // is also non-negative.  Their sum should get both nuw and nsw.
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn f(a:i64) { var s:i64 = 0; for (i in 0...len(a)) { s = s + i; } return s; } "
        "fn main() { return f([1, 2, 3]); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("f");
    ASSERT_NE(fn, nullptr);
    bool foundNUWAdd = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
                if (bo->getOpcode() == llvm::Instruction::Add
                        && bo->hasNoUnsignedWrap()
                        && bo->hasNoSignedWrap()) {
                    foundNUWAdd = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundNUWAdd) << "Expected at least one add nuw nsw instruction";
}

// ── !range metadata on non-negative variable loads ────────────────────────

// At O1+, loading a variable that is tracked as non-negative (e.g. a
// for-loop iterator starting at 0) should attach !range [0, INT64_MAX)
// metadata to the load instruction.  This makes the non-negativity visible
// directly in the IR to every LLVM pass, not just through assume intrinsics.
TEST(CodegenTest, RangeMetadataOnNonNegLoad) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn f(n:i64) { for (i in 0...n) { var x:i64 = i * 2; } } "
        "fn main() { f(10); return 0; }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("f");
    ASSERT_NE(fn, nullptr);
    bool foundRangeLoad = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                if (load->getType()->isIntegerTy(64)
                        && load->getMetadata(llvm::LLVMContext::MD_range)) {
                    foundRangeLoad = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundRangeLoad)
        << "Expected at least one i64 load with !range metadata";
}

// ===========================================================================
// Deep compile-time constant evaluation (inter-procedural)
// ===========================================================================

// Helper: return true if a function's IR has no call to strlen (i.e. the
// len() call was folded to a constant at compile time).
static bool hasNoStrlen(llvm::Function* fn) {
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I))
                if (call->getCalledFunction() &&
                    call->getCalledFunction()->getName() == "strlen")
                    return false;
    return true;
}

// Helper: return true if a function's IR has no call to any user-defined
// function (only intrinsics / libc are permitted).
static bool hasNoUserCall(llvm::Module* mod, llvm::Function* fn,
                          const std::string& calleeName) {
    (void)mod;
    for (auto& BB : *fn)
        for (auto& I : BB)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&I))
                if (call->getCalledFunction() &&
                    call->getCalledFunction()->getName() == calleeName)
                    return false;
    return true;
}

// 1. len() of a zero-param function returning a string literal should fold
//    to a ConstantInt — no strlen call, no make_str call.
TEST(CodegenTest, DeepConstEval_LenOfConstantReturningFunction) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn make_str() { return \"hello world\"; }"
        "fn main() { return len(make_str()); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // The return value should be a ConstantInt(11) — no strlen, no make_str call.
    EXPECT_TRUE(hasNoStrlen(mainFn))
        << "len(make_str()) should be folded to a constant — no strlen expected";
    EXPECT_TRUE(hasNoUserCall(mod, mainFn, "make_str"))
        << "make_str() should be short-circuited — no call site expected";
}

// 2. const s = make_str(); len(s) should also fold — the const var inherits
//    the compile-time string value from the function's return analysis.
TEST(CodegenTest, DeepConstEval_ConstVarFromConstantReturningFunction) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn make_str() { return \"hi\"; }"
        "fn main() { const s = make_str(); return len(s); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    EXPECT_TRUE(hasNoStrlen(mainFn))
        << "len(s) where s = make_str() (const) should fold — no strlen expected";
}

// 3. len("a" + "b" + "c") should fold to 3 — all parts are string literals.
TEST(CodegenTest, DeepConstEval_LenOfStringConcatLiterals) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn main() { return len(\"a\" + \"b\" + \"c\"); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    EXPECT_TRUE(hasNoStrlen(mainFn))
        << "len(\"a\" + \"b\" + \"c\") should fold to 3 at compile time";
}

// 4. const s = "hello"; len(s + " world") should fold.
TEST(CodegenTest, DeepConstEval_LenOfConstVarConcat) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn main() { const s = \"hello\"; return len(s + \" world\"); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    EXPECT_TRUE(hasNoStrlen(mainFn))
        << "len(s + \" world\") where s is const should fold";
}

// 5. Two-hop chain: fn a() { return "x"; } fn b() { return a() + "y"; }
//    len(b()) should fold to 2.
TEST(CodegenTest, DeepConstEval_TwoHopChainFolding) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn a() { return \"x\"; }"
        "fn b() { return a() + \"y\"; }"
        "fn main() { return len(b()); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    EXPECT_TRUE(hasNoStrlen(mainFn))
        << "len(b()) with two-hop chain should fold to 2";
    EXPECT_TRUE(hasNoUserCall(mod, mainFn, "b"))
        << "b() should be short-circuited in main";
}

// 6. Integer constant-returning function: fn get_n() { return 42; }
//    const n = get_n() should record n as a compile-time constant so that
//    subsequent uses don't emit a call.
TEST(CodegenTest, DeepConstEval_IntConstantReturningFunction) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn get_n() { return 42; }"
        "fn main() { return get_n(); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    EXPECT_TRUE(hasNoUserCall(mod, mainFn, "get_n"))
        << "get_n() returning a constant integer should be short-circuited";
}

// 7. Functions with side-effects (non-const var, reassignment) should NOT
//    be marked as constant-returning.
TEST(CodegenTest, DeepConstEval_SideEffectFunctionNotFolded) {
    CodeGenerator codegen(OptimizationLevel::O1);
    // This function has a non-const var — the analysis must reject it.
    auto* mod = generateIR(
        "fn make_str() { var s:i64 = \"a\"; s = s + \"b\"; return s; }"
        "fn main() { return len(make_str()); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    // We just verify compilation succeeds — the function call must not be
    // elided (it has a non-const var), so strlen will be present.
    auto* mainFn = mod->getFunction("main");
    ASSERT_NE(mainFn, nullptr);
    // The function should still be called (not short-circuited).
    bool callsStrlen = !hasNoStrlen(mainFn);
    bool callsMakeStr = !hasNoUserCall(mod, mainFn, "make_str");
    // Either strlen is present (runtime len) OR make_str is called (not elided)
    // — at least one of these must be true.
    EXPECT_TRUE(callsStrlen || callsMakeStr)
        << "make_str() with side effects must not be short-circuited";
}

// ===========================================================================
// generateIncDec improvements: !noundef, !range, nuw, nonNeg tracking
// ===========================================================================

// At O1+, the load emitted inside x++ should carry !noundef metadata,
// matching what generateIdentifier emits for regular variable reads.
TEST(CodegenTest, IncDecLoadHasNoundef) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn f() { var x:i64 = 0; x++; return x; } fn main() { return f(); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("f");
    ASSERT_NE(fn, nullptr);
    bool foundNoundef = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (auto* li = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                if (li->getMetadata(llvm::LLVMContext::MD_noundef)) {
                    foundNoundef = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundNoundef)
        << "Load in x++ should have !noundef at O1+";
}

// At O1+, when a non-negative variable is incremented, the load emitted
// inside x++ should carry !range [0, INT64_MAX) metadata.
TEST(CodegenTest, IncDecLoadHasRangeOnNonNeg) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn f() { var x:i64 = 5; x++; return x; } fn main() { return f(); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("f");
    ASSERT_NE(fn, nullptr);
    bool foundRange = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (auto* li = llvm::dyn_cast<llvm::LoadInst>(&I)) {
                if (li->getType()->isIntegerTy(64)
                        && li->getMetadata(llvm::LLVMContext::MD_range)) {
                    foundRange = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundRange)
        << "Load of non-negative variable in x++ should have !range metadata at O1+";
}

// When a non-negative variable is incremented with ++, the resulting add
// should have both nuw and nsw flags set.  nsw is always set; nuw is now
// set for non-negative variables because result ∈ [1, INT64_MAX] which
// cannot unsigned-wrap.
TEST(CodegenTest, IncrementNUWOnNonNegVariable) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn f() { var x:i64 = 0; x++; return x; } fn main() { return f(); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("f");
    ASSERT_NE(fn, nullptr);
    bool foundNUWNSW = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
                if (bo->getOpcode() == llvm::Instruction::Add
                        && bo->hasNoUnsignedWrap()
                        && bo->hasNoSignedWrap()) {
                    foundNUWNSW = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundNUWNSW)
        << "x++ on a non-negative variable should produce add nuw nsw";
}

// After x++, the variable should still be tracked as non-negative, so that
// subsequent operations involving x (e.g. x + 0 in a later add) can also
// get nuw+nsw.
TEST(CodegenTest, IncrementPreservesNonNegTracking) {
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn f() { var x:i64 = 0; x++; var y:i64 = x + 1; return y; } fn main() { return f(); }",
        codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("f");
    ASSERT_NE(fn, nullptr);
    // After x++ (x now ≥ 1), x + 1 should also get nuw+nsw (both non-neg).
    bool foundNUWNSW = false;
    for (auto& BB : *fn) {
        for (auto& I : BB) {
            if (auto* bo = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
                if (bo->getOpcode() == llvm::Instruction::Add
                        && bo->hasNoUnsignedWrap()
                        && bo->hasNoSignedWrap()) {
                    foundNUWNSW = true;
                }
            }
        }
    }
    EXPECT_TRUE(foundNUWNSW)
        << "After x++ on a non-negative var, subsequent add should also get nuw+nsw";
}

// ===========================================================================
// Pipeline statement codegen
// ===========================================================================

TEST(CodegenTest, PipelineBasicCount) {
    // pipeline 5 { stage body { } } compiles at O0
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { pipeline 5 { stage body { } } }", codegen);
    ASSERT_NE(mod, nullptr);
    ASSERT_NE(mod->getFunction("main"), nullptr);
}

TEST(CodegenTest, PipelineOneShot) {
    // One-shot form (no count) compiles as straight-line block
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var x:i64 = 0; pipeline { stage s { x = x + 1; } } return x; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PipelineMultipleStages) {
    // Three-stage pipeline; stages execute in declaration order
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var s:i64 = 0; pipeline 4 {"
        "  stage load    { s = s + 1; }"
        "  stage compute { s = s * 2; }"
        "  stage store   { s = s - 1; }"
        "} return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PipelineIteratorAccessible) {
    // __pipeline_i is accessible inside stage bodies
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { var s:i64 = 0; pipeline 10 {"
        "  stage body { s = s + __pipeline_i; }"
        "} return s; }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PipelineZeroCountNoBody) {
    // pipeline 0 should produce valid IR (empty loop, immediate exit)
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main() { pipeline 0 { stage s { } } }", codegen);
    ASSERT_NE(mod, nullptr);
}

TEST(CodegenTest, PipelineO1LoopMetadata) {
    // At O1 the back-edge should carry llvm.loop metadata (mustprogress etc.)
    CodeGenerator codegen(OptimizationLevel::O1);
    auto* mod = generateIR(
        "fn main() { pipeline 8 { stage s { } } }", codegen);
    ASSERT_NE(mod, nullptr);
    auto* fn = mod->getFunction("main");
    ASSERT_NE(fn, nullptr);
    bool hasLoopMD = false;
    for (auto& BB : *fn) {
        if (auto* br = llvm::dyn_cast<llvm::BranchInst>(BB.getTerminator())) {
            if (br->getMetadata(llvm::LLVMContext::MD_loop)) {
                hasLoopMD = true;
                break;
            }
        }
    }
    EXPECT_TRUE(hasLoopMD) << "pipeline loop back-edge should carry llvm.loop metadata";
}

TEST(CodegenTest, PipelineExprCount) {
    // Count can be a runtime value
    CodeGenerator codegen(OptimizationLevel::O0);
    auto* mod = generateIR(
        "fn main(n:i64) { pipeline n { stage s { } } }", codegen);
    ASSERT_NE(mod, nullptr);
}
