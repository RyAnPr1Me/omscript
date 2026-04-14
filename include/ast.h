#pragma once

#ifndef AST_H
#define AST_H

/// @file ast.h
/// @brief Abstract syntax tree (AST) node definitions for OmScript.
///
/// Every syntactic construct in the language — expressions, statements,
/// declarations, and the top-level program — is represented by a concrete
/// struct that inherits from either Expression or Statement.

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
    STRUCT_DECL,
    STRUCT_LITERAL_EXPR,
    FIELD_ACCESS_EXPR,
    FIELD_ASSIGN_EXPR,
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
    PIPE_EXPR,
    MOVE_EXPR,
    BORROW_EXPR,
    DICT_EXPR,
    INVALIDATE_STMT,
    MOVE_DECL,
    FREEZE_STMT,
    PREFETCH_STMT,
    DEFER_STMT,
    SCOPE_RESOLUTION_EXPR,
    ASSUME_STMT,
    COMPTIME_EXPR,  // comptime { ... } — compile-time evaluated block expression
    REBORROW_EXPR,  // reborrow ref = &src; / reborrow ref = &src.field; / reborrow ref = &src[idx];
    PIPELINE_STMT   // pipeline (i in start...end) { stage name { ... } ... }
};

class ASTNode {
  public:
    ASTNodeType type;
    int line = 0;
    int column = 0;
    virtual ~ASTNode() = default;

  protected:
    explicit ASTNode(ASTNodeType t) : type(t) {}
};

class Expression : public ASTNode {
  protected:
    explicit Expression(ASTNodeType t) : ASTNode(t) {}
};

class Statement : public ASTNode {
  protected:
    explicit Statement(ASTNodeType t) : ASTNode(t) {}
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

    explicit LiteralExpr(long long val)
        : Expression(ASTNodeType::LITERAL_EXPR), literalType(LiteralType::INTEGER), intValue(val) {}

    explicit LiteralExpr(double val) : Expression(ASTNodeType::LITERAL_EXPR), literalType(LiteralType::FLOAT), floatValue(val) {}

    explicit LiteralExpr(const std::string& val)
        : Expression(ASTNodeType::LITERAL_EXPR), literalType(LiteralType::STRING), intValue(0), stringValue(val) {}
};

class IdentifierExpr : public Expression {
  public:
    std::string name;

    explicit IdentifierExpr(const std::string& n) : Expression(ASTNodeType::IDENTIFIER_EXPR), name(n) {}
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

    explicit ArrayExpr(std::vector<std::unique_ptr<Expression>> elems)
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

    explicit SpreadExpr(std::unique_ptr<Expression> op) : Expression(ASTNodeType::SPREAD_EXPR), operand(std::move(op)) {}
};

class PipeExpr : public Expression {
  public:
    std::unique_ptr<Expression> left;
    std::string functionName;

    PipeExpr(std::unique_ptr<Expression> l, const std::string& fn)
        : Expression(ASTNodeType::PIPE_EXPR), left(std::move(l)), functionName(fn) {}
};

/// Scope resolution expression: Scope::Member (e.g. Color::RED, Status::OK)
class ScopeResolutionExpr : public Expression {
  public:
    std::string scopeName;   ///< The scope/namespace name (e.g. "Color")
    std::string memberName;  ///< The member name (e.g. "RED")

    ScopeResolutionExpr(const std::string& scope, const std::string& member)
        : Expression(ASTNodeType::SCOPE_RESOLUTION_EXPR), scopeName(scope), memberName(member) {}
};

// Statements
class ExprStmt : public Statement {
  public:
    std::unique_ptr<Expression> expression;

    explicit ExprStmt(std::unique_ptr<Expression> expr) : Statement(ASTNodeType::EXPR_STMT), expression(std::move(expr)) {}
};

class VarDecl : public Statement {
  public:
    std::string name;
    std::unique_ptr<Expression> initializer;
    bool isConst;
    std::string typeName;
    bool isRegister = false; ///< `register var` — force variable into CPU register via mem2reg

    VarDecl(const std::string& n, std::unique_ptr<Expression> init, bool cnst = false, const std::string& type = "")
        : Statement(ASTNodeType::VAR_DECL), name(n), initializer(std::move(init)), isConst(cnst), typeName(type) {}
};

class ReturnStmt : public Statement {
  public:
    std::unique_ptr<Expression> value;

    explicit ReturnStmt(std::unique_ptr<Expression> val) : Statement(ASTNodeType::RETURN_STMT), value(std::move(val)) {}
};

struct LoopConfig {
    int  unrollCount  = 0;    // 0 = auto
    bool vectorize    = false;
    bool noVectorize  = false;
    int  tileSize     = 0;    // 0 = no tiling
    bool parallel     = false;
    bool independent  = false; ///< @independent — no cross-iteration dependencies (alias-free)
    bool fuse         = false; ///< @fuse — merge with adjacent compatible loop
};

struct MemoryConfig {
    /// Prefer stack allocation for small local arrays instead of heap.
    /// Reserved for future implementation; currently ignored.
    bool preferStack  = false;
    /// Emit llvm.prefetch hints for all pointer-type parameters at function
    /// entry.  Active: auto-prefetch emitted in generateFunction().
    bool prefetch     = false;
    /// All pointer parameters are already noalias in OPTMAX functions (language
    /// invariant).  Setting this to true adds an additional per-pair
    /// alias-scope annotation pass (future work beyond the base noalias).
    bool noalias      = false;
};

enum class SafetyLevel { On, Relaxed, Off };

struct OptMaxConfig {
    bool         enabled       = false;
    SafetyLevel  safety        = SafetyLevel::On;
    bool         fastMath      = false;
    bool         aggressiveVec = false;
    LoopConfig   loop;
    MemoryConfig memory;
    std::vector<std::string> assumes;
    std::vector<std::string> specialize;
    bool         report        = false;
};

class IfStmt : public Statement {
  public:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Statement> thenBranch;
    std::unique_ptr<Statement> elseBranch;

    /// Branch prediction hints: `likely if (...)` / `unlikely if (...)`
    bool hintLikely = false;    ///< Hint: then-branch is the common path
    bool hintUnlikely = false;  ///< Hint: then-branch is the rare path

    IfStmt(std::unique_ptr<Expression> cond, std::unique_ptr<Statement> thenB,
           std::unique_ptr<Statement> elseB = nullptr, bool likely = false, bool unlikely = false)
        : Statement(ASTNodeType::IF_STMT), condition(std::move(cond)), thenBranch(std::move(thenB)),
          elseBranch(std::move(elseB)), hintLikely(likely), hintUnlikely(unlikely) {}
};

class WhileStmt : public Statement {
  public:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Statement> body;
    LoopConfig loopHints;

    WhileStmt(std::unique_ptr<Expression> cond, std::unique_ptr<Statement> b)
        : Statement(ASTNodeType::WHILE_STMT), condition(std::move(cond)), body(std::move(b)) {}
};

class DoWhileStmt : public Statement {
  public:
    std::unique_ptr<Statement> body;
    std::unique_ptr<Expression> condition;
    LoopConfig loopHints;

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
    LoopConfig loopHints;

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
    LoopConfig loopHints;

    ForEachStmt(const std::string& iter, std::unique_ptr<Expression> coll, std::unique_ptr<Statement> b)
        : Statement(ASTNodeType::FOR_EACH_STMT), iteratorVar(iter), collection(std::move(coll)), body(std::move(b)) {}
};

// A single case arm in a switch statement.
// Supports multiple values per case: case 1, 2, 3: ...
struct SwitchCase {
    std::unique_ptr<Expression> value; // nullptr for default case (first value for single-value compat)
    std::vector<std::unique_ptr<Expression>> values; // all values for multi-value case
    std::vector<std::unique_ptr<Statement>> body;
    bool isDefault;

    SwitchCase(std::unique_ptr<Expression> v, std::vector<std::unique_ptr<Statement>> b, bool def = false)
        : value(std::move(v)), body(std::move(b)), isDefault(def) {}

    SwitchCase(std::vector<std::unique_ptr<Expression>> vs, std::vector<std::unique_ptr<Statement>> b, bool def = false)
        : body(std::move(b)), isDefault(def) {
        if (!vs.empty()) {
            // Store first value for backward compatibility
            value = std::move(vs[0]);
            for (size_t i = 1; i < vs.size(); ++i) {
                values.push_back(std::move(vs[i]));
            }
        }
    }
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

    explicit BlockStmt(std::vector<std::unique_ptr<Statement>> stmts)
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

    explicit ThrowStmt(std::unique_ptr<Expression> val) : Statement(ASTNodeType::THROW_STMT), value(std::move(val)) {}
};

class DeferStmt : public Statement {
  public:
    std::unique_ptr<Statement> body;

    explicit DeferStmt(std::unique_ptr<Statement> b) : Statement(ASTNodeType::DEFER_STMT), body(std::move(b)) {}
};

class AssumeStmt : public Statement {
  public:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Statement> deoptBody;  // nullptr if no else deopt
    AssumeStmt(std::unique_ptr<Expression> cond, std::unique_ptr<Statement> body = nullptr)
        : Statement(ASTNodeType::ASSUME_STMT), condition(std::move(cond)), deoptBody(std::move(body)) {}
};

class EnumDecl : public Statement {
  public:
    std::string name;
    std::vector<std::pair<std::string, long long>> members; // name → value

    EnumDecl(const std::string& n, std::vector<std::pair<std::string, long long>> m)
        : Statement(ASTNodeType::ENUM_DECL), name(n), members(std::move(m)) {}
};

/// Field attribute flags for optimization hints.
struct FieldAttrs {
    bool hot = false;       ///< Hint: frequently accessed — guide cache layout
    bool cold = false;      ///< Hint: rarely accessed
    bool noalias = false;   ///< Hint: pointer does not alias any other pointer
    bool immut = false;     ///< Hint: field never modified after initialization
    bool isMove = false;    ///< Hint: field participates in ownership transfer
    int align = 0;          ///< Alignment hint (0 = default)
    long long rangeMin = 0; ///< Value range lower bound (0 if unset)
    long long rangeMax = 0; ///< Value range upper bound (0 if unset)
    bool hasRange = false;  ///< Whether range(min,max) was specified
};

/// A single field within a struct declaration, with optional attributes.
struct StructField {
    std::string name;
    std::string typeName;   ///< Optional type annotation
    FieldAttrs attrs;

    StructField(const std::string& n, const std::string& t = "", FieldAttrs a = {})
        : name(n), typeName(t), attrs(a) {}
};

// Forward declaration for OperatorOverload.
class FunctionDecl;

/// Operator overload definition within a struct.
/// e.g. `operator+(other: Vec2) -> Vec2 { ... }`
struct OperatorOverload {
    std::string op;          ///< Operator string: "+", "-", "*", "/", "==", "!=", "<", ">"
    std::string paramName;   ///< Right-hand operand parameter name
    std::string paramType;   ///< Optional type annotation for parameter
    std::string returnType;  ///< Optional return type annotation
    std::unique_ptr<FunctionDecl> impl; ///< Implementation function
};

class StructDecl : public Statement {
  public:
    std::string name;
    std::vector<std::string> fields;       ///< Field names (for backwards compat)
    std::vector<StructField> fieldDecls;   ///< Rich field info with attributes
    std::vector<OperatorOverload> operators; ///< Operator overload definitions

    StructDecl(const std::string& n, std::vector<std::string> f)
        : Statement(ASTNodeType::STRUCT_DECL), name(n), fields(std::move(f)) {}

    StructDecl(const std::string& n, std::vector<std::string> f, std::vector<StructField> fd)
        : Statement(ASTNodeType::STRUCT_DECL), name(n), fields(std::move(f)), fieldDecls(std::move(fd)) {}
};

class StructLiteralExpr : public Expression {
  public:
    std::string structName;
    std::vector<std::pair<std::string, std::unique_ptr<Expression>>> fieldValues;

    StructLiteralExpr(const std::string& name,
                      std::vector<std::pair<std::string, std::unique_ptr<Expression>>> fv)
        : Expression(ASTNodeType::STRUCT_LITERAL_EXPR), structName(name), fieldValues(std::move(fv)) {}
};

class FieldAccessExpr : public Expression {
  public:
    std::unique_ptr<Expression> object;
    std::string fieldName;

    FieldAccessExpr(std::unique_ptr<Expression> obj, const std::string& field)
        : Expression(ASTNodeType::FIELD_ACCESS_EXPR), object(std::move(obj)), fieldName(field) {}
};

class FieldAssignExpr : public Expression {
  public:
    std::unique_ptr<Expression> object;
    std::string fieldName;
    std::unique_ptr<Expression> value;

    FieldAssignExpr(std::unique_ptr<Expression> obj, const std::string& field, std::unique_ptr<Expression> val)
        : Expression(ASTNodeType::FIELD_ASSIGN_EXPR), object(std::move(obj)), fieldName(field),
          value(std::move(val)) {}
};

// Top-level
class Parameter {
  public:
    std::string name;
    std::string typeName;
    std::unique_ptr<Expression> defaultValue; // nullptr if no default
    bool hintPrefetch = false; ///< @prefetch — prefetch memory at function entry, invalidate at exit

    Parameter(const std::string& n, const std::string& t = "", std::unique_ptr<Expression> def = nullptr)
        : name(n), typeName(t), defaultValue(std::move(def)) {}
};

class FunctionDecl : public ASTNode {
  public:
    std::string name;
    std::vector<std::string> typeParams;  // Generic type parameters like <T, R>
    std::vector<Parameter> parameters;
    std::unique_ptr<BlockStmt> body;
    bool isOptMax;
    std::string returnType;  // Optional return type annotation (e.g. "int", "int[]", "Point")

    /// Compiler hint annotations for functions.
    bool hintInline = false;    ///< @inline — suggest inlining this function
    bool hintNoInline = false;  ///< @noinline — prevent inlining this function
    bool hintCold = false;      ///< @cold — mark function as rarely executed
    bool hintHot = false;       ///< @hot — mark function as frequently executed
    bool hintPure = false;      ///< @pure — function has no side effects
    bool hintNoReturn = false;  ///< @noreturn — function never returns
    bool hintStatic = false;    ///< @static — use internal linkage for better IPO
    bool hintFlatten = false;   ///< @flatten — inline all callees within this function
    bool hintUnroll = false;    ///< @unroll — aggressively unroll all loops in this function
    bool hintNoUnroll = false;  ///< @nounroll — disable loop unrolling in this function
    bool hintRestrict = false;  ///< @restrict — all pointer params are noalias (no aliasing)
    bool hintVectorize = false;   ///< @vectorize — enable loop vectorization for all loops
    bool hintNoVectorize = false; ///< @novectorize — disable loop vectorization for all loops
    bool hintParallelize = false;   ///< @parallel — enable auto-parallelization for all loops
    bool hintNoParallelize = false; ///< @noparallel — disable auto-parallelization for all loops
    bool hintMinSize = false;     ///< @minsize — optimize for minimum code size
    bool hintOptNone = false;     ///< @optnone — disable all optimizations (useful for debugging)
    bool hintNoUnwind = false;    ///< @nounwind — function never throws C++ exceptions
    bool hintConstEval = false;   ///< @const_eval — evaluate at compile time when all args are constants
    OptMaxConfig optMaxConfig;    ///< OPTMAX v2 configuration (enabled when @optmax(...) annotation is used)

    /// @allocator(size=N) or @allocator(size=N, count=M) annotation.
    /// Marks this function as an allocator wrapper: LLVM will add the
    /// `allocsize` attribute so alias analysis can track allocation sizes.
    ///   allocatorSizeParam >= 0: 0-based index of the "size" parameter
    ///   allocatorCountParam >= 0: 0-based index of the "count" parameter (-1 = none)
    int allocatorSizeParam  = -1; ///< -1 = not an allocator wrapper
    int allocatorCountParam = -1; ///< -1 = no count parameter

    FunctionDecl(const std::string& n, std::vector<std::string> tps, std::vector<Parameter> params, std::unique_ptr<BlockStmt> b, bool optMax = false, const std::string& retType = "")
        : ASTNode(ASTNodeType::FUNCTION), name(n), typeParams(std::move(tps)), parameters(std::move(params)), body(std::move(b)), isOptMax(optMax), returnType(retType) {
    }

    /// Returns the number of parameters that have no default value.
    size_t requiredParameters() const {
        size_t count = 0;
        for (const auto& p : parameters) {
            if (!p.defaultValue)
                ++count;
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
    std::vector<std::unique_ptr<StructDecl>> structs;
    bool fileNoAlias = false;  ///< @noalias file directive: all pointers are noalias

    Program(std::vector<std::unique_ptr<FunctionDecl>> funcs, std::vector<std::unique_ptr<EnumDecl>> enms = {},
            std::vector<std::unique_ptr<StructDecl>> strcts = {}, bool noAlias = false)
        : ASTNode(ASTNodeType::PROGRAM), functions(std::move(funcs)), enums(std::move(enms)),
          structs(std::move(strcts)), fileNoAlias(noAlias) {}
};

// ---------------------------------------------------------------------------
// Ownership system nodes
// ---------------------------------------------------------------------------

/// `move x` — transfer ownership, source becomes logically dead.
class MoveExpr : public Expression {
  public:
    std::unique_ptr<Expression> source;

    explicit MoveExpr(std::unique_ptr<Expression> src)
        : Expression(ASTNodeType::MOVE_EXPR), source(std::move(src)) {}
};

/// `borrow ref = &x` — non-owning reference hint for alias analysis.
/// `borrow mut ref = x` — single mutable reference (unique mutable alias).
class BorrowExpr : public Expression {
  public:
    std::unique_ptr<Expression> source;
    bool isMut = false; ///< true for `borrow mut` — mutable borrow

    /// Set by generateBorrowExpr to the source variable name so that the
    /// surrounding VarDecl codegen can register the alias mapping after
    /// the ref variable's name is known.
    mutable std::string pendingSrcVar;

    explicit BorrowExpr(std::unique_ptr<Expression> src, bool mut = false)
        : Expression(ASTNodeType::BORROW_EXPR), source(std::move(src)), isMut(mut) {}
};

/// `reborrow ref = &src;` — create a new borrow reference from an existing borrow.
/// `reborrow ref = &src.field;` — partial borrow of a struct field.
/// `reborrow ref = &src[idx];` — partial borrow of an array element.
class ReborrowExpr : public Expression {
  public:
    std::unique_ptr<Expression> source;  ///< The source expression (identifier, field access, index)
    bool isMut = false;                  ///< true for mutable reborrow
    std::string fieldName;               ///< Non-empty if partial borrow of a field
    std::unique_ptr<Expression> indexExpr; ///< Non-null if partial borrow of an array element

    /// Pending source variable name (set during codegen, mirrors BorrowExpr).
    mutable std::string pendingSrcVar;

    explicit ReborrowExpr(std::unique_ptr<Expression> src, bool mut = false,
                          std::string field = "", std::unique_ptr<Expression> idx = nullptr)
        : Expression(ASTNodeType::REBORROW_EXPR), source(std::move(src)), isMut(mut),
          fieldName(std::move(field)), indexExpr(std::move(idx)) {}
};

/// Dict literal: `{"key": val, ...}` — zero-cost map construction.
/// Compiles to a single malloc + direct stores with no search loops,
/// in contrast to repeated map_set() calls which each do malloc+memcpy+loop.
class DictExpr : public Expression {
  public:
    /// Each entry is a (key-expression, value-expression) pair.
    std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> pairs;

    explicit DictExpr(std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> p)
        : Expression(ASTNodeType::DICT_EXPR), pairs(std::move(p)) {}
};

/// `invalidate x;` — explicitly mark a variable as dead.
class InvalidateStmt : public Statement {
  public:
    std::string varName;

    explicit InvalidateStmt(const std::string& name)
        : Statement(ASTNodeType::INVALIDATE_STMT), varName(name) {}
};

/// `move T a = b;` — variable declaration with move semantics.
class MoveDecl : public Statement {
  public:
    std::string name;
    std::string typeName;
    std::unique_ptr<Expression> initializer;

    MoveDecl(const std::string& n, const std::string& t, std::unique_ptr<Expression> init)
        : Statement(ASTNodeType::MOVE_DECL), name(n), typeName(t), initializer(std::move(init)) {}
};

/// `prefetch x:u32;` — standalone prefetch hint for an existing variable.
/// `prefetch [hot] [immut] var name:type = expr;` — variable declaration with prefetch hint.
class PrefetchStmt : public Statement {
  public:
    /// For standalone prefetch of an existing variable (no declaration).
    std::string varName;

    /// For prefetch with variable declaration.
    std::unique_ptr<VarDecl> varDecl;

    /// Attribute hints.
    bool hintHot = false;
    bool hintImmut = false;

    /// Byte offset for speculative prefetch (e.g. prefetch+128 to prefetch
    /// 2 cache lines ahead).  Default 0 means prefetch at the variable's
    /// base address only.
    int64_t offsetBytes = 0;

    /// Constructor for standalone prefetch of an existing variable.
    explicit PrefetchStmt(const std::string& name, int64_t offset = 0)
        : Statement(ASTNodeType::PREFETCH_STMT), varName(name), offsetBytes(offset) {}

    /// Constructor for prefetch with variable declaration.
    PrefetchStmt(std::unique_ptr<VarDecl> decl, bool hot, bool immut, int64_t offset = 0)
        : Statement(ASTNodeType::PREFETCH_STMT), varDecl(std::move(decl)),
          hintHot(hot), hintImmut(immut), offsetBytes(offset) {}
};

/// `freeze x;` — marks variable `x` immutable for the rest of its lifetime.
/// After freeze:
///   - All loads become !invariant.load (LLVM can hoist/CSE across loops).
///   - Writes to the variable are a compile-time error (same as const).
///   - llvm.invariant.start is emitted so the optimizer can eliminate
///     redundant loads and fold the value into constants where possible.
class FreezeStmt : public Statement {
  public:
    std::string varName;

    explicit FreezeStmt(const std::string& name)
        : Statement(ASTNodeType::FREEZE_STMT), varName(name) {}
};

/// `comptime { statements... }` — compile-time evaluated block expression.
/// The block is evaluated at compile time by the OmScript constant evaluator.
/// The result (the value of the last return statement) is a compile-time
/// constant that replaces the expression wherever it is used.
///
/// Typical uses:
///   var x = comptime { return 2 * 3 + 1; }     // x = 7, a compile-time const
///   var y = comptime { var n = 10; return n * n; } // y = 100
///
/// If the block cannot be evaluated at compile time (e.g. it calls I/O),
/// a compile error is emitted.
class ComptimeExpr : public Expression {
  public:
    std::unique_ptr<BlockStmt> body;

    explicit ComptimeExpr(std::unique_ptr<BlockStmt> b)
        : Expression(ASTNodeType::COMPTIME_EXPR), body(std::move(b)) {}
};

// ---------------------------------------------------------------------------
// Effect system
// ---------------------------------------------------------------------------

/// Inferred side-effect summary for a user function.
///
/// Populated by CodeGenerator::inferFunctionEffects() before any code is
/// generated.  Drives automatic LLVM attribute inference (readonly, readnone,
/// nosync, willreturn) and warns when @pure is annotated on a function whose
/// body contains detectable effects.
struct FunctionEffects {
    bool readsMemory  = false; ///< Function reads from heap/params (loads)
    bool writesMemory = false; ///< Function writes to heap/params (stores, push, pop, sort…)
    bool hasIO        = false; ///< Function performs I/O (print, file_*, input, sleep…)
    bool hasMutation  = false; ///< Function mutates a parameter observable outside the callee

    /// True when the function has no detectable effects at all (pure).
    bool isReadNone() const { return !readsMemory && !writesMemory && !hasIO && !hasMutation; }
    /// True when the function only reads memory and has no I/O or mutations.
    bool isReadOnly() const { return readsMemory && !writesMemory && !hasIO && !hasMutation; }
    /// True when the function is safe to mark nosync (no I/O, no concurrency).
    bool isNoSync()   const { return !hasIO; }
    /// True when the function can be inferred as @pure by the compiler.
    bool inferredPure() const { return isReadNone() || isReadOnly(); }
};

// ---------------------------------------------------------------------------
// Pipeline construct
// ---------------------------------------------------------------------------

/// A single named stage inside a pipeline block.
/// Contains the stage name and its body statements.
struct StageDecl {
    std::string name;
    std::unique_ptr<BlockStmt> body;

    StageDecl(std::string n, std::unique_ptr<BlockStmt> b)
        : name(std::move(n)), body(std::move(b)) {}
};

/// `pipeline (iterVar in start...end) { stage name { ... } ... }`
///
/// Desugared by codegen into a for-loop over [start, end) where the stages
/// execute sequentially per iteration.  The compiler annotates the generated
/// loop with software-pipeline metadata so LLVM's instruction scheduler and
/// auto-vectorizer can exploit inter-stage parallelism.
class PipelineStmt : public Statement {
  public:
    std::string iterVar;                     ///< Loop variable name (e.g. "i")
    std::string iterType;                    ///< Optional type annotation (e.g. "int")
    std::unique_ptr<Expression> start;       ///< Range start expression
    std::unique_ptr<Expression> end;         ///< Range end expression (exclusive)
    std::unique_ptr<Expression> step;        ///< Optional step (nullptr = 1)
    std::vector<StageDecl> stages;           ///< Ordered list of stages

    PipelineStmt(std::string iv, std::string it,
                 std::unique_ptr<Expression> s, std::unique_ptr<Expression> e,
                 std::unique_ptr<Expression> step_,
                 std::vector<StageDecl> stgs)
        : Statement(ASTNodeType::PIPELINE_STMT),
          iterVar(std::move(iv)), iterType(std::move(it)),
          start(std::move(s)), end(std::move(e)), step(std::move(step_)),
          stages(std::move(stgs)) {}
};

} // namespace omscript

#endif // AST_H
