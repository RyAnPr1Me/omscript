#include "ast.h"
#include "lexer.h"
#include "parser.h"
#include <gtest/gtest.h>

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

TEST(ParserTest, EmptyTokenVectorDoesNotCrash) {
    std::vector<Token> tokens;
    Parser parser(tokens);
    EXPECT_NO_THROW({
        auto program = parser.parse();
        ASSERT_NE(program, nullptr);
        EXPECT_TRUE(program->functions.empty());
    });
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

TEST(ParserTest, MissingSemicolonMessage) {
    try {
        parse("fn main() { return 0 }");
        FAIL() << "Expected parser error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("Expected ';'"), std::string::npos);
    }
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

// ---------------------------------------------------------------------------
// Switch statement parsing
// ---------------------------------------------------------------------------

TEST(ParserTest, SwitchStatement) {
    auto program = parse("fn main() { switch (x) { case 1: return 10; case 2: return 20; default: return 0; } }");
    EXPECT_EQ(program->functions.size(), 1u);
}

TEST(ParserTest, SwitchNoCases) {
    auto program = parse("fn main() { switch (x) { } }");
    EXPECT_EQ(program->functions.size(), 1u);
}

TEST(ParserTest, SwitchDuplicateDefault) {
    EXPECT_THROW(parse("fn main() { switch (x) { default: return 1; default: return 2; } }"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Array element assignment
// ---------------------------------------------------------------------------

TEST(ParserTest, ArrayIndexAssignment) {
    auto program = parse("fn main() { var arr = [1, 2, 3]; arr[0] = 42; }");
    EXPECT_EQ(program->functions.size(), 1u);
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 2u);
    // Second statement is an expression statement containing IndexAssignExpr
    auto* exprStmt = dynamic_cast<omscript::ExprStmt*>(stmts[1].get());
    ASSERT_NE(exprStmt, nullptr);
    EXPECT_EQ(exprStmt->expression->type, omscript::ASTNodeType::INDEX_ASSIGN_EXPR);
}

TEST(ParserTest, ArrayIndexAssignmentInvalid) {
    // Cannot assign to a non-identifier, non-index target
    EXPECT_THROW(parse("fn main() { 42 = 1; }"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// For-each loop parsing
// ---------------------------------------------------------------------------

TEST(ParserTest, ForEachLoop) {
    auto program = parse("fn main() { var arr = [1, 2, 3]; for (x in arr) { return x; } }");
    EXPECT_EQ(program->functions.size(), 1u);
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 2u);
    EXPECT_EQ(stmts[1]->type, omscript::ASTNodeType::FOR_EACH_STMT);
}

TEST(ParserTest, ForEachVsRangeFor) {
    // Range-based for should still work
    auto program = parse("fn main() { for (i in 0...10) { return i; } }");
    EXPECT_EQ(program->functions.size(), 1u);
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 1u);
    EXPECT_EQ(stmts[0]->type, omscript::ASTNodeType::FOR_STMT);
}

// ---------------------------------------------------------------------------
// Multi-variable declarations
// ---------------------------------------------------------------------------

TEST(ParserTest, MultiVarDeclaration) {
    auto program = parse("fn main() { var a = 1, b = 2, c = 3; }");
    EXPECT_EQ(program->functions.size(), 1u);
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_EQ(stmts.size(), 3u);
    EXPECT_EQ(stmts[0]->type, omscript::ASTNodeType::VAR_DECL);
    EXPECT_EQ(stmts[1]->type, omscript::ASTNodeType::VAR_DECL);
    EXPECT_EQ(stmts[2]->type, omscript::ASTNodeType::VAR_DECL);
    auto* a = dynamic_cast<omscript::VarDecl*>(stmts[0].get());
    auto* b = dynamic_cast<omscript::VarDecl*>(stmts[1].get());
    auto* c = dynamic_cast<omscript::VarDecl*>(stmts[2].get());
    EXPECT_EQ(a->name, "a");
    EXPECT_EQ(b->name, "b");
    EXPECT_EQ(c->name, "c");
    EXPECT_FALSE(a->isConst);
}

TEST(ParserTest, MultiConstDeclaration) {
    auto program = parse("fn main() { const x = 10, y = 20; }");
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_EQ(stmts.size(), 2u);
    auto* x = dynamic_cast<omscript::VarDecl*>(stmts[0].get());
    auto* y = dynamic_cast<omscript::VarDecl*>(stmts[1].get());
    EXPECT_EQ(x->name, "x");
    EXPECT_TRUE(x->isConst);
    EXPECT_EQ(y->name, "y");
    EXPECT_TRUE(y->isConst);
}

TEST(ParserTest, SingleVarStillWorks) {
    auto program = parse("fn main() { var x = 42; }");
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_EQ(stmts.size(), 1u);
    EXPECT_EQ(stmts[0]->type, omscript::ASTNodeType::VAR_DECL);
}

// ---------------------------------------------------------------------------
// Multi-error reporting
// ---------------------------------------------------------------------------

TEST(ParserTest, MultipleErrors) {
    // Two bad functions + one good one — should report both errors
    try {
        parse("fn a() { return } fn b() { return } fn main() { return 0; }");
        FAIL() << "Expected exception for multiple errors";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        // Should contain error references from both bad functions
        EXPECT_NE(msg.find("line 1"), std::string::npos);
        // Should contain at least 2 "Parse error" occurrences
        size_t first = msg.find("Parse error");
        EXPECT_NE(first, std::string::npos);
        size_t second = msg.find("Parse error", first + 1);
        EXPECT_NE(second, std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Postfix/prefix operator validation
// ---------------------------------------------------------------------------

TEST(ParserTest, PostfixIncrementOnLiteral) {
    EXPECT_THROW(parse("fn main() { 5++; return 0; }"), std::runtime_error);
}

TEST(ParserTest, PostfixDecrementOnLiteral) {
    EXPECT_THROW(parse("fn main() { 5--; return 0; }"), std::runtime_error);
}

TEST(ParserTest, PrefixIncrementOnLiteral) {
    EXPECT_THROW(parse("fn main() { ++5; return 0; }"), std::runtime_error);
}

TEST(ParserTest, PrefixDecrementOnLiteral) {
    EXPECT_THROW(parse("fn main() { --5; return 0; }"), std::runtime_error);
}

TEST(ParserTest, PostfixOnIdentifierSucceeds) {
    auto program = parse("fn main() { var x = 0; x++; return x; }");
    ASSERT_EQ(program->functions.size(), 1u);
}

TEST(ParserTest, PrefixOnIdentifierSucceeds) {
    auto program = parse("fn main() { var x = 0; ++x; return x; }");
    ASSERT_EQ(program->functions.size(), 1u);
}

TEST(ParserTest, PostfixOnParenExprSucceeds) {
    // (a)++ is valid because parenthesized (a) resolves to identifier a
    auto program = parse("fn main() { var a = 1; (a)++; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
}

TEST(ParserTest, PrefixOnParenExprSucceeds) {
    // ++(a) is valid because parenthesized (a) resolves to identifier a
    auto program = parse("fn main() { var a = 1; ++(a); return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
}

// ---------------------------------------------------------------------------
// Boolean and null literals
// ---------------------------------------------------------------------------

TEST(ParserTest, TrueLiteral) {
    auto program = parse("fn main() { return true; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto& body = program->functions[0]->body->statements;
    ASSERT_EQ(body.size(), 1u);
    auto* retStmt = dynamic_cast<ReturnStmt*>(body[0].get());
    ASSERT_NE(retStmt, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(retStmt->value.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->literalType, LiteralExpr::LiteralType::INTEGER);
    EXPECT_EQ(lit->intValue, 1);
}

TEST(ParserTest, FalseLiteral) {
    auto program = parse("fn main() { return false; }");
    auto& body = program->functions[0]->body->statements;
    auto* retStmt = dynamic_cast<ReturnStmt*>(body[0].get());
    auto* lit = dynamic_cast<LiteralExpr*>(retStmt->value.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->intValue, 0);
}

TEST(ParserTest, NullLiteral) {
    auto program = parse("fn main() { return null; }");
    auto& body = program->functions[0]->body->statements;
    auto* retStmt = dynamic_cast<ReturnStmt*>(body[0].get());
    auto* lit = dynamic_cast<LiteralExpr*>(retStmt->value.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->intValue, 0);
}

TEST(ParserTest, BoolInCondition) {
    auto program = parse("fn main() { if (true) { return 1; } return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
}

TEST(ParserTest, BoolInVarDecl) {
    auto program = parse("fn main() { var x = true; var y = false; var z = null; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto& body = program->functions[0]->body->statements;
    ASSERT_GE(body.size(), 4u);
}

// ---------------------------------------------------------------------------
// Bitwise compound assignment
// ---------------------------------------------------------------------------

TEST(ParserTest, BitwiseAndAssign) {
    auto program = parse("fn main() { var x = 15; x &= 6; return x; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto& body = program->functions[0]->body->statements;
    ASSERT_GE(body.size(), 2u);
    // x &= 6 desugars to x = x & 6
    auto* exprStmt = dynamic_cast<ExprStmt*>(body[1].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* assign = dynamic_cast<AssignExpr*>(exprStmt->expression.get());
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->name, "x");
    auto* binExpr = dynamic_cast<BinaryExpr*>(assign->value.get());
    ASSERT_NE(binExpr, nullptr);
    EXPECT_EQ(binExpr->op, "&");
}

TEST(ParserTest, ShiftAssignLeft) {
    auto program = parse("fn main() { var x = 1; x <<= 3; return x; }");
    auto& body = program->functions[0]->body->statements;
    auto* exprStmt = dynamic_cast<ExprStmt*>(body[1].get());
    auto* assign = dynamic_cast<AssignExpr*>(exprStmt->expression.get());
    ASSERT_NE(assign, nullptr);
    auto* binExpr = dynamic_cast<BinaryExpr*>(assign->value.get());
    ASSERT_NE(binExpr, nullptr);
    EXPECT_EQ(binExpr->op, "<<");
}

// ---------------------------------------------------------------------------
// Array compound assignment
// ---------------------------------------------------------------------------

TEST(ParserTest, ArrayCompoundAssign) {
    auto program = parse("fn main() { var arr = [10, 20]; arr[0] += 5; return arr[0]; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto& body = program->functions[0]->body->statements;
    ASSERT_GE(body.size(), 2u);
    // arr[0] += 5 desugars to arr[0] = arr[0] + 5 → IndexAssignExpr
    auto* exprStmt = dynamic_cast<ExprStmt*>(body[1].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* idxAssign = dynamic_cast<IndexAssignExpr*>(exprStmt->expression.get());
    ASSERT_NE(idxAssign, nullptr);
    // The value should be a BinaryExpr with op "+"
    auto* binExpr = dynamic_cast<BinaryExpr*>(idxAssign->value.get());
    ASSERT_NE(binExpr, nullptr);
    EXPECT_EQ(binExpr->op, "+");
}

TEST(ParserTest, ArrayCompoundAssignWithVarIndex) {
    auto program = parse("fn main() { var arr = [10]; var i = 0; arr[i] += 5; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
}

// ---------------------------------------------------------------------------
// Source location tracking on statement nodes
// ---------------------------------------------------------------------------

TEST(ParserTest, IfStmtHasSourceLocation) {
    auto program = parse("fn main() {\n  if (1) { return 0; }\n}");
    ASSERT_EQ(program->functions.size(), 1u);
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 1u);
    EXPECT_EQ(stmts[0]->type, ASTNodeType::IF_STMT);
    EXPECT_EQ(stmts[0]->line, 2);
    EXPECT_GT(stmts[0]->column, 0);
}

TEST(ParserTest, WhileStmtHasSourceLocation) {
    auto program = parse("fn main() {\n  while (0) {}\n}");
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 1u);
    EXPECT_EQ(stmts[0]->type, ASTNodeType::WHILE_STMT);
    EXPECT_EQ(stmts[0]->line, 2);
}

TEST(ParserTest, ReturnStmtHasSourceLocation) {
    auto program = parse("fn main() {\n  return 0;\n}");
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 1u);
    EXPECT_EQ(stmts[0]->type, ASTNodeType::RETURN_STMT);
    EXPECT_EQ(stmts[0]->line, 2);
}

TEST(ParserTest, BreakStmtHasSourceLocation) {
    auto program = parse("fn main() {\n  while (1) {\n    break;\n  }\n}");
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 1u);
    auto* whileStmt = dynamic_cast<WhileStmt*>(stmts[0].get());
    ASSERT_NE(whileStmt, nullptr);
    auto* body = dynamic_cast<BlockStmt*>(whileStmt->body.get());
    ASSERT_NE(body, nullptr);
    ASSERT_GE(body->statements.size(), 1u);
    EXPECT_EQ(body->statements[0]->type, ASTNodeType::BREAK_STMT);
    EXPECT_EQ(body->statements[0]->line, 3);
}

TEST(ParserTest, ContinueStmtHasSourceLocation) {
    auto program = parse("fn main() {\n  while (1) {\n    continue;\n  }\n}");
    auto& stmts = program->functions[0]->body->statements;
    auto* whileStmt = dynamic_cast<WhileStmt*>(stmts[0].get());
    auto* body = dynamic_cast<BlockStmt*>(whileStmt->body.get());
    ASSERT_GE(body->statements.size(), 1u);
    EXPECT_EQ(body->statements[0]->type, ASTNodeType::CONTINUE_STMT);
    EXPECT_EQ(body->statements[0]->line, 3);
}

TEST(ParserTest, ForStmtHasSourceLocation) {
    auto program = parse("fn main() {\n  for (i in 0...10) {}\n}");
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 1u);
    EXPECT_EQ(stmts[0]->type, ASTNodeType::FOR_STMT);
    EXPECT_EQ(stmts[0]->line, 2);
}

TEST(ParserTest, DoWhileStmtHasSourceLocation) {
    auto program = parse("fn main() {\n  do {} while (0);\n}");
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 1u);
    EXPECT_EQ(stmts[0]->type, ASTNodeType::DO_WHILE_STMT);
    EXPECT_EQ(stmts[0]->line, 2);
}

TEST(ParserTest, SwitchStmtHasSourceLocation) {
    auto program = parse("fn main() {\n  switch (1) { case 1: return 0; }\n}");
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 1u);
    EXPECT_EQ(stmts[0]->type, ASTNodeType::SWITCH_STMT);
    EXPECT_EQ(stmts[0]->line, 2);
}

TEST(ParserTest, ExprStmtHasSourceLocation) {
    auto program = parse("fn main() {\n  print(1);\n}");
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 1u);
    EXPECT_EQ(stmts[0]->type, ASTNodeType::EXPR_STMT);
    EXPECT_EQ(stmts[0]->line, 2);
}

TEST(ParserTest, BlockStmtHasSourceLocation) {
    auto program = parse("fn main() {\n  {\n    return 0;\n  }\n}");
    auto& stmts = program->functions[0]->body->statements;
    ASSERT_GE(stmts.size(), 1u);
    EXPECT_EQ(stmts[0]->type, ASTNodeType::BLOCK);
    EXPECT_EQ(stmts[0]->line, 2);
}

// ===========================================================================
// Edge cases - error handling
// ===========================================================================

TEST(ParserTest, MissingRightParen) {
    EXPECT_THROW(parse("fn main() { (1 + 2; }"), std::runtime_error);
}

TEST(ParserTest, MissingRightBracket) {
    EXPECT_THROW(parse("fn main() { [1, 2; }"), std::runtime_error);
}

TEST(ParserTest, MissingRightBrace) {
    EXPECT_THROW(parse("fn main() { var x = 1;"), std::runtime_error);
}

TEST(ParserTest, EmptyFunctionBody) {
    auto program = parse("fn empty() { }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->name, "empty");
}

TEST(ParserTest, NestedBlocks) {
    auto program = parse("fn main() { { { return 1; } } }");
    auto* block = dynamic_cast<BlockStmt*>(program->functions[0]->body.get());
    ASSERT_NE(block, nullptr);
    ASSERT_EQ(block->statements.size(), 1u);
    EXPECT_EQ(block->statements[0]->type, ASTNodeType::BLOCK);
}

TEST(ParserTest, MultipleStatementsInBlock) {
    auto program = parse("fn main() { var a = 1; var b = 2; return a + b; }");
    auto* block = dynamic_cast<BlockStmt*>(program->functions[0]->body.get());
    ASSERT_NE(block, nullptr);
    EXPECT_EQ(block->statements.size(), 3u);
}

TEST(ParserTest, TernaryWithComplexCondition) {
    auto program = parse("fn main() { var x = 1; return x > 0 ? x : -x; }");
    ASSERT_EQ(program->functions.size(), 1u);
}

TEST(ParserTest, TernaryNested) {
    auto program = parse("fn main() { return true ? (false ? 1 : 2) : 3; }");
    ASSERT_EQ(program->functions.size(), 1u);
}

TEST(ParserTest, ArrayInCondition) {
    auto program = parse("fn main() { var arr = [1, 2, 3]; if (arr[0]) { return 1; } return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
}

TEST(ParserTest, FunctionCallAsArrayIndex) {
    auto program = parse("fn getIdx() { return 0; } fn main() { var arr = [10]; return arr[getIdx()]; }");
    ASSERT_EQ(program->functions.size(), 2u);
}
