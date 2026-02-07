#ifndef AST_H
#define AST_H

#include <string>
#include <vector>
#include <memory>

namespace omscript {

enum class ASTNodeType {
    PROGRAM,
    FUNCTION,
    PARAMETER,
    BLOCK,
    VAR_DECL,
    RETURN_STMT,
    IF_STMT,
    WHILE_STMT,
    EXPR_STMT,
    BINARY_EXPR,
    UNARY_EXPR,
    CALL_EXPR,
    LITERAL_EXPR,
    IDENTIFIER_EXPR,
    ASSIGN_EXPR
};

class ASTNode {
public:
    ASTNodeType type;
    virtual ~ASTNode() = default;
    
protected:
    ASTNode(ASTNodeType t) : type(t) {}
};

class Expression : public ASTNode {
protected:
    Expression(ASTNodeType t) : ASTNode(t) {}
};

class Statement : public ASTNode {
protected:
    Statement(ASTNodeType t) : ASTNode(t) {}
};

// Expressions
class LiteralExpr : public Expression {
public:
    enum class LiteralType { INTEGER, FLOAT, STRING };
    LiteralType literalType;
    union {
        long long intValue;
        double floatValue;
    };
    std::string stringValue;
    
    LiteralExpr(long long val)
        : Expression(ASTNodeType::LITERAL_EXPR), literalType(LiteralType::INTEGER), intValue(val) {}
    
    LiteralExpr(double val)
        : Expression(ASTNodeType::LITERAL_EXPR), literalType(LiteralType::FLOAT), floatValue(val) {}
    
    LiteralExpr(const std::string& val)
        : Expression(ASTNodeType::LITERAL_EXPR), literalType(LiteralType::STRING), stringValue(val), intValue(0) {}
};

class IdentifierExpr : public Expression {
public:
    std::string name;
    
    IdentifierExpr(const std::string& n)
        : Expression(ASTNodeType::IDENTIFIER_EXPR), name(n) {}
};

class BinaryExpr : public Expression {
public:
    std::string op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;
    
    BinaryExpr(const std::string& o, std::unique_ptr<Expression> l, std::unique_ptr<Expression> r)
        : Expression(ASTNodeType::BINARY_EXPR), op(o), left(std::move(l)), right(std::move(r)) {}
};

class UnaryExpr : public Expression {
public:
    std::string op;
    std::unique_ptr<Expression> operand;
    
    UnaryExpr(const std::string& o, std::unique_ptr<Expression> opnd)
        : Expression(ASTNodeType::UNARY_EXPR), op(o), operand(std::move(opnd)) {}
};

class CallExpr : public Expression {
public:
    std::string callee;
    std::vector<std::unique_ptr<Expression>> arguments;
    
    CallExpr(const std::string& c, std::vector<std::unique_ptr<Expression>> args)
        : Expression(ASTNodeType::CALL_EXPR), callee(c), arguments(std::move(args)) {}
};

class AssignExpr : public Expression {
public:
    std::string name;
    std::unique_ptr<Expression> value;
    
    AssignExpr(const std::string& n, std::unique_ptr<Expression> v)
        : Expression(ASTNodeType::ASSIGN_EXPR), name(n), value(std::move(v)) {}
};

// Statements
class ExprStmt : public Statement {
public:
    std::unique_ptr<Expression> expression;
    
    ExprStmt(std::unique_ptr<Expression> expr)
        : Statement(ASTNodeType::EXPR_STMT), expression(std::move(expr)) {}
};

class VarDecl : public Statement {
public:
    std::string name;
    std::unique_ptr<Expression> initializer;
    bool isConst;
    
    VarDecl(const std::string& n, std::unique_ptr<Expression> init, bool cnst = false)
        : Statement(ASTNodeType::VAR_DECL), name(n), initializer(std::move(init)), isConst(cnst) {}
};

class ReturnStmt : public Statement {
public:
    std::unique_ptr<Expression> value;
    
    ReturnStmt(std::unique_ptr<Expression> val)
        : Statement(ASTNodeType::RETURN_STMT), value(std::move(val)) {}
};

class IfStmt : public Statement {
public:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Statement> thenBranch;
    std::unique_ptr<Statement> elseBranch;
    
    IfStmt(std::unique_ptr<Expression> cond, std::unique_ptr<Statement> thenB, std::unique_ptr<Statement> elseB = nullptr)
        : Statement(ASTNodeType::IF_STMT), condition(std::move(cond)), thenBranch(std::move(thenB)), elseBranch(std::move(elseB)) {}
};

class WhileStmt : public Statement {
public:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Statement> body;
    
    WhileStmt(std::unique_ptr<Expression> cond, std::unique_ptr<Statement> b)
        : Statement(ASTNodeType::WHILE_STMT), condition(std::move(cond)), body(std::move(b)) {}
};

class BlockStmt : public Statement {
public:
    std::vector<std::unique_ptr<Statement>> statements;
    
    BlockStmt(std::vector<std::unique_ptr<Statement>> stmts)
        : Statement(ASTNodeType::BLOCK), statements(std::move(stmts)) {}
};

// Top-level
class Parameter {
public:
    std::string name;
    
    Parameter(const std::string& n) : name(n) {}
};

class FunctionDecl : public ASTNode {
public:
    std::string name;
    std::vector<Parameter> parameters;
    std::unique_ptr<BlockStmt> body;
    
    FunctionDecl(const std::string& n, std::vector<Parameter> params, std::unique_ptr<BlockStmt> b)
        : ASTNode(ASTNodeType::FUNCTION), name(n), parameters(std::move(params)), body(std::move(b)) {}
};

class Program : public ASTNode {
public:
    std::vector<std::unique_ptr<FunctionDecl>> functions;
    
    Program(std::vector<std::unique_ptr<FunctionDecl>> funcs)
        : ASTNode(ASTNodeType::PROGRAM), functions(std::move(funcs)) {}
};

} // namespace omscript

#endif // AST_H
