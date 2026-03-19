#include "ast.h"
#include <gtest/gtest.h>

using namespace omscript;

// ---------------------------------------------------------------------------
// ASTNode types
// ---------------------------------------------------------------------------

TEST(ASTTest, LiteralExprInteger) {
    LiteralExpr lit(42LL);
    EXPECT_EQ(lit.type, ASTNodeType::LITERAL_EXPR);
    EXPECT_EQ(lit.literalType, LiteralExpr::LiteralType::INTEGER);
    EXPECT_EQ(lit.intValue, 42);
}

TEST(ASTTest, LiteralExprFloat) {
    LiteralExpr lit(3.14);
    EXPECT_EQ(lit.type, ASTNodeType::LITERAL_EXPR);
    EXPECT_EQ(lit.literalType, LiteralExpr::LiteralType::FLOAT);
    EXPECT_DOUBLE_EQ(lit.floatValue, 3.14);
}

TEST(ASTTest, LiteralExprString) {
    LiteralExpr lit(std::string("hello"));
    EXPECT_EQ(lit.type, ASTNodeType::LITERAL_EXPR);
    EXPECT_EQ(lit.literalType, LiteralExpr::LiteralType::STRING);
    EXPECT_EQ(lit.stringValue, "hello");
}

TEST(ASTTest, IdentifierExpr) {
    IdentifierExpr id("myVar");
    EXPECT_EQ(id.type, ASTNodeType::IDENTIFIER_EXPR);
    EXPECT_EQ(id.name, "myVar");
}

TEST(ASTTest, BinaryExpr) {
    auto left = std::make_unique<LiteralExpr>(1LL);
    auto right = std::make_unique<LiteralExpr>(2LL);
    BinaryExpr bin("+", std::move(left), std::move(right));
    EXPECT_EQ(bin.type, ASTNodeType::BINARY_EXPR);
    EXPECT_EQ(bin.op, "+");
}

TEST(ASTTest, UnaryExpr) {
    auto operand = std::make_unique<LiteralExpr>(5LL);
    UnaryExpr un("-", std::move(operand));
    EXPECT_EQ(un.type, ASTNodeType::UNARY_EXPR);
    EXPECT_EQ(un.op, "-");
}

TEST(ASTTest, CallExpr) {
    std::vector<std::unique_ptr<Expression>> args;
    args.push_back(std::make_unique<LiteralExpr>(1LL));
    CallExpr call("foo", std::move(args));
    EXPECT_EQ(call.type, ASTNodeType::CALL_EXPR);
    EXPECT_EQ(call.callee, "foo");
    EXPECT_EQ(call.arguments.size(), 1u);
}

TEST(ASTTest, AssignExpr) {
    auto value = std::make_unique<LiteralExpr>(42LL);
    AssignExpr assign("x", std::move(value));
    EXPECT_EQ(assign.type, ASTNodeType::ASSIGN_EXPR);
    EXPECT_EQ(assign.name, "x");
}

TEST(ASTTest, PostfixExpr) {
    auto operand = std::make_unique<IdentifierExpr>("x");
    PostfixExpr post("++", std::move(operand));
    EXPECT_EQ(post.type, ASTNodeType::POSTFIX_EXPR);
    EXPECT_EQ(post.op, "++");
}

TEST(ASTTest, PrefixExpr) {
    auto operand = std::make_unique<IdentifierExpr>("x");
    PrefixExpr pre("--", std::move(operand));
    EXPECT_EQ(pre.type, ASTNodeType::PREFIX_EXPR);
    EXPECT_EQ(pre.op, "--");
}

TEST(ASTTest, TernaryExpr) {
    auto cond = std::make_unique<LiteralExpr>(1LL);
    auto thenE = std::make_unique<LiteralExpr>(2LL);
    auto elseE = std::make_unique<LiteralExpr>(3LL);
    TernaryExpr tern(std::move(cond), std::move(thenE), std::move(elseE));
    EXPECT_EQ(tern.type, ASTNodeType::TERNARY_EXPR);
}

TEST(ASTTest, ArrayExpr) {
    std::vector<std::unique_ptr<Expression>> elems;
    elems.push_back(std::make_unique<LiteralExpr>(1LL));
    elems.push_back(std::make_unique<LiteralExpr>(2LL));
    ArrayExpr arr(std::move(elems));
    EXPECT_EQ(arr.type, ASTNodeType::ARRAY_EXPR);
    EXPECT_EQ(arr.elements.size(), 2u);
}

TEST(ASTTest, IndexExpr) {
    auto array = std::make_unique<IdentifierExpr>("arr");
    auto index = std::make_unique<LiteralExpr>(0LL);
    IndexExpr idx(std::move(array), std::move(index));
    EXPECT_EQ(idx.type, ASTNodeType::INDEX_EXPR);
}

// ---------------------------------------------------------------------------
// Statement types
// ---------------------------------------------------------------------------

TEST(ASTTest, ExprStmt) {
    auto expr = std::make_unique<LiteralExpr>(42LL);
    ExprStmt stmt(std::move(expr));
    EXPECT_EQ(stmt.type, ASTNodeType::EXPR_STMT);
}

TEST(ASTTest, VarDecl) {
    auto init = std::make_unique<LiteralExpr>(5LL);
    VarDecl var("x", std::move(init), false, "int");
    EXPECT_EQ(var.type, ASTNodeType::VAR_DECL);
    EXPECT_EQ(var.name, "x");
    EXPECT_FALSE(var.isConst);
    EXPECT_EQ(var.typeName, "int");
}

TEST(ASTTest, VarDeclConst) {
    VarDecl var("y", nullptr, true);
    EXPECT_TRUE(var.isConst);
    EXPECT_EQ(var.initializer, nullptr);
}

TEST(ASTTest, ReturnStmt) {
    auto value = std::make_unique<LiteralExpr>(0LL);
    ReturnStmt ret(std::move(value));
    EXPECT_EQ(ret.type, ASTNodeType::RETURN_STMT);
}

TEST(ASTTest, IfStmt) {
    auto cond = std::make_unique<LiteralExpr>(1LL);
    std::vector<std::unique_ptr<Statement>> stmts;
    auto body = std::make_unique<BlockStmt>(std::move(stmts));
    IfStmt ifStmt(std::move(cond), std::move(body));
    EXPECT_EQ(ifStmt.type, ASTNodeType::IF_STMT);
    EXPECT_EQ(ifStmt.elseBranch, nullptr);
}

TEST(ASTTest, WhileStmt) {
    auto cond = std::make_unique<LiteralExpr>(1LL);
    std::vector<std::unique_ptr<Statement>> stmts;
    auto body = std::make_unique<BlockStmt>(std::move(stmts));
    WhileStmt whileStmt(std::move(cond), std::move(body));
    EXPECT_EQ(whileStmt.type, ASTNodeType::WHILE_STMT);
}

TEST(ASTTest, DoWhileStmt) {
    std::vector<std::unique_ptr<Statement>> stmts;
    auto body = std::make_unique<BlockStmt>(std::move(stmts));
    auto cond = std::make_unique<LiteralExpr>(1LL);
    DoWhileStmt doWhile(std::move(body), std::move(cond));
    EXPECT_EQ(doWhile.type, ASTNodeType::DO_WHILE_STMT);
}

TEST(ASTTest, ForStmt) {
    auto start = std::make_unique<LiteralExpr>(0LL);
    auto end = std::make_unique<LiteralExpr>(10LL);
    std::vector<std::unique_ptr<Statement>> stmts;
    auto body = std::make_unique<BlockStmt>(std::move(stmts));
    ForStmt forStmt("i", std::move(start), std::move(end), nullptr, std::move(body));
    EXPECT_EQ(forStmt.type, ASTNodeType::FOR_STMT);
    EXPECT_EQ(forStmt.iteratorVar, "i");
    EXPECT_EQ(forStmt.step, nullptr);
}

TEST(ASTTest, BreakStmt) {
    BreakStmt brk;
    EXPECT_EQ(brk.type, ASTNodeType::BREAK_STMT);
}

TEST(ASTTest, ContinueStmt) {
    ContinueStmt cont;
    EXPECT_EQ(cont.type, ASTNodeType::CONTINUE_STMT);
}

TEST(ASTTest, BlockStmt) {
    std::vector<std::unique_ptr<Statement>> stmts;
    stmts.push_back(std::make_unique<BreakStmt>());
    BlockStmt block(std::move(stmts));
    EXPECT_EQ(block.type, ASTNodeType::BLOCK);
    EXPECT_EQ(block.statements.size(), 1u);
}

// ---------------------------------------------------------------------------
// Top-level types
// ---------------------------------------------------------------------------

TEST(ASTTest, Parameter) {
    Parameter p("x", "int");
    EXPECT_EQ(p.name, "x");
    EXPECT_EQ(p.typeName, "int");
}

TEST(ASTTest, ParameterNoType) {
    Parameter p("y");
    EXPECT_EQ(p.name, "y");
    EXPECT_EQ(p.typeName, "");
}

TEST(ASTTest, FunctionDecl) {
    std::vector<Parameter> params;
    params.emplace_back("a");
    std::vector<std::unique_ptr<Statement>> stmts;
    auto body = std::make_unique<BlockStmt>(std::move(stmts));
    FunctionDecl func("test", {}, std::move(params), std::move(body), false);
    EXPECT_EQ(func.type, ASTNodeType::FUNCTION);
    EXPECT_EQ(func.name, "test");
    EXPECT_FALSE(func.isOptMax);
    EXPECT_EQ(func.returnType, "");
}

TEST(ASTTest, FunctionDeclWithReturnType) {
    std::vector<Parameter> params;
    std::vector<std::unique_ptr<Statement>> stmts;
    auto body = std::make_unique<BlockStmt>(std::move(stmts));
    FunctionDecl func("test", {}, std::move(params), std::move(body), false, "int");
    EXPECT_EQ(func.returnType, "int");
}

TEST(ASTTest, FunctionDeclWithArrayReturnType) {
    std::vector<Parameter> params;
    std::vector<std::unique_ptr<Statement>> stmts;
    auto body = std::make_unique<BlockStmt>(std::move(stmts));
    FunctionDecl func("test", {}, std::move(params), std::move(body), false, "int[]");
    EXPECT_EQ(func.returnType, "int[]");
}

TEST(ASTTest, FunctionDeclOptMax) {
    std::vector<Parameter> params;
    std::vector<std::unique_ptr<Statement>> stmts;
    auto body = std::make_unique<BlockStmt>(std::move(stmts));
    FunctionDecl func("test", {}, std::move(params), std::move(body), true);
    EXPECT_TRUE(func.isOptMax);
}

TEST(ASTTest, Program) {
    std::vector<std::unique_ptr<FunctionDecl>> funcs;
    Program prog(std::move(funcs));
    EXPECT_EQ(prog.type, ASTNodeType::PROGRAM);
    EXPECT_TRUE(prog.functions.empty());
}

TEST(ASTTest, LineColumn) {
    LiteralExpr lit(42LL);
    lit.line = 10;
    lit.column = 5;
    EXPECT_EQ(lit.line, 10);
    EXPECT_EQ(lit.column, 5);
}

// ===========================================================================
// Ownership AST node tests
// ===========================================================================

TEST(ASTTest, MoveExpr) {
    auto src = std::make_unique<IdentifierExpr>("x");
    MoveExpr move(std::move(src));
    EXPECT_EQ(move.type, ASTNodeType::MOVE_EXPR);
    ASSERT_NE(move.source, nullptr);
    auto* id = dynamic_cast<IdentifierExpr*>(move.source.get());
    ASSERT_NE(id, nullptr);
    EXPECT_EQ(id->name, "x");
}

TEST(ASTTest, BorrowExpr) {
    auto src = std::make_unique<IdentifierExpr>("y");
    BorrowExpr borrow(std::move(src));
    EXPECT_EQ(borrow.type, ASTNodeType::BORROW_EXPR);
    ASSERT_NE(borrow.source, nullptr);
}

TEST(ASTTest, InvalidateStmt) {
    InvalidateStmt inv("z");
    EXPECT_EQ(inv.type, ASTNodeType::INVALIDATE_STMT);
    EXPECT_EQ(inv.varName, "z");
}

TEST(ASTTest, MoveDecl) {
    auto init = std::make_unique<IdentifierExpr>("a");
    MoveDecl md("b", "int", std::move(init));
    EXPECT_EQ(md.type, ASTNodeType::MOVE_DECL);
    EXPECT_EQ(md.name, "b");
    EXPECT_EQ(md.typeName, "int");
    ASSERT_NE(md.initializer, nullptr);
}

TEST(ASTTest, FieldAttrs) {
    FieldAttrs attrs;
    attrs.hot = true;
    attrs.cold = false;
    attrs.noalias = true;
    attrs.immut = true;
    attrs.isMove = false;
    attrs.align = 64;
    attrs.hasRange = true;
    attrs.rangeMin = 0;
    attrs.rangeMax = 100;

    EXPECT_TRUE(attrs.hot);
    EXPECT_FALSE(attrs.cold);
    EXPECT_TRUE(attrs.noalias);
    EXPECT_TRUE(attrs.immut);
    EXPECT_FALSE(attrs.isMove);
    EXPECT_EQ(attrs.align, 64);
    EXPECT_TRUE(attrs.hasRange);
    EXPECT_EQ(attrs.rangeMin, 0);
    EXPECT_EQ(attrs.rangeMax, 100);
}

TEST(ASTTest, StructField) {
    FieldAttrs attrs;
    attrs.hot = true;
    StructField sf("score", "int", attrs);
    EXPECT_EQ(sf.name, "score");
    EXPECT_EQ(sf.typeName, "int");
    EXPECT_TRUE(sf.attrs.hot);
}

TEST(ASTTest, StructDeclWithFieldDecls) {
    std::vector<std::string> fields = {"x", "y"};
    FieldAttrs a1; a1.hot = true;
    FieldAttrs a2; a2.cold = true;
    std::vector<StructField> fd = {StructField("x", "int", a1), StructField("y", "int", a2)};
    StructDecl sd("Point", std::move(fields), std::move(fd));
    EXPECT_EQ(sd.name, "Point");
    ASSERT_EQ(sd.fieldDecls.size(), 2u);
    EXPECT_TRUE(sd.fieldDecls[0].attrs.hot);
    EXPECT_TRUE(sd.fieldDecls[1].attrs.cold);
}
