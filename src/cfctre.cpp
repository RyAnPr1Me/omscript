/// @file cfctre.cpp
/// @brief CF-CTRE — Cross-Function Compile-Time Reasoning Engine (implementation).
///
/// Implements deterministic, fuel-bounded, memoised compile-time evaluation
/// of pure OmScript functions operating on integer/float/string/array values.
///
/// Architecture:
///   CTValue  — immutable value wrapper (scalars inline, arrays by handle)
///   CTArray  — fixed-length array stored on the compile-time heap
///   CTHeap   — deterministic handle-based allocator for compile-time arrays
///   CTFrame  — per-call execution context (locals, heap ptr, control signals)
///   CTEngine — main evaluator: expression/statement walker, builtin table,
///              memoisation, pipeline-SIMD tiles, purity analysis, runPass

#include "cfctre.h"
#include "ast.h"
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
// CTValue implementation
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
// Uses a stack buffer + snprintf to avoid std::ostringstream overhead for
// the common scalar cases (integers, floats, booleans).
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
    }
    out.push_back('?');
}

std::string CTValue::memoHash() const {
    std::string result;
    result.reserve(24);                     // typical: "I:-1234567890" fits in SSO
    appendMemoHash(result);
    return result;
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
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════════════════
// CTArray implementation
// ═══════════════════════════════════════════════════════════════════════════

CTArray::CTArray(uint64_t n, const CTValue& fill)
    : len(n), data(static_cast<size_t>(n), fill) {}

// ═══════════════════════════════════════════════════════════════════════════
// CTHeap implementation
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
}

bool CTHeap::push(CTArrayHandle h, CTValue val) {
    auto it = arrays_.find(h);
    if (it == arrays_.end()) return false;
    CTArray& arr = it->second;
    arr.data.push_back(std::move(val));
    ++arr.len;
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
// CTEngine implementation
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
    // valid for the specific combination of concrete args that drove the
    // evaluation; other callers with different concrete values for the same
    // args might produce a different result.
    if (!hasSymbolic) {
        // Snapshot array results before storing in cache (so cache is stable).
        CTValue cached = result;
        if (cached.isArray() && cached.arr != CT_NULL_HANDLE)
            cached.arr = snapshotArray(cached.arr);
        memoCache_[key] = cached;
        // Record this function as CF-CTRE-foldable: it produced a concrete
        // result for at least one set of concrete arguments.  The codegen will
        // use this to apply InlineHint so LLVM preferentially inlines the
        // function, exposing the same folding opportunities at remaining runtime
        // call sites where arguments aren't compile-time constants.
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
// evalExpr — evaluate a single AST expression
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
        if (it != frame.locals.end()) return it->second;
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
        if (pfx->operand->type != ASTNodeType::IDENTIFIER_EXPR) return CTValue::uninit();
        auto* id  = static_cast<const IdentifierExpr*>(pfx->operand.get());
        auto  it  = frame.locals.find(id->name);
        if (it == frame.locals.end() || !it->second.isInt()) return CTValue::uninit();
        CTValue old = it->second;
        const int64_t delta = (pfx->op == "++") ? 1 : -1;
        it->second = CTValue::fromI64(it->second.asI64() + delta);
        return old;  // postfix returns old value
    }

    // ── Prefix ++ / -- ────────────────────────────────────────────────────
    case ASTNodeType::PREFIX_EXPR: {
        auto* pfx = static_cast<const PrefixExpr*>(e);
        if (pfx->op != "++" && pfx->op != "--") return CTValue::uninit();
        if (pfx->operand->type != ASTNodeType::IDENTIFIER_EXPR) return CTValue::uninit();
        auto* id  = static_cast<const IdentifierExpr*>(pfx->operand.get());
        auto  it  = frame.locals.find(id->name);
        if (it == frame.locals.end() || !it->second.isInt()) return CTValue::uninit();
        const int64_t delta = (pfx->op == "++") ? 1 : -1;
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

    default:
        return CTValue::uninit();
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// evalBinaryOp
// ═══════════════════════════════════════════════════════════════════════════

CTValue CTEngine::evalBinaryOp(const std::string& op, const CTValue& lhs, const CTValue& rhs) {
    if (!lhs.isKnown() || !rhs.isKnown()) return CTValue::uninit();

    // Partial-evaluation: propagate SYMBOLIC through binary ops.
    // For most ops, one SYMBOLIC operand makes the result SYMBOLIC.
    // Exceptions (absorbers that yield CONCRETE regardless of the other operand):
    //   x * 0  = 0,   0 * x  = 0
    //   x & 0  = 0,   0 & x  = 0
    //   x ** 0 = 1   (anything to the 0th power = 1)
    //   x && false = false  (already handled by short-circuit in evalExpr for "&&")
    //   x || true  = true   (already handled by short-circuit in evalExpr for "||")
    if (lhs.isSymbolic() || rhs.isSymbolic()) {
        if (op == "*") {
            if (rhs.isConcrete() && rhs.isInt() && rhs.asI64() == 0) return CTValue::fromI64(0);
            if (lhs.isConcrete() && lhs.isInt() && lhs.asI64() == 0) return CTValue::fromI64(0);
        }
        if (op == "&") {
            if (rhs.isConcrete() && rhs.isInt() && rhs.asI64() == 0) return CTValue::fromI64(0);
            if (lhs.isConcrete() && lhs.isInt() && lhs.asI64() == 0) return CTValue::fromI64(0);
        }
        if (op == "**") {
            if (rhs.isConcrete() && rhs.isInt() && rhs.asI64() == 0) return CTValue::fromI64(1);
        }
        // Bitwise OR with all-ones annihilates the other operand → -1.
        if (op == "|") {
            if (rhs.isConcrete() && rhs.isInt() && rhs.asI64() == -1) return CTValue::fromI64(-1);
            if (lhs.isConcrete() && lhs.isInt() && lhs.asI64() == -1) return CTValue::fromI64(-1);
        }
        // Modulo by ±1 is always 0, regardless of the dividend.
        if (op == "%" || op == "mod") {
            if (rhs.isConcrete() && rhs.isInt() &&
                (rhs.asI64() == 1 || rhs.asI64() == -1))
                return CTValue::fromI64(0);
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
// evalUnaryOp
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
// evalTypeCast — integer type-cast builtins
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
// evalCall — dispatch builtin or user function
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
        auto result = executeFunction(it->second, args);
        return result.value_or(CTValue::uninit());
    }

    return CTValue::uninit();
}

// ═══════════════════════════════════════════════════════════════════════════
// evalBuiltin — all pure built-in functions
// ═══════════════════════════════════════════════════════════════════════════

// Helper lambdas (defined at function scope to access args cleanly).
std::optional<CTValue> CTEngine::evalBuiltin(const std::string& name,
                                               const std::vector<CTValue>& args) {
    // ── Fast reject: skip the entire if-chain for unknown names ──────────
    static const std::unordered_set<std::string> kKnownBuiltins = {
        "len","str_len","abs","push","pop","min","max","sign","clamp","pow",
        "sqrt","gcd","lcm","log","log2","log10","exp2","is_even","is_odd","floor","ceil",
        "round","to_char","is_alpha","is_digit","to_string","number_to_string",
        "to_int","string_to_number","str_to_int","char_code","str_find",
        "str_index_of","str_contains","str_starts_with","startswith",
        "str_ends_with","endswith","str_substr","str_upper","str_lower",
        "str_repeat","str_trim","str_reverse","str_count","str_replace",
        "str_pad_left","str_pad_right","str_eq","str_concat","char_at",
        "is_power_of_2","popcount","clz","ctz","bitreverse","bswap",
        "rotate_left","rotate_right","saturating_add","saturating_sub",
        "str_chars","typeof","array_fill","range","range_step","array_concat",
        "array_slice","array_copy","sum","array_product","array_min",
        "array_max","array_last","array_contains","index_of","array_find",
        "is_upper","is_lower","is_space","is_alnum",
        "fast_add","fast_sub","fast_mul","fast_div",
        "precise_add","precise_sub","precise_mul","precise_div",
        "sin","cos","tan","asin","acos","atan","atan2","exp","cbrt","hypot",
        "fma","copysign","min_float","max_float",
        "reverse","sort","array_remove","array_insert",
        "array_any","array_every","array_count",
        "str_split","str_join",
        // int/uint/bool: type casts handled by evalTypeCast; iN/uN handled dynamically
        "int","uint","bool",
        // Program synthesis stdlib function (flat-name form of std::synthesize)
        "std__synthesize","std_synthesize",
        // Type-specific fast builtins
        "mulhi","mulhi_u","absdiff","fast_sqrt","is_nan","is_inf"
    };
    // Also accept iN/uN type-cast names (handled by evalTypeCast via evalCall).
    auto isIntWidthCastName = [](const std::string& nm) -> bool {
        if (nm.size() < 2 || (nm[0] != 'i' && nm[0] != 'u')) return false;
        int bw = 0;
        for (size_t j = 1; j < nm.size(); ++j) {
            if (!std::isdigit(static_cast<unsigned char>(nm[j]))) return false;
            bw = bw * 10 + (nm[j] - '0');
            if (bw > 256) return false;
        }
        return bw >= 1 && bw <= 256;
    };
    if (kKnownBuiltins.find(name) == kKnownBuiltins.end() && !isIntWidthCastName(name)) {
        // Accept __tw_* and __tf_* width/type-specific builtins for comptime eval.
        const bool isTW = name.size()>5 && name.substr(0,5)=="__tw_";
        const bool isTF = name.size()>5 && name.substr(0,5)=="__tf_";
        if (!isTW && !isTF)
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
    // C <cctype> functions require the argument to be in [0, UCHAR_MAX] or EOF.
    // OmScript passes character code points as int64_t; out-of-range → false.
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
    // Recognized as "std__synthesize" (scope-resolved flat name from the
    // `std::` namespace lowering) or "std_synthesize" (legacy flat name).
    // Signature: std::synthesize(examples, [ops], [max_depth], [cost_hint])
    //   examples  : int[][]  — each inner array = [in0,in1,...,inN-1,expected]
    //   ops       : string[] — allowed operators (optional)
    //   max_depth : int      — expression depth limit (optional, default 4)
    //   cost_hint : string   — "size"|"speed" (optional, default "speed")
    // Returns the synthesized expression's value on the first example's inputs.
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
//
// When a for-range loop body consists only of simple scalar accumulations
// (x += delta, x -= delta, x *= c, x ^= c, x &= c, x |= c, x = expr,
//  x++ / x--) whose operands are loop-invariant or linear in the loop
// variable, the engine computes the post-loop state in O(1) using
// closed-form arithmetic:
//
//   for (i in start...end) { x += d;    }  →  x += N * d
//   for (i in start...end) { x += i;    }  →  x += N*start + step*N*(N-1)/2
//   for (i in start...end) { x += a*i+b;}  →  x += a*Σi + b*N
//   for (i in start...end) { x *= c;    }  →  x *= c^N
//   for (i in start...end) { x ^= c;    }  →  x ^= (N%2 ? c : 0)
//   for (i in start...end) { x &= c;    }  →  x &= c   (N≥1)
//   for (i in start...end) { x |= c;    }  →  x |= c   (N≥1)
//   for (i in start...end) { x = c;     }  →  x = c    (last-write)
//   for (i in start...end) { x = a*i+b; }  →  x = a*(last_i) + b
//   for (i in start...end) { x++;       }  →  x += N
//
// Multiple independent effects on different variables are all applied.
// Any body statement that cannot be classified causes the analysis to
// abort and the caller falls back to direct iteration.
// ═══════════════════════════════════════════════════════════════════════════

// ── Linear expression descriptor ─────────────────────────────────────────
// Represents value = coefA * loopVar + coefB where coefA == 0 is invariant.
struct LRLinear {
    bool    valid{false};
    int64_t a{0};   // coefficient of the loop induction variable
    int64_t b{0};   // loop-invariant bias
};

// Classify an expression as a*iv + b, returning {false} if unrepresentable.
// modVars:  variables written in the loop body — treated as non-invariant.
// The loop variable itself is NOT in modVars; it is recognized by name (iv).
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

    default:
        return {};
    }
}

// ── Per-iteration effect on one scalar variable ───────────────────────────
struct LREffect {
    enum class Kind { ADD, MUL, XOR, AND, OR, SET, INCR, DECR };
    Kind        kind;
    std::string var;
    int64_t     a{0};   // coefficient of loop var (for ADD / SET)
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
// start, step define the induction variable sequence:
//   iv_k = start + k * step,  k in [0, N)
static void lrApplyEffects(
    CTFrame&                        frame,
    const std::vector<LREffect>&    effects,
    int64_t                         start,
    int64_t                         step,
    int64_t                         N)
{
    // Sum of the induction variable over all N iterations:
    //   Σiv = N*start + step * N*(N-1)/2
    // Use __int128 to avoid signed overflow during intermediate computation;
    // the final wrap-around cast to int64 (via uint64) matches OmScript's
    // wrapping arithmetic semantics.
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
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CTEngine::tryReasonForLoop
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
        // When the condition is symbolic, evaluate both branches independently
        // on frame forks.  If they agree (same return value, or same local
        // variable state), fold to the agreed result without needing to know
        // which branch would actually be taken at runtime.
        //
        // Safety notes:
        //   • Frame locals are deep-copied per branch; the heap pointer is
        //     shared.  Any array allocations made by abandoned branches leave
        //     dangling handles in the heap (no future reference → harmless
        //     memory waste, not a correctness issue).
        //   • fuel_ is saved/restored per branch and set to max(both) so
        //     we charge fairly for the more expensive branch.
        //   • We only fold Case 1 (both return non-array concrete values) or
        //     Case 2 (both fall through, locals agree or diverge → symbolic),
        //     not any state where break/continue signals differ.
        if (cond.isSymbolic() && fuel_ + 2 <= kMaxInstructions) {
            const int64_t savedFuel = fuel_;

            // Fork A: then-branch.
            CTFrame thenF = frame;
            thenF.hasReturned = false; thenF.didBreak = false; thenF.didContinue = false;
            bool thenOk = evalStmt(thenF, ifs->thenBranch.get());
            const int64_t fuelAfterThen = fuel_;

            // Fork B: else-branch (or identity if absent).
            CTFrame elseF = frame;
            elseF.hasReturned = false; elseF.didBreak = false; elseF.didContinue = false;
            fuel_ = savedFuel;
            bool elseOk = true;
            if (ifs->elseBranch)
                elseOk = evalStmt(elseF, ifs->elseBranch.get());
            const int64_t fuelAfterElse = fuel_;
            fuel_ = std::max(fuelAfterThen, fuelAfterElse);

            // Case 1: both branches return the same concrete non-array value.
            // The result is independent of the condition → constant fold.
            // Helper: branch returned a concrete scalar/string value we can merge.
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
            // Variables that agree across both branches keep their value;
            // variables that diverge are marked symbolic so downstream code
            // can still proceed (conservatively) without aborting.
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
        // Attempt O(1) closed-form analysis before falling back to iteration.
        // For bodies that contain only simple scalar accumulations this avoids
        // the O(N) fuel cost and enables evaluation of million-iteration loops.
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
// Pipeline SIMD tile execution
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

// ═══════════════════════════════════════════════════════════════════════════
// runPass — whole-program CF-CTRE analysis
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
    static const std::unordered_set<std::string> kBuiltinPure = {
        "abs","min","max","sign","clamp","pow","sqrt","floor","ceil","round",
        "log","log2","log10","exp2","sin","cos","tan","asin","acos","atan",
        "atan2","cbrt","hypot","fma","copysign","min_float","max_float",
        "gcd","lcm","is_even","is_odd","is_power_of_2",
        "popcount","clz","ctz","bswap","bitreverse","rotate_left","rotate_right",
        "saturating_add","saturating_sub",
        "to_int","to_float","to_string","number_to_string","string_to_number",
        "str_to_int","char_code","to_char","str_len","len","typeof",
        "char_at","str_eq","str_concat","str_find","str_index_of",
        "str_starts_with","str_ends_with","startswith","endswith",
        "str_upper","str_lower","str_trim","str_reverse","str_repeat",
        "str_substr","str_count","str_replace","str_chars","str_pad_left",
        "str_pad_right","is_alpha","is_digit",
        "array_fill","range","range_step","array_concat","array_slice",
        "array_copy","sum","array_product","array_min","array_max",
        "array_last","array_contains","index_of","array_find",
        // int/uint/bool type casts; iN/uN handled dynamically below
        "bool","int","uint"
    };
    // iN/uN type-cast names (for N in [1..256]) are always pure.
    // Also __tw_* (width-specific integer intrinsics) and __tf_* (f32 intrinsics).
    auto isIntWidthCastNamePure = [](const std::string& nm) -> bool {
        if (nm.size() > 5 && (nm.substr(0,5)=="__tw_" || nm.substr(0,5)=="__tf_"))
            return true;
        if (nm.size() < 2 || (nm[0] != 'i' && nm[0] != 'u')) return false;
        int bw = 0;
        for (size_t j = 1; j < nm.size(); ++j) {
            if (!std::isdigit(static_cast<unsigned char>(nm[j]))) return false;
            bw = bw * 10 + (nm[j] - '0');
            if (bw > 256) return false;
        }
        return bw >= 1 && bw <= 256;
    };

    // Detect whether a function body is pure (no I/O, no mutations of globals).
    // Uses a recursive helper with a visited set to handle mutual recursion.
    std::function<bool(const FunctionDecl*, std::unordered_set<std::string>&)> isPureBody;
    isPureBody = [&](const FunctionDecl* fn, std::unordered_set<std::string>& visiting) -> bool {
        if (!fn || !fn->body) return false;
        if (kBuiltinPure.count(fn->name) || isIntWidthCastNamePure(fn->name)) return true;
        if (pureFunctions_.count(fn->name)) return true;
        if (visiting.count(fn->name)) return false; // conservatively not pure (recursion)
        visiting.insert(fn->name);

        // Walk the body; reject if any I/O or non-pure call is found.
        static const std::unordered_set<std::string> kImpure = {
            "print","println","write","print_char","input","input_line",
            "file_read","file_write","file_append","file_exists",
            "thread_create","thread_join","mutex_new","mutex_lock",
            "mutex_unlock","mutex_destroy","time","sleep","random",
            "exit","exit_program","assert"
        };

        std::function<bool(const Statement*)>  pureS;
        std::function<bool(const Expression*)> pureE;
        pureE = [&](const Expression* ex) -> bool {
            if (!ex) return true;
            if (ex->type == ASTNodeType::CALL_EXPR) {
                auto* call = static_cast<const CallExpr*>(ex);
                if (kImpure.count(call->callee)) return false;
                if (!kBuiltinPure.count(call->callee) && !isIntWidthCastNamePure(call->callee)) {
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

    // ── Phase 5: build call graph (lightweight pass) ───────────────────────
    for (auto& fn : program->functions) {
        if (!fn->body) continue;
        graph_.nodes.push_back(fn->name);
        // Walk calls in the function body.
        std::function<void(const Expression*)> walkE = [&](const Expression* ex) {
            if (!ex) return;
            if (ex->type == ASTNodeType::CALL_EXPR) {
                auto* call = static_cast<const CallExpr*>(ex);
                graph_.edges.push_back({fn->name, call->callee});
            }
            // Recurse into sub-expressions (simplified).
            if (ex->type == ASTNodeType::BINARY_EXPR) {
                auto* b = static_cast<const BinaryExpr*>(ex);
                walkE(b->left.get()); walkE(b->right.get());
            }
            if (ex->type == ASTNodeType::CALL_EXPR) {
                auto* c = static_cast<const CallExpr*>(ex);
                for (auto& arg : c->arguments) walkE(arg.get());
            }
        };
        std::function<void(const Statement*)> walkS = [&](const Statement* st) {
            if (!st) return;
            if (st->type == ASTNodeType::EXPR_STMT) {
                auto* es = static_cast<const ExprStmt*>(st);
                walkE(es->expression.get());
            }
            if (st->type == ASTNodeType::RETURN_STMT) {
                auto* r = static_cast<const ReturnStmt*>(st);
                walkE(r->value.get());
            }
            if (st->type == ASTNodeType::VAR_DECL) {
                auto* d = static_cast<const VarDecl*>(st);
                walkE(d->initializer.get());
            }
            if (st->type == ASTNodeType::BLOCK) {
                auto* blk = static_cast<const BlockStmt*>(st);
                for (auto& s2 : blk->statements) walkS(s2.get());
            }
            if (st->type == ASTNodeType::IF_STMT) {
                auto* ifs = static_cast<const IfStmt*>(st);
                walkE(ifs->condition.get());
                walkS(ifs->thenBranch.get());
                walkS(ifs->elseBranch.get());
            }
            if (st->type == ASTNodeType::FOR_STMT) {
                auto* fs = static_cast<const ForStmt*>(st);
                walkE(fs->start.get()); walkE(fs->end.get());
                walkS(fs->body.get());
            }
        };
        for (auto& stmt : fn->body->statements) walkS(stmt.get());
    }

    // ── Phase 6: deduplicate graph nodes ──────────────────────────────────
    std::sort(graph_.nodes.begin(), graph_.nodes.end());
    graph_.nodes.erase(std::unique(graph_.nodes.begin(), graph_.nodes.end()), graph_.nodes.end());
}

} // namespace omscript
