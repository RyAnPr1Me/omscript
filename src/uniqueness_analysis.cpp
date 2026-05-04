/// @file uniqueness_analysis.cpp
/// @brief Ownership-Aware Uniqueness Analysis — implementation.
///
/// See uniqueness_analysis.h for a full description of the algorithm.

#include "uniqueness_analysis.h"
#include "pass_utils.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace omscript {

// ─────────────────────────────────────────────────────────────────────────────
// Helper: known string-producing builtin names
// ─────────────────────────────────────────────────────────────────────────────
//
// A call to any of these builtins always allocates and returns a *fresh* string
// buffer — the result cannot alias any pre-existing variable's buffer.

static bool isKnownFreshStringBuiltin(const std::string& callee) noexcept {
    // Use a sorted array + binary search for O(log n) lookup.
    static constexpr const char* kNames[] = {
        "format", "input", "str_concat", "str_format", "str_join",
        "str_lower", "str_ltrim", "str_pad_left", "str_pad_right",
        "str_repeat", "str_replace", "str_reverse", "str_rtrim",
        "str_slice", "str_trim", "str_upper", "substr", "to_string",
    };
    static constexpr size_t kN = sizeof(kNames) / sizeof(kNames[0]);
    // The array is sorted — binary search.
    const char** lo = kNames;
    const char** hi = kNames + kN;
    while (lo < hi) {
        const char** mid = lo + (hi - lo) / 2;
        const int cmp = callee.compare(*mid);
        if (cmp == 0) return true;
        if (cmp < 0) hi = mid;
        else         lo = mid + 1;
    }
    return false;
}

/// True for known array-allocating builtins that always return a fresh buffer.
static bool isKnownFreshArrayBuiltin(const std::string& callee) noexcept {
    static constexpr const char* kNames[] = {
        "array_concat", "array_copy", "array_fill", "array_filter",
        "array_map", "array_remove", "array_slice", "pop",
        "push", "reverse", "sort", "str_chars", "str_split", "unshift",
    };
    static constexpr size_t kN = sizeof(kNames) / sizeof(kNames[0]);
    const char** lo = kNames;
    const char** hi = kNames + kN;
    while (lo < hi) {
        const char** mid = lo + (hi - lo) / 2;
        const int cmp = callee.compare(*mid);
        if (cmp == 0) return true;
        if (cmp < 0) hi = mid;
        else         lo = mid + 1;
    }
    return false;
}

/// True when calling @p callee is known to NOT capture any string/array
/// argument (read-only or IO-only, never stores the pointer).
static bool isKnownNonCapturingBuiltin(const std::string& callee) noexcept {
    // Read-only memory builtins + output-only builtins.
    static constexpr const char* kNames[] = {
        "assert", "contains", "ends_with", "eprint", "eprintln",
        "find", "is_null", "len", "print", "println",
        "starts_with", "str_contains", "str_count", "str_ends_with",
        "str_find", "str_index_of", "str_starts_with", "type_of",
    };
    static constexpr size_t kN = sizeof(kNames) / sizeof(kNames[0]);
    const char** lo = kNames;
    const char** hi = kNames + kN;
    while (lo < hi) {
        const char** mid = lo + (hi - lo) / 2;
        const int cmp = callee.compare(*mid);
        if (cmp == 0) return true;
        if (cmp < 0) hi = mid;
        else         lo = mid + 1;
    }
    // Also consult the BuiltinEffectTable: if it's a known read-only or IO-only
    // builtin (writesMemory = false, allocates = false), it can't capture.
    if (BuiltinEffectTable::contains(callee)) {
        const auto& e = BuiltinEffectTable::get(callee);
        return !e.writesMemory && !e.allocates;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: classify expression — fresh allocation vs. alias
// ─────────────────────────────────────────────────────────────────────────────
//
// An expression is *fresh* when evaluating it always produces a newly-allocated
// buffer that cannot alias any pre-existing variable.
// An expression is an *alias* when evaluating it reads an existing variable's
// pointer without copying the underlying data.

/// Returns true if @p expr evaluates to a freshly-allocated string buffer.
/// @p strVars  — names of variables currently known to hold string pointers.
/// @p arrVars  — names of variables currently known to hold array pointers.
static bool isFreshStringExpr(const Expression* expr,
                               const std::unordered_set<std::string>& strVars,
                               const std::unordered_set<std::string>& arrVars) noexcept;

/// Returns true if @p expr evaluates to a freshly-allocated array buffer.
static bool isFreshArrayExpr(const Expression* expr,
                              const std::unordered_set<std::string>& strVars,
                              const std::unordered_set<std::string>& arrVars) noexcept;

static bool isFreshStringExpr(const Expression* expr,
                               const std::unordered_set<std::string>& strVars,
                               const std::unordered_set<std::string>& arrVars) noexcept {
    if (!expr) return false;
    switch (expr->type) {
    case ASTNodeType::LITERAL_EXPR: {
        const auto* lit = static_cast<const LiteralExpr*>(expr);
        // String literals → fresh malloc'd buffer.
        return lit->literalType == LiteralExpr::LiteralType::STRING;
    }
    case ASTNodeType::IDENTIFIER_EXPR: {
        // Identifier of an existing string/array variable → alias (NOT fresh).
        const auto* id = static_cast<const IdentifierExpr*>(expr);
        return !strVars.count(id->name) && !arrVars.count(id->name);
    }
    case ASTNodeType::BINARY_EXPR: {
        const auto* bin = static_cast<const BinaryExpr*>(expr);
        // str + x  or  str * n  → new malloc'd buffer (always fresh).
        if (bin->op == "+" || bin->op == "*") {
            // A binary op involving at least one string operand produces a new
            // buffer even if both operands are existing string variables.
            const bool leftStr  = isFreshStringExpr(bin->left.get(),  strVars, arrVars)
                || (bin->left->type  == ASTNodeType::IDENTIFIER_EXPR && strVars.count(
                    static_cast<const IdentifierExpr*>(bin->left.get())->name));
            const bool rightStr = isFreshStringExpr(bin->right.get(), strVars, arrVars)
                || (bin->right->type == ASTNodeType::IDENTIFIER_EXPR && strVars.count(
                    static_cast<const IdentifierExpr*>(bin->right.get())->name));
            if (leftStr || rightStr)
                return true;  // The result is a freshly malloc'd concatenation.
        }
        return false;
    }
    case ASTNodeType::TERNARY_EXPR: {
        // Fresh if BOTH branches produce fresh strings (conservative).
        const auto* tern = static_cast<const TernaryExpr*>(expr);
        return isFreshStringExpr(tern->thenExpr.get(), strVars, arrVars) &&
               isFreshStringExpr(tern->elseExpr.get(), strVars, arrVars);
    }
    case ASTNodeType::CALL_EXPR: {
        const auto* call = static_cast<const CallExpr*>(expr);
        if (isKnownFreshStringBuiltin(call->callee)) return true;
        // User-defined functions: conservatively assume they may return an
        // alias of one of their arguments → NOT provably fresh.
        return false;
    }
    default:
        return false;
    }
}

static bool isFreshArrayExpr(const Expression* expr,
                              const std::unordered_set<std::string>& strVars,
                              const std::unordered_set<std::string>& arrVars) noexcept {
    if (!expr) return false;
    switch (expr->type) {
    case ASTNodeType::ARRAY_EXPR:
        return true;  // Array literal → fresh malloc'd buffer.
    case ASTNodeType::IDENTIFIER_EXPR: {
        const auto* id = static_cast<const IdentifierExpr*>(expr);
        return !arrVars.count(id->name) && !strVars.count(id->name);
    }
    case ASTNodeType::CALL_EXPR: {
        const auto* call = static_cast<const CallExpr*>(expr);
        if (isKnownFreshArrayBuiltin(call->callee)) return true;
        return false;
    }
    case ASTNodeType::TERNARY_EXPR: {
        const auto* tern = static_cast<const TernaryExpr*>(expr);
        return isFreshArrayExpr(tern->thenExpr.get(), strVars, arrVars) &&
               isFreshArrayExpr(tern->elseExpr.get(), strVars, arrVars);
    }
    default:
        return false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 1: collectStringArrayVars — discover all string/array var names
// ─────────────────────────────────────────────────────────────────────────────

/// Recursively scan @p block and add any variable name whose initialiser
/// produces a string or array to @p strVars / @p arrVars.
static void collectStringArrayVars(
    const Statement*                 stmt,
    std::unordered_set<std::string>& strVars,
    std::unordered_set<std::string>& arrVars) {

    if (!stmt) return;

    if (const auto* blk = dynamic_cast<const BlockStmt*>(stmt)) {
        for (const auto& s : blk->statements)
            collectStringArrayVars(s.get(), strVars, arrVars);
        return;
    }

    if (const auto* vd = dynamic_cast<const VarDecl*>(stmt)) {
        if (vd->initializer) {
            // String type annotation always overrides
            if (vd->typeName == "string" || vd->typeName == "str") {
                strVars.insert(vd->name);
            } else if (isFreshStringExpr(vd->initializer.get(), strVars, arrVars)) {
                strVars.insert(vd->name);
            } else if (vd->initializer->type == ASTNodeType::IDENTIFIER_EXPR) {
                const auto* id = static_cast<const IdentifierExpr*>(vd->initializer.get());
                if (strVars.count(id->name))
                    strVars.insert(vd->name);  // propagate type via alias
                else if (arrVars.count(id->name))
                    arrVars.insert(vd->name);
            } else if (isFreshArrayExpr(vd->initializer.get(), strVars, arrVars)) {
                arrVars.insert(vd->name);
            }
        }
        return;  // No nested body to scan in a VarDecl
    }

    // Recurse into compound statements
    if (const auto* ifc = dynamic_cast<const IfStmt*>(stmt)) {
        collectStringArrayVars(ifc->thenBranch.get(), strVars, arrVars);
        if (ifc->elseBranch)
            collectStringArrayVars(ifc->elseBranch.get(), strVars, arrVars);
        return;
    }
    if (const auto* ws = dynamic_cast<const WhileStmt*>(stmt)) {
        collectStringArrayVars(ws->body.get(), strVars, arrVars);
        return;
    }
    if (const auto* dw = dynamic_cast<const DoWhileStmt*>(stmt)) {
        collectStringArrayVars(dw->body.get(), strVars, arrVars);
        return;
    }
    if (const auto* fs = dynamic_cast<const ForStmt*>(stmt)) {
        collectStringArrayVars(fs->body.get(), strVars, arrVars);
        return;
    }
    if (const auto* fes = dynamic_cast<const ForEachStmt*>(stmt)) {
        collectStringArrayVars(fes->body.get(), strVars, arrVars);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Phase 2: markSharedVars — find aliasing operations that kill uniqueness
// ─────────────────────────────────────────────────────────────────────────────

/// Recursively scan @p stmt and add any string/array variable name that
/// appears in an *aliasing position* to @p sharedVars.
static void markSharedVars(
    const Statement*                       stmt,
    const std::unordered_set<std::string>& strVars,
    const std::unordered_set<std::string>& arrVars,
    std::unordered_set<std::string>&       sharedVars) {

    if (!stmt) return;

    // ── Helper lambda: check a single expression used as an *argument* ────
    // An expression in argument position potentially captures the pointer.
    // We only mark identifiers of known string/array vars as Shared.
    const auto markArgShared = [&](const Expression* argExpr,
                                   const std::string& callee) {
        if (!argExpr) return;
        if (argExpr->type != ASTNodeType::IDENTIFIER_EXPR) return;
        const auto* id = static_cast<const IdentifierExpr*>(argExpr);
        if (!strVars.count(id->name) && !arrVars.count(id->name)) return;
        // Known non-capturing builtins are exempt.
        if (isKnownNonCapturingBuiltin(callee)) return;
        sharedVars.insert(id->name);
    };

    // ── Helper lambda: scan an expression for aliasing stores to strVars ──
    // Recursively inspect an expression for sub-expressions that put a
    // string/array variable into a "captured" position.
    std::function<void(const Expression*)> scanExprForAliases;
    scanExprForAliases = [&](const Expression* expr) {
        if (!expr) return;
        // Index assignment arr[i] = x — x is stored into an array.
        if (expr->type == ASTNodeType::INDEX_ASSIGN_EXPR) {
            const auto* ia = static_cast<const IndexAssignExpr*>(expr);
            if (ia->value && ia->value->type == ASTNodeType::IDENTIFIER_EXPR) {
                const auto* id = static_cast<const IdentifierExpr*>(ia->value.get());
                if (strVars.count(id->name) || arrVars.count(id->name))
                    sharedVars.insert(id->name);
            }
            scanExprForAliases(ia->array.get());
            scanExprForAliases(ia->index.get());
            scanExprForAliases(ia->value.get());
            return;
        }
        // Field assignment obj.field = x — x may be captured in a struct.
        if (expr->type == ASTNodeType::FIELD_ASSIGN_EXPR) {
            const auto* fa = static_cast<const FieldAssignExpr*>(expr);
            if (fa->value && fa->value->type == ASTNodeType::IDENTIFIER_EXPR) {
                const auto* id = static_cast<const IdentifierExpr*>(fa->value.get());
                if (strVars.count(id->name) || arrVars.count(id->name))
                    sharedVars.insert(id->name);
            }
            scanExprForAliases(fa->value.get());
            return;
        }
        // Assign x = y where y is a string/array var → both become Shared.
        if (expr->type == ASTNodeType::ASSIGN_EXPR) {
            const auto* ae = static_cast<const AssignExpr*>(expr);
            if (ae->value && ae->value->type == ASTNodeType::IDENTIFIER_EXPR) {
                const auto* id = static_cast<const IdentifierExpr*>(ae->value.get());
                if (strVars.count(id->name) || arrVars.count(id->name)) {
                    if (strVars.count(ae->name) || arrVars.count(ae->name))
                        sharedVars.insert(id->name);  // source becomes Shared
                }
            }
            // Don't recurse further (we've checked what we need).
            return;
        }
        // Call expression — check each string/array argument.
        if (expr->type == ASTNodeType::CALL_EXPR) {
            const auto* call = static_cast<const CallExpr*>(expr);
            for (const auto& arg : call->arguments)
                markArgShared(arg.get(), call->callee);
            return;
        }
        // Recurse into sub-expressions (binary, unary, ternary, etc.)
        if (expr->type == ASTNodeType::BINARY_EXPR) {
            const auto* bin = static_cast<const BinaryExpr*>(expr);
            // For binary concat x + y, neither x nor y is captured.
            // But for other binary ops we recurse.
            if (bin->op != "+" && bin->op != "*")
                scanExprForAliases(bin->left.get());
            scanExprForAliases(bin->right.get());
            return;
        }
        if (expr->type == ASTNodeType::UNARY_EXPR) {
            scanExprForAliases(static_cast<const UnaryExpr*>(expr)->operand.get());
            return;
        }
        if (expr->type == ASTNodeType::TERNARY_EXPR) {
            const auto* t = static_cast<const TernaryExpr*>(expr);
            scanExprForAliases(t->condition.get());
            scanExprForAliases(t->thenExpr.get());
            scanExprForAliases(t->elseExpr.get());
            return;
        }
    };

    // ── Dispatch by statement type ─────────────────────────────────────────

    if (const auto* blk = dynamic_cast<const BlockStmt*>(stmt)) {
        for (const auto& s : blk->statements)
            markSharedVars(s.get(), strVars, arrVars, sharedVars);
        return;
    }

    if (const auto* vd = dynamic_cast<const VarDecl*>(stmt)) {
        // var y = x  where x is a string/array var → x becomes Shared.
        if (vd->initializer && vd->initializer->type == ASTNodeType::IDENTIFIER_EXPR) {
            const auto* id = static_cast<const IdentifierExpr*>(vd->initializer.get());
            if (strVars.count(id->name) || arrVars.count(id->name)) {
                sharedVars.insert(id->name);  // source becomes Shared
                // The new variable is also an alias → Shared.
                sharedVars.insert(vd->name);
            }
        }
        // var y = call(x) — scan arguments for captures.
        if (vd->initializer && vd->initializer->type == ASTNodeType::CALL_EXPR) {
            const auto* call = static_cast<const CallExpr*>(vd->initializer.get());
            for (const auto& arg : call->arguments)
                markArgShared(arg.get(), call->callee);
        }
        return;
    }

    if (const auto* es = dynamic_cast<const ExprStmt*>(stmt)) {
        scanExprForAliases(es->expression.get());
        return;
    }

    if (const auto* rs = dynamic_cast<const ReturnStmt*>(stmt)) {
        // Return transfers ownership — does NOT create a second live reference
        // within this function; we do not mark the returned variable as Shared.
        // (The caller receiving it is a different scope.)
        (void)rs;
        return;
    }

    // Recurse into compound statements
    if (const auto* ifc = dynamic_cast<const IfStmt*>(stmt)) {
        scanExprForAliases(ifc->condition.get());
        markSharedVars(ifc->thenBranch.get(), strVars, arrVars, sharedVars);
        if (ifc->elseBranch)
            markSharedVars(ifc->elseBranch.get(), strVars, arrVars, sharedVars);
        return;
    }
    if (const auto* ws = dynamic_cast<const WhileStmt*>(stmt)) {
        scanExprForAliases(ws->condition.get());
        markSharedVars(ws->body.get(), strVars, arrVars, sharedVars);
        return;
    }
    if (const auto* dw = dynamic_cast<const DoWhileStmt*>(stmt)) {
        markSharedVars(dw->body.get(), strVars, arrVars, sharedVars);
        scanExprForAliases(dw->condition.get());
        return;
    }
    if (const auto* fs = dynamic_cast<const ForStmt*>(stmt)) {
        scanExprForAliases(fs->start.get());
        scanExprForAliases(fs->end.get());
        if (fs->step) scanExprForAliases(fs->step.get());
        markSharedVars(fs->body.get(), strVars, arrVars, sharedVars);
        return;
    }
    if (const auto* fes = dynamic_cast<const ForEachStmt*>(stmt)) {
        scanExprForAliases(fes->iterable.get());
        markSharedVars(fes->body.get(), strVars, arrVars, sharedVars);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// computeUniqueness — public entry point
// ─────────────────────────────────────────────────────────────────────────────

UniquenessAnalysis computeUniqueness(
    const FunctionDecl&                    fn,
    const std::unordered_set<std::string>& strVarHints,
    const std::unordered_set<std::string>& arrVarHints)
{
    // ── Phase 0: seed with hints from pre-analysis ─────────────────────────
    std::unordered_set<std::string> strVars = strVarHints;
    std::unordered_set<std::string> arrVars = arrVarHints;

    // ── Phase 1: discover additional string/array variable names ──────────
    //
    // Run two passes to handle forward references (e.g. `var b = a` where
    // a's type is determined later in the function).  Two passes are sufficient
    // because each pass can only add new facts (monotone).
    for (int pass = 0; pass < 2; ++pass) {
        if (fn.body)
            collectStringArrayVars(fn.body.get(), strVars, arrVars);
    }

    // ── Phase 2: initialise Shared set with function parameters ───────────
    //
    // Parameters default to Shared: the caller may retain a reference.
    std::unordered_set<std::string> sharedVars;
    for (const auto& param : fn.parameters) {
        if (strVars.count(param.name) || arrVars.count(param.name) ||
            param.typeName == "string" || param.typeName == "str" ||
            (!param.typeName.empty() && param.typeName.back() == ']')) {
            sharedVars.insert(param.name);
        }
    }

    // ── Phase 3: scan function body for aliasing operations ───────────────
    if (fn.body)
        markSharedVars(fn.body.get(), strVars, arrVars, sharedVars);

    // ── Phase 4: build result — Unique = (strVars ∪ arrVars) \ sharedVars ──
    UniquenessAnalysis result;
    for (const auto& name : strVars) {
        if (!sharedVars.count(name))
            result.uniqueVars.insert(name);
    }
    for (const auto& name : arrVars) {
        if (!sharedVars.count(name))
            result.uniqueVars.insert(name);
    }
    return result;
}

} // namespace omscript
