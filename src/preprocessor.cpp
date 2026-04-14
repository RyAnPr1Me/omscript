#include "preprocessor.h"
#include "diagnostic.h"
#include "version.h"
#include <algorithm>
#include <cctype>
#include <sstream>

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
            size_t pstart = i;
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
        char c = text[pos++];
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
        char c = text[i];

        // String literal: copy verbatim
        if (c == '"' || c == '\'') {
            char q = c;
            result += c;
            i++;
            while (i < text.size()) {
                char sc = text[i++];
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
            size_t start = i;
            while (i < text.size() && isIdentChar(text[i])) i++;
            std::string ident = text.substr(start, i - start);

            auto it = macros_.find(ident);
            bool isSpecial = (ident == "__FILE__" || ident == "__LINE__" || ident == "__COUNTER__");

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

                    for (size_t pi = 0; pi < def.params.size(); pi++) {
                        const std::string& param = def.params[pi];
                        const std::string& arg = (pi < args.size()) ? args[pi] : "";
                        std::string newExp;
                        size_t ei = 0;
                        while (ei < expanded.size()) {
                            if (isIdentStart(expanded[ei])) {
                                size_t es = ei;
                                while (ei < expanded.size() && isIdentChar(expanded[ei])) ei++;
                                std::string tok = expanded.substr(es, ei - es);
                                newExp += (tok == param) ? arg : tok;
                            } else {
                                newExp += expanded[ei++];
                            }
                        }
                        expanded = newExp;
                    }
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
    long long parseCmp();
    long long parseAnd();
    long long parseOr();
    long long parse() { return parseOr(); }
};

long long ExprEval::parsePrimary() {
    skip();
    if (pos >= src.size()) return 0;

    if (src[pos] == '(') {
        pos++;
        long long v = parseOr();
        skip();
        if (pos < src.size() && src[pos] == ')') pos++;
        return v;
    }

    if (std::isdigit(static_cast<unsigned char>(src[pos]))) {
        size_t start = pos;
        while (pos < src.size() && std::isdigit(static_cast<unsigned char>(src[pos]))) pos++;
        return std::stoll(src.substr(start, pos - start));
    }

    if (isIdStart(src[pos])) {
        size_t start = pos;
        while (pos < src.size() && isIdChar(src[pos])) pos++;
        std::string ident = src.substr(start, pos - start);

        if (ident == "defined") {
            skip();
            bool paren = (pos < src.size() && src[pos] == '(');
            if (paren) pos++;
            skip();
            size_t ns = pos;
            while (pos < src.size() && isIdChar(src[pos])) pos++;
            std::string name = src.substr(ns, pos - ns);
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
    return parsePrimary();
}
long long ExprEval::parseMul() {
    long long v = parseUnary();
    while (true) {
        skip();
        if (pos >= src.size()) break;
        if (src[pos] == '*') { pos++; v *= parseUnary(); }
        else if (src[pos] == '/') { pos++; long long r = parseUnary(); v = r ? v / r : 0; }
        else if (src[pos] == '%') { pos++; long long r = parseUnary(); v = r ? v % r : 0; }
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
long long ExprEval::parseCmp() {
    long long v = parseAdd();
    while (true) {
        skip();
        if (pos + 1 < src.size()) {
            if (src[pos]=='=' && src[pos+1]=='=') { pos+=2; v=(v==parseAdd()); continue; }
            if (src[pos]=='!' && src[pos+1]=='=') { pos+=2; v=(v!=parseAdd()); continue; }
            if (src[pos]=='<' && src[pos+1]=='=') { pos+=2; v=(v<=parseAdd()); continue; }
            if (src[pos]=='>' && src[pos+1]=='=') { pos+=2; v=(v>=parseAdd()); continue; }
        }
        if (pos < src.size() && src[pos] == '<') { pos++; v=(v< parseAdd()); continue; }
        if (pos < src.size() && src[pos] == '>') { pos++; v=(v> parseAdd()); continue; }
        break;
    }
    return v;
}
long long ExprEval::parseAnd() {
    long long v = parseCmp();
    while (true) {
        skip();
        if (pos + 1 < src.size() && src[pos]=='&' && src[pos+1]=='&') {
            pos += 2; long long r = parseCmp(); v = (v && r);
        } else break;
    }
    return v;
}
long long ExprEval::parseOr() {
    long long v = parseAnd();
    while (true) {
        skip();
        if (pos + 1 < src.size() && src[pos]=='|' && src[pos+1]=='|') {
            pos += 2; long long r = parseAnd(); v = (v || r);
        } else break;
    }
    return v;
}

} // anonymous namespace

long long Preprocessor::evalExpr(const std::string& expr, int lineNo) const {
    std::string expanded = substituteMacros(trim(expr), lineNo, 0);
    ExprEval ev(expanded, lineNo, this, macros_);
    return ev.parse();
}

// ============================================================
// Main processing loop
// ============================================================

std::string Preprocessor::process(const std::string& source) {
    // Step 1: Join backslash-continuation lines
    std::string joined;
    joined.reserve(source.size());
    for (size_t i = 0; i < source.size(); i++) {
        if (source[i] == '\\' && i + 1 < source.size() && source[i + 1] == '\n') {
            joined += ' ';
            i++;
            continue;
        }
        joined += source[i];
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

        size_t dpos = stripped.find('#');
        std::string rest = trimLeft(stripped.substr(dpos + 1));

        size_t kend = 0;
        while (kend < rest.size() && std::isalpha(static_cast<unsigned char>(rest[kend]))) kend++;
        std::string kw = rest.substr(0, kend);
        std::string arg = trim(rest.substr(kend));

        if (kw == "ifdef" || kw == "ifndef") {
            const std::string name = trim(arg);
            const bool defined = macros_.count(name) > 0;
            const bool cond = (kw == "ifdef") ? defined : !defined;
            condStack.push_back({isActive() && cond, cond, false});
            output += '\n'; continue;
        }
        if (kw == "if") {
            bool cond = isActive() && (evalExpr(arg, lineNo) != 0);
            condStack.push_back({cond, cond, false});
            output += '\n'; continue;
        }
        if (kw == "elif") {
            if (condStack.size() <= 1)
                throw omscript::DiagnosticError(omscript::Diagnostic{
                    omscript::DiagnosticSeverity::Error,
                    {filename_, lineNo, 0},
                    "#elif without #if"});
            CondEntry& top = condStack.back();
            if (top.done)
                throw omscript::DiagnosticError(omscript::Diagnostic{
                    omscript::DiagnosticSeverity::Error,
                    {filename_, lineNo, 0},
                    "#elif after #else"});
            if (!top.seenTrue && condStack[condStack.size()-2].active) {
                bool cond = (evalExpr(arg, lineNo) != 0);
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
            size_t qpos = arg.find('"');
            if (qpos != std::string::npos) {
                exprPart = trim(arg.substr(0, qpos));
                size_t qend = arg.rfind('"');
                if (qend > qpos)
                    msgPart = arg.substr(qpos + 1, qend - qpos - 1);
            }
            if (evalExpr(exprPart, lineNo) == 0) {
                std::string msg = msgPart.empty()
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
