#include <gtest/gtest.h>
#include "lexer.h"
#include "parser.h"
#include "ast.h"

using namespace omscript;

// Helper: parse source and return the AST program
static std::unique_ptr<Program> parse(const std::string& src) {
    Lexer lexer(src);
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    return parser.parse();
}

// ---------------------------------------------------------------------------
// Basic function
// ---------------------------------------------------------------------------

TEST(ParserTest, EmptyFunction) {
    auto program = parse("fn main() {}");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->name, "main");
    EXPECT_TRUE(program->functions[0]->parameters.empty());
    EXPECT_TRUE(program->functions[0]->body->statements.empty());
}

TEST(ParserTest, FunctionWithParameters) {
    auto program = parse("fn add(a, b) { return a; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->parameters.size(), 2u);
    EXPECT_EQ(program->functions[0]->parameters[0].name, "a");
    EXPECT_EQ(program->functions[0]->parameters[1].name, "b");
}

TEST(ParserTest, MultipleFunctions) {
    auto program = parse("fn a() {} fn b() {}");
    ASSERT_EQ(program->functions.size(), 2u);
    EXPECT_EQ(program->functions[0]->name, "a");
    EXPECT_EQ(program->functions[1]->name, "b");
}

// ---------------------------------------------------------------------------
// Return statement
// ---------------------------------------------------------------------------

TEST(ParserTest, ReturnLiteral) {
    auto program = parse("fn main() { return 42; }");
    ASSERT_EQ(program->functions[0]->body->statements.size(), 1u);
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(ret, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(ret->value.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->literalType, LiteralExpr::LiteralType::INTEGER);
    EXPECT_EQ(lit->intValue, 42);
}

TEST(ParserTest, ReturnVoid) {
    auto program = parse("fn main() { return; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(ret, nullptr);
    EXPECT_EQ(ret->value, nullptr);
}

// ---------------------------------------------------------------------------
// Variable declaration
// ---------------------------------------------------------------------------

TEST(ParserTest, VarDecl) {
    auto program = parse("fn main() { var x = 10; }");
    auto* var = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->name, "x");
    EXPECT_FALSE(var->isConst);
    ASSERT_NE(var->initializer, nullptr);
}

TEST(ParserTest, ConstDecl) {
    auto program = parse("fn main() { const y = 5; }");
    auto* var = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_TRUE(var->isConst);
}

TEST(ParserTest, VarDeclNoInit) {
    auto program = parse("fn main() { var x; }");
    auto* var = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->initializer, nullptr);
}

TEST(ParserTest, VarDeclWithType) {
    auto program = parse("fn main() { var x: int = 5; }");
    auto* var = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->typeName, "int");
}

// ---------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------

TEST(ParserTest, BinaryExpr) {
    auto program = parse("fn main() { return 1 + 2; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(ret->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "+");
}

TEST(ParserTest, UnaryMinus) {
    auto program = parse("fn main() { return -5; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* un = dynamic_cast<UnaryExpr*>(ret->value.get());
    ASSERT_NE(un, nullptr);
    EXPECT_EQ(un->op, "-");
}

TEST(ParserTest, UnaryNot) {
    auto program = parse("fn main() { return !0; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* un = dynamic_cast<UnaryExpr*>(ret->value.get());
    ASSERT_NE(un, nullptr);
    EXPECT_EQ(un->op, "!");
}

TEST(ParserTest, UnaryBitwiseNot) {
    auto program = parse("fn main() { return ~0; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* un = dynamic_cast<UnaryExpr*>(ret->value.get());
    ASSERT_NE(un, nullptr);
    EXPECT_EQ(un->op, "~");
}

TEST(ParserTest, TernaryExpr) {
    auto program = parse("fn main() { return 1 ? 2 : 3; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* tern = dynamic_cast<TernaryExpr*>(ret->value.get());
    ASSERT_NE(tern, nullptr);
}

TEST(ParserTest, FloatLiteral) {
    auto program = parse("fn main() { return 3.14; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* lit = dynamic_cast<LiteralExpr*>(ret->value.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->literalType, LiteralExpr::LiteralType::FLOAT);
    EXPECT_DOUBLE_EQ(lit->floatValue, 3.14);
}

TEST(ParserTest, StringLiteral) {
    auto program = parse("fn main() { return \"hello\"; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* lit = dynamic_cast<LiteralExpr*>(ret->value.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->literalType, LiteralExpr::LiteralType::STRING);
    EXPECT_EQ(lit->stringValue, "hello");
}

TEST(ParserTest, FunctionCall) {
    auto program = parse("fn main() { foo(1, 2); }");
    auto* stmt = dynamic_cast<ExprStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(stmt, nullptr);
    auto* call = dynamic_cast<CallExpr*>(stmt->expression.get());
    ASSERT_NE(call, nullptr);
    EXPECT_EQ(call->callee, "foo");
    EXPECT_EQ(call->arguments.size(), 2u);
}

TEST(ParserTest, Assignment) {
    auto program = parse("fn main() { var x = 0; x = 5; }");
    auto* stmt = dynamic_cast<ExprStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(stmt, nullptr);
    auto* assign = dynamic_cast<AssignExpr*>(stmt->expression.get());
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->name, "x");
}

TEST(ParserTest, CompoundAssignment) {
    auto program = parse("fn main() { var x = 0; x += 5; }");
    auto* stmt = dynamic_cast<ExprStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(stmt, nullptr);
    // Compound assignment desugars to: x = x + 5
    auto* assign = dynamic_cast<AssignExpr*>(stmt->expression.get());
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->name, "x");
    auto* bin = dynamic_cast<BinaryExpr*>(assign->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "+");
}

TEST(ParserTest, PostfixIncrement) {
    auto program = parse("fn main() { var x = 0; x++; }");
    auto* stmt = dynamic_cast<ExprStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(stmt, nullptr);
    auto* post = dynamic_cast<PostfixExpr*>(stmt->expression.get());
    ASSERT_NE(post, nullptr);
    EXPECT_EQ(post->op, "++");
}

TEST(ParserTest, PrefixDecrement) {
    auto program = parse("fn main() { var x = 0; --x; }");
    auto* stmt = dynamic_cast<ExprStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(stmt, nullptr);
    auto* pre = dynamic_cast<PrefixExpr*>(stmt->expression.get());
    ASSERT_NE(pre, nullptr);
    EXPECT_EQ(pre->op, "--");
}

TEST(ParserTest, ArrayLiteral) {
    auto program = parse("fn main() { return [1, 2, 3]; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* arr = dynamic_cast<ArrayExpr*>(ret->value.get());
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->elements.size(), 3u);
}

TEST(ParserTest, EmptyArray) {
    auto program = parse("fn main() { return []; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* arr = dynamic_cast<ArrayExpr*>(ret->value.get());
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(arr->elements.size(), 0u);
}

TEST(ParserTest, IndexExpr) {
    auto program = parse("fn main() { var a = [1]; return a[0]; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[1].get());
    auto* idx = dynamic_cast<IndexExpr*>(ret->value.get());
    ASSERT_NE(idx, nullptr);
}

// ---------------------------------------------------------------------------
// Control flow statements
// ---------------------------------------------------------------------------

TEST(ParserTest, IfStmt) {
    auto program = parse("fn main() { if (1) { return 1; } }");
    auto* ifStmt = dynamic_cast<IfStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(ifStmt, nullptr);
    EXPECT_NE(ifStmt->condition, nullptr);
    EXPECT_NE(ifStmt->thenBranch, nullptr);
    EXPECT_EQ(ifStmt->elseBranch, nullptr);
}

TEST(ParserTest, IfElseStmt) {
    auto program = parse("fn main() { if (1) { return 1; } else { return 0; } }");
    auto* ifStmt = dynamic_cast<IfStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(ifStmt, nullptr);
    EXPECT_NE(ifStmt->elseBranch, nullptr);
}

TEST(ParserTest, WhileStmt) {
    auto program = parse("fn main() { while (1) { break; } }");
    auto* whileStmt = dynamic_cast<WhileStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(whileStmt, nullptr);
}

TEST(ParserTest, DoWhileStmt) {
    auto program = parse("fn main() { do { break; } while (0); }");
    auto* doWhile = dynamic_cast<DoWhileStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(doWhile, nullptr);
}

TEST(ParserTest, ForStmt) {
    auto program = parse("fn main() { for (i in 0...10) { continue; } }");
    auto* forStmt = dynamic_cast<ForStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(forStmt, nullptr);
    EXPECT_EQ(forStmt->iteratorVar, "i");
    EXPECT_EQ(forStmt->step, nullptr);
}

TEST(ParserTest, ForStmtWithStep) {
    auto program = parse("fn main() { for (i in 0...10...2) { continue; } }");
    auto* forStmt = dynamic_cast<ForStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(forStmt, nullptr);
    EXPECT_NE(forStmt->step, nullptr);
}

TEST(ParserTest, BreakStmt) {
    auto program = parse("fn main() { while (1) { break; } }");
    auto* whileStmt = dynamic_cast<WhileStmt*>(program->functions[0]->body->statements[0].get());
    auto* body = dynamic_cast<BlockStmt*>(whileStmt->body.get());
    ASSERT_NE(body, nullptr);
    auto* brk = dynamic_cast<BreakStmt*>(body->statements[0].get());
    ASSERT_NE(brk, nullptr);
}

TEST(ParserTest, ContinueStmt) {
    auto program = parse("fn main() { while (1) { continue; } }");
    auto* whileStmt = dynamic_cast<WhileStmt*>(program->functions[0]->body->statements[0].get());
    auto* body = dynamic_cast<BlockStmt*>(whileStmt->body.get());
    ASSERT_NE(body, nullptr);
    auto* cont = dynamic_cast<ContinueStmt*>(body->statements[0].get());
    ASSERT_NE(cont, nullptr);
}

TEST(ParserTest, BlockStmt) {
    auto program = parse("fn main() { { var x = 1; } }");
    auto* block = dynamic_cast<BlockStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->statements.size(), 1u);
}

// ---------------------------------------------------------------------------
// OPTMAX
// ---------------------------------------------------------------------------

TEST(ParserTest, OptmaxFunction) {
    auto program = parse("OPTMAX=: fn foo(x: int) { return x; } OPTMAX!:");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_TRUE(program->functions[0]->isOptMax);
    EXPECT_EQ(program->functions[0]->parameters[0].typeName, "int");
}

// ---------------------------------------------------------------------------
// Operator precedence
// ---------------------------------------------------------------------------

TEST(ParserTest, MulBeforeAdd) {
    // 1 + 2 * 3 should parse as 1 + (2 * 3)
    auto program = parse("fn main() { return 1 + 2 * 3; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(ret->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "+");
    auto* rhs = dynamic_cast<BinaryExpr*>(bin->right.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->op, "*");
}

// ---------------------------------------------------------------------------
// Parenthesized expression
// ---------------------------------------------------------------------------

TEST(ParserTest, ParenthesizedExpr) {
    auto program = parse("fn main() { return (1 + 2) * 3; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(ret->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "*");
}

// ---------------------------------------------------------------------------
// Parse errors
// ---------------------------------------------------------------------------

TEST(ParserTest, MissingSemicolon) {
    EXPECT_THROW(parse("fn main() { return 0 }"), std::runtime_error);
}

TEST(ParserTest, MissingFunction) {
    EXPECT_THROW(parse("return 0;"), std::runtime_error);
}

TEST(ParserTest, MissingClosingParen) {
    EXPECT_THROW(parse("fn main( { }"), std::runtime_error);
}

TEST(ParserTest, MissingClosingBrace) {
    EXPECT_THROW(parse("fn main() {"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Logical operators
// ---------------------------------------------------------------------------

TEST(ParserTest, LogicalOr) {
    auto program = parse("fn main() { return 1 || 0; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(ret->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "||");
}

TEST(ParserTest, LogicalAnd) {
    auto program = parse("fn main() { return 1 && 0; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(ret->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "&&");
}

// ---------------------------------------------------------------------------
// Shift / bitwise
// ---------------------------------------------------------------------------

TEST(ParserTest, BitwiseOps) {
    auto program = parse("fn main() { return 1 & 2 | 3 ^ 4; }");
    // just check it parses without error
    ASSERT_EQ(program->functions.size(), 1u);
}

TEST(ParserTest, ShiftOps) {
    auto program = parse("fn main() { return 1 << 2 >> 3; }");
    ASSERT_EQ(program->functions.size(), 1u);
}

// ---------------------------------------------------------------------------
// Comparison operators
// ---------------------------------------------------------------------------

TEST(ParserTest, ComparisonOps) {
    auto program = parse("fn main() { return 1 < 2; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(ret->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "<");
}

// ---------------------------------------------------------------------------
// Nested expression
// ---------------------------------------------------------------------------

TEST(ParserTest, NestedCallInBinary) {
    auto program = parse("fn main() { return foo(1) + bar(2); }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[0].get());
    auto* bin = dynamic_cast<BinaryExpr*>(ret->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "+");
}

// ---------------------------------------------------------------------------
// OPTMAX errors
// ---------------------------------------------------------------------------

TEST(ParserTest, NestedOptmaxBlocks) {
    EXPECT_THROW(parse("OPTMAX=: OPTMAX=: fn foo(x: int) { return x; } OPTMAX!: OPTMAX!:"), std::runtime_error);
}

TEST(ParserTest, OptmaxEndWithoutStart) {
    EXPECT_THROW(parse("OPTMAX!:"), std::runtime_error);
}

TEST(ParserTest, UnterminatedOptmax) {
    EXPECT_THROW(parse("OPTMAX=: fn foo(x: int) { return x; }"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// OPTMAX parameter / variable type annotation errors
// ---------------------------------------------------------------------------

TEST(ParserTest, OptmaxParamMissingType) {
    EXPECT_THROW(parse("OPTMAX=: fn foo(x) { return x; } OPTMAX!:"), std::runtime_error);
}

TEST(ParserTest, OptmaxVarMissingType) {
    EXPECT_THROW(parse("OPTMAX=: fn foo(x: int) { var y = 5; } OPTMAX!:"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// For loop iterator with type annotation
// ---------------------------------------------------------------------------

TEST(ParserTest, ForLoopIteratorTypeAnnotation) {
    auto program = parse("fn main() { for (i: int in 0...10) { continue; } }");
    auto* forStmt = dynamic_cast<ForStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(forStmt, nullptr);
    EXPECT_EQ(forStmt->iteratorVar, "i");
    EXPECT_EQ(forStmt->iteratorType, "int");
}

TEST(ParserTest, OptmaxForLoopVarMissingType) {
    EXPECT_THROW(parse("OPTMAX=: fn foo(x: int) { for (i in 0...10) { } } OPTMAX!:"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Invalid assignment target
// ---------------------------------------------------------------------------

TEST(ParserTest, InvalidAssignmentTarget) {
    EXPECT_THROW(parse("fn main() { 1 = 5; }"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Slash-assign and percent-assign compound operators
// ---------------------------------------------------------------------------

TEST(ParserTest, SlashAssign) {
    auto program = parse("fn main() { var x = 10; x /= 2; }");
    auto* stmt = dynamic_cast<ExprStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(stmt, nullptr);
    auto* assign = dynamic_cast<AssignExpr*>(stmt->expression.get());
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->name, "x");
    auto* bin = dynamic_cast<BinaryExpr*>(assign->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "/");
}

TEST(ParserTest, PercentAssign) {
    auto program = parse("fn main() { var x = 10; x %= 3; }");
    auto* stmt = dynamic_cast<ExprStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(stmt, nullptr);
    auto* assign = dynamic_cast<AssignExpr*>(stmt->expression.get());
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->name, "x");
    auto* bin = dynamic_cast<BinaryExpr*>(assign->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "%");
}

// ---------------------------------------------------------------------------
// Invalid compound assignment target
// ---------------------------------------------------------------------------

TEST(ParserTest, InvalidCompoundAssignmentTarget) {
    EXPECT_THROW(parse("fn main() { 1 += 5; }"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Invalid function call (non-identifier callee)
// ---------------------------------------------------------------------------

TEST(ParserTest, InvalidFunctionCall) {
    EXPECT_THROW(parse("fn main() { (1+2)(3); }"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Expected expression error
// ---------------------------------------------------------------------------

TEST(ParserTest, ExpectedExpressionError) {
    EXPECT_THROW(parse("fn main() { var x = ; }"), std::runtime_error);
}
