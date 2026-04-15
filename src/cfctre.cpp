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

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstring>
#include <functional>
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
        int n = std::snprintf(buf, sizeof(buf), "I:%lld",
                              static_cast<long long>(scalar.i64));
        out.append(buf, static_cast<size_t>(n));
        return;
    }
    case CTValueKind::CONCRETE_U64: {
        int n = std::snprintf(buf, sizeof(buf), "U:%llu",
                              static_cast<unsigned long long>(scalar.u64));
        out.append(buf, static_cast<size_t>(n));
        return;
    }
    case CTValueKind::CONCRETE_F64: {
        int n = std::snprintf(buf, sizeof(buf), "F:%.17g", scalar.f64);
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
        int n = std::snprintf(buf, sizeof(buf), "A:%llu",
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
    CTArrayHandle dst = heap_.alloc(orig->len);
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
    for (size_t i = 0; i < args.size(); ++i)
        frame.locals[fn->parameters[i].name] = args[i];

    // Execute.
    ++currentDepth_;
    fuel_ = 0;
    bool ok = executeBody(frame, fn->body.get());
    --currentDepth_;

    if (!ok && !frame.hasReturned && !frame.hasLastBare) return std::nullopt;

    CTValue result;
    if (frame.hasReturned)  result = frame.returnValue;
    else if (frame.hasLastBare) result = frame.lastBareExpr;
    else                    return std::nullopt;

    // Snapshot array results before storing in cache (so cache is stable).
    CTValue cached = result;
    if (cached.isArray() && cached.arr != CT_NULL_HANDLE)
        cached.arr = snapshotArray(cached.arr);
    memoCache_[key] = cached;

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
    bool ok = executeBody(frame, body);
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
            CTValue lv = evalExpr(frame, bin->left.get());
            if (!lv.isKnown()) return CTValue::uninit();
            if (!lv.isTruthy()) return CTValue::fromI64(0);
            CTValue rv = evalExpr(frame, bin->right.get());
            if (!rv.isKnown()) return CTValue::uninit();
            return CTValue::fromI64(rv.isTruthy() ? 1 : 0);
        }
        if (bin->op == "||") {
            CTValue lv = evalExpr(frame, bin->left.get());
            if (!lv.isKnown()) return CTValue::uninit();
            if (lv.isTruthy()) return CTValue::fromI64(1);
            CTValue rv = evalExpr(frame, bin->right.get());
            if (!rv.isKnown()) return CTValue::uninit();
            return CTValue::fromI64(rv.isTruthy() ? 1 : 0);
        }
        // Null-coalescing (??)
        if (bin->op == "??") {
            CTValue lv = evalExpr(frame, bin->left.get());
            if (lv.isKnown() && lv.isTruthy()) return lv;
            return evalExpr(frame, bin->right.get());
        }
        CTValue lv = evalExpr(frame, bin->left.get());
        CTValue rv = evalExpr(frame, bin->right.get());
        return evalBinaryOp(bin->op, std::move(lv), std::move(rv));
    }

    // ── Unary expression ─────────────────────────────────────────────────
    case ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<const UnaryExpr*>(e);
        CTValue val = evalExpr(frame, un->operand.get());
        return evalUnaryOp(un->op, std::move(val));
    }

    // ── Postfix ++ / -- ──────────────────────────────────────────────────
    case ASTNodeType::POSTFIX_EXPR: {
        auto* pfx = static_cast<const PostfixExpr*>(e);
        if (pfx->operand->type != ASTNodeType::IDENTIFIER_EXPR) return CTValue::uninit();
        auto* id  = static_cast<const IdentifierExpr*>(pfx->operand.get());
        auto  it  = frame.locals.find(id->name);
        if (it == frame.locals.end() || !it->second.isInt()) return CTValue::uninit();
        CTValue old = it->second;
        int64_t delta = (pfx->op == "++") ? 1 : -1;
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
        int64_t delta = (pfx->op == "++") ? 1 : -1;
        it->second = CTValue::fromI64(it->second.asI64() + delta);
        return it->second;  // prefix returns new value
    }

    // ── Ternary ──────────────────────────────────────────────────────────
    case ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<const TernaryExpr*>(e);
        CTValue cond = evalExpr(frame, tern->condition.get());
        if (!cond.isKnown()) return CTValue::uninit();
        return cond.isTruthy() ? evalExpr(frame, tern->thenExpr.get())
                                : evalExpr(frame, tern->elseExpr.get());
    }

    // ── Array literal → allocate on CTHeap ──────────────────────────────
    case ASTNodeType::ARRAY_EXPR: {
        auto* ae = static_cast<const ArrayExpr*>(e);
        uint64_t n = static_cast<uint64_t>(ae->elements.size());
        CTArrayHandle h = heap_.alloc(n);
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
        CTValue arr = evalExpr(frame, idx->array.get());
        CTValue idxv= evalExpr(frame, idx->index.get());
        if (!arr.isKnown() || !idxv.isKnown() || !idxv.isInt()) return CTValue::uninit();
        int64_t i = idxv.asI64();
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
        std::string flat = sr->scopeName + "_" + sr->memberName;
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
        CTValue arrVal = evalExpr(frame, ia->array.get());
        CTValue idxVal = evalExpr(frame, ia->index.get());
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
        CTValue lv = evalExpr(frame, pipe->left.get());
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

CTValue CTEngine::evalBinaryOp(const std::string& op, CTValue lhs, CTValue rhs) {
    if (!lhs.isKnown() || !rhs.isKnown()) return CTValue::uninit();

    // String concatenation.
    if (op == "+" && lhs.isString() && rhs.isString())
        return CTValue::fromString(lhs.asStr() + rhs.asStr());

    // String equality / inequality.
    if (lhs.isString() && rhs.isString()) {
        if (op == "==") return CTValue::fromI64(lhs.asStr() == rhs.asStr() ? 1 : 0);
        if (op == "!=") return CTValue::fromI64(lhs.asStr() != rhs.asStr() ? 1 : 0);
        return CTValue::uninit();
    }

    // Float arithmetic.
    if (lhs.isFloat() || rhs.isFloat()) {
        double a = lhs.asF64(), b = rhs.asF64();
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
    int64_t  a = lhs.asI64(), b = rhs.asI64();
    uint64_t ua = lhs.asU64();

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
        int sh = static_cast<int>(b & 63);
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
        int sh = static_cast<int>(b & 63);
        return CTValue::fromU64(ua >> sh);
    }

    return CTValue::uninit();
}

// ═══════════════════════════════════════════════════════════════════════════
// evalUnaryOp
// ═══════════════════════════════════════════════════════════════════════════

CTValue CTEngine::evalUnaryOp(const std::string& op, CTValue val) {
    if (!val.isKnown()) return CTValue::uninit();
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

CTValue CTEngine::evalTypeCast(const std::string& name, CTValue val) {
    if (!val.isKnown()) return CTValue::uninit();
    if (val.isInt()) {
        int64_t v = val.asI64();
        if (name == "u64" || name == "i64" || name == "int" || name == "uint")
            return CTValue::fromI64(v);
        if (name == "u32")
            return CTValue::fromI64(static_cast<int64_t>(static_cast<uint32_t>(v)));
        if (name == "i32")
            return CTValue::fromI64(static_cast<int64_t>(static_cast<int32_t>(v)));
        if (name == "u16")
            return CTValue::fromI64(static_cast<int64_t>(static_cast<uint16_t>(v)));
        if (name == "i16")
            return CTValue::fromI64(static_cast<int64_t>(static_cast<int16_t>(v)));
        if (name == "u8")
            return CTValue::fromI64(static_cast<int64_t>(static_cast<uint8_t>(v)));
        if (name == "i8")
            return CTValue::fromI64(static_cast<int64_t>(static_cast<int8_t>(v)));
        if (name == "bool")
            return CTValue::fromI64(v != 0 ? 1 : 0);
    }
    return CTValue::uninit();
}

// ═══════════════════════════════════════════════════════════════════════════
// evalCall — dispatch builtin or user function
// ═══════════════════════════════════════════════════════════════════════════

CTValue CTEngine::evalCall(CTFrame& /*callerFrame*/,
                            const std::string& fnName,
                            const std::vector<CTValue>& args) {
    // 1. Type-cast builtins.
    static const std::unordered_set<std::string> kTypeCasts = {
        "u64","i64","int","uint","u32","i32","u16","i16","u8","i8","bool"
    };
    if (kTypeCasts.count(fnName) && args.size() == 1)
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
        // Type casts are handled by evalTypeCast, not here, but include for
        // completeness if they reach evalBuiltin via the fallback path:
        "u64","i64","int","uint","u32","i32","u16","i16","u8","i8","bool"
    };
    if (kKnownBuiltins.find(name) == kKnownBuiltins.end())
        return std::nullopt;

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
        CTArrayHandle h = arrArg(0);
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
        CTArrayHandle h = arrArg(0);
        if (h != CT_NULL_HANDLE && args[1].isKnown()) {
            heap_.push(h, args[1]);
            return CTValue::fromI64(0);
        }
        return std::nullopt;
    }

    // ── pop(arr) → removes & returns last element ───────────────────────
    if (name == "pop" && n == 1) {
        CTArrayHandle h = arrArg(0);
        if (h != CT_NULL_HANDLE) {
            uint64_t len = heap_.length(h);
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
            while (ub) { uint64_t t = ub; ub = ua % ub; ua = t; }
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
            while (tb) { uint64_t t = tb; tb = g % tb; g = t; }
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
            int sh = static_cast<int>(*k) & 63;
            return CTValue::fromI64(static_cast<int64_t>((x << sh) | (x >> (64 - sh))));
        }
        return std::nullopt;
    }
    if (name == "rotate_right" && n == 2) {
        auto v = intArg(0), k = intArg(1);
        if (v && k) {
            uint64_t x = static_cast<uint64_t>(*v);
            int sh = static_cast<int>(*k) & 63;
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
            try { return CTValue::fromI64(static_cast<int64_t>(std::stoll(*s))); } catch (...) {}
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

    return std::nullopt;
}

// ═══════════════════════════════════════════════════════════════════════════
// evalStmt — evaluate a single AST statement
// ═══════════════════════════════════════════════════════════════════════════

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
        if (!v.isKnown()) return false;
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
        if (!v.isKnown()) return false;
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
            if (!v.isKnown()) return false;
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
            if (!arrVal.isArray() || !idxVal.isInt()) return false;
            heap_.store(arrVal.asArr(), idxVal.asI64(), std::move(newVal));
            return true;
        }
        // General expression (++/-- / function call with side effects / etc.)
        CTValue v = evalExpr(frame, es->expression.get());
        if (v.isKnown()) { frame.lastBareExpr = v; frame.hasLastBare = true; }
        return v.isKnown();
    }

    // ── If / else ─────────────────────────────────────────────────────────
    case ASTNodeType::IF_STMT: {
        auto* ifs = static_cast<const IfStmt*>(s);
        CTValue cond = evalExpr(frame, ifs->condition.get());
        if (!cond.isKnown()) return false;
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
        int64_t cur = sv.asI64(), end = ev.asI64();
        while (step > 0 ? cur < end : cur > end) {
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
        "u64","u32","u16","u8","i64","i32","i16","i8","bool","int","uint"
    };

    // Detect whether a function body is pure (no I/O, no mutations of globals).
    // Uses a recursive helper with a visited set to handle mutual recursion.
    std::function<bool(const FunctionDecl*, std::unordered_set<std::string>&)> isPureBody;
    isPureBody = [&](const FunctionDecl* fn, std::unordered_set<std::string>& visiting) -> bool {
        if (!fn || !fn->body) return false;
        if (kBuiltinPure.count(fn->name)) return true;
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
                if (!kBuiltinPure.count(call->callee)) {
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
            if (st->type == ASTNodeType::TRY_CATCH_STMT) return false;
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
