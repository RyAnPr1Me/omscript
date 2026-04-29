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
#include "opt_context.h"
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

/// Convert an extracted e-graph node back into an AST Expression.
///
/// `bestMap` is the result of a SINGLE `EGraph::extractAll(root, model)`
/// call performed at the top level — every class reachable from the
/// extraction root is already populated with its optimal e-node, so
/// child lookups are O(1) hash hits.  The previous implementation
/// invoked `EGraph::extract(child, model)` per recursion step, which
/// in turn invoked `extractAll(child, model)` — yielding O(N²) work
/// in the size of the e-graph.  Reusing one `bestMap` makes AST
/// conversion strictly linear in the size of the extracted tree.
static std::unique_ptr<Expression> eNodeToAST(EGraph& graph, ClassId cls,
                                               const std::unordered_map<ClassId,
                                                   EGraph::ExtractionResult>& bestMap) {
    cls = graph.find(cls);

    ENode node;
    auto it = bestMap.find(cls);
    if (it != bestMap.end()) {
        node = it->second.bestNode;
    } else {
        // Fallback: pick the first node of the class.  This matches the
        // legacy `EGraph::extract` behaviour and only triggers for classes
        // unreachable from the extraction root, which by construction
        // cannot occur for sub-classes referenced by an extracted node.
        const auto& nodes = graph.getClass(cls).nodes;
        node = nodes.empty() ? ENode(Op::Nop) : nodes.front();
    }

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
        auto left = eNodeToAST(graph, node.children[0], bestMap);
        auto right = eNodeToAST(graph, node.children[1], bestMap);
        return std::make_unique<BinaryExpr>(opToString(node.op),
                                             std::move(left), std::move(right));
    }

    // Unary operations
    if (isUnaryOp(node.op) && node.children.size() == 1) {
        auto operand = eNodeToAST(graph, node.children[0], bestMap);
        return std::make_unique<UnaryExpr>(opToString(node.op), std::move(operand));
    }

    // Ternary
    if (node.op == Op::Ternary && node.children.size() == 3) {
        auto cond = eNodeToAST(graph, node.children[0], bestMap);
        auto thenE = eNodeToAST(graph, node.children[1], bestMap);
        auto elseE = eNodeToAST(graph, node.children[2], bestMap);
        return std::make_unique<TernaryExpr>(std::move(cond), std::move(thenE),
                                              std::move(elseE));
    }

    // Function call
    if (node.op == Op::Call) {
        std::vector<std::unique_ptr<Expression>> args;
        args.reserve(node.children.size());
        for (auto child : node.children) {
            args.push_back(eNodeToAST(graph, child, bestMap));
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
/// Core implementation: optimize an expression using a provided EGraphOptContext.
/// This is the single implementation used by all public entry points.
static std::unique_ptr<Expression> optimizeExpressionImpl(const Expression* expr,
                                                           const EGraphOptContext& ctx);

/// Recursively optimize all expressions within a statement.
static void optimizeStatementImpl(Statement* stmt, const EGraphOptContext& ctx);

/// Check whether an expression tree can be fully represented in the e-graph.
/// Returns false if any sub-expression would degenerate into an opaque variable
/// (e.g. IndexExpr, unsupported binary operators like ??), which would cause
/// the reconstructed AST to reference non-existent variables like __opaque.
static bool canRepresentInEGraph(const Expression* expr, const EGraphOptContext& ctx) {
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
        return canRepresentInEGraph(bin->left.get(), ctx) &&
               canRepresentInEGraph(bin->right.get(), ctx);
    }

    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<const UnaryExpr*>(expr);
        if (un->op != "-" && un->op != "~" && un->op != "!") return false;
        return canRepresentInEGraph(un->operand.get(), ctx);
    }

    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<const TernaryExpr*>(expr);
        return canRepresentInEGraph(tern->condition.get(), ctx) &&
               canRepresentInEGraph(tern->thenExpr.get(), ctx) &&
               canRepresentInEGraph(tern->elseExpr.get(), ctx);
    }

    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const CallExpr*>(expr);
        // Allow known-pure builtins (no side effects, no I/O) as classified
        // by BuiltinEffectTable, plus user functions confirmed pure by analysis.
        const bool isKnownPure =
            BuiltinEffectTable::isPure(call->callee) ||
            (ctx.pureUserFuncs && ctx.pureUserFuncs->count(call->callee) > 0);
        if (!isKnownPure)
            return false;
        for (const auto& arg : call->arguments) {
            if (!canRepresentInEGraph(arg.get(), ctx)) return false;
        }
        return true;
    }

    // MoveExpr / BorrowExpr — delegate to inner expression.
    case ASTNodeType::MOVE_EXPR: {
        auto* mv = static_cast<const MoveExpr*>(expr);
        return canRepresentInEGraph(mv->source.get(), ctx);
    }
    case ASTNodeType::BORROW_EXPR: {
        auto* bw = static_cast<const BorrowExpr*>(expr);
        return canRepresentInEGraph(bw->source.get(), ctx);
    }

    default:
        // IndexExpr, AssignExpr, etc. — cannot be represented.
        return false;
    }
}

/// Core implementation: optimize one expression using the provided context.
static std::unique_ptr<Expression> optimizeExpressionImpl(const Expression* expr,
                                                           const EGraphOptContext& ctx) {
    if (!expr) return nullptr;

    EGraph graph(ctx.config);
    const ClassId root = astToEGraph(graph, expr);

    // The rule set is constant across the lifetime of the process, so we build
    // it once and reuse it for every expression.  C++11 guarantees thread-safe
    // initialisation of function-local statics.
    static const std::vector<RewriteRule> kAllRules = getAllRules();
    graph.saturate(kAllRules);

    const CostModel model;
    // Single bottom-up cost-DAG construction for the entire reachable
    // sub-graph from `root`.  Reused for every recursive `eNodeToAST`
    // call below; replaces the old per-class `EGraph::extract` which
    // re-ran extractAll for each visited class (O(N²)).
    auto bestMap = graph.extractAll(root, model);
    return eNodeToAST(graph, root, bestMap);
}

/// Optimize an expression in-place by replacing with the e-graph result.
static std::unique_ptr<Expression> tryOptimize(std::unique_ptr<Expression> expr,
                                                const EGraphOptContext& ctx) {
    if (!expr) return nullptr;

    switch (expr->type) {
    case ASTNodeType::BINARY_EXPR:
    case ASTNodeType::UNARY_EXPR:
    case ASTNodeType::LITERAL_EXPR:
    case ASTNodeType::IDENTIFIER_EXPR:
    case ASTNodeType::TERNARY_EXPR: {
        // Guard: skip expressions that contain sub-expressions the e-graph
        // cannot model (e.g. array index, null coalesce ??).
        if (!canRepresentInEGraph(expr.get(), ctx))
            return expr;
        // Preserve source location so error messages keep line/column info.
        const int origLine   = expr->line;
        const int origColumn = expr->column;
        auto optimized = optimizeExpressionImpl(expr.get(), ctx);
        if (optimized) {
            if (optimized->line == 0) {
                optimized->line   = origLine;
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

static void optimizeStatementImpl(Statement* stmt, const EGraphOptContext& ctx) {
    if (!stmt) return;

    switch (stmt->type) {
    case ASTNodeType::VAR_DECL: {
        auto* varDecl = static_cast<VarDecl*>(stmt);
        if (varDecl->initializer) {
            varDecl->initializer = tryOptimize(std::move(varDecl->initializer), ctx);
        }
        break;
    }
    case ASTNodeType::RETURN_STMT: {
        auto* ret = static_cast<ReturnStmt*>(stmt);
        if (ret->value) {
            ret->value = tryOptimize(std::move(ret->value), ctx);
        }
        break;
    }
    case ASTNodeType::EXPR_STMT: {
        auto* exprStmt = static_cast<ExprStmt*>(stmt);
        if (exprStmt->expression) {
            exprStmt->expression = tryOptimize(std::move(exprStmt->expression), ctx);
        }
        break;
    }
    case ASTNodeType::IF_STMT: {
        auto* ifStmt = static_cast<IfStmt*>(stmt);
        if (ifStmt->condition) {
            ifStmt->condition = tryOptimize(std::move(ifStmt->condition), ctx);
        }
        optimizeStatementImpl(ifStmt->thenBranch.get(), ctx);
        optimizeStatementImpl(ifStmt->elseBranch.get(), ctx);
        break;
    }
    case ASTNodeType::WHILE_STMT: {
        auto* whileStmt = static_cast<WhileStmt*>(stmt);
        if (whileStmt->condition) {
            whileStmt->condition = tryOptimize(std::move(whileStmt->condition), ctx);
        }
        optimizeStatementImpl(whileStmt->body.get(), ctx);
        break;
    }
    case ASTNodeType::DO_WHILE_STMT: {
        auto* doWhile = static_cast<DoWhileStmt*>(stmt);
        optimizeStatementImpl(doWhile->body.get(), ctx);
        if (doWhile->condition) {
            doWhile->condition = tryOptimize(std::move(doWhile->condition), ctx);
        }
        break;
    }
    case ASTNodeType::BLOCK: {
        auto* block = static_cast<BlockStmt*>(stmt);
        for (auto& s : block->statements) {
            optimizeStatementImpl(s.get(), ctx);
        }
        break;
    }
    case ASTNodeType::MOVE_DECL: {
        auto* moveDecl = static_cast<MoveDecl*>(stmt);
        if (moveDecl->initializer) {
            moveDecl->initializer = tryOptimize(std::move(moveDecl->initializer), ctx);
        }
        break;
    }
    case ASTNodeType::INVALIDATE_STMT:
        [[fallthrough]];
    default:
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API — context-aware overloads
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<Expression> optimizeExpression(const Expression* expr,
                                                const EGraphOptContext& ctx) {
    if (!expr) return nullptr;
    if (!canRepresentInEGraph(expr, ctx)) return nullptr;
    return optimizeExpressionImpl(expr, ctx);
}

void optimizeFunction(FunctionDecl* func, const EGraphOptContext& ctx) {
    if (!func || !func->body) return;
    for (auto& stmt : func->body->statements) {
        optimizeStatementImpl(stmt.get(), ctx);
    }
}

void optimizeProgram(Program* program, const EGraphOptContext& ctx) {
    if (!program) return;
    for (auto& func : program->functions) {
        optimizeFunction(func.get(), ctx);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API — legacy overloads (delegate to context-aware versions)
// ─────────────────────────────────────────────────────────────────────────────

std::unique_ptr<Expression> optimizeExpression(const Expression* expr) {
    static const EGraphOptContext kDefault;
    return optimizeExpression(expr, kDefault);
}

void optimizeFunction(FunctionDecl* func) {
    static const EGraphOptContext kDefault;
    optimizeFunction(func, kDefault);
}

void optimizeProgram(Program* program) {
    static const EGraphOptContext kDefault;
    optimizeProgram(program, kDefault);
}

} // namespace egraph
} // namespace omscript
