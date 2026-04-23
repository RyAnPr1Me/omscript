#include "preprocessor.h"
#include "diagnostic.h"
#include "version.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <unordered_set>

// Detect OS and architecture at compile time for __OS__ / __ARCH__
#if defined(_WIN32) || defined(_WIN64)
#  define OMSC_PP_OS   "windows"
#elif defined(__APPLE__)
#  define OMSC_PP_OS   "macos"
#else
#  define OMSC_PP_OS   "linux"
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  define OMSC_PP_ARCH "x86_64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#  define OMSC_PP_ARCH "aarch64"
#elif defined(__arm__) || defined(_M_ARM)
#  define OMSC_PP_ARCH "arm"
#else
#  define OMSC_PP_ARCH "unknown"
#endif

namespace omscript {

// ============================================================
// Construction / predefined macros
// ============================================================

Preprocessor::Preprocessor(std::string filename)
    : filename_(std::move(filename)) {
    macros_["__VERSION__"] = {false, false, {}, "\"" OMSC_VERSION "\""};
    macros_["__OS__"]      = {false, false, {}, "\"" OMSC_PP_OS "\""};
    macros_["__ARCH__"]    = {false, false, {}, "\"" OMSC_PP_ARCH "\""};
    // Original file as the user passed it on the command line — preserved
    // across `#line` directives that rewrite __FILE__ / line numbers (a la
    // C99 __BASE_FILE__).
    macros_["__BASE_FILE__"] = {false, false, {}, "\"" + filename_ + "\""};

    // Build-time date / time of the *compilation of this source file*.
    // Computed once per Preprocessor instance to keep all uses within a
    // single source consistent (mirrors C semantics: a single pp-token
    // value per translation unit).
    {
        std::time_t now = std::time(nullptr);
        std::tm tm_buf{};
#if defined(_WIN32)
        localtime_s(&tm_buf, &now);
#else
        localtime_r(&now, &tm_buf);
#endif
        // C-style "Mmm dd yyyy" — matches C's __DATE__.
        static const char* const kMonths[12] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"};
        char dateBuf[32];
        std::snprintf(dateBuf, sizeof(dateBuf), "\"%s %2d %4d\"",
                      kMonths[tm_buf.tm_mon], tm_buf.tm_mday,
                      tm_buf.tm_year + 1900);
        char timeBuf[16];
        std::snprintf(timeBuf, sizeof(timeBuf), "\"%02d:%02d:%02d\"",
                      tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        macros_["__DATE__"] = {false, false, {}, dateBuf};
        macros_["__TIME__"] = {false, false, {}, timeBuf};
    }

    // Target-feature predefined macros — reflect the SIMD / ISA feature set
    // the compiler binary itself was built for.  This lets user code
    // conditionally select vector widths and intrinsics, e.g.
    //   #if defined(__SIMD_AVX2__)
    //       var v: i32x8 = ...;
    //   #else
    //       var v: i32x4 = ...;
    //   #endif
    // Only macros for features actually present are defined (mirrors GCC /
    // clang behavior for __AVX2__, __SSE4_2__, etc.).
    auto define1 = [&](const char* name) {
        macros_[name] = {false, false, {}, "1"};
    };
#if defined(__SSE2__)
    define1("__SIMD_SSE2__");
#endif
#if defined(__SSE3__)
    define1("__SIMD_SSE3__");
#endif
#if defined(__SSSE3__)
    define1("__SIMD_SSSE3__");
#endif
#if defined(__SSE4_1__)
    define1("__SIMD_SSE41__");
#endif
#if defined(__SSE4_2__)
    define1("__SIMD_SSE42__");
#endif
#if defined(__AVX__)
    define1("__SIMD_AVX__");
#endif
#if defined(__AVX2__)
    define1("__SIMD_AVX2__");
#endif
#if defined(__AVX512F__)
    define1("__SIMD_AVX512F__");
#endif
#if defined(__AVX512BW__)
    define1("__SIMD_AVX512BW__");
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    define1("__SIMD_NEON__");
#endif

    // The natural SIMD vector width (lane count for i32) — matches the
    // value codegen reports via preferredVectorWidth_.  Lets user code
    // size hand-tuned SIMD chunks without hardcoding `4` or `8`.
#if defined(__AVX512F__)
    macros_["__VECTOR_WIDTH__"] = {false, false, {}, "16"};
#elif defined(__AVX2__) || defined(__AVX__)
    macros_["__VECTOR_WIDTH__"] = {false, false, {}, "8"};
#else
    macros_["__VECTOR_WIDTH__"] = {false, false, {}, "4"};
#endif
}

// ============================================================
// Static helpers
// ============================================================

bool Preprocessor::isIdentStart(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool Preprocessor::isIdentChar(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}
std::string Preprocessor::trimLeft(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) i++;
    return s.substr(i);
}
std::string Preprocessor::trim(const std::string& s) {
    if (s.empty()) return s;
    size_t l = 0, r = s.size() - 1;
    while (l <= r && std::isspace(static_cast<unsigned char>(s[l]))) l++;
    while (r > l && std::isspace(static_cast<unsigned char>(s[r]))) r--;
    return s.substr(l, r - l + 1);
}

// ============================================================
// Version comparison
// ============================================================

int Preprocessor::cmpVersion(const std::string& a, const std::string& b) {
    auto parse = [](const std::string& s) {
        std::vector<int> parts;
        std::istringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, '.')) {
            try { parts.push_back(std::stoi(tok)); }
            catch (...) { parts.push_back(0); }
        }
        return parts;
    };
    auto pa = parse(a), pb = parse(b);
    const size_t n = std::max(pa.size(), pb.size());
    pa.resize(n, 0); pb.resize(n, 0);
    for (size_t i = 0; i < n; i++) {
        if (pa[i] < pb[i]) return -1;
        if (pa[i] > pb[i]) return  1;
    }
    return 0;
}

// ============================================================
// #define parsing
// ============================================================

void Preprocessor::handleDefine(const std::string& rest, int lineNo) {
    const std::string r = trimLeft(rest);
    size_t i = 0;
    if (i >= r.size() || !isIdentStart(r[i])) {
        throw omscript::DiagnosticError(omscript::Diagnostic{
            omscript::DiagnosticSeverity::Error,
            {filename_, lineNo, 0},
            "#define: expected macro name"});
    }
    while (i < r.size() && isIdentChar(r[i])) i++;
    const std::string name = r.substr(0, i);

    MacroDef def;

    if (i < r.size() && r[i] == '(') {
        def.isFunctionLike = true;
        i++;
        while (i < r.size() && r[i] != ')') {
            while (i < r.size() && std::isspace(static_cast<unsigned char>(r[i]))) i++;
            if (r[i] == ')') break;
            const size_t pstart = i;
            while (i < r.size() && isIdentChar(r[i])) i++;
            if (i > pstart) def.params.push_back(r.substr(pstart, i - pstart));
            while (i < r.size() && std::isspace(static_cast<unsigned char>(r[i]))) i++;
            if (i < r.size() && r[i] == ',') i++;
        }
        if (i < r.size()) i++;
    }

    if (i < r.size()) {
        def.body = trim(r.substr(i));
    }

    macros_[name] = std::move(def);
}

// ============================================================
// Macro argument collection
// ============================================================

std::vector<std::string> Preprocessor::collectArgs(const std::string& text,
                                                     size_t& pos) {
    std::vector<std::string> args;
    std::string cur;
    int depth = 1;
    bool inStr = false;
    char strChar = 0;
    while (pos < text.size() && depth > 0) {
        const char c = text[pos++];
        if (inStr) {
            cur += c;
            if (c == '\\' && pos < text.size()) { cur += text[pos++]; continue; }
            if (c == strChar) inStr = false;
            continue;
        }
        if (c == '"' || c == '\'') { inStr = true; strChar = c; cur += c; continue; }
        if (c == '(') { depth++; cur += c; continue; }
        if (c == ')') { depth--; if (depth == 0) { args.push_back(trim(cur)); return args; } cur += c; continue; }
        if (c == ',' && depth == 1) { args.push_back(trim(cur)); cur.clear(); continue; }
        cur += c;
    }
    if (!trim(cur).empty()) args.push_back(trim(cur));
    return args;
}

// ============================================================
// expandSimple
// ============================================================

std::string Preprocessor::expandSimple(const std::string& name, int lineNo,
                                        int depth) const {
    if (name == "__FILE__")
        return "\"" + filename_ + "\"";
    if (name == "__LINE__")
        return std::to_string(lineNo);
    if (name == "__COUNTER__") {
        return std::to_string(const_cast<Preprocessor*>(this)->globalCounter_++);
    }

    auto it = macros_.find(name);
    if (it == macros_.end()) return name;

    const MacroDef& def = it->second;

    if (def.isCounter) {
        return std::to_string(def.counterValue++);
    }

    if (def.isFunctionLike) return name;

    return substituteMacros(def.body, lineNo, depth + 1);
}

// ============================================================
// substituteMacros — the single full definition
// ============================================================

std::string Preprocessor::substituteMacros(const std::string& text, int lineNo,
                                             int depth) const {
    if (depth > 64) return text;

    std::string result;
    result.reserve(text.size());
    size_t i = 0;

    while (i < text.size()) {
        const char c = text[i];

        // String literal: copy verbatim
        if (c == '"' || c == '\'') {
            const char q = c;
            result += c;
            i++;
            while (i < text.size()) {
                const char sc = text[i++];
                result += sc;
                if (sc == '\\' && i < text.size()) { result += text[i++]; continue; }
                if (sc == q) break;
            }
            continue;
        }

        // Line comment
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size()) result += text[i++];
            continue;
        }

        // Block comment
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            result += text[i++]; result += text[i++];
            while (i + 1 < text.size()) {
                if (text[i] == '*' && text[i + 1] == '/') {
                    result += text[i++]; result += text[i++]; break;
                }
                result += text[i++];
            }
            continue;
        }

        // Identifier: check for macro
        if (isIdentStart(c)) {
            const size_t start = i;
            while (i < text.size() && isIdentChar(text[i])) i++;
            const std::string ident = text.substr(start, i - start);

            auto it = macros_.find(ident);
            const bool isSpecial = (ident == "__FILE__" || ident == "__LINE__" || ident == "__COUNTER__");

            if (isSpecial || (it != macros_.end() && !it->second.isFunctionLike)) {
                result += expandSimple(ident, lineNo, depth);
                continue;
            }

            if (it != macros_.end() && it->second.isFunctionLike) {
                size_t j = i;
                while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) j++;
                if (j < text.size() && text[j] == '(') {
                    j++;
                    std::vector<std::string> args = collectArgs(text, j);
                    i = j;

                    const MacroDef& def = it->second;
                    std::string expanded = def.body;

                    // ── Stringification: replace `#param` with `"arg"`.
                    //
                    // Per C99-style preprocessor semantics, `#x` in the macro
                    // body is replaced by a string literal containing the
                    // (un-expanded) actual argument text, with leading and
                    // trailing whitespace trimmed and embedded `"` and `\`
                    // escaped.  This pass runs BEFORE normal parameter
                    // substitution so that stringified parameters are not
                    // also macro-expanded.
                    auto stringifyArg = [](const std::string& a) {
                        std::string s = "\"";
                        // Trim leading/trailing whitespace.
                        size_t start = 0, end = a.size();
                        while (start < end && std::isspace(static_cast<unsigned char>(a[start]))) ++start;
                        while (end > start && std::isspace(static_cast<unsigned char>(a[end - 1]))) --end;
                        for (size_t k = start; k < end; ++k) {
                            char ch = a[k];
                            if (ch == '"' || ch == '\\') s += '\\';
                            s += ch;
                        }
                        s += '"';
                        return s;
                    };
                    {
                        std::string out;
                        out.reserve(expanded.size());
                        size_t k = 0;
                        while (k < expanded.size()) {
                            // Recognise `#` followed by an identifier — but
                            // skip `##` (handled below) so we don't confuse
                            // stringification with token pasting.
                            if (expanded[k] == '#'
                                && k + 1 < expanded.size()
                                && expanded[k + 1] != '#'
                                && (k == 0 || expanded[k - 1] != '#')) {
                                size_t s = k + 1;
                                // Tolerate whitespace between `#` and the param name.
                                while (s < expanded.size()
                                       && std::isspace(static_cast<unsigned char>(expanded[s])))
                                    ++s;
                                if (s < expanded.size() && isIdentStart(expanded[s])) {
                                    size_t e = s;
                                    while (e < expanded.size() && isIdentChar(expanded[e])) ++e;
                                    const std::string name = expanded.substr(s, e - s);
                                    bool replaced = false;
                                    for (size_t pi = 0; pi < def.params.size(); ++pi) {
                                        if (def.params[pi] == name) {
                                            const std::string& a =
                                                (pi < args.size()) ? args[pi] : std::string();
                                            out += stringifyArg(a);
                                            k = e;
                                            replaced = true;
                                            break;
                                        }
                                    }
                                    if (replaced) continue;
                                }
                            }
                            out += expanded[k++];
                        }
                        expanded = std::move(out);
                    }

                    for (size_t pi = 0; pi < def.params.size(); pi++) {
                        const std::string& param = def.params[pi];
                        const std::string& arg = (pi < args.size()) ? args[pi] : "";
                        std::string newExp;
                        size_t ei = 0;
                        while (ei < expanded.size()) {
                            if (isIdentStart(expanded[ei])) {
                                const size_t es = ei;
                                while (ei < expanded.size() && isIdentChar(expanded[ei])) ei++;
                                const std::string tok = expanded.substr(es, ei - es);
                                newExp += (tok == param) ? arg : tok;
                            } else {
                                newExp += expanded[ei++];
                            }
                        }
                        expanded = newExp;
                    }

                    // ── Token pasting: collapse `A ## B` into the single
                    // token `AB`.  We walk the text, and whenever we see
                    // `##` (optionally surrounded by whitespace) we splice
                    // the runs to its left and right together.  Multiple
                    // consecutive pastes (`A ## B ## C`) are handled by
                    // re-running the loop until no `##` remains.
                    auto applyTokenPaste = [](std::string s) {
                        for (;;) {
                            const size_t hpos = s.find("##");
                            if (hpos == std::string::npos) return s;
                            size_t lo = hpos;
                            while (lo > 0 && std::isspace(static_cast<unsigned char>(s[lo - 1])))
                                --lo;
                            size_t hi = hpos + 2;
                            while (hi < s.size() && std::isspace(static_cast<unsigned char>(s[hi])))
                                ++hi;
                            s = s.substr(0, lo) + s.substr(hi);
                        }
                    };
                    expanded = applyTokenPaste(std::move(expanded));

                    result += substituteMacros(expanded, lineNo, depth + 1);
                    continue;
                }
            }

            result += ident;
            continue;
        }

        result += c;
        i++;
    }

    return result;
}

// ============================================================
// Expression evaluator for #if / #elif / #assert
// ============================================================

namespace {

struct ExprEval {
    const std::string& src;
    size_t pos;
    const Preprocessor* pp;
    int lineNo;
    const std::unordered_map<std::string, Preprocessor::MacroDef>& macros;

    explicit ExprEval(const std::string& s, int ln, const Preprocessor* p,
                      const std::unordered_map<std::string, Preprocessor::MacroDef>& m)
        : src(s), pos(0), pp(p), lineNo(ln), macros(m) {}

    void skip() {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) pos++;
    }

    bool isIdStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
    bool isIdChar(char c)  { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

    long long parsePrimary();
    long long parseUnary();
    long long parseMul();
    long long parseAdd();
    long long parseShift();
    long long parseCmp();
    long long parseEq();
    long long parseBitAnd();
    long long parseBitXor();
    long long parseBitOr();
    long long parseAnd();
    long long parseOr();
    long long parseTernary();
    long long parse() { return parseTernary(); }
};

long long ExprEval::parsePrimary() {
    skip();
    if (pos >= src.size()) return 0;

    if (src[pos] == '(') {
        pos++;
        long long v = parseTernary();
        skip();
        if (pos < src.size() && src[pos] == ')') pos++;
        return v;
    }

    if (std::isdigit(static_cast<unsigned char>(src[pos]))) {
        const size_t start = pos;
        // Hex literal: 0x… / 0X…
        if (src[pos] == '0' && pos + 1 < src.size() &&
            (src[pos + 1] == 'x' || src[pos + 1] == 'X')) {
            pos += 2;
            const size_t hstart = pos;
            while (pos < src.size() &&
                   std::isxdigit(static_cast<unsigned char>(src[pos]))) pos++;
            if (pos == hstart) return 0;
            return std::stoll(src.substr(hstart, pos - hstart), nullptr, 16);
        }
        while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos]))) pos++;
        return std::stoll(src.substr(start, pos - start));
    }

    if (isIdStart(src[pos])) {
        const size_t start = pos;
        while (pos < src.size() && isIdChar(src[pos])) pos++;
        const std::string ident = src.substr(start, pos - start);

        if (ident == "defined") {
            skip();
            const bool paren = (pos < src.size() && src[pos] == '(');
            if (paren) pos++;
            skip();
            const size_t ns = pos;
            while (pos < src.size() && isIdChar(src[pos])) pos++;
            const std::string name = src.substr(ns, pos - ns);
            if (paren) { skip(); if (pos < src.size() && src[pos] == ')') pos++; }
            return macros.count(name) ? 1 : 0;
        }

        const std::string expanded = pp->substituteMacros(ident, lineNo, 0);
        if (expanded != ident) {
            ExprEval sub(expanded, lineNo, pp, macros);
            return sub.parse();
        }
        return 0;
    }

    return 0;
}

long long ExprEval::parseUnary() {
    skip();
    if (pos < src.size() && src[pos] == '!') { pos++; return !parseUnary(); }
    if (pos < src.size() && src[pos] == '-') { pos++; return -parseUnary(); }
    if (pos < src.size() && src[pos] == '+') { pos++; return parseUnary(); }
    if (pos < src.size() && src[pos] == '~') { pos++; return ~parseUnary(); }
    return parsePrimary();
}
long long ExprEval::parseMul() {
    long long v = parseUnary();
    while (true) {
        skip();
        if (pos >= src.size()) break;
        if (src[pos] == '*') { pos++; v *= parseUnary(); }
        else if (src[pos] == '/') { pos++; const long long r = parseUnary(); v = r ? v / r : 0; }
        else if (src[pos] == '%') { pos++; const long long r = parseUnary(); v = r ? v % r : 0; }
        else break;
    }
    return v;
}
long long ExprEval::parseAdd() {
    long long v = parseMul();
    while (true) {
        skip();
        if (pos >= src.size()) break;
        if (src[pos] == '+') { pos++; v += parseMul(); }
        else if (src[pos] == '-') { pos++; v -= parseMul(); }
        else break;
    }
    return v;
}
long long ExprEval::parseShift() {
    long long v = parseAdd();
    while (true) {
        skip();
        if (pos + 1 < src.size() && src[pos] == '<' && src[pos+1] == '<') {
            pos += 2; v = v << parseAdd();
        } else if (pos + 1 < src.size() && src[pos] == '>' && src[pos+1] == '>') {
            pos += 2; v = v >> parseAdd();
        } else break;
    }
    return v;
}
long long ExprEval::parseCmp() {
    long long v = parseShift();
    while (true) {
        skip();
        // Two-char relational operators must be tested before the
        // single-char `<` / `>` to avoid eating `<=` as `<` then `=`.
        // `==` and `!=` have lower precedence in C — handled in parseEq.
        if (pos + 1 < src.size()) {
            if (src[pos]=='<' && src[pos+1]=='=') { pos+=2; v=(v<=parseShift()); continue; }
            if (src[pos]=='>' && src[pos+1]=='=') { pos+=2; v=(v>=parseShift()); continue; }
        }
        if (pos < src.size() && src[pos] == '<' &&
            (pos + 1 >= src.size() || src[pos+1] != '<')) {
            pos++; v=(v< parseShift()); continue;
        }
        if (pos < src.size() && src[pos] == '>' &&
            (pos + 1 >= src.size() || src[pos+1] != '>')) {
            pos++; v=(v> parseShift()); continue;
        }
        break;
    }
    return v;
}
long long ExprEval::parseEq() {
    long long v = parseCmp();
    while (true) {
        skip();
        if (pos + 1 < src.size() && src[pos]=='=' && src[pos+1]=='=') {
            pos += 2; v = (v == parseCmp());
        } else if (pos + 1 < src.size() && src[pos]=='!' && src[pos+1]=='=') {
            pos += 2; v = (v != parseCmp());
        } else break;
    }
    return v;
}
long long ExprEval::parseBitAnd() {
    long long v = parseEq();
    while (true) {
        skip();
        // Single `&`, NOT `&&` (handled in parseAnd).
        if (pos < src.size() && src[pos] == '&' &&
            (pos + 1 >= src.size() || src[pos+1] != '&')) {
            pos++; v &= parseEq();
        } else break;
    }
    return v;
}
long long ExprEval::parseBitXor() {
    long long v = parseBitAnd();
    while (true) {
        skip();
        if (pos < src.size() && src[pos] == '^') { pos++; v ^= parseBitAnd(); }
        else break;
    }
    return v;
}
long long ExprEval::parseBitOr() {
    long long v = parseBitXor();
    while (true) {
        skip();
        // Single `|`, NOT `||` (handled in parseOr).
        if (pos < src.size() && src[pos] == '|' &&
            (pos + 1 >= src.size() || src[pos+1] != '|')) {
            pos++; v |= parseBitXor();
        } else break;
    }
    return v;
}
long long ExprEval::parseAnd() {
    long long v = parseBitOr();
    while (true) {
        skip();
        if (pos + 1 < src.size() && src[pos]=='&' && src[pos+1]=='&') {
            pos += 2; const long long r = parseBitOr(); v = (v && r);
        } else break;
    }
    return v;
}
long long ExprEval::parseOr() {
    long long v = parseAnd();
    while (true) {
        skip();
        if (pos + 1 < src.size() && src[pos]=='|' && src[pos+1]=='|') {
            pos += 2; const long long r = parseAnd(); v = (v || r);
        } else break;
    }
    return v;
}
long long ExprEval::parseTernary() {
    long long c = parseOr();
    skip();
    if (pos < src.size() && src[pos] == '?') {
        pos++;
        // Both branches are evaluated unconditionally to keep the parser
        // straight-line; this is acceptable for `#if`-style integer
        // expressions which have no side effects.
        long long t = parseTernary();
        skip();
        if (pos < src.size() && src[pos] == ':') {
            pos++;
            long long f = parseTernary();
            return c ? t : f;
        }
        return c ? t : 0;
    }
    return c;
}

} // anonymous namespace

long long Preprocessor::evalExpr(const std::string& expr, int lineNo) const {
    const std::string expanded = substituteMacros(trim(expr), lineNo, 0);
    ExprEval ev(expanded, lineNo, this, macros_);
    return ev.parse();
}

// ============================================================
// Main processing loop
// ============================================================

std::string Preprocessor::process(const std::string& source) {
    // Step 1: Join backslash-continuation lines AND normalise CRLF / lone CR
    // to LF.  Without this, a source file authored on Windows would carry
    // embedded `\r` characters into directive keywords (`#define\r`),
    // macro names (`FOO\r`), and `#if` expression text — silently breaking
    // every directive on the line.
    std::string joined;
    joined.reserve(source.size());
    for (size_t i = 0; i < source.size(); i++) {
        const char c = source[i];
        if (c == '\\' && i + 1 < source.size() && source[i + 1] == '\n') {
            joined += ' ';
            i++;
            continue;
        }
        // CRLF → LF (drop the \r)
        if (c == '\r' && i + 1 < source.size() && source[i + 1] == '\n') {
            continue; // skip the \r; the \n is emitted on the next iteration
        }
        // Lone CR → LF (very old Mac line endings)
        if (c == '\r') {
            joined += '\n';
            continue;
        }
        joined += c;
    }

    // Step 2: Split into lines
    std::vector<std::string> lines;
    {
        std::istringstream ss(joined);
        std::string line;
        while (std::getline(ss, line)) lines.push_back(line);
    }

    // Step 3: Process each line
    struct CondEntry {
        bool active;
        bool seenTrue;
        bool done;
    };
    std::vector<CondEntry> condStack;
    condStack.push_back({true, true, false});

    auto isActive = [&]() { return condStack.back().active; };

    std::string output;
    output.reserve(source.size());

    for (int lineIdx = 0; lineIdx < static_cast<int>(lines.size()); lineIdx++) {
        const int lineNo = lineIdx + 1;
        const std::string& rawLine = lines[lineIdx];

        std::string stripped = trimLeft(rawLine);
        const bool isDirective = !stripped.empty() && stripped[0] == '#';

        if (!isDirective) {
            if (isActive()) {
                output += substituteMacros(rawLine, lineNo, 0);
            }
            output += '\n';
            continue;
        }

        const size_t dpos = stripped.find('#');
        std::string rest = trimLeft(stripped.substr(dpos + 1));

        size_t kend = 0;
        while (kend < rest.size() && std::isalpha(static_cast<unsigned char>(rest[kend]))) kend++;
        const std::string kw = rest.substr(0, kend);
        const std::string arg = trim(rest.substr(kend));

        if (kw == "ifdef" || kw == "ifndef") {
            const std::string name = trim(arg);
            const bool defined = macros_.count(name) > 0;
            const bool cond = (kw == "ifdef") ? defined : !defined;
            condStack.push_back({isActive() && cond, cond, false});
            output += '\n'; continue;
        }
        if (kw == "if") {
            const bool cond = isActive() && (evalExpr(arg, lineNo) != 0);
            condStack.push_back({cond, cond, false});
            output += '\n'; continue;
        }
        if (kw == "elif" || kw == "elifdef" || kw == "elifndef") {
            if (condStack.size() <= 1)
                throw omscript::DiagnosticError(omscript::Diagnostic{
                    omscript::DiagnosticSeverity::Error,
                    {filename_, lineNo, 0},
                    "#" + kw + " without #if"});
            CondEntry& top = condStack.back();
            if (top.done)
                throw omscript::DiagnosticError(omscript::Diagnostic{
                    omscript::DiagnosticSeverity::Error,
                    {filename_, lineNo, 0},
                    "#" + kw + " after #else"});
            if (!top.seenTrue && condStack[condStack.size()-2].active) {
                bool cond;
                if (kw == "elifdef") {
                    cond = macros_.count(trim(arg)) > 0;
                } else if (kw == "elifndef") {
                    cond = macros_.count(trim(arg)) == 0;
                } else {
                    cond = (evalExpr(arg, lineNo) != 0);
                }
                top.active = cond;
                top.seenTrue = cond;
            } else {
                top.active = false;
            }
            output += '\n'; continue;
        }
        if (kw == "else") {
            if (condStack.size() <= 1)
                throw omscript::DiagnosticError(omscript::Diagnostic{
                    omscript::DiagnosticSeverity::Error,
                    {filename_, lineNo, 0},
                    "#else without #if"});
            CondEntry& top = condStack.back();
            if (top.done)
                throw omscript::DiagnosticError(omscript::Diagnostic{
                    omscript::DiagnosticSeverity::Error,
                    {filename_, lineNo, 0},
                    "duplicate #else"});
            top.done = true;
            if (!top.seenTrue && condStack[condStack.size()-2].active) {
                top.active = true;
                top.seenTrue = true;
            } else {
                top.active = false;
            }
            output += '\n'; continue;
        }
        if (kw == "endif") {
            if (condStack.size() <= 1)
                throw omscript::DiagnosticError(omscript::Diagnostic{
                    omscript::DiagnosticSeverity::Error,
                    {filename_, lineNo, 0},
                    "#endif without #if"});
            condStack.pop_back();
            output += '\n'; continue;
        }

        if (!isActive()) {
            output += '\n'; continue;
        }

        if (kw == "define") {
            handleDefine(arg, lineNo);
            output += '\n'; continue;
        }
        if (kw == "undef") {
            macros_.erase(trim(arg));
            output += '\n'; continue;
        }
        if (kw == "error") {
            throw omscript::DiagnosticError(omscript::Diagnostic{
                omscript::DiagnosticSeverity::Error,
                {filename_, lineNo, 0},
                "#error " + arg});
        }
        if (kw == "warning") {
            warnings_.push_back(filename_ + ":" + std::to_string(lineNo) +
                                  ": warning: " + arg);
            output += '\n'; continue;
        }
        if (kw == "info") {
            warnings_.push_back(filename_ + ":" + std::to_string(lineNo) +
                                  ": info: " + arg);
            output += '\n'; continue;
        }
        if (kw == "assert") {
            std::string exprPart = arg;
            std::string msgPart;
            const size_t qpos = arg.find('"');
            if (qpos != std::string::npos) {
                exprPart = trim(arg.substr(0, qpos));
                const size_t qend = arg.rfind('"');
                if (qend > qpos)
                    msgPart = arg.substr(qpos + 1, qend - qpos - 1);
            }
            if (evalExpr(exprPart, lineNo) == 0) {
                const std::string msg = msgPart.empty()
                    ? "compile-time assertion failed: " + exprPart
                    : msgPart;
                throw omscript::DiagnosticError(omscript::Diagnostic{
                    omscript::DiagnosticSeverity::Error,
                    {filename_, lineNo, 0},
                    "#assert failed: " + msg});
            }
            output += '\n'; continue;
        }
        if (kw == "require") {
            std::string reqVer = trim(arg);
            if (reqVer.size() >= 2 && reqVer.front() == '"' && reqVer.back() == '"')
                reqVer = reqVer.substr(1, reqVer.size() - 2);
            if (cmpVersion(OMSC_VERSION, reqVer) < 0) {
                throw omscript::DiagnosticError(omscript::Diagnostic{
                    omscript::DiagnosticSeverity::Error,
                    {filename_, lineNo, 0},
                    "#require: compiler version " OMSC_VERSION
                    " is older than required " + reqVer});
            }
            output += '\n'; continue;
        }
        if (kw == "counter") {
            const std::string name = trim(arg);
            MacroDef def;
            def.isCounter = true;
            def.counterValue = 0;
            macros_[name] = std::move(def);
            output += '\n'; continue;
        }
        if (kw == "pragma") {
            // `#pragma once` is recognised silently so user code can adopt
            // the GCC/Clang-style header-guard idiom.  Real `#include`
            // de-duplication isn't part of this preprocessor (OmScript uses
            // the `import` statement at the parser level for that), but we
            // still accept the directive so polyglot headers don't trip a
            // warning on every line.
            //
            // Other `#pragma <key>` forms are passed through silently for
            // now to preserve behaviour with existing code that may use
            // pragmas as toolchain hints.
            output += '\n'; continue;
        }

        warnings_.push_back(filename_ + ":" + std::to_string(lineNo) +
                              ": warning: unknown preprocessor directive '#" + kw + "'");
        output += '\n';
    }

    if (condStack.size() > 1) {
        throw omscript::DiagnosticError(omscript::Diagnostic{
            omscript::DiagnosticSeverity::Error,
            {filename_, 0, 0},
            "unterminated #if / #ifdef block"});
    }

    return output;
}

} // namespace omscript
