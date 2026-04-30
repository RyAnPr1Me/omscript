/// Implements deterministic, fuel-bounded, memoised compile-time evaluation

#include "cfctre.h"
#include "ast.h"
#include "opt_context.h"
#include "synthesize.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace omscript {

// ═══════════════════════════════════════════════════════════════════════════

CTValue CTValue::fromU64(uint64_t v) noexcept {
    CTValue r;
    r.kind = CTValueKind::CONCRETE_U64;
    r.scalar.u64 = v;
    return r;
}

CTValue CTValue::fromI64(int64_t v) noexcept {
    CTValue r;
    r.kind = CTValueKind::CONCRETE_I64;
    r.scalar.i64 = v;
    return r;
}

CTValue CTValue::fromF64(double v) noexcept {
    CTValue r;
    r.kind = CTValueKind::CONCRETE_F64;
    r.scalar.f64 = v;
    return r;
}

CTValue CTValue::fromBool(bool v) noexcept {
    CTValue r;
    r.kind = CTValueKind::CONCRETE_BOOL;
    r.scalar.b = v;
    return r;
}

CTValue CTValue::fromString(std::string s) {
    CTValue r;
    r.kind = CTValueKind::CONCRETE_STRING;
    r.str  = std::move(s);
    return r;
}

CTValue CTValue::fromArray(CTArrayHandle h) noexcept {
    CTValue r;
    r.kind = CTValueKind::CONCRETE_ARRAY;
    r.arr  = h;
    return r;
}

int64_t CTValue::asI64() const noexcept {
    switch (kind) {
    case CTValueKind::CONCRETE_I64:  return scalar.i64;
    case CTValueKind::CONCRETE_U64:  return static_cast<int64_t>(scalar.u64);
    case CTValueKind::CONCRETE_BOOL: return scalar.b ? 1 : 0;
    case CTValueKind::CONCRETE_F64:  return static_cast<int64_t>(scalar.f64);
    default: return 0;
    }
}

uint64_t CTValue::asU64() const noexcept {
    switch (kind) {
    case CTValueKind::CONCRETE_U64:  return scalar.u64;
    case CTValueKind::CONCRETE_I64:  return static_cast<uint64_t>(scalar.i64);
    case CTValueKind::CONCRETE_BOOL: return scalar.b ? 1 : 0;
    case CTValueKind::CONCRETE_F64:  return static_cast<uint64_t>(scalar.f64);
    default: return 0;
    }
}

double CTValue::asF64() const noexcept {
    switch (kind) {
    case CTValueKind::CONCRETE_F64:  return scalar.f64;
    case CTValueKind::CONCRETE_I64:  return static_cast<double>(scalar.i64);
    case CTValueKind::CONCRETE_U64:  return static_cast<double>(scalar.u64);
    case CTValueKind::CONCRETE_BOOL: return scalar.b ? 1.0 : 0.0;
    default: return 0.0;
    }
}

bool CTValue::asBool() const noexcept {
    switch (kind) {
    case CTValueKind::CONCRETE_BOOL: return scalar.b;
    case CTValueKind::CONCRETE_I64:  return scalar.i64 != 0;
    case CTValueKind::CONCRETE_U64:  return scalar.u64 != 0;
    case CTValueKind::CONCRETE_F64:  return scalar.f64 != 0.0;
    case CTValueKind::CONCRETE_STRING: return !str.empty();
    case CTValueKind::CONCRETE_ARRAY:  return arr != CT_NULL_HANDLE;
    default: return false;
    }
}

const std::string& CTValue::asStr() const {
    return str;
}

CTArrayHandle CTValue::asArr() const noexcept {
    return arr;
}

bool CTValue::isTruthy() const noexcept {
    return asBool();
}

// Append a compact, deterministic representation to `out` for memoisation.
void CTValue::appendMemoHash(std::string& out) const {
    char buf[32];                           // enough for "X:" + 20-digit int64
    switch (kind) {
    case CTValueKind::CONCRETE_I64: {
        const int n = std::snprintf(buf, sizeof(buf), "I:%lld",
                              static_cast<long long>(scalar.i64));
        out.append(buf, static_cast<size_t>(n));
        return;
    }
    case CTValueKind::CONCRETE_U64: {
        const int n = std::snprintf(buf, sizeof(buf), "U:%llu",
                              static_cast<unsigned long long>(scalar.u64));
        out.append(buf, static_cast<size_t>(n));
        return;
    }
    case CTValueKind::CONCRETE_F64: {
        const int n = std::snprintf(buf, sizeof(buf), "F:%.17g", scalar.f64);
        out.append(buf, static_cast<size_t>(n));
        return;
    }
    case CTValueKind::CONCRETE_BOOL:
        out.append(scalar.b ? "B:1" : "B:0", 3);
        return;
    case CTValueKind::CONCRETE_STRING:
        out.append("S:", 2);
        out.append(str);
        return;
    case CTValueKind::CONCRETE_ARRAY: {
        const int n = std::snprintf(buf, sizeof(buf), "A:%llu",
                              static_cast<unsigned long long>(arr));
        out.append(buf, static_cast<size_t>(n));
        return;
    }
    case CTValueKind::UNINITIALIZED:
        out.push_back('?');
        return;
    case CTValueKind::SYMBOLIC: {
        // Include the symId so that different symbolic variables produce different
        // cache keys (needed for the partial-specialisation cache).
        char buf[16];
        const int n = std::snprintf(buf, sizeof(buf), "SYM:%u", symId);
        out.append(buf, static_cast<size_t>(n));
        return;
    }
    }
    out.push_back('?');
}

std::string CTValue::memoHash() const {
    std::string result;
    result.reserve(24);                     // typical: "I:-1234567890" fits in SSO
    appendMemoHash(result);
    return result;
}

// ── Global symbolic-ID counter ────────────────────────────────────────────────
static thread_local uint32_t g_symIdCounter{0};

CTValue CTValue::symbolic() noexcept {
    CTValue r;
    r.kind  = CTValueKind::SYMBOLIC;
    r.symId = ++g_symIdCounter;
    return r;
}

bool CTValue::operator==(const CTValue& o) const noexcept {
    if (kind != o.kind) return false;
    switch (kind) {
    case CTValueKind::CONCRETE_I64:    return scalar.i64 == o.scalar.i64;
    case CTValueKind::CONCRETE_U64:    return scalar.u64 == o.scalar.u64;
    case CTValueKind::CONCRETE_F64:    return scalar.f64 == o.scalar.f64;
    case CTValueKind::CONCRETE_BOOL:   return scalar.b   == o.scalar.b;
    case CTValueKind::CONCRETE_STRING: return str == o.str;
    case CTValueKind::CONCRETE_ARRAY:  return arr == o.arr;
    case CTValueKind::UNINITIALIZED:   return true;
    case CTValueKind::SYMBOLIC:
        // Two symbolic values are "equal" iff they refer to the same symbolic
        return symId != 0 && symId == o.symId;
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════

CTArray::CTArray(uint64_t n, const CTValue& fill)
    : len(n), data(static_cast<size_t>(n), fill) {}

// ═══════════════════════════════════════════════════════════════════════════

CTArrayHandle CTHeap::alloc(uint64_t n, const CTValue& fill) {
    CTArrayHandle h = nextHandle_++;
    arrays_.emplace(h, CTArray(n, fill));
    return h;
}

CTValue CTHeap::load(CTArrayHandle h, int64_t idx) const {
    auto it = arrays_.find(h);
    if (it == arrays_.end()) return CTValue::uninit();
    const CTArray& arr = it->second;
    if (idx < 0 || static_cast<uint64_t>(idx) >= arr.len) return CTValue::uninit();
    return arr.data[static_cast<size_t>(idx)];
}

void CTHeap::store(CTArrayHandle h, int64_t idx, CTValue val) {
    auto it = arrays_.find(h);
    if (it == arrays_.end()) return;
    CTArray& arr = it->second;
    if (idx < 0 || static_cast<uint64_t>(idx) >= arr.len) return;
    arr.data[static_cast<size_t>(idx)] = std::move(val);
    mutableHandles_.insert(h);   // Phase E: mark written
}

bool CTHeap::push(CTArrayHandle h, CTValue val) {
    auto it = arrays_.find(h);
    if (it == arrays_.end()) return false;
    CTArray& arr = it->second;
    arr.data.push_back(std::move(val));
    ++arr.len;
    mutableHandles_.insert(h);   // Phase E: mark written
    return true;
}

uint64_t CTHeap::length(CTArrayHandle h) const {
    auto it = arrays_.find(h);
    return it != arrays_.end() ? it->second.len : 0;
}

bool CTHeap::exists(CTArrayHandle h) const {
    return arrays_.find(h) != arrays_.end();
}

void CTHeap::freeArray(CTArrayHandle h) {
    arrays_.erase(h);
    mutableHandles_.erase(h);   // Phase E: clean up mutability record
}

bool CTHeap::isImmutable(CTArrayHandle h) const noexcept {
    return arrays_.count(h) && !mutableHandles_.count(h);
}

const CTArray* CTHeap::get(CTArrayHandle h) const {
    auto it = arrays_.find(h);
    return it != arrays_.end() ? &it->second : nullptr;
}

CTArray* CTHeap::getMut(CTArrayHandle h) {
    auto it = arrays_.find(h);
    return it != arrays_.end() ? &it->second : nullptr;
}

// ═══════════════════════════════════════════════════════════════════════════

CTEngine::CTEngine() = default;

// ── Registry ──────────────────────────────────────────────────────────────

void CTEngine::registerFunction(const std::string& name, const FunctionDecl* fn) {
    functions_[name] = fn;
    ++stats_.functionsRegistered;
}

void CTEngine::registerGlobalConst(const std::string& name, CTValue val) {
    globalConsts_[name] = std::move(val);
}

void CTEngine::registerEnumConst(const std::string& name, int64_t val) {
    enumConsts_[name] = val;
}

void CTEngine::markPure(const std::string& name) {
    if (pureFunctions_.insert(name).second)
        ++stats_.pureFunctionsDetected;
}

bool CTEngine::isPure(const std::string& name) const {
    return pureFunctions_.count(name) > 0;
}

// ── Heap access ────────────────────────────────────────────────────────────

std::vector<CTValue> CTEngine::extractArray(CTArrayHandle h) const {
    const CTArray* arr = heap_.get(h);
    if (!arr) return {};
    return arr->data;
}

uint64_t CTEngine::arrayLength(CTArrayHandle h) const {
    return heap_.length(h);
}

CTArrayHandle CTEngine::snapshotArray(CTArrayHandle src) {
    const CTArray* orig = heap_.get(src);
    if (!orig) return CT_NULL_HANDLE;
    // Phase E: immutable arrays can be aliased directly — no copy needed.
    if (heap_.isImmutable(src)) return src;
    const CTArrayHandle dst = heap_.alloc(orig->len);
    CTArray* copy = heap_.getMut(dst);
    copy->data = orig->data;
    return dst;
}

// ── Specialisation key ─────────────────────────────────────────────────────

std::string CTEngine::specializationKey(const std::string& fnName,
                                         const std::vector<CTValue>& args) const {
    std::string key = fnName + "|";
    for (const auto& a : args) {
        key += a.memoHash();
        key += ',';
    }
    return key;
}

// ── Main executeFunction entry points ─────────────────────────────────────

std::optional<CTValue> CTEngine::executeFunction(const std::string&          fnName,
                                                  const std::vector<CTValue>& args) {
    auto it = functions_.find(fnName);
    if (it == functions_.end()) return std::nullopt;
    return executeFunction(it->second, args);
}

std::optional<CTValue> CTEngine::executeFunction(const FunctionDecl*         fn,
                                                  const std::vector<CTValue>& args) {
    if (!fn || !fn->body) return std::nullopt;
    if (currentDepth_ >= kMaxDepth)  return std::nullopt;

    // Build memoisation key.
    // Use appendMemoHash to avoid per-argument temporary strings.
    CTMemoKey key;
    key.fnName = fn->name;
    key.argsHash.reserve(args.size() * 24);   // typical arg hash ≤ ~22 chars
    for (const auto& a : args) {
        a.appendMemoHash(key.argsHash);
        key.argsHash.push_back('|');
    }

    // Cache hit?
    auto hit = memoCache_.find(key);
    if (hit != memoCache_.end()) {
        ++stats_.functionCallsMemoized;
        // If the cached value is an array, snapshot it so the caller gets
        // a fresh handle whose elements can be mutated independently.
        CTValue result = hit->second;
        if (result.isArray() && result.arr != CT_NULL_HANDLE) {
            result.arr = snapshotArray(result.arr);
        }
        return result;
    }

    // Build frame.
    CTFrame frame;
    frame.fn   = fn;
    frame.heap = &heap_;
    if (fn->parameters.size() != args.size()) return std::nullopt;
    bool hasSymbolic = false;
    for (size_t i = 0; i < args.size(); ++i) {
        frame.locals[fn->parameters[i].name] = args[i];
        if (args[i].isSymbolic()) hasSymbolic = true;
    }

    // Execute.
    ++currentDepth_;
    fuel_ = 0;
    const bool ok = executeBody(frame, fn->body.get());
    --currentDepth_;

    if (!ok && !frame.hasReturned && !frame.hasLastBare) return std::nullopt;

    CTValue result;
    if (frame.hasReturned)  result = frame.returnValue;
    else if (frame.hasLastBare) result = frame.lastBareExpr;
    else                    return std::nullopt;

    // SYMBOLIC result means we couldn't fully evaluate — don't fold to constant.
    if (result.isSymbolic()) return std::nullopt;

    // Don't cache results produced with symbolic args: the result is only
    if (!hasSymbolic) {
        // Snapshot array results before storing in cache (so cache is stable).
        CTValue cached = result;
        if (cached.isArray() && cached.arr != CT_NULL_HANDLE)
            cached.arr = snapshotArray(cached.arr);
        memoCache_[key] = cached;
        // Record this function as CF-CTRE-foldable: it produced a concrete
        if (!result.isSymbolic() && !fn->parameters.empty())
            foldableCallees_.insert(fn->name);
    }

    // Record call graph edge.
    if (currentDepth_ > 0) {
        // We are inside another function call — build edge lazily.
        // (Caller name is not tracked here; graph is built in runPass.)
    }
    graph_.nodes.push_back(fn->name);

    return result;
}

// ── evalComptimeBlock ──────────────────────────────────────────────────────

std::optional<CTValue> CTEngine::evalComptimeBlock(
    const BlockStmt* body,
    const std::unordered_map<std::string, CTValue>& env)
{
    if (!body) return std::nullopt;
    if (currentDepth_ >= kMaxDepth) return std::nullopt;

    CTFrame frame;
    frame.fn   = nullptr;
    frame.heap = &heap_;
    for (auto& [k, v] : env) frame.locals[k] = v;

    ++currentDepth_;
    fuel_ = 0;
    const bool ok = executeBody(frame, body);
    --currentDepth_;

    if (frame.hasReturned)  return frame.returnValue;
    if (frame.hasLastBare)  return frame.lastBareExpr;
    if (!ok) return std::nullopt;
    return std::nullopt;
}

// ── evalSingleExpr ─────────────────────────────────────────────────────────
// Evaluate a single expression in the given environment.
// Side effects (array writes, local mutations) stay inside the temporary frame
// and are never propagated back to the caller.

CTValue CTEngine::evalSingleExpr(
    const std::unordered_map<std::string, CTValue>& env,
    const Expression* e)
{
    if (!e) return CTValue::uninit();
    CTFrame frame;
    frame.fn   = nullptr;
    frame.heap = &heap_;
    for (auto& [k, v] : env) frame.locals[k] = v;
    fuel_ = 0;
    return evalExpr(frame, e);
}

// ── executeBody ────────────────────────────────────────────────────────────

bool CTEngine::executeBody(CTFrame& frame, const BlockStmt* body) {
    if (!body) return true;
    for (auto& stmt : body->statements) {
        if (!evalStmt(frame, stmt.get())) return false;
        if (frame.hasReturned || frame.didBreak || frame.didContinue) return true;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════

CTValue CTEngine::evalExpr(CTFrame& frame, const Expression* e) {
    if (!e) return CTValue::uninit();
    ++stats_.instructionsExecuted;
    if (++fuel_ > kMaxInstructions) return CTValue::uninit();

    switch (e->type) {

    // ── Integer / float / string literal ────────────────────────────────
    case ASTNodeType::LITERAL_EXPR: {
        auto* lit = static_cast<const LiteralExpr*>(e);
        if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
            return CTValue::fromI64(static_cast<int64_t>(lit->intValue));
        if (lit->literalType == LiteralExpr::LiteralType::FLOAT)
            return CTValue::fromF64(lit->floatValue);
        return CTValue::fromString(lit->stringValue);
    }

    // ── Identifier lookup ────────────────────────────────────────────────
    case ASTNodeType::IDENTIFIER_EXPR: {
        auto* id = static_cast<const IdentifierExpr*>(e);
        // 1. Local variable (incl. function parameters).
        auto it = frame.locals.find(id->name);
        if (it != frame.locals.end()) {
            // If the local is symbolic but we have a branch constraint that
            if (it->second.isSymbolic()) {
                auto cit = frame.constraints.find(id->name);
                if (cit != frame.constraints.end() && cit->second.isConcrete()) {
                    ++stats_.constraintFolds;
                    return CTValue::fromI64(cit->second.lo);
                }
            }
            return it->second;
        }
        // 2. Enum constant.
        auto eit = enumConsts_.find(id->name);
        if (eit != enumConsts_.end()) return CTValue::fromI64(eit->second);
        // 3. Global compile-time constant.
        auto git = globalConsts_.find(id->name);
        if (git != globalConsts_.end()) return git->second;
        return CTValue::uninit();
    }

    // ── Binary expression ────────────────────────────────────────────────
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const BinaryExpr*>(e);
        // Short-circuit logical ops first.
        if (bin->op == "&&") {
            const CTValue lv = evalExpr(frame, bin->left.get());
            if (lv.isSymbolic()) {
                // SYMBOLIC && x: can only short-circuit if right is known false.
                const CTValue rv = evalExpr(frame, bin->right.get());
                if (rv.isConcrete() && !rv.isTruthy()) return CTValue::fromI64(0); // false && anything = false
                return CTValue::symbolic();
            }
            if (!lv.isKnown()) return CTValue::uninit();
            if (!lv.isTruthy()) return CTValue::fromI64(0);
            const CTValue rv = evalExpr(frame, bin->right.get());
            if (!rv.isKnown()) return CTValue::uninit();
            if (rv.isSymbolic()) return CTValue::symbolic();
            return CTValue::fromI64(rv.isTruthy() ? 1 : 0);
        }
        if (bin->op == "||") {
            const CTValue lv = evalExpr(frame, bin->left.get());
            if (lv.isSymbolic()) {
                // SYMBOLIC || x: can only short-circuit if right is known true.
                const CTValue rv = evalExpr(frame, bin->right.get());
                if (rv.isConcrete() && rv.isTruthy()) return CTValue::fromI64(1); // true || anything = true
                return CTValue::symbolic();
            }
            if (!lv.isKnown()) return CTValue::uninit();
            if (lv.isTruthy()) return CTValue::fromI64(1);
            const CTValue rv = evalExpr(frame, bin->right.get());
            if (!rv.isKnown()) return CTValue::uninit();
            if (rv.isSymbolic()) return CTValue::symbolic();
            return CTValue::fromI64(rv.isTruthy() ? 1 : 0);
        }
        // Null-coalescing (??)
        if (bin->op == "??") {
            const CTValue lv = evalExpr(frame, bin->left.get());
            if (lv.isKnown() && lv.isTruthy()) return lv;
            return evalExpr(frame, bin->right.get());
        }
        CTValue lv = evalExpr(frame, bin->left.get());
        CTValue rv = evalExpr(frame, bin->right.get());
        return evalBinaryOp(bin->op, lv, rv);
    }

    // ── Unary expression ─────────────────────────────────────────────────
    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<const UnaryExpr*>(e);
        CTValue val = evalExpr(frame, un->operand.get());
        return evalUnaryOp(un->op, val);
    }

    // ── Postfix ++ / -- ──────────────────────────────────────────────────
    case ASTNodeType::POSTFIX_EXPR: {
        auto* pfx = static_cast<const PostfixExpr*>(e);
        const int64_t delta = (pfx->op == "++") ? 1 : -1;
        // arr[i]++ / arr[i]-- — array element postfix
        if (pfx->operand->type == ASTNodeType::INDEX_EXPR) {
            auto* idx    = static_cast<const IndexExpr*>(pfx->operand.get());
            const CTValue arrVal = evalExpr(frame, idx->array.get());
            const CTValue idxVal = evalExpr(frame, idx->index.get());
            if (!arrVal.isKnown() || !idxVal.isKnown() ||
                !arrVal.isArray() || !idxVal.isInt()) return CTValue::uninit();
            const int64_t i = idxVal.asI64();
            CTValue current = heap_.load(arrVal.asArr(), i);
            if (!current.isKnown() || !current.isInt()) return CTValue::uninit();
            heap_.store(arrVal.asArr(), i, CTValue::fromI64(current.asI64() + delta));
            return current;  // postfix: return old value
        }
        // x++ / x-- — simple variable postfix
        if (pfx->operand->type != ASTNodeType::IDENTIFIER_EXPR) return CTValue::uninit();
        auto* id = static_cast<const IdentifierExpr*>(pfx->operand.get());
        auto  it = frame.locals.find(id->name);
        if (it == frame.locals.end() || !it->second.isInt()) return CTValue::uninit();
        CTValue old = it->second;
        it->second = CTValue::fromI64(it->second.asI64() + delta);
        return old;  // postfix returns old value
    }

    // ── Prefix ++ / -- ────────────────────────────────────────────────────
    case ASTNodeType::PREFIX_EXPR: {
        auto* pfx = static_cast<const PrefixExpr*>(e);
        if (pfx->op != "++" && pfx->op != "--") return CTValue::uninit();
        const int64_t delta = (pfx->op == "++") ? 1 : -1;
        // ++arr[i] / --arr[i] — array element prefix
        if (pfx->operand->type == ASTNodeType::INDEX_EXPR) {
            auto* idx    = static_cast<const IndexExpr*>(pfx->operand.get());
            const CTValue arrVal = evalExpr(frame, idx->array.get());
            const CTValue idxVal = evalExpr(frame, idx->index.get());
            if (!arrVal.isKnown() || !idxVal.isKnown() ||
                !arrVal.isArray() || !idxVal.isInt()) return CTValue::uninit();
            const int64_t i = idxVal.asI64();
            CTValue current = heap_.load(arrVal.asArr(), i);
            if (!current.isKnown() || !current.isInt()) return CTValue::uninit();
            CTValue updated = CTValue::fromI64(current.asI64() + delta);
            heap_.store(arrVal.asArr(), i, updated);
            return updated;  // prefix: return new value
        }
        // ++x / --x — simple variable prefix
        if (pfx->operand->type != ASTNodeType::IDENTIFIER_EXPR) return CTValue::uninit();
        auto* id = static_cast<const IdentifierExpr*>(pfx->operand.get());
        auto  it = frame.locals.find(id->name);
        if (it == frame.locals.end() || !it->second.isInt()) return CTValue::uninit();
        it->second = CTValue::fromI64(it->second.asI64() + delta);
        return it->second;  // prefix returns new value
    }

    // ── Ternary ──────────────────────────────────────────────────────────
    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<const TernaryExpr*>(e);
        const CTValue cond = evalExpr(frame, tern->condition.get());
        if (!cond.isKnown()) return CTValue::uninit();
        if (cond.isSymbolic()) {
            // Path-sensitive: evaluate both arms.  If they agree on a concrete
            // non-array value the condition cannot affect the result — fold.
            const CTValue tv = evalExpr(frame, tern->thenExpr.get());
            const CTValue ev = evalExpr(frame, tern->elseExpr.get());
            if (tv.isConcrete() && !tv.isArray() &&
                ev.isConcrete() && !ev.isArray() && tv == ev) {
                ++stats_.ternaryMerges;
                return tv;
            }
            return CTValue::symbolic();
        }
        return cond.isTruthy() ? evalExpr(frame, tern->thenExpr.get())
                                : evalExpr(frame, tern->elseExpr.get());
    }

    // ── Array literal → allocate on CTHeap ──────────────────────────────
    case ASTNodeType::ARRAY_EXPR: {
        auto* ae = static_cast<const ArrayExpr*>(e);
        const uint64_t n = static_cast<uint64_t>(ae->elements.size());
        const CTArrayHandle h = heap_.alloc(n);
        ++stats_.arraysAllocated;
        for (size_t i = 0; i < ae->elements.size(); ++i) {
            CTValue v = evalExpr(frame, ae->elements[i].get());
            if (!v.isKnown()) { heap_.freeArray(h); return CTValue::uninit(); }
            heap_.store(h, static_cast<int64_t>(i), std::move(v));
        }
        return CTValue::fromArray(h);
    }

    // ── Array / string subscript (read) ──────────────────────────────────
    case ASTNodeType::INDEX_EXPR: {
        auto* idx  = static_cast<const IndexExpr*>(e);
        const CTValue arr = evalExpr(frame, idx->array.get());
        const CTValue idxv= evalExpr(frame, idx->index.get());
        if (!arr.isKnown() || !idxv.isKnown() || !idxv.isInt()) return CTValue::uninit();
        const int64_t i = idxv.asI64();
        if (arr.isArray()) {
            return heap_.load(arr.asArr(), i);
        }
        if (arr.isString()) {
            const std::string& s = arr.asStr();
            if (i < 0 || static_cast<size_t>(i) >= s.size()) return CTValue::uninit();
            return CTValue::fromI64(static_cast<unsigned char>(s[static_cast<size_t>(i)]));
        }
        return CTValue::uninit();
    }

    // ── Enum / scope resolution ───────────────────────────────────────────
    case ASTNodeType::SCOPE_RESOLUTION_EXPR: {
        auto* sr  = static_cast<const ScopeResolutionExpr*>(e);
        const std::string flat = sr->scopeName + "_" + sr->memberName;
        auto eit = enumConsts_.find(flat);
        if (eit != enumConsts_.end()) return CTValue::fromI64(eit->second);
        return CTValue::uninit();
    }

    // ── Nested comptime block ─────────────────────────────────────────────
    case ASTNodeType::COMPTIME_EXPR: {
        auto* ct = static_cast<const ComptimeExpr*>(e);
        auto result = evalComptimeBlock(ct->body.get(), frame.locals);
        return result.value_or(CTValue::uninit());
    }

    // ── Assignment as expression (returns new value) ─────────────────────
    case ASTNodeType::ASSIGN_EXPR: {
        auto* assign = static_cast<const AssignExpr*>(e);
        CTValue v = evalExpr(frame, assign->value.get());
        if (!v.isKnown()) return CTValue::uninit();
        frame.locals[assign->name] = v;
        return v;
    }

    // ── Index assignment as expression (arr[i] = val) ────────────────────
    case ASTNodeType::INDEX_ASSIGN_EXPR: {
        auto* ia = static_cast<const IndexAssignExpr*>(e);
        const CTValue arrVal = evalExpr(frame, ia->array.get());
        const CTValue idxVal = evalExpr(frame, ia->index.get());
        CTValue newVal = evalExpr(frame, ia->value.get());
        if (!arrVal.isKnown() || !idxVal.isKnown() || !newVal.isKnown()) return CTValue::uninit();
        if (!arrVal.isArray() || !idxVal.isInt()) return CTValue::uninit();
        heap_.store(arrVal.asArr(), idxVal.asI64(), newVal);
        return newVal;
    }

    // ── Function call ─────────────────────────────────────────────────────
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const CallExpr*>(e);
        std::vector<CTValue> foldedArgs;
        foldedArgs.reserve(call->arguments.size());
        for (auto& arg : call->arguments) {
            CTValue v = evalExpr(frame, arg.get());
            if (!v.isKnown()) return CTValue::uninit();
            foldedArgs.push_back(std::move(v));
        }
        return evalCall(frame, call->callee, foldedArgs);
    }

    // ── Pipe expression: x |> f ───────────────────────────────────────────
    case ASTNodeType::PIPE_EXPR: {
        auto* pipe = static_cast<const PipeExpr*>(e);
        const CTValue lv = evalExpr(frame, pipe->left.get());
        if (!lv.isKnown()) return CTValue::uninit();
        return evalCall(frame, pipe->functionName, {lv});
    }

    // ── Range annotation (strip hint, evaluate inner) ─────────────────────
    case ASTNodeType::RANGE_ANNOT_EXPR:
        return evalExpr(frame, static_cast<const RangeAnnotExpr*>(e)->inner.get());

    default:
        return CTValue::uninit();
    }
}

// ═══════════════════════════════════════════════════════════════════════════

CTValue CTEngine::evalBinaryOp(const std::string& op, const CTValue& lhs, const CTValue& rhs) {
    if (!lhs.isKnown() || !rhs.isKnown()) return CTValue::uninit();

    // ── Algebraic identities (Phase B) ───────────────────────────────────────
    if (lhs.isSymbolic() || rhs.isSymbolic()) {
        const bool lSym = lhs.isSymbolic();
        const bool rSym = rhs.isSymbolic();
        // Helper lambdas for concise concrete-side checks.
        auto lInt = [&](int64_t v) { return lhs.isConcrete() && lhs.isInt() && lhs.asI64() == v; };
        auto rInt = [&](int64_t v) { return rhs.isConcrete() && rhs.isInt() && rhs.asI64() == v; };
        // Same symbolic variable?
        const bool sameVar = lSym && rSym && lhs.symId != 0 && lhs.symId == rhs.symId;

        // ── Additive identity: x + 0 = x, 0 + x = x ─────────────────────
        if (op == "+") {
            if (lSym && rInt(0)) { ++stats_.algebraicFolds; return lhs; }
            if (rSym && lInt(0)) { ++stats_.algebraicFolds; return rhs; }
        }
        // ── Subtractive: x - 0 = x, x - x = 0 ───────────────────────────
        if (op == "-") {
            if (lSym && rInt(0))  { ++stats_.algebraicFolds; return lhs; }
            if (sameVar)          { ++stats_.algebraicFolds; return CTValue::fromI64(0); }
        }
        // ── Multiplicative identity: x * 1 = x, 1 * x = x ───────────────
        if (op == "*") {
            if (rhs.isConcrete() && rhs.isInt() && rhs.asI64() == 0) return CTValue::fromI64(0);
            if (lhs.isConcrete() && lhs.isInt() && lhs.asI64() == 0) return CTValue::fromI64(0);
            if (lSym && rInt(1))  { ++stats_.algebraicFolds; return lhs; }
            if (rSym && lInt(1))  { ++stats_.algebraicFolds; return rhs; }
        }
        // ── Division identity: x / 1 = x ─────────────────────────────────
        if (op == "/" || op == "/=") {
            if (lSym && rInt(1))  { ++stats_.algebraicFolds; return lhs; }
        }
        // ── Power: x ** 0 = 1, x ** 1 = x, 1 ** x = 1 ──────────────────
        if (op == "**") {
            if (rhs.isConcrete() && rhs.isInt() && rhs.asI64() == 0) return CTValue::fromI64(1);
            if (lSym && rInt(1))  { ++stats_.algebraicFolds; return lhs; }
            // 1 raised to any integer power is always 1.
            if (lhs.isConcrete() && lhs.isInt() && lhs.asI64() == 1)
                { ++stats_.algebraicFolds; return CTValue::fromI64(1); }
        }
        // ── Bitwise AND: x & 0 = 0, x & -1 = x, x & x = x ──────────────
        if (op == "&" || op == "&=") {
            if (rhs.isConcrete() && rhs.isInt() && rhs.asI64() == 0) return CTValue::fromI64(0);
            if (lhs.isConcrete() && lhs.isInt() && lhs.asI64() == 0) return CTValue::fromI64(0);
            if (lSym && rInt(-1)) { ++stats_.algebraicFolds; return lhs; }
            if (rSym && lInt(-1)) { ++stats_.algebraicFolds; return rhs; }
            if (sameVar)          { ++stats_.algebraicFolds; return lhs; }
        }
        // ── Bitwise OR: x | 0 = x, x | -1 = -1, x | x = x ──────────────
        if (op == "|" || op == "|=") {
            if (rhs.isConcrete() && rhs.isInt() && rhs.asI64() == -1) return CTValue::fromI64(-1);
            if (lhs.isConcrete() && lhs.isInt() && lhs.asI64() == -1) return CTValue::fromI64(-1);
            if (lSym && rInt(0))  { ++stats_.algebraicFolds; return lhs; }
            if (rSym && lInt(0))  { ++stats_.algebraicFolds; return rhs; }
            if (sameVar)          { ++stats_.algebraicFolds; return lhs; }
        }
        // ── Bitwise XOR: x ^ 0 = x, 0 ^ x = x, x ^ x = 0 ───────────────
        if (op == "^" || op == "^=") {
            if (lSym && rInt(0))  { ++stats_.algebraicFolds; return lhs; }
            if (rSym && lInt(0))  { ++stats_.algebraicFolds; return rhs; }
            if (sameVar)          { ++stats_.algebraicFolds; return CTValue::fromI64(0); }
        }
        // ── Shifts: x >> 0 = x, x << 0 = x, x >>> 0 = x ─────────────────
        if (op == ">>" || op == ">>=" || op == "<<" || op == "<<=" || op == ">>>") {
            if (lSym && rInt(0))  { ++stats_.algebraicFolds; return lhs; }
        }
        // ── Modulo: x % ±1 = 0, x % x = 0 ──────────────────────────────
        if (op == "%" || op == "mod" || op == "%=") {
            if (rhs.isConcrete() && rhs.isInt() &&
                (rhs.asI64() == 1 || rhs.asI64() == -1))
                return CTValue::fromI64(0);
            if (sameVar) { ++stats_.algebraicFolds; return CTValue::fromI64(0); }
        }
        // ── Logical AND short-circuits and identities ─────────────────────
        // x && 0 = 0, 0 && x = 0 (short-circuit regardless of x)
        // x && 1 = x, 1 && x = x (identity when the concrete side is true)
        if (op == "&&") {
            if (lhs.isConcrete() && lhs.isInt() && lhs.asI64() == 0) return CTValue::fromI64(0);
            if (rhs.isConcrete() && rhs.isInt() && rhs.asI64() == 0) return CTValue::fromI64(0);
            if (lSym && rhs.isConcrete() && rhs.isInt() && rhs.asI64() != 0)
                { ++stats_.algebraicFolds; return lhs; }
            if (rSym && lhs.isConcrete() && lhs.isInt() && lhs.asI64() != 0)
                { ++stats_.algebraicFolds; return rhs; }
        }
        // ── Logical OR short-circuits and identities ──────────────────────
        // x || 1 = 1, 1 || x = 1 (short-circuit when concrete side is true)
        // x || 0 = x, 0 || x = x (identity when the concrete side is false)
        if (op == "||") {
            if (lhs.isConcrete() && lhs.isInt() && lhs.asI64() != 0) return lhs;
            if (rhs.isConcrete() && rhs.isInt() && rhs.asI64() != 0) return rhs;
            if (lSym && rhs.isConcrete() && rhs.isInt() && rhs.asI64() == 0)
                { ++stats_.algebraicFolds; return lhs; }
            if (rSym && lhs.isConcrete() && lhs.isInt() && lhs.asI64() == 0)
                { ++stats_.algebraicFolds; return rhs; }
        }
        // ── Same-variable comparisons ──────────────────────────────────────
        // x == x → 1, x != x → 0, x < x → 0, x <= x → 1, x > x → 0, x >= x → 1.
        // These arise naturally in induction-variable guard patterns.
        if (sameVar) {
            if (op == "==" || op == "<=" || op == ">=")
                { ++stats_.algebraicFolds; return CTValue::fromI64(1); }
            if (op == "!=" || op == "<"  || op == ">")
                { ++stats_.algebraicFolds; return CTValue::fromI64(0); }
        }

        return CTValue::symbolic();
    }

    // String concatenation.
    if (op == "+" && lhs.isString() && rhs.isString())
        return CTValue::fromString(lhs.asStr() + rhs.asStr());

    // Array concatenation.
    if (op == "+" && lhs.isArray() && rhs.isArray()) {
        auto l = extractArray(lhs.asArr());
        auto r = extractArray(rhs.asArr());
        if (l.size() > (std::numeric_limits<uint64_t>::max)() - r.size())
            return CTValue::uninit();
        const uint64_t outLen = static_cast<uint64_t>(l.size() + r.size());
        CTArrayHandle out = heap_.alloc(outLen, CTValue::uninit());
        ++stats_.arraysAllocated;
        uint64_t i = 0;
        for (const auto& v : l) heap_.store(out, static_cast<int64_t>(i++), v);
        for (const auto& v : r) heap_.store(out, static_cast<int64_t>(i++), v);
        return CTValue::fromArray(out);
    }

    // String equality / inequality.
    if (lhs.isString() && rhs.isString()) {
        if (op == "==") return CTValue::fromI64(lhs.asStr() == rhs.asStr() ? 1 : 0);
        if (op == "!=") return CTValue::fromI64(lhs.asStr() != rhs.asStr() ? 1 : 0);
        return CTValue::uninit();
    }

    // Float arithmetic.
    if (lhs.isFloat() || rhs.isFloat()) {
        const double a = lhs.asF64(), b = rhs.asF64();
        if (op == "+")  return CTValue::fromF64(a + b);
        if (op == "-")  return CTValue::fromF64(a - b);
        if (op == "*")  return CTValue::fromF64(a * b);
        if (op == "/" && b != 0.0)  return CTValue::fromF64(a / b);
        if (op == "==") return CTValue::fromI64(a == b ? 1 : 0);
        if (op == "!=") return CTValue::fromI64(a != b ? 1 : 0);
        if (op == "<")  return CTValue::fromI64(a < b  ? 1 : 0);
        if (op == "<=") return CTValue::fromI64(a <= b ? 1 : 0);
        if (op == ">")  return CTValue::fromI64(a > b  ? 1 : 0);
        if (op == ">=") return CTValue::fromI64(a >= b ? 1 : 0);
        return CTValue::uninit();
    }

    // Integer arithmetic.
    if (!lhs.isInt() || !rhs.isInt()) return CTValue::uninit();
    const int64_t  a = lhs.asI64(), b = rhs.asI64();
    const uint64_t ua = lhs.asU64();

    if (op == "+")  return CTValue::fromI64(a + b);
    if (op == "-")  return CTValue::fromI64(a - b);
    if (op == "*")  return CTValue::fromI64(a * b);
    if (op == "/" && b != 0) return CTValue::fromI64(a / b);
    if (op == "%" && b != 0) return CTValue::fromI64(a % b);
    if (op == "&")  return CTValue::fromI64(a & b);
    if (op == "|")  return CTValue::fromI64(a | b);
    if (op == "^")  return CTValue::fromI64(a ^ b);
    if (op == "<<" && b >= 0 && b < 64) return CTValue::fromU64(ua << static_cast<unsigned>(b));
    if (op == ">>" && b >= 0 && b < 64) return CTValue::fromI64(a >> static_cast<int>(b));
    if (op == ">>>") {
        const int sh = static_cast<int>(b & 63);
        return CTValue::fromU64(ua >> sh);
    }
    if (op == "**") {
        if (b < 0) return (a == 1) ? CTValue::fromI64(1) : CTValue::uninit();
        int64_t r = 1, base = a, rem = b;
        while (rem > 0) { if (rem & 1) r *= base; base *= base; rem >>= 1; }
        return CTValue::fromI64(r);
    }
    if (op == "==") return CTValue::fromI64(a == b ? 1 : 0);
    if (op == "!=") return CTValue::fromI64(a != b ? 1 : 0);
    if (op == "<")  return CTValue::fromI64(a < b  ? 1 : 0);
    if (op == "<=") return CTValue::fromI64(a <= b ? 1 : 0);
    if (op == ">")  return CTValue::fromI64(a > b  ? 1 : 0);
    if (op == ">=") return CTValue::fromI64(a >= b ? 1 : 0);

    // Compound-assignment operators (desugared by parser into BinaryExpr in some paths).
    if (op == "+=") return CTValue::fromI64(a + b);
    if (op == "-=") return CTValue::fromI64(a - b);
    if (op == "*=") return CTValue::fromI64(a * b);
    if (op == "/=" && b != 0) return CTValue::fromI64(a / b);
    if (op == "%=" && b != 0) return CTValue::fromI64(a % b);
    if (op == "&=") return CTValue::fromI64(a & b);
    if (op == "|=") return CTValue::fromI64(a | b);
    if (op == "^=") return CTValue::fromI64(a ^ b);
    if (op == "<<=" && b >= 0 && b < 64) return CTValue::fromU64(ua << static_cast<unsigned>(b));
    if (op == ">>=" && b >= 0 && b < 64) return CTValue::fromI64(a >> static_cast<int>(b));
    if (op == ">>>=") {
        const int sh = static_cast<int>(b & 63);
        return CTValue::fromU64(ua >> sh);
    }

    return CTValue::uninit();
}

// ═══════════════════════════════════════════════════════════════════════════

CTValue CTEngine::evalUnaryOp(const std::string& op, const CTValue& val) {
    if (!val.isKnown()) return CTValue::uninit();
    if (val.isSymbolic()) return CTValue::symbolic(); // propagate symbolic
    if (val.isFloat()) {
        if (op == "-")  return CTValue::fromF64(-val.asF64());
        if (op == "+")  return val;
        return CTValue::uninit();
    }
    if (val.isInt()) {
        if (op == "-")  return CTValue::fromI64(-val.asI64());
        if (op == "~")  return CTValue::fromI64(~val.asI64());
        if (op == "!")  return CTValue::fromI64(val.asI64() == 0 ? 1 : 0);
        if (op == "+")  return val;
        return CTValue::uninit();
    }
    if (val.isString() && op == "!")
        return CTValue::fromI64(val.asStr().empty() ? 1 : 0);
    return CTValue::uninit();
}

// ═══════════════════════════════════════════════════════════════════════════

CTValue CTEngine::evalTypeCast(const std::string& name, const CTValue& val) {
    if (!val.isKnown()) return CTValue::uninit();
    if (val.isSymbolic()) return CTValue::symbolic(); // propagate symbolic
    if (val.isInt()) {
        const int64_t v = val.asI64();
        // General iN/uN handler for N in [1..256]
        unsigned castBits = 0; bool castUnsigned = false;
        if (name == "int")  { castBits = 64; castUnsigned = false; }
        else if (name == "uint") { castBits = 64; castUnsigned = true; }
        else if (name == "bool") { castBits = 1;  castUnsigned = true; }
        else if (name.size() >= 2 && (name[0] == 'i' || name[0] == 'u')) {
            bool allDigits = true; int bw = 0;
            for (size_t j = 1; j < name.size(); ++j) {
                if (!std::isdigit(static_cast<unsigned char>(name[j]))) { allDigits = false; break; }
                bw = bw * 10 + (name[j] - '0'); if (bw > 256) { allDigits = false; break; }
            }
            if (allDigits && bw >= 1 && bw <= 256) { castBits = static_cast<unsigned>(bw); castUnsigned = (name[0] == 'u'); }
        }
        if (castBits >= 1) {
            if (castBits == 1) return CTValue::fromI64(v != 0 ? 1 : 0);
            if (castBits >= 64) return CTValue::fromI64(v);
            const uint64_t mask = (UINT64_C(1) << castBits) - 1u;
            if (castUnsigned) return CTValue::fromI64(static_cast<int64_t>(static_cast<uint64_t>(v) & mask));
            uint64_t uv = static_cast<uint64_t>(v) & mask;
            const uint64_t signBit = UINT64_C(1) << (castBits - 1);
            if (uv & signBit) uv |= ~mask;
            return CTValue::fromI64(static_cast<int64_t>(uv));
        }
    }
    return CTValue::uninit();
}

// ═══════════════════════════════════════════════════════════════════════════

CTValue CTEngine::evalCall(CTFrame& /*callerFrame*/,
                            const std::string& fnName,
                            const std::vector<CTValue>& args) {
    // 1. Type-cast builtins — iN/uN for N in [1..256], plus int/uint/bool.
    auto isTypeCastName = [](const std::string& nm) -> bool {
        if (nm == "int" || nm == "uint" || nm == "bool") return true;
        if (nm.size() < 2 || (nm[0] != 'i' && nm[0] != 'u')) return false;
        int bw = 0;
        for (size_t j = 1; j < nm.size(); ++j) {
            if (!std::isdigit(static_cast<unsigned char>(nm[j]))) return false;
            bw = bw * 10 + (nm[j] - '0');
            if (bw > 256) return false;
        }
        return bw >= 1 && bw <= 256;
    };
    if (isTypeCastName(fnName) && args.size() == 1)
        return evalTypeCast(fnName, args[0]);

    // 2. Pure built-in functions.
    auto bv = evalBuiltin(fnName, args);
    if (bv) return *bv;

    // 3. User-defined function (requires all args to be CT-known).
    auto it = functions_.find(fnName);
    if (it != functions_.end() && it->second &&
        it->second->body && args.size() == it->second->parameters.size()) {

        // ── Phase D: partial-specialisation cache lookup ──────────────────
        const std::string specKey = partialSpecKey(fnName, args);
        {
            auto sit = specCache_.find(specKey);
            if (sit != specCache_.end()) {
                ++stats_.partialEvalFolds;
                CTValue cached = sit->second;
                if (cached.isArray() && cached.arr != CT_NULL_HANDLE)
                    cached.arr = snapshotArray(cached.arr);
                return cached;
            }
        }

        auto result = executeFunction(it->second, args);

        // If the result is concrete and at least one arg was symbolic,
        // cache in specCache_ so future partial-eval calls hit immediately.
        if (result && result->isConcrete()) {
            const bool hasSymbolic = std::any_of(args.begin(), args.end(),
                                                 [](const CTValue& a){ return a.isSymbolic(); });
            if (hasSymbolic) {
                CTValue toCache = *result;
                if (toCache.isArray() && toCache.arr != CT_NULL_HANDLE)
                    toCache.arr = snapshotArray(toCache.arr);
                specCache_[specKey] = std::move(toCache);
            }
        }
        return result.value_or(CTValue::uninit());
    }

    return CTValue::uninit();
}

// ═══════════════════════════════════════════════════════════════════════════

// Helper lambdas (defined at function scope to access args cleanly).
std::optional<CTValue> CTEngine::evalBuiltin(const std::string& name,
                                               const std::vector<CTValue>& args) {
    // ── Fast reject: skip the entire if-chain for unknown names ──────────
    // BuiltinEffectTable::contains() is the authoritative registry of all
    // known OmScript builtins.  BuiltinEffectTable::isWidthCastName() covers
    // the dynamically-named iN/uN and __tw_*/__tf_* cast intrinsics.
    if (!BuiltinEffectTable::contains(name) && !BuiltinEffectTable::isWidthCastName(name)) {
        return std::nullopt;
    }

    const size_t n = args.size();

    // Helpers.
    auto intArg = [&](size_t i) -> std::optional<int64_t> {
        if (i < n && args[i].isInt()) return args[i].asI64();
        return std::nullopt;
    };
    // strArg returns a pointer to the arg's string, avoiding a copy.
    auto strArg = [&](size_t i) -> const std::string* {
        if (i < n && args[i].isString()) return &args[i].asStr();
        return nullptr;
    };
    auto arrArg = [&](size_t i) -> CTArrayHandle {
        if (i < n && args[i].isArray()) return args[i].asArr();
        return CT_NULL_HANDLE;
    };
    // arrData returns a direct const pointer to the heap array's element
    // vector, avoiding a full copy.  Returns nullptr if the handle is invalid.
    auto arrData = [&](CTArrayHandle h) -> const std::vector<CTValue>* {
        const CTArray* arr = heap_.get(h);
        return arr ? &arr->data : nullptr;
    };
    // arrCopy returns a mutable copy — only use when elements will be mutated
    // (sort, reverse, insert, remove).
    auto arrCopy = [&](CTArrayHandle h) -> std::vector<CTValue> {
        return extractArray(h);
    };

    // ── len ──────────────────────────────────────────────────────────────
    if (name == "len" && n == 1) {
        if (auto s = strArg(0)) return CTValue::fromI64(static_cast<int64_t>(s->size()));
        const CTArrayHandle h = arrArg(0);
        if (h != CT_NULL_HANDLE)
            return CTValue::fromI64(static_cast<int64_t>(heap_.length(h)));
        return std::nullopt;
    }
    if (name == "str_len" && n == 1) {
        if (auto s = strArg(0)) return CTValue::fromI64(static_cast<int64_t>(s->size()));
        return std::nullopt;
    }

    // ── abs ──────────────────────────────────────────────────────────────
    if (name == "abs" && n == 1) {
        if (auto v = intArg(0)) return CTValue::fromI64(*v < 0 ? -*v : *v);
        return std::nullopt;
    }

    // ── push(arr, val) → mutates array, returns 0 (void) ────────────────
    if (name == "push" && n == 2) {
        const CTArrayHandle h = arrArg(0);
        if (h != CT_NULL_HANDLE && args[1].isKnown()) {
            heap_.push(h, args[1]);
            return CTValue::fromI64(0);
        }
        return std::nullopt;
    }

    // ── pop(arr) → removes & returns last element ───────────────────────
    if (name == "pop" && n == 1) {
        const CTArrayHandle h = arrArg(0);
        if (h != CT_NULL_HANDLE) {
            const uint64_t len = heap_.length(h);
            if (len == 0) return std::nullopt;
            CTValue last = heap_.load(h, static_cast<int64_t>(len - 1));
            CTArray* arr = heap_.getMut(h);
            if (arr && arr->len > 0) {
                arr->data.pop_back();
                --arr->len;
            }
            return last;
        }
        return std::nullopt;
    }

    // ── min / max ────────────────────────────────────────────────────────
    if (name == "min" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) return CTValue::fromI64(std::min(*a, *b));
        return std::nullopt;
    }
    if (name == "max" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) return CTValue::fromI64(std::max(*a, *b));
        return std::nullopt;
    }

    // ── sign ─────────────────────────────────────────────────────────────
    if (name == "sign" && n == 1) {
        if (auto v = intArg(0))
            return CTValue::fromI64(*v > 0 ? 1 : (*v < 0 ? -1 : 0));
        return std::nullopt;
    }

    // ── clamp ────────────────────────────────────────────────────────────
    if (name == "clamp" && n == 3) {
        auto v = intArg(0), lo = intArg(1), hi = intArg(2);
        if (v && lo && hi) return CTValue::fromI64(std::max(*lo, std::min(*v, *hi)));
        return std::nullopt;
    }

    // ── pow ──────────────────────────────────────────────────────────────
    if (name == "pow" && n == 2) {
        auto b = intArg(0), ex = intArg(1);
        if (b && ex) {
            if (*ex < 0) return (*b == 1) ? std::optional<CTValue>(CTValue::fromI64(1)) : std::nullopt;
            int64_t r = 1, base = *b;
            int64_t e = *ex;
            while (e > 0) { if (e & 1) r *= base; base *= base; e >>= 1; }
            return CTValue::fromI64(r);
        }
        return std::nullopt;
    }

    // ── sqrt / floor / ceil / round ───────────────────────────────────────
    if (name == "sqrt" && n == 1) {
        if (auto v = intArg(0))
            if (*v >= 0) return CTValue::fromI64(static_cast<int64_t>(std::sqrt(static_cast<double>(*v))));
        return std::nullopt;
    }
    if (name == "floor" && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(*v); return std::nullopt; }
    if (name == "ceil"  && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(*v); return std::nullopt; }
    if (name == "round" && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(*v); return std::nullopt; }

    // ── log2 / log / log10 / exp2 ─────────────────────────────────────────
    if (name == "log2" && n == 1) {
        if (auto v = intArg(0)) {
            if (*v <= 0) return std::nullopt;
            int64_t x = *v, r = 0;
            while (x > 1) { x >>= 1; r++; }
            return CTValue::fromI64(r);
        }
        return std::nullopt;
    }
    if (name == "log" && n == 1) {
        if (auto v = intArg(0))
            if (*v > 0) return CTValue::fromI64(static_cast<int64_t>(std::log(static_cast<double>(*v))));
        return std::nullopt;
    }
    if (name == "log10" && n == 1) {
        if (auto v = intArg(0))
            if (*v > 0) return CTValue::fromI64(static_cast<int64_t>(std::log10(static_cast<double>(*v))));
        return std::nullopt;
    }
    if (name == "exp2" && n == 1) {
        if (auto v = intArg(0))
            if (*v >= 0 && *v < 63) return CTValue::fromI64(int64_t(1) << static_cast<int>(*v));
        return std::nullopt;
    }

    // ── gcd / lcm ────────────────────────────────────────────────────────
    if (name == "gcd" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) {
            uint64_t ua = static_cast<uint64_t>(std::abs(*a));
            uint64_t ub = static_cast<uint64_t>(std::abs(*b));
            while (ub) { const uint64_t t = ub; ub = ua % ub; ua = t; }
            return CTValue::fromI64(static_cast<int64_t>(ua));
        }
        return std::nullopt;
    }
    if (name == "lcm" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) {
            uint64_t ua = static_cast<uint64_t>(std::abs(*a));
            uint64_t ub = static_cast<uint64_t>(std::abs(*b));
            if (ua == 0 || ub == 0) return CTValue::fromI64(0);
            uint64_t g = ua, tb = ub;
            while (tb) { const uint64_t t = tb; tb = g % tb; g = t; }
            return CTValue::fromI64(static_cast<int64_t>(ua / g * ub));
        }
        return std::nullopt;
    }

    // ── is_even / is_odd / is_power_of_2 ─────────────────────────────────
    if (name == "is_even"       && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64((*v & 1) == 0 ? 1 : 0); return std::nullopt; }
    if (name == "is_odd"        && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64((*v & 1)      ? 1 : 0); return std::nullopt; }
    if (name == "is_power_of_2" && n == 1) {
        if (auto v = intArg(0))
            return CTValue::fromI64((*v > 0 && (*v & (*v - 1)) == 0) ? 1 : 0);
        return std::nullopt;
    }

    // ── Bitwise intrinsics ────────────────────────────────────────────────
    if (name == "popcount" && n == 1) {
        if (auto v = intArg(0))
            return CTValue::fromI64(static_cast<int64_t>(__builtin_popcountll(static_cast<uint64_t>(*v))));
        return std::nullopt;
    }
    if (name == "clz" && n == 1) {
        if (auto v = intArg(0))
            if (*v != 0) return CTValue::fromI64(static_cast<int64_t>(__builtin_clzll(static_cast<uint64_t>(*v))));
        return std::nullopt;
    }
    if (name == "ctz" && n == 1) {
        if (auto v = intArg(0))
            if (*v != 0) return CTValue::fromI64(static_cast<int64_t>(__builtin_ctzll(static_cast<uint64_t>(*v))));
        return std::nullopt;
    }
    if (name == "bswap" && n == 1) {
        if (auto v = intArg(0))
            return CTValue::fromI64(static_cast<int64_t>(__builtin_bswap64(static_cast<uint64_t>(*v))));
        return std::nullopt;
    }
    if (name == "bitreverse" && n == 1) {
        if (auto v = intArg(0)) {
            uint64_t x = static_cast<uint64_t>(*v);
            x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
            x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
            x = ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
            x = __builtin_bswap64(x);
            return CTValue::fromI64(static_cast<int64_t>(x));
        }
        return std::nullopt;
    }
    if (name == "rotate_left" && n == 2) {
        auto v = intArg(0), k = intArg(1);
        if (v && k) {
            uint64_t x = static_cast<uint64_t>(*v);
            const int sh = static_cast<int>(*k) & 63;
            return CTValue::fromI64(static_cast<int64_t>((x << sh) | (x >> (64 - sh))));
        }
        return std::nullopt;
    }
    if (name == "rotate_right" && n == 2) {
        auto v = intArg(0), k = intArg(1);
        if (v && k) {
            uint64_t x = static_cast<uint64_t>(*v);
            const int sh = static_cast<int>(*k) & 63;
            return CTValue::fromI64(static_cast<int64_t>((x >> sh) | (x << (64 - sh))));
        }
        return std::nullopt;
    }
    if (name == "saturating_add" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) {
            int64_t r; if (__builtin_add_overflow(*a, *b, &r)) r = (*a > 0) ? INT64_MAX : INT64_MIN;
            return CTValue::fromI64(r);
        }
        return std::nullopt;
    }
    if (name == "saturating_sub" && n == 2) {
        auto a = intArg(0), b = intArg(1);
        if (a && b) {
            int64_t r; if (__builtin_sub_overflow(*a, *b, &r)) r = (*a > 0) ? INT64_MAX : INT64_MIN;
            return CTValue::fromI64(r);
        }
        return std::nullopt;
    }

    // ── String builtins ───────────────────────────────────────────────────
    if (name == "char_at" && n == 2) {
        auto s = strArg(0); auto i = intArg(1);
        if (s && i && *i >= 0 && *i < static_cast<int64_t>(s->size()))
            return CTValue::fromI64(static_cast<unsigned char>((*s)[static_cast<size_t>(*i)]));
        return std::nullopt;
    }
    if (name == "str_eq" && n == 2) {
        auto a = strArg(0), b = strArg(1);
        if (a && b) return CTValue::fromI64(*a == *b ? 1 : 0);
        return std::nullopt;
    }
    if (name == "str_concat" && n == 2) {
        auto a = strArg(0), b = strArg(1);
        if (a && b) return CTValue::fromString(*a + *b);
        return std::nullopt;
    }
    if (name == "to_char" && n == 1) {
        if (auto v = intArg(0)) {
            char c = static_cast<char>(static_cast<uint8_t>(*v & 0xFF));
            return CTValue::fromString(std::string(1, c));
        }
        return std::nullopt;
    }
    if ((name == "is_alpha") && n == 1) {
        if (auto v = intArg(0)) return CTValue::fromI64((*v >= 0 && *v <= 127 && std::isalpha(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    if ((name == "is_digit") && n == 1) {
        if (auto v = intArg(0)) return CTValue::fromI64((*v >= 0 && *v <= 127 && std::isdigit(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    if ((name == "to_string" || name == "number_to_string") && n == 1) {
        if (auto v = intArg(0)) return CTValue::fromString(std::to_string(*v));
        return std::nullopt;
    }
    if ((name == "to_int" || name == "string_to_number" || name == "str_to_int") && n == 1) {
        if (auto s = strArg(0)) {
            try { return CTValue::fromI64(static_cast<int64_t>(std::stoll(*s))); } catch (...) {} // NOLINT(bugprone-empty-catch)
        }
        return std::nullopt;
    }
    if (name == "char_code" && n == 1) {
        if (auto s = strArg(0))
            if (!s->empty()) return CTValue::fromI64(static_cast<unsigned char>((*s)[0]));
        return std::nullopt;
    }
    if ((name == "str_find" || name == "str_index_of") && n == 2) {
        auto hay = strArg(0), needle = strArg(1);
        if (hay && needle) {
            auto pos = hay->find(*needle);
            return CTValue::fromI64(pos == std::string::npos ? -1 : static_cast<int64_t>(pos));
        }
        return std::nullopt;
    }
    if (name == "str_contains" && n == 2) {
        auto hay = strArg(0), needle = strArg(1);
        if (hay && needle) return CTValue::fromI64(hay->find(*needle) != std::string::npos ? 1 : 0);
        return std::nullopt;
    }
    if ((name == "str_starts_with" || name == "startswith") && n == 2) {
        auto s = strArg(0), p = strArg(1);
        if (s && p) {
            bool r = s->size() >= p->size() && s->compare(0, p->size(), *p) == 0;
            return CTValue::fromI64(r ? 1 : 0);
        }
        return std::nullopt;
    }
    if ((name == "str_ends_with" || name == "endswith") && n == 2) {
        auto s = strArg(0), sf = strArg(1);
        if (s && sf) {
            bool r = s->size() >= sf->size() && s->compare(s->size() - sf->size(), sf->size(), *sf) == 0;
            return CTValue::fromI64(r ? 1 : 0);
        }
        return std::nullopt;
    }
    if (name == "str_substr" && n == 3) {
        auto s = strArg(0); auto start = intArg(1); auto slen = intArg(2);
        if (s && start && slen) {
            int64_t sz = static_cast<int64_t>(s->size());
            int64_t st = std::max(int64_t(0), std::min(*start, sz));
            int64_t ln = std::max(int64_t(0), std::min(*slen, sz - st));
            return CTValue::fromString(s->substr(static_cast<size_t>(st), static_cast<size_t>(ln)));
        }
        return std::nullopt;
    }
    if (name == "str_upper" && n == 1) {
        if (auto s = strArg(0)) {
            std::string r = *s;
            for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return CTValue::fromString(std::move(r));
        }
        return std::nullopt;
    }
    if (name == "str_lower" && n == 1) {
        if (auto s = strArg(0)) {
            std::string r = *s;
            for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return CTValue::fromString(std::move(r));
        }
        return std::nullopt;
    }
    if (name == "str_repeat" && n == 2) {
        auto s = strArg(0); auto cnt = intArg(1);
        if (s && cnt) {
            if (*cnt <= 0) return CTValue::fromString("");
            if (*cnt > 1000) return std::nullopt;
            std::string r;
            r.reserve(s->size() * static_cast<size_t>(*cnt));
            for (int64_t i = 0; i < *cnt; ++i) r += *s;
            return CTValue::fromString(std::move(r));
        }
        return std::nullopt;
    }
    if (name == "str_trim" && n == 1) {
        if (auto s = strArg(0)) {
            size_t start = 0, end = s->size();
            while (start < end && std::isspace(static_cast<unsigned char>((*s)[start]))) ++start;
            while (end > start && std::isspace(static_cast<unsigned char>((*s)[end-1]))) --end;
            return CTValue::fromString(s->substr(start, end - start));
        }
        return std::nullopt;
    }
    if (name == "str_reverse" && n == 1) {
        if (auto s = strArg(0)) {
            std::string r = *s;
            std::reverse(r.begin(), r.end());
            return CTValue::fromString(std::move(r));
        }
        return std::nullopt;
    }
    if (name == "str_count" && n == 2) {
        auto s = strArg(0), sub = strArg(1);
        if (s && sub && !sub->empty()) {
            int64_t cnt = 0;
            size_t pos = 0;
            while ((pos = s->find(*sub, pos)) != std::string::npos) { ++cnt; pos += sub->size(); }
            return CTValue::fromI64(cnt);
        }
        return std::nullopt;
    }
    if (name == "str_replace" && n == 3) {
        auto s = strArg(0), old_sub = strArg(1), new_sub = strArg(2);
        if (s && old_sub && new_sub) {
            if (old_sub->empty()) return CTValue::fromString(*s);
            std::string r;
            size_t pos = 0, prev = 0;
            while ((pos = s->find(*old_sub, prev)) != std::string::npos) {
                r.append(*s, prev, pos - prev);
                r += *new_sub;
                prev = pos + old_sub->size();
            }
            r.append(*s, prev, std::string::npos);
            return CTValue::fromString(std::move(r));
        }
        return std::nullopt;
    }
    if (name == "str_pad_left" && n == 3) {
        auto s = strArg(0); auto w = intArg(1); auto fill = strArg(2);
        if (s && w && fill && !fill->empty()) {
            int64_t slen = static_cast<int64_t>(s->size());
            if (*w <= slen) return CTValue::fromString(*s);
            if (*w > 65536) return std::nullopt;
            std::string r(*w - slen, (*fill)[0]);
            r += *s;
            return CTValue::fromString(std::move(r));
        }
        return std::nullopt;
    }
    if (name == "str_pad_right" && n == 3) {
        auto s = strArg(0); auto w = intArg(1); auto fill = strArg(2);
        if (s && w && fill && !fill->empty()) {
            int64_t slen = static_cast<int64_t>(s->size());
            if (*w <= slen) return CTValue::fromString(*s);
            if (*w > 65536) return std::nullopt;
            std::string r(*s);
            r.append(*w - slen, (*fill)[0]);
            return CTValue::fromString(std::move(r));
        }
        return std::nullopt;
    }
    if (name == "str_chars" && n == 1) {
        if (auto s = strArg(0)) {
            CTArrayHandle h = heap_.alloc(static_cast<uint64_t>(s->size()));
            ++stats_.arraysAllocated;
            for (size_t i = 0; i < s->size(); ++i)
                heap_.store(h, static_cast<int64_t>(i),
                            CTValue::fromI64(static_cast<unsigned char>((*s)[i])));
            return CTValue::fromArray(h);
        }
        return std::nullopt;
    }
    if (name == "typeof" && n == 1) {
        if (args[0].isInt())    return CTValue::fromI64(1);
        if (args[0].isFloat())  return CTValue::fromI64(2);
        if (args[0].isString()) return CTValue::fromI64(3);
        return CTValue::fromI64(1);
    }

    // ── Array builtins ────────────────────────────────────────────────────
    if (name == "array_fill" && n == 2) {
        auto cnt = intArg(0);
        if (!cnt) return std::nullopt;
        int64_t count = *cnt;
        if (count < 0) count = 0;
        if (count > 65536) return std::nullopt;
        CTArrayHandle h = heap_.alloc(static_cast<uint64_t>(count), args[1]);
        ++stats_.arraysAllocated;
        return CTValue::fromArray(h);
    }
    if (name == "range" && n == 2) {
        auto start = intArg(0), end = intArg(1);
        if (!start || !end) return std::nullopt;
        int64_t len = std::max(int64_t(0), *end - *start);
        if (len > 65536) return std::nullopt;
        CTArrayHandle h = heap_.alloc(static_cast<uint64_t>(len));
        ++stats_.arraysAllocated;
        for (int64_t i = 0; i < len; ++i)
            heap_.store(h, i, CTValue::fromI64(*start + i));
        return CTValue::fromArray(h);
    }
    if (name == "range_step" && n == 3) {
        auto start = intArg(0), end = intArg(1), step = intArg(2);
        if (!start || !end || !step || *step == 0) return std::nullopt;
        // Estimate element count for reserve.
        int64_t estLen = (*step > 0 && *end > *start) ? ((*end - *start + *step - 1) / *step) :
                         (*step < 0 && *end < *start) ? ((*start - *end + (-*step) - 1) / (-*step)) : 0;
        if (estLen > 65536) estLen = 65536;
        std::vector<int64_t> vals;
        if (estLen > 0) vals.reserve(static_cast<size_t>(estLen));
        for (int64_t cur = *start;
             (*step > 0 ? cur < *end : cur > *end) && vals.size() < 65536;
             cur += *step)
            vals.push_back(cur);
        CTArrayHandle h = heap_.alloc(static_cast<uint64_t>(vals.size()));
        ++stats_.arraysAllocated;
        for (size_t i = 0; i < vals.size(); ++i)
            heap_.store(h, static_cast<int64_t>(i), CTValue::fromI64(vals[i]));
        return CTValue::fromArray(h);
    }
    if (name == "array_concat" && n == 2) {
        CTArrayHandle ha = arrArg(0), hb = arrArg(1);
        if (ha == CT_NULL_HANDLE || hb == CT_NULL_HANDLE) return std::nullopt;
        const auto* ea = arrData(ha); const auto* eb = arrData(hb);
        if (!ea || !eb) return std::nullopt;
        CTArrayHandle h = heap_.alloc(static_cast<uint64_t>(ea->size() + eb->size()));
        ++stats_.arraysAllocated;
        int64_t idx = 0;
        for (const auto& v : *ea) heap_.store(h, idx++, v);
        for (const auto& v : *eb) heap_.store(h, idx++, v);
        return CTValue::fromArray(h);
    }
    if (name == "array_slice" && n == 3) {
        CTArrayHandle ha = arrArg(0);
        auto start = intArg(1), end = intArg(2);
        if (ha == CT_NULL_HANDLE || !start || !end) return std::nullopt;
        int64_t sz = static_cast<int64_t>(heap_.length(ha));
        int64_t st = std::max(int64_t(0), std::min(*start, sz));
        int64_t en = std::max(st, std::min(*end, sz));
        CTArrayHandle h = heap_.alloc(static_cast<uint64_t>(en - st));
        ++stats_.arraysAllocated;
        for (int64_t i = st; i < en; ++i)
            heap_.store(h, i - st, heap_.load(ha, i));
        return CTValue::fromArray(h);
    }
    if (name == "array_copy" && n == 1) {
        CTArrayHandle src = arrArg(0);
        if (src == CT_NULL_HANDLE) return std::nullopt;
        return CTValue::fromArray(snapshotArray(src));
    }
    if ((name == "sum") && n == 1) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE) return std::nullopt;
        const auto* elems = arrData(h);
        if (!elems) return std::nullopt;
        int64_t total = 0;
        for (const auto& v : *elems) {
            if (!v.isInt()) return std::nullopt;
            total += v.asI64();
        }
        return CTValue::fromI64(total);
    }
    if (name == "array_product" && n == 1) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE) return std::nullopt;
        const auto* elems = arrData(h);
        if (!elems) return std::nullopt;
        int64_t product = 1;
        for (const auto& v : *elems) {
            if (!v.isInt()) return std::nullopt;
            product *= v.asI64();
        }
        return CTValue::fromI64(product);
    }
    if (name == "array_min" && n == 1) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE) return std::nullopt;
        const auto* elems = arrData(h);
        if (!elems || elems->empty()) return CTValue::fromI64(0);
        int64_t minv = INT64_MAX;
        for (const auto& v : *elems) { if (!v.isInt()) return std::nullopt; if (v.asI64() < minv) minv = v.asI64(); }
        return CTValue::fromI64(minv);
    }
    if (name == "array_max" && n == 1) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE) return std::nullopt;
        const auto* elems = arrData(h);
        if (!elems || elems->empty()) return CTValue::fromI64(0);
        int64_t maxv = INT64_MIN;
        for (const auto& v : *elems) { if (!v.isInt()) return std::nullopt; if (v.asI64() > maxv) maxv = v.asI64(); }
        return CTValue::fromI64(maxv);
    }
    if (name == "array_last" && n == 1) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE) return std::nullopt;
        uint64_t len = heap_.length(h);
        if (len == 0) return std::nullopt;
        return heap_.load(h, static_cast<int64_t>(len - 1));
    }
    if (name == "array_contains" && n == 2) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE || !args[1].isInt()) return std::nullopt;
        int64_t needle = args[1].asI64();
        const auto* elems = arrData(h);
        if (!elems) return std::nullopt;
        for (const auto& v : *elems)
            if (v.isInt() && v.asI64() == needle) return CTValue::fromI64(1);
        return CTValue::fromI64(0);
    }
    if ((name == "index_of" || name == "array_find") && n == 2) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE || !args[1].isInt()) return std::nullopt;
        int64_t needle = args[1].asI64();
        const auto* elems = arrData(h);
        if (!elems) return std::nullopt;
        for (size_t i = 0; i < elems->size(); ++i)
            if ((*elems)[i].isInt() && (*elems)[i].asI64() == needle)
                return CTValue::fromI64(static_cast<int64_t>(i));
        return CTValue::fromI64(-1);
    }

    // ── Character classification predicates ──────────────────────────────
    if (name == "is_upper" && n == 1) {
        if (auto v = intArg(0)) return CTValue::fromI64((*v >= 0 && *v <= 127 && std::isupper(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    if (name == "is_lower" && n == 1) {
        if (auto v = intArg(0)) return CTValue::fromI64((*v >= 0 && *v <= 127 && std::islower(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    if (name == "is_space" && n == 1) {
        if (auto v = intArg(0)) return CTValue::fromI64((*v >= 0 && *v <= 127 && std::isspace(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }
    if (name == "is_alnum" && n == 1) {
        if (auto v = intArg(0)) return CTValue::fromI64((*v >= 0 && *v <= 127 && std::isalnum(static_cast<unsigned char>(*v))) ? 1 : 0);
        return std::nullopt;
    }

    // ── Unchecked integer arithmetic (fast_* and precise_*) ─────────────
    // At compile time these are identical to normal arithmetic.
    if (name == "fast_add" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CTValue::fromI64(*a + *b); return std::nullopt; }
    if (name == "fast_sub" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CTValue::fromI64(*a - *b); return std::nullopt; }
    if (name == "fast_mul" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CTValue::fromI64(*a * *b); return std::nullopt; }
    if (name == "fast_div" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b && *b != 0) return CTValue::fromI64(*a / *b); return std::nullopt; }
    if (name == "precise_add" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CTValue::fromI64(*a + *b); return std::nullopt; }
    if (name == "precise_sub" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CTValue::fromI64(*a - *b); return std::nullopt; }
    if (name == "precise_mul" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CTValue::fromI64(*a * *b); return std::nullopt; }
    if (name == "precise_div" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b && *b != 0) return CTValue::fromI64(*a / *b); return std::nullopt; }

    // ── Trigonometric / math (integer floor for integer args) ────────────
    if (name == "sin" && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(static_cast<int64_t>(std::sin(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "cos" && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(static_cast<int64_t>(std::cos(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "tan" && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(static_cast<int64_t>(std::tan(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "asin" && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(static_cast<int64_t>(std::asin(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "acos" && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(static_cast<int64_t>(std::acos(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "atan" && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(static_cast<int64_t>(std::atan(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "atan2" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CTValue::fromI64(static_cast<int64_t>(std::atan2(static_cast<double>(*a), static_cast<double>(*b)))); return std::nullopt; }
    if (name == "exp" && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(static_cast<int64_t>(std::exp(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "cbrt" && n == 1) { if (auto v = intArg(0)) return CTValue::fromI64(static_cast<int64_t>(std::cbrt(static_cast<double>(*v)))); return std::nullopt; }
    if (name == "hypot" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CTValue::fromI64(static_cast<int64_t>(std::hypot(static_cast<double>(*a), static_cast<double>(*b)))); return std::nullopt; }
    if (name == "fma" && n == 3) { auto a = intArg(0), b = intArg(1), c = intArg(2); if (a && b && c) return CTValue::fromI64(*a * *b + *c); return std::nullopt; }
    if (name == "copysign" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) { int64_t mag = *a < 0 ? -*a : *a; return CTValue::fromI64(*b >= 0 ? mag : -mag); } return std::nullopt; }
    if (name == "min_float" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CTValue::fromI64(std::min(*a, *b)); return std::nullopt; }
    if (name == "max_float" && n == 2) { auto a = intArg(0), b = intArg(1); if (a && b) return CTValue::fromI64(std::max(*a, *b)); return std::nullopt; }

    // ── Array: reverse, sort, remove, insert ────────────────────────────
    if (name == "reverse" && n == 1) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE) return std::nullopt;
        auto elems = arrCopy(h);
        std::reverse(elems.begin(), elems.end());
        CTArrayHandle nh = heap_.alloc(static_cast<uint64_t>(elems.size()));
        ++stats_.arraysAllocated;
        for (size_t i = 0; i < elems.size(); ++i)
            heap_.store(nh, static_cast<int64_t>(i), std::move(elems[i]));
        return CTValue::fromArray(nh);
    }
    if (name == "sort" && n == 1) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE) return std::nullopt;
        auto elems = arrCopy(h);
        // Sort only integer arrays at compile time.
        for (auto& e : elems) if (!e.isInt()) return std::nullopt;
        std::sort(elems.begin(), elems.end(), [](const CTValue& a, const CTValue& b) {
            return a.asI64() < b.asI64();
        });
        CTArrayHandle nh = heap_.alloc(static_cast<uint64_t>(elems.size()));
        ++stats_.arraysAllocated;
        for (size_t i = 0; i < elems.size(); ++i)
            heap_.store(nh, static_cast<int64_t>(i), std::move(elems[i]));
        return CTValue::fromArray(nh);
    }
    if (name == "array_remove" && n == 2) {
        CTArrayHandle h = arrArg(0);
        auto idx = intArg(1);
        if (h == CT_NULL_HANDLE || !idx) return std::nullopt;
        auto elems = arrCopy(h);
        int64_t i = *idx;
        if (i < 0 || i >= static_cast<int64_t>(elems.size())) return std::nullopt;
        elems.erase(elems.begin() + i);
        CTArrayHandle nh = heap_.alloc(static_cast<uint64_t>(elems.size()));
        ++stats_.arraysAllocated;
        for (size_t j = 0; j < elems.size(); ++j)
            heap_.store(nh, static_cast<int64_t>(j), std::move(elems[j]));
        return CTValue::fromArray(nh);
    }
    if (name == "array_insert" && n == 3) {
        CTArrayHandle h = arrArg(0);
        auto idx = intArg(1);
        if (h == CT_NULL_HANDLE || !idx || !args[2].isKnown()) return std::nullopt;
        auto elems = arrCopy(h);
        int64_t i = *idx;
        if (i < 0 || i > static_cast<int64_t>(elems.size())) return std::nullopt;
        elems.insert(elems.begin() + i, args[2]);
        CTArrayHandle nh = heap_.alloc(static_cast<uint64_t>(elems.size()));
        ++stats_.arraysAllocated;
        for (size_t j = 0; j < elems.size(); ++j)
            heap_.store(nh, static_cast<int64_t>(j), std::move(elems[j]));
        return CTValue::fromArray(nh);
    }
    if (name == "array_any" && n == 1) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE) return std::nullopt;
        const auto* elems = arrData(h);
        if (!elems) return std::nullopt;
        for (const auto& v : *elems)
            if (v.isInt() && v.asI64() != 0) return CTValue::fromI64(1);
        return CTValue::fromI64(0);
    }
    if (name == "array_every" && n == 1) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE) return std::nullopt;
        const auto* elems = arrData(h);
        if (!elems) return std::nullopt;
        for (const auto& v : *elems) {
            if (!v.isInt()) return std::nullopt;
            if (v.asI64() == 0) return CTValue::fromI64(0);
        }
        return CTValue::fromI64(1);
    }
    if (name == "array_count" && n == 2) {
        CTArrayHandle h = arrArg(0);
        if (h == CT_NULL_HANDLE || !args[1].isInt()) return std::nullopt;
        int64_t needle = args[1].asI64(), count = 0;
        const auto* elems = arrData(h);
        if (!elems) return std::nullopt;
        for (const auto& v : *elems)
            if (v.isInt() && v.asI64() == needle) ++count;
        return CTValue::fromI64(count);
    }

    // ── String: str_split, str_join ─────────────────────────────────────
    if (name == "str_split" && n == 2) {
        auto s = strArg(0), delim = strArg(1);
        if (!s || !delim || delim->empty()) return std::nullopt;
        std::vector<std::string> parts;
        size_t pos = 0, prev = 0;
        while ((pos = s->find(*delim, prev)) != std::string::npos) {
            parts.push_back(s->substr(prev, pos - prev));
            prev = pos + delim->size();
        }
        parts.push_back(s->substr(prev));
        CTArrayHandle h = heap_.alloc(static_cast<uint64_t>(parts.size()));
        ++stats_.arraysAllocated;
        for (size_t i = 0; i < parts.size(); ++i)
            heap_.store(h, static_cast<int64_t>(i), CTValue::fromString(std::move(parts[i])));
        return CTValue::fromArray(h);
    }
    if (name == "str_join" && n == 2) {
        CTArrayHandle h = arrArg(0);
        auto sep = strArg(1);
        if (h == CT_NULL_HANDLE || !sep) return std::nullopt;
        const auto* elems = arrData(h);
        if (!elems) return std::nullopt;
        std::string result;
        for (size_t i = 0; i < elems->size(); ++i) {
            if (!(*elems)[i].isString()) return std::nullopt;
            if (i > 0) result += *sep;
            result += (*elems)[i].asStr();
        }
        return CTValue::fromString(std::move(result));
    }

    // Integer type-cast aliases (also handled by evalTypeCast).
    if (n == 1 && args[0].isInt()) {
        const int64_t v = args[0].asI64();
        if (name == "u64" || name == "i64" || name == "int" || name == "uint") return CTValue::fromI64(v);
        if (name == "u32") return CTValue::fromI64(static_cast<int64_t>(static_cast<uint32_t>(v)));
        if (name == "i32") return CTValue::fromI64(static_cast<int64_t>(static_cast<int32_t>(v)));
        if (name == "u16") return CTValue::fromI64(static_cast<int64_t>(static_cast<uint16_t>(v)));
        if (name == "i16") return CTValue::fromI64(static_cast<int64_t>(static_cast<int16_t>(v)));
        if (name == "u8")  return CTValue::fromI64(static_cast<int64_t>(static_cast<uint8_t>(v)));
        if (name == "i8")  return CTValue::fromI64(static_cast<int64_t>(static_cast<int8_t>(v)));
        if (name == "bool")return CTValue::fromI64(v != 0 ? 1 : 0);
    }

    // ── std::synthesize (program synthesis stdlib function) ──────────────
    if ((name == "std__synthesize" || name == "std_synthesize") && n >= 1) {
        // Argument 0: examples must be a concrete array of arrays.
        CTArrayHandle examplesH = arrArg(0);
        if (examplesH == CT_NULL_HANDLE) return std::nullopt;
        const auto* outerArr = heap_.get(examplesH);
        if (!outerArr || outerArr->data.empty()) return std::nullopt;

        std::vector<omscript::SynthExample> examples;
        size_t nInputs = 0;
        for (const auto& inner : outerArr->data) {
            if (!inner.isArray()) return std::nullopt;
            const auto* innerArr = heap_.get(inner.asArr());
            if (!innerArr || innerArr->data.size() < 2) return std::nullopt;
            if (nInputs == 0) nInputs = innerArr->data.size() - 1;
            if (innerArr->data.size() - 1 != nInputs) return std::nullopt; // mismatched widths

            omscript::SynthExample ex;
            ex.inputs.reserve(nInputs);
            for (size_t i = 0; i < innerArr->data.size(); ++i) {
                if (!innerArr->data[i].isInt()) return std::nullopt;
                if (i < nInputs)
                    ex.inputs.push_back(innerArr->data[i].asI64());
                else
                    ex.output = innerArr->data[i].asI64();
            }
            examples.push_back(std::move(ex));
        }
        if (examples.empty()) return std::nullopt;

        omscript::SynthConfig cfg;
        cfg.maxCandidates = 200000;

        // Argument 1 (optional): ops array.
        if (n >= 2) {
            CTArrayHandle opsH = arrArg(1);
            if (opsH != CT_NULL_HANDLE) {
                const auto* opsArr = heap_.get(opsH);
                if (opsArr) {
                    for (const auto& opV : opsArr->data) {
                        if (opV.isString()) cfg.ops.push_back(opV.asStr());
                    }
                }
            }
        }

        // Argument 2 (optional): max_depth.
        if (n >= 3 && args[2].isInt()) {
            int d = static_cast<int>(args[2].asI64());
            cfg.maxDepth = std::min(std::max(d, 1), 8);
        }

        // Argument 3 (optional): cost_hint.
        if (n >= 4 && args[3].isString()) {
            cfg.preferSize = (args[3].asStr() == "size");
        }

        omscript::SynthesisEngine eng;
        auto result = eng.synthesize(static_cast<int>(nInputs), examples, cfg);
        if (!result) return std::nullopt;
        return CTValue::fromI64(result->firstOutput);
    }

    // ── mulhi(a, b) — signed high 64 bits of 128-bit product ─────────────────
    if (name == "mulhi" && n == 2) {
        if (auto a = intArg(0))
            if (auto b = intArg(1)) {
                using I128 = __int128;
                I128 product = static_cast<I128>(*a) * static_cast<I128>(*b);
                return CTValue::fromI64(static_cast<int64_t>(product >> 64));
            }
        return std::nullopt;
    }

    // ── mulhi_u(a, b) — unsigned high 64 bits of 128-bit product ─────────────
    if (name == "mulhi_u" && n == 2) {
        if (auto a = intArg(0))
            if (auto b = intArg(1)) {
                using U128 = unsigned __int128;
                U128 product = static_cast<U128>(static_cast<uint64_t>(*a))
                             * static_cast<U128>(static_cast<uint64_t>(*b));
                return CTValue::fromI64(static_cast<int64_t>(
                    static_cast<uint64_t>(product >> 64)));
            }
        return std::nullopt;
    }

    // ── absdiff(a, b) — |a - b| without overflow ─────────────────────────────
    if (name == "absdiff" && n == 2) {
        if (auto a = intArg(0))
            if (auto b = intArg(1)) {
                using I128 = __int128;
                I128 diff = static_cast<I128>(*a) - static_cast<I128>(*b);
                if (diff < 0) diff = -diff;
                return CTValue::fromI64(static_cast<int64_t>(diff));
            }
        return std::nullopt;
    }

    // ── fast_sqrt(x) — sqrt with fast-math (comptime: exact sqrt) ────────────
    if (name == "fast_sqrt" && n == 1) {
        if (auto v = intArg(0))
            return CTValue::fromI64(static_cast<int64_t>(std::sqrt(static_cast<double>(*v))));
        return std::nullopt;
    }

    // ── is_nan(x) — 1 if x reinterpreted as f64 is NaN ───────────────────────
    if (name == "is_nan" && n == 1) {
        if (auto v = intArg(0)) {
            double d;
            uint64_t bits = static_cast<uint64_t>(*v);
            std::memcpy(&d, &bits, 8);
            return CTValue::fromI64(std::isnan(d) ? 1 : 0);
        }
        return std::nullopt;
    }

    // ── is_inf(x) — 1 if x reinterpreted as f64 is ±Inf ─────────────────────
    if (name == "is_inf" && n == 1) {
        if (auto v = intArg(0)) {
            double d;
            uint64_t bits = static_cast<uint64_t>(*v);
            std::memcpy(&d, &bits, 8);
            return CTValue::fromI64(std::isinf(d) ? 1 : 0);
        }
        return std::nullopt;
    }

    // ── __tw_<op>_<N> — width-specific integer comptime evaluation ───────────
    if (name.size() > 5 && name.substr(0,5) == "__tw_") {
        const std::string suffix = name.substr(5);
        const auto uscore = suffix.rfind('_');
        if (uscore == std::string::npos) return std::nullopt;
        const std::string opname   = suffix.substr(0, uscore);
        const std::string widthStr = suffix.substr(uscore+1);
        int bw = 0;
        for (char c : widthStr) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
            bw = bw*10+(c-'0');
        }
        if (bw<1 || bw>64) return std::nullopt;
        const uint64_t mask = (bw<64) ? ((1ULL<<bw)-1) : ~0ULL;

        if (opname=="popcount" && n==1) {
            if (auto v = intArg(0))
                return CTValue::fromI64(static_cast<int64_t>(__builtin_popcountll(static_cast<uint64_t>(*v) & mask)));
        }
        if (opname=="clz" && n==1) {
            if (auto v = intArg(0)) {
                uint64_t bits = static_cast<uint64_t>(*v) & mask;
                int64_t r = bits==0 ? bw : static_cast<int64_t>(__builtin_clzll(bits)-(64-bw));
                return CTValue::fromI64(r);
            }
        }
        if (opname=="ctz" && n==1) {
            if (auto v = intArg(0)) {
                uint64_t bits = static_cast<uint64_t>(*v) & mask;
                int64_t r = bits==0 ? bw : static_cast<int64_t>(__builtin_ctzll(bits));
                return CTValue::fromI64(r);
            }
        }
        if (opname=="bitreverse" && n==1) {
            if (auto v = intArg(0)) {
                uint64_t bits = static_cast<uint64_t>(*v) & mask;
                uint64_t rev = 0;
                for (int i=0;i<bw;++i) rev|=((bits>>i)&1ULL)<<(bw-1-i);
                return CTValue::fromI64(static_cast<int64_t>(rev));
            }
        }
        if (opname=="bswap" && n==1 && bw>=16 && (bw%8)==0) {
            if (auto v = intArg(0)) {
                uint64_t bits = static_cast<uint64_t>(*v) & mask;
                uint64_t sw = 0; int bytes=bw/8;
                for (int i=0;i<bytes;++i) sw|=((bits>>(i*8))&0xFF)<<((bytes-1-i)*8);
                return CTValue::fromI64(static_cast<int64_t>(sw));
            }
        }
        if ((opname=="rotate_left"||opname=="rotl") && n==2) {
            if (auto v=intArg(0)) if (auto a=intArg(1)) {
                uint64_t bits=static_cast<uint64_t>(*v)&mask;
                int amt=static_cast<int>(*a)%bw; if(amt<0) amt+=bw;
                uint64_t r=(bits<<amt)|(bits>>(bw-amt));
                return CTValue::fromI64(static_cast<int64_t>(r&mask));
            }
        }
        if ((opname=="rotate_right"||opname=="rotr") && n==2) {
            if (auto v=intArg(0)) if (auto a=intArg(1)) {
                uint64_t bits=static_cast<uint64_t>(*v)&mask;
                int amt=static_cast<int>(*a)%bw; if(amt<0) amt+=bw;
                uint64_t r=(bits>>amt)|(bits<<(bw-amt));
                return CTValue::fromI64(static_cast<int64_t>(r&mask));
            }
        }
        if (opname=="saturating_add" && n==2) {
            if (auto a=intArg(0)) if (auto b=intArg(1)) {
                using I128=__int128;
                I128 res=static_cast<I128>(*a)+static_cast<I128>(*b);
                int64_t lo=-(int64_t(1)<<(bw-1)), hi=(int64_t(1)<<(bw-1))-1;
                if(res<lo) res=lo; if(res>hi) res=hi;
                return CTValue::fromI64(static_cast<int64_t>(res));
            }
        }
        if (opname=="saturating_sub" && n==2) {
            if (auto a=intArg(0)) if (auto b=intArg(1)) {
                using I128=__int128;
                I128 res=static_cast<I128>(*a)-static_cast<I128>(*b);
                int64_t lo=-(int64_t(1)<<(bw-1)), hi=(int64_t(1)<<(bw-1))-1;
                if(res<lo) res=lo; if(res>hi) res=hi;
                return CTValue::fromI64(static_cast<int64_t>(res));
            }
        }
        return std::nullopt;
    }

    // ── __tf_<op> — f32-typed float comptime evaluation ──────────────────────
    if (name.size() > 5 && name.substr(0,5) == "__tf_") {
        const std::string opname = name.substr(5);
        // For comptime eval, just compute with double precision and return.
        auto dArg = [&](size_t i) -> std::optional<double> {
            if (i>=n) return std::nullopt;
            if (auto v = intArg(i)) return static_cast<double>(*v);
            return std::nullopt;
        };
        auto fromD = [](double d) { return CTValue::fromI64(static_cast<int64_t>(d)); };
        if (opname=="sqrt"  && n==1) { if(auto v=dArg(0)) return fromD(std::sqrt(*v)); }
        if (opname=="sin"   && n==1) { if(auto v=dArg(0)) return fromD(std::sin(*v)); }
        if (opname=="cos"   && n==1) { if(auto v=dArg(0)) return fromD(std::cos(*v)); }
        if (opname=="tan"   && n==1) { if(auto v=dArg(0)) return fromD(std::tan(*v)); }
        if (opname=="asin"  && n==1) { if(auto v=dArg(0)) return fromD(std::asin(*v)); }
        if (opname=="acos"  && n==1) { if(auto v=dArg(0)) return fromD(std::acos(*v)); }
        if (opname=="atan"  && n==1) { if(auto v=dArg(0)) return fromD(std::atan(*v)); }
        if (opname=="atan2" && n==2) { if(auto a=dArg(0)) if(auto b=dArg(1)) return fromD(std::atan2(*a,*b)); }
        if (opname=="log"   && n==1) { if(auto v=dArg(0)) return fromD(std::log(*v)); }
        if (opname=="log2"  && n==1) { if(auto v=dArg(0)) return fromD(std::log2(*v)); }
        if (opname=="log10" && n==1) { if(auto v=dArg(0)) return fromD(std::log10(*v)); }
        if (opname=="exp"   && n==1) { if(auto v=dArg(0)) return fromD(std::exp(*v)); }
        if (opname=="exp2"  && n==1) { if(auto v=dArg(0)) return fromD(std::exp2(*v)); }
        if (opname=="cbrt"  && n==1) { if(auto v=dArg(0)) return fromD(std::cbrt(*v)); }
        if (opname=="hypot" && n==2) { if(auto a=dArg(0)) if(auto b=dArg(1)) return fromD(std::hypot(*a,*b)); }
        if (opname=="fma"   && n==3) { if(auto a=dArg(0)) if(auto b=dArg(1)) if(auto c=dArg(2)) return fromD(std::fma(*a,*b,*c)); }
        if (opname=="copysign" && n==2) { if(auto a=dArg(0)) if(auto b=dArg(1)) return fromD(std::copysign(*a,*b)); }
        if (opname=="fast_sqrt" && n==1) { if(auto v=dArg(0)) return fromD(std::sqrt(*v)); }
        return std::nullopt;
    }

    return std::nullopt;
}
// When a for-range loop body consists only of simple scalar accumulations

// ── Linear expression descriptor ─────────────────────────────────────────
// Represents value = coefA * loopVar + coefB where coefA == 0 is invariant.
struct LRLinear {
    bool    valid{false};
    int64_t a{0};   // coefficient of the loop induction variable
    int64_t b{0};   // loop-invariant bias
};

// Classify an expression as a*iv + b, returning {false} if unrepresentable.
static LRLinear lrLinearize(
    const Expression*                                   e,
    const std::string&                                  iv,
    const std::unordered_map<std::string, CTValue>&     snap,
    const std::unordered_set<std::string>&              modVars)
{
    if (!e) return {};
    switch (e->type) {

    case ASTNodeType::LITERAL_EXPR: {
        auto* lit = static_cast<const LiteralExpr*>(e);
        if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
            return {true, 0, static_cast<int64_t>(lit->intValue)};
        return {};
    }

    case ASTNodeType::IDENTIFIER_EXPR: {
        auto* id = static_cast<const IdentifierExpr*>(e);
        if (id->name == iv) return {true, 1, 0};
        if (modVars.count(id->name)) return {};   // mutated — not invariant
        auto it = snap.find(id->name);
        if (it != snap.end() && it->second.isInt())
            return {true, 0, it->second.asI64()};
        return {};
    }

    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<const UnaryExpr*>(e);
        if (un->op == "-") {
            LRLinear s = lrLinearize(un->operand.get(), iv, snap, modVars);
            if (!s.valid) return {};
            return {true, -s.a, -s.b};
        }
        if (un->op == "+")
            return lrLinearize(un->operand.get(), iv, snap, modVars);
        if (un->op == "~") {
            LRLinear s = lrLinearize(un->operand.get(), iv, snap, modVars);
            if (!s.valid || s.a != 0) return {};   // ~c only for invariant c
            return {true, 0, ~s.b};
        }
        return {};
    }

    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const BinaryExpr*>(e);
        if (bin->op == "+" || bin->op == "-") {
            LRLinear L = lrLinearize(bin->left.get(),  iv, snap, modVars);
            LRLinear R = lrLinearize(bin->right.get(), iv, snap, modVars);
            if (!L.valid || !R.valid) return {};
            int64_t ra = (bin->op == "+") ?  R.a : -R.a;
            int64_t rb = (bin->op == "+") ?  R.b : -R.b;
            return {true, L.a + ra, L.b + rb};
        }
        if (bin->op == "*") {
            LRLinear L = lrLinearize(bin->left.get(),  iv, snap, modVars);
            LRLinear R = lrLinearize(bin->right.get(), iv, snap, modVars);
            if (!L.valid || !R.valid) return {};
            if (L.a == 0 && R.a == 0) return {true, 0,       L.b * R.b};
            if (L.a == 0)             return {true, L.b*R.a,  L.b*R.b};   // L invariant
            if (R.a == 0)             return {true, R.b*L.a,  R.b*L.b};   // R invariant
            return {};   // quadratic — not representable
        }
        if (bin->op == "<<") {
            LRLinear L = lrLinearize(bin->left.get(),  iv, snap, modVars);
            LRLinear R = lrLinearize(bin->right.get(), iv, snap, modVars);
            if (!L.valid || !R.valid || R.a != 0) return {};
            if (R.b < 0 || R.b >= 64) return {};
            unsigned sh = static_cast<unsigned>(R.b);
            return {true, L.a << sh, L.b << sh};
        }
        if (bin->op == ">>" || bin->op == ">>>") {
            LRLinear L = lrLinearize(bin->left.get(),  iv, snap, modVars);
            LRLinear R = lrLinearize(bin->right.get(), iv, snap, modVars);
            if (!L.valid || !R.valid || R.a != 0 || L.a != 0) return {};
            if (R.b < 0 || R.b >= 64) return {};
            int sh = static_cast<int>(R.b);
            if (bin->op == ">>")
                return {true, 0, L.b >> sh};
            return {true, 0, static_cast<int64_t>(static_cast<uint64_t>(L.b) >> sh)};
        }
        // Bitwise ops on two invariant operands only.
        if (bin->op == "&" || bin->op == "|" || bin->op == "^") {
            LRLinear L = lrLinearize(bin->left.get(),  iv, snap, modVars);
            LRLinear R = lrLinearize(bin->right.get(), iv, snap, modVars);
            if (!L.valid || !R.valid || L.a != 0 || R.a != 0) return {};
            int64_t res = 0;
            if      (bin->op == "&") res = L.b & R.b;
            else if (bin->op == "|") res = L.b | R.b;
            else                     res = L.b ^ R.b;
            return {true, 0, res};
        }
        return {};
    }

    // ── Built-in calls over loop-invariant operands ──────────────────────
    case ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const CallExpr*>(e);
        const std::string& fn = call->callee;
        if (fn == "abs" && call->arguments.size() == 1) {
            LRLinear A = lrLinearize(call->arguments[0].get(), iv, snap, modVars);
            if (!A.valid || A.a != 0) return {};   // only invariant operand
            int64_t v = A.b;
            return {true, 0, v < 0 ? -v : v};
        }
        if ((fn == "min" || fn == "max") && call->arguments.size() == 2) {
            LRLinear A = lrLinearize(call->arguments[0].get(), iv, snap, modVars);
            LRLinear B = lrLinearize(call->arguments[1].get(), iv, snap, modVars);
            if (!A.valid || !B.valid || A.a != 0 || B.a != 0) return {};
            int64_t v = (fn == "min") ? std::min(A.b, B.b) : std::max(A.b, B.b);
            return {true, 0, v};
        }
        return {};
    }

    default:
        return {};
    }
}

// ── Per-iteration effect on one scalar variable ───────────────────────────
struct LREffect {
    enum class Kind { ADD, MUL, XOR, AND, OR, SET, INCR, DECR, MIN, MAX };
    Kind        kind;
    std::string var;
    int64_t     a{0};   // coefficient of loop var (for ADD / SET / MIN / MAX)
    int64_t     b{0};   // constant part           (all ops)
};

// Collect all variable names written in a statement subtree.
static void lrCollectWrites(const Statement* s,
                             std::unordered_set<std::string>& out)
{
    if (!s) return;
    switch (s->type) {
    case ASTNodeType::BLOCK: {
        auto* blk = static_cast<const BlockStmt*>(s);
        for (auto& st : blk->statements) lrCollectWrites(st.get(), out);
        break;
    }
    case ASTNodeType::EXPR_STMT: {
        auto* es = static_cast<const ExprStmt*>(s);
        const Expression* expr = es->expression.get();
        if (expr->type == ASTNodeType::ASSIGN_EXPR)
            out.insert(static_cast<const AssignExpr*>(expr)->name);
        else if (expr->type == ASTNodeType::INDEX_ASSIGN_EXPR) {
            auto* ia = static_cast<const IndexAssignExpr*>(expr);
            if (ia->array->type == ASTNodeType::IDENTIFIER_EXPR)
                out.insert(static_cast<const IdentifierExpr*>(ia->array.get())->name);
        } else if (expr->type == ASTNodeType::POSTFIX_EXPR) {
            auto* pfx = static_cast<const PostfixExpr*>(expr);
            if (pfx->operand->type == ASTNodeType::IDENTIFIER_EXPR)
                out.insert(static_cast<const IdentifierExpr*>(pfx->operand.get())->name);
        } else if (expr->type == ASTNodeType::PREFIX_EXPR) {
            auto* pfx = static_cast<const PrefixExpr*>(expr);
            if (pfx->op == "++" || pfx->op == "--")
                if (pfx->operand->type == ASTNodeType::IDENTIFIER_EXPR)
                    out.insert(static_cast<const IdentifierExpr*>(pfx->operand.get())->name);
        }
        break;
    }
    case ASTNodeType::VAR_DECL:
        out.insert(static_cast<const VarDecl*>(s)->name);
        break;
    default:
        break;
    }
}

// Analyze every statement in `body` and populate `effects`.
// Returns false if any statement cannot be classified as a simple scalar effect.
static bool lrAnalyzeBody(
    const Statement*                                    body,
    const std::string&                                  iv,
    const std::unordered_map<std::string, CTValue>&     snap,
    const std::unordered_set<std::string>&              modVars,
    std::vector<LREffect>&                              effects)
{
    if (!body) return true;

    // Flatten top-level block (one level only — nested control flow → bail).
    std::vector<const Statement*> stmts;
    if (body->type == ASTNodeType::BLOCK) {
        auto* blk = static_cast<const BlockStmt*>(body);
        for (auto& st : blk->statements) stmts.push_back(st.get());
    } else {
        stmts.push_back(body);
    }

    for (const Statement* s : stmts) {
        if (!s) continue;
        if (s->type != ASTNodeType::EXPR_STMT) return false;  // control flow → bail

        auto* es   = static_cast<const ExprStmt*>(s);
        const Expression* expr = es->expression.get();

        // ── x++ / x-- (postfix or prefix) ─────────────────────────────────
        if (expr->type == ASTNodeType::POSTFIX_EXPR ||
            expr->type == ASTNodeType::PREFIX_EXPR) {
            const std::string* namePtr = nullptr;
            std::string op;
            if (expr->type == ASTNodeType::POSTFIX_EXPR) {
                auto* pfx = static_cast<const PostfixExpr*>(expr);
                if (pfx->operand->type != ASTNodeType::IDENTIFIER_EXPR) return false;
                namePtr = &static_cast<const IdentifierExpr*>(pfx->operand.get())->name;
                op = pfx->op;
            } else {
                auto* pfx = static_cast<const PrefixExpr*>(expr);
                if (pfx->op != "++" && pfx->op != "--") return false;
                if (pfx->operand->type != ASTNodeType::IDENTIFIER_EXPR) return false;
                namePtr = &static_cast<const IdentifierExpr*>(pfx->operand.get())->name;
                op = pfx->op;
            }
            if (*namePtr == iv) return false;  // can't modify loop var
            // Each variable may appear at most once (no double-effect).
            for (auto& eff : effects) if (eff.var == *namePtr) return false;
            effects.push_back({op == "++" ? LREffect::Kind::INCR : LREffect::Kind::DECR,
                                *namePtr, 0, 0});
            continue;
        }

        // ── x = RHS  (compound assignments desugared by parser) ────────────
        if (expr->type == ASTNodeType::ASSIGN_EXPR) {
            auto* assign = static_cast<const AssignExpr*>(expr);
            const std::string& varName = assign->name;
            if (varName == iv) return false;
            for (auto& eff : effects) if (eff.var == varName) return false;

            const Expression* rhs = assign->value.get();

            // Try accumulation patterns:  x = x op delta
            if (rhs->type == ASTNodeType::BINARY_EXPR) {
                auto* bin = static_cast<const BinaryExpr*>(rhs);

                auto isSelf = [&](const Expression* e) -> bool {
                    return e->type == ASTNodeType::IDENTIFIER_EXPR &&
                           static_cast<const IdentifierExpr*>(e)->name == varName;
                };

                // x = x + delta  /  x = delta + x
                if (bin->op == "+" || bin->op == "-") {
                    bool sl = isSelf(bin->left.get());
                    bool sr = isSelf(bin->right.get());
                    if (sl || sr) {
                        // For subtraction, only x - delta is an accumulation.
                        if (bin->op == "-" && !sl) return false;
                        const Expression* deltaExpr =
                            sl ? bin->right.get() : bin->left.get();
                        LRLinear d = lrLinearize(deltaExpr, iv, snap, modVars);
                        if (!d.valid) return false;
                        int64_t sign = (bin->op == "-") ? -1 : 1;
                        effects.push_back({LREffect::Kind::ADD, varName,
                                           d.a * sign, d.b * sign});
                        continue;
                    }
                }

                // x = x * factor  (factor must be loop-invariant)
                if (bin->op == "*") {
                    bool sl = isSelf(bin->left.get());
                    bool sr = isSelf(bin->right.get());
                    if (sl || sr) {
                        const Expression* factExpr =
                            sl ? bin->right.get() : bin->left.get();
                        LRLinear f = lrLinearize(factExpr, iv, snap, modVars);
                        if (!f.valid || f.a != 0) return false;
                        effects.push_back({LREffect::Kind::MUL, varName, 0, f.b});
                        continue;
                    }
                }

                // x = x ^ mask  /  x = mask ^ x  (mask invariant)
                if (bin->op == "^") {
                    bool sl = isSelf(bin->left.get());
                    bool sr = isSelf(bin->right.get());
                    if (sl || sr) {
                        const Expression* maskExpr =
                            sl ? bin->right.get() : bin->left.get();
                        LRLinear m = lrLinearize(maskExpr, iv, snap, modVars);
                        if (!m.valid || m.a != 0) return false;
                        effects.push_back({LREffect::Kind::XOR, varName, 0, m.b});
                        continue;
                    }
                }

                // x = x & mask
                if (bin->op == "&") {
                    bool sl = isSelf(bin->left.get());
                    bool sr = isSelf(bin->right.get());
                    if (sl || sr) {
                        const Expression* maskExpr =
                            sl ? bin->right.get() : bin->left.get();
                        LRLinear m = lrLinearize(maskExpr, iv, snap, modVars);
                        if (!m.valid || m.a != 0) return false;
                        effects.push_back({LREffect::Kind::AND, varName, 0, m.b});
                        continue;
                    }
                }

                // x = x | mask
                if (bin->op == "|") {
                    bool sl = isSelf(bin->left.get());
                    bool sr = isSelf(bin->right.get());
                    if (sl || sr) {
                        const Expression* maskExpr =
                            sl ? bin->right.get() : bin->left.get();
                        LRLinear m = lrLinearize(maskExpr, iv, snap, modVars);
                        if (!m.valid || m.a != 0) return false;
                        effects.push_back({LREffect::Kind::OR, varName, 0, m.b});
                        continue;
                    }
                }
            }

            // x = min(x, expr)  /  x = max(x, expr)  /  x = min(expr, x) etc.
            if (rhs->type == ASTNodeType::CALL_EXPR) {
                auto* call = static_cast<const CallExpr*>(rhs);
                const std::string& fn = call->callee;
                if ((fn == "min" || fn == "max") && call->arguments.size() == 2) {
                    auto isSelfArg = [&](const Expression* e) -> bool {
                        return e->type == ASTNodeType::IDENTIFIER_EXPR &&
                               static_cast<const IdentifierExpr*>(e)->name == varName;
                    };
                    bool sl = isSelfArg(call->arguments[0].get());
                    bool sr = isSelfArg(call->arguments[1].get());
                    if (sl || sr) {
                        const Expression* otherExpr =
                            sl ? call->arguments[1].get() : call->arguments[0].get();
                        LRLinear o = lrLinearize(otherExpr, iv, snap, modVars);
                        if (!o.valid) return false;
                        // Encode (a, b) of the other-side expression into the effect.
                        // a == 0 → pure invariant; a != 0 → linear in iv.
                        effects.push_back({
                            (fn == "min") ? LREffect::Kind::MIN : LREffect::Kind::MAX,
                            varName, o.a, o.b});
                        continue;
                    }
                }
            }

            // Pure SET:  x = expr  (RHS must not reference varName itself,
            // which lrLinearize enforces via modVars containing varName).
            LRLinear rhs_info = lrLinearize(rhs, iv, snap, modVars);
            if (!rhs_info.valid) return false;
            effects.push_back({LREffect::Kind::SET, varName,
                                rhs_info.a, rhs_info.b});
            continue;
        }

        return false;  // unrecognized expression statement
    }

    return true;
}

// Apply a set of loop effects after N full iterations.
static void lrApplyEffects(
    CTFrame&                        frame,
    const std::vector<LREffect>&    effects,
    int64_t                         start,
    int64_t                         step,
    int64_t                         N)
{
    // Sum of the induction variable over all N iterations:
    const __int128 bigN    = static_cast<__int128>(N);
    const __int128 bigS    = static_cast<__int128>(start);
    const __int128 bigStep = static_cast<__int128>(step);
    const int64_t  sumIV   = static_cast<int64_t>(
        static_cast<uint64_t>(bigN * bigS + bigStep * bigN * (bigN - 1) / 2));

    for (const auto& eff : effects) {
        auto it  = frame.locals.find(eff.var);
        int64_t cur = (it != frame.locals.end() && it->second.isInt())
                      ? it->second.asI64() : 0;

        switch (eff.kind) {

        case LREffect::Kind::INCR:
            // x++ N times  →  x += N
            frame.locals[eff.var] = CTValue::fromI64(
                static_cast<int64_t>(static_cast<uint64_t>(cur) +
                                     static_cast<uint64_t>(N)));
            break;

        case LREffect::Kind::DECR:
            // x-- N times  →  x -= N
            frame.locals[eff.var] = CTValue::fromI64(
                static_cast<int64_t>(static_cast<uint64_t>(cur) -
                                     static_cast<uint64_t>(N)));
            break;

        case LREffect::Kind::ADD: {
            // delta per iteration = eff.a * iv + eff.b
            // total               = eff.a * Σiv + eff.b * N
            const int64_t total = static_cast<int64_t>(
                static_cast<uint64_t>(eff.a) * static_cast<uint64_t>(sumIV) +
                static_cast<uint64_t>(eff.b) * static_cast<uint64_t>(N));
            frame.locals[eff.var] = CTValue::fromI64(
                static_cast<int64_t>(static_cast<uint64_t>(cur) +
                                     static_cast<uint64_t>(total)));
            break;
        }

        case LREffect::Kind::MUL: {
            // x *= c, N times  →  x *= c^N
            int64_t r = 1;
            int64_t base = eff.b;
            int64_t rem  = N;
            while (rem > 0) {
                if (rem & 1) r = static_cast<int64_t>(
                    static_cast<uint64_t>(r) * static_cast<uint64_t>(base));
                base = static_cast<int64_t>(
                    static_cast<uint64_t>(base) * static_cast<uint64_t>(base));
                rem >>= 1;
            }
            frame.locals[eff.var] = CTValue::fromI64(
                static_cast<int64_t>(static_cast<uint64_t>(cur) *
                                     static_cast<uint64_t>(r)));
            break;
        }

        case LREffect::Kind::XOR:
            // x ^= c, N times  →  x ^ c  if N is odd, else x unchanged
            frame.locals[eff.var] = CTValue::fromI64(
                (N & 1) ? (cur ^ eff.b) : cur);
            break;

        case LREffect::Kind::AND:
            // x &= c, N≥1 times  →  x & c (idempotent)
            frame.locals[eff.var] = CTValue::fromI64(cur & eff.b);
            break;

        case LREffect::Kind::OR:
            // x |= c, N≥1 times  →  x | c (idempotent)
            frame.locals[eff.var] = CTValue::fromI64(cur | eff.b);
            break;

        case LREffect::Kind::SET: {
            // x = a*iv + b  →  value at last iteration iv_{N-1} = start+(N-1)*step
            const int64_t lastIV = static_cast<int64_t>(
                static_cast<uint64_t>(start) +
                static_cast<uint64_t>(N - 1) * static_cast<uint64_t>(step));
            frame.locals[eff.var] = CTValue::fromI64(
                static_cast<int64_t>(
                    static_cast<uint64_t>(eff.a) * static_cast<uint64_t>(lastIV) +
                    static_cast<uint64_t>(eff.b)));
            break;
        }

        case LREffect::Kind::MIN:
        case LREffect::Kind::MAX: {
            // x = min/max(x, a*iv + b) for k = 0..N-1.
            const __int128 ivFirst128 = static_cast<__int128>(start);
            const __int128 ivLast128  = ivFirst128 +
                static_cast<__int128>(step) * static_cast<__int128>(N - 1);
            const __int128 a128 = static_cast<__int128>(eff.a);
            const __int128 b128 = static_cast<__int128>(eff.b);
            // Wrap intermediate products via uint64 cast (matches OmScript
            // wrapping arithmetic — same convention used by the SET case).
            const int64_t valFirst = static_cast<int64_t>(
                static_cast<uint64_t>(static_cast<int64_t>(a128 * ivFirst128 + b128)));
            const int64_t valLast  = static_cast<int64_t>(
                static_cast<uint64_t>(static_cast<int64_t>(a128 * ivLast128  + b128)));
            int64_t reduced = (eff.kind == LREffect::Kind::MIN)
                ? std::min(valFirst, valLast)
                : std::max(valFirst, valLast);
            int64_t out = (eff.kind == LREffect::Kind::MIN)
                ? std::min(cur, reduced)
                : std::max(cur, reduced);
            frame.locals[eff.var] = CTValue::fromI64(out);
            break;
        }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────

bool CTEngine::tryReasonForLoop(CTFrame& frame, const ForStmt* fs,
                                 int64_t start, int64_t /*end*/, int64_t step,
                                 int64_t N)
{
    // Collect all variables written by the loop body.
    std::unordered_set<std::string> modVars;
    lrCollectWrites(fs->body.get(), modVars);

    // If the loop body writes to the loop variable itself, bail out.
    if (modVars.count(fs->iteratorVar)) return false;

    // Build a snapshot of pre-loop locals, excluding modified variables and
    // the loop induction variable (which changes each iteration).
    std::unordered_map<std::string, CTValue> snap;
    snap.reserve(frame.locals.size());
    for (auto& [k, v] : frame.locals) {
        if (!modVars.count(k) && k != fs->iteratorVar)
            snap[k] = v;
    }

    // Analyze every statement in the body.
    std::vector<LREffect> effects;
    if (!lrAnalyzeBody(fs->body.get(), fs->iteratorVar, snap, modVars, effects))
        return false;

    // Apply closed-form updates.
    lrApplyEffects(frame, effects, start, step, N);
    ++stats_.loopsReasoned;
    return true;
}



bool CTEngine::evalStmt(CTFrame& frame, const Statement* s) {
    if (!s || frame.hasReturned || frame.didBreak || frame.didContinue) return true;
    ++stats_.instructionsExecuted;
    if (++fuel_ > kMaxInstructions) return false;

    switch (s->type) {

    // ── Return ────────────────────────────────────────────────────────────
    case ASTNodeType::RETURN_STMT: {
        auto* ret = static_cast<const ReturnStmt*>(s);
        if (!ret->value) { frame.returnValue = CTValue::fromI64(0); frame.hasReturned = true; return true; }
        CTValue v = evalExpr(frame, ret->value.get());
        // Propagate SYMBOLIC returns — caller (executeFunction) will check and
        // discard them rather than baking a symbolic value into the IR.
        if (!v.isKnown() && !v.isSymbolic()) return false;
        frame.returnValue = std::move(v);
        frame.hasReturned = true;
        return true;
    }

    // ── Variable declaration ──────────────────────────────────────────────
    case ASTNodeType::VAR_DECL: {
        auto* decl = static_cast<const VarDecl*>(s);
        CTValue v = decl->initializer
                    ? evalExpr(frame, decl->initializer.get())
                    : CTValue::fromI64(0);
        // Allow SYMBOLIC to be stored — it propagates through subsequent uses
        // of this variable.  UNINITIALIZED (truly unknown expr) still aborts.
        if (!v.isKnown() && !v.isSymbolic()) return false;
        frame.locals[decl->name] = std::move(v);
        return true;
    }

    // ── Expression statement ──────────────────────────────────────────────
    case ASTNodeType::EXPR_STMT: {
        auto* es = static_cast<const ExprStmt*>(s);
        // Named assignment: name = val
        if (es->expression->type == ASTNodeType::ASSIGN_EXPR) {
            auto* assign = static_cast<const AssignExpr*>(es->expression.get());
            CTValue v = evalExpr(frame, assign->value.get());
            // Allow SYMBOLIC assignments — propagates unknown value into variable.
            if (!v.isKnown() && !v.isSymbolic()) return false;
            frame.locals[assign->name] = std::move(v);
            return true;
        }
        // Array element assignment: arr[i] = val (compound or plain)
        if (es->expression->type == ASTNodeType::INDEX_ASSIGN_EXPR) {
            auto* ia = static_cast<const IndexAssignExpr*>(es->expression.get());
            CTValue arrVal = evalExpr(frame, ia->array.get());
            CTValue idxVal = evalExpr(frame, ia->index.get());
            CTValue newVal = evalExpr(frame, ia->value.get());
            if (!arrVal.isKnown() || !idxVal.isKnown() || !newVal.isKnown()) return false;
            if (arrVal.isSymbolic() || idxVal.isSymbolic()) return false;
            if (!arrVal.isArray() || !idxVal.isInt()) return false;
            heap_.store(arrVal.asArr(), idxVal.asI64(), std::move(newVal));
            return true;
        }
        // General expression (++/-- / function call with side effects / etc.)
        CTValue v = evalExpr(frame, es->expression.get());
        if (v.isConcrete()) { frame.lastBareExpr = v; frame.hasLastBare = true; }
        return v.isKnown(); // includes SYMBOLIC (just don't set lastBare)
    }

    // ── If / else ─────────────────────────────────────────────────────────
    case ASTNodeType::IF_STMT: {
        auto* ifs = static_cast<const IfStmt*>(s);
        CTValue cond = evalExpr(frame, ifs->condition.get());

        // ── Path-sensitive branch merge ───────────────────────────────────
        if (cond.isSymbolic() && fuel_ + 2 <= kMaxInstructions) {
            const int64_t savedFuel = fuel_;

            // Fork A: then-branch.
            CTFrame thenF = frame;
            thenF.hasReturned = false; thenF.didBreak = false; thenF.didContinue = false;
            // Narrow variable ranges inside each branch based on the branch condition.
            CTFrame elsePreF = frame;   // pre-narrow else frame (shared setup)
            elsePreF.hasReturned = false; elsePreF.didBreak = false; elsePreF.didContinue = false;
            narrowBranchConstraints(ifs->condition.get(), thenF, elsePreF);
            bool thenOk = evalStmt(thenF, ifs->thenBranch.get());
            const int64_t fuelAfterThen = fuel_;

            // Fork B: else-branch (already narrowed by narrowBranchConstraints above).
            CTFrame& elseF = elsePreF;
            fuel_ = savedFuel;
            bool elseOk = true;
            if (ifs->elseBranch)
                elseOk = evalStmt(elseF, ifs->elseBranch.get());
            const int64_t fuelAfterElse = fuel_;
            fuel_ = std::max(fuelAfterThen, fuelAfterElse);

            // Case 1: both branches return the same concrete non-array value.
            auto isConcreteScalarReturn = [](const CTFrame& f) {
                return f.hasReturned &&
                       !f.returnValue.isArray() &&
                       !f.returnValue.isSymbolic();
            };
            if (thenOk && elseOk &&
                isConcreteScalarReturn(thenF) && isConcreteScalarReturn(elseF) &&
                thenF.returnValue == elseF.returnValue) {
                frame.returnValue = thenF.returnValue;
                frame.hasReturned = true;
                ++stats_.branchMerges;
                return true;
            }

            // Case 2: neither branch returns or breaks — merge locals.
            if (thenOk && !thenF.hasReturned && !thenF.didBreak &&
                elseOk && !elseF.hasReturned && !elseF.didBreak) {
                // Apply merged state: iterate over all keys in both branches.
                for (auto& [k, v] : thenF.locals) {
                    auto eit = elseF.locals.find(k);
                    if (eit != elseF.locals.end()) {
                        frame.locals[k] = (v == eit->second) ? v : CTValue::symbolic();
                    } else {
                        // Present in then-branch only → conditionally defined → symbolic.
                        frame.locals[k] = CTValue::symbolic();
                    }
                }
                for (auto& [k, v] : elseF.locals) {
                    if (!thenF.locals.count(k))
                        frame.locals[k] = CTValue::symbolic();
                }
                ++stats_.branchMerges;
                return true;
            }

            // Merge failed — locals are unchanged (heap may have new dangling
            // handles, but they won't be referenced by anyone).
            return false;
        }

        if (!cond.isKnown() || cond.isSymbolic()) return false;
        if (cond.isTruthy()) return evalStmt(frame, ifs->thenBranch.get());
        if (ifs->elseBranch) return evalStmt(frame, ifs->elseBranch.get());
        return true;
    }

    // ── For-range loop ────────────────────────────────────────────────────
    case ASTNodeType::FOR_STMT: {
        auto* fs = static_cast<const ForStmt*>(s);
        CTValue sv = evalExpr(frame, fs->start.get());
        CTValue ev = evalExpr(frame, fs->end.get());
        if (!sv.isKnown() || !ev.isKnown() || !sv.isInt() || !ev.isInt()) return false;
        int64_t step = (sv.asI64() <= ev.asI64()) ? 1 : -1;
        if (fs->step) {
            CTValue stv = evalExpr(frame, fs->step.get());
            if (!stv.isKnown() || !stv.isInt()) return false;
            step = stv.asI64();
        }
        if (step == 0) return false;
        const int64_t startV = sv.asI64();
        const int64_t endV   = ev.asI64();

        // Compute exact iteration count N.
        int64_t N = 0;
        if (step > 0 && startV < endV)
            N = (endV - startV - 1) / step + 1;
        else if (step < 0 && startV > endV)
            N = (startV - endV - 1) / (-step) + 1;
        // else N = 0 (zero-trip loop)

        if (N <= 0) {
            frame.locals.erase(fs->iteratorVar);
            return true;
        }

        // ── Symbolic reasoning path ───────────────────────────────────────
        if (tryReasonForLoop(frame, fs, startV, endV, step, N)) {
            frame.locals.erase(fs->iteratorVar);
            return true;
        }

        // ── Direct iteration (fallback for complex bodies) ────────────────
        int64_t cur = startV;
        while (step > 0 ? cur < endV : cur > endV) {
            if (++fuel_ > kMaxInstructions) return false;
            frame.locals[fs->iteratorVar] = CTValue::fromI64(cur);
            if (!evalStmt(frame, fs->body.get())) return false;
            if (frame.hasReturned) { frame.locals.erase(fs->iteratorVar); return true; }
            if (frame.didBreak) { frame.didBreak = false; break; }
            frame.didContinue = false;
            cur += step;
        }
        frame.locals.erase(fs->iteratorVar);
        return true;
    }

    // ── ForEach loop ──────────────────────────────────────────────────────
    case ASTNodeType::FOR_EACH_STMT: {
        auto* fes = static_cast<const ForEachStmt*>(s);
        CTValue coll = evalExpr(frame, fes->collection.get());
        if (!coll.isKnown()) return false;
        if (coll.isString()) {
            const std::string str = coll.asStr();
            for (size_t i = 0; i < str.size(); ++i) {
                if (++fuel_ > kMaxInstructions) return false;
                frame.locals[fes->iteratorVar] = CTValue::fromI64(static_cast<unsigned char>(str[i]));
                if (!evalStmt(frame, fes->body.get())) return false;
                if (frame.hasReturned) { frame.locals.erase(fes->iteratorVar); return true; }
                if (frame.didBreak) { frame.didBreak = false; break; }
                frame.didContinue = false;
            }
        } else if (coll.isArray()) {
            // Snapshot the elements: the loop body may mutate the heap, so we
            // need our own stable copy of the array data to iterate over.
            auto elems = extractArray(coll.asArr());
            for (const auto& elem : elems) {
                if (++fuel_ > kMaxInstructions) return false;
                frame.locals[fes->iteratorVar] = elem;
                if (!evalStmt(frame, fes->body.get())) return false;
                if (frame.hasReturned) { frame.locals.erase(fes->iteratorVar); return true; }
                if (frame.didBreak) { frame.didBreak = false; break; }
                frame.didContinue = false;
            }
        } else {
            return false;
        }
        frame.locals.erase(fes->iteratorVar);
        return true;
    }

    // ── While loop ────────────────────────────────────────────────────────
    case ASTNodeType::WHILE_STMT: {
        auto* ws = static_cast<const WhileStmt*>(s);
        while (true) {
            if (++fuel_ > kMaxInstructions) return false;
            CTValue cond = evalExpr(frame, ws->condition.get());
            if (!cond.isKnown()) return false;
            if (!cond.isTruthy()) break;
            if (!evalStmt(frame, ws->body.get())) return false;
            if (frame.hasReturned) return true;
            if (frame.didBreak) { frame.didBreak = false; break; }
            frame.didContinue = false;
        }
        return true;
    }

    // ── Do-while loop ─────────────────────────────────────────────────────
    case ASTNodeType::DO_WHILE_STMT: {
        auto* dw = static_cast<const DoWhileStmt*>(s);
        do {
            if (++fuel_ > kMaxInstructions) return false;
            if (!evalStmt(frame, dw->body.get())) return false;
            if (frame.hasReturned) return true;
            if (frame.didBreak) { frame.didBreak = false; break; }
            frame.didContinue = false;
            CTValue cond = evalExpr(frame, dw->condition.get());
            if (!cond.isKnown()) return false;
            if (!cond.isTruthy()) break;
        } while (true);
        return true;
    }

    // ── Switch ────────────────────────────────────────────────────────────
    case ASTNodeType::SWITCH_STMT: {
        auto* sw = static_cast<const SwitchStmt*>(s);
        CTValue cond = evalExpr(frame, sw->condition.get());
        if (!cond.isKnown() || !cond.isInt()) return false;
        int64_t condVal = cond.asI64();
        const SwitchCase* matched = nullptr;
        const SwitchCase* defaultCase = nullptr;
        for (auto& c : sw->cases) {
            if (c.isDefault) { defaultCase = &c; continue; }
            if (c.value) {
                CTValue cv = evalExpr(frame, c.value.get());
                if (!cv.isKnown() || !cv.isInt()) return false;
                if (cv.asI64() == condVal) { matched = &c; break; }
            }
            bool found = false;
            for (auto& vx : c.values) {
                CTValue cv = evalExpr(frame, vx.get());
                if (!cv.isKnown() || !cv.isInt()) return false;
                if (cv.asI64() == condVal) { found = true; break; }
            }
            if (found) { matched = &c; break; }
        }
        const SwitchCase* target = matched ? matched : defaultCase;
        if (!target) return true;
        for (auto& stmt : target->body) {
            if (!evalStmt(frame, stmt.get())) return false;
            if (frame.hasReturned) return true;
            if (frame.didBreak) { frame.didBreak = false; return true; }
            if (frame.didContinue) return true;
        }
        return true;
    }

    // ── Break / Continue ──────────────────────────────────────────────────
    case ASTNodeType::BREAK_STMT:
        frame.didBreak = true; return true;
    case ASTNodeType::CONTINUE_STMT:
        frame.didContinue = true; return true;

    // ── Block ─────────────────────────────────────────────────────────────
    case ASTNodeType::BLOCK: {
        auto* blk = static_cast<const BlockStmt*>(s);
        // Shadow tracking for scope correctness.
        std::vector<std::pair<std::string, std::optional<CTValue>>> scopeGuard;
        for (auto& stmt : blk->statements) {
            if (stmt->type == ASTNodeType::VAR_DECL) {
                auto* decl = static_cast<const VarDecl*>(stmt.get());
                auto it = frame.locals.find(decl->name);
                if (it != frame.locals.end())
                    scopeGuard.emplace_back(decl->name, it->second);
                else
                    scopeGuard.emplace_back(decl->name, std::nullopt);
            }
            if (!evalStmt(frame, stmt.get())) {
                for (auto& [nm, val] : scopeGuard) {
                    if (val) frame.locals[nm] = *val; else frame.locals.erase(nm);
                }
                return false;
            }
            if (frame.hasReturned || frame.didBreak || frame.didContinue) {
                for (auto& [nm, val] : scopeGuard) {
                    if (val) frame.locals[nm] = *val; else frame.locals.erase(nm);
                }
                return true;
            }
        }
        for (auto& [nm, val] : scopeGuard) {
            if (val) frame.locals[nm] = *val; else frame.locals.erase(nm);
        }
        return true;
    }

    // ── Pipeline statement — SIMD tile execution ───────────────────────────
    case ASTNodeType::PIPELINE_STMT:
        return evalPipelineStmt(frame, s);

    // ── Assume / freeze / invalidate / prefetch — skip safely ────────────
    case ASTNodeType::ASSUME_STMT:
    case ASTNodeType::FREEZE_STMT:
    case ASTNodeType::INVALIDATE_STMT:
    case ASTNodeType::PREFETCH_STMT:
    case ASTNodeType::DEFER_STMT:
        return true;  // no-op in CT evaluation

    // ── MoveDecl: treat like VarDecl ──────────────────────────────────────
    case ASTNodeType::MOVE_DECL: {
        auto* md = static_cast<const MoveDecl*>(s);
        CTValue v = md->initializer ? evalExpr(frame, md->initializer.get()) : CTValue::fromI64(0);
        if (!v.isKnown()) return false;
        frame.locals[md->name] = std::move(v);
        return true;
    }

    default:
        // try/catch, throw, I/O — not safe to evaluate at compile time.
        return false;
    }
}

// ═══════════════════════════════════════════════════════════════════════════

bool CTEngine::evalPipelineStmt(CTFrame& frame, const Statement* s) {
    auto* ps = static_cast<const PipelineStmt*>(s);
    const int nStages = static_cast<int>(ps->stages.size());

    // Helper: execute non-final stages with failure semantics, then always
    // run the last stage.  Returns true if pipeline should continue iterating.
    auto runStagesWithFailure = [&]() -> bool {
        if (nStages <= 1) {
            // Single stage — just run it.
            if (!executeBody(frame, ps->stages[0].body.get())) return false;
            return true;
        }

        bool stageFailed = false;

        // Execute non-final stages; on failure, skip remaining middle stages.
        for (int si = 0; si < nStages - 1; ++si) {
            if (!evalStmt(frame, ps->stages[si].body.get())) {
                stageFailed = true;
                break;
            }
            if (frame.hasReturned || frame.didBreak) {
                stageFailed = true;
                break;
            }
        }

        // Last stage always runs ("finally").
        if (!evalStmt(frame, ps->stages[nStages - 1].body.get()))
            return false;

        // If a middle stage failed, signal cancellation to the caller.
        if (stageFailed) return false;
        return true;
    };

    // One-shot form (no count): execute stages once with failure semantics.
    if (!ps->count) {
        runStagesWithFailure();
        return true;
    }

    CTValue countVal = evalExpr(frame, ps->count.get());
    if (!countVal.isKnown() || !countVal.isInt()) return false;
    int64_t n = countVal.asI64();
    if (n <= 0) return true;  // zero-trip loop

    // Collect all stage bodies in order.
    std::vector<const Statement*> stageStmts;
    for (auto& stage : ps->stages)
        stageStmts.push_back(static_cast<const Statement*>(stage.body.get()));

    // Execute in SIMD tiles of kSIMDLaneWidth (spec §9).
    int64_t nTiles = (n + kSIMDLaneWidth - 1) / kSIMDLaneWidth;
    for (int64_t tile = 0; tile < nTiles; ++tile) {
        int64_t base = tile * kSIMDLaneWidth;
        executeTile(frame, stageStmts, base, n);
        if (frame.hasReturned || frame.didBreak) return true;
        ++stats_.pipelineTilesExecuted;
    }
    return true;
}

void CTEngine::executeTile(CTFrame& frame,
                            const std::vector<const Statement*>& stageStmts,
                            int64_t baseIdx, int64_t n) {
    const int nStages = static_cast<int>(stageStmts.size());

    // Execute each lane in the tile sequentially.
    for (int lane = 0; lane < kSIMDLaneWidth; ++lane) {
        int64_t i = baseIdx + lane;
        if (i >= n) break;  // mask inactive lanes
        // Set the pipeline iterator.
        frame.locals["__pipeline_i"] = CTValue::fromI64(i);

        if (nStages <= 1) {
            // Single stage — just run it.
            if (!evalStmt(frame, stageStmts[0])) return;
            if (frame.hasReturned || frame.didBreak || frame.didContinue) return;
        } else {
            // Execute non-final stages; on failure skip to last stage.
            bool stageFailed = false;
            for (int si = 0; si < nStages - 1; ++si) {
                if (!evalStmt(frame, stageStmts[si])) {
                    stageFailed = true;
                    break;
                }
                if (frame.hasReturned || frame.didBreak || frame.didContinue) {
                    stageFailed = true;
                    break;
                }
            }

            // Last stage always runs ("finally").
            if (!evalStmt(frame, stageStmts[nStages - 1])) return;
            if (frame.hasReturned || frame.didBreak || frame.didContinue) return;

            // If a middle stage failed, cancel the pipeline (no more lanes).
            if (stageFailed) return;
        }

        frame.didContinue = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

namespace {
constexpr int kCompoundDepthBudget = 6;

// Atomic narrowing: handle one comparison (varName op literal) with optional
inline void mergeConstraint(
    std::unordered_map<std::string, omscript::CTInterval>& dst,
    const std::string& var,
    const omscript::CTInterval& iv)
{
    if (iv.isTop()) return;          // adds nothing
    auto it = dst.find(var);
    if (it == dst.end()) {
        if (!iv.isBottom()) dst[var] = iv;
        else                dst[var] = omscript::CTInterval::bottom();
        return;
    }
    it->second = it->second.intersect(iv);
}

// Lookup the current concrete bound for `id` from an existing constraint set.
// Returns CTInterval::top() if no information is available.
inline omscript::CTInterval lookupConstraint(
    const std::unordered_map<std::string, omscript::CTInterval>& src,
    const std::string& var)
{
    auto it = src.find(var);
    return (it != src.end()) ? it->second : omscript::CTInterval::top();
}
} // namespace

void CTEngine::narrowBranchConstraints(const Expression* cond,
                                        CTFrame& thenF,
                                        CTFrame& elseF) const
{
    if (!cond) return;

    // ── Recursive worker — builds new constraint deltas without mutating frames
    // until the recursion completes, so we can apply A∧B / A∨B correctly.
    std::function<void(const Expression*,
                       std::unordered_map<std::string, CTInterval>&,
                       std::unordered_map<std::string, CTInterval>&,
                       int)> walk;
    walk = [&](const Expression* c,
               std::unordered_map<std::string, CTInterval>& thenC,
               std::unordered_map<std::string, CTInterval>& elseC,
               int depth)
    {
        if (!c || depth > kCompoundDepthBudget) return;

        // ── Logical NOT: !A swaps then/else narrowings of A ────────────────
        if (c->type == ASTNodeType::UNARY_EXPR) {
            auto* un = static_cast<const UnaryExpr*>(c);
            if (un->op == "!") {
                walk(un->operand.get(), elseC, thenC, depth + 1);
            }
            return;
        }

        if (c->type != ASTNodeType::BINARY_EXPR) return;
        auto* bin = static_cast<const BinaryExpr*>(c);
        const std::string& op = bin->op;

        // ── Logical AND: both narrowings apply on then; nothing on else ───
        if (op == "&&") {
            std::unordered_map<std::string, CTInterval> dummyElse;
            walk(bin->left.get(),  thenC, dummyElse, depth + 1);
            walk(bin->right.get(), thenC, dummyElse, depth + 1);
            return;
        }
        // ── Logical OR: both narrowings apply on else; nothing on then ───
        if (op == "||") {
            std::unordered_map<std::string, CTInterval> dummyThen;
            walk(bin->left.get(),  dummyThen, elseC, depth + 1);
            walk(bin->right.get(), dummyThen, elseC, depth + 1);
            return;
        }

        // ── Atomic comparison ──────────────────────────────────────────────
        static const std::unordered_set<std::string> kCmpOps{
            "==", "!=", "<", "<=", ">", ">="
        };
        if (!kCmpOps.count(op)) return;

        // Helper: from `op`, derive (then-narrow, else-narrow) on a CTInterval
        // applied to a numeric bound.
        auto deriveNarrow = [](const std::string& cmpOp, int64_t bound,
                                const CTInterval& start)
            -> std::pair<CTInterval, CTInterval>
        {
            if (cmpOp == "==") return {start.narrowEQ(bound), start.narrowNE(bound)};
            if (cmpOp == "!=") return {start.narrowNE(bound), start.narrowEQ(bound)};
            if (cmpOp == "<")  return {start.narrowLT(bound), start.narrowGE(bound)};
            if (cmpOp == "<=") return {start.narrowLE(bound), start.narrowGT(bound)};
            if (cmpOp == ">")  return {start.narrowGT(bound), start.narrowLE(bound)};
            if (cmpOp == ">=") return {start.narrowGE(bound), start.narrowLT(bound)};
            return {start, start};
        };

        // Resolve each side to either an identifier name or a concrete int.
        auto resolveSide = [&](const Expression* side,
                                std::string& varName,
                                int64_t& litVal,
                                bool& isVar,
                                bool& isLit)
        {
            isVar = isLit = false;
            if (!side) return;
            if (side->type == ASTNodeType::IDENTIFIER_EXPR) {
                isVar = true;
                varName = static_cast<const IdentifierExpr*>(side)->name;
                // Promote known concrete locals/globals/enums to literals.
                CTInterval iv = lookupConstraint(thenC, varName);
                if (iv.isTop()) iv = lookupConstraint(thenF.constraints, varName);
                if (iv.isConcrete()) { isLit = true; litVal = iv.lo; }
                else {
                    auto lit = thenF.locals.find(varName);
                    if (lit != thenF.locals.end() && lit->second.isInt()) {
                        isLit = true;
                        litVal = lit->second.asI64();
                    } else {
                        auto gIt = globalConsts_.find(varName);
                        if (gIt != globalConsts_.end() && gIt->second.isInt()) {
                            isLit = true;
                            litVal = gIt->second.asI64();
                        } else {
                            auto eIt = enumConsts_.find(varName);
                            if (eIt != enumConsts_.end()) {
                                isLit = true;
                                litVal = eIt->second;
                            }
                        }
                    }
                }
            } else if (side->type == ASTNodeType::LITERAL_EXPR) {
                auto* lit = static_cast<const LiteralExpr*>(side);
                if (lit->literalType == LiteralExpr::LiteralType::INTEGER) {
                    isLit = true;
                    litVal = static_cast<int64_t>(lit->intValue);
                }
            }
        };

        std::string lName, rName;
        int64_t     lVal = 0,  rVal = 0;
        bool lIsVar=false, lIsLit=false, rIsVar=false, rIsLit=false;
        resolveSide(bin->left.get(),  lName, lVal, lIsVar, lIsLit);
        resolveSide(bin->right.get(), rName, rVal, rIsVar, rIsLit);

        const CTInterval full = CTInterval::top();

        // Case A: var ∘ literal  →  narrow the variable.
        if (lIsVar && rIsLit) {
            auto [t, e] = deriveNarrow(op, rVal, full);
            mergeConstraint(thenC, lName, t);
            mergeConstraint(elseC, lName, e);
        }
        // Case B: literal ∘ var  →  flip the operator and narrow the variable.
        else if (rIsVar && lIsLit) {
            std::string flipped = op;
            if      (op == "<")  flipped = ">";
            else if (op == "<=") flipped = ">=";
            else if (op == ">")  flipped = "<";
            else if (op == ">=") flipped = "<=";
            auto [t, e] = deriveNarrow(flipped, lVal, full);
            mergeConstraint(thenC, rName, t);
            mergeConstraint(elseC, rName, e);
        }
        // Case C: var ∘ var with one side known concrete → narrow the other.
        else if (lIsVar && rIsVar) {
            if (rIsLit) { // right side resolved to a known int via constraints
                auto [t, e] = deriveNarrow(op, rVal, full);
                mergeConstraint(thenC, lName, t);
                mergeConstraint(elseC, lName, e);
            }
            if (lIsLit) {
                std::string flipped = op;
                if      (op == "<")  flipped = ">";
                else if (op == "<=") flipped = ">=";
                else if (op == ">")  flipped = "<";
                else if (op == ">=") flipped = "<=";
                auto [t, e] = deriveNarrow(flipped, lVal, full);
                mergeConstraint(thenC, rName, t);
                mergeConstraint(elseC, rName, e);
            }
        }
        // Other patterns (compound expressions on either side) are not yet
        // narrowed; fall through silently (conservative).
    };

    // Build local working maps so compound walking can compose results.
    std::unordered_map<std::string, CTInterval> thenC, elseC;
    walk(cond, thenC, elseC, 0);

    // Commit non-trivial deltas back to the frames, intersecting with any
    // prior constraint (preserves outer-guard narrowing).
    for (auto& [var, iv] : thenC) {
        if (iv.isTop()) continue;
        auto it = thenF.constraints.find(var);
        thenF.constraints[var] =
            (it == thenF.constraints.end()) ? iv : it->second.intersect(iv);
    }
    for (auto& [var, iv] : elseC) {
        if (iv.isTop()) continue;
        auto it = elseF.constraints.find(var);
        elseF.constraints[var] =
            (it == elseF.constraints.end()) ? iv : it->second.intersect(iv);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
std::string CTEngine::partialSpecKey(const std::string& fnName,
                                      const std::vector<CTValue>& args) const
{
    std::string key;
    key.reserve(fnName.size() + args.size() * 16 + 2);
    key.append(fnName);
    key.push_back('|');
    for (const auto& a : args) {
        a.appendMemoHash(key);   // handles SYMBOLIC via the symId path added in Phase A
        key.push_back(',');
    }
    return key;
}

// ═══════════════════════════════════════════════════════════════════════════

void CTEngine::runPass(const Program* program) {
    if (!program) return;

    // ── Phase 1: register all functions ──────────────────────────────────
    for (auto& fn : program->functions)
        registerFunction(fn->name, fn.get());

    // Register enum constants.
    for (auto& en : program->enums) {
        for (auto& [mname, mval] : en->members) {
            registerEnumConst(en->name + "_" + mname, static_cast<int64_t>(mval));
            registerEnumConst(en->name + "::" + mname, static_cast<int64_t>(mval));
        }
    }

    // ── Phase 2: purity analysis — mark @pure / @const_eval functions ────
    for (auto& fn : program->functions) {
        if (fn->hintPure || fn->hintConstEval) markPure(fn->name);
    }

    // ── Phase 3: auto-detect pure functions (fixed-point) ────────────────
    // Detect whether a function body is pure (no I/O, no mutations of globals).
    // Uses a recursive helper with a visited set to handle mutual recursion.
    // Builtin purity is delegated to BuiltinEffectTable (the authoritative
    // single source of truth); we no longer maintain a local copy here.
    std::function<bool(const FunctionDecl*, std::unordered_set<std::string>&)> isPureBody;
    isPureBody = [&](const FunctionDecl* fn, std::unordered_set<std::string>& visiting) -> bool {
        if (!fn || !fn->body) return false;
        if (BuiltinEffectTable::isPure(fn->name) || BuiltinEffectTable::isWidthCastName(fn->name)) return true;
        if (pureFunctions_.count(fn->name)) return true;
        if (visiting.count(fn->name)) return false; // conservatively not pure (recursion)
        visiting.insert(fn->name);

        // Walk the body; reject if any I/O or non-pure call is found.
        std::function<bool(const Statement*)>  pureS;
        std::function<bool(const Expression*)> pureE;
        pureE = [&](const Expression* ex) -> bool {
            if (!ex) return true;
            if (ex->type == ASTNodeType::CALL_EXPR) {
                auto* call = static_cast<const CallExpr*>(ex);
                if (BuiltinEffectTable::isImpure(call->callee)) return false;
                if (!BuiltinEffectTable::isPure(call->callee) && !BuiltinEffectTable::isWidthCastName(call->callee)) {
                    auto it = functions_.find(call->callee);
                    if (it != functions_.end()) {
                        if (!isPureBody(it->second, visiting)) return false;
                    } else {
                        return false; // unknown external — assume impure
                    }
                }
                for (auto& arg : call->arguments)
                    if (!pureE(arg.get())) return false;
                return true;
            }
            // Other expressions: recurse into sub-expressions.
            if (ex->type == ASTNodeType::BINARY_EXPR) {
                auto* b = static_cast<const BinaryExpr*>(ex);
                return pureE(b->left.get()) && pureE(b->right.get());
            }
            if (ex->type == ASTNodeType::UNARY_EXPR) {
                auto* u = static_cast<const UnaryExpr*>(ex);
                return pureE(u->operand.get());
            }
            if (ex->type == ASTNodeType::TERNARY_EXPR) {
                auto* t = static_cast<const TernaryExpr*>(ex);
                return pureE(t->condition.get()) && pureE(t->thenExpr.get()) && pureE(t->elseExpr.get());
            }
            if (ex->type == ASTNodeType::INDEX_EXPR) {
                auto* idx = static_cast<const IndexExpr*>(ex);
                return pureE(idx->array.get()) && pureE(idx->index.get());
            }
            if (ex->type == ASTNodeType::ARRAY_EXPR) {
                auto* ae = static_cast<const ArrayExpr*>(ex);
                for (auto& el : ae->elements)
                    if (!pureE(el.get())) return false;
                return true;
            }
            return true;
        };
        pureS = [&](const Statement* st) -> bool {
            if (!st) return true;
            if (st->type == ASTNodeType::THROW_STMT) return false;
            if (st->type == ASTNodeType::CATCH_STMT) return false;
            if (st->type == ASTNodeType::RETURN_STMT) {
                auto* r = static_cast<const ReturnStmt*>(st);
                return !r->value || pureE(r->value.get());
            }
            if (st->type == ASTNodeType::VAR_DECL) {
                auto* d = static_cast<const VarDecl*>(st);
                return !d->initializer || pureE(d->initializer.get());
            }
            if (st->type == ASTNodeType::MOVE_DECL) {
                auto* md = static_cast<const MoveDecl*>(st);
                return !md->initializer || pureE(md->initializer.get());
            }
            if (st->type == ASTNodeType::EXPR_STMT) {
                auto* es = static_cast<const ExprStmt*>(st);
                return pureE(es->expression.get());
            }
            if (st->type == ASTNodeType::IF_STMT) {
                auto* ifs = static_cast<const IfStmt*>(st);
                return pureE(ifs->condition.get()) &&
                       pureS(ifs->thenBranch.get()) &&
                       (!ifs->elseBranch || pureS(ifs->elseBranch.get()));
            }
            if (st->type == ASTNodeType::BLOCK) {
                auto* blk = static_cast<const BlockStmt*>(st);
                for (auto& s2 : blk->statements)
                    if (!pureS(s2.get())) return false;
                return true;
            }
            if (st->type == ASTNodeType::FOR_STMT) {
                auto* fs = static_cast<const ForStmt*>(st);
                return pureE(fs->start.get()) && pureE(fs->end.get()) &&
                       (!fs->step || pureE(fs->step.get())) && pureS(fs->body.get());
            }
            if (st->type == ASTNodeType::FOR_EACH_STMT) {
                auto* fes = static_cast<const ForEachStmt*>(st);
                return pureE(fes->collection.get()) && pureS(fes->body.get());
            }
            if (st->type == ASTNodeType::WHILE_STMT) {
                auto* ws = static_cast<const WhileStmt*>(st);
                return pureE(ws->condition.get()) && pureS(ws->body.get());
            }
            if (st->type == ASTNodeType::DO_WHILE_STMT) {
                auto* dw = static_cast<const DoWhileStmt*>(st);
                return pureS(dw->body.get()) && pureE(dw->condition.get());
            }
            if (st->type == ASTNodeType::SWITCH_STMT) {
                auto* sw = static_cast<const SwitchStmt*>(st);
                if (!pureE(sw->condition.get())) return false;
                for (auto& c : sw->cases)
                    for (auto& bs : c.body)
                        if (!pureS(bs.get())) return false;
                return true;
            }
            // break, continue, freeze, invalidate, prefetch, assume: pure
            return true;
        };

        bool pure = true;
        for (auto& stmt : fn->body->statements) {
            if (!pureS(stmt.get())) { pure = false; break; }
        }
        visiting.erase(fn->name);
        return pure;
    };

    // Fixed-point: keep marking until stable.
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& fn : program->functions) {
            if (pureFunctions_.count(fn->name)) continue;
            std::unordered_set<std::string> vis;
            if (isPureBody(fn.get(), vis)) {
                markPure(fn->name);
                changed = true;
            }
        }
    }

    // ── Phase 4: pre-evaluate zero-arg pure functions ─────────────────────
    for (auto& fn : program->functions) {
        if (!fn->body || !fn->parameters.empty()) continue;
        if (!pureFunctions_.count(fn->name)) continue;
        auto result = executeFunction(fn.get(), {});
        if (result) {
            // Store result as global const for downstream fold.
            registerGlobalConst(fn->name, *result);
        }
    }

    // ── Phase 5: build call graph (complete pass) ─────────────────────────
    for (auto& fn : program->functions) {
        if (!fn->body) continue;
        graph_.nodes.push_back(fn->name);
        // Walk all expressions to collect call edges.
        std::function<void(const Expression*)> walkE = [&](const Expression* ex) {
            if (!ex) return;
            switch (ex->type) {
            case ASTNodeType::CALL_EXPR: {
                auto* call = static_cast<const CallExpr*>(ex);
                graph_.edges.push_back({fn->name, call->callee});
                for (auto& arg : call->arguments) walkE(arg.get());
                return;
            }
            case ASTNodeType::BINARY_EXPR: {
                auto* b = static_cast<const BinaryExpr*>(ex);
                walkE(b->left.get()); walkE(b->right.get());
                return;
            }
            case ASTNodeType::UNARY_EXPR: {
                auto* u = static_cast<const UnaryExpr*>(ex);
                walkE(u->operand.get());
                return;
            }
            case ASTNodeType::PREFIX_EXPR: {
                auto* p = static_cast<const PrefixExpr*>(ex);
                walkE(p->operand.get());
                return;
            }
            case ASTNodeType::POSTFIX_EXPR: {
                auto* p = static_cast<const PostfixExpr*>(ex);
                walkE(p->operand.get());
                return;
            }
            case ASTNodeType::TERNARY_EXPR: {
                auto* t = static_cast<const TernaryExpr*>(ex);
                walkE(t->condition.get()); walkE(t->thenExpr.get()); walkE(t->elseExpr.get());
                return;
            }
            case ASTNodeType::INDEX_EXPR: {
                auto* idx = static_cast<const IndexExpr*>(ex);
                walkE(idx->array.get()); walkE(idx->index.get());
                return;
            }
            case ASTNodeType::INDEX_ASSIGN_EXPR: {
                auto* ia = static_cast<const IndexAssignExpr*>(ex);
                walkE(ia->array.get()); walkE(ia->index.get()); walkE(ia->value.get());
                return;
            }
            case ASTNodeType::ARRAY_EXPR: {
                auto* ae = static_cast<const ArrayExpr*>(ex);
                for (auto& el : ae->elements) walkE(el.get());
                return;
            }
            case ASTNodeType::PIPE_EXPR: {
                auto* pe = static_cast<const PipeExpr*>(ex);
                walkE(pe->left.get());
                graph_.edges.push_back({fn->name, pe->functionName});
                return;
            }
            case ASTNodeType::ASSIGN_EXPR: {
                auto* ae = static_cast<const AssignExpr*>(ex);
                walkE(ae->value.get());
                return;
            }
            case ASTNodeType::SPREAD_EXPR: {
                auto* se = static_cast<const SpreadExpr*>(ex);
                walkE(se->operand.get());
                return;
            }
            case ASTNodeType::RANGE_ANNOT_EXPR: {
                auto* ra = static_cast<const RangeAnnotExpr*>(ex);
                walkE(ra->inner.get());
                return;
            }
            default:
                return;
            }
        };
        // Walk all statements to find call expressions.
        std::function<void(const Statement*)> walkS = [&](const Statement* st) {
            if (!st) return;
            switch (st->type) {
            case ASTNodeType::EXPR_STMT: {
                auto* es = static_cast<const ExprStmt*>(st);
                walkE(es->expression.get());
                return;
            }
            case ASTNodeType::RETURN_STMT: {
                auto* r = static_cast<const ReturnStmt*>(st);
                walkE(r->value.get());
                return;
            }
            case ASTNodeType::VAR_DECL: {
                auto* d = static_cast<const VarDecl*>(st);
                walkE(d->initializer.get());
                return;
            }
            case ASTNodeType::MOVE_DECL: {
                auto* md = static_cast<const MoveDecl*>(st);
                walkE(md->initializer.get());
                return;
            }
            case ASTNodeType::BLOCK: {
                auto* blk = static_cast<const BlockStmt*>(st);
                for (auto& s2 : blk->statements) walkS(s2.get());
                return;
            }
            case ASTNodeType::IF_STMT: {
                auto* ifs = static_cast<const IfStmt*>(st);
                walkE(ifs->condition.get());
                walkS(ifs->thenBranch.get());
                walkS(ifs->elseBranch.get());
                return;
            }
            case ASTNodeType::FOR_STMT: {
                auto* fs = static_cast<const ForStmt*>(st);
                walkE(fs->start.get()); walkE(fs->end.get());
                if (fs->step) walkE(fs->step.get());
                walkS(fs->body.get());
                return;
            }
            case ASTNodeType::FOR_EACH_STMT: {
                auto* fes = static_cast<const ForEachStmt*>(st);
                walkE(fes->collection.get());
                walkS(fes->body.get());
                return;
            }
            case ASTNodeType::WHILE_STMT: {
                auto* ws = static_cast<const WhileStmt*>(st);
                walkE(ws->condition.get());
                walkS(ws->body.get());
                return;
            }
            case ASTNodeType::DO_WHILE_STMT: {
                auto* dw = static_cast<const DoWhileStmt*>(st);
                walkS(dw->body.get());
                walkE(dw->condition.get());
                return;
            }
            case ASTNodeType::SWITCH_STMT: {
                auto* sw = static_cast<const SwitchStmt*>(st);
                walkE(sw->condition.get());
                for (auto& c : sw->cases) {
                    if (c.value) walkE(c.value.get());
                    for (auto& v : c.values) walkE(v.get());
                    for (auto& bs : c.body) walkS(bs.get());
                }
                return;
            }
            case ASTNodeType::PIPELINE_STMT: {
                auto* ps = static_cast<const PipelineStmt*>(st);
                if (ps->count) walkE(ps->count.get());
                for (auto& stage : ps->stages) walkS(stage.body.get());
                return;
            }
            default:
                return;
            }
        };
        for (auto& stmt : fn->body->statements) walkS(stmt.get());
    }

    // ── Phase 6: deduplicate graph nodes ──────────────────────────────────
    std::sort(graph_.nodes.begin(), graph_.nodes.end());
    graph_.nodes.erase(std::unique(graph_.nodes.begin(), graph_.nodes.end()), graph_.nodes.end());

    // ── Phase 7: uniform return value detection ────────────────────────────
    for (auto& fn : program->functions) {
        if (!fn->body) continue;
        if (!pureFunctions_.count(fn->name)) continue;
        if (uniformReturnValues_.count(fn->name)) continue; // zero-arg: already in globalConsts_
        if (fn->parameters.empty()) continue;

        // Build all-symbolic argument vector.
        std::vector<CTValue> symbolicArgs;
        symbolicArgs.reserve(fn->parameters.size());
        for (size_t i = 0; i < fn->parameters.size(); ++i)
            symbolicArgs.push_back(CTValue::symbolic());

        auto result = executeFunction(fn.get(), symbolicArgs);
        if (result && result->isConcrete()) {
            uniformReturnValues_[fn->name] = *result;
            ++stats_.uniformReturnFunctionsFound;
        }
    }

    // ── Phase 8: dead function detection ──────────────────────────────────
    {
        // Build callee → callers reverse index AND forward callees index.
        std::unordered_map<std::string, std::vector<std::string>> callees;
        std::unordered_set<std::string> allCallees;
        for (auto& edge : graph_.edges) {
            callees[edge.callerName].push_back(edge.calleeName);
            allCallees.insert(edge.calleeName);
        }

        // Seed: "main" + any function that is NEVER a callee (could be external entry).
        std::unordered_set<std::string> alive;
        std::vector<std::string> worklist;
        bool hasSeed = false;
        for (auto& fn : program->functions) {
            if (!fn->body) continue;
            // "main" is always an entry point.
            const bool isMain = fn->name == "main";
            // A function with no callers in the program graph could be called
            // from outside (external linkage) — treat as live.
            const bool noCaller = !allCallees.count(fn->name);
            if (isMain || noCaller) {
                if (alive.insert(fn->name).second) {
                    worklist.push_back(fn->name);
                    hasSeed = true;
                }
            }
        }

        if (hasSeed) {
            // Forward BFS.
            while (!worklist.empty()) {
                std::string curr = std::move(worklist.back());
                worklist.pop_back();
                auto it = callees.find(curr);
                if (it == callees.end()) continue;
                for (auto& callee : it->second) {
                    if (alive.insert(callee).second)
                        worklist.push_back(callee);
                }
            }

            // Mark functions that are registered but not alive.
            for (auto& fn : program->functions) {
                if (!fn->body) continue;
                if (!alive.count(fn->name)) {
                    deadFunctions_.insert(fn->name);
                    ++stats_.deadFunctionsDetected;
                }
            }
        }
    }

    // ── Phase 9: abstract interpretation (Q1–Q5) ──────────────────────────
    runAbstractInterpretation(program);

    // ── Phase 10: inter-call-site pre-evaluation ──────────────────────────
    {
        // Helper: try to resolve a single expression to a concrete CTValue.
        // Handles literals, global/enum constants, unary negation on any
        // resolvable sub-expression, and binary arithmetic on two resolvable
        // sub-expressions.  Recursive, so -1, 2 + 3 * 4, etc. all work.
        std::function<std::optional<CTValue>(const Expression*)> tryResolveLiteral;
        tryResolveLiteral = [&](const Expression* ex) -> std::optional<CTValue> {
            if (!ex) return std::nullopt;
            if (ex->type == ASTNodeType::LITERAL_EXPR) {
                auto* lit = static_cast<const LiteralExpr*>(ex);
                if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
                    return CTValue::fromI64(static_cast<int64_t>(lit->intValue));
                if (lit->literalType == LiteralExpr::LiteralType::FLOAT)
                    return CTValue::fromF64(lit->floatValue);
                return CTValue::fromString(lit->stringValue);
            }
            if (ex->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* id = static_cast<const IdentifierExpr*>(ex);
                auto git = globalConsts_.find(id->name);
                if (git != globalConsts_.end() && git->second.isConcrete())
                    return git->second;
                auto eit = enumConsts_.find(id->name);
                if (eit != enumConsts_.end())
                    return CTValue::fromI64(eit->second);
            }
            if (ex->type == ASTNodeType::SCOPE_RESOLUTION_EXPR) {
                auto* sr = static_cast<const ScopeResolutionExpr*>(ex);
                const std::string flat = sr->scopeName + "_" + sr->memberName;
                auto eit = enumConsts_.find(flat);
                if (eit != enumConsts_.end()) return CTValue::fromI64(eit->second);
            }
            // Unary negation / logical-not on a resolvable sub-expression.
            if (ex->type == ASTNodeType::UNARY_EXPR) {
                auto* un = static_cast<const UnaryExpr*>(ex);
                auto inner = tryResolveLiteral(un->operand.get());
                if (inner) {
                    CTValue v = evalUnaryOp(un->op, *inner);
                    if (v.isConcrete()) return v;
                }
                return std::nullopt;
            }
            // Binary arithmetic on two resolvable sub-expressions (e.g. 2 + 3 * 4).
            if (ex->type == ASTNodeType::BINARY_EXPR) {
                auto* bin = static_cast<const BinaryExpr*>(ex);
                auto l = tryResolveLiteral(bin->left.get());
                auto r = tryResolveLiteral(bin->right.get());
                if (l && r) {
                    CTValue v = evalBinaryOp(bin->op, *l, *r);
                    if (v.isConcrete()) return v;
                }
                return std::nullopt;
            }
            return std::nullopt;
        };

        std::function<void(const Expression*)> walkCallSites = [&](const Expression* ex) {
            if (!ex) return;
            if (ex->type == ASTNodeType::CALL_EXPR) {
                auto* call = static_cast<const CallExpr*>(ex);
                // Only handle calls to registered pure user functions.
                if (pureFunctions_.count(call->callee) && functions_.count(call->callee)) {
                    std::vector<CTValue> resolvedArgs;
                    resolvedArgs.reserve(call->arguments.size());
                    bool allConcrete = true;
                    for (auto& arg : call->arguments) {
                        auto v = tryResolveLiteral(arg.get());
                        if (!v) { allConcrete = false; break; }
                        resolvedArgs.push_back(*v);
                    }
                    if (allConcrete) {
                        // Evaluate with concrete args; result will be stored in memoCache_.
                        auto result = executeFunction(call->callee, resolvedArgs);
                        if (result) ++stats_.callSitesFolded;
                    }
                }
                for (auto& arg : call->arguments) walkCallSites(arg.get());
                return;
            }
            // Recurse into sub-expressions.
            switch (ex->type) {
            case ASTNodeType::BINARY_EXPR: {
                auto* b = static_cast<const BinaryExpr*>(ex);
                walkCallSites(b->left.get()); walkCallSites(b->right.get()); break; }
            case ASTNodeType::UNARY_EXPR: {
                auto* u = static_cast<const UnaryExpr*>(ex);
                walkCallSites(u->operand.get()); break; }
            case ASTNodeType::TERNARY_EXPR: {
                auto* t = static_cast<const TernaryExpr*>(ex);
                walkCallSites(t->condition.get()); walkCallSites(t->thenExpr.get());
                walkCallSites(t->elseExpr.get()); break; }
            case ASTNodeType::INDEX_EXPR: {
                auto* i = static_cast<const IndexExpr*>(ex);
                walkCallSites(i->array.get()); walkCallSites(i->index.get()); break; }
            case ASTNodeType::ARRAY_EXPR: {
                auto* a = static_cast<const ArrayExpr*>(ex);
                for (auto& el : a->elements) walkCallSites(el.get()); break; }
            case ASTNodeType::ASSIGN_EXPR: {
                auto* a = static_cast<const AssignExpr*>(ex);
                walkCallSites(a->value.get()); break; }
            default: break;
            }
        };

        std::function<void(const Statement*)> walkStmtCallSites = [&](const Statement* st) {
            if (!st) return;
            switch (st->type) {
            case ASTNodeType::EXPR_STMT:
                walkCallSites(static_cast<const ExprStmt*>(st)->expression.get()); break;
            case ASTNodeType::RETURN_STMT:
                walkCallSites(static_cast<const ReturnStmt*>(st)->value.get()); break;
            case ASTNodeType::VAR_DECL:
                walkCallSites(static_cast<const VarDecl*>(st)->initializer.get()); break;
            case ASTNodeType::BLOCK:
                for (auto& s2 : static_cast<const BlockStmt*>(st)->statements)
                    walkStmtCallSites(s2.get()); break;
            case ASTNodeType::IF_STMT: {
                auto* ifs = static_cast<const IfStmt*>(st);
                walkCallSites(ifs->condition.get());
                walkStmtCallSites(ifs->thenBranch.get());
                walkStmtCallSites(ifs->elseBranch.get()); break; }
            case ASTNodeType::FOR_STMT: {
                auto* fs = static_cast<const ForStmt*>(st);
                walkCallSites(fs->start.get()); walkCallSites(fs->end.get());
                if (fs->step) walkCallSites(fs->step.get());
                walkStmtCallSites(fs->body.get()); break; }
            case ASTNodeType::WHILE_STMT: {
                auto* ws = static_cast<const WhileStmt*>(st);
                walkCallSites(ws->condition.get());
                walkStmtCallSites(ws->body.get()); break; }
            case ASTNodeType::FOR_EACH_STMT: {
                auto* fes = static_cast<const ForEachStmt*>(st);
                walkCallSites(fes->collection.get());
                walkStmtCallSites(fes->body.get()); break; }
            case ASTNodeType::SWITCH_STMT: {
                auto* sw = static_cast<const SwitchStmt*>(st);
                walkCallSites(sw->condition.get());
                for (auto& c : sw->cases) {
                    if (c.value) walkCallSites(c.value.get());
                    for (auto& bs : c.body) walkStmtCallSites(bs.get()); }
                break; }
            default: break;
            }
        };

        for (auto& fn : program->functions) {
            if (!fn->body) continue;
            for (auto& stmt : fn->body->statements)
                walkStmtCallSites(stmt.get());
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════

namespace {

// Overflow-safe int64 helpers used by the transfer functions.
inline bool safeAddI64(int64_t a, int64_t b, int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_add_overflow(a, b, &out);
#else
    if (b > 0 && a > std::numeric_limits<int64_t>::max() - b) return false;
    if (b < 0 && a < std::numeric_limits<int64_t>::min() - b) return false;
    out = a + b;
    return true;
#endif
}
inline bool safeMulI64(int64_t a, int64_t b, int64_t& out) {
#if defined(__GNUC__) || defined(__clang__)
    return !__builtin_mul_overflow(a, b, &out);
#else
    if (a == 0 || b == 0) { out = 0; return true; }
    if (a > 0 && b > 0 && a > std::numeric_limits<int64_t>::max() / b) return false;
    if (a < 0 && b < 0 && (-a) > std::numeric_limits<int64_t>::max() / (-b)) return false;
    if (a > 0 && b < 0 && b < std::numeric_limits<int64_t>::min() / a) return false;
    if (a < 0 && b > 0 && a < std::numeric_limits<int64_t>::min() / b) return false;
    out = a * b;
    return true;
#endif
}

// All 4 corner products for multiplication on intervals.
inline CTInterval mulInterval(int64_t la, int64_t ha, int64_t lb, int64_t hb) {
    int64_t p[4] = {};
    bool ok = safeMulI64(la, lb, p[0]) && safeMulI64(la, hb, p[1]) &&
              safeMulI64(ha, lb, p[2]) && safeMulI64(ha, hb, p[3]);
    if (!ok) return CTInterval::top();
    return CTInterval::range(*std::min_element(p, p+4),
                             *std::max_element(p, p+4));
}

} // anonymous namespace

CTInterval CTInterval::opAdd(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return bottom();
    if (isTop() || o.isTop()) return top();
    int64_t newLo, newHi;
    if (!safeAddI64(lo, o.lo, newLo) || !safeAddI64(hi, o.hi, newHi)) return top();
    return range(newLo, newHi);
}

CTInterval CTInterval::opSub(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return bottom();
    if (isTop() || o.isTop()) return top();
    // [la,ha] - [lb,hb] = [la-hb, ha-lb]
    int64_t newLo, newHi;
    if (!safeAddI64(lo,  -o.hi, newLo) || !safeAddI64(hi, -o.lo, newHi)) return top();
    return range(newLo, newHi);
}

CTInterval CTInterval::opMul(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return bottom();
    if (isTop() || o.isTop()) return top();
    return mulInterval(lo, hi, o.lo, o.hi);
}

CTInterval CTInterval::opDiv(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return bottom();
    if (isTop() || o.isTop()) return top();
    // Division by zero is undefined — return TOP conservatively.
    if (o.includesZero()) return top();
    // Avoid MIN / -1 overflow.
    if (lo == std::numeric_limits<int64_t>::min() && o.includes(-1)) return top();
    // Divide by extreme divisors to find the tightest bounds.
    const int64_t divs[4] = {lo / o.lo, lo / o.hi, hi / o.lo, hi / o.hi};
    return range(*std::min_element(divs, divs+4),
                 *std::max_element(divs, divs+4));
}

CTInterval CTInterval::opMod(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return bottom();
    if (o.includesZero()) return top();
    // For signed modulo: result lies in [-(|divisor|-1), |divisor|-1]
    // We use the abs of the divisor's maximum absolute magnitude.
    const int64_t absDivisorMax = std::max(std::abs(o.lo), std::abs(o.hi));
    if (absDivisorMax <= 0) return top();
    return range(-(absDivisorMax - 1), absDivisorMax - 1);
}

CTInterval CTInterval::opShl(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return bottom();
    if (isTop() || o.isTop()) return top();
    // Only safe when shift amount is in [0, 62] and base is non-negative.
    if (o.lo < 0 || o.hi > 62) return top();
    if (lo < 0) return top();  // signed shift of negative: UB in C++
    int64_t newLo, newHi;
    // lo << o.lo and hi << o.hi — if either overflows, return TOP.
    if (!safeMulI64(lo, int64_t(1) << o.lo, newLo)) return top();
    if (!safeMulI64(hi, int64_t(1) << o.hi, newHi)) return top();
    return range(newLo, newHi);
}

CTInterval CTInterval::opShr(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return bottom();
    if (isTop() || o.isTop()) return top();
    if (o.lo < 0 || o.hi > 63) return top();
    // For positive values: [lo >> o.hi, hi >> o.lo] (right shift reduces magnitude)
    if (lo >= 0) return range(lo >> o.hi, hi >> o.lo);
    // Negative values: arithmetic shift preserves sign; result is still negative
    return range(lo >> o.lo, hi >> o.hi);
}

CTInterval CTInterval::opBitAnd(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return bottom();
    if (isTop() || o.isTop()) return top();
    // Conservative: [0, min(ha, hb)] when both are non-negative
    if (lo >= 0 && o.lo >= 0)
        return range(0, std::min(hi, o.hi));
    return top();
}

CTInterval CTInterval::opBitOr(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return bottom();
    if (isTop() || o.isTop()) return top();
    // Conservative: [max(la,lb), ha|hb] when both non-negative
    if (lo >= 0 && o.lo >= 0) {
        // Upper bound: all bits set up to the highest bit of either hi
        int64_t upper = hi | o.hi;  // bitwise OR of upper bounds
        return range(std::max(lo, o.lo), upper);
    }
    return top();
}

CTInterval CTInterval::opBitXor(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return bottom();
    if (isTop() || o.isTop()) return top();
    if (lo >= 0 && o.lo >= 0)
        return range(0, hi | o.hi);
    return top();
}

CTInterval CTInterval::opNeg() const noexcept {
    if (isBottom() || isTop()) return *this;
    // Careful: -INT64_MIN overflows
    if (lo == std::numeric_limits<int64_t>::min()) return top();
    return range(-hi, -lo);
}

CTInterval CTInterval::opAbs() const noexcept {
    if (isBottom() || isTop()) return *this;
    if (lo == std::numeric_limits<int64_t>::min()) return top();
    if (lo >= 0) return *this;
    if (hi < 0) return range(-hi, -lo);
    return range(0, std::max(-lo, hi));
}

CTInterval::CmpResult CTInterval::cmpLT(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return CmpResult::UNKNOWN;
    if (!isTop() && !o.isTop()) {
        if (hi < o.lo) return CmpResult::ALWAYS_TRUE;   // every a < every b
        if (lo >= o.hi) return CmpResult::ALWAYS_FALSE; // every a >= every b
    }
    return CmpResult::UNKNOWN;
}
CTInterval::CmpResult CTInterval::cmpLE(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return CmpResult::UNKNOWN;
    if (!isTop() && !o.isTop()) {
        if (hi <= o.lo) return CmpResult::ALWAYS_TRUE;
        if (lo > o.hi) return CmpResult::ALWAYS_FALSE;
    }
    return CmpResult::UNKNOWN;
}
CTInterval::CmpResult CTInterval::cmpGT(const CTInterval& o) const noexcept { return o.cmpLT(*this); }
CTInterval::CmpResult CTInterval::cmpGE(const CTInterval& o) const noexcept { return o.cmpLE(*this); }
CTInterval::CmpResult CTInterval::cmpEQ(const CTInterval& o) const noexcept {
    if (isBottom() || o.isBottom()) return CmpResult::UNKNOWN;
    if (!isTop() && !o.isTop()) {
        if (isConcrete() && o.isConcrete() && lo == o.lo) return CmpResult::ALWAYS_TRUE;
        // No overlap → always false
        if (hi < o.lo || lo > o.hi) return CmpResult::ALWAYS_FALSE;
    }
    return CmpResult::UNKNOWN;
}
CTInterval::CmpResult CTInterval::cmpNE(const CTInterval& o) const noexcept {
    auto eq = cmpEQ(o);
    if (eq == CmpResult::ALWAYS_TRUE)  return CmpResult::ALWAYS_FALSE;
    if (eq == CmpResult::ALWAYS_FALSE) return CmpResult::ALWAYS_TRUE;
    return CmpResult::UNKNOWN;
}

// ── CTEngine::getExitRange ────────────────────────────────────────────────────
CTInterval CTEngine::getExitRange(const std::string& fnName,
                                  const std::string& varName) const noexcept {
    auto fi = analysisExitEnvs_.find(fnName);
    if (fi == analysisExitEnvs_.end()) return CTInterval::top();
    auto vi = fi->second.find(varName);
    if (vi == fi->second.end()) return CTInterval::top();
    return vi->second;
}

// ═══════════════════════════════════════════════════════════════════════════════

// Helper: look up a variable's abstract value in env, defaulting to TOP.
static CTInterval envGet(const CTAbstractEnv& env, const std::string& name) {
    auto it = env.find(name);
    return (it != env.end()) ? it->second : CTInterval::top();
}

CTInterval CTAbstractInterpreter::analyzeExpr(const Expression* e,
                                               const CTAbstractEnv& env,
                                               CTAnalysisResult& result) {
    if (!e) return CTInterval::top();

    switch (e->type) {
    // ── Literals ──────────────────────────────────────────────────────────
    case ASTNodeType::LITERAL_EXPR: {
        auto* lit = static_cast<const LiteralExpr*>(e);
        if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
            return CTInterval::exact(lit->intValue);
        if (lit->literalType == LiteralExpr::LiteralType::FLOAT)
            return CTInterval::top(); // float domain not tracked
        return CTInterval::top();    // strings
    }

    // ── Variable / global constant lookup ────────────────────────────────
    case ASTNodeType::IDENTIFIER_EXPR: {
        auto* id = static_cast<const IdentifierExpr*>(e);
        // 1. Local variable: use the abstract environment.
        auto localIt = env.find(id->name);
        if (localIt != env.end()) return localIt->second;
        // 2. Global constant: if it has a known integer value, convert it.
        auto gIt = globals_.find(id->name);
        if (gIt != globals_.end() && gIt->second.isInt())
            return CTInterval::exact(gIt->second.asI64());
        // 3. Enum constant.
        auto eIt = enums_.find(id->name);
        if (eIt != enums_.end()) return CTInterval::exact(eIt->second);
        return CTInterval::top();
    }

    // ── Scope resolution (enum member) ────────────────────────────────────
    case ASTNodeType::SCOPE_RESOLUTION_EXPR: {
        auto* sr = static_cast<const ScopeResolutionExpr*>(e);
        const std::string key = sr->scopeName + "::" + sr->memberName;
        auto eIt = enums_.find(key);
        if (eIt != enums_.end()) return CTInterval::exact(eIt->second);
        return CTInterval::top();
    }

    // ── Binary operations — semantic transfer functions ───────────────────
    case ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const BinaryExpr*>(e);
        CTInterval lhs = analyzeExpr(bin->left.get(),  env, result);
        CTInterval rhs = analyzeExpr(bin->right.get(), env, result);

        // Arithmetic operations: apply the appropriate transfer function.
        CTInterval res = CTInterval::top();
        const std::string& op = bin->op;

        if      (op == "+")  res = lhs.opAdd(rhs);
        else if (op == "-")  res = lhs.opSub(rhs);
        else if (op == "*")  res = lhs.opMul(rhs);
        else if (op == "/")  { res = lhs.opDiv(rhs);
                               if (!rhs.includesZero()) result.safeDivisions.insert(e); }
        else if (op == "%")  { res = lhs.opMod(rhs);
                               if (!rhs.includesZero()) result.safeDivisions.insert(e); }
        else if (op == "<<") res = lhs.opShl(rhs);
        else if (op == ">>") res = lhs.opShr(rhs);
        else if (op == "&")  res = lhs.opBitAnd(rhs);
        else if (op == "|")  res = lhs.opBitOr(rhs);
        else if (op == "^")  res = lhs.opBitXor(rhs);
        // Comparisons: result is 0 or 1
        else if (op == "<" || op == "<=" || op == ">" || op == ">=" ||
                 op == "==" || op == "!=" || op == "&&" || op == "||")
            res = CTInterval::range(0, 1);

        // Q5: arithmetic overflow safety — if operands' ranges fit within int64
        // and the result is concrete, the operation is provably safe.
        if (res.isRange() && !res.isTop()) {
            if ((op == "+" || op == "-" || op == "*") && !lhs.isTop() && !rhs.isTop())
                result.safeArithmetic.insert(e);
        }

        // Q4: Range-conditioned cheaper rewrites.
        if (op == "/" && !rhs.isTop() && rhs.isConcrete() && rhs.lo > 0 &&
            lhs.isNonNegative()) {
            int64_t divisor = rhs.lo;
            // Is divisor a power of 2?
            if (divisor > 0 && (divisor & (divisor - 1)) == 0)
                result.cheaperRewrites[e] = ">>";  // x/2^k → x>>k
        }
        if (op == "%" && !rhs.isTop() && rhs.isConcrete() && rhs.lo > 0 &&
            lhs.isNonNegative()) {
            int64_t divisor = rhs.lo;
            if (divisor > 0 && (divisor & (divisor - 1)) == 0)
                result.cheaperRewrites[e] = "&";   // x%2^k → x&(2^k-1)
        }
        if (op == "*" && !rhs.isTop() && rhs.isConcrete()) {
            int64_t factor = rhs.lo;
            // x * 2^k → x << k  (valid for all integer values — no precondition needed)
            if (factor > 0 && (factor & (factor - 1)) == 0)
                result.cheaperRewrites[e] = "<<";  // x*2^k → x<<k
        }

        return res;
    }

    // ── Unary operations ──────────────────────────────────────────────────
    case ASTNodeType::UNARY_EXPR: {
        auto* u = static_cast<const UnaryExpr*>(e);
        CTInterval operand = analyzeExpr(u->operand.get(), env, result);
        if (u->op == "-") return operand.opNeg();
        if (u->op == "!" || u->op == "not") return CTInterval::range(0, 1);
        if (u->op == "~") return CTInterval::top(); // bitwise NOT
        return CTInterval::top();
    }

    case ASTNodeType::PREFIX_EXPR:
    case ASTNodeType::POSTFIX_EXPR:
        // ++ / -- modify a variable: return TOP conservatively
        return CTInterval::top();

    // ── Ternary — join both branches ──────────────────────────────────────
    case ASTNodeType::TERNARY_EXPR: {
        auto* t = static_cast<const TernaryExpr*>(e);
        CTInterval cond = analyzeExpr(t->condition.get(), env, result);
        if (cond.isConcrete()) {
            if (cond.lo != 0) return analyzeExpr(t->thenExpr.get(), env, result);
            else              return analyzeExpr(t->elseExpr.get(), env, result);
        }
        CTInterval thenI = analyzeExpr(t->thenExpr.get(), env, result);
        CTInterval elseI = analyzeExpr(t->elseExpr.get(), env, result);
        return thenI.join(elseI);
    }

    // ── Array index — Q5 bounds check ────────────────────────────────────
    case ASTNodeType::INDEX_EXPR: {
        auto* idx = static_cast<const IndexExpr*>(e);
        CTInterval indexRange = analyzeExpr(idx->index.get(), env, result);
        // Lookup the array's known length.
        if (idx->array->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto* arrId = static_cast<const IdentifierExpr*>(idx->array.get());
            int64_t len = getArrayLength(arrId->name, env);
            if (len > 0 && indexRange.isRange() && !indexRange.isTop()) {
                if (indexRange.lo >= 0 && indexRange.hi < len) {
                    // Proven: 0 ≤ index < len for every possible execution.
                    // This is a SEMANTIC PROOF, not a syntactic pattern.
                    result.safeArrayAccesses.insert(e);
                }
            }
        }
        return CTInterval::top(); // element value unknown
    }

    default:
        return CTInterval::top();
    }
}

// Helper for compound condition narrowing (used by narrowCondition).
namespace {
constexpr int kAINarrowDepthBudget = 6;
}

void CTAbstractInterpreter::narrowCondition(const Expression* cond,
                                             CTAbstractEnv& thenEnv,
                                             CTAbstractEnv& elseEnv) {
    if (!cond) return;

    // ── Compound dispatcher ────────────────────────────────────────────────
    std::function<void(const Expression*, CTAbstractEnv&, CTAbstractEnv&, int)> walk;
    walk = [&](const Expression* c, CTAbstractEnv& thenE, CTAbstractEnv& elseE, int depth) {
        if (!c || depth > kAINarrowDepthBudget) return;

        // !A: swap then/else of A.
        if (c->type == ASTNodeType::UNARY_EXPR) {
            auto* un = static_cast<const UnaryExpr*>(c);
            if (un->op == "!") {
                walk(un->operand.get(), elseE, thenE, depth + 1);
            }
            return;
        }

        if (c->type != ASTNodeType::BINARY_EXPR) return;
        auto* bin = static_cast<const BinaryExpr*>(c);
        const std::string& op = bin->op;

        // A && B: both narrowings on then; nothing on else.
        if (op == "&&") {
            CTAbstractEnv dummyElse = elseE;
            walk(bin->left.get(),  thenE, dummyElse, depth + 1);
            walk(bin->right.get(), thenE, dummyElse, depth + 1);
            return;
        }
        // A || B: both narrowings on else; nothing on then.
        if (op == "||") {
            CTAbstractEnv dummyThen = thenE;
            walk(bin->left.get(),  dummyThen, elseE, depth + 1);
            walk(bin->right.get(), dummyThen, elseE, depth + 1);
            return;
        }

        // Atomic comparison (var ∘ literal, literal ∘ var, var ∘ var).
        auto narrowAtomic = [&](const Expression* varExpr, const CTInterval& bound,
                                 bool varOnLeft) {
            if (!varExpr || varExpr->type != ASTNodeType::IDENTIFIER_EXPR) return;
            auto* id = static_cast<const IdentifierExpr*>(varExpr);
            if (!bound.isConcrete()) return;
            const int64_t B = bound.lo;
            CTInterval cur = envGet(thenE, id->name);
            CTInterval newThen = cur, newElse = cur;
            if ((varOnLeft  && op == "<")  || (!varOnLeft && op == ">")) {
                newThen = cur.narrowLT(B); newElse = cur.narrowGE(B);
            } else if ((varOnLeft  && op == "<=") || (!varOnLeft && op == ">=")) {
                newThen = cur.narrowLE(B); newElse = cur.narrowGT(B);
            } else if ((varOnLeft  && op == ">")  || (!varOnLeft && op == "<")) {
                newThen = cur.narrowGT(B); newElse = cur.narrowLE(B);
            } else if ((varOnLeft  && op == ">=") || (!varOnLeft && op == "<=")) {
                newThen = cur.narrowGE(B); newElse = cur.narrowLT(B);
            } else if (op == "==") {
                newThen = cur.narrowEQ(B); newElse = cur.narrowNE(B);
            } else if (op == "!=") {
                newThen = cur.narrowNE(B); newElse = cur.narrowEQ(B);
            } else {
                return;
            }
            // Compose with anything previously deduced for this variable in this
            // pass via intersection (compound conditions stack tighter bounds).
            auto tIt = thenE.find(id->name);
            thenE[id->name] = (tIt == thenE.end()) ? newThen
                                                   : tIt->second.intersect(newThen);
            auto eIt = elseE.find(id->name);
            elseE[id->name] = (eIt == elseE.end()) ? newElse
                                                   : eIt->second.intersect(newElse);
        };

        CTAnalysisResult tmp;
        CTInterval lhsI = analyzeExpr(bin->left.get(),  thenE, tmp);
        CTInterval rhsI = analyzeExpr(bin->right.get(), thenE, tmp);
        narrowAtomic(bin->left.get(),  rhsI, /*varOnLeft=*/true);
        narrowAtomic(bin->right.get(), lhsI, /*varOnLeft=*/false);
    };

    walk(cond, thenEnv, elseEnv, 0);
}

CTAbstractEnv CTAbstractInterpreter::joinEnvs(const CTAbstractEnv& a,
                                               const CTAbstractEnv& b) {
    CTAbstractEnv result = a;
    for (auto& [name, valB] : b) {
        auto it = result.find(name);
        if (it == result.end())
            result[name] = valB;
        else
            it->second = it->second.join(valB);
    }
    return result;
}

int64_t CTAbstractInterpreter::getArrayLength(const std::string& varName,
                                               const CTAbstractEnv& /*env*/) const {
    auto it = arrayLengths_.find(varName);
    return (it != arrayLengths_.end()) ? it->second : -1;
}

bool CTAbstractInterpreter::analyzeStmt(const Statement* s, CTAbstractEnv& env,
                                         CTAnalysisResult& result) {
    if (!s) return true;

    switch (s->type) {

    // ── Variable declaration ───────────────────────────────────────────────
    case ASTNodeType::VAR_DECL: {
        auto* d = static_cast<const VarDecl*>(s);
        CTInterval iv = analyzeExpr(d->initializer.get(), env, result);
        env[d->name] = iv;
        // Track array lengths from array literal declarations.
        if (d->initializer &&
            d->initializer->type == ASTNodeType::ARRAY_EXPR) {
            auto* ae = static_cast<const ArrayExpr*>(d->initializer.get());
            arrayLengths_[d->name] = static_cast<int64_t>(ae->elements.size());
        }
        return true;
    }

    case ASTNodeType::MOVE_DECL: {
        auto* md = static_cast<const MoveDecl*>(s);
        env[md->name] = analyzeExpr(md->initializer.get(), env, result);
        return true;
    }

    // ── Assignment ────────────────────────────────────────────────────────
    case ASTNodeType::EXPR_STMT: {
        auto* es = static_cast<const ExprStmt*>(s);
        if (es->expression) {
            if (es->expression->type == ASTNodeType::ASSIGN_EXPR) {
                auto* ae = static_cast<const AssignExpr*>(es->expression.get());
                env[ae->name] = analyzeExpr(ae->value.get(), env, result);
            } else {
                analyzeExpr(es->expression.get(), env, result);
            }
        }
        return true;
    }

    // ── Return ────────────────────────────────────────────────────────────
    case ASTNodeType::RETURN_STMT:
        // Rest of the block is unreachable after a return.
        return false;

    case ASTNodeType::BREAK_STMT:
    case ASTNodeType::CONTINUE_STMT:
        return false;

    // ── Block ─────────────────────────────────────────────────────────────
    case ASTNodeType::BLOCK: {
        auto* blk = static_cast<const BlockStmt*>(s);
        analyzeBlock(blk, env, result);
        return true;
    }

    // ── If statement (Q2: reachability) ───────────────────────────────────
    case ASTNodeType::IF_STMT: {
        auto* ifs = static_cast<const IfStmt*>(s);
        // Evaluate the condition's abstract value.
        CTInterval condRange = analyzeExpr(ifs->condition.get(), env, result);

        // Derive reachability from the condition's abstract value.
        // condAlwaysTrue:  condition range cannot contain 0 and is a concrete range.
        // condAlwaysFalse: condition is the single concrete value 0.
        const bool condAlwaysTrue  = condRange.isRange() &&
                                      !condRange.includesZero() && !condRange.isTop();
        const bool condAlwaysFalse = condRange.isConcrete() && condRange.lo == 0;

        if (condAlwaysFalse) {
            result.deadThenBranches.insert(s);
            if (ifs->elseBranch) analyzeStmt(ifs->elseBranch.get(), env, result);
            return true;
        }
        if (condAlwaysTrue) {
            result.deadElseBranches.insert(s);
            analyzeStmt(ifs->thenBranch.get(), env, result);
            return true;
        }

        // Branch is genuinely conditional — analyse both arms with narrowed envs.
        CTAbstractEnv thenEnv = env, elseEnv = env;
        narrowCondition(ifs->condition.get(), thenEnv, elseEnv);

        // After narrowing compound/atomic conditions, check whether any
        auto bottomFromTopVarOnly = [&](const CTAbstractEnv& narrowed) -> bool {
            for (auto& [name, iv] : narrowed) {
                if (!iv.isBottom()) continue;
                auto eIt = env.find(name);
                // Variable was TOP (or absent) before narrowing → the
                // contradiction must come from the condition itself.
                if (eIt == env.end() || eIt->second.isTop()) return true;
            }
            return false;
        };
        if (bottomFromTopVarOnly(thenEnv)) {
            result.deadThenBranches.insert(s);
            if (ifs->elseBranch) analyzeStmt(ifs->elseBranch.get(), elseEnv, result);
            env = elseEnv;
            return true;
        }
        if (bottomFromTopVarOnly(elseEnv)) {
            result.deadElseBranches.insert(s);
            analyzeStmt(ifs->thenBranch.get(), thenEnv, result);
            env = thenEnv;
            return true;
        }

        analyzeStmt(ifs->thenBranch.get(), thenEnv, result);
        if (ifs->elseBranch)
            analyzeStmt(ifs->elseBranch.get(), elseEnv, result);
        // Merge environments at the join point.
        env = joinEnvs(thenEnv, elseEnv);
        return true;
    }

    // ── For loop (Q1: induction variable range) ───────────────────────────
    case ASTNodeType::FOR_STMT: {
        auto* fs = static_cast<const ForStmt*>(s);
        CTInterval startI = analyzeExpr(fs->start.get(), env, result);
        CTInterval endI   = analyzeExpr(fs->end.get(),   env, result);
        CTInterval stepI  = fs->step
                            ? analyzeExpr(fs->step.get(), env, result)
                            : CTInterval::exact(1);

        // Compute induction variable range from start/end semantics.
        CTInterval ivRange;
        if (startI.isRange() && endI.isRange() && stepI.isRange() &&
            !startI.isTop() && !endI.isTop() && !stepI.isTop()) {
            if (stepI.lo > 0) {
                // Positive step: i iterates from start (inclusive) to end (exclusive).
                // Range: [startI.lo, endI.hi - 1]
                int64_t loIV = startI.lo;
                int64_t hiIV;
                safeAddI64(endI.hi, -1, hiIV);
                ivRange = CTInterval::range(loIV, hiIV);
            } else if (stepI.hi < 0) {
                // Negative step: i iterates from start (inclusive) down to end (exclusive).
                // Range: [endI.lo + 1, startI.hi]
                int64_t loIV;
                int64_t hiIV = startI.hi;
                safeAddI64(endI.lo, 1, loIV);
                ivRange = CTInterval::range(loIV, hiIV);
            } else {
                // Step includes zero or straddles positive/negative — conservatively TOP.
                ivRange = CTInterval::top();
            }
        } else {
            ivRange = CTInterval::top();
        }

        // Set the loop variable's range for the body analysis.
        CTAbstractEnv bodyEnv = env;
        bodyEnv[fs->iteratorVar] = ivRange;

        // Analyse loop body — variables modified inside may widen.
        CTAbstractEnv postBody = bodyEnv;
        analyzeStmt(fs->body.get(), postBody, result);

        // Widening: for any variable that changed in the loop body,
        // widen its range to prevent infinite iteration of the fixpoint.
        for (auto& [name, postVal] : postBody) {
            if (name == fs->iteratorVar) continue;  // IV handled separately
            auto preIt = bodyEnv.find(name);
            if (preIt == bodyEnv.end()) {
                env[name] = postVal;
            } else if (!(preIt->second.isRange() && postVal.isRange() &&
                         preIt->second.lo == postVal.lo &&
                         preIt->second.hi == postVal.hi)) {
                // Range changed: widen to prevent non-termination.
                env[name] = postVal.widen(preIt->second);
            }
        }
        // After the loop, the IV is out of scope (or equals end).
        env.erase(fs->iteratorVar);
        return true;
    }

    // ── While loop ────────────────────────────────────────────────────────
    case ASTNodeType::WHILE_STMT: {
        auto* ws = static_cast<const WhileStmt*>(s);
        // Conservative: all variables modified in the loop are widened to TOP.
        // A full fixpoint loop is too expensive for compile-time analysis.
        CTAbstractEnv loopEnv = env;
        analyzeStmt(ws->body.get(), loopEnv, result);
        for (auto& [name, val] : loopEnv) {
            auto preIt = env.find(name);
            if (preIt == env.end() || !(preIt->second.isRange() &&
                val.isRange() && preIt->second.lo == val.lo &&
                preIt->second.hi == val.hi)) {
                env[name] = CTInterval::top();
            }
        }
        return true;
    }

    case ASTNodeType::DO_WHILE_STMT: {
        auto* dw = static_cast<const DoWhileStmt*>(s);
        CTAbstractEnv loopEnv = env;
        analyzeStmt(dw->body.get(), loopEnv, result);
        for (auto& [name, val] : loopEnv) {
            auto preIt = env.find(name);
            if (preIt == env.end() || !(preIt->second.isRange() &&
                val.isRange() && preIt->second.lo == val.lo &&
                preIt->second.hi == val.hi)) {
                env[name] = CTInterval::top();
            }
        }
        return true;
    }

    // ── Switch statement ──────────────────────────────────────────────────
    case ASTNodeType::SWITCH_STMT: {
        auto* sw = static_cast<const SwitchStmt*>(s);
        analyzeExpr(sw->condition.get(), env, result);
        CTAbstractEnv merged = env;
        bool first = true;
        for (auto& c : sw->cases) {
            CTAbstractEnv caseEnv = env;
            for (auto& bs : c.body) analyzeStmt(bs.get(), caseEnv, result);
            if (first) { merged = caseEnv; first = false; }
            else merged = joinEnvs(merged, caseEnv);
        }
        if (!first) env = merged;
        return true;
    }

    default:
        return true;
    }
}

void CTAbstractInterpreter::analyzeBlock(const BlockStmt* block,
                                          CTAbstractEnv& env,
                                          CTAnalysisResult& result) {
    if (!block) return;
    for (auto& stmt : block->statements) {
        if (!analyzeStmt(stmt.get(), env, result)) break;
    }
}

CTAnalysisResult CTAbstractInterpreter::analyzeFunction(const FunctionDecl* fn) {
    CTAnalysisResult result;
    if (!fn || !fn->body) return result;

    // Initial environment: parameters start as TOP (any value).
    CTAbstractEnv env;
    for (auto& param : fn->parameters)
        env[param.name] = CTInterval::top();

    analyzeBlock(fn->body.get(), env, result);
    result.exitEnv = env;
    return result;
}

// ── CTEngine::runAbstractInterpretation ──────────────────────────────────────
void CTEngine::runAbstractInterpretation(const Program* program) {
    if (!program) return;

    CTAbstractInterpreter interp(*this, globalConsts_, enumConsts_);

    for (auto& fn : program->functions) {
        if (!fn->body) continue;

        CTAnalysisResult res = interp.analyzeFunction(fn.get());

        // Merge per-function results into the engine's global tables.

        // Q1: Store exit environment for cross-function queries.
        analysisExitEnvs_[fn->name] = res.exitEnv;

        // Q2: Dead branch annotations.
        for (auto* s : res.deadThenBranches) {
            analysisDeadThen_.insert(s);
            ++stats_.deadBranchesEliminated;
        }
        for (auto* s : res.deadElseBranches) {
            analysisDeadElse_.insert(s);
            ++stats_.deadBranchesEliminated;
        }

        // Q4: Range-conditioned cheaper rewrites.
        for (auto& [e, alt] : res.cheaperRewrites) {
            analysisCheaperRewrites_[e] = alt;
            ++stats_.cheaperRewritesFound;
        }

        // Q5: Safety proofs.
        for (auto* e : res.safeArrayAccesses) {
            analysisSafeArrayAccess_.insert(e);
            ++stats_.safeArrayAccesses;
        }
        for (auto* e : res.safeDivisions) {
            analysisSafeDiv_.insert(e);
            ++stats_.safeDivisions;
        }
        for (auto* e : res.safeArithmetic) {
            analysisSafeArith_.insert(e);
            ++stats_.safeArithmetic;
        }
    }
}

} // namespace omscript
