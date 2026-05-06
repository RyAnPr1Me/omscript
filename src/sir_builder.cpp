// === COMPILER LAYER 2 (SEMANTICS): SIR Builder ===
// Walks the AST (post all pre-passes) and consolidates every semantic fact
// into a SIRModule.  This is a read-only pass — it does not transform the AST.

#include "sir.h"
#include "ast.h"
#include "opt_context.h"
#include "pass_utils.h"  // isIntLiteral, isIntLiteralVal

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// SIRType::str — diagnostic string representation
// ─────────────────────────────────────────────────────────────────────────────
std::string SIRType::str() const {
    switch (kind) {
        case BaseKind::Void:   return "void";
        case BaseKind::Bool:   return "bool";
        case BaseKind::Int:    return "i" + std::to_string(bitWidth);
        case BaseKind::UInt:   return "u" + std::to_string(bitWidth);
        case BaseKind::Float:  return "f" + std::to_string(bitWidth);
        case BaseKind::String: return "string";
        case BaseKind::BigInt: return "bigint";
        case BaseKind::Struct: return "struct " + structName;
        case BaseKind::Enum:   return "enum " + structName;
        case BaseKind::Array: {
            std::string elem = elementType ? elementType->str() : "?";
            if (staticArrayLen >= 0)
                return elem + "[" + std::to_string(staticArrayLen) + "]";
            return elem + "[]";
        }
        case BaseKind::Fn:    return "fn";
        default:              return "?";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// typeFromAnnotation — parse a type-annotation string into a SIRType
// ─────────────────────────────────────────────────────────────────────────────
static SIRType typeFromAnnotation(
        const std::string& annot,
        const std::unordered_map<std::string, StructRepr>& structReprs) {
    if (annot.empty() || annot == "int" || annot == "i64")
        return SIRType::makeInt(64);
    if (annot == "i32")   return SIRType::makeInt(32);
    if (annot == "i16")   return SIRType::makeInt(16);
    if (annot == "i8")    return SIRType::makeInt(8);
    if (annot == "i128")  return SIRType::makeInt(128);
    if (annot == "u64" || annot == "uint") return SIRType::makeUInt(64);
    if (annot == "u32")   return SIRType::makeUInt(32);
    if (annot == "u16")   return SIRType::makeUInt(16);
    if (annot == "u8")    return SIRType::makeUInt(8);
    if (annot == "u128")  return SIRType::makeUInt(128);
    if (annot == "f64" || annot == "float") return SIRType::makeFloat(64);
    if (annot == "f32")   return SIRType::makeFloat(32);
    if (annot == "bool")  return SIRType::makeBool();
    if (annot == "string") return SIRType::makeString();
    if (annot == "bigint") { SIRType t; t.kind = SIRType::BaseKind::BigInt; return t; }
    if (annot == "void")   return SIRType::makeVoid();
    // Array suffix: "int[]", "Point[]", …
    if (annot.size() > 2 && annot.back() == ']' && annot[annot.size()-2] == '[') {
        const std::string elem = annot.substr(0, annot.size() - 2);
        return SIRType::makeArray(typeFromAnnotation(elem, structReprs));
    }
    // Named struct / enum
    {
        StructRepr repr = StructRepr::Auto;
        const auto it = structReprs.find(annot);
        if (it != structReprs.end()) repr = it->second;
        return SIRType::makeStruct(annot, repr);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// estimateStmtSize — rough instruction-count heuristic
// ─────────────────────────────────────────────────────────────────────────────
static unsigned estimateStmtSize(const Statement* stmt, int depth = 0) {
    if (!stmt || depth > 16) return depth > 0 ? 10 : 0;
    switch (stmt->type) {
        case ASTNodeType::VAR_DECL:
        case ASTNodeType::EXPR_STMT:
        case ASTNodeType::RETURN_STMT:
            return 1;
        case ASTNodeType::IF_STMT: {
            const auto* s = static_cast<const IfStmt*>(stmt);
            return 1
                + estimateStmtSize(s->thenBranch.get(), depth+1)
                + estimateStmtSize(s->elseBranch.get(), depth+1);
        }
        case ASTNodeType::WHILE_STMT: {
            const auto* s = static_cast<const WhileStmt*>(stmt);
            return 3 + estimateStmtSize(s->body.get(), depth+1);
        }
        case ASTNodeType::DO_WHILE_STMT: {
            const auto* s = static_cast<const DoWhileStmt*>(stmt);
            return 3 + estimateStmtSize(s->body.get(), depth+1);
        }
        case ASTNodeType::FOR_STMT: {
            const auto* s = static_cast<const ForStmt*>(stmt);
            return 3 + estimateStmtSize(s->body.get(), depth+1);
        }
        case ASTNodeType::FOR_EACH_STMT: {
            const auto* s = static_cast<const ForEachStmt*>(stmt);
            return 3 + estimateStmtSize(s->body.get(), depth+1);
        }
        case ASTNodeType::BLOCK: {
            const auto* s = static_cast<const BlockStmt*>(stmt);
            unsigned n = 0;
            for (const auto& ch : s->statements)
                n += estimateStmtSize(ch.get(), depth+1);
            return n;
        }
        default:
            return 1;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// collectCalleesFromExpr / collectCalleesFromStmt
// ─────────────────────────────────────────────────────────────────────────────
static void collectCalleesExpr(const Expression* e,
                                std::unordered_set<std::string>& out) {
    if (!e) return;
    switch (e->type) {
        case ASTNodeType::CALL_EXPR: {
            const auto* c = static_cast<const CallExpr*>(e);
            out.insert(c->callee);
            for (const auto& a : c->arguments) collectCalleesExpr(a.get(), out);
            break;
        }
        case ASTNodeType::BINARY_EXPR: {
            const auto* b = static_cast<const BinaryExpr*>(e);
            collectCalleesExpr(b->left.get(), out);
            collectCalleesExpr(b->right.get(), out);
            break;
        }
        case ASTNodeType::UNARY_EXPR:
            collectCalleesExpr(static_cast<const UnaryExpr*>(e)->operand.get(), out);
            break;
        case ASTNodeType::TERNARY_EXPR: {
            const auto* t = static_cast<const TernaryExpr*>(e);
            collectCalleesExpr(t->condition.get(), out);
            collectCalleesExpr(t->thenExpr.get(), out);
            collectCalleesExpr(t->elseExpr.get(), out);
            break;
        }
        case ASTNodeType::FIELD_ACCESS_EXPR:
            collectCalleesExpr(static_cast<const FieldAccessExpr*>(e)->object.get(), out);
            break;
        case ASTNodeType::FIELD_ASSIGN_EXPR:
            collectCalleesExpr(static_cast<const FieldAssignExpr*>(e)->value.get(), out);
            break;
        case ASTNodeType::INDEX_EXPR: {
            const auto* a = static_cast<const IndexExpr*>(e);
            collectCalleesExpr(a->array.get(), out);
            collectCalleesExpr(a->index.get(), out);
            break;
        }
        case ASTNodeType::INDEX_ASSIGN_EXPR: {
            const auto* a = static_cast<const IndexAssignExpr*>(e);
            collectCalleesExpr(a->array.get(), out);
            collectCalleesExpr(a->index.get(), out);
            collectCalleesExpr(a->value.get(), out);
            break;
        }
        case ASTNodeType::ASSIGN_EXPR:
            collectCalleesExpr(static_cast<const AssignExpr*>(e)->value.get(), out);
            break;
        default: break;
    }
}

static void collectCalleesStmt(const Statement* s,
                                std::unordered_set<std::string>& out) {
    if (!s) return;
    switch (s->type) {
        case ASTNodeType::BLOCK:
            for (const auto& ch : static_cast<const BlockStmt*>(s)->statements)
                collectCalleesStmt(ch.get(), out);
            break;
        case ASTNodeType::IF_STMT: {
            const auto* st = static_cast<const IfStmt*>(s);
            collectCalleesExpr(st->condition.get(), out);
            collectCalleesStmt(st->thenBranch.get(), out);
            collectCalleesStmt(st->elseBranch.get(), out);
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            const auto* st = static_cast<const WhileStmt*>(s);
            collectCalleesExpr(st->condition.get(), out);
            collectCalleesStmt(st->body.get(), out);
            break;
        }
        case ASTNodeType::DO_WHILE_STMT: {
            const auto* st = static_cast<const DoWhileStmt*>(s);
            collectCalleesExpr(st->condition.get(), out);
            collectCalleesStmt(st->body.get(), out);
            break;
        }
        case ASTNodeType::FOR_STMT: {
            const auto* st = static_cast<const ForStmt*>(s);
            collectCalleesExpr(st->start.get(), out);
            collectCalleesExpr(st->end.get(), out);
            collectCalleesExpr(st->step.get(), out);
            collectCalleesStmt(st->body.get(), out);
            break;
        }
        case ASTNodeType::FOR_EACH_STMT: {
            const auto* st = static_cast<const ForEachStmt*>(s);
            collectCalleesExpr(st->collection.get(), out);
            collectCalleesStmt(st->body.get(), out);
            break;
        }
        case ASTNodeType::RETURN_STMT:
            collectCalleesExpr(static_cast<const ReturnStmt*>(s)->value.get(), out);
            break;
        case ASTNodeType::VAR_DECL:
            collectCalleesExpr(static_cast<const VarDecl*>(s)->initializer.get(), out);
            break;
        case ASTNodeType::EXPR_STMT:
            collectCalleesExpr(static_cast<const ExprStmt*>(s)->expression.get(), out);
            break;
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// collectCallSitesExpr / collectCallSitesStmt
// ─────────────────────────────────────────────────────────────────────────────
static void collectCallSitesExpr(
        const Expression* e,
        std::vector<SIRCallSite>& out,
        const OptimizationContext& ctx,
        bool tailPos = false) {
    if (!e) return;
    switch (e->type) {
        case ASTNodeType::CALL_EXPR: {
            const auto* c = static_cast<const CallExpr*>(e);
            SIRCallSite cs;
            cs.callee        = c->callee;
            cs.isTailPosition = tailPos;
            const FunctionFacts& ff = ctx.getFacts(c->callee);
            cs.isPureCall   = ff.isPure;
            cs.isBuiltin    = false; // builtins tracked separately
            cs.effectKind   = ff.effects.hasIO ? EffectKind::IO :
                              (ff.effects.writesMemory ? EffectKind::Write :
                               (ff.effects.readsMemory  ? EffectKind::Read : EffectKind::None));
            cs.stability    = ff.ersl.isStable ? Stability::Stable : Stability::InputDependent;
            cs.argEscape    = ff.ersl.writeEscape;
            cs.isInlinable  = ff.isPure;

            for (const auto& arg : c->arguments) {
                long long v = 0;
                if (arg && isIntLiteral(arg.get(), &v)) {
                    cs.constIntArgs.push_back(static_cast<int64_t>(v));
                    cs.constStrArgs.push_back(std::nullopt);
                } else if (arg && arg->type == ASTNodeType::LITERAL_EXPR) {
                    const auto* lit = static_cast<const LiteralExpr*>(arg.get());
                    if (lit->literalType == LiteralExpr::LiteralType::STRING) {
                        cs.constIntArgs.push_back(std::nullopt);
                        cs.constStrArgs.push_back(lit->stringValue);
                    } else {
                        cs.constIntArgs.push_back(std::nullopt);
                        cs.constStrArgs.push_back(std::nullopt);
                    }
                } else {
                    cs.constIntArgs.push_back(std::nullopt);
                    cs.constStrArgs.push_back(std::nullopt);
                }
            }
            out.push_back(std::move(cs));
            for (const auto& arg : c->arguments)
                collectCallSitesExpr(arg.get(), out, ctx, false);
            break;
        }
        case ASTNodeType::BINARY_EXPR: {
            const auto* b = static_cast<const BinaryExpr*>(e);
            collectCallSitesExpr(b->left.get(), out, ctx);
            collectCallSitesExpr(b->right.get(), out, ctx);
            break;
        }
        case ASTNodeType::UNARY_EXPR:
            collectCallSitesExpr(
                static_cast<const UnaryExpr*>(e)->operand.get(), out, ctx);
            break;
        case ASTNodeType::TERNARY_EXPR: {
            const auto* t = static_cast<const TernaryExpr*>(e);
            collectCallSitesExpr(t->condition.get(), out, ctx);
            collectCallSitesExpr(t->thenExpr.get(), out, ctx);
            collectCallSitesExpr(t->elseExpr.get(), out, ctx);
            break;
        }
        case ASTNodeType::FIELD_ASSIGN_EXPR:
            collectCallSitesExpr(
                static_cast<const FieldAssignExpr*>(e)->value.get(), out, ctx);
            break;
        case ASTNodeType::INDEX_EXPR: {
            const auto* a = static_cast<const IndexExpr*>(e);
            collectCallSitesExpr(a->array.get(), out, ctx);
            collectCallSitesExpr(a->index.get(), out, ctx);
            break;
        }
        case ASTNodeType::INDEX_ASSIGN_EXPR: {
            const auto* a = static_cast<const IndexAssignExpr*>(e);
            collectCallSitesExpr(a->array.get(), out, ctx);
            collectCallSitesExpr(a->index.get(), out, ctx);
            collectCallSitesExpr(a->value.get(), out, ctx);
            break;
        }
        case ASTNodeType::ASSIGN_EXPR:
            collectCallSitesExpr(
                static_cast<const AssignExpr*>(e)->value.get(), out, ctx);
            break;
        default: break;
    }
}

static void collectCallSitesStmt(const Statement* s,
                                  std::vector<SIRCallSite>& out,
                                  const OptimizationContext& ctx) {
    if (!s) return;
    switch (s->type) {
        case ASTNodeType::BLOCK: {
            const auto& stmts = static_cast<const BlockStmt*>(s)->statements;
            for (size_t i = 0; i < stmts.size(); ++i) {
                const Statement* ch = stmts[i].get();
                const bool isLast = (i + 1 == stmts.size());
                if (isLast && ch && ch->type == ASTNodeType::RETURN_STMT) {
                    const auto* r = static_cast<const ReturnStmt*>(ch);
                    collectCallSitesExpr(r->value.get(), out, ctx, /*tailPos=*/true);
                } else {
                    collectCallSitesStmt(ch, out, ctx);
                }
            }
            break;
        }
        case ASTNodeType::IF_STMT: {
            const auto* st = static_cast<const IfStmt*>(s);
            collectCallSitesExpr(st->condition.get(), out, ctx);
            collectCallSitesStmt(st->thenBranch.get(), out, ctx);
            collectCallSitesStmt(st->elseBranch.get(), out, ctx);
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            const auto* st = static_cast<const WhileStmt*>(s);
            collectCallSitesExpr(st->condition.get(), out, ctx);
            collectCallSitesStmt(st->body.get(), out, ctx);
            break;
        }
        case ASTNodeType::DO_WHILE_STMT: {
            const auto* st = static_cast<const DoWhileStmt*>(s);
            collectCallSitesExpr(st->condition.get(), out, ctx);
            collectCallSitesStmt(st->body.get(), out, ctx);
            break;
        }
        case ASTNodeType::FOR_STMT: {
            const auto* st = static_cast<const ForStmt*>(s);
            collectCallSitesExpr(st->start.get(), out, ctx);
            collectCallSitesExpr(st->end.get(), out, ctx);
            collectCallSitesExpr(st->step.get(), out, ctx);
            collectCallSitesStmt(st->body.get(), out, ctx);
            break;
        }
        case ASTNodeType::FOR_EACH_STMT: {
            const auto* st = static_cast<const ForEachStmt*>(s);
            collectCallSitesExpr(st->collection.get(), out, ctx);
            collectCallSitesStmt(st->body.get(), out, ctx);
            break;
        }
        case ASTNodeType::RETURN_STMT:
            collectCallSitesExpr(
                static_cast<const ReturnStmt*>(s)->value.get(), out, ctx, true);
            break;
        case ASTNodeType::VAR_DECL:
            collectCallSitesExpr(
                static_cast<const VarDecl*>(s)->initializer.get(), out, ctx);
            break;
        case ASTNodeType::EXPR_STMT:
            collectCallSitesExpr(
                static_cast<const ExprStmt*>(s)->expression.get(), out, ctx);
            break;
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// collectLoopsStmt
// ─────────────────────────────────────────────────────────────────────────────
static void collectArrayAccessesStmt(
        const Statement* s,
        std::unordered_set<std::string>& readArrays,
        std::unordered_set<std::string>& writtenArrays) {
    if (!s) return;
    switch (s->type) {
        case ASTNodeType::BLOCK:
            for (const auto& ch : static_cast<const BlockStmt*>(s)->statements)
                collectArrayAccessesStmt(ch.get(), readArrays, writtenArrays);
            break;
        case ASTNodeType::EXPR_STMT: {
            // Detect index-assign: arr[i] = v
            const auto* es = static_cast<const ExprStmt*>(s);
            if (es->expression &&
                es->expression->type == ASTNodeType::INDEX_ASSIGN_EXPR) {
                const auto* ia =
                    static_cast<const IndexAssignExpr*>(es->expression.get());
                if (ia->array && ia->array->type == ASTNodeType::IDENTIFIER_EXPR)
                    writtenArrays.insert(
                        static_cast<const IdentifierExpr*>(ia->array.get())->name);
            } else if (es->expression &&
                       es->expression->type == ASTNodeType::INDEX_EXPR) {
                const auto* ie =
                    static_cast<const IndexExpr*>(es->expression.get());
                if (ie->array && ie->array->type == ASTNodeType::IDENTIFIER_EXPR)
                    readArrays.insert(
                        static_cast<const IdentifierExpr*>(ie->array.get())->name);
            }
            break;
        }
        case ASTNodeType::VAR_DECL: {
            const auto* vd = static_cast<const VarDecl*>(s);
            if (vd->initializer &&
                vd->initializer->type == ASTNodeType::INDEX_EXPR) {
                const auto* ie =
                    static_cast<const IndexExpr*>(vd->initializer.get());
                if (ie->array && ie->array->type == ASTNodeType::IDENTIFIER_EXPR)
                    readArrays.insert(
                        static_cast<const IdentifierExpr*>(ie->array.get())->name);
            }
            break;
        }
        default: break;
    }
}

static void collectLoopsStmt(
        const Statement* s,
        std::vector<SIRLoopInfo>& out,
        const std::unordered_map<std::string, ValueRange>& rangeMap,
        int depth) {
    if (!s || depth > 20) return;
    switch (s->type) {
        case ASTNodeType::FOR_STMT: {
            const auto* fs = static_cast<const ForStmt*>(s);
            SIRLoopInfo li;
            li.iterVar      = fs->iteratorVar;
            li.nestingDepth = depth;
            li.hints        = fs->loopHints;
            li.countingUp   = true;

            if (fs->start) {
                long long sv = 0;
                if (isIntLiteral(fs->start.get(), &sv)) li.staticStart = static_cast<int64_t>(sv);
            }
            if (fs->end) {
                long long ev = 0;
                if (isIntLiteral(fs->end.get(), &ev)) li.staticEnd = static_cast<int64_t>(ev);
            }
            if (fs->step) {
                long long step = 0;
                if (isIntLiteral(fs->step.get(), &step)) {
                    li.staticStep = static_cast<int64_t>(step);
                    li.countingUp = step > 0;
                }
            }
            li.hasConstBounds = li.staticStart.has_value() && li.staticEnd.has_value();

            if (li.hasConstBounds) {
                const int64_t s0   = *li.staticStart;
                const int64_t e0   = *li.staticEnd;
                const int64_t step = li.staticStep.value_or(1);
                if (step != 0) {
                    const int64_t diff = (step > 0) ? (e0 - s0) : (s0 - e0);
                    if (diff > 0) {
                        const int64_t abs_step = step > 0 ? step : -step;
                        li.tripCount = (diff + abs_step - 1) / abs_step;
                    } else {
                        li.tripCount = 0;
                    }
                    li.tripCountMax = li.tripCount;
                }
                li.isCountable = true;
            }

            const auto rit = rangeMap.find(fs->iteratorVar);
            if (rit != rangeMap.end())
                li.isIterVarNonNeg = rit->second.isNonNeg();

            std::unordered_set<std::string> ra, wa;
            collectArrayAccessesStmt(fs->body.get(), ra, wa);
            for (const auto& n : ra) li.readArrays.push_back(n);
            for (const auto& n : wa) li.writtenArrays.push_back(n);

            out.push_back(std::move(li));
            collectLoopsStmt(fs->body.get(), out, rangeMap, depth + 1);
            break;
        }
        case ASTNodeType::FOR_EACH_STMT: {
            const auto* fe = static_cast<const ForEachStmt*>(s);
            SIRLoopInfo li;
            li.iterVar      = fe->iteratorVar;
            li.nestingDepth = depth;
            li.hints        = fe->loopHints;
            li.isCountable  = false;
            std::unordered_set<std::string> ra, wa;
            collectArrayAccessesStmt(fe->body.get(), ra, wa);
            for (const auto& n : ra) li.readArrays.push_back(n);
            for (const auto& n : wa) li.writtenArrays.push_back(n);
            out.push_back(std::move(li));
            collectLoopsStmt(fe->body.get(), out, rangeMap, depth + 1);
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            const auto* ws = static_cast<const WhileStmt*>(s);
            SIRLoopInfo li;
            li.nestingDepth = depth;
            li.hints        = ws->loopHints;
            li.isCountable  = false;
            std::unordered_set<std::string> ra, wa;
            collectArrayAccessesStmt(ws->body.get(), ra, wa);
            for (const auto& n : ra) li.readArrays.push_back(n);
            for (const auto& n : wa) li.writtenArrays.push_back(n);
            out.push_back(std::move(li));
            collectLoopsStmt(ws->body.get(), out, rangeMap, depth + 1);
            break;
        }
        case ASTNodeType::DO_WHILE_STMT: {
            const auto* dw = static_cast<const DoWhileStmt*>(s);
            SIRLoopInfo li;
            li.nestingDepth = depth;
            li.hints        = dw->loopHints;
            li.isCountable  = false;
            std::unordered_set<std::string> ra, wa;
            collectArrayAccessesStmt(dw->body.get(), ra, wa);
            for (const auto& n : ra) li.readArrays.push_back(n);
            for (const auto& n : wa) li.writtenArrays.push_back(n);
            out.push_back(std::move(li));
            collectLoopsStmt(dw->body.get(), out, rangeMap, depth + 1);
            break;
        }
        case ASTNodeType::BLOCK:
            for (const auto& ch : static_cast<const BlockStmt*>(s)->statements)
                collectLoopsStmt(ch.get(), out, rangeMap, depth);
            break;
        case ASTNodeType::IF_STMT: {
            const auto* is = static_cast<const IfStmt*>(s);
            collectLoopsStmt(is->thenBranch.get(), out, rangeMap, depth);
            collectLoopsStmt(is->elseBranch.get(), out, rangeMap, depth);
            break;
        }
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// collectVarFactsStmt
// ─────────────────────────────────────────────────────────────────────────────
static void collectVarFactsStmt(
        const Statement* s,
        std::unordered_map<std::string, SIRVarFacts>& out,
        const std::unordered_map<std::string, ValueRange>& rangeMap,
        const std::unordered_set<std::string>& uniqueSet,
        const std::unordered_map<std::string, StructRepr>& structReprs) {
    if (!s) return;
    switch (s->type) {
        case ASTNodeType::VAR_DECL: {
            const auto* vd = static_cast<const VarDecl*>(s);
            if (out.count(vd->name)) break; // already seen
            SIRVarFacts vf;
            vf.type           = typeFromAnnotation(vd->typeName, structReprs);
            vf.type.isConst   = vd->isConst;
            vf.type.isAtomic  = vd->isAtomic;
            vf.type.isVolatile = vd->isVolatile;
            vf.isImmutable    = vd->isConst;
            vf.isAtomic       = vd->isAtomic;
            vf.isVolatile     = vd->isVolatile;
            // Range
            const auto rit = rangeMap.find(vd->name);
            if (rit != rangeMap.end()) {
                vf.range      = rit->second;
                vf.isNonNeg   = rit->second.isNonNeg();
                if (rit->second.isConst()) vf.constIntVal = rit->second.constVal();
            }
            // Compile-time constant from literal init
            if (!vf.constIntVal && vd->initializer) {
                long long v = 0;
                if (isIntLiteral(vd->initializer.get(), &v)) {
                    vf.constIntVal = static_cast<int64_t>(v);
                } else if (vd->initializer->type == ASTNodeType::LITERAL_EXPR) {
                    const auto* lit =
                        static_cast<const LiteralExpr*>(vd->initializer.get());
                    if (lit->literalType == LiteralExpr::LiteralType::STRING)
                        vf.constStrVal = lit->stringValue;
                }
            }
            // Uniqueness / aliasing
            vf.isUnique = uniqueSet.count(vd->name) > 0;
            vf.mayAlias = !vf.isUnique;
            out[vd->name] = std::move(vf);
            break;
        }
        case ASTNodeType::BLOCK:
            for (const auto& ch : static_cast<const BlockStmt*>(s)->statements)
                collectVarFactsStmt(ch.get(), out, rangeMap, uniqueSet, structReprs);
            break;
        case ASTNodeType::IF_STMT: {
            const auto* is = static_cast<const IfStmt*>(s);
            collectVarFactsStmt(is->thenBranch.get(), out, rangeMap, uniqueSet, structReprs);
            collectVarFactsStmt(is->elseBranch.get(), out, rangeMap, uniqueSet, structReprs);
            break;
        }
        case ASTNodeType::WHILE_STMT: {
            collectVarFactsStmt(
                static_cast<const WhileStmt*>(s)->body.get(),
                out, rangeMap, uniqueSet, structReprs);
            break;
        }
        case ASTNodeType::DO_WHILE_STMT: {
            collectVarFactsStmt(
                static_cast<const DoWhileStmt*>(s)->body.get(),
                out, rangeMap, uniqueSet, structReprs);
            break;
        }
        case ASTNodeType::FOR_STMT: {
            const auto* fs = static_cast<const ForStmt*>(s);
            // Induction variable
            if (!out.count(fs->iteratorVar)) {
                SIRVarFacts ivf;
                ivf.type    = SIRType::makeInt();
                const auto rit = rangeMap.find(fs->iteratorVar);
                if (rit != rangeMap.end()) {
                    ivf.range    = rit->second;
                    ivf.isNonNeg = rit->second.isNonNeg();
                    if (rit->second.isConst()) ivf.constIntVal = rit->second.constVal();
                }
                out[fs->iteratorVar] = std::move(ivf);
            }
            collectVarFactsStmt(fs->body.get(), out, rangeMap, uniqueSet, structReprs);
            break;
        }
        case ASTNodeType::FOR_EACH_STMT: {
            const auto* fe = static_cast<const ForEachStmt*>(s);
            if (!out.count(fe->iteratorVar)) {
                SIRVarFacts ivf;
                ivf.type = SIRType::makeUnknown();
                out[fe->iteratorVar] = std::move(ivf);
            }
            collectVarFactsStmt(fe->body.get(), out, rangeMap, uniqueSet, structReprs);
            break;
        }
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// buildSIR — main entry point
// ─────────────────────────────────────────────────────────────────────────────
SIRModule buildSIR(
    const Program&  program,
    const OptimizationContext& ctx,
    const std::unordered_map<std::string,
                             std::unordered_map<std::string, ValueRange>>& rangeMap,
    const std::unordered_map<std::string,
                             std::unordered_set<std::string>>& uniqueSets,
    const std::unordered_map<std::string, std::vector<StructField>>& structFieldDecls,
    const std::unordered_map<std::string, StructRepr>&  structReprs,
    const std::unordered_map<std::string, int>&         structReprAlignN)
{
    SIRModule sir;

    // ── 1. Struct type table ──────────────────────────────────────────────
    // First from the structFieldDecls map passed by CodeGenerator
    for (const auto& [sname, fields] : structFieldDecls) {
        SIRStructType sty;
        sty.name = sname;
        const auto reprIt = structReprs.find(sname);
        sty.repr = (reprIt != structReprs.end()) ? reprIt->second : StructRepr::Auto;
        const auto alignIt = structReprAlignN.find(sname);
        sty.reprAlignN = (alignIt != structReprAlignN.end()) ? alignIt->second : 0;
        sty.hasSoAGroup = (sty.repr == StructRepr::SoA ||
                           sty.repr == StructRepr::AosToSoa);
        sty.isPacked    = (sty.repr == StructRepr::Packed);
        sty.isExternC   = (sty.repr == StructRepr::C);
        for (const auto& f : fields) {
            SIRStructField sf;
            sf.name  = f.name;
            sf.type  = typeFromAnnotation(f.typeName, structReprs);
            sf.attrs = f.attrs;
            sty.fields.push_back(std::move(sf));
        }
        sir.structTypes[sname] = std::move(sty);
    }
    // Also from AST struct declarations (may have no corresponding field-decl map entry)
    for (const auto& sdecl : program.structs) {
        if (!sdecl) continue;
        if (sir.structTypes.count(sdecl->name)) continue; // already added
        SIRStructType sty;
        sty.name = sdecl->name;
        sty.repr = sdecl->repr;
        sty.reprAlignN = sdecl->reprAlignN;
        sty.hasSoAGroup = (sty.repr == StructRepr::SoA ||
                           sty.repr == StructRepr::AosToSoa);
        sty.isPacked    = (sty.repr == StructRepr::Packed);
        sty.isExternC   = (sty.repr == StructRepr::C);
        for (const auto& f : sdecl->fieldDecls) {
            SIRStructField sf;
            sf.name  = f.name;
            sf.type  = typeFromAnnotation(f.typeName, structReprs);
            sf.attrs = f.attrs;
            sty.fields.push_back(std::move(sf));
        }
        sir.structTypes[sdecl->name] = std::move(sty);
    }

    // ── 2. Per-function analysis ──────────────────────────────────────────
    for (const auto& decl : program.functions) {
        const FunctionDecl* fn = decl.get();
        if (!fn) continue;

        SIRFunction sf;
        sf.name = fn->name;

        // ── 2a. Signature ─────────────────────────────────────────────────
        sf.returnType = typeFromAnnotation(fn->returnType, structReprs);
        for (const auto& p : fn->parameters) {
            SIRParam sp;
            sp.name  = p.name;
            sp.type  = typeFromAnnotation(p.typeName, structReprs);
            sp.isConst = false; // Parameter doesn't carry isConst directly
            // Range for parameter
            const auto& fnRangeMap = rangeMap.count(fn->name)
                ? rangeMap.at(fn->name)
                : std::unordered_map<std::string, ValueRange>{};
            const auto rit = fnRangeMap.find(p.name);
            if (rit != fnRangeMap.end()) sp.range = rit->second;
            sf.params.push_back(std::move(sp));
        }

        // ── 2b. Consolidated FunctionFacts ────────────────────────────────
        sf.facts = ctx.getFacts(fn->name);

        // ── 2c. Annotation hints ──────────────────────────────────────────
        sf.forceInline  = fn->hintInline;
        sf.neverInline  = fn->hintNoInline;
        sf.isHot        = fn->hintHot;
        sf.isCold       = fn->hintCold;
        sf.neverReturns = fn->hintNoReturn;
        sf.alwaysReturns = !sf.neverReturns;
        sf.isEntry      = (fn->name == "main");

        // ── 2d. Code-shape analysis ───────────────────────────────────────
        if (fn->body) {
            sf.estimatedBodySize = estimateStmtSize(fn->body.get());
            // Detect direct self-recursion via callee set
            collectCalleesStmt(fn->body.get(), sf.directCallees);
            sf.isRecursive = sf.directCallees.count(fn->name) > 0;
            sf.isLeaf      = sf.directCallees.empty();
        }

        // ── 2e. Per-variable facts ────────────────────────────────────────
        const auto& fnRangeMap = rangeMap.count(fn->name)
            ? rangeMap.at(fn->name)
            : std::unordered_map<std::string, ValueRange>{};
        const auto& fnUniqueSet = uniqueSets.count(fn->name)
            ? uniqueSets.at(fn->name)
            : std::unordered_set<std::string>{};

        if (fn->body)
            collectVarFactsStmt(fn->body.get(), sf.varFacts,
                                 fnRangeMap, fnUniqueSet, structReprs);

        // Parameters as var-facts entries
        for (const auto& p : fn->parameters) {
            if (sf.varFacts.count(p.name)) continue;
            SIRVarFacts pvf;
            pvf.type = typeFromAnnotation(p.typeName, structReprs);
            const auto rit = fnRangeMap.find(p.name);
            if (rit != fnRangeMap.end()) {
                pvf.range    = rit->second;
                pvf.isNonNeg = rit->second.isNonNeg();
                if (rit->second.isConst()) pvf.constIntVal = rit->second.constVal();
            }
            pvf.isUnique = fnUniqueSet.count(p.name) > 0;
            pvf.mayAlias = !pvf.isUnique;
            sf.varFacts[p.name] = std::move(pvf);
        }

        // ── 2f. Call sites ────────────────────────────────────────────────
        if (fn->body)
            collectCallSitesStmt(fn->body.get(), sf.callSites, ctx);

        // ── 2g. Loop info ─────────────────────────────────────────────────
        if (fn->body)
            collectLoopsStmt(fn->body.get(), sf.loops, fnRangeMap, 0);

        sir.functions[fn->name] = std::move(sf);
    }

    // ── 3. Call graph ─────────────────────────────────────────────────────
    for (const auto& [fname, sfn] : sir.functions) {
        sir.callGraph[fname] = sfn.directCallees;
        for (const auto& callee : sfn.directCallees)
            sir.callerGraph[callee].insert(fname);
        if (sfn.isEntry) sir.entryPoints.insert(fname);
    }

    // ── 4. Module-level metrics ───────────────────────────────────────────
    sir.totalFunctions = static_cast<unsigned>(sir.functions.size());
    for (const auto& [fname, sfn] : sir.functions)
        sir.estimatedTotalSize += sfn.estimatedBodySize;

    return sir;
}

} // namespace omscript
