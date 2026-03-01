#ifndef AST_H
#define AST_H

#include <memory>
#include <string>
#include <vector>

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
    DO_WHILE_STMT,
    FOR_STMT,
    BREAK_STMT,
    CONTINUE_STMT,
    SWITCH_STMT,
    FOR_EACH_STMT,
    TRY_CATCH_STMT,
    THROW_STMT,
    ENUM_DECL,
    EXPR_STMT,
    BINARY_EXPR,
    UNARY_EXPR,
    CALL_EXPR,
    LITERAL_EXPR,
    IDENTIFIER_EXPR,
    ASSIGN_EXPR,
    POSTFIX_EXPR,
    PREFIX_EXPR,
    TERNARY_EXPR,
    ARRAY_EXPR,
    INDEX_EXPR,
    INDEX_ASSIGN_EXPR,
    LAMBDA_EXPR,
    SPREAD_EXPR,
    PIPE_EXPR
};

class ASTNode {
  public:
    ASTNodeType type;
    int line = 0;
    int column = 0;
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

    LiteralExpr(double val) : Expression(ASTNodeType::LITERAL_EXPR), literalType(LiteralType::FLOAT), floatValue(val) {}

    LiteralExpr(const std::string& val)
        : Expression(ASTNodeType::LITERAL_EXPR), literalType(LiteralType::STRING), intValue(0), stringValue(val) {}
};

class IdentifierExpr : public Expression {
  public:
    std::string name;

    IdentifierExpr(const std::string& n) : Expression(ASTNodeType::IDENTIFIER_EXPR), name(n) {}
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

class PostfixExpr : public Expression {
  public:
    std::string op; // ++ or --
    std::unique_ptr<Expression> operand;

    PostfixExpr(const std::string& o, std::unique_ptr<Expression> opnd)
        : Expression(ASTNodeType::POSTFIX_EXPR), op(o), operand(std::move(opnd)) {}
};

class PrefixExpr : public Expression {
  public:
    std::string op; // ++ or --
    std::unique_ptr<Expression> operand;

    PrefixExpr(const std::string& o, std::unique_ptr<Expression> opnd)
        : Expression(ASTNodeType::PREFIX_EXPR), op(o), operand(std::move(opnd)) {}
};

class TernaryExpr : public Expression {
  public:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> thenExpr;
    std::unique_ptr<Expression> elseExpr;

    TernaryExpr(std::unique_ptr<Expression> cond, std::unique_ptr<Expression> thenE, std::unique_ptr<Expression> elseE)
        : Expression(ASTNodeType::TERNARY_EXPR), condition(std::move(cond)), thenExpr(std::move(thenE)),
          elseExpr(std::move(elseE)) {}
};

class ArrayExpr : public Expression {
  public:
    std::vector<std::unique_ptr<Expression>> elements;

    ArrayExpr(std::vector<std::unique_ptr<Expression>> elems)
        : Expression(ASTNodeType::ARRAY_EXPR), elements(std::move(elems)) {}
};

class IndexExpr : public Expression {
  public:
    std::unique_ptr<Expression> array;
    std::unique_ptr<Expression> index;

    IndexExpr(std::unique_ptr<Expression> arr, std::unique_ptr<Expression> idx)
        : Expression(ASTNodeType::INDEX_EXPR), array(std::move(arr)), index(std::move(idx)) {}
};

class IndexAssignExpr : public Expression {
  public:
    std::unique_ptr<Expression> array;
    std::unique_ptr<Expression> index;
    std::unique_ptr<Expression> value;

    IndexAssignExpr(std::unique_ptr<Expression> arr, std::unique_ptr<Expression> idx, std::unique_ptr<Expression> val)
        : Expression(ASTNodeType::INDEX_ASSIGN_EXPR), array(std::move(arr)), index(std::move(idx)),
          value(std::move(val)) {}
};

class LambdaExpr : public Expression {
  public:
    std::vector<std::string> params;
    std::unique_ptr<Expression> body;

    LambdaExpr(std::vector<std::string> p, std::unique_ptr<Expression> b)
        : Expression(ASTNodeType::LAMBDA_EXPR), params(std::move(p)), body(std::move(b)) {}
};

class SpreadExpr : public Expression {
  public:
    std::unique_ptr<Expression> operand;

    SpreadExpr(std::unique_ptr<Expression> op)
        : Expression(ASTNodeType::SPREAD_EXPR), operand(std::move(op)) {}
};

class PipeExpr : public Expression {
  public:
    std::unique_ptr<Expression> left;
    std::string functionName;

    PipeExpr(std::unique_ptr<Expression> l, const std::string& fn)
        : Expression(ASTNodeType::PIPE_EXPR), left(std::move(l)), functionName(fn) {}
};

// Statements
class ExprStmt : public Statement {
  public:
    std::unique_ptr<Expression> expression;

    ExprStmt(std::unique_ptr<Expression> expr) : Statement(ASTNodeType::EXPR_STMT), expression(std::move(expr)) {}
};

class VarDecl : public Statement {
  public:
    std::string name;
    std::unique_ptr<Expression> initializer;
    bool isConst;
    std::string typeName;

    VarDecl(const std::string& n, std::unique_ptr<Expression> init, bool cnst = false, const std::string& type = "")
        : Statement(ASTNodeType::VAR_DECL), name(n), initializer(std::move(init)), isConst(cnst), typeName(type) {}
};

class ReturnStmt : public Statement {
  public:
    std::unique_ptr<Expression> value;

    ReturnStmt(std::unique_ptr<Expression> val) : Statement(ASTNodeType::RETURN_STMT), value(std::move(val)) {}
};

class IfStmt : public Statement {
  public:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Statement> thenBranch;
    std::unique_ptr<Statement> elseBranch;

    IfStmt(std::unique_ptr<Expression> cond, std::unique_ptr<Statement> thenB,
           std::unique_ptr<Statement> elseB = nullptr)
        : Statement(ASTNodeType::IF_STMT), condition(std::move(cond)), thenBranch(std::move(thenB)),
          elseBranch(std::move(elseB)) {}
};

class WhileStmt : public Statement {
  public:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Statement> body;

    WhileStmt(std::unique_ptr<Expression> cond, std::unique_ptr<Statement> b)
        : Statement(ASTNodeType::WHILE_STMT), condition(std::move(cond)), body(std::move(b)) {}
};

class DoWhileStmt : public Statement {
  public:
    std::unique_ptr<Statement> body;
    std::unique_ptr<Expression> condition;

    DoWhileStmt(std::unique_ptr<Statement> b, std::unique_ptr<Expression> cond)
        : Statement(ASTNodeType::DO_WHILE_STMT), body(std::move(b)), condition(std::move(cond)) {}
};

class ForStmt : public Statement {
  public:
    std::string iteratorVar;
    std::string iteratorType;
    std::unique_ptr<Expression> start;
    std::unique_ptr<Expression> end;
    std::unique_ptr<Expression> step; // Optional, can be nullptr
    std::unique_ptr<Statement> body;

    ForStmt(const std::string& iter, std::unique_ptr<Expression> s, std::unique_ptr<Expression> e,
            std::unique_ptr<Expression> st, std::unique_ptr<Statement> b, const std::string& iterType = "")
        : Statement(ASTNodeType::FOR_STMT), iteratorVar(iter), iteratorType(iterType), start(std::move(s)),
          end(std::move(e)), step(std::move(st)), body(std::move(b)) {}
};

class BreakStmt : public Statement {
  public:
    BreakStmt() : Statement(ASTNodeType::BREAK_STMT) {}
};

class ContinueStmt : public Statement {
  public:
    ContinueStmt() : Statement(ASTNodeType::CONTINUE_STMT) {}
};

class ForEachStmt : public Statement {
  public:
    std::string iteratorVar;
    std::unique_ptr<Expression> collection;
    std::unique_ptr<Statement> body;

    ForEachStmt(const std::string& iter, std::unique_ptr<Expression> coll, std::unique_ptr<Statement> b)
        : Statement(ASTNodeType::FOR_EACH_STMT), iteratorVar(iter), collection(std::move(coll)), body(std::move(b)) {}
};

// A single case arm in a switch statement.
struct SwitchCase {
    std::unique_ptr<Expression> value; // nullptr for default case
    std::vector<std::unique_ptr<Statement>> body;
    bool isDefault;

    SwitchCase(std::unique_ptr<Expression> v, std::vector<std::unique_ptr<Statement>> b, bool def = false)
        : value(std::move(v)), body(std::move(b)), isDefault(def) {}
};

class SwitchStmt : public Statement {
  public:
    std::unique_ptr<Expression> condition;
    std::vector<SwitchCase> cases;

    SwitchStmt(std::unique_ptr<Expression> cond, std::vector<SwitchCase> c)
        : Statement(ASTNodeType::SWITCH_STMT), condition(std::move(cond)), cases(std::move(c)) {}
};

class BlockStmt : public Statement {
  public:
    std::vector<std::unique_ptr<Statement>> statements;

    BlockStmt(std::vector<std::unique_ptr<Statement>> stmts)
        : Statement(ASTNodeType::BLOCK), statements(std::move(stmts)) {}
};

class TryCatchStmt : public Statement {
  public:
    std::unique_ptr<BlockStmt> tryBlock;
    std::string catchVar; // variable name for the error code in catch block
    std::unique_ptr<BlockStmt> catchBlock;

    TryCatchStmt(std::unique_ptr<BlockStmt> tryB, const std::string& var, std::unique_ptr<BlockStmt> catchB)
        : Statement(ASTNodeType::TRY_CATCH_STMT), tryBlock(std::move(tryB)), catchVar(var),
          catchBlock(std::move(catchB)) {}
};

class ThrowStmt : public Statement {
  public:
    std::unique_ptr<Expression> value;

    ThrowStmt(std::unique_ptr<Expression> val) : Statement(ASTNodeType::THROW_STMT), value(std::move(val)) {}
};

class EnumDecl : public Statement {
  public:
    std::string name;
    std::vector<std::pair<std::string, long long>> members; // name → value

    EnumDecl(const std::string& n, std::vector<std::pair<std::string, long long>> m)
        : Statement(ASTNodeType::ENUM_DECL), name(n), members(std::move(m)) {}
};

// Top-level
class Parameter {
  public:
    std::string name;
    std::string typeName;
    std::unique_ptr<Expression> defaultValue;  // nullptr if no default

    Parameter(const std::string& n, const std::string& t = "", std::unique_ptr<Expression> def = nullptr)
        : name(n), typeName(t), defaultValue(std::move(def)) {}
};

class FunctionDecl : public ASTNode {
  public:
    std::string name;
    std::vector<Parameter> parameters;
    std::unique_ptr<BlockStmt> body;
    bool isOptMax;

    FunctionDecl(const std::string& n, std::vector<Parameter> params, std::unique_ptr<BlockStmt> b, bool optMax = false)
        : ASTNode(ASTNodeType::FUNCTION), name(n), parameters(std::move(params)), body(std::move(b)), isOptMax(optMax) {
    }

    /// Returns the number of parameters that have no default value.
    size_t requiredParameters() const {
        size_t count = 0;
        for (const auto& p : parameters) {
            if (!p.defaultValue) ++count;
        }
        return count;
    }

    /// Returns true if every parameter has a type annotation.
    bool hasFullTypeAnnotations() const {
        if (parameters.empty())
            return true;
        for (const auto& p : parameters) {
            if (p.typeName.empty())
                return false;
        }
        return true;
    }
};

class Program : public ASTNode {
  public:
    std::vector<std::unique_ptr<FunctionDecl>> functions;
    std::vector<std::unique_ptr<EnumDecl>> enums;

    Program(std::vector<std::unique_ptr<FunctionDecl>> funcs, std::vector<std::unique_ptr<EnumDecl>> enms = {})
        : ASTNode(ASTNodeType::PROGRAM), functions(std::move(funcs)), enums(std::move(enms)) {}
};

} // namespace omscript

#endif // AST_H
