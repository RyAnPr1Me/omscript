/// @file egraph_optimizer.cpp
/// @brief AST ↔ E-Graph bridge for equality saturation optimization.
///
/// Converts OmScript AST expressions into e-graph nodes, runs equality
/// saturation with all optimization rules, then extracts the optimal
/// expression back into AST form.
///
/// This optimizer is invoked:
///   - At O2+: for expressions in OPTMAX-marked functions
///   - At O3:  for all function bodies
///
/// The optimizer preserves semantic correctness by:
///   - Only applying mathematically sound rewrite rules
///   - Respecting IEEE-754 semantics for floating-point (no x*0→0 for floats)
///   - Preserving side effects (function calls are not reordered or eliminated)

#include "ast.h"
#include "egraph.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace omscript {
namespace egraph {

/// Converts an AST Expression to an e-graph node, returning the class ID.
/// Recursively converts sub-expressions.
static ClassId astToEGraph(EGraph& graph, const Expression* expr) {
    if (!expr) return graph.addConst(0);

    switch (expr->type) {
    case ASTNodeType::LITERAL_EXPR: {
        auto* lit = static_cast<const LiteralExpr*>(expr);
        switch (lit->literalType) {
        case LiteralExpr::LiteralType::INTEGER:
            return graph.addConst(lit->intValue);
        case LiteralExpr::LiteralType::FLOAT:
            return graph.addConstF(lit->floatValue);
        case LiteralExpr::LiteralType::STRING:
            // Strings are opaque — represent as a named variable
            return graph.addVar("__str_" + lit->stringValue);
        }
        break;
    }

    case ASTNodeType::IDENTIFIER_EXPR: {
        auto* id = static_cast<const IdentifierExpr*>(expr);
        return graph.addVar(id->name);
    }

    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const BinaryExpr*>(expr);
        const ClassId lhs = astToEGraph(graph, bin->left.get());
        const ClassId rhs = astToEGraph(graph, bin->right.get());

        // Map OmScript binary operators to e-graph Ops
        Op op = Op::Nop;
        if      (bin->op == "+")  op = Op::Add;
        else if (bin->op == "-")  op = Op::Sub;
        else if (bin->op == "*")  op = Op::Mul;
        else if (bin->op == "/")  op = Op::Div;
        else if (bin->op == "%")  op = Op::Mod;
        else if (bin->op == "**") op = Op::Pow;
        else if (bin->op == "&")  op = Op::BitAnd;
        else if (bin->op == "|")  op = Op::BitOr;
        else if (bin->op == "^")  op = Op::BitXor;
        else if (bin->op == "<<") op = Op::Shl;
        else if (bin->op == ">>") op = Op::Shr;
        else if (bin->op == "==") op = Op::Eq;
        else if (bin->op == "!=") op = Op::Ne;
        else if (bin->op == "<")  op = Op::Lt;
        else if (bin->op == "<=") op = Op::Le;
        else if (bin->op == ">")  op = Op::Gt;
        else if (bin->op == ">=") op = Op::Ge;
        else if (bin->op == "&&") op = Op::LogAnd;
        else if (bin->op == "||") op = Op::LogOr;
        else {
            // Unknown operator — treat as opaque
            return graph.addVar("__binop_" + bin->op);
        }

        return graph.addBinOp(op, lhs, rhs);
    }

    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<const UnaryExpr*>(expr);
        const ClassId operand = astToEGraph(graph, un->operand.get());

        Op op = Op::Nop;
        if      (un->op == "-") op = Op::Neg;
        else if (un->op == "~") op = Op::BitNot;
        else if (un->op == "!") op = Op::LogNot;
        else {
            return graph.addVar("__unop_" + un->op);
        }

        return graph.addUnaryOp(op, operand);
    }

    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<const TernaryExpr*>(expr);
        const ClassId cond = astToEGraph(graph, tern->condition.get());
        const ClassId thenE = astToEGraph(graph, tern->thenExpr.get());
        const ClassId elseE = astToEGraph(graph, tern->elseExpr.get());

        ENode node(Op::Ternary);
        node.children = {cond, thenE, elseE};
        return graph.add(std::move(node));
    }

    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const CallExpr*>(expr);
        std::vector<ClassId> argIds;
        argIds.reserve(call->arguments.size());
        for (const auto& arg : call->arguments) {
            argIds.push_back(astToEGraph(graph, arg.get()));
        }
        ENode node(Op::Call, call->callee, std::move(argIds));
        return graph.add(std::move(node));
    }

    default:
        // For complex expressions we can't optimize (index, field access, etc.),
        // represent as opaque variables to preserve correctness.
        return graph.addVar("__opaque");
    }

    return graph.addConst(0);
}

/// Map e-graph Op back to OmScript operator string.
static std::string opToString(Op op) {
    switch (op) {
    case Op::Add: return "+";
    case Op::Sub: return "-";
    case Op::Mul: return "*";
    case Op::Div: return "/";
    case Op::Mod: return "%";
    case Op::Pow: return "**";
    case Op::BitAnd: return "&";
    case Op::BitOr:  return "|";
    case Op::BitXor: return "^";
    case Op::Shl: return "<<";
    case Op::Shr: return ">>";
    case Op::Eq: return "==";
    case Op::Ne: return "!=";
    case Op::Lt: return "<";
    case Op::Le: return "<=";
    case Op::Gt: return ">";
    case Op::Ge: return ">=";
    case Op::LogAnd: return "&&";
    case Op::LogOr:  return "||";
    case Op::Neg: return "-";
    case Op::BitNot: return "~";
    case Op::LogNot: return "!";
    default: return "";
    }
}

/// Check if an Op is a binary operation.
static bool isBinaryOp(Op op) {
    switch (op) {
    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod:
    case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl: case Op::Shr:
    case Op::Eq: case Op::Ne: case Op::Lt: case Op::Le: case Op::Gt: case Op::Ge:
    case Op::LogAnd: case Op::LogOr: case Op::Pow:
        return true;
    default:
        return false;
    }
}

/// Check if an Op is a unary operation.
static bool isUnaryOp(Op op) {
    switch (op) {
    case Op::Neg: case Op::BitNot: case Op::LogNot:
        return true;
    default:
        return false;
    }
}

/// Recursively extract an ENode tree from the e-graph starting at a class.
/// Uses the cost model to pick the best node at each level.
static ENode extractRecursive(EGraph& graph, ClassId cls, const CostModel& model,
                               std::unordered_map<ClassId, ENode>& cache) {
    cls = graph.find(cls);
    auto it = cache.find(cls);
    if (it != cache.end()) return it->second;

    ENode best = graph.extract(cls, model);
    cache[cls] = best;
    return best;
}

/// Convert an extracted e-graph node back into an AST Expression.
/// Recursively converts children, consulting the e-graph for sub-expressions.
static std::unique_ptr<Expression> eNodeToAST(EGraph& graph, ClassId cls,
                                               const CostModel& model,
                                               std::unordered_map<ClassId, ENode>& cache) {
    cls = graph.find(cls);
    ENode node = extractRecursive(graph, cls, model, cache);

    switch (node.op) {
    case Op::Const:
        return std::make_unique<LiteralExpr>(node.value);

    case Op::ConstF:
        return std::make_unique<LiteralExpr>(node.fvalue);

    case Op::Var: {
        // Strip internal prefixes
        const std::string name = node.name;
        if (name.substr(0, 6) == "__str_") {
            return std::make_unique<LiteralExpr>(name.substr(6));
        }
        return std::make_unique<IdentifierExpr>(name);
    }

    case Op::Nop:
        return std::make_unique<LiteralExpr>(0LL);

    default:
        break;
    }

    // Binary operations
    if (isBinaryOp(node.op) && node.children.size() == 2) {
        auto left = eNodeToAST(graph, node.children[0], model, cache);
        auto right = eNodeToAST(graph, node.children[1], model, cache);
        return std::make_unique<BinaryExpr>(opToString(node.op),
                                             std::move(left), std::move(right));
    }

    // Unary operations
    if (isUnaryOp(node.op) && node.children.size() == 1) {
        auto operand = eNodeToAST(graph, node.children[0], model, cache);
        return std::make_unique<UnaryExpr>(opToString(node.op), std::move(operand));
    }

    // Ternary
    if (node.op == Op::Ternary && node.children.size() == 3) {
        auto cond = eNodeToAST(graph, node.children[0], model, cache);
        auto thenE = eNodeToAST(graph, node.children[1], model, cache);
        auto elseE = eNodeToAST(graph, node.children[2], model, cache);
        return std::make_unique<TernaryExpr>(std::move(cond), std::move(thenE),
                                              std::move(elseE));
    }

    // Function call
    if (node.op == Op::Call) {
        std::vector<std::unique_ptr<Expression>> args;
        args.reserve(node.children.size());
        for (auto child : node.children) {
            args.push_back(eNodeToAST(graph, child, model, cache));
        }
        auto e = std::make_unique<CallExpr>(node.name, std::move(args));
        e->fromStdNamespace = true; // e-graph optimizer generated
        return e;
    }

    // Fallback
    return std::make_unique<LiteralExpr>(0LL);
}

/// Optimize a single AST expression using e-graph equality saturation.
/// Returns the optimized expression (may be the same if no improvement found).
std::unique_ptr<Expression> optimizeExpression(const Expression* expr) {
    if (!expr) return nullptr;

    SaturationConfig config;
    config.maxNodes = 10000;
    config.maxIterations = 15;
    config.enableConstantFolding = true;

    EGraph graph(config);
    const ClassId root = astToEGraph(graph, expr);

    // Run equality saturation with all rules.
    // The rule set is constant across the lifetime of the process, so we build
    // it once and reuse it for every expression.  C++11 guarantees that the
    // initialisation of a function-local static is thread-safe.
    static const std::vector<RewriteRule> kAllRules = getAllRules();
    graph.saturate(kAllRules);

    // Extract the optimal expression
    const CostModel model;
    std::unordered_map<ClassId, ENode> cache;
    return eNodeToAST(graph, root, model, cache);
}

/// Recursively optimize all expressions within a statement.
static void optimizeStatement(Statement* stmt);

/// Check whether an expression tree can be fully represented in the e-graph.
/// Returns false if any sub-expression would degenerate into an opaque variable
/// (e.g. IndexExpr, unsupported binary operators like ??), which would cause
/// the reconstructed AST to reference non-existent variables like __opaque.
static bool canRepresentInEGraph(const Expression* expr) {
    if (!expr) return true;

    switch (expr->type) {
    case ASTNodeType::LITERAL_EXPR:
    case ASTNodeType::IDENTIFIER_EXPR:
        return true;

    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const BinaryExpr*>(expr);
        // Only operators that have a corresponding e-graph Op are safe.
        static const std::unordered_set<std::string> supported = {
            "+", "-", "*", "/", "%", "**", "&", "|", "^", "<<", ">>",
            "==", "!=", "<", "<=", ">", ">=", "&&", "||"
        };
        if (supported.find(bin->op) == supported.end()) return false;
        // String operands with * or + have different semantics (repetition,
        // concatenation) than integer arithmetic.  The e-graph's algebraic
        // rules (e.g. x*0→0, x*1→x) are not valid for string operations,
        // so we must exclude them.
        if (bin->op == "*" || bin->op == "+") {
            auto isStringLit = [](const Expression* e) -> bool {
                if (!e) return false;
                if (auto* lit = dynamic_cast<const LiteralExpr*>(e))
                    return lit->literalType == LiteralExpr::LiteralType::STRING;
                return false;
            };
            if (isStringLit(bin->left.get()) || isStringLit(bin->right.get()))
                return false;
        }
        return canRepresentInEGraph(bin->left.get()) &&
               canRepresentInEGraph(bin->right.get());
    }

    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<const UnaryExpr*>(expr);
        if (un->op != "-" && un->op != "~" && un->op != "!") return false;
        return canRepresentInEGraph(un->operand.get());
    }

    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<const TernaryExpr*>(expr);
        return canRepresentInEGraph(tern->condition.get()) &&
               canRepresentInEGraph(tern->thenExpr.get()) &&
               canRepresentInEGraph(tern->elseExpr.get());
    }

    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const CallExpr*>(expr);
        // Only allow known-pure builtins (no side effects, no I/O).
        // Functions like print, input, swap, reverse, assert are excluded
        // because they have observable side effects that the e-graph's
        // rewrite rules could incorrectly eliminate or reorder.
        static const std::unordered_set<std::string> pureBuiltins = {
            "abs", "min", "max", "sign", "clamp", "pow", "sqrt",
            "is_even", "is_odd", "len", "str_len", "to_string",
            "to_int", "to_float", "typeof", "log2", "gcd",
            "char_at", "str_eq", "str_find", "floor", "ceil", "round"
        };
        if (pureBuiltins.find(call->callee) == pureBuiltins.end())
            return false;
        for (const auto& arg : call->arguments) {
            if (!canRepresentInEGraph(arg.get())) return false;
        }
        return true;
    }

    // MoveExpr / BorrowExpr — delegate to inner expression.
    case ASTNodeType::MOVE_EXPR: {
        auto* mv = static_cast<const MoveExpr*>(expr);
        return canRepresentInEGraph(mv->source.get());
    }
    case ASTNodeType::BORROW_EXPR: {
        auto* bw = static_cast<const BorrowExpr*>(expr);
        return canRepresentInEGraph(bw->source.get());
    }

    default:
        // IndexExpr, AssignExpr, etc. — cannot be represented.
        return false;
    }
}

/// Optimize an expression in-place by replacing with the e-graph result.
static std::unique_ptr<Expression> tryOptimize(std::unique_ptr<Expression> expr) {
    if (!expr) return nullptr;

    // Only optimize pure expressions (no side effects)
    // Skip calls, assignments, and other effectful operations
    switch (expr->type) {
    case ASTNodeType::BINARY_EXPR:
    case ASTNodeType::UNARY_EXPR:
    case ASTNodeType::LITERAL_EXPR:
    case ASTNodeType::IDENTIFIER_EXPR:
    case ASTNodeType::TERNARY_EXPR: {
        // Guard: skip expressions containing sub-expressions that the e-graph
        // cannot represent (e.g. array index, null coalesce ??).  Without this
        // check, the e-graph would introduce opaque variable references like
        // __opaque or __binop_?? that break codegen.
        if (!canRepresentInEGraph(expr.get()))
            return expr;
        // Preserve source location from the original expression so that error
        // messages from the codegen still include line/column information.
        const int origLine = expr->line;
        const int origColumn = expr->column;
        auto optimized = optimizeExpression(expr.get());
        if (optimized) {
            if (optimized->line == 0) {
                optimized->line = origLine;
                optimized->column = origColumn;
            }
            return optimized;
        }
        return expr;
    }
    default:
        return expr;
    }
}

static void optimizeStatement(Statement* stmt) {
    if (!stmt) return;

    switch (stmt->type) {
    case ASTNodeType::VAR_DECL: {
        auto* varDecl = static_cast<VarDecl*>(stmt);
        if (varDecl->initializer) {
            varDecl->initializer = tryOptimize(std::move(varDecl->initializer));
        }
        break;
    }
    case ASTNodeType::RETURN_STMT: {
        auto* ret = static_cast<ReturnStmt*>(stmt);
        if (ret->value) {
            ret->value = tryOptimize(std::move(ret->value));
        }
        break;
    }
    case ASTNodeType::EXPR_STMT: {
        auto* exprStmt = static_cast<ExprStmt*>(stmt);
        if (exprStmt->expression) {
            exprStmt->expression = tryOptimize(std::move(exprStmt->expression));
        }
        break;
    }
    case ASTNodeType::IF_STMT: {
        auto* ifStmt = static_cast<IfStmt*>(stmt);
        if (ifStmt->condition) {
            ifStmt->condition = tryOptimize(std::move(ifStmt->condition));
        }
        optimizeStatement(ifStmt->thenBranch.get());
        optimizeStatement(ifStmt->elseBranch.get());
        break;
    }
    case ASTNodeType::WHILE_STMT: {
        auto* whileStmt = static_cast<WhileStmt*>(stmt);
        if (whileStmt->condition) {
            whileStmt->condition = tryOptimize(std::move(whileStmt->condition));
        }
        optimizeStatement(whileStmt->body.get());
        break;
    }
    case ASTNodeType::DO_WHILE_STMT: {
        auto* doWhile = static_cast<DoWhileStmt*>(stmt);
        optimizeStatement(doWhile->body.get());
        if (doWhile->condition) {
            doWhile->condition = tryOptimize(std::move(doWhile->condition));
        }
        break;
    }
    case ASTNodeType::BLOCK: {
        auto* block = static_cast<BlockStmt*>(stmt);
        for (auto& s : block->statements) {
            optimizeStatement(s.get());
        }
        break;
    }
    case ASTNodeType::MOVE_DECL: {
        auto* moveDecl = static_cast<MoveDecl*>(stmt);
        if (moveDecl->initializer) {
            moveDecl->initializer = tryOptimize(std::move(moveDecl->initializer));
        }
        break;
    }
    case ASTNodeType::INVALIDATE_STMT:
        // Nothing to optimize for invalidate statements.
        [[fallthrough]];
    default:
        break;
    }
}

/// Optimize an entire function's body using e-graph equality saturation.
void optimizeFunction(FunctionDecl* func) {
    if (!func || !func->body) return;
    for (auto& stmt : func->body->statements) {
        optimizeStatement(stmt.get());
    }
}

/// Optimize all functions in a program using e-graph equality saturation.
void optimizeProgram(Program* program) {
    if (!program) return;
    for (auto& func : program->functions) {
        optimizeFunction(func.get());
    }
}

} // namespace egraph
} // namespace omscript
