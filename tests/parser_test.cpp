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

TEST(ParserTest, FunctionWithReturnType) {
    auto program = parse("fn main() -> int { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->name, "main");
    EXPECT_EQ(program->functions[0]->returnType, "int");
    ASSERT_EQ(program->functions[0]->body->statements.size(), 1u);
}

TEST(ParserTest, FunctionWithParamsAndReturnType) {
    auto program = parse("fn add(a: int, b: int) -> int { return a; }");
    ASSERT_EQ(program->functions.size(), 1u);
    ASSERT_EQ(program->functions[0]->parameters.size(), 2u);
    EXPECT_EQ(program->functions[0]->parameters[0].name, "a");
    EXPECT_EQ(program->functions[0]->parameters[1].name, "b");
    EXPECT_EQ(program->functions[0]->returnType, "int");
    ASSERT_EQ(program->functions[0]->body->statements.size(), 1u);
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
// OPTMAX synchronize regression tests
// An error inside an OPTMAX function must NOT consume OPTMAX!: (which would
// leave the block unterminated) and must NOT produce cascading "Expected 'fn'"
// errors for statement-level keywords inside the broken function body.
// ---------------------------------------------------------------------------

TEST(ParserTest, OptmaxErrorDoesNotLeakUnterminatedBlock) {
    // var without type annotation triggers an error inside the OPTMAX block.
    // The error message should be exactly the annotation error, NOT also
    // "Unterminated OPTMAX block".
    try {
        parse("OPTMAX=: fn foo(x: int) { var y = 5; } OPTMAX!: fn main() { return 0; }");
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("OPTMAX variables must include type annotations"), std::string::npos);
        EXPECT_EQ(msg.find("Unterminated OPTMAX block"), std::string::npos)
            << "synchronize() must not skip OPTMAX!: and leave block unterminated";
    }
}

TEST(ParserTest, OptmaxErrorNoCascadingFnErrors) {
    // An error early in an OPTMAX function body must not produce dozens of
    // "Expected 'fn'" errors for every keyword token in the remainder of the
    // function body.
    try {
        parse("OPTMAX=: fn foo(x: int) { var y = 5; for (i: int in 0...10) { return i; } } OPTMAX!: fn main() { return "
              "0; }");
        FAIL() << "Expected std::runtime_error";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        // The message must contain the true error but must NOT contain cascaded fn errors.
        EXPECT_NE(msg.find("OPTMAX variables must include type annotations"), std::string::npos);
        EXPECT_EQ(msg.find("Expected 'fn'"), std::string::npos)
            << "synchronize() must not stop at statement keywords and cause cascading errors";
    }
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

TEST(ParserTest, SwitchMultiValueCase) {
    auto program = parse("fn main() { switch (x) { case 1, 2, 3: return 10; default: return 0; } }");
    EXPECT_EQ(program->functions.size(), 1u);
    auto* sw = dynamic_cast<SwitchStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(sw, nullptr);
    ASSERT_EQ(sw->cases.size(), 2u);
    // First case should have value (1) + 2 additional values (2, 3)
    EXPECT_FALSE(sw->cases[0].isDefault);
    EXPECT_NE(sw->cases[0].value, nullptr);
    EXPECT_EQ(sw->cases[0].values.size(), 2u);
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

// ---------------------------------------------------------------------------
// Default function parameters
// ---------------------------------------------------------------------------

TEST(ParserTest, DefaultParameterInteger) {
    auto program = parse("fn foo(a, b = 10) { return a + b; }");
    ASSERT_EQ(program->functions.size(), 1u);
    ASSERT_EQ(program->functions[0]->parameters.size(), 2u);
    EXPECT_EQ(program->functions[0]->parameters[0].name, "a");
    EXPECT_EQ(program->functions[0]->parameters[1].name, "b");
    EXPECT_FALSE(program->functions[0]->parameters[0].defaultValue);
    ASSERT_TRUE(program->functions[0]->parameters[1].defaultValue != nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(program->functions[0]->parameters[1].defaultValue.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->literalType, LiteralExpr::LiteralType::INTEGER);
    EXPECT_EQ(lit->intValue, 10);
}

TEST(ParserTest, DefaultParameterString) {
    auto program = parse("fn greet(name, msg = \"Hello\") { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    ASSERT_EQ(program->functions[0]->parameters.size(), 2u);
    ASSERT_TRUE(program->functions[0]->parameters[1].defaultValue != nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(program->functions[0]->parameters[1].defaultValue.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->literalType, LiteralExpr::LiteralType::STRING);
    EXPECT_EQ(lit->stringValue, "Hello");
}

TEST(ParserTest, MultipleDefaultParameters) {
    auto program = parse("fn foo(a, b = 5, c = 100) { return a; }");
    ASSERT_EQ(program->functions.size(), 1u);
    ASSERT_EQ(program->functions[0]->parameters.size(), 3u);
    EXPECT_FALSE(program->functions[0]->parameters[0].defaultValue);
    EXPECT_TRUE(program->functions[0]->parameters[1].defaultValue != nullptr);
    EXPECT_TRUE(program->functions[0]->parameters[2].defaultValue != nullptr);
    EXPECT_EQ(program->functions[0]->requiredParameters(), 1u);
}

TEST(ParserTest, AllDefaultParameters) {
    auto program = parse("fn foo(a = 1, b = 2) { return a + b; }");
    ASSERT_EQ(program->functions.size(), 1u);
    ASSERT_EQ(program->functions[0]->parameters.size(), 2u);
    EXPECT_TRUE(program->functions[0]->parameters[0].defaultValue != nullptr);
    EXPECT_TRUE(program->functions[0]->parameters[1].defaultValue != nullptr);
    EXPECT_EQ(program->functions[0]->requiredParameters(), 0u);
}

TEST(ParserTest, DefaultParameterWithType) {
    auto program = parse("fn foo(a: int, b: int = 10) { return a; }");
    ASSERT_EQ(program->functions.size(), 1u);
    ASSERT_EQ(program->functions[0]->parameters.size(), 2u);
    EXPECT_EQ(program->functions[0]->parameters[1].typeName, "int");
    ASSERT_TRUE(program->functions[0]->parameters[1].defaultValue != nullptr);
}

TEST(ParserTest, DefaultParameterNonDefaultAfterDefaultError) {
    Lexer lexer("fn foo(a = 10, b) { return 0; }");
    auto tokens = lexer.tokenize();
    Parser parser(tokens);
    EXPECT_THROW(parser.parse(), std::runtime_error);
}

// ===========================================================================
// Lambda expression tests
// ===========================================================================

TEST(ParserTest, LambdaDesugaredToFunction) {
    // Lambda |x| x * 2 should be desugared into a generated function
    auto program = parse("fn main() { var f = |x| x * 2; return 0; }");
    // The program should have main + 1 generated lambda function
    ASSERT_GE(program->functions.size(), 2u);
    // The last function should be the lambda
    bool foundLambda = false;
    for (const auto& func : program->functions) {
        if (func->name.find("__lambda_") == 0) {
            foundLambda = true;
            EXPECT_EQ(func->parameters.size(), 1u);
            EXPECT_EQ(func->parameters[0].name, "x");
        }
    }
    EXPECT_TRUE(foundLambda);
}

TEST(ParserTest, LambdaTwoParams) {
    auto program = parse("fn main() { var f = |a, b| a + b; return 0; }");
    ASSERT_GE(program->functions.size(), 2u);
    bool foundLambda = false;
    for (const auto& func : program->functions) {
        if (func->name.find("__lambda_") == 0) {
            foundLambda = true;
            EXPECT_EQ(func->parameters.size(), 2u);
            EXPECT_EQ(func->parameters[0].name, "a");
            EXPECT_EQ(func->parameters[1].name, "b");
        }
    }
    EXPECT_TRUE(foundLambda);
}

TEST(ParserTest, LambdaResultIsStringLiteral) {
    // Lambda should be desugared into a string literal (the function name)
    auto program = parse("fn main() { var f = |x| x; return 0; }");
    auto* varDecl = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(varDecl, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(varDecl->initializer.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->literalType, LiteralExpr::LiteralType::STRING);
    EXPECT_TRUE(lit->stringValue.find("__lambda_") == 0);
}

// ===========================================================================
// Pipe operator tests
// ===========================================================================

TEST(ParserTest, PipeOperator) {
    auto program = parse("fn main() { var x = 5 |> double; return 0; }");
    auto* varDecl = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(varDecl, nullptr);
    auto* pipe = dynamic_cast<PipeExpr*>(varDecl->initializer.get());
    ASSERT_NE(pipe, nullptr);
    EXPECT_EQ(pipe->functionName, "double");
}

TEST(ParserTest, PipeOperatorChain) {
    auto program = parse("fn main() { var x = 5 |> f |> g; return 0; }");
    auto* varDecl = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(varDecl, nullptr);
    auto* pipe = dynamic_cast<PipeExpr*>(varDecl->initializer.get());
    ASSERT_NE(pipe, nullptr);
    EXPECT_EQ(pipe->functionName, "g");
    // The left side should also be a PipeExpr
    auto* innerPipe = dynamic_cast<PipeExpr*>(pipe->left.get());
    ASSERT_NE(innerPipe, nullptr);
    EXPECT_EQ(innerPipe->functionName, "f");
}

// ===========================================================================
// Spread operator tests
// ===========================================================================

TEST(ParserTest, SpreadInArrayLiteral) {
    auto program = parse("fn main() { var a = [1, 2]; var b = [...a, 3]; return 0; }");
    auto* varDecl = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(varDecl, nullptr);
    auto* arr = dynamic_cast<ArrayExpr*>(varDecl->initializer.get());
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->elements.size(), 2u);
    // First element should be a SpreadExpr
    auto* spread = dynamic_cast<SpreadExpr*>(arr->elements[0].get());
    ASSERT_NE(spread, nullptr);
    auto* spreadId = dynamic_cast<IdentifierExpr*>(spread->operand.get());
    ASSERT_NE(spreadId, nullptr);
    EXPECT_EQ(spreadId->name, "a");
    // Second element should be a LiteralExpr
    auto* lit = dynamic_cast<LiteralExpr*>(arr->elements[1].get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->intValue, 3);
}

TEST(ParserTest, MultipleSpreadInArray) {
    auto program = parse("fn main() { var a = [1]; var b = [2]; var c = [...a, ...b]; return 0; }");
    auto* varDecl = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[2].get());
    ASSERT_NE(varDecl, nullptr);
    auto* arr = dynamic_cast<ArrayExpr*>(varDecl->initializer.get());
    ASSERT_NE(arr, nullptr);
    ASSERT_EQ(arr->elements.size(), 2u);
    EXPECT_NE(dynamic_cast<SpreadExpr*>(arr->elements[0].get()), nullptr);
    EXPECT_NE(dynamic_cast<SpreadExpr*>(arr->elements[1].get()), nullptr);
}

// ===========================================================================
// Struct declaration tests
// ===========================================================================

TEST(ParserTest, StructDeclaration) {
    auto program = parse("struct Point { x, y } fn main() { return 0; }");
    ASSERT_EQ(program->structs.size(), 1u);
    EXPECT_EQ(program->structs[0]->name, "Point");
    ASSERT_EQ(program->structs[0]->fields.size(), 2u);
    EXPECT_EQ(program->structs[0]->fields[0], "x");
    EXPECT_EQ(program->structs[0]->fields[1], "y");
}

TEST(ParserTest, StructDeclarationSingleField) {
    auto program = parse("struct Wrapper { value } fn main() { return 0; }");
    ASSERT_EQ(program->structs.size(), 1u);
    EXPECT_EQ(program->structs[0]->name, "Wrapper");
    ASSERT_EQ(program->structs[0]->fields.size(), 1u);
    EXPECT_EQ(program->structs[0]->fields[0], "value");
}

TEST(ParserTest, MultipleStructDeclarations) {
    auto program = parse(
        "struct Point { x, y }"
        "struct Color { r, g, b }"
        "fn main() { return 0; }");
    ASSERT_EQ(program->structs.size(), 2u);
    EXPECT_EQ(program->structs[0]->name, "Point");
    EXPECT_EQ(program->structs[1]->name, "Color");
}

TEST(ParserTest, StructLiteralExpr) {
    auto program = parse(
        "struct Point { x, y }"
        "fn main() { var p = Point { x: 10, y: 20 }; return 0; }");
    ASSERT_EQ(program->structs.size(), 1u);
    auto* varDecl = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(varDecl, nullptr);
    auto* lit = dynamic_cast<StructLiteralExpr*>(varDecl->initializer.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->structName, "Point");
    ASSERT_EQ(lit->fieldValues.size(), 2u);
    EXPECT_EQ(lit->fieldValues[0].first, "x");
    EXPECT_EQ(lit->fieldValues[1].first, "y");
}

TEST(ParserTest, StructFieldAccess) {
    auto program = parse(
        "struct Point { x, y }"
        "fn main() { var p = Point { x: 1, y: 2 }; return p.x; }");
    auto* retStmt = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(retStmt, nullptr);
    auto* fieldAccess = dynamic_cast<FieldAccessExpr*>(retStmt->value.get());
    ASSERT_NE(fieldAccess, nullptr);
    EXPECT_EQ(fieldAccess->fieldName, "x");
}

// ===========================================================================
// Generic function declaration tests
// ===========================================================================

TEST(ParserTest, GenericFunctionNoTypeParams) {
    auto program = parse("fn id(x) { return x; } fn main() { return 0; }");
    ASSERT_EQ(program->functions[0]->name, "id");
    EXPECT_TRUE(program->functions[0]->typeParams.empty());
}

TEST(ParserTest, GenericFunctionSingleTypeParam) {
    auto program = parse("fn identity<T>(x: T) -> T { return x; } fn main() { return 0; }");
    ASSERT_EQ(program->functions[0]->name, "identity");
    ASSERT_EQ(program->functions[0]->typeParams.size(), 1u);
    EXPECT_EQ(program->functions[0]->typeParams[0], "T");
}

TEST(ParserTest, GenericFunctionMultipleTypeParams) {
    auto program = parse("fn pair<A, B>(a: A, b: B) { return a; } fn main() { return 0; }");
    ASSERT_EQ(program->functions[0]->name, "pair");
    ASSERT_EQ(program->functions[0]->typeParams.size(), 2u);
    EXPECT_EQ(program->functions[0]->typeParams[0], "A");
    EXPECT_EQ(program->functions[0]->typeParams[1], "B");
}

// ===========================================================================
// Return type annotation tests
// ===========================================================================

TEST(ParserTest, ReturnTypeStored) {
    auto program = parse("fn main() -> int { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "int");
}

TEST(ParserTest, ReturnTypeVoid) {
    auto program = parse("fn doStuff() -> void { return; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "void");
}

TEST(ParserTest, ReturnTypeFloat) {
    auto program = parse("fn pi() -> float { return 3.14; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "float");
}

TEST(ParserTest, ReturnTypeString) {
    auto program = parse("fn greet() -> string { return \"hi\"; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "string");
}

TEST(ParserTest, ReturnTypeBool) {
    auto program = parse("fn isEven(n: int) -> bool { return true; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "bool");
}

TEST(ParserTest, NoReturnTypeEmpty) {
    auto program = parse("fn main() { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "");
}

TEST(ParserTest, ReturnTypeWithParams) {
    auto program = parse("fn add(a: int, b: int) -> int { return a; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "int");
    EXPECT_EQ(program->functions[0]->parameters[0].typeName, "int");
    EXPECT_EQ(program->functions[0]->parameters[1].typeName, "int");
}

// ===========================================================================
// Array type annotation tests
// ===========================================================================

TEST(ParserTest, ReturnTypeArray) {
    auto program = parse("fn getArr() -> int[] { return [1, 2]; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "int[]");
}

TEST(ParserTest, ReturnTypeStringArray) {
    auto program = parse("fn getNames() -> string[] { return [\"a\"]; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "string[]");
}

TEST(ParserTest, ReturnTypeNestedArray) {
    auto program = parse("fn getMatrix() -> int[][] { return [1]; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "int[][]");
}

TEST(ParserTest, ParamTypeArray) {
    auto program = parse("fn sum(arr: int[]) { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->parameters[0].typeName, "int[]");
}

TEST(ParserTest, ParamTypeStringArray) {
    auto program = parse("fn join(arr: string[]) { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->parameters[0].typeName, "string[]");
}

TEST(ParserTest, ParamTypeNestedArray) {
    auto program = parse("fn flatten(mat: int[][]) { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->parameters[0].typeName, "int[][]");
}

TEST(ParserTest, VarDeclArrayType) {
    auto program = parse("fn main() { var arr: int[] = [1, 2, 3]; }");
    auto* var = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->typeName, "int[]");
}

TEST(ParserTest, VarDeclNestedArrayType) {
    auto program = parse("fn main() { var mat: float[][] = [1]; }");
    auto* var = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(var, nullptr);
    EXPECT_EQ(var->typeName, "float[][]");
}

// ===========================================================================
// Struct type annotation tests
// ===========================================================================

TEST(ParserTest, ReturnTypeStruct) {
    auto program = parse(
        "struct Point { x, y }"
        "fn makePoint() -> Point { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "Point");
}

TEST(ParserTest, ParamTypeStruct) {
    auto program = parse(
        "struct Point { x, y }"
        "fn distance(p: Point) { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->parameters[0].typeName, "Point");
}

TEST(ParserTest, ReturnTypeStructArray) {
    auto program = parse(
        "struct Point { x, y }"
        "fn getPoints() -> Point[] { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->returnType, "Point[]");
}

TEST(ParserTest, ParamTypeStructArray) {
    auto program = parse(
        "struct Point { x, y }"
        "fn sumPoints(pts: Point[]) { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->parameters[0].typeName, "Point[]");
}

// ===========================================================================
// Generic function return type tests
// ===========================================================================

TEST(ParserTest, GenericReturnType) {
    auto program = parse("fn identity<T>(x: T) -> T { return x; } fn main() { return 0; }");
    ASSERT_EQ(program->functions[0]->returnType, "T");
}

TEST(ParserTest, GenericReturnTypeArray) {
    auto program = parse("fn wrap<T>(x: T) -> T[] { return x; } fn main() { return 0; }");
    ASSERT_EQ(program->functions[0]->returnType, "T[]");
}

// ===========================================================================
// Combined param + return type annotation tests
// ===========================================================================

TEST(ParserTest, ArrayParamAndReturnType) {
    auto program = parse("fn sortArr(arr: int[]) -> int[] { return arr; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->parameters[0].typeName, "int[]");
    EXPECT_EQ(program->functions[0]->returnType, "int[]");
}

TEST(ParserTest, MultipleArrayParams) {
    auto program = parse("fn merge(a: int[], b: string[]) -> int { return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    EXPECT_EQ(program->functions[0]->parameters[0].typeName, "int[]");
    EXPECT_EQ(program->functions[0]->parameters[1].typeName, "string[]");
    EXPECT_EQ(program->functions[0]->returnType, "int");
}

TEST(ParserTest, ForLoopIteratorArrayType) {
    auto program = parse("fn main() { for (i: int[] in 0...10) { continue; } }");
    auto* forStmt = dynamic_cast<ForStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(forStmt, nullptr);
    EXPECT_EQ(forStmt->iteratorType, "int[]");
}

// ===========================================================================
// Ownership system parser tests
// ===========================================================================

TEST(ParserTest, InvalidateStatement) {
    auto program = parse("fn main() { var x = 1; invalidate x; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* inv = dynamic_cast<InvalidateStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(inv, nullptr);
    EXPECT_EQ(inv->varName, "x");
}

TEST(ParserTest, MoveDeclaration) {
    auto program = parse("fn main() { var a = 1; move var b = a; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* md = dynamic_cast<MoveDecl*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(md, nullptr);
    EXPECT_EQ(md->name, "b");
    ASSERT_NE(md->initializer, nullptr);
}

TEST(ParserTest, MoveExprInAssignment) {
    auto program = parse("fn main() { var x = 1; var y = move x; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* vd = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(vd, nullptr);
    EXPECT_EQ(vd->name, "y");
    auto* mv = dynamic_cast<MoveExpr*>(vd->initializer.get());
    ASSERT_NE(mv, nullptr);
}

TEST(ParserTest, MoveExprInReturn) {
    auto program = parse("fn main() { var x = 1; return move x; }");
    auto* ret = dynamic_cast<ReturnStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(ret, nullptr);
    auto* mv = dynamic_cast<MoveExpr*>(ret->value.get());
    ASSERT_NE(mv, nullptr);
}

TEST(ParserTest, BorrowDeclaration) {
    auto program = parse("fn main() { var x = 1; borrow var ref = x; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* vd = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(vd, nullptr);
    EXPECT_EQ(vd->name, "ref");
    auto* bw = dynamic_cast<BorrowExpr*>(vd->initializer.get());
    ASSERT_NE(bw, nullptr);
}

TEST(ParserTest, StructFieldAttrsHotCold) {
    auto program = parse("struct Foo { hot int x, cold int y } fn main() { return 0; }");
    ASSERT_EQ(program->structs.size(), 1u);
    ASSERT_EQ(program->structs[0]->fieldDecls.size(), 2u);
    EXPECT_TRUE(program->structs[0]->fieldDecls[0].attrs.hot);
    EXPECT_FALSE(program->structs[0]->fieldDecls[0].attrs.cold);
    EXPECT_FALSE(program->structs[0]->fieldDecls[1].attrs.hot);
    EXPECT_TRUE(program->structs[0]->fieldDecls[1].attrs.cold);
}

TEST(ParserTest, StructFieldAttrsNoalias) {
    auto program = parse("struct Foo { noalias int ptr } fn main() { return 0; }");
    ASSERT_EQ(program->structs[0]->fieldDecls.size(), 1u);
    EXPECT_TRUE(program->structs[0]->fieldDecls[0].attrs.noalias);
}

TEST(ParserTest, StructFieldAttrsImmut) {
    auto program = parse("struct Foo { immut int id } fn main() { return 0; }");
    ASSERT_EQ(program->structs[0]->fieldDecls.size(), 1u);
    EXPECT_TRUE(program->structs[0]->fieldDecls[0].attrs.immut);
}

TEST(ParserTest, StructFieldAttrsAlign) {
    auto program = parse("struct Foo { align(64) int buf } fn main() { return 0; }");
    ASSERT_EQ(program->structs[0]->fieldDecls.size(), 1u);
    EXPECT_EQ(program->structs[0]->fieldDecls[0].attrs.align, 64);
}

TEST(ParserTest, StructFieldAttrsRange) {
    auto program = parse("struct Foo { range(0,100) int score } fn main() { return 0; }");
    ASSERT_EQ(program->structs[0]->fieldDecls.size(), 1u);
    EXPECT_TRUE(program->structs[0]->fieldDecls[0].attrs.hasRange);
    EXPECT_EQ(program->structs[0]->fieldDecls[0].attrs.rangeMin, 0);
    EXPECT_EQ(program->structs[0]->fieldDecls[0].attrs.rangeMax, 100);
}

TEST(ParserTest, StructFieldAttrsMove) {
    auto program = parse("struct Foo { move int inner } fn main() { return 0; }");
    ASSERT_EQ(program->structs[0]->fieldDecls.size(), 1u);
    EXPECT_TRUE(program->structs[0]->fieldDecls[0].attrs.isMove);
}

TEST(ParserTest, StructFieldMultipleAttrs) {
    auto program = parse("struct Foo { hot immut int id, cold noalias int ptr } fn main() { return 0; }");
    ASSERT_EQ(program->structs[0]->fieldDecls.size(), 2u);
    EXPECT_TRUE(program->structs[0]->fieldDecls[0].attrs.hot);
    EXPECT_TRUE(program->structs[0]->fieldDecls[0].attrs.immut);
    EXPECT_TRUE(program->structs[0]->fieldDecls[1].attrs.cold);
    EXPECT_TRUE(program->structs[0]->fieldDecls[1].attrs.noalias);
}

TEST(ParserTest, BorrowWithRefTypeAndAddressOf) {
    // `borrow var j:&i32 = &x;`  — reference type annotation + address-of expr
    auto program = parse("fn main() { var x :i32 = 5; borrow var j:&i32 = &x; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* vd = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(vd, nullptr);
    EXPECT_EQ(vd->name, "j");
    EXPECT_EQ(vd->typeName, "&i32");
    auto* bw = dynamic_cast<BorrowExpr*>(vd->initializer.get());
    ASSERT_NE(bw, nullptr);
    // The source inside BorrowExpr should be a UnaryExpr with op "&"
    auto* unary = dynamic_cast<UnaryExpr*>(bw->source.get());
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op, "&");
}

TEST(ParserTest, AddressOfUnaryOperator) {
    // `&x` parsed as a unary expression
    auto program = parse("fn main() { var x = 1; var y = &x; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* vd = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(vd, nullptr);
    EXPECT_EQ(vd->name, "y");
    auto* unary = dynamic_cast<UnaryExpr*>(vd->initializer.get());
    ASSERT_NE(unary, nullptr);
    EXPECT_EQ(unary->op, "&");
}

TEST(ParserTest, RefTypeAnnotation) {
    // `&i64` reference type annotation in borrow declaration
    auto program = parse("fn main() { var x = 10; borrow var k:&i64 = &x; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* vd = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(vd, nullptr);
    EXPECT_EQ(vd->name, "k");
    EXPECT_EQ(vd->typeName, "&i64");
}

// ---------------------------------------------------------------------------
// Prefetch statement parsing
// ---------------------------------------------------------------------------

TEST(ParserTest, PrefetchVarDecl) {
    auto program = parse("fn main() { prefetch var x:i32 = 5; invalidate x; return 0; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* pf = dynamic_cast<PrefetchStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(pf, nullptr);
    ASSERT_NE(pf->varDecl, nullptr);
    EXPECT_EQ(pf->varDecl->name, "x");
    EXPECT_EQ(pf->varDecl->typeName, "i32");
    EXPECT_FALSE(pf->hintHot);
    EXPECT_FALSE(pf->hintImmut);
}

TEST(ParserTest, PrefetchHotAttr) {
    auto program = parse("fn main() { prefetch hot var v:i64 = 10; invalidate v; return 0; }");
    auto* pf = dynamic_cast<PrefetchStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(pf, nullptr);
    EXPECT_TRUE(pf->hintHot);
    EXPECT_FALSE(pf->hintImmut);
}

TEST(ParserTest, PrefetchImmutAttr) {
    auto program = parse("fn main() { prefetch immut var c:i32 = 3; invalidate c; return 0; }");
    auto* pf = dynamic_cast<PrefetchStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(pf, nullptr);
    EXPECT_FALSE(pf->hintHot);
    EXPECT_TRUE(pf->hintImmut);
}

TEST(ParserTest, PrefetchStandaloneExistingVar) {
    auto program = parse("fn main() { var x = 1; prefetch x; invalidate x; return 0; }");
    auto* pf = dynamic_cast<PrefetchStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(pf, nullptr);
    EXPECT_EQ(pf->varDecl, nullptr);
    EXPECT_EQ(pf->varName, "x");
}

// ---------------------------------------------------------------------------
// **= and ??= compound assignment operators
// ---------------------------------------------------------------------------

TEST(ParserTest, PowerAssignOperator) {
    auto program = parse("fn main() { var x = 2; x **= 3; }");
    ASSERT_EQ(program->functions.size(), 1u);
    // Statement 1 should be an expression statement with an AssignExpr
    auto* exprStmt = dynamic_cast<ExprStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* assign = dynamic_cast<AssignExpr*>(exprStmt->expression.get());
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->name, "x");
    // The RHS should be a binary expr with op "**"
    auto* bin = dynamic_cast<BinaryExpr*>(assign->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "**");
}

TEST(ParserTest, NullCoalesceAssignOperator) {
    auto program = parse("fn main() { var x = 0; x ?\?= 42; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(exprStmt, nullptr);
    auto* assign = dynamic_cast<AssignExpr*>(exprStmt->expression.get());
    ASSERT_NE(assign, nullptr);
    EXPECT_EQ(assign->name, "x");
    auto* bin = dynamic_cast<BinaryExpr*>(assign->value.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "??");
}

// ---------------------------------------------------------------------------
// Struct field compound assignment
// ---------------------------------------------------------------------------

TEST(ParserTest, StructFieldCompoundAssignment) {
    auto program = parse("struct S { x, y }\nfn main() { var s = S { x: 1, y: 2 }; s.x += 5; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* exprStmt = dynamic_cast<ExprStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(exprStmt, nullptr);
    // Should be a FieldAssignExpr
    auto* fieldAssign = dynamic_cast<FieldAssignExpr*>(exprStmt->expression.get());
    ASSERT_NE(fieldAssign, nullptr);
    EXPECT_EQ(fieldAssign->fieldName, "x");
}

// ---------------------------------------------------------------------------
// Register keyword
// ---------------------------------------------------------------------------

TEST(ParserTest, RegisterVarDecl) {
    auto program = parse("fn main() { register var x = 42; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* varDecl = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(varDecl, nullptr);
    EXPECT_EQ(varDecl->name, "x");
    EXPECT_TRUE(varDecl->isRegister);
}

// ---------------------------------------------------------------------------
// SIMD type annotation parsing
// ---------------------------------------------------------------------------

TEST(ParserTest, SimdTypeAnnotation) {
    auto program = parse("fn main() { var v: f32x4 = [1.0, 2.0, 3.0, 4.0]; }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* varDecl = dynamic_cast<VarDecl*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(varDecl, nullptr);
    EXPECT_EQ(varDecl->typeName, "f32x4");
}

// ---------------------------------------------------------------------------
// Pipeline statement parsing
// ---------------------------------------------------------------------------

TEST(ParserTest, PipelineStmtWithCount) {
    // pipeline 5 { stage body { } }
    auto program = parse("fn main() { pipeline 5 { stage body { } } }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* pl = dynamic_cast<PipelineStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(pl, nullptr);
    EXPECT_NE(pl->count, nullptr);
    ASSERT_EQ(pl->stages.size(), 1u);
    EXPECT_EQ(pl->stages[0].name, "body");
}

TEST(ParserTest, PipelineStmtOneShot) {
    // pipeline { stage s { } }  — no count, one-shot
    auto program = parse("fn main() { pipeline { stage s { } } }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* pl = dynamic_cast<PipelineStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(pl, nullptr);
    EXPECT_EQ(pl->count, nullptr);
    ASSERT_EQ(pl->stages.size(), 1u);
    EXPECT_EQ(pl->stages[0].name, "s");
}

TEST(ParserTest, PipelineStmtMultipleStages) {
    auto program = parse(
        "fn main() { pipeline 10 { stage load { } stage compute { } stage store { } } }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* pl = dynamic_cast<PipelineStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(pl, nullptr);
    ASSERT_EQ(pl->stages.size(), 3u);
    EXPECT_EQ(pl->stages[0].name, "load");
    EXPECT_EQ(pl->stages[1].name, "compute");
    EXPECT_EQ(pl->stages[2].name, "store");
}

TEST(ParserTest, PipelineStmtExprCount) {
    // Count can be any expression, including a variable reference.
    auto program = parse("fn main() { var n = 8; pipeline n { stage s { } } }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* pl = dynamic_cast<PipelineStmt*>(program->functions[0]->body->statements[1].get());
    ASSERT_NE(pl, nullptr);
    EXPECT_NE(pl->count, nullptr);
    // count should parse as an identifier expression "n"
    auto* ident = dynamic_cast<IdentifierExpr*>(pl->count.get());
    ASSERT_NE(ident, nullptr);
    EXPECT_EQ(ident->name, "n");
}

TEST(ParserTest, PipelineStmtCountLiteralIsIntExpr) {
    auto program = parse("fn main() { pipeline 3 { stage s { } } }");
    ASSERT_EQ(program->functions.size(), 1u);
    auto* pl = dynamic_cast<PipelineStmt*>(program->functions[0]->body->statements[0].get());
    ASSERT_NE(pl, nullptr);
    auto* lit = dynamic_cast<LiteralExpr*>(pl->count.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->intValue, 3);
}
