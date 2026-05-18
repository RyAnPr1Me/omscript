#include "parser.h"
#include "diagnostic.h"
#include "pass_utils.h" // isIntWidthTypeName, isKnownScalarTypeName
#include <filesystem>
#include <fstream>
#include <llvm/Support/ErrorHandling.h>
#include <optional>
#include <stdexcept>

namespace omscript {

/// Maximum number of hops followed when resolving a transitive type alias chain.
/// Must match the same constant in codegen.cpp.
static constexpr int kMaxTypeAliasHops = 32;

Parser::Parser(const std::vector<Token>& tokens)
    : tokens(tokens), current(0), inOptMaxFunction(false),
      importedFiles_(std::make_shared<std::unordered_set<std::string>>()) {
    registerStdNamespace();
}

Parser::Parser(std::vector<Token>&& tokens)
    : tokens(std::move(tokens)), current(0), inOptMaxFunction(false),
      importedFiles_(std::make_shared<std::unordered_set<std::string>>()) {
    registerStdNamespace();
}

void Parser::registerStdNamespace() {
    // The `std` namespace is built-in: every standard library function is
    // accessible as std::name without any import statement.  Each entry maps
    // the unqualified name to itself (same resolution as a plain call).
    // `std::synthesize` maps to `std__synthesize` which is handled specially
    // by the CF-CTRE evaluator and the synthesis pre-codegen pass.
    static const std::vector<std::pair<std::string, std::string>> kStdFunctions = {
        // ── Math ────────────────────────────────────────────────────────────
        {"abs", "abs"},
        {"min", "min"},
        {"max", "max"},
        {"sign", "sign"},
        {"clamp", "clamp"},
        {"pow", "pow"},
        {"sqrt", "sqrt"},
        {"cbrt", "cbrt"},
        {"floor", "floor"},
        {"ceil", "ceil"},
        {"round", "round"},
        {"log", "log"},
        {"log2", "log2"},
        {"log10", "log10"},
        {"exp", "exp"},
        {"exp2", "exp2"},
        {"gcd", "gcd"},
        {"lcm", "lcm"},
        {"hypot", "hypot"},
        {"fma", "fma"},
        {"copysign", "copysign"},
        {"min_float", "min_float"},
        {"max_float", "max_float"},
        // ── Trig ────────────────────────────────────────────────────────────
        {"sin", "sin"},
        {"cos", "cos"},
        {"tan", "tan"},
        {"asin", "asin"},
        {"acos", "acos"},
        {"atan", "atan"},
        {"atan2", "atan2"},
        // ── Bit ops ─────────────────────────────────────────────────────────
        {"popcount", "popcount"},
        {"clz", "clz"},
        {"ctz", "ctz"},
        {"bitreverse", "bitreverse"},
        {"bswap", "bswap"},
        {"rotate_left", "rotate_left"},
        {"rotate_right", "rotate_right"},
        {"saturating_add", "saturating_add"},
        {"saturating_sub", "saturating_sub"},
        {"is_power_of_2", "is_power_of_2"},
        {"is_even", "is_even"},
        {"is_odd", "is_odd"},
        // ── Type casts / numeric ─────────────────────────────────────────────
        {"to_int", "to_int"},
        {"to_float", "to_float"},
        {"to_string", "to_string"},
        {"to_char", "to_char"},
        {"number_to_string", "number_to_string"},
        {"string_to_number", "string_to_number"},
        {"str_to_int", "str_to_int"},
        {"str_to_float", "str_to_float"},
        {"typeof", "typeof"},
        // ── String ──────────────────────────────────────────────────────────
        {"len", "len"},
        {"str_len", "str_len"},
        {"str_eq", "str_eq"},
        {"str_concat", "str_concat"},
        {"str_find", "str_find"},
        {"str_index_of", "str_index_of"},
        {"str_contains", "str_contains"},
        {"str_starts_with", "str_starts_with"},
        {"str_ends_with", "str_ends_with"},
        {"str_substr", "str_substr"},
        {"str_upper", "str_upper"},
        {"str_lower", "str_lower"},
        {"str_trim", "str_trim"},
        {"str_lstrip", "str_lstrip"},
        {"str_rstrip", "str_rstrip"},
        {"str_reverse", "str_reverse"},
        {"str_repeat", "str_repeat"},
        {"str_count", "str_count"},
        {"str_replace", "str_replace"},
        {"str_pad_left", "str_pad_left"},
        {"str_pad_right", "str_pad_right"},
        {"str_chars", "str_chars"},
        {"str_split", "str_split"},
        {"str_join", "str_join"},
        {"str_filter", "str_filter"},
        {"str_remove", "str_remove"},
        {"str_format", "str_format"},
        {"char_at", "char_at"},
        {"char_code", "char_code"},
        {"is_alpha", "is_alpha"},
        {"is_digit", "is_digit"},
        {"is_upper", "is_upper"},
        {"is_lower", "is_lower"},
        {"is_space", "is_space"},
        {"is_alnum", "is_alnum"},
        // ── Array ────────────────────────────────────────────────────────────
        {"push", "push"},
        {"pop", "pop"},
        {"shift", "shift"},
        {"unshift", "unshift"},
        {"len", "len"},
        {"reverse", "reverse"},
        {"sort", "sort"},
        {"sum", "sum"},
        {"index_of", "index_of"},
        {"swap", "swap"},
        {"array_fill", "array_fill"},
        {"range", "range"},
        {"range_step", "range_step"},
        {"array_concat", "array_concat"},
        {"array_slice", "array_slice"},
        {"array_copy", "array_copy"},
        {"array_contains", "array_contains"},
        {"array_find", "array_find"},
        {"array_min", "array_min"},
        {"array_max", "array_max"},
        {"array_last", "array_last"},
        {"array_product", "array_product"},
        {"array_map", "array_map"},
        {"array_filter", "array_filter"},
        {"array_reduce", "array_reduce"},
        {"array_any", "array_any"},
        {"array_every", "array_every"},
        {"array_count", "array_count"},
        {"array_unique", "array_unique"},
        {"array_zip", "array_zip"},
        {"array_take", "array_take"},
        {"array_drop", "array_drop"},
        {"array_rotate", "array_rotate"},
        {"array_insert", "array_insert"},
        {"array_remove", "array_remove"},
        {"array_mean", "array_mean"},
        // ── Map ─────────────────────────────────────────────────────────────
        {"map_new", "map_new"},
        {"map_get", "map_get"},
        {"map_set", "map_set"},
        {"map_has", "map_has"},
        {"map_remove", "map_remove"},
        {"map_keys", "map_keys"},
        {"map_values", "map_values"},
        {"map_size", "map_size"},
        {"map_merge", "map_merge"},
        {"map_filter", "map_filter"},
        {"map_invert", "map_invert"},
        // ── Generic ──────────────────────────────────────────────────────────
        {"filter", "filter"},
        // ── I/O ─────────────────────────────────────────────────────────────
        {"print", "print"},
        {"println", "println"},
        {"write", "write"},
        {"print_char", "print_char"},
        {"input", "input"},
        {"input_line", "input_line"},
        {"file_read", "file_read"},
        {"file_write", "file_write"},
        {"file_append", "file_append"},
        {"file_exists", "file_exists"},
        // ── System ──────────────────────────────────────────────────────────
        {"exit", "exit"},
        {"exit_program", "exit_program"},
        {"command", "command"},
        {"shell", "shell"},
        {"sudo_command", "sudo_command"},
        {"env_get", "env_get"},
        {"env_set", "env_set"},
        {"time", "time"},
        {"sleep", "sleep"},
        {"random", "random"},
        // ── Threading ────────────────────────────────────────────────────────
        {"thread_create", "thread_create"},
        {"thread_create_ex", "thread_create_ex"},
        {"thread_join", "thread_join"},
        {"thread_detach", "thread_detach"},
        {"thread_self", "thread_self"},
        {"thread_equal", "thread_equal"},
        {"mutex_new", "mutex_new"},
        {"mutex_lock", "mutex_lock"},
        {"mutex_try_lock", "mutex_try_lock"},
        {"mutex_unlock", "mutex_unlock"},
        {"mutex_destroy", "mutex_destroy"},
        {"rwlock_new", "rwlock_new"},
        {"rwlock_rdlock", "rwlock_rdlock"},
        {"rwlock_try_rdlock", "rwlock_try_rdlock"},
        {"rwlock_wrlock", "rwlock_wrlock"},
        {"rwlock_try_wrlock", "rwlock_try_wrlock"},
        {"rwlock_unlock", "rwlock_unlock"},
        {"rwlock_destroy", "rwlock_destroy"},
        {"cond_new", "cond_new"},
        {"cond_wait", "cond_wait"},
        {"cond_signal", "cond_signal"},
        {"cond_broadcast", "cond_broadcast"},
        {"cond_destroy", "cond_destroy"},
        // ── Assertions / hints ────────────────────────────────────────────────
        {"assert", "assert"},
        {"expect", "expect"},
        {"assume", "assume"},
        {"unreachable", "unreachable"},
        // ── Compile-time compilation ──────────────────────────────────────────
        {"compile", "compile"},
        // ── BigInt ───────────────────────────────────────────────────────────
        {"bigint", "bigint"},
        {"bigint_add", "bigint_add"},
        {"bigint_sub", "bigint_sub"},
        {"bigint_mul", "bigint_mul"},
        {"bigint_div", "bigint_div"},
        {"bigint_mod", "bigint_mod"},
        {"bigint_neg", "bigint_neg"},
        {"bigint_abs", "bigint_abs"},
        {"bigint_pow", "bigint_pow"},
        {"bigint_gcd", "bigint_gcd"},
        {"bigint_cmp", "bigint_cmp"},
        {"bigint_eq", "bigint_eq"},
        {"bigint_lt", "bigint_lt"},
        {"bigint_le", "bigint_le"},
        {"bigint_gt", "bigint_gt"},
        {"bigint_ge", "bigint_ge"},
        {"bigint_tostring", "bigint_tostring"},
        {"bigint_to_i64", "bigint_to_i64"},
        {"bigint_shl", "bigint_shl"},
        {"bigint_shr", "bigint_shr"},
        {"bigint_bit_length", "bigint_bit_length"},
        {"bigint_is_zero", "bigint_is_zero"},
        {"bigint_is_negative", "bigint_is_negative"},
        // ── Fast / precise arithmetic ─────────────────────────────────────────
        {"fast_add", "fast_add"},
        {"fast_sub", "fast_sub"},
        {"fast_mul", "fast_mul"},
        {"fast_div", "fast_div"},
        {"precise_add", "precise_add"},
        {"precise_sub", "precise_sub"},
        {"precise_mul", "precise_mul"},
        {"precise_div", "precise_div"},
        // ── std::synthesize — the program synthesis stdlib function ───────────
        // Resolves to the internal name "std__synthesize" so that the CF-CTRE
        // builtin evaluator and the synthesis pre-codegen pass can identify it.
        {"synthesize", "std__synthesize"},
        // ── Type-specific fast builtins ───────────────────────────────────────
        {"mulhi", "mulhi"},
        {"mulhi_u", "mulhi_u"},
        {"absdiff", "absdiff"},
        {"fast_sqrt", "fast_sqrt"},
        {"is_nan", "is_nan"},
        {"is_inf", "is_inf"},
        // ── Round-92: zero-cost builtins ─────────────────────────────────────
        {"lerp",             "lerp"},
        {"trunc",            "trunc"},
        {"rint",             "rint"},
        {"saturating_mul",   "saturating_mul"},
        {"sat_add",          "sat_add"},
        {"sat_sub",          "sat_sub"},
        {"sat_mul",          "sat_mul"},
        {"wrap_add",         "wrap_add"},
        {"wrap_sub",         "wrap_sub"},
        {"wrap_mul",         "wrap_mul"},
        {"bit_floor",        "bit_floor"},
        {"bit_ceil",         "bit_ceil"},
        {"bit_width",        "bit_width"},
    };

    auto& stdNS = importNamespaces_["std"];
    for (const auto& [alias, actual] : kStdFunctions)
        stdNS[alias] = actual;
}

const Token& Parser::peek(int offset) const noexcept {
    static const Token eofToken(TokenType::END_OF_FILE, "", 0, 0);
    if (__builtin_expect(tokens.empty(), 0)) {
        return eofToken;
    }
    const size_t index = current + static_cast<size_t>(offset);
    if (__builtin_expect(index >= tokens.size(), 0)) {
        return tokens.back();
    }
    return tokens[index];
}

Token Parser::advance() noexcept {
    if (!isAtEnd()) {
        current++;
    }
    return tokens[current - 1];
}

bool Parser::check(TokenType type) const noexcept {
    if (__builtin_expect(isAtEnd(), 0))
        return false;
    return peek().type == type;
}

bool Parser::match(TokenType type) noexcept {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::isAtEnd() const noexcept {
    return peek().type == TokenType::END_OF_FILE;
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) {
        return advance();
    }
    error(message);
    llvm_unreachable("error() always throws");
}

[[gnu::cold]] void Parser::error(const std::string& message) {
    Token token = peek();
    throw DiagnosticError(
        Diagnostic{DiagnosticSeverity::Error, {"", token.line, token.column}, "Parse error: " + message});
}

void Parser::synchronize() {
    advance();
    while (!isAtEnd()) {
        // Stop only at tokens that begin a top-level declaration so that
        // statement-level keywords (var, if, for, return, …) inside a broken
        // function body do not cause cascading "Expected 'fn'" errors, and so
        // that OPTMAX_END is never accidentally consumed (which would leave the
        // optMaxTagActive flag set and produce "Unterminated OPTMAX block").
        switch (peek().type) {
        case TokenType::FN:
        case TokenType::OPTMAX_START:
        case TokenType::OPTMAX_END:
            return;
        default:
            break;
        }
        advance();
    }
}

// ---------------------------------------------------------------------------
// Error accumulation helper.
// ---------------------------------------------------------------------------
// Records an exception into both the plain-string errors_ vector (backward
// compatibility) and the structured diagnostics_ vector (rich diagnostics).
// Preserves source location when the exception is a DiagnosticError.
// ---------------------------------------------------------------------------
static void recordException(std::vector<std::string>& errors, std::vector<omscript::Diagnostic>& diagnostics,
                      const std::exception& e) {
    errors.push_back(e.what());
    if (const auto* de = dynamic_cast<const omscript::DiagnosticError*>(&e)) {
        diagnostics.push_back(de->diagnostic());
    } else {
        diagnostics.push_back(
            omscript::Diagnostic{omscript::DiagnosticSeverity::Error, {}, std::string(e.what())});
    }
}

// ---------------------------------------------------------------------------
// Pre-scan: collect all custom operator symbols before the main parse.
// ---------------------------------------------------------------------------
// Walks the token stream looking for the pattern:
//   IDENTIFIER("operator")  [non-LPAREN tokens...]  LPAREN
// and registers the concatenated lexemes of the non-LPAREN tokens as a
// custom operator symbol.  This runs before the main parse so that any
// operator used in expressions later in the file is already known.
// ---------------------------------------------------------------------------
void Parser::prescanCustomOperators() {
    const size_t saved = current;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::IDENTIFIER && tokens[i].lexeme == "operator") {
            // Collect tokens between 'operator' and the first '('
            std::string opStr;
            size_t j = i + 1;
            while (j < tokens.size() && tokens[j].type != TokenType::LPAREN &&
                   tokens[j].type != TokenType::END_OF_FILE) {
                // Skip quoted-string names — they are only callable via backtick.
                // Raw token-sequence symbols (e.g. "<=>" from LE + GT) are what
                // we register here for direct-infix usage.
                if (tokens[j].type != TokenType::STRING)
                    opStr += tokens[j].lexeme;
                ++j;
            }
            if (!opStr.empty() && j < tokens.size() && tokens[j].type == TokenType::LPAREN) {
                customOperatorSymbols_.insert(opStr);
            }
        }
    }
    current = saved;
}

// Pre-scan to collect parameter names for every user-defined function so
// that named call arguments (e.g. foo(height: 3, width: 4)) can be reordered
// to match the declaration order before building the CallExpr.
void Parser::prescanFunctionParams() {
    const size_t saved = current;
    for (size_t i = 0; i + 2 < tokens.size(); ++i) {
        // Match `fn` followed by an identifier (or ident :: ident for methods),
        // then `(`.  Collect param names from `name :` pairs inside `(...)`.
        if (tokens[i].type != TokenType::FN) continue;
        // Advance past 'fn'
        size_t j = i + 1;
        // Skip optional attributes like @hot etc. — skip anything that's not an IDENTIFIER or SCOPE
        while (j < tokens.size() && tokens[j].type == TokenType::AT) {
            // skip @attr(...) block
            j++; // skip '@'
            if (j < tokens.size() && tokens[j].type == TokenType::IDENTIFIER) j++; // attr name
            if (j < tokens.size() && tokens[j].type == TokenType::LPAREN) {
                int depth = 1; j++;
                while (j < tokens.size() && depth > 0) {
                    if (tokens[j].type == TokenType::LPAREN) depth++;
                    else if (tokens[j].type == TokenType::RPAREN) depth--;
                    j++;
                }
            }
        }
        if (j >= tokens.size() || tokens[j].type != TokenType::IDENTIFIER) continue;
        // Collect function name (may be  Name :: method)
        std::string fnName = tokens[j].lexeme;
        j++;
        while (j + 1 < tokens.size() && tokens[j].type == TokenType::SCOPE &&
               tokens[j+1].type == TokenType::IDENTIFIER) {
            fnName += "::" + tokens[j+1].lexeme;
            j += 2;
        }
        if (j >= tokens.size() || tokens[j].type != TokenType::LPAREN) continue;
        j++; // skip '('
        // Collect parameter names: `name :` patterns
        std::vector<std::string> params;
        int depth = 1;
        while (j < tokens.size() && depth > 0) {
            if (tokens[j].type == TokenType::RPAREN) {
                depth--;
                if (depth == 0) break;
                j++; continue;
            }
            if (tokens[j].type == TokenType::LPAREN) { depth++; j++; continue; }
            // A param name is an IDENTIFIER followed by COLON at depth == 1
            if (depth == 1 && tokens[j].type == TokenType::IDENTIFIER &&
                j + 1 < tokens.size() && tokens[j+1].type == TokenType::COLON) {
                params.push_back(tokens[j].lexeme);
            }
            j++;
        }
        if (!params.empty()) {
            funcParamNames_[fnName] = std::move(params);
        }
    }
    current = saved;
}

std::unique_ptr<Program> Parser::parse() {
    // Pre-scan to collect custom operator symbols so that multi-token infix
    // operators (e.g. "<=>" or "^><") are recognisable during expression parsing.
    prescanCustomOperators();
    // Pre-scan to collect function parameter names for named-argument support.
    prescanFunctionParams();

    std::vector<std::unique_ptr<FunctionDecl>> functions;
    std::vector<std::unique_ptr<EnumDecl>> enums;
    std::vector<std::unique_ptr<StructDecl>> structs;
    std::vector<std::unique_ptr<VarDecl>> globals;
    bool optMaxTagActive = false;
    bool fileNoAlias = false;

    while (!isAtEnd()) {
        if (match(TokenType::IMPORT)) {
            try {
                parseImport(functions, enums, structs, globals);
            } catch (const std::exception& e) {
                recordException(errors_, diagnostics_, e);
                synchronize();
            }
            continue;
        }
        if (match(TokenType::NAMESPACE)) {
            try {
                parseNamespace(functions, enums, structs, globals);
            } catch (const std::exception& e) {
                recordException(errors_, diagnostics_, e);
                synchronize();
            }
            continue;
        }
        if (check(TokenType::GLOBAL)) {
            try {
                auto gv = parseGlobalDecl();
                globals.push_back(std::move(gv));
            } catch (const std::exception& e) {
                recordException(errors_, diagnostics_, e);
                synchronize();
            }
            continue;
        }
        // Top-level `const NAME [: TYPE] = VALUE;` — shorthand for `global const`.
        // This is a common pattern; accept it to avoid confusing "Expected 'fn'" errors.
        if (check(TokenType::CONST)) {
            try {
                advance(); // consume 'const'
                const Token name = consume(TokenType::IDENTIFIER, "Expected variable name after 'const'");
                std::string typeName;
                if (match(TokenType::COLON)) {
                    typeName = parseTypeAnnotation();
                }
                std::unique_ptr<Expression> init = nullptr;
                if (match(TokenType::ASSIGN)) {
                    init = parseExpression();
                }
                consume(TokenType::SEMICOLON, "Expected ';' after const declaration");
                auto decl = std::make_unique<VarDecl>(name.lexeme, std::move(init), /*isConst=*/true, typeName);
                decl->isGlobal = true;
                decl->line = name.line;
                decl->column = name.column;
                globals.push_back(std::move(decl));
            } catch (const std::exception& e) {
                recordException(errors_, diagnostics_, e);
                synchronize();
            }
            continue;
        }
        if (match(TokenType::OPTMAX_START)) {
            if (optMaxTagActive) {
                error("Nested OPTMAX blocks are not allowed");
            }
            optMaxTagActive = true;
            continue;
        }
        if (match(TokenType::OPTMAX_END)) {
            if (!optMaxTagActive) {
                error("OPTMAX!: without a matching OPTMAX=:");
            }
            optMaxTagActive = false;
            continue;
        }
        if (match(TokenType::ENUM)) {
            try {
                enums.push_back(parseEnumDecl());
            } catch (const std::exception& e) {
                recordException(errors_, diagnostics_, e);
                synchronize();
            }
            continue;
        }
        // Top-level `comptime { ... }` — compile-time constant block.
        //
        // Supported statements inside the block:
        //   const NAME [: TYPE] = VALUE;        — define a comptime constant
        //   if (COND) { ... } [else if (...) { ... }]* [else { ... }]
        //                                        — conditional compilation
        //   error("message");                   — abort compilation with a message
        //   warning("message");                 — emit a diagnostic and continue
        //
        // VALUE may be an integer, float, string, boolean literal, or a reference
        // to another comptime constant already defined in this or an earlier block.
        //
        // COND may use: integer/string literals, comptime const names, the built-in
        // identifiers OS / ARCH / VERSION (matching __OS__ / __ARCH__ / __VERSION__
        // from the preprocessor), defined(NAME) to test whether a comptime const
        // exists, comparison operators (==, !=, <, <=, >, >=), and logical
        // operators (&&, ||, !).
        //
        // Every const definition that survives an active branch is also injected as
        // a global const VarDecl so the name is accessible inside function bodies.
        if (check(TokenType::COMPTIME)) {
            try {
                advance(); // consume 'comptime'

                // ── Detect `comptime if COND { ... }` shorthand ──────────────
                // This is syntactic sugar for `comptime { if (COND) { ... } }`.
                // The condition is evaluated without surrounding parentheses so
                // bare boolean flags work: `comptime if BUILD_DEBUG { ... }`.
                const bool isComptimeIf = check(TokenType::IF);
                if (!isComptimeIf) {
                    consume(TokenType::LBRACE, "Expected '{' after 'comptime'");
                }

                // ── comptime condition-value helper ──────────────────────────
                // A value in a condition expression — either a signed integer or
                // a string.  Boolean results are represented as i64 0/1.
                struct CVal {
                    bool isStr = false;
                    long long i = 0;
                    std::string s;
                    bool asBool() const {
                        return isStr ? !s.empty() : (i != 0);
                    }
                };

                // Retrieve a comptime value by name.  Checks the built-in
                // constants in priority order:
                //   OS, ARCH, VERSION  → string constants (platform-detected)
                //   FILE               → string constant (current source file path)
                //   comptimeConstants_ → user-defined / CLI-injected integer constants
                //   comptimeStrings_   → user-defined / CLI-injected string constants
                // Returns an empty optional if the name is not defined.
                auto getComptimeVar = [&](const std::string& name) -> std::optional<CVal> {
#if defined(_WIN32) || defined(_WIN64)
                    static constexpr const char* kOS = "windows";
#elif defined(__APPLE__)
                    static constexpr const char* kOS = "macos";
#else
                    static constexpr const char* kOS = "linux";
#endif
#if defined(__x86_64__) || defined(_M_X64)
                    static constexpr const char* kArch = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
                    static constexpr const char* kArch = "aarch64";
#elif defined(__arm__) || defined(_M_ARM)
                    static constexpr const char* kArch = "arm";
#else
                    static constexpr const char* kArch = "unknown";
#endif
                    if (name == "OS")
                        return CVal{true, 0, kOS};
                    if (name == "ARCH")
                        return CVal{true, 0, kArch};
                    if (name == "VERSION")
                        return CVal{true, 0, OMSC_VERSION};
                    if (name == "FILE")
                        return CVal{true, 0, currentFile_};
                    {
                        auto it = comptimeConstants_.find(name);
                        if (it != comptimeConstants_.end())
                            return CVal{false, it->second, ""};
                    }
                    {
                        auto it = comptimeStrings_.find(name);
                        if (it != comptimeStrings_.end())
                            return CVal{true, 0, it->second};
                    }
                    return std::nullopt;
                };

                // ── condition evaluator ──────────────────────────────────────
                // Precedence (low → high): || > && > comparison > primary
                //
                // primary: literal | identifier | defined(name) | (expr) | !primary
                // comparison: primary  (== | != | < | <= | > | >=)  primary
                // and-expr: comparison (&&  comparison)*
                // or-expr:  and-expr   (||  and-expr)*
                //
                // evalOr is declared first so that evalPrimary can capture it
                // by reference for parenthesised sub-expressions.
                std::function<bool()> evalOr;
                std::function<bool()> evalAnd;
                std::function<CVal()> evalCmp;
                std::function<CVal()> evalPrimary;

                evalPrimary = [&]() -> CVal {
                    // Logical NOT
                    if (match(TokenType::NOT)) {
                        CVal v = evalPrimary();
                        return CVal{false, v.asBool() ? 0LL : 1LL, ""};
                    }
                    // Parenthesised sub-expression
                    if (match(TokenType::LPAREN)) {
                        bool r = evalOr();
                        if (check(TokenType::RPAREN))
                            advance();
                        return CVal{false, r ? 1LL : 0LL, ""};
                    }
                    // String literal
                    if (check(TokenType::STRING)) {
                        return CVal{true, 0, advance().lexeme};
                    }
                    // Integer literal (optionally negated)
                    if (check(TokenType::MINUS)) {
                        advance();
                        if (check(TokenType::INTEGER)) {
                            return CVal{false, -(long long)advance().intValue, ""};
                        }
                        return CVal{false, 0, ""};
                    }
                    if (check(TokenType::INTEGER)) {
                        return CVal{false, (long long)advance().intValue, ""};
                    }
                    // Float literal — coerce to int for comptime comparisons
                    if (check(TokenType::FLOAT)) {
                        return CVal{false, (long long)advance().floatValue, ""};
                    }
                    // Boolean literals
                    if (match(TokenType::TRUE))
                        return CVal{false, 1, ""};
                    if (match(TokenType::FALSE))
                        return CVal{false, 0, ""};
                    // Identifier — comptime var ref or defined(name) predicate
                    if (check(TokenType::IDENTIFIER)) {
                        const std::string name = advance().lexeme;
                        // defined(NAME) — tests whether the name is recognised by
                        // getComptimeVar (covers built-ins, user constants, and
                        // constants pre-populated from -D CLI flags before parse()).
                        if (name == "defined" && check(TokenType::LPAREN)) {
                            advance(); // consume '('
                            std::string dname;
                            if (check(TokenType::IDENTIFIER))
                                dname = advance().lexeme;
                            if (check(TokenType::RPAREN))
                                advance(); // consume ')'
                            const bool isDef = getComptimeVar(dname).has_value();
                            return CVal{false, isDef ? 1LL : 0LL, ""};
                        }
                        auto v = getComptimeVar(name);
                        return v.value_or(CVal{false, 0, ""});
                    }
                    return CVal{false, 0, ""};
                };

                evalCmp = [&]() -> CVal {
                    CVal lhs = evalPrimary();
                    if (!check(TokenType::EQ) && !check(TokenType::NE) && !check(TokenType::LT) &&
                        !check(TokenType::LE) && !check(TokenType::GT) && !check(TokenType::GE)) {
                        return lhs;
                    }
                    const TokenType op = advance().type; // consume comparison operator
                    CVal rhs = evalPrimary();
                    bool result = false;
                    if (lhs.isStr && rhs.isStr) {
                        if (op == TokenType::EQ)
                            result = lhs.s == rhs.s;
                        else if (op == TokenType::NE)
                            result = lhs.s != rhs.s;
                        else if (op == TokenType::LT)
                            result = lhs.s < rhs.s;
                        else if (op == TokenType::LE)
                            result = lhs.s <= rhs.s;
                        else if (op == TokenType::GT)
                            result = lhs.s > rhs.s;
                        else if (op == TokenType::GE)
                            result = lhs.s >= rhs.s;
                    } else {
                        const long long l = lhs.isStr ? 0LL : lhs.i;
                        const long long r = rhs.isStr ? 0LL : rhs.i;
                        if (op == TokenType::EQ)
                            result = l == r;
                        else if (op == TokenType::NE)
                            result = l != r;
                        else if (op == TokenType::LT)
                            result = l < r;
                        else if (op == TokenType::LE)
                            result = l <= r;
                        else if (op == TokenType::GT)
                            result = l > r;
                        else if (op == TokenType::GE)
                            result = l >= r;
                    }
                    return CVal{false, result ? 1LL : 0LL, ""};
                };

                evalAnd = [&]() -> bool {
                    bool result = evalCmp().asBool();
                    while (check(TokenType::AND)) {
                        advance();
                        // Always evaluate the RHS (no short-circuit; this is
                        // a constant evaluator, so no side-effects).
                        bool rhs = evalCmp().asBool();
                        result = result && rhs;
                    }
                    return result;
                };

                evalOr = [&]() -> bool {
                    bool result = evalAnd();
                    while (check(TokenType::OR)) {
                        advance();
                        bool rhs = evalAnd();
                        result = result || rhs;
                    }
                    return result;
                };

                // ── body parser ───────────────────────────────────────────────
                // parseBody(active) processes comptime statements until '}'.
                // When active==false the branch is inactive (dead): constants
                // are NOT registered and VarDecls are NOT emitted, but the
                // tokens are consumed so the parser stays in sync.
                std::function<void(bool)> parseBody;
                parseBody = [&](bool active) {
                    while (!check(TokenType::RBRACE) && !isAtEnd()) {
                        // ── const declaration ──────────────────────────────
                        if (match(TokenType::CONST)) {
                            const Token cname = consume(TokenType::IDENTIFIER,
                                                        "Expected constant name after 'const' in comptime block");
                            std::string typeName;
                            if (match(TokenType::COLON)) {
                                typeName = parseTypeAnnotation();
                            }
                            consume(TokenType::ASSIGN, "Expected '=' in comptime const declaration");
                            // Determine the value's kind.
                            const bool neg = match(TokenType::MINUS);
                            if (check(TokenType::INTEGER)) {
                                const long long val =
                                    neg ? -(long long)advance().intValue : (long long)advance().intValue;
                                consume(TokenType::SEMICOLON, "Expected ';' after comptime const");
                                if (active) {
                                    comptimeConstants_[cname.lexeme] = val;
                                    const std::string tn = typeName.empty() ? "int" : typeName;
                                    auto initExpr = std::make_unique<LiteralExpr>(val);
                                    auto gv = std::make_unique<VarDecl>(cname.lexeme, std::move(initExpr),
                                                                        /*isConst=*/true, tn);
                                    gv->isCompilerGenerated = true;
                                    gv->line = cname.line;
                                    gv->column = cname.column;
                                    globals.push_back(std::move(gv));
                                }
                            } else if (check(TokenType::FLOAT)) {
                                const double fval = neg ? -advance().floatValue : advance().floatValue;
                                consume(TokenType::SEMICOLON, "Expected ';' after comptime const");
                                if (active) {
                                    // Store truncated integer for numeric
                                    // comptime-if comparisons.
                                    comptimeConstants_[cname.lexeme] = (long long)fval;
                                    const std::string tn = typeName.empty() ? "float" : typeName;
                                    auto initExpr = std::make_unique<LiteralExpr>(fval);
                                    auto gv = std::make_unique<VarDecl>(cname.lexeme, std::move(initExpr),
                                                                        /*isConst=*/true, tn);
                                    gv->isCompilerGenerated = true;
                                    gv->line = cname.line;
                                    gv->column = cname.column;
                                    globals.push_back(std::move(gv));
                                }
                            } else if (!neg && check(TokenType::STRING)) {
                                const std::string sval = advance().lexeme;
                                consume(TokenType::SEMICOLON, "Expected ';' after comptime const");
                                if (active) {
                                    comptimeStrings_[cname.lexeme] = sval;
                                    const std::string tn = typeName.empty() ? "string" : typeName;
                                    auto initExpr = std::make_unique<LiteralExpr>(sval);
                                    auto gv = std::make_unique<VarDecl>(cname.lexeme, std::move(initExpr),
                                                                        /*isConst=*/true, tn);
                                    gv->isCompilerGenerated = true;
                                    gv->line = cname.line;
                                    gv->column = cname.column;
                                    globals.push_back(std::move(gv));
                                }
                            } else if (!neg && match(TokenType::TRUE)) {
                                consume(TokenType::SEMICOLON, "Expected ';' after comptime const");
                                if (active) {
                                    comptimeConstants_[cname.lexeme] = 1;
                                    const std::string tn = typeName.empty() ? "bool" : typeName;
                                    auto initExpr = std::make_unique<LiteralExpr>(1LL);
                                    auto gv = std::make_unique<VarDecl>(cname.lexeme, std::move(initExpr),
                                                                        /*isConst=*/true, tn);
                                    gv->isCompilerGenerated = true;
                                    gv->line = cname.line;
                                    gv->column = cname.column;
                                    globals.push_back(std::move(gv));
                                }
                            } else if (!neg && match(TokenType::FALSE)) {
                                consume(TokenType::SEMICOLON, "Expected ';' after comptime const");
                                if (active) {
                                    comptimeConstants_[cname.lexeme] = 0;
                                    const std::string tn = typeName.empty() ? "bool" : typeName;
                                    auto initExpr = std::make_unique<LiteralExpr>(0LL);
                                    auto gv = std::make_unique<VarDecl>(cname.lexeme, std::move(initExpr),
                                                                        /*isConst=*/true, tn);
                                    gv->isCompilerGenerated = true;
                                    gv->line = cname.line;
                                    gv->column = cname.column;
                                    globals.push_back(std::move(gv));
                                }
                            } else if (!neg && check(TokenType::IDENTIFIER)) {
                                // Reference to an already-defined comptime const.
                                const std::string refName = advance().lexeme;
                                consume(TokenType::SEMICOLON, "Expected ';' after comptime const");
                                if (active) {
                                    const auto itI = comptimeConstants_.find(refName);
                                    const auto itS = comptimeStrings_.find(refName);
                                    auto bv = getComptimeVar(refName);
                                    if (bv && !bv->isStr) {
                                        // Integer or built-in numeric
                                        comptimeConstants_[cname.lexeme] = bv->i;
                                        const std::string tn = typeName.empty() ? "int" : typeName;
                                        auto initExpr = std::make_unique<LiteralExpr>(bv->i);
                                        auto gv = std::make_unique<VarDecl>(cname.lexeme, std::move(initExpr),
                                                                            /*isConst=*/true, tn);
                                        gv->isCompilerGenerated = true;
                                        gv->line = cname.line;
                                        gv->column = cname.column;
                                        globals.push_back(std::move(gv));
                                    } else if (bv && bv->isStr) {
                                        // String (user-defined or built-in
                                        // OS/ARCH/VERSION)
                                        comptimeStrings_[cname.lexeme] = bv->s;
                                        const std::string tn = typeName.empty() ? "string" : typeName;
                                        auto initExpr = std::make_unique<LiteralExpr>(bv->s);
                                        auto gv = std::make_unique<VarDecl>(cname.lexeme, std::move(initExpr),
                                                                            /*isConst=*/true, tn);
                                        gv->isCompilerGenerated = true;
                                        gv->line = cname.line;
                                        gv->column = cname.column;
                                        globals.push_back(std::move(gv));
                                    } else if (itI != comptimeConstants_.end()) {
                                        // Already checked via getComptimeVar but
                                        // keep as fallback for non-builtin ints.
                                        comptimeConstants_[cname.lexeme] = itI->second;
                                        const std::string tn = typeName.empty() ? "int" : typeName;
                                        auto initExpr = std::make_unique<LiteralExpr>(itI->second);
                                        auto gv = std::make_unique<VarDecl>(cname.lexeme, std::move(initExpr),
                                                                            /*isConst=*/true, tn);
                                        gv->isCompilerGenerated = true;
                                        gv->line = cname.line;
                                        gv->column = cname.column;
                                        globals.push_back(std::move(gv));
                                    } else if (itS != comptimeStrings_.end()) {
                                        comptimeStrings_[cname.lexeme] = itS->second;
                                        const std::string tn = typeName.empty() ? "string" : typeName;
                                        auto initExpr = std::make_unique<LiteralExpr>(itS->second);
                                        auto gv = std::make_unique<VarDecl>(cname.lexeme, std::move(initExpr),
                                                                            /*isConst=*/true, tn);
                                        gv->isCompilerGenerated = true;
                                        gv->line = cname.line;
                                        gv->column = cname.column;
                                        globals.push_back(std::move(gv));
                                    } else {
                                        errors_.push_back("comptime const '" + cname.lexeme +
                                                          "': undefined reference to comptime "
                                                          "constant '" +
                                                          refName + "'");
                                    }
                                }
                            } else {
                                // Unrecognised value form — skip to semicolon.
                                while (!check(TokenType::SEMICOLON) && !check(TokenType::RBRACE) && !isAtEnd())
                                    advance();
                                if (check(TokenType::SEMICOLON))
                                    advance();
                            }
                        }
                        // ── comptime if / else if / else ───────────────────
                        else if (match(TokenType::IF)) {
                            consume(TokenType::LPAREN, "Expected '(' after 'if' in comptime block");
                            const bool cond = evalOr();
                            consume(TokenType::RPAREN, "Expected ')' after condition in comptime if");
                            consume(TokenType::LBRACE, "Expected '{' after comptime if condition");
                            parseBody(active && cond);
                            consume(TokenType::RBRACE, "Expected '}' to close comptime if block");
                            bool handled = cond; // true branch was taken
                            while (check(TokenType::ELSE)) {
                                advance(); // consume 'else'
                                if (match(TokenType::IF)) {
                                    // else if (COND) { ... }
                                    consume(TokenType::LPAREN, "Expected '(' after 'else if' in "
                                                               "comptime block");
                                    const bool elseIfCond = evalOr();
                                    consume(TokenType::RPAREN, "Expected ')' after comptime else if "
                                                               "condition");
                                    consume(TokenType::LBRACE, "Expected '{' after comptime else if "
                                                               "condition");
                                    parseBody(active && !handled && elseIfCond);
                                    consume(TokenType::RBRACE, "Expected '}' to close comptime else if "
                                                               "block");
                                    if (elseIfCond)
                                        handled = true;
                                } else {
                                    // plain else { ... }
                                    consume(TokenType::LBRACE, "Expected '{' after 'else' in comptime "
                                                               "block");
                                    parseBody(active && !handled);
                                    consume(TokenType::RBRACE, "Expected '}' to close comptime else "
                                                               "block");
                                    break; // no more branches after else
                                }
                            }
                        }
                        // ── error("msg") / warning("msg") ─────────────────
                        else if (check(TokenType::IDENTIFIER) &&
                                 (tokens[current].lexeme == "error" || tokens[current].lexeme == "warning")) {
                            const std::string fname = advance().lexeme;
                            if (match(TokenType::LPAREN)) {
                                std::string msg;
                                if (check(TokenType::STRING))
                                    msg = advance().lexeme;
                                if (check(TokenType::RPAREN))
                                    advance();
                                if (check(TokenType::SEMICOLON))
                                    advance();
                                if (active) {
                                    if (fname == "error") {
                                        errors_.push_back("comptime error: " + msg);
                                    } else {
                                        warnings_.push_back("comptime warning: " + msg);
                                    }
                                }
                            } else {
                                while (!check(TokenType::SEMICOLON) && !check(TokenType::RBRACE) && !isAtEnd())
                                    advance();
                                if (check(TokenType::SEMICOLON))
                                    advance();
                            }
                        }
                        // ── skip anything else (future-proofing) ──────────
                        else {
                            while (!check(TokenType::SEMICOLON) && !check(TokenType::RBRACE) && !check(TokenType::IF) &&
                                   !check(TokenType::ELSE) && !check(TokenType::CONST) && !isAtEnd())
                                advance();
                            if (check(TokenType::SEMICOLON))
                                advance();
                        }
                    }
                };

                if (isComptimeIf) {
                    // ── `comptime if COND { ... } [else if COND { ... }]* [else { ... }]` ──
                    // Condition is evaluated without parentheses; body is the same as
                    // the `if (COND) { ... }` form inside a regular comptime block.
                    advance(); // consume 'if'
                    const bool cond = evalOr();
                    consume(TokenType::LBRACE, "Expected '{' after comptime if condition");
                    parseBody(cond);
                    consume(TokenType::RBRACE, "Expected '}' to close comptime if block");
                    bool handled = cond;
                    while (check(TokenType::ELSE)) {
                        advance(); // consume 'else'
                        if (match(TokenType::IF)) {
                            // else if COND { ... } — no parens required
                            const bool elseIfCond = evalOr();
                            consume(TokenType::LBRACE, "Expected '{' after comptime else if condition");
                            parseBody(!handled && elseIfCond);
                            consume(TokenType::RBRACE, "Expected '}' to close comptime else if block");
                            if (elseIfCond)
                                handled = true;
                        } else {
                            // plain else { ... }
                            consume(TokenType::LBRACE, "Expected '{' after 'else' in comptime if");
                            parseBody(!handled);
                            consume(TokenType::RBRACE, "Expected '}' to close comptime else block");
                            break;
                        }
                    }
                } else {
                    parseBody(/*active=*/true);
                    consume(TokenType::RBRACE, "Expected '}' to close comptime block");
                }
            } catch (const std::exception& e) {
                recordException(errors_, diagnostics_, e);
                synchronize();
            }
            continue;
        }
        // Top-level `type Alias = TypeExpr` — defines a type alias.
        if (check(TokenType::TYPE)) {
            try {
                advance(); // consume 'type'
                const Token aliasName = consume(TokenType::IDENTIFIER, "Expected alias name after 'type'");
                consume(TokenType::ASSIGN, "Expected '=' after type alias name");
                // `type X = struct { ... }` — inline struct type alias.
                // Parse the struct body using the alias name as the struct name.
                if (check(TokenType::STRUCT)) {
                    advance(); // consume 'struct'
                    auto sd = parseStructDecl(StructRepr::Auto, 0, aliasName.lexeme);
                    structs.push_back(std::move(sd));
                    // Also install the alias so that it resolves to itself (no-op alias).
                    typeAliases_[aliasName.lexeme] = aliasName.lexeme;
                    match(TokenType::SEMICOLON); // optional trailing semicolon
                    continue;
                }
                // Parse the right-hand side type using the full type annotation
                // parser so that *T, **T, (T1,T2), ptr<T>, fn(T)->R, etc. all work.
                if (!check(TokenType::IDENTIFIER) && !check(TokenType::STAR) &&
                    !check(TokenType::STAR_STAR) && !check(TokenType::LPAREN) &&
                    !check(TokenType::AMPERSAND) && !check(TokenType::FN)) {
                    error("Expected type name after '=' in type alias");
                }
                const std::string typeName = parseTypeAnnotation();
                consume(TokenType::SEMICOLON, "Expected ';' after type alias");
                typeAliases_[aliasName.lexeme] = typeName;
            } catch (const std::exception& e) {
                recordException(errors_, diagnostics_, e);
                synchronize();
            }
            continue;
        }
        if (match(TokenType::STRUCT)) {
            try {
                structs.push_back(parseStructDecl());
            } catch (const std::exception& e) {
                recordException(errors_, diagnostics_, e);
                synchronize();
            }
            continue;
        }
        // @repr(...) struct annotation — must appear immediately before 'struct'
        if (check(TokenType::AT) && current + 1 < tokens.size() && tokens[current + 1].type == TokenType::IDENTIFIER &&
            tokens[current + 1].lexeme == "repr") {
            try {
                advance(); // consume '@'
                advance(); // consume 'repr'
                consume(TokenType::LPAREN, "Expected '(' after @repr");
                StructRepr repr = StructRepr::Auto;
                int reprAlignN = 0;
                if (!check(TokenType::RPAREN)) {
                    const Token reprTok =
                        consume(TokenType::IDENTIFIER, "Expected repr kind: C, packed, auto, soa, or align");
                    if (reprTok.lexeme == "C") {
                        repr = StructRepr::C;
                    } else if (reprTok.lexeme == "packed") {
                        repr = StructRepr::Packed;
                    } else if (reprTok.lexeme == "auto") {
                        repr = StructRepr::Auto;
                    } else if (reprTok.lexeme == "soa") {
                        repr = StructRepr::SoA;
                    } else if (reprTok.lexeme == "align") {
                        repr = StructRepr::AlignN;
                        consume(TokenType::LPAREN, "Expected '(' after repr align");
                        const Token nTok = consume(TokenType::INTEGER, "Expected integer alignment in @repr(align(N))");
                        reprAlignN = static_cast<int>(nTok.intValue);
                        consume(TokenType::RPAREN, "Expected ')' after repr align value");
                    } else {
                        error("Unknown @repr kind '" + reprTok.lexeme + "'; supported: C, packed, auto, soa, align(N)");
                    }
                }
                consume(TokenType::RPAREN, "Expected ')' after @repr");
                consume(TokenType::STRUCT, "Expected 'struct' after @repr(...)");
                structs.push_back(parseStructDecl(repr, reprAlignN));
            } catch (const std::exception& e) {
                recordException(errors_, diagnostics_, e);
                synchronize();
            }
            continue;
        }
        // @packed — shorthand for @repr(packed), must appear immediately before 'struct'
        if (check(TokenType::AT) && current + 1 < tokens.size() && tokens[current + 1].type == TokenType::IDENTIFIER &&
            tokens[current + 1].lexeme == "packed") {
            try {
                advance(); // consume '@'
                advance(); // consume 'packed'
                consume(TokenType::STRUCT, "Expected 'struct' after @packed");
                structs.push_back(parseStructDecl(StructRepr::Packed, 0));
            } catch (const std::exception& e) {
                recordException(errors_, diagnostics_, e);
                synchronize();
            }
            continue;
        }
        try {
            // Check for file-level @noalias directive (appears before any function
            // keyword and is not followed by 'fn').
            if (check(TokenType::AT) && !fileNoAlias) {
                const size_t savedPos = current;
                // Peek ahead: if it's @noalias followed by a newline/struct/enum
                // (not 'fn'), treat it as a file-level directive.
                advance(); // consume '@'
                if (check(TokenType::IDENTIFIER) && peek().lexeme == "noalias") {
                    advance(); // consume 'noalias'
                    // Check if the next thing is NOT a function-related token
                    if (check(TokenType::AT) || check(TokenType::FN) || check(TokenType::STRUCT) ||
                        check(TokenType::ENUM) || check(TokenType::OPTMAX_START) || check(TokenType::IMPORT) ||
                        isAtEnd()) {
                        // This is a file-level @noalias directive
                        fileNoAlias = true;
                        continue;
                    }
                }
                // Not a file-level directive, restore position
                current = savedPos;
            }
            // Parse optional function annotations: @inline, @noinline, @cold,
            // @hot, @pure, @noreturn, @static, @flatten, @unroll, @nounroll,
            // @restrict, @vectorize, @novectorize, @minsize, @optnone, @nounwind,
            // @deprecated, @deprecated("msg")
            bool hintInline = false, hintNoInline = false, hintCold = false;
            bool hintHot = false, hintPure = false, hintNoReturn = false;
            bool hintStatic = false, hintFlatten = false;
            bool hintUnroll = false, hintNoUnroll = false;
            bool hintRestrict = false;
            bool hintVectorize = false, hintNoVectorize = false;
            bool hintParallelize = false, hintNoParallelize = false;
            bool hintMinSize = false, hintOptNone = false, hintNoUnwind = false;
            bool hintConstEval = false;
            bool hintSpeculatable = false;
            bool hintWillReturn = false;
            bool hintNoSync = false;
            bool hintNoFree = false;
            int hintAlign = 0;
            bool isOptMaxFromAnnotation = false;
            OptMaxConfig optMaxCfgFromAnnotation;
            int allocatorSizeParam = -1;
            int allocatorCountParam = -1;
            FunctionDecl::MemoryEffect hintMemoryEffect = FunctionDecl::MemoryEffect::Default;
            bool hintNoAliasReturn = false;
            bool hintDeprecated = false;
            std::string deprecatedMsg;
            while (check(TokenType::AT)) {
                advance(); // consume '@'
                const Token ann = consume(TokenType::IDENTIFIER, "Expected annotation name after '@'");

                // ── Compound annotation forms (canonical) ────────────────────
                // @opt(key, key, align=N, ...)   — optimization hints
                // @semantics(key, key, ...)       — semantic/ABI attributes
                // @memory(allocator, size=N, ...) — memory-model attributes
                if (ann.lexeme == "opt") {
                    consume(TokenType::LPAREN, "Expected '(' after @opt");
                    while (!check(TokenType::RPAREN) && !isAtEnd()) {
                        const Token key = consume(TokenType::IDENTIFIER, "Expected option name in @opt(...)");
                        if (key.lexeme == "inline")
                            hintInline = true;
                        else if (key.lexeme == "noinline")
                            hintNoInline = true;
                        else if (key.lexeme == "hot")
                            hintHot = true;
                        else if (key.lexeme == "cold")
                            hintCold = true;
                        else if (key.lexeme == "vectorize")
                            hintVectorize = true;
                        else if (key.lexeme == "novectorize")
                            hintNoVectorize = true;
                        else if (key.lexeme == "unroll")
                            hintUnroll = true;
                        else if (key.lexeme == "nounroll")
                            hintNoUnroll = true;
                        else if (key.lexeme == "parallel")
                            hintParallelize = true;
                        else if (key.lexeme == "noparallel")
                            hintNoParallelize = true;
                        else if (key.lexeme == "flatten")
                            hintFlatten = true;
                        else if (key.lexeme == "minsize")
                            hintMinSize = true;
                        else if (key.lexeme == "optnone")
                            hintOptNone = true;
                        else if (key.lexeme == "align") {
                            if (match(TokenType::ASSIGN)) {
                                // align=AUTO  →  cache-line optimal (same as bare align)
                                // align=N     →  explicit power-of-two alignment
                                if (check(TokenType::IDENTIFIER)) {
                                    const Token v = advance();
                                    if (v.lexeme == "AUTO" || v.lexeme == "auto") {
                                        hintAlign = -1;
                                    } else {
                                        error("Expected integer or AUTO after align= in @opt;"
                                              " got '" +
                                              v.lexeme + "'");
                                    }
                                } else {
                                    const Token v =
                                        consume(TokenType::INTEGER, "Expected integer or AUTO after align= in @opt");
                                    hintAlign = static_cast<int>(v.intValue);
                                }
                            } else {
                                hintAlign = -1; // bare align — auto / cache-line
                            }
                        } else {
                            error("Unknown option '" + key.lexeme +
                                  "' in @opt(...);"
                                  " valid: inline, noinline, hot, cold, vectorize,"
                                  " novectorize, unroll, nounroll, parallel, noparallel,"
                                  " flatten, minsize, optnone, align, align=N, align=AUTO");
                        }
                        if (!check(TokenType::RPAREN))
                            match(TokenType::COMMA);
                    }
                    consume(TokenType::RPAREN, "Expected ')' after @opt options");

                } else if (ann.lexeme == "semantics") {
                    consume(TokenType::LPAREN, "Expected '(' after @semantics");
                    while (!check(TokenType::RPAREN) && !isAtEnd()) {
                        const Token key = consume(TokenType::IDENTIFIER, "Expected attribute in @semantics(...)");
                        if (key.lexeme == "pure")
                            hintPure = true;
                        else if (key.lexeme == "speculatable")
                            hintSpeculatable = true;
                        else if (key.lexeme == "noreturn")
                            hintNoReturn = true;
                        else if (key.lexeme == "nounwind")
                            hintNoUnwind = true;
                        else if (key.lexeme == "restrict")
                            hintRestrict = true;
                        else if (key.lexeme == "noalias")
                            hintRestrict = true;
                        else if (key.lexeme == "const_eval")
                            hintConstEval = true;
                        else if (key.lexeme == "willreturn")
                            hintWillReturn = true;
                        else if (key.lexeme == "nosync")
                            hintNoSync = true;
                        else if (key.lexeme == "nofree")
                            hintNoFree = true;
                        else {
                            error("Unknown attribute '" + key.lexeme +
                                  "' in @semantics(...);"
                                  " valid: pure, speculatable, noreturn, nounwind,"
                                  " restrict, noalias, const_eval,"
                                  " willreturn, nosync, nofree");
                        }
                        if (!check(TokenType::RPAREN))
                            match(TokenType::COMMA);
                    }
                    consume(TokenType::RPAREN, "Expected ')' after @semantics attributes");

                } else if (ann.lexeme == "memory") {
                    consume(TokenType::LPAREN, "Expected '(' after @memory");
                    while (!check(TokenType::RPAREN) && !isAtEnd()) {
                        const Token key = consume(TokenType::IDENTIFIER, "Expected option in @memory(...)");
                        if (key.lexeme == "allocator") {
                            // `allocator` enables the allocator attribute with default
                            // size-param index 0.  A subsequent `size=N` in the same
                            // annotation overrides the index; `size=N` appearing BEFORE
                            // `allocator` in the list is also respected (the guard keeps
                            // the already-set value).
                            if (allocatorSizeParam < 0)
                                allocatorSizeParam = 0;
                        } else if (key.lexeme == "size") {
                            consume(TokenType::ASSIGN, "Expected '=' after size in @memory");
                            const Token v = consume(TokenType::INTEGER, "Expected integer after size= in @memory");
                            allocatorSizeParam = static_cast<int>(v.intValue);
                        } else if (key.lexeme == "count") {
                            consume(TokenType::ASSIGN, "Expected '=' after count in @memory");
                            const Token v = consume(TokenType::INTEGER, "Expected integer after count= in @memory");
                            allocatorCountParam = static_cast<int>(v.intValue);

                            // ── Memory-access level options (mutually exclusive) ───
                        } else if (key.lexeme == "none") {
                            hintMemoryEffect = FunctionDecl::MemoryEffect::None;
                        } else if (key.lexeme == "readonly") {
                            hintMemoryEffect = FunctionDecl::MemoryEffect::ReadOnly;
                        } else if (key.lexeme == "writeonly") {
                            hintMemoryEffect = FunctionDecl::MemoryEffect::WriteOnly;
                        } else if (key.lexeme == "readwrite") {
                            hintMemoryEffect = FunctionDecl::MemoryEffect::ReadWrite;
                        } else if (key.lexeme == "argmem_ro") {
                            hintMemoryEffect = FunctionDecl::MemoryEffect::ArgMemRO;
                        } else if (key.lexeme == "argmem") {
                            hintMemoryEffect = FunctionDecl::MemoryEffect::ArgMem;
                        } else if (key.lexeme == "inaccessiblemem_or_argmem") {
                            hintMemoryEffect = FunctionDecl::MemoryEffect::InaccessibleOrArgMem;
                        } else if (key.lexeme == "inaccessiblemem") {
                            hintMemoryEffect = FunctionDecl::MemoryEffect::InaccessibleMem;

                            // ── Aliasing hint ──────────────────────────────────────
                        } else if (key.lexeme == "noalias_ret") {
                            hintNoAliasReturn = true;

                        } else {
                            error("Unknown option '" + key.lexeme +
                                  "' in @memory(...);\n"
                                  "  Allocator : allocator, size=N, count=M\n"
                                  "  Access    : none, readonly, writeonly, readwrite\n"
                                  "              argmem, argmem_ro\n"
                                  "              inaccessiblemem, inaccessiblemem_or_argmem\n"
                                  "  Aliasing  : noalias_ret");
                        }
                        if (!check(TokenType::RPAREN))
                            match(TokenType::COMMA);
                    }
                    consume(TokenType::RPAREN, "Expected ')' after @memory options");

                    // ── Non-compound annotations kept as-is ──────────────────────
                } else if (ann.lexeme == "static") {
                    hintStatic = true;
                } else if (ann.lexeme == "deprecated") {
                    hintDeprecated = true;
                    // Accept optional string message: @deprecated("msg")
                    if (check(TokenType::LPAREN)) {
                        advance(); // consume '('
                        if (check(TokenType::STRING)) {
                            deprecatedMsg = advance().lexeme;
                        }
                        consume(TokenType::RPAREN, "Expected ')' after @deprecated message");
                    }
                } else if (ann.lexeme == "optmax") {
                    isOptMaxFromAnnotation = true;
                    if (check(TokenType::LPAREN)) {
                        optMaxCfgFromAnnotation = parseOptMaxConfig();
                    }

                    // ── Deprecated flat forms — accepted but emit a warning ───────
                    // Use @opt(...), @semantics(...), or @memory(...) instead.
                } else if (ann.lexeme == "inline") {
                    hintInline = true;
                    warnings_.push_back("warning: '@inline' is deprecated;"
                                        " use '@opt(inline)' instead");
                } else if (ann.lexeme == "noinline") {
                    hintNoInline = true;
                    warnings_.push_back("warning: '@noinline' is deprecated;"
                                        " use '@opt(noinline)' instead");
                } else if (ann.lexeme == "hot") {
                    hintHot = true;
                    warnings_.push_back("warning: '@hot' is deprecated;"
                                        " use '@opt(hot)' instead");
                } else if (ann.lexeme == "cold") {
                    hintCold = true;
                    warnings_.push_back("warning: '@cold' is deprecated;"
                                        " use '@opt(cold)' instead");
                } else if (ann.lexeme == "flatten") {
                    hintFlatten = true;
                    warnings_.push_back("warning: '@flatten' is deprecated;"
                                        " use '@opt(flatten)' instead");
                } else if (ann.lexeme == "unroll") {
                    hintUnroll = true;
                    warnings_.push_back("warning: '@unroll' is deprecated;"
                                        " use '@opt(unroll)' instead");
                } else if (ann.lexeme == "nounroll") {
                    hintNoUnroll = true;
                    warnings_.push_back("warning: '@nounroll' is deprecated;"
                                        " use '@opt(nounroll)' instead");
                } else if (ann.lexeme == "vectorize") {
                    hintVectorize = true;
                    warnings_.push_back("warning: '@vectorize' is deprecated;"
                                        " use '@opt(vectorize)' instead");
                } else if (ann.lexeme == "novectorize") {
                    hintNoVectorize = true;
                    warnings_.push_back("warning: '@novectorize' is deprecated;"
                                        " use '@opt(novectorize)' instead");
                } else if (ann.lexeme == "parallel") {
                    hintParallelize = true;
                    warnings_.push_back("warning: '@parallel' is deprecated;"
                                        " use '@opt(parallel)' instead");
                } else if (ann.lexeme == "noparallel") {
                    hintNoParallelize = true;
                    warnings_.push_back("warning: '@noparallel' is deprecated;"
                                        " use '@opt(noparallel)' instead");
                } else if (ann.lexeme == "minsize") {
                    hintMinSize = true;
                    warnings_.push_back("warning: '@minsize' is deprecated;"
                                        " use '@opt(minsize)' instead");
                } else if (ann.lexeme == "optnone") {
                    hintOptNone = true;
                    warnings_.push_back("warning: '@optnone' is deprecated;"
                                        " use '@opt(optnone)' instead");
                } else if (ann.lexeme == "align") {
                    consume(TokenType::LPAREN, "Expected '(' after @align");
                    if (check(TokenType::RPAREN)) {
                        hintAlign = -1;
                    } else {
                        const Token alignVal = consume(TokenType::INTEGER, "Expected integer alignment value");
                        hintAlign = static_cast<int>(alignVal.intValue);
                    }
                    consume(TokenType::RPAREN, "Expected ')' after @align value");
                    warnings_.push_back("warning: '@align(N)' is deprecated;"
                                        " use '@opt(align=N)' or '@opt(align)' instead");
                } else if (ann.lexeme == "pure") {
                    hintPure = true;
                    warnings_.push_back("warning: '@pure' is deprecated;"
                                        " use '@semantics(pure)' instead");
                } else if (ann.lexeme == "speculatable") {
                    hintSpeculatable = true;
                    warnings_.push_back("warning: '@speculatable' is deprecated;"
                                        " use '@semantics(speculatable)' instead");
                } else if (ann.lexeme == "noreturn") {
                    hintNoReturn = true;
                    warnings_.push_back("warning: '@noreturn' is deprecated;"
                                        " use '@semantics(noreturn)' instead");
                } else if (ann.lexeme == "nounwind") {
                    hintNoUnwind = true;
                    warnings_.push_back("warning: '@nounwind' is deprecated;"
                                        " use '@semantics(nounwind)' instead");
                } else if (ann.lexeme == "restrict") {
                    hintRestrict = true;
                    warnings_.push_back("warning: '@restrict' is deprecated;"
                                        " use '@semantics(restrict)' instead");
                } else if (ann.lexeme == "noalias") {
                    hintRestrict = true;
                    warnings_.push_back("warning: '@noalias' is deprecated;"
                                        " use '@semantics(noalias)' instead");
                } else if (ann.lexeme == "const_eval") {
                    hintConstEval = true;
                    warnings_.push_back("warning: '@const_eval' is deprecated;"
                                        " use '@semantics(const_eval)' instead");
                } else if (ann.lexeme == "allocator") {
                    consume(TokenType::LPAREN, "Expected '(' after @allocator");
                    while (!check(TokenType::RPAREN) && !isAtEnd()) {
                        const Token paramKey = consume(TokenType::IDENTIFIER, "Expected param name in @allocator");
                        consume(TokenType::ASSIGN, "Expected '=' in @allocator");
                        const Token paramVal = advance();
                        int idx = 0;
                        if (paramVal.type == TokenType::INTEGER)
                            idx = static_cast<int>(paramVal.intValue);
                        if (paramKey.lexeme == "size")
                            allocatorSizeParam = idx;
                        else if (paramKey.lexeme == "count")
                            allocatorCountParam = idx;
                        if (!check(TokenType::RPAREN))
                            match(TokenType::COMMA);
                    }
                    consume(TokenType::RPAREN, "Expected ')' after @allocator params");
                    warnings_.push_back("warning: '@allocator(...)' is deprecated;"
                                        " use '@memory(allocator, size=N)' instead");
                } else {
                    error("Unknown function annotation '@" + ann.lexeme +
                          "';\n"
                          "  Use compound forms: @opt(...), @semantics(...), @memory(...)\n"
                          "  @opt    : inline, noinline, hot, cold, vectorize, novectorize,\n"
                          "            unroll, nounroll, parallel, noparallel, flatten,\n"
                          "            minsize, optnone, align, align=N, align=AUTO\n"
                          "  @semantics: pure, speculatable, noreturn, nounwind,\n"
                          "              restrict, noalias, const_eval,\n"
                          "              willreturn, nosync, nofree\n"
                          "  @memory : none, readonly, writeonly, readwrite,\n"
                          "            argmem, argmem_ro,\n"
                          "            inaccessiblemem, inaccessiblemem_or_argmem,\n"
                          "            noalias_ret,\n"
                          "            allocator, size=N, count=M\n"
                          "  Other   : @static, @optmax, @optmax(...), @deprecated, @deprecated(\"msg\")");
                }
            }
            auto func = parseFunction(optMaxTagActive || isOptMaxFromAnnotation);
            func->hintInline = hintInline;
            func->hintNoInline = hintNoInline;
            func->hintCold = hintCold;
            func->hintHot = hintHot;
            func->hintPure = hintPure;
            func->hintNoReturn = hintNoReturn;
            func->hintStatic = hintStatic;
            func->hintFlatten = hintFlatten;
            func->hintUnroll = hintUnroll;
            func->hintNoUnroll = hintNoUnroll;
            func->hintRestrict = hintRestrict;
            func->hintVectorize = hintVectorize;
            func->hintNoVectorize = hintNoVectorize;
            func->hintParallelize = hintParallelize;
            func->hintNoParallelize = hintNoParallelize;
            func->hintMinSize = hintMinSize;
            func->hintOptNone = hintOptNone;
            func->hintNoUnwind = hintNoUnwind;
            func->hintConstEval = hintConstEval;
            func->hintSpeculatable = hintSpeculatable;
            func->hintWillReturn = hintWillReturn;
            func->hintNoSync = hintNoSync;
            func->hintNoFree = hintNoFree;
            func->hintAlign = hintAlign;
            func->allocatorSizeParam = allocatorSizeParam;
            func->allocatorCountParam = allocatorCountParam;
            func->hintMemoryEffect = hintMemoryEffect;
            func->hintNoAliasReturn = hintNoAliasReturn;
            func->hintDeprecated = hintDeprecated;
            func->deprecatedMsg = std::move(deprecatedMsg);
            if (isOptMaxFromAnnotation) {
                func->isOptMax = true;
                func->optMaxConfig = optMaxCfgFromAnnotation;
                func->optMaxConfig.enabled = true;
            }
            // Warn about conflicting annotations at parse time.
            // optnone is the strongest suppressor — nothing else is meaningful with it.
            if (hintOptNone && hintInline) {
                warnings_.push_back("warning: '@opt(optnone)' and '@opt(inline)' are mutually exclusive on function '" +
                                    func->name + "' — inline will be ignored (optnone requires noinline)");
            }
            if (hintOptNone && hintHot) {
                warnings_.push_back("warning: '@opt(optnone)' disables all optimizations on function '" + func->name +
                                    "' — hot hint will have no effect");
            }
            // Paired contradictions.
            if (hintHot && hintCold) {
                warnings_.push_back("warning: '@opt(hot)' and '@opt(cold)' are contradictory on function '" +
                                    func->name + "' — hot takes precedence");
            }
            if (hintInline && hintNoInline) {
                warnings_.push_back("warning: '@opt(inline)' and '@opt(noinline)' are contradictory on function '" +
                                    func->name + "' — noinline takes precedence");
            }
            if (hintVectorize && hintNoVectorize) {
                warnings_.push_back(
                    "warning: '@opt(vectorize)' and '@opt(novectorize)' are contradictory on function '" + func->name +
                    "' — novectorize takes precedence");
            }
            if (hintUnroll && hintNoUnroll) {
                warnings_.push_back("warning: '@opt(unroll)' and '@opt(nounroll)' are contradictory on function '" +
                                    func->name + "' — nounroll takes precedence");
            }
            if (hintParallelize && hintNoParallelize) {
                warnings_.push_back("warning: '@opt(parallel)' and '@opt(noparallel)' are contradictory on function '" +
                                    func->name + "' — noparallel takes precedence");
            }
            // @semantics(willreturn) and @semantics(noreturn) are contradictory.
            if (hintWillReturn && hintNoReturn) {
                warnings_.push_back(
                    "warning: '@semantics(willreturn)' and '@semantics(noreturn)' are contradictory on function '" +
                    func->name + "' — noreturn takes precedence");
                hintWillReturn = false; // noreturn wins
            }
            // @semantics(pure) implies memory(none) or memory(read) — warn if
            // an explicit @memory access level contradicts that contract.
            using ME = FunctionDecl::MemoryEffect;
            if (hintPure && (hintMemoryEffect == ME::WriteOnly || hintMemoryEffect == ME::ArgMem ||
                             hintMemoryEffect == ME::ReadWrite)) {
                warnings_.push_back("warning: '@semantics(pure)' and '@memory(" +
                                    std::string(hintMemoryEffect == ME::WriteOnly ? "writeonly"
                                                : hintMemoryEffect == ME::ArgMem  ? "argmem"
                                                                                  : "readwrite") +
                                    ")' are contradictory on function '" + func->name +
                                    "' — pure implies readonly or no-memory access");
            }
            // allocator + none is a contradiction (allocator must write memory).
            if (func->allocatorSizeParam >= 0 && hintMemoryEffect == ME::None) {
                warnings_.push_back("warning: '@memory(allocator)' and '@memory(none)' are"
                                    " contradictory on function '" +
                                    func->name + "' — an allocator must write memory");
            }
            functions.push_back(std::move(func));
        } catch (const std::exception& e) {
            recordException(errors_, diagnostics_, e);
            synchronize();
        }
    }

    if (optMaxTagActive) {
        errors_.push_back("Parse error: Unterminated OPTMAX block");
    }

    if (!errors_.empty()) {
        // When there is exactly one structured diagnostic, re-throw it directly so
        // that its raw message and source location are preserved without the
        // "error at line X: ..." prefix that format() bakes into the combined string.
        if (diagnostics_.size() == 1) {
            throw DiagnosticError(diagnostics_.front());
        }

        // Multiple errors: throw a MultiDiagnosticError so the driver can emit
        // each one individually with full source-location and snippet support.
        if (diagnostics_.size() > 1) {
            throw MultiDiagnosticError(diagnostics_);
        }

        // Fallback (plain-string errors without structured diagnostics).
        std::string combined;
        for (size_t i = 0; i < errors_.size(); ++i) {
            if (i > 0)
                combined += "\n";
            combined += errors_[i];
        }
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {}, combined});
    }

    // Append generated lambda functions to the program
    for (auto& lf : lambdaFunctions_) {
        functions.push_back(std::move(lf));
    }
    lambdaFunctions_.clear();

    // Drain pending globals from imports
    for (auto& pg : pendingGlobals_) {
        globals.push_back(std::move(pg));
    }
    pendingGlobals_.clear();

    // ── Transitive type alias resolution ─────────────────────────────────────
    // Replace every alias value that itself names another alias, up to 32 hops.
    // This turns  type A = B; type B = int;  into  A → "int", B → "int".
    // If a cycle is detected the original value is kept (the parser already
    // rejected it as an unknown annotation during parsing).
    for (auto& entry : typeAliases_) {
        std::string resolved = entry.second;
        for (int hop = 0; hop < kMaxTypeAliasHops; ++hop) {
            auto it = typeAliases_.find(resolved);
            if (it == typeAliases_.end()) break;
            if (it->second == resolved) break; // trivial self-cycle guard
            resolved = it->second;
        }
        entry.second = resolved;
    }

    return std::make_unique<Program>(std::move(functions), std::move(enums), std::move(structs), fileNoAlias,
                                     std::move(globals), globallyImportedNamespaces_,
                                     typeAliases_);
}

void Parser::parseImport(std::vector<std::unique_ptr<FunctionDecl>>& functions,
                         std::vector<std::unique_ptr<EnumDecl>>& enums,
                         std::vector<std::unique_ptr<StructDecl>>& structs,
                         std::vector<std::unique_ptr<VarDecl>>& /*globals*/) {
    // `import std;` (identifier form) — explicitly import a built-in namespace so
    // its functions are accessible without the `std::` qualifier.  The `std`
    // namespace is always registered; this directive marks it as globally imported
    // so the caller knows the import was intentional.  Any other identifier is
    // treated as an error since only the built-in `std` namespace is supported.
    if (check(TokenType::IDENTIFIER)) {
        const Token nsTok = advance();
        // Support  import std;  and  import std as alias;
        std::string alias = nsTok.lexeme;
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "as") {
            advance(); // consume 'as'
            const Token aliasTok = consume(TokenType::IDENTIFIER, "Expected alias name after 'as'");
            alias = aliasTok.lexeme;
        }
        consume(TokenType::SEMICOLON, "Expected ';' after namespace import");
        // All std functions are already in importNamespaces_["std"] from
        // registerStdNamespace().  Marking as globally imported means we also
        // inject them into the *direct* (no-qualifier) function namespace so that
        // e.g. `println(x)` resolves even without `std::`.  Since stdlib builtins
        // are always in the builtin lookup table, this is a no-op for them, but
        // for any namespace defined by user code it would expose all members.
        auto nsIt = importNamespaces_.find(nsTok.lexeme);
        if (nsIt != importNamespaces_.end()) {
            globallyImportedNamespaces_.insert(nsTok.lexeme);
            // For user-defined namespaces, inject bare function/struct aliases so
            // that `add(x)` resolves to `Math::add` after `import Math;`.
            // For stdlib ("std"), builtins are already accessible bare; skip.
            if (nsTok.lexeme != "std") {
                for (const auto& [shortName, qualName] : nsIt->second) {
                    bareImportedNames_[shortName] = qualName;
                    // If the qualified name is also a struct name, expose bare name too.
                    if (structNames_.count(qualName)) {
                        structNames_.insert(shortName);
                    }
                    // If the qualified name is also an enum name, expose bare enum alias too.
                    if (enumNames_.count(qualName)) {
                        bareEnumNames_[shortName] = qualName;
                    }
                }
            }
        }
        // No further action needed — stdlib functions work bare by default.
        return;
    }

    const Token fileToken = consume(TokenType::STRING, "Expected filename string after 'import'");

    // Optional alias: import "file" as alias
    // The alias may itself be multi-level: import "file" as john::int
    // which allows calling imported functions as john::int::funcname().
    std::string alias;
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "as") {
        advance(); // consume 'as'
        const Token aliasTok = consume(TokenType::IDENTIFIER, "Expected alias name after 'as'");
        alias = aliasTok.lexeme;
        // Support multi-level alias: as john::int  →  internal key "john::int"
        while (check(TokenType::SCOPE)) {
            advance(); // consume '::'
            const Token seg = consume(TokenType::IDENTIFIER, "Expected identifier in alias path");
            alias += "::" + seg.lexeme;
        }
    }

    consume(TokenType::SEMICOLON, "Expected ';' after import statement");

    std::string filename = fileToken.lexeme;
    // Append .om extension if not already present
    if (filename.size() < 3 || filename.compare(filename.size() - 3, 3, ".om") != 0) {
        filename += ".om";
    }

    // Resolve full path relative to the base directory
    std::string fullPath;
    if (baseDir_.empty()) {
        fullPath = filename;
    } else {
        fullPath = baseDir_ + "/" + filename;
    }

    // Normalize the path to a canonical form for circular import detection
    std::error_code ec;
    auto canonicalPath = std::filesystem::weakly_canonical(fullPath, ec);
    const std::string normalizedPath = ec ? fullPath : canonicalPath.string();

    // Check for circular/duplicate imports
    if (importedFiles_->count(normalizedPath)) {
        return; // Already imported, skip
    }
    importedFiles_->insert(normalizedPath);

    // Read the imported file
    std::ifstream file(fullPath);
    if (!file.is_open()) {
        error("Cannot open imported file: " + fullPath);
    }
    std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Lex the imported file
    Lexer importLexer(std::move(source));
    std::vector<Token> importTokens = importLexer.tokenize();

    // Parse the imported file
    Parser importParser(std::move(importTokens));
    const std::string importDir = std::filesystem::path(fullPath).parent_path().string();
    importParser.setBaseDir(importDir);
    importParser.importedFiles_ = importedFiles_; // Share import tracking

    auto importedProgram = importParser.parse();
    // Propagate warnings from the imported file back to this parser so they
    // are all visible to the caller without requiring stderr.
    for (const auto& w : importParser.warnings()) {
        warnings_.push_back(w);
    }

    // Merge imported declarations into the current program.
    // When an alias was given, register every imported function in the
    // namespace map so that alias::funcname(args) resolves directly to the
    // original function — no forwarder overhead, and the compiler can detect
    // typos in function names at parse time.
    // The original (unqualified) function is also kept so intra-module calls
    // inside the imported file continue to resolve correctly.
    for (auto& fn : importedProgram->functions) {
        if (!alias.empty()) {
            importNamespaces_[alias][fn->name] = fn->name;
        }
        functions.push_back(std::move(fn));
    }
    for (auto& en : importedProgram->enums) {
        enums.push_back(std::move(en));
    }
    for (auto& st : importedProgram->structs) {
        structs.push_back(std::move(st));
    }

    // Also import struct names so struct literals can be parsed
    for (const auto& name : importParser.structNames_) {
        structNames_.insert(name);
    }
    // Import enum names for scope resolution
    for (const auto& name : importParser.enumNames_) {
        enumNames_.insert(name);
    }
    // Import bare enum name aliases
    for (const auto& [shortName, qualName] : importParser.bareEnumNames_) {
        bareEnumNames_[shortName] = qualName;
    }

    // Import global variables: derive the alias stem if none was given.
    std::string globalAlias = alias;
    if (globalAlias.empty()) {
        // Derive stem from the filename (no directory, no extension)
        // e.g. "math.om" → "math",  "lib/utils.om" → "utils"
        globalAlias = std::filesystem::path(filename).stem().string();
    }
    for (auto& gv : importedProgram->globals) {
        if (!gv)
            continue;
        // No mangling: the global keeps its original name as the LLVM symbol.
        // The namespace registry (importedGlobalVars_) is the only level of
        // indirection; the actual LLVM global is just named by gv->name.
        importedGlobalVars_[globalAlias][gv->name] = gv->name;
        gv->globalNamespace = globalAlias;
        pendingGlobals_.push_back(std::move(gv));
    }
}

void Parser::parseNamespace(std::vector<std::unique_ptr<FunctionDecl>>& functions,
                            std::vector<std::unique_ptr<EnumDecl>>& enums,
                            std::vector<std::unique_ptr<StructDecl>>& structs,
                            std::vector<std::unique_ptr<VarDecl>>& globals,
                            const std::string& nsPrefix) {
    const Token nsNameTok = consume(TokenType::IDENTIFIER, "Expected namespace name");
    const std::string nsName = nsPrefix.empty() ? nsNameTok.lexeme
                                                 : nsPrefix + "::" + nsNameTok.lexeme;
    consume(TokenType::LBRACE, "Expected '{' after namespace name");

    auto& nsMap = importNamespaces_[nsName];

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (check(TokenType::FN)) {
            auto func = parseFunction(false);
            // Prefix the function name with the namespace.
            const std::string shortName = func->name;
            const std::string qualName = nsName + "::" + shortName;
            func->name = qualName;
            nsMap[shortName] = qualName;
            functions.push_back(std::move(func));
        } else if (match(TokenType::STRUCT)) {
            auto st = parseStructDecl();
            // Rename struct to qualified name and register in all tables.
            const std::string shortName = st->name;
            const std::string qualName = nsName + "::" + shortName;
            // structNames_ already has shortName from parseStructDecl; add qualified name.
            structNames_.insert(qualName);
            // Expose the bare short name → qualified name mapping so that struct literals
            // inside this namespace block (and later in the same file) can use the short
            // name without full qualification, consistent with `import NamespaceName;`.
            bareImportedNames_[shortName] = qualName;
            st->name = qualName;
            nsMap[shortName] = qualName;
            structs.push_back(std::move(st));
        } else if (match(TokenType::ENUM)) {
            auto en = parseEnumDecl();
            const std::string shortName = en->name;
            const std::string qualName = nsName + "::" + shortName;
            en->name = qualName;
            nsMap[shortName] = qualName;
            // Register the qualified name in enumNames_ so that 3-segment access
            // (e.g. Status::Code::OK) can identify the prefix as an enum.
            enumNames_.insert(qualName);
            // Also map the short name → qualified name so that bare enum access
            // (e.g. Code::OK inside namespace functions) resolves correctly.
            bareEnumNames_[shortName] = qualName;
            enums.push_back(std::move(en));
        } else if (check(TokenType::GLOBAL)) {
            // Global variable declaration inside a namespace block.
            // e.g.  namespace Config { global var VERSION: int = 42; }
            // The variable is renamed to the qualified name (Config::VERSION) and
            // registered in importedGlobalVars_ so that Config::VERSION resolves.
            auto gv = parseGlobalDecl();
            const std::string shortName = gv->name;
            const std::string qualName = nsName + "::" + shortName;
            gv->name = qualName;
            // Register for Ns::varName access from outside the namespace.
            importedGlobalVars_[nsName][shortName] = qualName;
            // Also register in the namespace member map (enables nsMap lookups).
            nsMap[shortName] = qualName;
            globals.push_back(std::move(gv));
        } else if (check(TokenType::TYPE)) {
            // Type alias inside a namespace block.
            // e.g.  namespace Math { type Scalar = int; }
            // Registers both qualified (Math::Scalar → int) and bare (Scalar → int)
            // aliases so that both `Math::Scalar` and bare `Scalar` work as type names
            // within the same file after the namespace is declared.
            advance(); // consume 'type'
            const Token aliasName = consume(TokenType::IDENTIFIER, "Expected alias name after 'type'");
            consume(TokenType::ASSIGN, "Expected '=' after type alias name");
            // `type X = struct { ... }` — inline struct type alias in namespace.
            if (check(TokenType::STRUCT)) {
                advance(); // consume 'struct'
                const std::string qualName = nsName + "::" + aliasName.lexeme;
                auto sd = parseStructDecl(StructRepr::Auto, 0, qualName);
                structs.push_back(std::move(sd));
                typeAliases_[qualName] = qualName;
                if (!typeAliases_.count(aliasName.lexeme))
                    typeAliases_[aliasName.lexeme] = qualName;
                nsMap[aliasName.lexeme] = qualName;
                match(TokenType::SEMICOLON);
                continue;
            }
            if (!check(TokenType::IDENTIFIER) && !check(TokenType::STAR) &&
                !check(TokenType::STAR_STAR) && !check(TokenType::LPAREN) &&
                !check(TokenType::AMPERSAND) && !check(TokenType::FN)) {
                error("Expected type name after '=' in type alias");
            }
            const std::string resolvedType = parseTypeAnnotation();
            consume(TokenType::SEMICOLON, "Expected ';' after type alias");
            const std::string qualAliasName = nsName + "::" + aliasName.lexeme;
            // Qualified alias: Math::Scalar → int
            typeAliases_[qualAliasName] = resolvedType;
            // Bare alias (file-scope): Scalar → int
            // Only register if the bare name isn't already taken to avoid shadowing.
            if (!typeAliases_.count(aliasName.lexeme))
                typeAliases_[aliasName.lexeme] = resolvedType;
            // Register in namespace member map for consistency.
            nsMap[aliasName.lexeme] = qualAliasName;
        } else if (match(TokenType::NAMESPACE)) {
            // Nested namespace: namespace Outer { namespace Inner { ... } }
            // The inner namespace's fully-qualified key becomes "Outer::Inner".
            parseNamespace(functions, enums, structs, globals, nsName);
        } else {
            error("Only 'fn', 'struct', 'enum', 'namespace', 'global var', and 'type' declarations are allowed inside a namespace block");
        }
    }
    consume(TokenType::RBRACE, "Expected '}' to close namespace block");
}

std::string Parser::resolveNamespacedPath(const std::vector<std::string>& segments) {
    // Try progressively shorter namespace prefixes (longest-match first).
    // Namespace keys use '::' as separator (matching the source syntax).
    // For segments [a, b, c]:
    //   1st pass: ns="a::b", fn="c"
    //   2nd pass: ns="a",    fn="b::c"
    for (int cut = (int)segments.size() - 1; cut >= 1; --cut) {
        std::string ns;
        for (int i = 0; i < cut; ++i) {
            if (i > 0)
                ns += "::";
            ns += segments[i];
        }
        auto nsIt = importNamespaces_.find(ns);
        if (nsIt == importNamespaces_.end())
            continue;

        // Namespace found — build function name from remaining segments.
        std::string fn;
        for (int i = cut; i < (int)segments.size(); ++i) {
            if (i > cut)
                fn += "::";
            fn += segments[i];
        }
        auto fnIt = nsIt->second.find(fn);
        if (fnIt != nsIt->second.end()) {
            return fnIt->second; // actual (original) function name
        }
        // Namespace exists but the requested function is not in it.
        error("Function '" + fn + "' not found in namespace '" + ns + "'");
    }
    return ""; // no namespace prefix matched — caller uses flat-name fallback
}

std::string Parser::parseTypeAnnotation() {
    // Support reference type annotations: &type (e.g., &i32, &f64)
    std::string prefix;
    if (match(TokenType::AMPERSAND)) {
        prefix = "&";
    }
    // ── *T typed pointer syntax (C/Rust style) ────────────────────────────────
    // `*T`  →  desugared to `ptr<T>` internally.  Chains: `**T` → `ptr<ptr<T>>`.
    // `ptr` (bare) and `ptr<T>` remain valid for raw/fat pointers.
    if (check(TokenType::STAR)) {
        advance(); // consume '*'
        std::string inner = parseTypeAnnotation(); // recursively parse pointed-to type
        return prefix + "ptr<" + inner + ">";
    }
    // `**T` — the lexer produces a STAR_STAR token; desugar as two pointer levels.
    if (check(TokenType::STAR_STAR)) {
        advance(); // consume '**'
        std::string inner = parseTypeAnnotation();
        return prefix + "ptr<ptr<" + inner + ">>";
    }
    // ── Tuple type: (T1, T2, ...) ─────────────────────────────────────────────
    // Parsed as "tuple<T1,T2,...>" internally; lowered to an anonymous LLVM struct.
    if (check(TokenType::LPAREN)) {
        advance(); // consume '('
        std::vector<std::string> elemTypes;
        if (!check(TokenType::RPAREN)) {
            elemTypes.push_back(parseTypeAnnotation());
            while (match(TokenType::COMMA)) {
                if (check(TokenType::RPAREN)) break; // trailing comma
                elemTypes.push_back(parseTypeAnnotation());
            }
        }
        consume(TokenType::RPAREN, "Expected ')' to close tuple type");
        if (elemTypes.empty())
            return prefix + "void"; // () == void / unit
        std::string tupleAnn = "tuple<";
        for (size_t i = 0; i < elemTypes.size(); ++i) {
            if (i) tupleAnn += ',';
            tupleAnn += elemTypes[i];
        }
        tupleAnn += '>';
        return prefix + tupleAnn;
    }
    // ── fn(T1, T2, ...) -> R  function-pointer type annotation ───────────────
    // C-style typed function pointers.  `fn(int, int) -> int` desugars to the
    // internal `funcptr` type so that all funcptr codegen paths handle it
    // transparently.  The parameter / return type names are stored in a
    // side-channel string for potential future typed-call improvements.
    if (check(TokenType::FN)) {
        advance(); // consume 'fn'
        consume(TokenType::LPAREN, "Expected '(' in fn(...) type annotation");
        std::vector<std::string> paramTypes;
        if (!check(TokenType::RPAREN)) {
            paramTypes.push_back(parseTypeAnnotation());
            while (match(TokenType::COMMA)) {
                if (check(TokenType::RPAREN)) break; // trailing comma
                paramTypes.push_back(parseTypeAnnotation());
            }
        }
        consume(TokenType::RPAREN, "Expected ')' to close fn(...) type annotation");
        std::string retType = "void";
        if (check(TokenType::ARROW)) {
            advance(); // consume '->'
            retType = parseTypeAnnotation();
        }
        // Build a canonical description for diagnostics but lower to "funcptr"
        // for codegen — LLVM function pointers are opaque ptrs at IR level.
        (void)paramTypes; (void)retType; // suppress unused warning for now
        return prefix + "funcptr";
    }
    // Accept identifiers and the 'struct' keyword as type names
    std::string typeName;
    if (check(TokenType::STRUCT)) {
        typeName = advance().lexeme;
    } else {
        typeName = consume(TokenType::IDENTIFIER, "Expected type name").lexeme;
    }
    // Normalize 'str' → 'string' so both spellings work identically.
    if (typeName == "str")
        typeName = "string";
    // Support namespace-qualified type names: Math::Scalar, A::B::Type, etc.
    // Consume '::' + IDENTIFIER suffix segments until the pattern breaks.
    while (check(TokenType::SCOPE) && (current + 1 < tokens.size()) &&
           tokens[current + 1].type == TokenType::IDENTIFIER) {
        advance(); // consume '::'
        typeName += "::" + advance().lexeme;
    }
    // Handle parameterised SIMD types: u64x{N}, u32x{N}, i32x{N}, f32x{N}, etc.
    // u64x{LANES} where LANES is a comptime constant resolves to u64x2/4/8, etc.
    if (check(TokenType::LBRACE) && typeName.size() >= 3 &&
        (typeName[0] == 'u' || typeName[0] == 'i' || typeName[0] == 'f') && typeName.find('x') != std::string::npos) {
        advance(); // consume '{'
        std::string sizeStr;
        if (check(TokenType::IDENTIFIER)) {
            const std::string ident = advance().lexeme;
            auto it = comptimeConstants_.find(ident);
            if (it != comptimeConstants_.end())
                sizeStr = std::to_string(it->second);
            else
                sizeStr = ident;
        } else if (check(TokenType::INTEGER)) {
            sizeStr = std::to_string(advance().intValue);
        }
        consume(TokenType::RBRACE, "Expected '}' after SIMD lane count");
        typeName = typeName + sizeStr;
    }
    // Support array type annotations: type[], type[][], etc.
    while (check(TokenType::LBRACKET) && (current + 1 < tokens.size()) &&
           tokens[current + 1].type == TokenType::RBRACKET) {
        advance(); // consume '['
        advance(); // consume ']'
        typeName += "[]";
    }
    // Support ptr<T> generic pointer annotation: ptr<i64>, ptr<i32[]>, ptr<ptr<i32>>, etc.
    // Uses <T> angle-bracket syntax (LT / GT tokens).
    if (typeName == "ptr" && check(TokenType::LT)) {
        advance();                                 // consume '<'
        std::string inner = parseTypeAnnotation(); // recursively parse element type
        consume(TokenType::GT, "Expected '>' to close ptr<T> type parameter");
        typeName = "ptr<" + inner + ">";
    }
    // Support pslice<T> fat-pointer slice annotation: pslice<i64>, pslice<f64>, etc.
    if (typeName == "pslice" && check(TokenType::LT)) {
        advance();                                 // consume '<'
        std::string inner = parseTypeAnnotation(); // recursively parse element type
        consume(TokenType::GT, "Expected '>' to close pslice<T> type parameter");
        typeName = "pslice<" + inner + ">";
    }
    // Support dict[KeyType, ValType] generic annotation (e.g., dict[str, int])
    // Only activates when the type name is exactly "dict" followed by '[' with
    // non-empty content — avoids any collision with the existing type[] handling.
    if (typeName == "dict" && check(TokenType::LBRACKET) && current + 1 < tokens.size() &&
        tokens[current + 1].type != TokenType::RBRACKET) {
        advance(); // consume '['
        std::string typeParams;
        int depth = 1;
        while (!isAtEnd() && depth > 0) {
            const Token& t = advance();
            if (t.type == TokenType::LBRACKET)
                depth++;
            else if (t.type == TokenType::RBRACKET) {
                depth--;
                if (depth == 0)
                    break;
            }
            if (depth > 0)
                typeParams += t.lexeme;
        }
        typeName += "[" + typeParams + "]";
    }
    // Resolve type aliases transitively (e.g. A → B → int from
    //   type A = B; type B = int;).  Chase up to 32 hops to avoid infinite
    //   loops on cycles while still catching arbitrarily deep alias chains.
    for (int hop = 0; hop < kMaxTypeAliasHops; ++hop) {
        auto it = typeAliases_.find(typeName);
        if (it == typeAliases_.end()) break;
        if (it->second == typeName) break; // self-cycle guard
        typeName = it->second;
    }
    // Resolve namespace-qualified struct type aliases (e.g. bare `Point` →
    // `Geom::Point` when the struct was declared inside `namespace Geom` in this
    // file, or when `import Geom;` brought Point into the bare namespace).
    // Only apply to struct types — primitive / builtin type names are never in
    // the bareImportedNames_ map.
    {
        auto importedNameIt = bareImportedNames_.find(typeName);
        if (importedNameIt != bareImportedNames_.end() && structNames_.count(importedNameIt->second))
            typeName = importedNameIt->second;
    }
    return prefix + typeName;
}

std::unique_ptr<FunctionDecl> Parser::parseFunction(bool isOptMax) {
    const bool savedOptMaxState = inOptMaxFunction;
    inOptMaxFunction = isOptMax;
    consume(TokenType::FN, "Expected 'fn'");
    Token name = consume(TokenType::IDENTIFIER, "Expected function name");
    // Support `fn StructName::method(...)` — build the qualified name by consuming
    // any `:: IDENTIFIER` suffix segments, joining them with '::'.
    std::string qualifiedName = name.lexeme;
    while (check(TokenType::SCOPE)) {
        advance(); // consume '::'
        // Accept IDENTIFIER or keyword tokens (e.g. 'swap') as method names.
        Token seg = (check(TokenType::IDENTIFIER) || check(TokenType::SWAP))
                        ? advance()
                        : consume(TokenType::IDENTIFIER, "Expected method name after '::'");
        qualifiedName += "::" + seg.lexeme;
        name = seg; // keep line/column at the method segment for error reporting
    }
    // Override the token lexeme with the fully-qualified name so all downstream
    // code that reads name.lexeme gets e.g. "Counter::increment".
    const_cast<std::string&>(name.lexeme) = qualifiedName;

    // Parse optional type parameters: <T, R, ...>
    std::vector<std::string> typeParams;
    if (match(TokenType::LT)) {
        do {
            const Token tp = consume(TokenType::IDENTIFIER, "Expected type parameter name");
            typeParams.push_back(tp.lexeme);
        } while (match(TokenType::COMMA));
        consume(TokenType::GT, "Expected '>' after type parameters");
    }
    // Type parameters are parsed for syntax compatibility but not yet implemented.
    // Any type parameter names used inside the body will be treated as unknown
    // type annotations and may produce unhelpful errors.
    if (!typeParams.empty()) {
        std::string paramList = typeParams[0];
        for (size_t tpi = 1; tpi < typeParams.size(); ++tpi)
            paramList += ", " + typeParams[tpi];
        warnings_.push_back("warning: line " + std::to_string(name.line) + ":" +
                            std::to_string(name.column) +
                            ": Generic type parameters <" + paramList +
                            "> on function '" + qualifiedName +
                            "' are not yet implemented; they are parsed but have no effect. "
                            "Use explicit typed overloads as a workaround.");
    }

    consume(TokenType::LPAREN, "Expected '(' after function name");

    std::vector<Parameter> parameters;
    bool hasDefault = false;
    if (!check(TokenType::RPAREN)) {
        do {
            // Parse optional parameter annotation: @prefetch
            bool paramPrefetch = false;
            if (check(TokenType::AT)) {
                advance(); // consume '@'
                // Accept both IDENTIFIER and PREFETCH keyword for @prefetch
                if (check(TokenType::PREFETCH)) {
                    advance();
                    paramPrefetch = true;
                } else {
                    const Token ann = consume(TokenType::IDENTIFIER, "Expected annotation name after '@'");
                    if (ann.lexeme == "prefetch") {
                        paramPrefetch = true;
                    } else {
                        error("Unknown parameter annotation '@" + ann.lexeme + "'; supported: @prefetch");
                    }
                }
            }
            const Token paramName = consume(TokenType::IDENTIFIER, "Expected parameter name");
            std::string typeName;
            if (match(TokenType::COLON)) {
                typeName = parseTypeAnnotation();
            } else if (inOptMaxFunction) {
                error("OPTMAX parameters must include type annotations");
            }
            std::unique_ptr<Expression> defaultVal = nullptr;
            if (match(TokenType::ASSIGN)) {
                defaultVal = parsePrimary();
                if (!dynamic_cast<LiteralExpr*>(defaultVal.get())) {
                    error("Default parameter value must be a literal (integer, float, or string)");
                }
                hasDefault = true;
            } else if (hasDefault) {
                error("Non-default parameter '" + paramName.lexeme + "' cannot follow a default parameter");
            }
            Parameter param(paramName.lexeme, typeName, std::move(defaultVal));
            param.hintPrefetch = paramPrefetch;
            parameters.push_back(std::move(param));
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::RPAREN, "Expected ')' after parameters");

    // Optional return type annotation: -> type
    std::string returnType;
    bool neverReturn = false; // set by `-> never` syntax
    if (match(TokenType::ARROW)) {
        returnType = parseTypeAnnotation();
        // `-> never` is syntactic sugar for `@noreturn` with void return type.
        // This mirrors Rust's `-> !` and C23's `[[noreturn]]` attribute.
        if (returnType == "never") {
            neverReturn = true;
            returnType = "void";
        }
    }

    std::unique_ptr<BlockStmt> body;

    // Function alias: fn name(params) [-> ret] == target::path;
    // The target path is resolved through the namespace registry when possible,
    // falling back to flat name mangling (a::b::c → a__b__c) for compatibility.
    // Desugars to a thin wrapper: fn name(params) { return target(params...); }
    if (match(TokenType::EQ)) {
        // Collect the full target path.
        std::vector<std::string> segs;
        segs.push_back(consume(TokenType::IDENTIFIER, "Expected function path after '=='").lexeme);
        while (check(TokenType::SCOPE)) {
            advance(); // consume '::'
            segs.push_back(consume(TokenType::IDENTIFIER, "Expected identifier in function path").lexeme);
        }
        consume(TokenType::SEMICOLON, "Expected ';' after function alias");
        // Resolve via namespace registry.
        std::string resolvedTarget;
        if (segs.size() >= 2) {
            resolvedTarget = resolveNamespacedPath(segs);
        }
        if (resolvedTarget.empty()) {
            if (segs.size() == 1) {
                resolvedTarget = segs[0];
            } else {
                // Build a helpful error: the user wrote a multi-segment path
                // that couldn't be resolved through any imported namespace.
                std::string path = segs[0];
                for (size_t i = 1; i < segs.size(); ++i)
                    path += "::" + segs[i];
                error("Cannot resolve function alias target '" + path +
                      "': no matching namespace found. "
                      "Import the file with 'import \"file\" as alias' first.");
            }
        }
        // Build: return resolvedTarget(params...);
        std::vector<std::unique_ptr<Expression>> callArgs;
        for (const auto& param : parameters) {
            auto argExpr = std::make_unique<IdentifierExpr>(param.name);
            callArgs.push_back(std::move(argExpr));
        }
        auto callExpr = std::make_unique<CallExpr>(resolvedTarget, std::move(callArgs));
        callExpr->line = name.line;
        callExpr->column = name.column;
        callExpr->fromStdNamespace = true; // compiler-generated wrapper
        auto retStmt = std::make_unique<ReturnStmt>(std::move(callExpr));
        retStmt->column = name.column;
        std::vector<std::unique_ptr<Statement>> stmts;
        stmts.push_back(std::move(retStmt));
        body = std::make_unique<BlockStmt>(std::move(stmts));
    } else if (match(TokenType::ASSIGN)) {
        auto expr = parseExpression();
        consume(TokenType::SEMICOLON, "Expected ';' after expression-body function");
        auto retStmt = std::make_unique<ReturnStmt>(std::move(expr));
        retStmt->line = name.line;
        retStmt->column = name.column;
        std::vector<std::unique_ptr<Statement>> stmts;
        stmts.push_back(std::move(retStmt));
        body = std::make_unique<BlockStmt>(std::move(stmts));
    } else {
        body = parseBlock();
    }

    inOptMaxFunction = savedOptMaxState;

    // ── Post-parse jmp/label validation ──────────────────────────────────────
    // Collect all labels defined in this function (recursively) and all jmp
    // targets; then:
    //   1. Error on any jmp target that has no matching label.
    //   2. Error on any forward jmp that skips over a var declaration at the
    //      top level of the function body (skipped initializer = uninitialized
    //      variable).
    if (body) {
        // Flatten the top-level statement list for linear-order analysis.
        const auto& stmts = body->statements;

        // --- Collect all label names (recursive) ----------------------------
        std::unordered_map<std::string, int /*line*/> definedLabels;
        std::function<void(const Statement*)> collectLabels = [&](const Statement* s) {
            if (!s) return;
            if (s->type == ASTNodeType::LABEL_STMT) {
                const auto* ls = static_cast<const LabelStmt*>(s);
                definedLabels[ls->labelName] = ls->line;
                return;
            }
            // Recurse into blocks/branches/loops.
            auto recurseBlock = [&](const BlockStmt* blk) {
                if (!blk) return;
                for (const auto& sub : blk->statements) collectLabels(sub.get());
            };
            switch (s->type) {
            case ASTNodeType::BLOCK: recurseBlock(static_cast<const BlockStmt*>(s)); break;
            case ASTNodeType::IF_STMT: {
                const auto* is = static_cast<const IfStmt*>(s);
                collectLabels(is->thenBranch.get());
                collectLabels(is->elseBranch.get());
                break;
            }
            case ASTNodeType::WHILE_STMT: collectLabels(static_cast<const WhileStmt*>(s)->body.get()); break;
            case ASTNodeType::DO_WHILE_STMT: collectLabels(static_cast<const DoWhileStmt*>(s)->body.get()); break;
            case ASTNodeType::FOR_STMT: collectLabels(static_cast<const ForStmt*>(s)->body.get()); break;
            case ASTNodeType::FOR_EACH_STMT: collectLabels(static_cast<const ForEachStmt*>(s)->body.get()); break;
            default: break;
            }
        };
        for (const auto& s : stmts) collectLabels(s.get());

        // --- Validate each jmp (recursive) ----------------------------------
        std::function<void(const Statement*)> validateJmps = [&](const Statement* s) {
            if (!s) return;
            if (s->type == ASTNodeType::JMP_STMT) {
                const auto* js = static_cast<const JmpStmt*>(s);
                if (!definedLabels.count(js->targetLabel)) {
                    error("'jmp " + js->targetLabel + "': label '" + js->targetLabel +
                          "' is not defined in this function");
                }
                return;
            }
            // Recurse same as collectLabels.
            switch (s->type) {
            case ASTNodeType::BLOCK:
                for (const auto& sub : static_cast<const BlockStmt*>(s)->statements)
                    validateJmps(sub.get());
                break;
            case ASTNodeType::IF_STMT: {
                const auto* is = static_cast<const IfStmt*>(s);
                validateJmps(is->thenBranch.get());
                validateJmps(is->elseBranch.get());
                break;
            }
            case ASTNodeType::WHILE_STMT: validateJmps(static_cast<const WhileStmt*>(s)->body.get()); break;
            case ASTNodeType::DO_WHILE_STMT: validateJmps(static_cast<const DoWhileStmt*>(s)->body.get()); break;
            case ASTNodeType::FOR_STMT: validateJmps(static_cast<const ForStmt*>(s)->body.get()); break;
            case ASTNodeType::FOR_EACH_STMT: validateJmps(static_cast<const ForEachStmt*>(s)->body.get()); break;
            default: break;
            }
        };
        for (const auto& s : stmts) validateJmps(s.get());

        // --- Forward-jump-over-var-decl check (top-level linear scan) --------
        // For each `jmp` at the top level of the function body, find the matching
        // `label` that appears later in the same linear statement list.  Any
        // `var` declaration between them is an error (initializer would be skipped).
        for (size_t i = 0; i < stmts.size(); ++i) {
            if (stmts[i]->type != ASTNodeType::JMP_STMT) continue;
            const auto* js = static_cast<const JmpStmt*>(stmts[i].get());
            // Find the label in the top-level list (forward only).
            for (size_t j = i + 1; j < stmts.size(); ++j) {
                if (stmts[j]->type == ASTNodeType::LABEL_STMT) {
                    const auto* ls = static_cast<const LabelStmt*>(stmts[j].get());
                    if (ls->labelName == js->targetLabel) {
                        // Found a forward jump. Scan statements between i and j for var decls.
                        for (size_t k = i + 1; k < j; ++k) {
                            if (stmts[k]->type == ASTNodeType::VAR_DECL) {
                                const auto* vd = static_cast<const VarDecl*>(stmts[k].get());
                                error("'jmp " + js->targetLabel + "' at line " +
                                      std::to_string(js->line) +
                                      " jumps forward over declaration of variable '" +
                                      vd->name + "' at line " +
                                      std::to_string(vd->line) +
                                      "; the initializer would be skipped. "
                                      "Move the declaration before the 'jmp' or after the label.");
                            }
                        }
                        break; // Only check the first matching label.
                    }
                }
            }
        }
    }
    // ─────────────────────────────────────────────────────────────────────────

    auto funcDecl = std::make_unique<FunctionDecl>(name.lexeme, std::move(typeParams), std::move(parameters),
                                                   std::move(body), isOptMax, returnType);
    funcDecl->line = name.line;
    funcDecl->column = name.column;
    if (neverReturn)
        funcDecl->hintNoReturn = true;
    return funcDecl;
}

std::unique_ptr<Statement> Parser::parseStatement() {
    const RecursionGuard guard(*this);
    // Capture the keyword token position for source location tracking.

    // @loop(...) placed before a for/while/foreach loop as a pre-loop annotation.
    // This is equivalent to placing it after the loop header.
    if (check(TokenType::AT) && current + 1 < tokens.size() &&
        (tokens[current + 1].type == TokenType::LOOP ||
         (tokens[current + 1].type == TokenType::IDENTIFIER && tokens[current + 1].lexeme == "loop"))) {
        const Token kw = tokens[current];
        advance(); // consume '@'
        advance(); // consume 'loop'
        LoopConfig loopHints = parseLoopAnnotation();
        // Parse the loop statement that follows
        auto loopStmt = parseStatement();
        // Apply loop hints to the following for/while/foreach statement
        if (auto* fs = dynamic_cast<ForStmt*>(loopStmt.get())) {
            fs->loopHints = loopHints;
        } else if (auto* ws = dynamic_cast<WhileStmt*>(loopStmt.get())) {
            ws->loopHints = loopHints;
        } else if (auto* fe = dynamic_cast<ForEachStmt*>(loopStmt.get())) {
            fe->loopHints = loopHints;
        }
        loopStmt->line = kw.line;
        loopStmt->column = kw.column;
        return loopStmt;
    }
    if (match(TokenType::LIKELY) || match(TokenType::UNLIKELY)) {
        const Token hintKw = tokens[current - 1];
        const bool isLikely = (hintKw.type == TokenType::LIKELY);
        if (!match(TokenType::IF)) {
            error("Expected 'if' after '" + hintKw.lexeme + "'");
        }
        const Token kw = tokens[current - 1];
        auto stmt = parseIfStmt();
        auto* ifStmt = dynamic_cast<IfStmt*>(stmt.get());
        if (ifStmt) {
            ifStmt->hintLikely = isLikely;
            ifStmt->hintUnlikely = !isLikely;
        }
        stmt->line = hintKw.line;
        stmt->column = hintKw.column;
        return stmt;
    }
    // Labeled loop: label_name: while/for/foreach/forever/loop/repeat/do/until { ... }
    // Enables `break label_name;` and `continue label_name;` to target specific outer loops.
    if (check(TokenType::IDENTIFIER) && current + 1 < tokens.size() &&
        tokens[current + 1].type == TokenType::COLON && current + 2 < tokens.size()) {
        const TokenType nextNext = tokens[current + 2].type;
        const bool isLoopKeyword =
            nextNext == TokenType::WHILE || nextNext == TokenType::FOR || nextNext == TokenType::FOREACH ||
            nextNext == TokenType::FOREVER || nextNext == TokenType::LOOP || nextNext == TokenType::REPEAT ||
            nextNext == TokenType::DO || nextNext == TokenType::UNTIL;
        if (isLoopKeyword) {
            const std::string loopLabel = tokens[current].lexeme;
            const Token labelTok = tokens[current];
            advance(); // consume identifier
            advance(); // consume ':'
            // Now parse the loop statement normally and attach the label.
            auto loopStmt = parseStatement();
            if (auto* ws = dynamic_cast<WhileStmt*>(loopStmt.get())) {
                ws->label = loopLabel;
            } else if (auto* fs = dynamic_cast<ForStmt*>(loopStmt.get())) {
                fs->label = loopLabel;
            } else if (auto* fe = dynamic_cast<ForEachStmt*>(loopStmt.get())) {
                fe->label = loopLabel;
            }
            loopStmt->line = labelTok.line;
            loopStmt->column = labelTok.column;
            return loopStmt;
        }
    }
    if (match(TokenType::IF)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseIfStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::WHILE)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseWhileStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::DO)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseDoWhileStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::FOR)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseForStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    // parallel for / parallel while / parallel foreach — auto-parallelizes the loop.
    // Sets loopHints.parallel = true on the loop statement.
    if (match(TokenType::PARALLEL)) {
        const Token kw = tokens[current - 1];
        if (match(TokenType::FOR)) {
            auto stmt = parseForStmt();
            stmt->line = kw.line;
            stmt->column = kw.column;
            if (auto* forStmt = dynamic_cast<ForStmt*>(stmt.get())) {
                forStmt->loopHints.parallel = true;
            }
            return stmt;
        } else if (match(TokenType::WHILE)) {
            auto stmt = parseWhileStmt();
            stmt->line = kw.line;
            stmt->column = kw.column;
            if (auto* whileStmt = dynamic_cast<WhileStmt*>(stmt.get())) {
                whileStmt->loopHints.parallel = true;
            }
            return stmt;
        } else if (match(TokenType::FOREACH)) {
            auto stmt = parseForEachStmt();
            stmt->line = kw.line;
            stmt->column = kw.column;
            if (auto* feStmt = dynamic_cast<ForEachStmt*>(stmt.get())) {
                feStmt->loopHints.parallel = true;
            }
            return stmt;
        } else {
            error("Expected 'for', 'while', or 'foreach' after 'parallel'");
        }
    }
    if (match(TokenType::RETURN)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseReturnStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::BREAK)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseBreakStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::CONTINUE)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseContinueStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::JMP)) {
        // parseJmpStmt reads tokens[current-1] for the warning location.
        return parseJmpStmt();
    }
    if (match(TokenType::LABEL)) {
        return parseLabelStmt();
    }
    if (match(TokenType::SWITCH)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseSwitchStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::UNLESS)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseUnlessStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::UNTIL)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseUntilStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::LOOP)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseLoopStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::REPEAT)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseRepeatStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::DEFER)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseDeferStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::GUARD)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseGuardStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::WHEN)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseWhenStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::FOREVER)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseForeverStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::FOREACH)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseForEachStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::ELIF)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseElifStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (check(TokenType::SWAP) && (current + 1 >= tokens.size() || tokens[current + 1].type != TokenType::LPAREN)) {
        advance(); // consume SWAP
        const Token kw = tokens[current - 1];
        auto stmt = parseSwapStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::TIMES)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseTimesStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::WITH)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseWithStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::PIPELINE)) {
        const Token kw = tokens[current - 1];
        auto stmt = parsePipelineStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::VAR)) {
        // Tuple destructuring: var (a, b) = expr;
        if (check(TokenType::LPAREN)) {
            auto decls = parseTupleDestructuringDecl(false);
            consume(TokenType::SEMICOLON, "Expected ';' after tuple destructuring declaration");
            // Wrap multiple decls in a block statement
            auto blk = std::make_unique<BlockStmt>(std::move(decls));
            blk->line = tokens[current - 1].line;
            blk->column = tokens[current - 1].column;
            return blk;
        }
        // Array destructuring: var [a, b, c] = expr;
        if (check(TokenType::LBRACKET)) {
            auto decls = parseDestructuringDecl(false);
            consume(TokenType::SEMICOLON, "Expected ';' after destructuring declaration");
            auto blk = std::make_unique<BlockStmt>(std::move(decls));
            blk->line = tokens[current - 1].line;
            blk->column = tokens[current - 1].column;
            return blk;
        }
        auto decl = parseVarDecl(false);
        consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");
        return decl;
    }
    if (match(TokenType::CONST)) {
        // Tuple destructuring: const (a, b) = expr;
        if (check(TokenType::LPAREN)) {
            auto decls = parseTupleDestructuringDecl(true);
            consume(TokenType::SEMICOLON, "Expected ';' after tuple destructuring declaration");
            auto blk = std::make_unique<BlockStmt>(std::move(decls));
            blk->line = tokens[current - 1].line;
            blk->column = tokens[current - 1].column;
            return blk;
        }
        // Array destructuring: const [a, b, c] = expr;
        if (check(TokenType::LBRACKET)) {
            auto decls = parseDestructuringDecl(true);
            consume(TokenType::SEMICOLON, "Expected ';' after destructuring declaration");
            auto blk = std::make_unique<BlockStmt>(std::move(decls));
            blk->line = tokens[current - 1].line;
            blk->column = tokens[current - 1].column;
            return blk;
        }
        auto decl = parseVarDecl(true);
        consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");
        return decl;
    }
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "assume") {
        advance(); // consume 'assume'
        const Token kw = tokens[current - 1];
        auto stmt = parseAssumeStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (check(TokenType::LBRACE)) {
        const Token kw = peek();
        auto stmt = parseBlock();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::CATCH)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseCatchStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::THROW)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseThrowStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::ENSURE)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseEnsureStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::INVALIDATE)) {
        const Token kw = tokens[current - 1];
        const Token varName = consume(TokenType::IDENTIFIER, "Expected variable name after 'invalidate'");
        consume(TokenType::SEMICOLON, "Expected ';' after invalidate statement");
        auto stmt = std::make_unique<InvalidateStmt>(varName.lexeme);
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    // register var — hint to keep variable in a CPU register.
    // Syntax: register var name[:type] = expr;
    if (match(TokenType::REGISTER)) {
        const Token kw = tokens[current - 1];
        const bool isConst = check(TokenType::CONST);
        if (!match(TokenType::VAR) && !match(TokenType::CONST)) {
            error("Expected 'var' or 'const' after 'register'");
        }
        const Token name = consume(TokenType::IDENTIFIER, "Expected variable name after 'register var'");
        std::string typeName;
        if (match(TokenType::COLON)) {
            typeName = parseTypeAnnotation();
        }
        std::unique_ptr<Expression> init = nullptr;
        if (match(TokenType::ASSIGN)) {
            init = parseExpression();
        }
        consume(TokenType::SEMICOLON, "Expected ';' after register variable declaration");

        if (typeName.empty() && !init) {
            error("Variable '" + name.lexeme +
                  "' requires an explicit type annotation "
                  "(e.g., '" +
                  name.lexeme + ":i64'). Untyped variables without an initializer are not allowed.");
        }
        auto decl = std::make_unique<VarDecl>(name.lexeme, std::move(init), isConst, typeName);
        decl->isRegister = true;
        decl->line = kw.line;
        decl->column = kw.column;
        return decl;
    }
    // global var/const — declare a program-wide variable inside a function body.
    // Syntax: global [mut] var name[:type] [= expr];
    if (match(TokenType::GLOBAL)) {
        const Token kw = tokens[current - 1];
        // Accept optional `mut` qualifier: `global mut var name` — the explicit
        // mutable form chosen for v5.0 compatibility. In v4.x this only records
        // the spelling choice; enforcement and deprecation of bare `global var`
        // are planned for the v5.0 transition.
        match(TokenType::MUT); // consume `mut` if present
        const bool isConst = check(TokenType::CONST);
        if (!match(TokenType::VAR) && !match(TokenType::CONST)) {
            error("Expected 'var' or 'const' after 'global'");
        }
        const Token name = consume(TokenType::IDENTIFIER, "Expected variable name after 'global var'");
        std::string typeName;
        if (match(TokenType::COLON)) {
            typeName = parseTypeAnnotation();
        }
        std::unique_ptr<Expression> init = nullptr;
        if (match(TokenType::ASSIGN)) {
            init = parseExpression();
        }
        consume(TokenType::SEMICOLON, "Expected ';' after global variable declaration");

        if (typeName.empty() && !init) {
            error("Variable '" + name.lexeme +
                  "' requires an explicit type annotation "
                  "(e.g., '" +
                  name.lexeme + ":i64'). Untyped variables without an initializer are not allowed.");
        }
        auto decl = std::make_unique<VarDecl>(name.lexeme, std::move(init), isConst, typeName);
        decl->isGlobal = true;
        decl->line = kw.line;
        decl->column = kw.column;
        return decl;
    }
    // atomic var / volatile var / atomic volatile var / volatile atomic var
    // ──────────────────────────────────────────────────────────────────────
    // Syntax: [atomic] [volatile] var name[:type] = expr;
    //         [volatile] [atomic] var name[:type] = expr;
    //
    // atomic  — all loads and stores to this variable use seq-cst atomic ordering,
    //           making them indivisible with respect to other threads.
    // volatile — all loads and stores are marked volatile in the emitted IR,
    //            preventing the compiler from eliding, reordering, or CSE-ing them.
    //
    // The two qualifiers may be combined in either order.  `const` is not
    // supported with these qualifiers (a const is already a compile-time
    // constant and can have no externally-visible storage to guard).
    if (check(TokenType::ATOMIC) || check(TokenType::VOLATILE)) {
        bool isAtom = false;
        bool isVol = false;

        // Capture the first qualifier token for accurate source location.
        const Token firstKw = tokens[current];

        // Consume one or two qualifiers.
        if (match(TokenType::ATOMIC)) {
            isAtom = true;
            if (match(TokenType::VOLATILE))
                isVol = true;
        } else if (match(TokenType::VOLATILE)) {
            isVol = true;
            if (match(TokenType::ATOMIC))
                isAtom = true;
        }

        if (!match(TokenType::VAR)) {
            error(std::string("Expected 'var' after '") +
                  (isAtom && isVol ? "atomic volatile"
                   : isAtom        ? "atomic"
                                   : "volatile") +
                  "'");
        }

        const Token name = consume(TokenType::IDENTIFIER, "Expected variable name after qualifier");
        std::string typeName;
        if (match(TokenType::COLON)) {
            typeName = parseTypeAnnotation();
        }
        std::unique_ptr<Expression> init = nullptr;
        if (match(TokenType::ASSIGN)) {
            init = parseExpression();
        }
        consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");

        if (typeName.empty() && !init) {
            error("Variable '" + name.lexeme + "' requires an explicit type annotation or initializer.");
        }

        auto decl = std::make_unique<VarDecl>(name.lexeme, std::move(init),
                                              /*isConst=*/false, typeName);
        decl->isAtomic = isAtom;
        decl->isVolatile = isVol;
        decl->line = firstKw.line;
        decl->column = firstKw.column;
        return decl;
    }
    if (match(TokenType::PREFETCH)) {
        const Token kw = tokens[current - 1];

        // Parse optional byte offset: prefetch+128 or prefetch+64 etc.
        // The '+' immediately after 'prefetch' indicates cache-line-ahead
        // speculative prefetching.  E.g. prefetch+128 fetches 2 cache lines
        // ahead (128 bytes = 2 × 64-byte cache lines on most CPUs).
        int64_t offsetBytes = 0;
        if (match(TokenType::PLUS)) {
            if (!check(TokenType::INTEGER)) {
                error("Expected integer offset after '+' in prefetch");
            }
            const Token offsetTok = advance();
            offsetBytes = offsetTok.intValue;
        }

        // Parse optional attributes: hot, immut
        bool hintHot = false;
        bool hintImmut = false;
        while (check(TokenType::IDENTIFIER) && !isAtEnd()) {
            const std::string& attrName = peek().lexeme;
            if (attrName == "hot") {
                hintHot = true;
                advance();
                continue;
            }
            if (attrName == "immut") {
                hintImmut = true;
                advance();
                continue;
            }
            break;
        }
        // Determine if this is a variable declaration or standalone prefetch.
        // Declaration forms:
        //   prefetch [attrs] var name[:type] [= expr];
        //   prefetch [attrs] name:type = expr;       (has '=')
        //   prefetch [attrs] name = expr;             (has '=')
        // Standalone forms (existing variable):
        //   prefetch name[:type];                     (no '=' → just re-prefetch)
        bool isDecl = false;
        if (check(TokenType::VAR)) {
            isDecl = true;
        } else if (check(TokenType::IDENTIFIER)) {
            // Look ahead past optional :type to see if there's an '='
            size_t lookahead = current + 1;
            if (lookahead < tokens.size() && tokens[lookahead].type == TokenType::COLON) {
                lookahead++; // skip ':'
                if (lookahead < tokens.size() && tokens[lookahead].type == TokenType::IDENTIFIER)
                    lookahead++; // skip type name
                // Skip optional array brackets []
                while (lookahead + 1 < tokens.size() && tokens[lookahead].type == TokenType::LBRACKET &&
                       tokens[lookahead + 1].type == TokenType::RBRACKET)
                    lookahead += 2;
            }
            if (lookahead < tokens.size() && tokens[lookahead].type == TokenType::ASSIGN)
                isDecl = true;
        }
        if (isDecl) {
            // prefetch [attrs] var name:type = expr; OR prefetch [attrs] name:type = expr;
            const bool isConst = hintImmut;
            std::string typeName;
            if (match(TokenType::VAR)) {
                // consumed var
            }
            const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in prefetch declaration");
            if (match(TokenType::COLON)) {
                typeName = parseTypeAnnotation();
            }
            std::unique_ptr<Expression> init = nullptr;
            if (match(TokenType::ASSIGN)) {
                init = parseExpression();
            }
            consume(TokenType::SEMICOLON, "Expected ';' after prefetch declaration");
            auto varDecl = std::make_unique<VarDecl>(name.lexeme, std::move(init), isConst, typeName);
            varDecl->line = kw.line;
            varDecl->column = kw.column;
            auto stmt = std::make_unique<PrefetchStmt>(std::move(varDecl), hintHot, hintImmut, offsetBytes);
            stmt->line = kw.line;
            stmt->column = kw.column;
            return stmt;
        }
        // Standalone prefetch of existing variable: prefetch name[:type];
        if (check(TokenType::IDENTIFIER)) {
            const Token varName = advance();
            // Consume optional :type annotation
            if (match(TokenType::COLON)) {
                parseTypeAnnotation(); // consume and discard
            }
            consume(TokenType::SEMICOLON, "Expected ';' after prefetch statement");
            auto stmt = std::make_unique<PrefetchStmt>(varName.lexeme, offsetBytes);
            stmt->line = kw.line;
            stmt->column = kw.column;
            return stmt;
        }
        // Function-call style: prefetch(expr) — prefetch the address of expr.
        if (check(TokenType::LPAREN)) {
            advance(); // consume '('
            auto addrExpr = parseExpression();
            consume(TokenType::RPAREN, "Expected ')' after prefetch expression");
            consume(TokenType::SEMICOLON, "Expected ';' after prefetch(expr) statement");
            auto prefetchStmt = std::make_unique<PrefetchStmt>(std::move(addrExpr), offsetBytes);
            prefetchStmt->hintHot = hintHot;
            prefetchStmt->hintImmut = hintImmut;
            prefetchStmt->line = kw.line;
            prefetchStmt->column = kw.column;
            return prefetchStmt;
        }
        error("Expected variable name or declaration after 'prefetch'");
    }
    if (match(TokenType::MOVE)) {
        // move semantics: either `move var x = expr;` or `move x = expr;`
        const Token kw = tokens[current - 1];
        // Could be: move <type> <name> = <expr>;
        // or just: move used as expression context (handled elsewhere)
        if (check(TokenType::VAR) || (check(TokenType::IDENTIFIER) && current + 1 < tokens.size() &&
                                      tokens[current + 1].type == TokenType::IDENTIFIER)) {
            // `move var x = expr;` or `move var x:type = expr;` or `move int x = expr;`
            std::string typeName;
            if (match(TokenType::VAR)) {
                typeName = "";
            } else {
                typeName = advance().lexeme; // consume type name
            }
            const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in move declaration");
            // Optional :type annotation (supports `move var x:i64 = expr;`)
            if (typeName.empty() && match(TokenType::COLON)) {
                typeName = parseTypeAnnotation();
            }
            // Move declarations always have an initializer (parsed below);
            // we infer the type from it when no annotation is given.
            // No error needed here.
            consume(TokenType::ASSIGN, "Expected '=' in move declaration");
            auto init = parseExpression();
            consume(TokenType::SEMICOLON, "Expected ';' after move declaration");
            auto stmt = std::make_unique<MoveDecl>(name.lexeme, typeName, std::move(init));
            stmt->line = kw.line;
            stmt->column = kw.column;
            return stmt;
        }
        // Otherwise treat as expression statement: move <expr> used in assignment
        // e.g. `x = move y;`  — rewind and let expression parsing handle it
        current--; // put back 'move' token
        return parseExprStmt();
    }
    if (match(TokenType::BORROW)) {
        // `borrow var ref = expr;`       — immutable borrow
        // `borrow mut ref = expr;`       — mutable borrow (single mutable alias)
        // `borrow type ref = expr;`      — typed immutable borrow
        // `borrow ref:type = expr;`      — typed immutable borrow
        const Token kw = tokens[current - 1];
        bool isMut = false;
        if (match(TokenType::MUT)) {
            isMut = true;
        }
        std::string typeName;
        if (!isMut) {
            if (match(TokenType::VAR)) {
                typeName = "";
            } else if (check(TokenType::IDENTIFIER) && current + 1 < tokens.size() &&
                       tokens[current + 1].type == TokenType::IDENTIFIER) {
                typeName = advance().lexeme;
            }
        }
        const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in borrow declaration");
        // Support name:type syntax: borrow j:u32 = expr;
        if (typeName.empty() && match(TokenType::COLON)) {
            typeName = parseTypeAnnotation();
        }
        consume(TokenType::ASSIGN, "Expected '=' in borrow declaration");
        auto init = parseExpression();
        consume(TokenType::SEMICOLON, "Expected ';' after borrow declaration");
        // Type annotation is optional when an initializer is present —
        // codegen infers the borrowed value's representation.
        // Create a VarDecl with a BorrowExpr wrapper
        auto borrowExpr = std::make_unique<BorrowExpr>(std::move(init), isMut);
        borrowExpr->line = kw.line;
        borrowExpr->column = kw.column;
        auto stmt = std::make_unique<VarDecl>(name.lexeme, std::move(borrowExpr), false, typeName);
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::REBORROW)) {
        // `reborrow ref = &src;`         — reborrow existing borrow
        // `reborrow mut ref = &src;`     — mutable reborrow
        // `reborrow ref = &src.field;`   — partial borrow of struct field
        // `reborrow ref = &src[idx];`    — partial borrow of array element
        const Token kw = tokens[current - 1];
        bool isMut = false;
        if (match(TokenType::MUT)) {
            isMut = true;
        }
        const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in reborrow declaration");
        consume(TokenType::ASSIGN, "Expected '=' in reborrow declaration");
        // Expect &expr
        consume(TokenType::AMPERSAND, "Expected '&' before source expression in reborrow");
        auto src = parsePrimary();
        std::string fieldName;
        std::unique_ptr<Expression> indexExpr;
        if (match(TokenType::DOT)) {
            const Token fld = consume(TokenType::IDENTIFIER, "Expected field name after '.' in reborrow");
            fieldName = fld.lexeme;
        } else if (match(TokenType::LBRACKET)) {
            indexExpr = parseExpression();
            consume(TokenType::RBRACKET, "Expected ']' after index in reborrow");
        }
        consume(TokenType::SEMICOLON, "Expected ';' after reborrow declaration");
        auto reborrowExpr =
            std::make_unique<ReborrowExpr>(std::move(src), isMut, std::move(fieldName), std::move(indexExpr));
        reborrowExpr->line = kw.line;
        reborrowExpr->column = kw.column;
        auto stmt = std::make_unique<VarDecl>(name.lexeme, std::move(reborrowExpr), false, "");
        stmt->isCompilerGenerated = true; // reborrow: type inferred from source
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::FREEZE)) {
        // `freeze x;` — mark variable x immutable for the rest of its lifetime.
        const Token kw = tokens[current - 1];
        const Token name = consume(TokenType::IDENTIFIER, "Expected variable name after 'freeze'");
        consume(TokenType::SEMICOLON, "Expected ';' after 'freeze'");
        auto stmt = std::make_unique<FreezeStmt>(name.lexeme);
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }

    if (match(TokenType::SHARED)) {
        // `shared x;` — transition x to read-only aliasable ownership (Ω spec §3.1).
        const Token kw = tokens[current - 1];
        const Token name = consume(TokenType::IDENTIFIER, "Expected variable name after 'shared'");
        consume(TokenType::SEMICOLON, "Expected ';' after 'shared'");
        auto stmt = std::make_unique<SharedStmt>(name.lexeme);
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }

    if (match(TokenType::OWN)) {
        // `own x;` — explicitly assert unique ownership of x (Ω spec §3.1).
        const Token kw = tokens[current - 1];
        const Token name = consume(TokenType::IDENTIFIER, "Expected variable name after 'own'");
        consume(TokenType::SEMICOLON, "Expected ';' after 'own'");
        auto stmt = std::make_unique<OwnStmt>(name.lexeme);
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }

    // ── construct EXPR { field: val, ... }; ──────────────────────────────────
    // In-place field initialisation of already-allocated memory.
    // The target is any expression that yields a ptr<T>.
    // Lowers to a sequence of GEP + store pairs — zero extra abstraction cost.
    if (match(TokenType::CONSTRUCT)) {
        const Token kw = tokens[current - 1];
        // Parse the target expression.  Intentionally stop before `{` so
        // that `construct arr + 1 { x: 5 }` is parsed as `(arr + 1)`.
        // Use parseUnary as the precedence ceiling (no comma or assignment).
        auto target = parseUnary();
        consume(TokenType::LBRACE, "Expected '{' after construct target expression");
        std::vector<std::pair<std::string, std::unique_ptr<Expression>>> fields;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            const Token fname = consume(TokenType::IDENTIFIER, "Expected field name in construct block");
            consume(TokenType::COLON, "Expected ':' after field name in construct block");
            auto val = parseExpression();
            fields.emplace_back(fname.lexeme, std::move(val));
            // Allow trailing comma
            if (check(TokenType::COMMA))
                advance();
        }
        consume(TokenType::RBRACE, "Expected '}' to close construct block");
        consume(TokenType::SEMICOLON, "Expected ';' after construct statement");
        auto stmt = std::make_unique<ConstructStmt>(std::move(target), /*typeName=*/"", std::move(fields));
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }

    return parseExprStmt();
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    consume(TokenType::LBRACE, "Expected '{'");

    std::vector<std::unique_ptr<Statement>> statements;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        // Multi-variable declarations: var a = 1, b = 2;
        if (check(TokenType::VAR) || check(TokenType::CONST)) {
            const bool isConst = check(TokenType::CONST);
            advance(); // consume var/const
            // Check for array destructuring: var [a, b, c] = expr;
            if (check(TokenType::LBRACKET)) {
                auto decls = parseDestructuringDecl(isConst);
                for (auto& d : decls) {
                    statements.push_back(std::move(d));
                }
                consume(TokenType::SEMICOLON, "Expected ';' after destructuring declaration");
            } else if (check(TokenType::LPAREN)) {
                // Tuple destructuring: var (a, b) = expr;
                auto decls = parseTupleDestructuringDecl(isConst);
                for (auto& d : decls) {
                    statements.push_back(std::move(d));
                }
                consume(TokenType::SEMICOLON, "Expected ';' after tuple destructuring declaration");
            } else {
                // Multi-variable declarations: var a:T = 1, b = 2, c = 3;
                // The type annotation on the first variable is propagated to
                // subsequent variables that omit a type annotation.
                auto firstDecl = parseVarDecl(isConst);
                // Capture the first var's type for propagation.
                std::string multiVarType;
                if (auto* vd = dynamic_cast<VarDecl*>(firstDecl.get()))
                    multiVarType = vd->typeName;
                statements.push_back(std::move(firstDecl));
                while (match(TokenType::COMMA)) {
                    // Pass the inherited type so subsequent vars like `b = 2`
                    // don't fail the mandatory-annotation check when they
                    // omit the type (they inherit from `a:T`).
                    statements.push_back(parseVarDeclWithInheritedType(isConst, multiVarType));
                }
                consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");
            }
        } else {
            statements.push_back(parseStatement());
        }
    }

    consume(TokenType::RBRACE, "Expected '}'");

    return std::make_unique<BlockStmt>(std::move(statements));
}

std::unique_ptr<Statement> Parser::parseVarDecl(bool isConst) {
    // Allow `var mut name: &T = &expr;` as shorthand for `borrow mut name: &T = &expr;`.
    // Consume an optional leading `mut` before the name.
    bool isBorrowMut = false;
    if (check(TokenType::MUT)) {
        isBorrowMut = true;
        advance(); // consume 'mut'
    }
    const Token name = consume(TokenType::IDENTIFIER, "Expected variable name");
    std::string typeName;
    if (match(TokenType::COLON)) {
        typeName = parseTypeAnnotation();
    } else if (inOptMaxFunction) {
        error("OPTMAX variables must include type annotations");
    }

    std::unique_ptr<Expression> initializer = nullptr;
    if (match(TokenType::ASSIGN)) {
        initializer = parseExpression();
    }

    // ── Type annotation enforcement ───────────────────────────────────────
    // Every variable must be typed, but we accept two forms:
    //   1. an explicit annotation (`var x:i64 = …`), or
    //   2. an initializer from which the type can be inferred at codegen
    //      time (`var x = 42;` → i64, `var s = "hi"` → string, etc.).
    // The codegen already tracks variable kinds (array/string/struct/dict)
    // from the initializer's AST shape and falls back to i64 for plain
    // numeric expressions, so omitting `:T` is safe whenever an initializer
    // is present.  An uninitialised, untyped declaration however leaves the
    // compiler with nothing to work from and remains an error.
    if (typeName.empty() && !initializer) {
        error("Variable '" + name.lexeme +
              "' requires an explicit type annotation "
              "(e.g., 'var " +
              name.lexeme +
              ":i64 = ...'). "
              "Untyped variables are not allowed; the compiler no longer "
              "silently defaults to 'i64'.");
    }

    // `File` variables must be initialized immediately with file_open(...).
    // A bare `var f: File` without an initializer would hold an uninitialized
    // file handle, which is always a bug.
    if (typeName == "File" && !initializer) {
        error("Variable '" + name.lexeme +
              "' of type 'File' must be initialized with file_open(...). "
              "Use: var " + name.lexeme + ": File = file_open(path, mode);");
    }

    // Shorthand borrow: `var name: &T = &expr;` → equivalent to `borrow var name: &T = &expr;`
    // If the type annotation is a reference type (&T) and an initializer is present,
    // automatically wrap the initializer in a BorrowExpr so the reference codegen path fires.
    if (!typeName.empty() && typeName[0] == '&' && initializer && initializer->type != ASTNodeType::BORROW_EXPR) {
        auto borrowExpr = std::make_unique<BorrowExpr>(std::move(initializer), isBorrowMut);
        borrowExpr->line = name.line;
        borrowExpr->column = name.column;
        initializer = std::move(borrowExpr);
    }

    // Compile-time validation: `ptr` variables may only be initialized with
    // expressions that could plausibly produce a pointer at runtime.
    // We reject only the most obvious misuse: a non-zero integer literal
    // (e.g. `var p: ptr = 42`) or a float/string literal, which can never
    // be a valid pointer in OmScript.
    // Everything else — function calls (builtins or user-defined), address-of,
    // identifiers, binary expressions, if-expressions, casts, etc. — is
    // accepted here and any type mismatch is caught by the code generator.
    if ((typeName == "ptr" || typeName.rfind("ptr<", 0) == 0) && initializer) {
        bool valid = true;
        const Expression* init = initializer.get();
        if (init->type == ASTNodeType::LITERAL_EXPR) {
            const auto* lit = static_cast<const LiteralExpr*>(init);
            // Only null (integer 0) is valid; non-zero integers, floats, and
            // string literals are rejected.
            valid = (lit->literalType == LiteralExpr::LiteralType::INTEGER && lit->intValue == 0);
        }
        if (!valid) {
            error("Pointer variable '" + name.lexeme +
                  "' cannot be initialized "
                  "with a non-pointer literal. Use &var, a function call, null/nullptr, "
                  "or another pointer expression.");
        }
    }

    auto decl = std::make_unique<VarDecl>(name.lexeme, std::move(initializer), isConst, typeName);
    decl->line = name.line;
    decl->column = name.column;
    return decl;
}

/// Variant of parseVarDecl used for subsequent variables in a multi-var
/// declaration (e.g., the `b = 2` and `c = 3` in `var a:i64 = 1, b = 2, c = 3`).
/// If the variable has no explicit `:type` annotation, `inheritedType` is used
/// instead, which is the type of the first variable in the declaration.
std::unique_ptr<Statement> Parser::parseVarDeclWithInheritedType(bool isConst, const std::string& inheritedType) {
    const Token name = consume(TokenType::IDENTIFIER, "Expected variable name");
    std::string typeName;
    if (match(TokenType::COLON)) {
        typeName = parseTypeAnnotation();
    } else {
        // Inherit from the first variable in the multi-var declaration.
        typeName = inheritedType;
    }

    std::unique_ptr<Expression> initializer = nullptr;
    if (match(TokenType::ASSIGN)) {
        initializer = parseExpression();
    }

    // Same relaxed rule as parseVarDecl: omit-type-with-init is allowed; a
    // bare `var x` with neither type nor initializer is the only error case.
    if (typeName.empty() && !initializer) {
        error("Variable '" + name.lexeme +
              "' requires an explicit type annotation "
              "(e.g., 'var " +
              name.lexeme + ":i64 = ...'). Untyped variables without an initializer are not allowed.");
    }

    auto decl = std::make_unique<VarDecl>(name.lexeme, std::move(initializer), isConst, typeName);
    decl->line = name.line;
    decl->column = name.column;
    return decl;
}

std::unique_ptr<VarDecl> Parser::parseGlobalDecl() {
    const Token kw = advance(); // consume 'global'
    // Accept optional `mut` qualifier: `global mut var name` — the explicit
    // mutable form chosen for v5.0 compatibility. Semantically identical to
    // `global var` in the current release.
    const bool hasMut = match(TokenType::MUT);
    (void)hasMut; // semantic enforcement reserved for v5.0
    const bool isConst = check(TokenType::CONST);
    if (!match(TokenType::VAR) && !match(TokenType::CONST)) {
        error("Expected 'var' or 'const' after 'global'");
    }
    const Token name = consume(TokenType::IDENTIFIER, "Expected variable name after 'global var'");
    std::string typeName;
    if (match(TokenType::COLON)) {
        typeName = parseTypeAnnotation();
    }
    std::unique_ptr<Expression> init = nullptr;
    if (match(TokenType::ASSIGN)) {
        init = parseExpression();
    }
    consume(TokenType::SEMICOLON, "Expected ';' after global variable declaration");
    auto decl = std::make_unique<VarDecl>(name.lexeme, std::move(init), isConst, typeName);
    decl->isGlobal = true;
    decl->line = kw.line;
    decl->column = kw.column;
    return decl;
}

std::unique_ptr<Statement> Parser::parseIfStmt() {
    // Allow optional parentheses: `if (cond) { }` and `if cond { }` both work.
    const bool hasParen = match(TokenType::LPAREN);
    auto condition = parseExpression();
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after condition");

    auto thenBranch = parseStatement();
    std::unique_ptr<Statement> elseBranch = nullptr;

    if (match(TokenType::ELSE)) {
        elseBranch = parseStatement();
    } else if (match(TokenType::ELIF)) {
        // elif (cond) { ... } => else if (cond) { ... }
        auto elifStmt = parseElifStmt();
        elifStmt->line = tokens[current - 1].line;
        elifStmt->column = tokens[current - 1].column;
        elseBranch = std::move(elifStmt);
    }

    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

std::unique_ptr<Statement> Parser::parseWhileStmt() {
    // Allow optional parentheses: `while (cond) { }` and `while cond { }` both work.
    const bool hasParen = match(TokenType::LPAREN);
    auto condition = parseExpression();
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after condition");

    LoopConfig loopHints;
    if (check(TokenType::AT) && current + 1 < tokens.size() && tokens[current + 1].lexeme == "loop") {
        advance(); // @
        advance(); // loop
        loopHints = parseLoopAnnotation();
    }
    auto body = parseStatement();
    auto stmt = std::make_unique<WhileStmt>(std::move(condition), std::move(body));
    stmt->loopHints = loopHints;
    return stmt;
}

std::unique_ptr<Statement> Parser::parseDoWhileStmt() {
    LoopConfig loopHints;
    if (check(TokenType::AT) && current + 1 < tokens.size() && tokens[current + 1].lexeme == "loop") {
        advance(); // @
        advance(); // loop
        loopHints = parseLoopAnnotation();
    }
    auto body = parseStatement();

    // Support both: do { ... } while (cond); and do { ... } until (cond);
    bool isUntil = false;
    if (match(TokenType::UNTIL)) {
        isUntil = true;
    } else {
        consume(TokenType::WHILE, "Expected 'while' or 'until' after do block");
    }
    consume(TokenType::LPAREN, isUntil ? "Expected '(' after 'until'" : "Expected '(' after 'while'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after condition");
    consume(TokenType::SEMICOLON, "Expected ';' after do-while/until statement");

    if (isUntil) {
        // Negate: do { ... } until (c) => do { ... } while (!c)
        auto negated = std::make_unique<UnaryExpr>("!", std::move(condition));
        auto doStmt = std::make_unique<DoWhileStmt>(std::move(body), std::move(negated));
        doStmt->loopHints = loopHints;
        return doStmt;
    }
    auto doStmt = std::make_unique<DoWhileStmt>(std::move(body), std::move(condition));
    doStmt->loopHints = loopHints;
    return doStmt;
}

std::unique_ptr<Statement> Parser::parseForStmt() {
    // Allow optional parentheses: `for (var in ...)` and `for var in ...` both work.
    // Also support:
    //   for x in arr { }           -- paren-free for-each over array/string
    //   for i in start..end { }    -- paren-free half-open range (exclusive end)
    //   for i in start..=end { }   -- paren-free inclusive range
    //   for i in start...end { }   -- classic OmScript range (exclusive, parens optional)
    const bool hasParen = match(TokenType::LPAREN);

    // Parse: for (var in start...end) or for (var in start...end...step)
    //    or: for (var in collection)  -- for-each over array
    //    or: for (idx, item in collection)  -- indexed for-each
    const Token varName = consume(TokenType::IDENTIFIER, "Expected iterator variable");

    // Check for indexed for-each: for (idx, item in collection)
    if (match(TokenType::COMMA)) {
        const Token itemName = consume(TokenType::IDENTIFIER, "Expected item variable after ',' in for");
        consume(TokenType::IN, "Expected 'in' after for variables");
        auto collection = parseExpression();
        if (hasParen)
            consume(TokenType::RPAREN, "Expected ')' after for-each collection");
        auto body = parseStatement();

        // Desugar to: { var __arr = collection; for (idx in 0...len(__arr)) { var item = __arr[idx]; body } }
        static int forIdxCounter = 0;
        const int id = forIdxCounter++;
        const std::string arrTmp = "__for_arr_" + std::to_string(id);

        std::vector<std::unique_ptr<Statement>> outerStmts;

        // var __for_arr_N = collection;
        auto arrDecl = std::make_unique<VarDecl>(arrTmp, std::move(collection));
        arrDecl->isCompilerGenerated = true;
        arrDecl->line = varName.line;
        arrDecl->column = varName.column;
        outerStmts.push_back(std::move(arrDecl));

        // Build inner body: { var item = __for_arr_N[idx]; original_body }
        std::vector<std::unique_ptr<Statement>> innerStmts;
        auto arrRef = std::make_unique<IdentifierExpr>(arrTmp);
        auto idxRef = std::make_unique<IdentifierExpr>(varName.lexeme);
        auto indexExpr = std::make_unique<IndexExpr>(std::move(arrRef), std::move(idxRef));
        auto itemDecl = std::make_unique<VarDecl>(itemName.lexeme, std::move(indexExpr));
        itemDecl->isCompilerGenerated = true;
        itemDecl->line = itemName.line;
        itemDecl->column = itemName.column;
        innerStmts.push_back(std::move(itemDecl));
        innerStmts.push_back(std::move(body));
        auto innerBlock = std::make_unique<BlockStmt>(std::move(innerStmts));

        // for (idx in 0...len(__for_arr_N))
        auto zero = std::make_unique<LiteralExpr>(static_cast<long long>(0));
        std::vector<std::unique_ptr<Expression>> lenArgs;
        lenArgs.push_back(std::make_unique<IdentifierExpr>(arrTmp));
        auto lenCall = std::make_unique<CallExpr>("len", std::move(lenArgs));
        lenCall->fromStdNamespace = true; // compiler-generated
        auto forStmt = std::make_unique<ForStmt>(varName.lexeme, std::move(zero), std::move(lenCall), nullptr,
                                                 std::move(innerBlock));
        forStmt->line = varName.line;
        forStmt->column = varName.column;
        outerStmts.push_back(std::move(forStmt));

        return std::make_unique<BlockStmt>(std::move(outerStmts));
    }

    std::string iteratorType;
    if (match(TokenType::COLON)) {
        iteratorType = parseTypeAnnotation();
    } else if (inOptMaxFunction) {
        error("OPTMAX loop variables must include type annotations");
    }
    consume(TokenType::IN, "Expected 'in' after iterator variable");

    auto firstExpr = parseExpression();

    // `..=` inclusive range: for i in start..=end  => for (i in start...end+1)
    // Peek ahead to detect `..=` before consuming anything.
    if (check(TokenType::DOT_DOT) && peek(1).type == TokenType::ASSIGN) {
        advance(); // consume '..'
        advance(); // consume '='
        auto endExpr = parseExpression();
        // Desugar: end+1
        auto one = std::make_unique<LiteralExpr>(static_cast<long long>(1));
        auto endPlusOne = std::make_unique<BinaryExpr>("+", std::move(endExpr), std::move(one));

        std::unique_ptr<Expression> step = nullptr;
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "step") {
            advance(); // consume 'step'
            step = parseExpression();
        }
        if (hasParen)
            consume(TokenType::RPAREN, "Expected ')' after for range");
        LoopConfig loopHints;
        if (check(TokenType::AT) && current + 1 < tokens.size() && tokens[current + 1].lexeme == "loop") {
            advance(); advance();
            loopHints = parseLoopAnnotation();
        }
        std::unique_ptr<Expression> whereFilter_r = nullptr;
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "where") {
            advance();
            whereFilter_r = parseExpression();
        }
        auto body = parseStatement();
        if (whereFilter_r) {
            std::vector<std::unique_ptr<Statement>> innerStmts;
            innerStmts.push_back(std::move(body));
            auto innerBlock = std::make_unique<BlockStmt>(std::move(innerStmts));
            body = std::make_unique<IfStmt>(std::move(whereFilter_r), std::move(innerBlock));
        }
        auto forStmt = std::make_unique<ForStmt>(varName.lexeme, std::move(firstExpr), std::move(endPlusOne),
                                                 std::move(step), std::move(body), iteratorType);
        forStmt->loopHints = loopHints;
        return forStmt;
    }

    // If next token is '...' or '..', this is a range-based for loop
    if (match(TokenType::RANGE) || match(TokenType::DOT_DOT)) {
        auto end = parseExpression();

        std::unique_ptr<Expression> step = nullptr;
        // Support both: for (i in 0...10...2) and for (i in 0...10 step 2)
        if (match(TokenType::RANGE)) {
            step = parseExpression();
        } else if (check(TokenType::IDENTIFIER) && peek().lexeme == "step") {
            advance(); // consume 'step'
            step = parseExpression();
        }

        if (hasParen)
            consume(TokenType::RPAREN, "Expected ')' after for range");
        LoopConfig loopHints;
        if (check(TokenType::AT) && current + 1 < tokens.size() && tokens[current + 1].lexeme == "loop") {
            advance(); // @
            advance(); // loop
            loopHints = parseLoopAnnotation();
        }
        std::unique_ptr<Expression> whereFilter_r = nullptr;
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "where") {
            advance();
            whereFilter_r = parseExpression();
        }
        auto body = parseStatement();
        if (whereFilter_r) {
            std::vector<std::unique_ptr<Statement>> innerStmts;
            innerStmts.push_back(std::move(body));
            auto innerBlock = std::make_unique<BlockStmt>(std::move(innerStmts));
            body = std::make_unique<IfStmt>(std::move(whereFilter_r), std::move(innerBlock));
        }
        auto forStmt = std::make_unique<ForStmt>(varName.lexeme, std::move(firstExpr), std::move(end), std::move(step),
                                                 std::move(body), iteratorType);
        forStmt->loopHints = loopHints;
        return forStmt;
    }

    // for (i in 10 downto 0) => for (i in 10...0...-1)
    // for (i in 10 downto 0 step 2) => for (i in 10...0...-2)
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "downto") {
        advance(); // consume 'downto'
        auto end = parseExpression();

        if (check(TokenType::IDENTIFIER) && peek().lexeme == "step") {
            advance(); // consume 'step'
            auto stepExpr = parseExpression();
            // The step is positive in user syntax, we negate it for downto
            // Desugar to: for (i in start...end...-step)
            if (hasParen)
                consume(TokenType::RPAREN, "Expected ')' after for downto range");
            LoopConfig loopHints;
            if (check(TokenType::AT) && current + 1 < tokens.size() && tokens[current + 1].lexeme == "loop") {
                advance(); // @
                advance(); // loop
                loopHints = parseLoopAnnotation();
            }
            auto body = parseStatement();
            auto negStep = std::make_unique<UnaryExpr>("-", std::move(stepExpr));
            auto forStmt = std::make_unique<ForStmt>(varName.lexeme, std::move(firstExpr), std::move(end),
                                                     std::move(negStep), std::move(body), iteratorType);
            forStmt->loopHints = loopHints;
            return forStmt;
        }

        if (hasParen)
            consume(TokenType::RPAREN, "Expected ')' after for downto range");
        LoopConfig loopHints;
        if (check(TokenType::AT) && current + 1 < tokens.size() && tokens[current + 1].lexeme == "loop") {
            advance(); // @
            advance(); // loop
            loopHints = parseLoopAnnotation();
        }
        auto body = parseStatement();
        auto negOne = std::make_unique<LiteralExpr>(static_cast<long long>(-1));
        auto forStmt = std::make_unique<ForStmt>(varName.lexeme, std::move(firstExpr), std::move(end),
                                                 std::move(negOne), std::move(body), iteratorType);
        forStmt->loopHints = loopHints;
        return forStmt;
    }

    // Otherwise this is a for-each loop: for (var in collection)
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after for-each collection");
    std::unique_ptr<Expression> whereFilter = nullptr;
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "where") {
        advance(); // consume 'where'
        whereFilter = parseExpression();
    }
    auto body = parseStatement();
    if (whereFilter) {
        std::vector<std::unique_ptr<Statement>> innerStmts;
        innerStmts.push_back(std::move(body));
        auto innerBlock = std::make_unique<BlockStmt>(std::move(innerStmts));
        body = std::make_unique<IfStmt>(std::move(whereFilter), std::move(innerBlock));
    }
    return std::make_unique<ForEachStmt>(varName.lexeme, std::move(firstExpr), std::move(body));
}

std::unique_ptr<Statement> Parser::parseBreakStmt() {
    auto stmt = std::make_unique<BreakStmt>();
    // Labeled break: break label_name;
    if (check(TokenType::IDENTIFIER)) {
        stmt->label = tokens[current].lexeme;
        advance();
    }
    consume(TokenType::SEMICOLON, "Expected ';' after 'break'");
    return stmt;
}

std::unique_ptr<Statement> Parser::parseContinueStmt() {
    auto stmt = std::make_unique<ContinueStmt>();
    // Labeled continue: continue label_name;
    if (check(TokenType::IDENTIFIER)) {
        stmt->label = tokens[current].lexeme;
        advance();
    }
    consume(TokenType::SEMICOLON, "Expected ';' after 'continue'");
    return stmt;
}

std::unique_ptr<Statement> Parser::parseJmpStmt() {
    // `jmp` keyword was already consumed by the caller.
    // Emit a deprecation warning at the use site.
    const Token jmpTok = tokens[current - 1];
    warnings_.push_back("line " + std::to_string(jmpTok.line) + ":" +
                        std::to_string(jmpTok.column) +
                        ": warning: `jmp` is deprecated; prefer structured control flow "
                        "(if / while / for / break / continue). "
                        "`jmp` will be removed in a future version of OmScript.");
    const Token labelTok = consume(TokenType::IDENTIFIER, "Expected label name after 'jmp'");
    consume(TokenType::SEMICOLON, "Expected ';' after 'jmp <label>'");
    auto stmt = std::make_unique<JmpStmt>(labelTok.lexeme);
    stmt->line = jmpTok.line;
    stmt->column = jmpTok.column;
    return stmt;
}

std::unique_ptr<Statement> Parser::parseLabelStmt() {
    // `label` keyword was already consumed by the caller.
    const Token labelTok = consume(TokenType::IDENTIFIER, "Expected label name after 'label'");
    consume(TokenType::COLON, "Expected ':' after label name (syntax: 'label name:')");
    auto stmt = std::make_unique<LabelStmt>(labelTok.lexeme);
    stmt->line = labelTok.line;
    stmt->column = labelTok.column;
    return stmt;
}

std::unique_ptr<Statement> Parser::parseSwitchStmt() {
    // Paren-free form: switch x { ... }  (same as if/while/for Round-70)
    const bool hasParen = match(TokenType::LPAREN);
    auto condition = parseExpression();
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after switch condition");
    consume(TokenType::LBRACE, "Expected '{' after switch condition");

    std::vector<SwitchCase> cases;
    bool hasDefault = false;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (match(TokenType::CASE)) {
            // Parse multiple comma-separated case values: case 1, 2, 3:
            std::vector<std::unique_ptr<Expression>> caseValues;
            caseValues.push_back(parseExpression());
            while (match(TokenType::COMMA)) {
                caseValues.push_back(parseExpression());
            }
            consume(TokenType::COLON, "Expected ':' after case value(s)");

            std::vector<std::unique_ptr<Statement>> body;
            while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) && !check(TokenType::RBRACE) && !isAtEnd()) {
                body.push_back(parseStatement());
            }
            cases.emplace_back(std::move(caseValues), std::move(body), false);
        } else if (match(TokenType::DEFAULT)) {
            if (hasDefault) {
                error("Duplicate default case in switch statement");
            }
            hasDefault = true;
            consume(TokenType::COLON, "Expected ':' after 'default'");

            std::vector<std::unique_ptr<Statement>> body;
            while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) && !check(TokenType::RBRACE) && !isAtEnd()) {
                body.push_back(parseStatement());
            }
            cases.emplace_back(nullptr, std::move(body), true);
        } else {
            error("Expected 'case' or 'default' in switch statement");
        }
    }

    consume(TokenType::RBRACE, "Expected '}' after switch body");
    return std::make_unique<SwitchStmt>(std::move(condition), std::move(cases));
}

std::unique_ptr<Statement> Parser::parseReturnStmt() {
    std::unique_ptr<Expression> value = nullptr;

    if (!check(TokenType::SEMICOLON)) {
        value = parseExpression();
    }

    consume(TokenType::SEMICOLON, "Expected ';' after return statement");

    return std::make_unique<ReturnStmt>(std::move(value));
}

std::unique_ptr<Statement> Parser::parseCatchStmt() {
    // catch(N) { body }    where N is an integer or string literal
    consume(TokenType::LPAREN, "Expected '(' after 'catch'");
    if (check(TokenType::INTEGER)) {
        const Token codeToken = advance();
        int64_t code = 0;
        code = codeToken.intValue;
        consume(TokenType::RPAREN, "Expected ')' after catch code");
        auto body = parseBlock();
        return std::make_unique<CatchStmt>(code, std::move(body));
    } else if (check(TokenType::STRING)) {
        const Token codeToken = advance();
        consume(TokenType::RPAREN, "Expected ')' after catch code");
        auto body = parseBlock();
        return std::make_unique<CatchStmt>(codeToken.lexeme, std::move(body));
    } else {
        error("catch requires an integer or string literal error code");
        return nullptr;
    }
}

std::unique_ptr<Statement> Parser::parseThrowStmt() {
    auto value = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after throw expression");
    return std::make_unique<ThrowStmt>(std::move(value));
}

std::unique_ptr<Statement> Parser::parseEnsureStmt() {
    // ensure condition;
    // ensure condition, "message";
    // Desugars to a call to the 'ensure' builtin (always-active, like assert but never elided)
    const Token startTok = peek();
    auto condition = parseExpression();
    std::unique_ptr<Expression> message = nullptr;
    if (match(TokenType::COMMA)) {
        message = parseExpression();
    }
    consume(TokenType::SEMICOLON, "Expected ';' after ensure statement");

    std::vector<std::unique_ptr<Expression>> args;
    args.push_back(std::move(condition));
    if (message) args.push_back(std::move(message));
    auto call = std::make_unique<CallExpr>("ensure", std::move(args));
    call->fromStdNamespace = true;
    call->line = startTok.line;
    call->column = startTok.column;
    return std::make_unique<ExprStmt>(std::move(call));
}

// unless (condition) { ... } else { ... }
// Desugars to: if (!condition) { ... } else { ... }
// Paren-free form accepted: unless cond { ... }  (same as if/while/for)
std::unique_ptr<Statement> Parser::parseUnlessStmt() {
    const bool hasParen = match(TokenType::LPAREN);
    auto condition = parseExpression();
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after condition");

    auto thenBranch = parseStatement();
    std::unique_ptr<Statement> elseBranch = nullptr;

    if (match(TokenType::ELSE)) {
        elseBranch = parseStatement();
    }

    // Negate the condition: unless (c) => if (!c)
    auto negated = std::make_unique<UnaryExpr>("!", std::move(condition));
    return std::make_unique<IfStmt>(std::move(negated), std::move(thenBranch), std::move(elseBranch));
}

// until (condition) { ... }
// Desugars to: while (!condition) { ... }
// Paren-free form accepted: until cond { ... }  (same as if/while/for)
std::unique_ptr<Statement> Parser::parseUntilStmt() {
    const bool hasParen = match(TokenType::LPAREN);
    auto condition = parseExpression();
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after condition");

    auto body = parseStatement();

    // Negate the condition: until (c) => while (!c)
    auto negated = std::make_unique<UnaryExpr>("!", std::move(condition));
    return std::make_unique<WhileStmt>(std::move(negated), std::move(body));
}

// loop { ... }
// Desugars to: while (true) { ... }
std::unique_ptr<Statement> Parser::parseLoopStmt() {
    // Two forms:
    // 1. loop { ... } — infinite loop (while true)
    // 2. loop N { ... } or loop (N) { ... } — counted loop (for __i in 0...N)
    if (check(TokenType::LBRACE)) {
        auto body = parseStatement();
        // Infinite loop: loop { ... } => while (true) { ... }
        auto trueVal = std::make_unique<LiteralExpr>(static_cast<long long>(1));
        return std::make_unique<WhileStmt>(std::move(trueVal), std::move(body));
    }

    // Counted form: loop N { ... } or loop (N) { ... }
    const bool hasParen = match(TokenType::LPAREN);
    auto count = parseExpression();
    if (hasParen) {
        consume(TokenType::RPAREN, "Expected ')' after loop count");
    }
    auto body = parseStatement();

    // Desugar to: for (__loop_N in 0...count) { body }
    static int loopCounter = 0;
    const std::string iterVar = "__loop_" + std::to_string(loopCounter++);
    auto start = std::make_unique<LiteralExpr>(static_cast<long long>(0));
    return std::make_unique<ForStmt>(iterVar, std::move(start), std::move(count), nullptr, std::move(body));
}

// repeat N { ... }  or  repeat (expr) { ... }
// Desugars to: for (__repeat_i in 0...N) { ... }
std::unique_ptr<Statement> Parser::parseRepeatStmt() {
    // Two forms:
    // 1. repeat N { ... }  or  repeat (N) { ... }  — counted loop
    // 2. repeat { ... } until (cond);               — post-test loop

    // If next token is '{', this is the repeat...until form
    if (check(TokenType::LBRACE)) {
        auto body = parseStatement();
        consume(TokenType::UNTIL, "Expected 'until' after repeat block");
        consume(TokenType::LPAREN, "Expected '(' after 'until'");
        auto condition = parseExpression();
        consume(TokenType::RPAREN, "Expected ')' after until condition");
        consume(TokenType::SEMICOLON, "Expected ';' after repeat...until");

        // Negate: repeat { ... } until (c) => do { ... } while (!c)
        auto negated = std::make_unique<UnaryExpr>("!", std::move(condition));
        return std::make_unique<DoWhileStmt>(std::move(body), std::move(negated));
    }

    // Counted form: repeat N { ... } or repeat (N) { ... }
    const bool hasParen = match(TokenType::LPAREN);
    auto count = parseExpression();
    if (hasParen) {
        consume(TokenType::RPAREN, "Expected ')' after repeat count");
    }

    auto body = parseStatement();

    // Desugar to: for (__repeat_N in 0...count) { body }
    static int repeatCounter = 0;
    const std::string iterVar = "__repeat_" + std::to_string(repeatCounter++);
    auto start = std::make_unique<LiteralExpr>(static_cast<long long>(0));
    return std::make_unique<ForStmt>(iterVar, std::move(start), std::move(count), nullptr, std::move(body));
}

// defer statement;  or  defer { ... }
// Stores the statement to be executed at the end of the enclosing block
std::unique_ptr<Statement> Parser::parseDeferStmt() {
    auto body = parseStatement();
    return std::make_unique<DeferStmt>(std::move(body));
}

// guard (condition) else { ... }
// Desugars to: if (!condition) { body }
// Used for early-exit / precondition patterns:
//   guard (x > 0) else { return -1; }
std::unique_ptr<Statement> Parser::parseGuardStmt() {
    // Paren-free form: guard cond else { ... }  (same as if/unless/while/for)
    const bool hasParen = match(TokenType::LPAREN);
    auto condition = parseExpression();
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after guard condition");
    consume(TokenType::ELSE, "Expected 'else' after guard condition");

    auto elseBody = parseStatement();

    // Negate the condition: guard (c) else { ... } => if (!c) { ... }
    auto negated = std::make_unique<UnaryExpr>("!", std::move(condition));
    return std::make_unique<IfStmt>(std::move(negated), std::move(elseBody));
}

// when (expr) { val1 => { stmts }, val2, val3 => { stmts }, _ => { stmts } }
// Desugars to a switch statement with fat-arrow syntax
std::unique_ptr<Statement> Parser::parseWhenStmt() {
    // Paren-free form: when x { ... }  (same as if/while/for Round-70)
    const bool hasParen = match(TokenType::LPAREN);
    auto condition = parseExpression();
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after when expression");
    consume(TokenType::LBRACE, "Expected '{' after when expression");

    std::vector<SwitchCase> cases;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        // Check for default case: _ => { ... }
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
            advance(); // consume '_'
            consume(TokenType::FAT_ARROW, "Expected '=>' after '_' in when clause");
            std::vector<std::unique_ptr<Statement>> body;
            body.push_back(parseStatement());
            cases.emplace_back(std::vector<std::unique_ptr<Expression>>{}, std::move(body), true);
        } else {
            // Parse one or more case values: val1, val2, val3 => { ... }
            std::vector<std::unique_ptr<Expression>> values;
            values.push_back(parseExpression());
            while (match(TokenType::COMMA)) {
                // Check if next is '=>' (end of value list)
                if (check(TokenType::FAT_ARROW))
                    break;
                values.push_back(parseExpression());
            }
            consume(TokenType::FAT_ARROW, "Expected '=>' after value(s) in when clause");
            std::vector<std::unique_ptr<Statement>> body;
            body.push_back(parseStatement());
            cases.emplace_back(std::move(values), std::move(body), false);
        }

        // Allow optional comma between arms
        match(TokenType::COMMA);
    }

    consume(TokenType::RBRACE, "Expected '}' to close when block");
    return std::make_unique<SwitchStmt>(std::move(condition), std::move(cases));
}

std::unique_ptr<Expression> Parser::parseWhenExpr() {
    // when val { 1 => expr1, 2 => expr2, _ => expr3 }
    // Desugars to ternary chain: (val==1) ? e1 : (val==2) ? e2 : e3
    // The discriminant may be any expression.
    // If it is not a bare identifier, it is lifted into a synthetic IIFE so
    // it is evaluated exactly once: (|__d| ternary)( expr )
    const bool hasParen = match(TokenType::LPAREN);
    auto discriminant = parseExpression();
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after when expression discriminant");

    // Determine the identifier name used for comparisons.
    // When the discriminant is already a bare variable we use it directly;
    // otherwise we generate a synthetic param name and wrap the result
    // in an IIFE lambda call so the expression is evaluated exactly once.
    const bool isIdent = discriminant->type == ASTNodeType::IDENTIFIER_EXPR;
    std::string discName;
    if (isIdent) {
        discName = static_cast<IdentifierExpr*>(discriminant.get())->name;
    } else {
        discName = "__when_disc_" + std::to_string(lambdaCounter_);
    }

    consume(TokenType::LBRACE, "Expected '{' after when expression");

    struct WhenArm {
        std::vector<std::unique_ptr<Expression>> values; // empty means default
        std::unique_ptr<Expression> result;
    };
    std::vector<WhenArm> arms;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        WhenArm arm;
        bool isDefault = false;

        if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
            advance(); // consume '_'
            consume(TokenType::FAT_ARROW, "Expected '=>' after '_' in when expression");
            arm.result = parseExpression();
            isDefault = true;
        } else {
            arm.values.push_back(parseExpression());
            while (match(TokenType::COMMA)) {
                if (check(TokenType::FAT_ARROW)) break;
                arm.values.push_back(parseExpression());
            }
            consume(TokenType::FAT_ARROW, "Expected '=>' after value in when expression");
            arm.result = parseExpression();
        }
        arms.push_back(std::move(arm));
        match(TokenType::COMMA);
        if (isDefault) break;
    }
    consume(TokenType::RBRACE, "Expected '}' to close when expression");

    // Build ternary chain from back to front
    std::unique_ptr<Expression> result = nullptr;
    int startIdx = static_cast<int>(arms.size()) - 1;

    // Check if last arm is default (_)
    if (!arms.empty() && arms.back().values.empty()) {
        result = std::move(arms.back().result);
        startIdx = static_cast<int>(arms.size()) - 2;
    } else {
        // No default: fallback to 0
        result = std::make_unique<LiteralExpr>(static_cast<long long>(0));
        startIdx = static_cast<int>(arms.size()) - 1;
    }

    for (int i = startIdx; i >= 0; --i) {
        auto& arm = arms[i];
        if (arm.values.empty()) continue;

        // Build condition: discName == v1 || discName == v2 || ...
        std::unique_ptr<Expression> cond = nullptr;
        for (auto& v : arm.values) {
            auto discRef = std::make_unique<IdentifierExpr>(discName);
            auto eq = std::make_unique<BinaryExpr>("==", std::move(discRef), std::move(v));
            if (!cond) {
                cond = std::move(eq);
            } else {
                cond = std::make_unique<BinaryExpr>("||", std::move(cond), std::move(eq));
            }
        }

        result = std::make_unique<TernaryExpr>(std::move(cond), std::move(arm.result), std::move(result));
    }

    // If the discriminant was a non-identifier expression, wrap in an IIFE
    // (|__when_disc_N| ternary_chain)(discriminant) so it is evaluated once.
    if (!isIdent) {
        const std::string lambdaName = "__lambda_" + std::to_string(lambdaCounter_++);
        Parameter param(discName);
        param.typeName = "i64"; // default; codegen infers actual type
        std::vector<Parameter> fnParams;
        fnParams.push_back(std::move(param));

        auto returnStmt = std::make_unique<ReturnStmt>(std::move(result));
        std::vector<std::unique_ptr<Statement>> stmts;
        stmts.push_back(std::move(returnStmt));
        auto block = std::make_unique<BlockStmt>(std::move(stmts));
        auto fnDecl = std::make_unique<FunctionDecl>(lambdaName, std::vector<std::string>{},
                                                      std::move(fnParams), std::move(block));
        lambdaFunctions_.push_back(std::move(fnDecl));

        // Build call: __lambda_N(discriminant)
        std::vector<std::unique_ptr<Expression>> callArgs;
        callArgs.push_back(std::move(discriminant));
        return std::make_unique<CallExpr>(lambdaName, std::move(callArgs));
    }

    return result;
}


// forever { ... }
// Desugars to: while (true) { ... }
std::unique_ptr<Statement> Parser::parseForeverStmt() {
    auto body = parseStatement();

    // Infinite loop: forever { ... } => while (true) { ... }
    auto trueVal = std::make_unique<LiteralExpr>(static_cast<long long>(1));
    return std::make_unique<WhileStmt>(std::move(trueVal), std::move(body));
}

// foreach item in collection { ... }
// foreach (i, item in collection) { ... } — indexed variant
// Desugars to: for (item in collection) { ... } (ForEachStmt)
// Indexed variant desugars to:
//   { var __foreach_arr_N = collection;
//     for (__foreach_idx_N in 0...len(__foreach_arr_N)) {
//       var i = __foreach_idx_N;
//       var item = __foreach_arr_N[__foreach_idx_N];
//       body } }
std::unique_ptr<Statement> Parser::parseForEachStmt() {
    // Allow optional parens: foreach item in arr or foreach (item in arr)
    const bool hasParen = match(TokenType::LPAREN);
    const Token firstName = consume(TokenType::IDENTIFIER, "Expected iterator variable after 'foreach'");

    // Check for indexed variant: foreach (idx, item in collection)
    if (match(TokenType::COMMA)) {
        // This is the indexed form: foreach (idx, item in collection)
        const Token itemName = consume(TokenType::IDENTIFIER, "Expected item variable after ',' in foreach");
        consume(TokenType::IN, "Expected 'in' after foreach variables");
        auto collection = parseExpression();
        if (hasParen) {
            consume(TokenType::RPAREN, "Expected ')' after foreach collection");
        }
        auto body = parseStatement();

        // Desugar to: { var __arr = collection; for (idx in 0...len(__arr)) { var item = __arr[idx]; body } }
        static int foreachIdxCounter = 0;
        const int id = foreachIdxCounter++;
        const std::string arrTmp = "__foreach_arr_" + std::to_string(id);

        std::vector<std::unique_ptr<Statement>> outerStmts;

        // var __foreach_arr_N = collection;
        auto arrDecl = std::make_unique<VarDecl>(arrTmp, std::move(collection));
        arrDecl->isCompilerGenerated = true;
        arrDecl->line = firstName.line;
        arrDecl->column = firstName.column;
        outerStmts.push_back(std::move(arrDecl));

        // Build inner body: { var item = __foreach_arr_N[idx]; original_body }
        std::vector<std::unique_ptr<Statement>> innerStmts;

        // var item = __foreach_arr_N[idx];
        auto arrRef = std::make_unique<IdentifierExpr>(arrTmp);
        auto idxRef = std::make_unique<IdentifierExpr>(firstName.lexeme);
        auto indexExpr = std::make_unique<IndexExpr>(std::move(arrRef), std::move(idxRef));
        auto itemDecl = std::make_unique<VarDecl>(itemName.lexeme, std::move(indexExpr));
        itemDecl->isCompilerGenerated = true;
        itemDecl->line = itemName.line;
        itemDecl->column = itemName.column;
        innerStmts.push_back(std::move(itemDecl));
        innerStmts.push_back(std::move(body));
        auto innerBlock = std::make_unique<BlockStmt>(std::move(innerStmts));

        // for (idx in 0...len(__foreach_arr_N))
        auto zero = std::make_unique<LiteralExpr>(static_cast<long long>(0));
        std::vector<std::unique_ptr<Expression>> lenArgs;
        lenArgs.push_back(std::make_unique<IdentifierExpr>(arrTmp));
        auto lenCall = std::make_unique<CallExpr>("len", std::move(lenArgs));
        lenCall->fromStdNamespace = true; // compiler-generated
        auto forStmt = std::make_unique<ForStmt>(firstName.lexeme, std::move(zero), std::move(lenCall), nullptr,
                                                 std::move(innerBlock));
        forStmt->line = firstName.line;
        forStmt->column = firstName.column;
        outerStmts.push_back(std::move(forStmt));

        return std::make_unique<BlockStmt>(std::move(outerStmts));
    }

    consume(TokenType::IN, "Expected 'in' after foreach variable");
    auto collection = parseExpression();
    if (hasParen) {
        consume(TokenType::RPAREN, "Expected ')' after foreach collection");
    }
    std::unique_ptr<Expression> whereFilter = nullptr;
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "where") {
        advance(); // consume 'where'
        whereFilter = parseExpression();
    }
    auto body = parseStatement();
    if (whereFilter) {
        std::vector<std::unique_ptr<Statement>> innerStmts;
        innerStmts.push_back(std::move(body));
        auto innerBlock = std::make_unique<BlockStmt>(std::move(innerStmts));
        body = std::make_unique<IfStmt>(std::move(whereFilter), std::move(innerBlock));
    }
    return std::make_unique<ForEachStmt>(firstName.lexeme, std::move(collection), std::move(body));
}

// elif (condition) { ... } [elif (...) { ... }] [else { ... }]
// Desugars to: if (condition) { ... } [else if (...) { ... }] [else { ... }]
std::unique_ptr<Statement> Parser::parseElifStmt() {
    // Allow optional parentheses: `elif (cond)` and `elif cond` both work.
    const bool hasParen = match(TokenType::LPAREN);
    auto condition = parseExpression();
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after elif condition");

    auto thenBranch = parseStatement();
    std::unique_ptr<Statement> elseBranch = nullptr;

    if (match(TokenType::ELIF)) {
        elseBranch = parseElifStmt();
    } else if (match(TokenType::ELSE)) {
        elseBranch = parseStatement();
    }

    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

// swap a, b;  or  swap a, b, c;  (circular rotation: a←b, b←c, c←old_a)
// Desugars to: { var __swap_tmp = a; a = b; b = c; ... last = __swap_tmp; }
std::unique_ptr<Statement> Parser::parseSwapStmt() {
    // Collect all operands
    std::vector<std::string> names;
    auto first = parseExpression();
    auto* firstIdent = dynamic_cast<IdentifierExpr*>(first.get());
    if (!firstIdent) {
        error("swap operands must be variable names");
    }
    names.push_back(firstIdent->name);

    while (match(TokenType::COMMA)) {
        auto operand = parseExpression();
        auto* ident = dynamic_cast<IdentifierExpr*>(operand.get());
        if (!ident) {
            error("swap operands must be variable names");
        }
        names.push_back(ident->name);
    }
    consume(TokenType::SEMICOLON, "Expected ';' after swap statement");

    if (names.size() < 2) {
        error("swap requires at least two variables");
    }

    static int swapCounter = 0;
    const std::string tmpName = "__swap_" + std::to_string(swapCounter++);

    std::vector<std::unique_ptr<Statement>> stmts;

    // var __swap_tmp = first;
    auto tmpInit = std::make_unique<IdentifierExpr>(names[0]);
    {
        auto _cg = std::make_unique<VarDecl>(tmpName, std::move(tmpInit));
        _cg->isCompilerGenerated = true;
        stmts.push_back(std::move(_cg));
    }

    // Circular rotation: names[0] = names[1]; names[1] = names[2]; ...
    for (size_t i = 0; i + 1 < names.size(); ++i) {
        auto assign = std::make_unique<AssignExpr>(names[i], std::make_unique<IdentifierExpr>(names[i + 1]));
        stmts.push_back(std::make_unique<ExprStmt>(std::move(assign)));
    }

    // last = __swap_tmp;
    auto assignLast = std::make_unique<AssignExpr>(names.back(), std::make_unique<IdentifierExpr>(tmpName));
    stmts.push_back(std::make_unique<ExprStmt>(std::move(assignLast)));

    return std::make_unique<BlockStmt>(std::move(stmts));
}

// times (N) { ... }  or  times N { ... }
// Desugars to: for (__times_i in 0...N) { ... }
std::unique_ptr<Statement> Parser::parseTimesStmt() {
    // Allow optional parens: times N { ... } or times (N) { ... }
    const bool hasParen = match(TokenType::LPAREN);
    auto count = parseExpression();
    if (hasParen) {
        consume(TokenType::RPAREN, "Expected ')' after times count");
    }

    auto body = parseStatement();

    // Desugar to: for (__times_N in 0...count) { body }
    static int timesCounter = 0;
    const std::string iterVar = "__times_" + std::to_string(timesCounter++);
    auto start = std::make_unique<LiteralExpr>(static_cast<long long>(0));
    return std::make_unique<ForStmt>(iterVar, std::move(start), std::move(count), nullptr, std::move(body));
}

// with (var x = expr) { body }  or  with (var x = expr, var y = expr2) { body }
// Desugars to: { var x = expr; [var y = expr2;] body }
// Scoped bindings that are cleaned up at block end
std::unique_ptr<Statement> Parser::parseWithStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'with'");

    std::vector<std::unique_ptr<Statement>> stmts;

    // Parse one or more variable declarations
    do {
        bool isConst = false;
        if (match(TokenType::CONST)) {
            isConst = true;
        } else {
            consume(TokenType::VAR, "Expected 'var' or 'const' in with binding");
        }
        auto decl = parseVarDecl(isConst);
        stmts.push_back(std::move(decl));
    } while (match(TokenType::COMMA));

    consume(TokenType::RPAREN, "Expected ')' after with bindings");

    auto body = parseStatement();
    stmts.push_back(std::move(body));

    return std::make_unique<BlockStmt>(std::move(stmts));
}

// pipeline (i in start...end) { stage name { ... } ... }
// pipeline (i in start...end...step) { ... }
// pipeline { stage name { ... } ... }   — no explicit range; stages share any enclosing loop var
std::unique_ptr<Statement> Parser::parsePipelineStmt() {
    // Syntax (two forms):
    //
    //   pipeline N { stage name { ... } ... }
    //     — Run stages N times.  N is any expression.  The compiler generates
    //       a software-prefetched loop [0, N) with hidden iterator
    //       __pipeline_i.  Array reads detected in stage bodies are
    //       automatically prefetched D iterations ahead.
    //
    //   pipeline { stage name { ... } ... }
    //     — One-shot form: stages execute exactly once as a straight-line
    //       block.  Useful for Load→Compute→Store bursts where the caller
    //       manages iteration.
    //
    // Notes:
    //   • 'stage' names are labels only — they impose no scoping on variables
    //     declared outside the pipeline block.
    //   • Break/continue inside a stage work identically to inside a for-loop.

    std::unique_ptr<Expression> countExpr;

    // Optional count expression — present when next token is NOT '{'
    if (!check(TokenType::LBRACE)) {
        countExpr = parseExpression();
    }

    consume(TokenType::LBRACE, "Expected '{' after pipeline header");

    std::vector<StageDecl> stages;
    while (!check(TokenType::RBRACE) && !check(TokenType::END_OF_FILE)) {
        if (!match(TokenType::STAGE)) {
            error("Expected 'stage' inside pipeline block");
        }
        const Token stageName = consume(TokenType::IDENTIFIER, "Expected stage name after 'stage'");
        auto stageBody = parseBlock();
        stages.emplace_back(stageName.lexeme, std::move(stageBody));
    }
    consume(TokenType::RBRACE, "Expected '}' to close pipeline block");

    if (stages.empty()) {
        error("pipeline block must contain at least one stage");
    }

    return std::make_unique<PipelineStmt>(std::move(countExpr), std::move(stages));
}

// var [a, b, c] = expr;  or  const [a, b, c] = expr;
// Desugars to:
//   { var __destructure_N = expr;
//     var a = __destructure_N[0];
//     var b = __destructure_N[1];
//     var c = __destructure_N[2]; }
// Use '_' to skip an element: var [a, _, c] = expr;
std::vector<std::unique_ptr<Statement>> Parser::parseDestructuringDecl(bool isConst) {
    const Token lbracket = consume(TokenType::LBRACKET, "Expected '[' for array destructuring");

    std::vector<std::string> names;
    if (!check(TokenType::RBRACKET)) {
        // First name or '_'
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
            advance();
            names.push_back("_");
        } else {
            const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in destructuring");
            names.push_back(name.lexeme);
        }
        while (match(TokenType::COMMA)) {
            if (check(TokenType::RBRACKET))
                break; // trailing comma
            if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
                advance();
                names.push_back("_");
            } else {
                const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in destructuring");
                names.push_back(name.lexeme);
            }
        }
    }
    consume(TokenType::RBRACKET, "Expected ']' after destructuring names");

    if (names.empty()) {
        error("Destructuring must have at least one variable name");
    }

    consume(TokenType::ASSIGN, "Expected '=' after destructuring pattern");
    auto initializer = parseExpression();

    // Desugar to a block of individual assignments
    static int destructureCounter = 0;
    const std::string tmpName = "__destructure_" + std::to_string(destructureCounter++);

    std::vector<std::unique_ptr<Statement>> stmts;

    // var __destructure_N = expr;
    auto tmpDecl = std::make_unique<VarDecl>(tmpName, std::move(initializer), false);
    tmpDecl->isCompilerGenerated = true;
    tmpDecl->line = lbracket.line;
    tmpDecl->column = lbracket.column;
    stmts.push_back(std::move(tmpDecl));

    // var a = __destructure_N[0]; var b = __destructure_N[1]; ...
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == "_")
            continue; // skip placeholder

        auto tmpRef = std::make_unique<IdentifierExpr>(tmpName);
        auto idx = std::make_unique<LiteralExpr>(static_cast<long long>(i));
        auto indexExpr = std::make_unique<IndexExpr>(std::move(tmpRef), std::move(idx));

        auto varDecl = std::make_unique<VarDecl>(names[i], std::move(indexExpr), isConst);
        varDecl->isCompilerGenerated = true;
        varDecl->line = lbracket.line;
        varDecl->column = lbracket.column;
        stmts.push_back(std::move(varDecl));
    }

    return stmts;
}

// var (a, b, c) = expr;  or  const (a, b) = expr;
// Desugars to:
//   { var __tdestr_N = expr;
//     var a = __tdestr_N.0;
//     var b = __tdestr_N.1; }
// Use '_' to skip an element.
std::vector<std::unique_ptr<Statement>> Parser::parseTupleDestructuringDecl(bool isConst) {
    const Token lparen = consume(TokenType::LPAREN, "Expected '(' for tuple destructuring");

    std::vector<std::string> names;
    if (!check(TokenType::RPAREN)) {
        auto parseName = [&]() {
            if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
                advance();
                names.push_back("_");
            } else {
                const Token n = consume(TokenType::IDENTIFIER, "Expected variable name in tuple destructuring");
                names.push_back(n.lexeme);
            }
        };
        parseName();
        while (match(TokenType::COMMA)) {
            if (check(TokenType::RPAREN)) break; // trailing comma
            parseName();
        }
    }
    consume(TokenType::RPAREN, "Expected ')' after tuple destructuring names");

    if (names.empty()) {
        error("Tuple destructuring must have at least one variable name");
    }

    consume(TokenType::ASSIGN, "Expected '=' after tuple destructuring pattern");
    auto initializer = parseExpression();

    // Desugar to individual field-access assignments
    static int tdestrCounter = 0;
    const std::string tmpName = "__tdestr_" + std::to_string(tdestrCounter++);

    std::vector<std::unique_ptr<Statement>> stmts;

    // var __tdestr_N = expr;
    auto tmpDecl = std::make_unique<VarDecl>(tmpName, std::move(initializer), false);
    tmpDecl->isCompilerGenerated = true;
    tmpDecl->line = lparen.line;
    tmpDecl->column = lparen.column;
    stmts.push_back(std::move(tmpDecl));

    // var a = __tdestr_N.0; var b = __tdestr_N.1; ...
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == "_") continue;

        auto tmpRef = std::make_unique<IdentifierExpr>(tmpName);
        // Use FieldAccessExpr with numeric field name (e.g. "0", "1")
        // — generateFieldAccess handles integer field names as tuple indices.
        auto fieldExpr = std::make_unique<FieldAccessExpr>(std::move(tmpRef), std::to_string(i));
        fieldExpr->line = lparen.line;
        fieldExpr->column = lparen.column;

        auto varDecl = std::make_unique<VarDecl>(names[i], std::move(fieldExpr), isConst);
        varDecl->isCompilerGenerated = true;
        varDecl->line = lparen.line;
        varDecl->column = lparen.column;
        stmts.push_back(std::move(varDecl));
    }

    return stmts;
}

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
    const Token nameToken = consume(TokenType::IDENTIFIER, "Expected enum name");
    consume(TokenType::LBRACE, "Expected '{' after enum name");

    std::vector<std::pair<std::string, long long>> members;
    long long nextValue = 0;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        const Token memberToken = consume(TokenType::IDENTIFIER, "Expected enum member name");
        long long memberValue = nextValue;
        if (match(TokenType::ASSIGN)) {
            const Token valToken = consume(TokenType::INTEGER, "Expected integer value for enum member");
            memberValue = valToken.intValue;
        }
        members.push_back({memberToken.lexeme, memberValue});
        nextValue = memberValue + 1;
        // Allow optional trailing comma
        match(TokenType::COMMA);
    }

    consume(TokenType::RBRACE, "Expected '}' after enum body");
    enumNames_.insert(nameToken.lexeme);
    return std::make_unique<EnumDecl>(nameToken.lexeme, std::move(members));
}

std::unique_ptr<StructDecl> Parser::parseStructDecl(StructRepr repr, int reprAlignN, const std::string& forcedName) {
    Token nameToken =
        forcedName.empty()
            ? consume(TokenType::IDENTIFIER, "Expected struct name")
            : Token(TokenType::IDENTIFIER, forcedName, tokens[current].line, tokens[current].column);
    consume(TokenType::LBRACE, "Expected '{' after struct name");

    // Register the struct name early so that operator bodies defined inside
    // this struct can form struct literals of this type (e.g., return Vec2 { x:…, y:…}).
    structNames_.insert(nameToken.lexeme);

    std::vector<std::string> fields;
    std::vector<StructField> fieldDecls;
    std::vector<OperatorOverload> operators;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        // Check for operator overload: fn operator+(...) -> Type { ... }
        if (check(TokenType::FN) && current + 1 < tokens.size() && tokens[current + 1].type == TokenType::IDENTIFIER &&
            tokens[current + 1].lexeme == "operator") {
            advance(); // consume 'fn'
            advance(); // consume 'operator'

            // Parse the operator symbol.
            //
            // Three forms are accepted:
            //
            //   1. Any sequence of operator tokens up to `(` — allows any
            //      symbol the user invents, e.g.:
            //          fn operator<=>(other: T) -> int { ... }
            //          fn operator^><(other: T) -> T   { ... }
            //          fn operator|>(other: T) -> T    { ... }
            //      These are usable directly in expressions: `v1 <=> v2`.
            //
            //   2. A quoted string name — e.g.:
            //          fn operator "dot"(other: Vec2) -> int { ... }
            //      Callable with backtick infix syntax: `v1 \`dot\` v2`.
            //
            //   3. A backtick-quoted name — e.g.:
            //          fn operator `cross`(other: Vec3) -> Vec3 { ... }
            //      Equivalent to form 2; also callable via `v1 \`cross\` v2`.
            //
            // Form 1 supports ALL standard operator symbols plus any novel
            // combination, making both operator overloading (redefine an
            // existing operator for a type) and operator creation (define an
            // entirely new operator symbol) available with the same syntax.

            std::string opStr;

            if (check(TokenType::STRING)) {
                // Form 2: quoted string name — backtick-callable only.
                opStr = peek().lexeme;
                advance();
            } else if (check(TokenType::BACKTICK_IDENT)) {
                // Form 3: backtick-quoted name — backtick-callable only.
                opStr = peek().lexeme;
                advance();
            } else {
                // Form 1: collect ALL non-LPAREN tokens and concatenate their
                // lexemes.  This handles single-token operators (+, -, **, etc.)
                // and arbitrary multi-token sequences (<=>, ^><, |>, …) uniformly.
                while (!check(TokenType::LPAREN) && !check(TokenType::END_OF_FILE)) {
                    opStr += peek().lexeme;
                    advance();
                }
                if (opStr.empty()) {
                    error("Expected operator symbol after 'operator' "
                          "(e.g., +, -, *, <=>, ^><, or a quoted name like \"dot\")");
                }
            }

            consume(TokenType::LPAREN, "Expected '(' after operator");
            const Token paramName = consume(TokenType::IDENTIFIER, "Expected parameter name");
            std::string paramType;
            if (match(TokenType::COLON)) {
                paramType = parseTypeAnnotation();
            }
            consume(TokenType::RPAREN, "Expected ')' after operator parameter");

            std::string returnType;
            if (match(TokenType::ARROW)) {
                returnType = parseTypeAnnotation();
            }

            // Parse the operator body as a function.
            // Synthesize a function name: __op_StructName_opname
            const std::string funcName = "__op_" + nameToken.lexeme + "_" + opStr;
            // Create parameters: self (implicit) + explicit param
            std::vector<Parameter> params;
            params.emplace_back("self", nameToken.lexeme);
            params.emplace_back(paramName.lexeme, paramType);

            auto body = parseBlock();

            auto funcDecl = std::make_unique<FunctionDecl>(funcName, std::vector<std::string>{}, std::move(params),
                                                           std::move(body), false, returnType);
            funcDecl->line = nameToken.line;
            funcDecl->column = nameToken.column;
            funcDecl->hintInline = true; // Operator overloads should be inlined

            OperatorOverload overload;
            overload.op = opStr;
            overload.paramName = paramName.lexeme;
            overload.paramType = paramType;
            overload.returnType = returnType;
            overload.impl = std::move(funcDecl);
            operators.push_back(std::move(overload));
            continue;
        }

        // Parse optional field attributes before the field name.
        FieldAttrs attrs;
        while ((check(TokenType::IDENTIFIER) || check(TokenType::MOVE)) && !isAtEnd()) {
            // Handle 'move' keyword as field attribute first
            if (check(TokenType::MOVE)) {
                attrs.isMove = true;
                advance();
                continue;
            }
            const std::string& kw = peek().lexeme;
            if (kw == "hot") {
                attrs.hot = true;
                advance();
                continue;
            }
            if (kw == "cold") {
                attrs.cold = true;
                advance();
                continue;
            }
            if (kw == "noalias") {
                attrs.noalias = true;
                advance();
                continue;
            }
            if (kw == "immut") {
                attrs.immut = true;
                advance();
                continue;
            }
            if (kw == "align") {
                advance(); // consume 'align'
                consume(TokenType::LPAREN, "Expected '(' after 'align'");
                const Token val = consume(TokenType::INTEGER, "Expected integer alignment");
                attrs.align = static_cast<int>(val.intValue);
                consume(TokenType::RPAREN, "Expected ')' after alignment value");
                continue;
            }
            if (kw == "range") {
                advance(); // consume 'range'
                consume(TokenType::LPAREN, "Expected '(' after 'range'");
                const Token minVal = consume(TokenType::INTEGER, "Expected integer min");
                consume(TokenType::COMMA, "Expected ',' between range values");
                const Token maxVal = consume(TokenType::INTEGER, "Expected integer max");
                consume(TokenType::RPAREN, "Expected ')' after range values");
                attrs.hasRange = true;
                attrs.rangeMin = minVal.intValue;
                attrs.rangeMax = maxVal.intValue;
                continue;
            }
            break; // not an attribute, must be field name or type
        }

        // Parse optional type name before field name
        std::string typeName;
        // If current is an identifier and next is also an identifier, current is the type
        if (check(TokenType::IDENTIFIER) && current + 1 < tokens.size() &&
            tokens[current + 1].type == TokenType::IDENTIFIER) {
            typeName = advance().lexeme;
        }

        const Token fieldToken = consume(TokenType::IDENTIFIER, "Expected field name");
        fields.push_back(fieldToken.lexeme);
        // Skip optional type annotation (e.g. x:int) — alternative syntax
        if (match(TokenType::COLON)) {
            typeName = parseTypeAnnotation();
        }
        // Optional default value: fieldname: type = expr
        std::unique_ptr<Expression> defaultVal = nullptr;
        if (match(TokenType::ASSIGN)) {
            defaultVal = parseExpression();
        }
        fieldDecls.push_back(StructField(fieldToken.lexeme, typeName, attrs, std::move(defaultVal)));
        match(TokenType::COMMA);
        match(TokenType::SEMICOLON);
    }

    consume(TokenType::RBRACE, "Expected '}' after struct body");
    // (structNames_ was already populated before the body was parsed)
    auto decl = std::make_unique<StructDecl>(nameToken.lexeme, std::move(fields), std::move(fieldDecls));
    decl->operators = std::move(operators);
    decl->repr = repr;
    decl->reprAlignN = reprAlignN;
    return decl;
}

std::unique_ptr<Expression> Parser::parseStructLiteral(const std::string& name, int line, int col) {
    std::vector<std::pair<std::string, std::unique_ptr<Expression>>> fieldValues;

    // Optional spread base: `Struct { ..base, field: val }`.
    // The spread expression must appear first (like Rust / JavaScript spread).
    std::unique_ptr<Expression> spreadBase;
    if (match(TokenType::DOT_DOT)) {
        spreadBase = parseExpression();
        match(TokenType::COMMA); // optional comma after spread
    }

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        const Token fieldToken = consume(TokenType::IDENTIFIER, "Expected field name in struct literal");
        consume(TokenType::COLON, "Expected ':' after field name");
        auto value = parseExpression();
        fieldValues.push_back({fieldToken.lexeme, std::move(value)});
        match(TokenType::COMMA);
    }

    consume(TokenType::RBRACE, "Expected '}' after struct literal");
    auto expr = std::make_unique<StructLiteralExpr>(name, std::move(fieldValues));
    expr->spreadBase = std::move(spreadBase);
    expr->line = line;
    expr->column = col;
    return expr;
}

std::unique_ptr<Statement> Parser::parseExprStmt() {
    const Token start = peek();
    auto expr = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after expression");

    auto stmt = std::make_unique<ExprStmt>(std::move(expr));
    stmt->line = start.line;
    stmt->column = start.column;
    return stmt;
}

std::unique_ptr<Expression> Parser::parseExpression() {
    const RecursionGuard guard(*this);
    return parseAssignment();
}

std::unique_ptr<Expression> Parser::parseAssignment() {
    auto expr = parsePipe();

    if (match(TokenType::ASSIGN)) {
        // Check if left side is an identifier
        if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto idExpr = dynamic_cast<IdentifierExpr*>(expr.get());
            auto value = parseAssignment();
            auto node = std::make_unique<AssignExpr>(idExpr->name, std::move(value));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else if (expr->type == ASTNodeType::INDEX_EXPR) {
            auto* indexExpr = static_cast<IndexExpr*>(expr.get());
            auto value = parseAssignment();
            auto arrClone = std::move(indexExpr->array);
            auto idxClone = std::move(indexExpr->index);
            auto node = std::make_unique<IndexAssignExpr>(std::move(arrClone), std::move(idxClone), std::move(value));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else if (expr->type == ASTNodeType::FIELD_ACCESS_EXPR) {
            auto* fieldExpr = static_cast<FieldAccessExpr*>(expr.get());
            auto value = parseAssignment();
            auto objClone = std::move(fieldExpr->object);
            const std::string fieldName = fieldExpr->fieldName;
            auto node = std::make_unique<FieldAssignExpr>(std::move(objClone), fieldName, std::move(value));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else if (expr->type == ASTNodeType::UNARY_EXPR) {
            // Handle `*ptr = value` — write-through-pointer (Ω spec §4.2).
            auto* ue = static_cast<UnaryExpr*>(expr.get());
            if (ue->op == "deref") {
                auto rhs = parseAssignment();
                auto ptrExpr = std::move(ue->operand);
                auto node = std::make_unique<DerefAssignExpr>(std::move(ptrExpr), std::move(rhs));
                node->line = expr->line;
                node->column = expr->column;
                return node;
            }
            error("Invalid assignment target");
        } else {
            error("Invalid assignment target");
        }
    }

    // Compound assignment operators: +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=, &&=, ||=
    if (match(TokenType::PLUS_ASSIGN) || match(TokenType::MINUS_ASSIGN) || match(TokenType::STAR_ASSIGN) ||
        match(TokenType::SLASH_ASSIGN) || match(TokenType::PERCENT_ASSIGN) || match(TokenType::AMPERSAND_ASSIGN) ||
        match(TokenType::PIPE_ASSIGN) || match(TokenType::CARET_ASSIGN) || match(TokenType::LSHIFT_ASSIGN) ||
        match(TokenType::RSHIFT_ASSIGN) || match(TokenType::STAR_STAR_ASSIGN) ||
        match(TokenType::NULL_COALESCE_ASSIGN) || match(TokenType::AND_ASSIGN) || match(TokenType::OR_ASSIGN)) {
        const TokenType opType = tokens[current - 1].type;
        const std::string opLexeme = tokens[current - 1].lexeme;

        // Determine the binary operator from the compound operator
        std::string binOp;
        switch (opType) {
        case TokenType::PLUS_ASSIGN:
            binOp = "+";
            break;
        case TokenType::MINUS_ASSIGN:
            binOp = "-";
            break;
        case TokenType::STAR_ASSIGN:
            binOp = "*";
            break;
        case TokenType::SLASH_ASSIGN:
            binOp = "/";
            break;
        case TokenType::PERCENT_ASSIGN:
            binOp = "%";
            break;
        case TokenType::AMPERSAND_ASSIGN:
            binOp = "&";
            break;
        case TokenType::PIPE_ASSIGN:
            binOp = "|";
            break;
        case TokenType::CARET_ASSIGN:
            binOp = "^";
            break;
        case TokenType::LSHIFT_ASSIGN:
            binOp = "<<";
            break;
        case TokenType::RSHIFT_ASSIGN:
            binOp = ">>";
            break;
        case TokenType::STAR_STAR_ASSIGN:
            binOp = "**";
            break;
        case TokenType::NULL_COALESCE_ASSIGN:
            binOp = "??";
            break;
        case TokenType::AND_ASSIGN:
            binOp = "&&";
            break;
        case TokenType::OR_ASSIGN:
            binOp = "||";
            break;
        default:
            error("Unknown compound assignment operator: " + opLexeme);
            break;
        }

        if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto idExpr = dynamic_cast<IdentifierExpr*>(expr.get());
            const std::string name = idExpr->name;
            auto rhs = parseAssignment();

            // Desugar: x += expr  =>  x = x + expr
            auto lhsRef = std::make_unique<IdentifierExpr>(name);
            lhsRef->line = expr->line;
            lhsRef->column = expr->column;
            auto binExpr = std::make_unique<BinaryExpr>(binOp, std::move(lhsRef), std::move(rhs));
            binExpr->line = expr->line;
            binExpr->column = expr->column;
            auto node = std::make_unique<AssignExpr>(name, std::move(binExpr));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else if (expr->type == ASTNodeType::INDEX_EXPR) {
            // Desugar: arr[i] += expr  =>  arr[i] = arr[i] + expr
            auto* indexExpr = static_cast<IndexExpr*>(expr.get());
            auto rhs = parseAssignment();

            // We need to re-read arr and i for the RHS since the originals will be moved.
            // The array and index expressions are moved into both the read and write sides.
            // We duplicate the array/index by creating fresh identifier references.
            // Note: This only works for simple array[index] patterns; the array expression
            // and index expression are AST subtrees that we cannot clone generically.
            // However, the common case is identifier[expr], which we handle here.
            auto arrClone = std::move(indexExpr->array);
            auto idxClone = std::move(indexExpr->index);

            // Build the read side: create a new IndexExpr for arr[i] on the RHS.
            // We need separate copies of the array and index expressions.
            // Since we can't clone arbitrary AST nodes, we extract identifier names
            // and rebuild the expressions. The array must be an identifier.
            // For the index, we support integer literals and identifiers.
            std::unique_ptr<Expression> arrRef2;
            std::unique_ptr<Expression> idxRef2;

            if (arrClone->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* arrId = static_cast<IdentifierExpr*>(arrClone.get());
                arrRef2 = std::make_unique<IdentifierExpr>(arrId->name);
                arrRef2->line = arrClone->line;
                arrRef2->column = arrClone->column;
            } else {
                error("Compound assignment to array elements requires a simple array variable");
            }

            if (idxClone->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* idxId = static_cast<IdentifierExpr*>(idxClone.get());
                idxRef2 = std::make_unique<IdentifierExpr>(idxId->name);
                idxRef2->line = idxClone->line;
                idxRef2->column = idxClone->column;
            } else if (idxClone->type == ASTNodeType::LITERAL_EXPR) {
                auto* litIdx = static_cast<LiteralExpr*>(idxClone.get());
                if (litIdx->literalType == LiteralExpr::LiteralType::INTEGER) {
                    idxRef2 = std::make_unique<LiteralExpr>(litIdx->intValue);
                } else {
                    error("Array index must be an integer, not a float");
                }
                idxRef2->line = idxClone->line;
                idxRef2->column = idxClone->column;
            } else {
                error("Compound assignment to array elements requires a simple index expression");
            }

            // Build: arr[i] + rhs
            auto readExpr = std::make_unique<IndexExpr>(std::move(arrRef2), std::move(idxRef2));
            readExpr->line = expr->line;
            readExpr->column = expr->column;
            auto binExpr = std::make_unique<BinaryExpr>(binOp, std::move(readExpr), std::move(rhs));
            binExpr->line = expr->line;
            binExpr->column = expr->column;

            // Build: arr[i] = (arr[i] + rhs)
            auto node = std::make_unique<IndexAssignExpr>(std::move(arrClone), std::move(idxClone), std::move(binExpr));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else if (expr->type == ASTNodeType::FIELD_ACCESS_EXPR) {
            // Desugar: s.field += expr  =>  s.field = s.field + expr
            auto* fieldExpr = static_cast<FieldAccessExpr*>(expr.get());
            auto rhs = parseAssignment();

            auto objClone = std::move(fieldExpr->object);
            const std::string fieldName = fieldExpr->fieldName;

            // Build the read side: create a new FieldAccessExpr for s.field on the RHS.
            // The object must be a simple identifier so we can duplicate it.
            std::unique_ptr<Expression> objRef2;
            if (objClone->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* objId = static_cast<IdentifierExpr*>(objClone.get());
                objRef2 = std::make_unique<IdentifierExpr>(objId->name);
                objRef2->line = objClone->line;
                objRef2->column = objClone->column;
            } else {
                error("Compound assignment to struct fields requires a simple struct variable (e.g., 's.x += 1')");
            }

            // Build: s.field + rhs
            auto readExpr = std::make_unique<FieldAccessExpr>(std::move(objRef2), fieldName);
            readExpr->line = expr->line;
            readExpr->column = expr->column;
            auto binExpr = std::make_unique<BinaryExpr>(binOp, std::move(readExpr), std::move(rhs));
            binExpr->line = expr->line;
            binExpr->column = expr->column;

            // Build: s.field = (s.field + rhs)
            auto node = std::make_unique<FieldAssignExpr>(std::move(objClone), fieldName, std::move(binExpr));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else if (expr->type == ASTNodeType::UNARY_EXPR) {
            // Desugar: *ptr op= rhs  =>  *ptr = *ptr op rhs
            auto* ue = static_cast<UnaryExpr*>(expr.get());
            if (ue->op != "deref")
                error("Compound assignment target must be a variable, *ptr, arr[i], or struct.field");
            auto rhs = parseAssignment();
            // We need two copies of the pointer expression.  The common case is
            // that the pointer operand is a simple identifier; duplicate it.
            auto ptrOp = std::move(ue->operand);
            std::unique_ptr<Expression> ptrRef2;
            if (ptrOp->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* pid = static_cast<IdentifierExpr*>(ptrOp.get());
                ptrRef2 = std::make_unique<IdentifierExpr>(pid->name);
                ptrRef2->line = ptrOp->line;
                ptrRef2->column = ptrOp->column;
            } else {
                error("Compound assignment '*p op= rhs' requires a simple pointer variable (e.g. '*p += 1')");
            }
            // Build: *ptr op rhs  (read side)
            auto readDeref = std::make_unique<UnaryExpr>("deref", std::move(ptrRef2));
            readDeref->line = expr->line;
            readDeref->column = expr->column;
            auto binExpr = std::make_unique<BinaryExpr>(binOp, std::move(readDeref), std::move(rhs));
            binExpr->line = expr->line;
            binExpr->column = expr->column;
            // Build: *ptr = (*ptr op rhs)  (write side)
            auto node = std::make_unique<DerefAssignExpr>(std::move(ptrOp), std::move(binExpr));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else {
            error("Compound assignment (e.g., '+=') is only supported on variables, array elements (arr[i]), "
                  "struct fields (s.x), and pointer dereferences (*p)");
        }
    }

    return expr;
}

std::unique_ptr<Expression> Parser::parseTernary() {
    auto expr = parseNullCoalesce();

    if (match(TokenType::QUESTION)) {
        const Token questionToken = tokens[current - 1];
        // Elvis operator: x ?: default → x ? x : default
        if (match(TokenType::COLON)) {
            auto elseExpr = parseTernary();
            // Duplicate the condition as the then-branch.
            // For simple identifiers this is safe; for complex expressions
            // the condition is evaluated once by the ternary codegen (it emits
            // a branch on the condition and selects the value).  We clone by
            // wrapping in a fresh IdentifierExpr when the condition is an
            // identifier, otherwise fall back to a TernaryExpr where condition
            // and thenExpr share the same AST node structure.
            std::unique_ptr<Expression> thenClone;
            if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* id = static_cast<IdentifierExpr*>(expr.get());
                thenClone = std::make_unique<IdentifierExpr>(id->name);
                thenClone->line = id->line;
                thenClone->column = id->column;
            } else if (expr->type == ASTNodeType::LITERAL_EXPR) {
                auto* lit = static_cast<LiteralExpr*>(expr.get());
                if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
                    thenClone = std::make_unique<LiteralExpr>(lit->intValue);
                else if (lit->literalType == LiteralExpr::LiteralType::FLOAT)
                    thenClone = std::make_unique<LiteralExpr>(lit->floatValue);
                else
                    thenClone = std::make_unique<LiteralExpr>(lit->stringValue);
                thenClone->line = lit->line;
                thenClone->column = lit->column;
            } else {
                // For other expression types, use null coalescing semantics
                // (x ?? default), which has equivalent behavior for non-null values.
                auto node = std::make_unique<BinaryExpr>("??", std::move(expr), std::move(elseExpr));
                node->line = questionToken.line;
                node->column = questionToken.column;
                return node;
            }
            auto node = std::make_unique<TernaryExpr>(std::move(expr), std::move(thenClone), std::move(elseExpr));
            node->line = questionToken.line;
            node->column = questionToken.column;
            return node;
        }
        auto thenExpr = parseExpression();
        consume(TokenType::COLON, "Expected ':' in ternary expression");
        auto elseExpr = parseTernary();
        auto node = std::make_unique<TernaryExpr>(std::move(expr), std::move(thenExpr), std::move(elseExpr));
        node->line = questionToken.line;
        node->column = questionToken.column;
        return node;
    }

    return expr;
}

std::unique_ptr<Expression> Parser::parseNullCoalesce() {
    auto left = parseCustomOp();

    while (match(TokenType::NULL_COALESCE)) {
        auto right = parseCustomOp();
        // x ?? y  desugars to  x != 0 ? x : y  (ternary expression)
        // We clone the left expression by re-wrapping it
        auto node = std::make_unique<BinaryExpr>("??", std::move(left), std::move(right));
        left = std::move(node);
    }

    return left;
}

// ---------------------------------------------------------------------------
// User-defined arbitrary-symbol operator parsing
// ---------------------------------------------------------------------------
// Provides two complementary calling conventions for custom operators:
//
//   1. Direct infix (any token sequence):
//        v1 <=> v2    (if "<=>" is registered in customOperatorSymbols_)
//        v1 ^>< v2    (if "^><" is registered)
//
//   2. Backtick infix (named operators, already handled in parseLogicalOr):
//        v1 `dot` v2   (using any string as operator name)
//
// Custom operators defined with a quoted string name:
//   fn operator "dot"(other: Vec2) -> int { ... }
// are only reachable via backtick syntax.  Custom operators defined with a
// raw token-sequence symbol are reachable via direct infix.
//
// Precedence: custom operators sit between null-coalesce (??) and
// logical-or (||), i.e. lower than all arithmetic but higher than ternary.
// ---------------------------------------------------------------------------

std::string Parser::tryMatchCustomOperator() const {
    std::string best;
    for (const auto& opSym : customOperatorSymbols_) {
        // Try to match opSym against consecutive token lexemes from current.
        size_t tokIdx = current;
        size_t symIdx = 0;
        while (symIdx < opSym.size() && tokIdx < tokens.size()) {
            const std::string& lex = tokens[tokIdx].lexeme;
            if (opSym.compare(symIdx, lex.size(), lex) != 0) {
                symIdx = opSym.size() + 1;
                break;
            }
            symIdx += lex.size();
            ++tokIdx;
        }
        if (symIdx == opSym.size() && opSym.size() > best.size())
            best = opSym;
    }
    return best;
}

size_t Parser::customOpTokenCount(const std::string& opSym) const {
    size_t tokIdx = current;
    size_t symIdx = 0;
    size_t count = 0;
    while (symIdx < opSym.size() && tokIdx < tokens.size()) {
        symIdx += tokens[tokIdx].lexeme.size();
        ++tokIdx;
        ++count;
    }
    return count;
}

bool Parser::isStartOfLongerCustomOp(size_t standardLen) const {
    const std::string matched = tryMatchCustomOperator();
    return !matched.empty() && matched.size() > standardLen;
}

std::unique_ptr<Expression> Parser::parseCustomOp() {
    auto left = parseLogicalOr();

    while (true) {
        const std::string opSym = tryMatchCustomOperator();
        if (opSym.empty())
            break;
        // Consume the tokens that spell out this custom operator.
        const size_t tokCount = customOpTokenCount(opSym);
        for (size_t i = 0; i < tokCount; ++i)
            advance();
        auto right = parseLogicalOr();
        left = std::make_unique<BinaryExpr>(opSym, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseLogicalOr() {
    auto left = parseLogicalAnd();

    while (match(TokenType::OR)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseLogicalAnd();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    // Backtick infix operator: `name`  (operator creation / named operator call)
    //   a `dot` b  ≡  BinaryExpr("dot", a, b)
    // The codegen dispatches this via the operatorOverloads_ registry exactly
    // like any other struct operator, so it works for both overloading and creation.
    while (check(TokenType::BACKTICK_IDENT)) {
        const std::string opName = peek().lexeme;
        advance(); // consume the backtick token
        auto right = parseLogicalAnd();
        left = std::make_unique<BinaryExpr>(opName, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseLogicalAnd() {
    auto left = parseBitwiseOr();

    while (match(TokenType::AND)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseBitwiseOr();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseBitwiseOr() {
    auto left = parseBitwiseXor();

    while (check(TokenType::PIPE) && !isStartOfLongerCustomOp(1)) {
        advance();
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseBitwiseXor();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseBitwiseXor() {
    auto left = parseBitwiseAnd();

    while (check(TokenType::CARET) && !isStartOfLongerCustomOp(1)) {
        advance();
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseBitwiseAnd();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseBitwiseAnd() {
    auto left = parseEquality();

    while (check(TokenType::AMPERSAND) && !isStartOfLongerCustomOp(1)) {
        advance();
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseEquality();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseEquality() {
    auto left = parseComparison();

    while ((check(TokenType::EQ) || check(TokenType::NE)) && !isStartOfLongerCustomOp(2)) {
        advance();
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseComparison();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseComparison() {
    auto left = parseShift();

    // Chained comparisons: a < b < c desugars to (a < b) && (b < c)
    // Supports arbitrary chains like a < b <= c > d.
    // Guard: if the comparison token(s) are the start of a longer custom
    // operator (e.g. `<=` is the prefix of registered `<=>`), yield to
    // parseCustomOp() instead of consuming the token here.
    auto isCompTok = [&]() {
        return (check(TokenType::LT) && !isStartOfLongerCustomOp(1)) ||
               (check(TokenType::GT) && !isStartOfLongerCustomOp(1)) ||
               (check(TokenType::LE) && !isStartOfLongerCustomOp(2)) ||
               (check(TokenType::GE) && !isStartOfLongerCustomOp(2));
    };

    if (isCompTok()) {
        const Token firstOp = peek();
        advance();
        const std::string op1 = firstOp.lexeme;
        auto mid = parseShift();

        // Check for chained comparison: if another comparison op follows
        if (isCompTok()) {
            // We need to duplicate `mid` for the second comparison.
            // Clone mid by creating a fresh reference if it's an identifier or literal.
            std::unique_ptr<Expression> midClone;
            if (mid->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* id = static_cast<IdentifierExpr*>(mid.get());
                midClone = std::make_unique<IdentifierExpr>(id->name);
                midClone->line = id->line;
                midClone->column = id->column;
            } else if (mid->type == ASTNodeType::LITERAL_EXPR) {
                auto* lit = static_cast<LiteralExpr*>(mid.get());
                if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
                    midClone = std::make_unique<LiteralExpr>(lit->intValue);
                else if (lit->literalType == LiteralExpr::LiteralType::FLOAT)
                    midClone = std::make_unique<LiteralExpr>(lit->floatValue);
                else
                    midClone = std::make_unique<LiteralExpr>(lit->stringValue);
                midClone->line = lit->line;
                midClone->column = lit->column;
            } else {
                // For complex expressions, fall back to non-chained behavior
                left = std::make_unique<BinaryExpr>(op1, std::move(left), std::move(mid));
                while (isCompTok()) {
                    advance();
                    const std::string op = tokens[current - 1].lexeme;
                    auto right = parseShift();
                    left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
                }
                goto check_in;
            }

            // First comparison: left op1 mid
            auto cmp1 = std::make_unique<BinaryExpr>(op1, std::move(left), std::move(mid));
            cmp1->line = firstOp.line;
            cmp1->column = firstOp.column;

            std::unique_ptr<Expression> result = std::move(cmp1);

            // Continue chaining: midClone op2 right ...
            left = std::move(midClone);
            while (isCompTok()) {
                advance();
                const std::string opN = tokens[current - 1].lexeme;
                auto right = parseShift();

                // Clone right for potential further chaining
                std::unique_ptr<Expression> rightClone;
                if (isCompTok()) {
                    if (right->type == ASTNodeType::IDENTIFIER_EXPR) {
                        auto* id = static_cast<IdentifierExpr*>(right.get());
                        rightClone = std::make_unique<IdentifierExpr>(id->name);
                        rightClone->line = id->line;
                        rightClone->column = id->column;
                    } else if (right->type == ASTNodeType::LITERAL_EXPR) {
                        auto* lit = static_cast<LiteralExpr*>(right.get());
                        if (lit->literalType == LiteralExpr::LiteralType::INTEGER)
                            rightClone = std::make_unique<LiteralExpr>(lit->intValue);
                        else if (lit->literalType == LiteralExpr::LiteralType::FLOAT)
                            rightClone = std::make_unique<LiteralExpr>(lit->floatValue);
                        else
                            rightClone = std::make_unique<LiteralExpr>(lit->stringValue);
                        rightClone->line = lit->line;
                        rightClone->column = lit->column;
                    }
                }

                auto cmpN = std::make_unique<BinaryExpr>(opN, std::move(left), std::move(right));
                result = std::make_unique<BinaryExpr>("&&", std::move(result), std::move(cmpN));

                if (rightClone) {
                    left = std::move(rightClone);
                } else {
                    break;
                }
            }
            left = std::move(result);
        } else {
            left = std::make_unique<BinaryExpr>(op1, std::move(left), std::move(mid));
        }
    }

check_in:
    // 'not in' operator: x not in arr → !array_contains(arr, x)
    // 'not' is parsed as an IDENTIFIER (no dedicated keyword), so check for
    // IDENTIFIER with lexeme "not" followed by the IN keyword.
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "not" &&
        current + 1 < tokens.size() && tokens[current + 1].type == TokenType::IN) {
        const Token notToken = tokens[current];
        advance(); // consume 'not'
        advance(); // consume 'in'
        auto container = parseShift();
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(std::move(container));
        args.push_back(std::move(left));
        auto callExpr = std::make_unique<CallExpr>("array_contains", std::move(args));
        callExpr->fromStdNamespace = true;
        callExpr->line = notToken.line;
        callExpr->column = notToken.column;
        auto notExpr = std::make_unique<UnaryExpr>("!", std::move(callExpr));
        notExpr->line = notToken.line;
        notExpr->column = notToken.column;
        return notExpr;
    }

    // 'in' operator: x in arr → array_contains(arr, x)
    // Placed at comparison precedence so it binds like other relational ops.
    if (match(TokenType::IN)) {
        const Token inToken = tokens[current - 1];
        auto container = parseShift();
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(std::move(container)); // array is first arg
        args.push_back(std::move(left));      // value is second arg
        auto callExpr = std::make_unique<CallExpr>("array_contains", std::move(args));
        callExpr->fromStdNamespace = true; // compiler-generated (in operator desugaring)
        callExpr->line = inToken.line;
        callExpr->column = inToken.column;
        return callExpr;
    }

    // 'is' type-check: expr is typename → is_type(expr, "typename")
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "is" &&
        current + 1 < tokens.size() && tokens[current + 1].type == TokenType::IDENTIFIER) {
        const Token isTok = tokens[current];
        advance(); // consume 'is'
        const Token typeTok = tokens[current];
        advance(); // consume typename
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(std::move(left));
        args.push_back(std::make_unique<LiteralExpr>(typeTok.lexeme));
        auto callExpr = std::make_unique<CallExpr>("is_type", std::move(args));
        callExpr->fromStdNamespace = true;
        callExpr->line = isTok.line;
        callExpr->column = isTok.column;
        return callExpr;
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseShift() {
    auto left = parseAddition();

    while ((check(TokenType::LSHIFT) || check(TokenType::RSHIFT)) && !isStartOfLongerCustomOp(2)) {
        advance();
        const std::string op = tokens[current - 1].lexeme;
        const int opLine = tokens[current - 1].line;
        auto right = parseAddition();
        // Warn on a shift count that definitely exceeds 63: the result is
        // undefined in C/LLVM and is almost always a programming error.
        if (right && right->type == ASTNodeType::LITERAL_EXPR) {
            const auto* lit = static_cast<const LiteralExpr*>(right.get());
            if (lit->literalType == LiteralExpr::LiteralType::INTEGER && lit->intValue > 63) {
                warnings_.push_back("warning: shift count " + std::to_string(lit->intValue) + " exceeds 63 (line " +
                                    std::to_string(opLine) + ")");
            }
        }
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseAddition() {
    auto left = parseMultiplication();

    while (match(TokenType::PLUS) || match(TokenType::MINUS)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseMultiplication();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseMultiplication() {
    auto left = parsePower();

    while (match(TokenType::STAR) || match(TokenType::SLASH) || match(TokenType::PERCENT)) {
        const std::string op = tokens[current - 1].lexeme;
        const int opLine = tokens[current - 1].line;
        auto right = parsePower();
        // Warn on a literal zero divisor: integer division/modulo by zero is
        // undefined behaviour and is almost certainly a programming error.
        if ((op == "/" || op == "%") && right && right->type == ASTNodeType::LITERAL_EXPR) {
            const auto* lit = static_cast<const LiteralExpr*>(right.get());
            if (lit->literalType == LiteralExpr::LiteralType::INTEGER && lit->intValue == 0) {
                warnings_.push_back("warning: " + op + " by zero literal (line " + std::to_string(opLine) + ")");
            }
        }
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parsePower() {
    auto left = parseCast();

    if (match(TokenType::STAR_STAR)) {
        // Right-associative: 2 ** 3 ** 2 = 2 ** (3 ** 2) = 2 ** 9 = 512
        auto right = parsePower();
        left = std::make_unique<BinaryExpr>("**", std::move(left), std::move(right));
    }

    return left;
}

// ── parseCast ─────────────────────────────────────────────────────────────────
// Handles `expr as TypeName` with LOWER precedence than all unary prefix
// operators (&, *, -, !, ~) but HIGHER than multiplication/power.
// This ensures `&x as *T` parses as `(&x) as *T` (C semantics), not
// `&(x as *T)`.
std::unique_ptr<Expression> Parser::parseCast() {
    auto expr = parseUnary();

    while (check(TokenType::IDENTIFIER) && peek().lexeme == "as") {
        const Token asTok = advance(); // consume 'as'
        const std::string targetType = parseTypeAnnotation();
        auto typeExpr = std::make_unique<IdentifierExpr>(targetType);
        typeExpr->line = asTok.line;
        typeExpr->column = asTok.column;
        auto castExpr = std::make_unique<BinaryExpr>("as", std::move(expr), std::move(typeExpr));
        castExpr->line = asTok.line;
        castExpr->column = asTok.column;
        expr = std::move(castExpr);
    }

    return expr;
}

std::unique_ptr<Expression> Parser::parseUnary() {
    auto makeThreadKeywordCall = [this](const std::string& callee, const Token& kw, std::vector<std::unique_ptr<Expression>> args)
        -> std::unique_ptr<Expression> {
        auto call = std::make_unique<CallExpr>(callee, std::move(args));
        call->line = kw.line;
        call->column = kw.column;
        return call;
    };

    auto parseSpawnTargetCompact = [this]() -> std::unique_ptr<Expression> {
        if (match(TokenType::IDENTIFIER)) {
            const Token t = tokens[current - 1];
            auto id = std::make_unique<IdentifierExpr>(t.lexeme);
            id->line = t.line;
            id->column = t.column;
            return id;
        }
        if (match(TokenType::STRING)) {
            const Token t = tokens[current - 1];
            auto lit = std::make_unique<LiteralExpr>(t.lexeme);
            lit->line = t.line;
            lit->column = t.column;
            return lit;
        }
        error("Expected function name after 'spawn' (identifier or string literal)");
        return nullptr;
    };

    // ── @range[lo, hi] expr ───────────────────────────────────────────────
    // Internal range-bound annotation.  Parses two integer literals (each
    // with an optional unary minus) and wraps the following unary
    // expression in a RangeAnnotExpr.  The pair is required to satisfy
    // lo <= hi at parse time; codegen additionally errors out if `expr`
    // is a compile-time constant outside [lo, hi].
    //
    // The annotation binds tighter than any binary operator (it sits in
    // unary position), so `@range[0,9] x + 1` parses as `(@range[0,9] x) + 1`.
    if (check(TokenType::AT) && current + 1 < tokens.size() && tokens[current + 1].type == TokenType::IDENTIFIER &&
        tokens[current + 1].lexeme == "range" && current + 2 < tokens.size() &&
        tokens[current + 2].type == TokenType::LBRACKET) {
        const Token atTok = tokens[current];
        advance(); // '@'
        advance(); // 'range'
        advance(); // '['
        auto parseSignedInt = [this](const char* what) -> int64_t {
            bool neg = false;
            if (match(TokenType::MINUS))
                neg = true;
            const Token lit =
                consume(TokenType::INTEGER, std::string("Expected integer ") + what + " in @range[lo, hi]");
            int64_t v = static_cast<int64_t>(lit.intValue);
            return neg ? -v : v;
        };
        const int64_t lo = parseSignedInt("lo");
        consume(TokenType::COMMA, "Expected ',' between lo and hi in @range[lo, hi]");
        const int64_t hi = parseSignedInt("hi");
        consume(TokenType::RBRACKET, "Expected ']' to close @range[lo, hi]");
        if (lo > hi) {
            error("@range[lo, hi] requires lo <= hi (got lo=" + std::to_string(lo) + ", hi=" + std::to_string(hi) +
                  ")");
        }
        auto inner = parseUnary();
        auto node = std::make_unique<RangeAnnotExpr>(lo, hi, std::move(inner));
        node->line = atTok.line;
        node->column = atTok.column;
        return node;
    }

    // Threading keyword sugar:
    //   spawn f()        => thread_create(f)
    //   spawn f(x)       => thread_create(f, x)
    //   join t           => thread_join(t)
    //   detach t         => thread_detach(t)
    //   lock m           => mutex_lock(m)
    //   unlock m         => mutex_unlock(m)
    //   trylock m        => mutex_try_lock(m)
    if (match(TokenType::SPAWN)) {
        const Token kw = tokens[current - 1];

        // ── Enhanced spawn: collect modifiers before the target function ──
        // Syntax: spawn [modifier...] fn(arg)
        // Modifiers (in any order, each consumed once):
        //   detached | joinable | move | shared | pinned | cold | hot
        //   stack(N)  | priority(N) | affinity(N)
        //
        // Desugars to:
        //   thread_create_ex(fn, arg, flags, stack_sz, priority, affinity)
        // where flags = bit0:detached, bit2:cold, bit3:hot
        //
        // If no modifiers are present, falls back to thread_create(fn[, arg]).

        struct SpawnOpts {
            bool detached  = false;
            bool cold      = false;
            bool hot       = false;
            int64_t stackSz    = 0;  // 0 = default
            int64_t priority   = 0;  // 0 = default
            int64_t affinity   = -1; // -1 = no pin
            bool hasModifiers  = false;
        } opts;

        // Helper: peek at current token as an identifier
        auto peekIdent = [this](const std::string& name) -> bool {
            return check(TokenType::IDENTIFIER) && tokens[current].lexeme == name;
        };
        auto consumeIdent = [this](const std::string& name) {
            if (check(TokenType::IDENTIFIER) && tokens[current].lexeme == name) advance();
        };
        auto parseIntArg = [this](const char* mod) -> int64_t {
            consume(TokenType::LPAREN, std::string("Expected '(' after '") + mod + "'");
            bool neg = false;
            if (match(TokenType::MINUS)) neg = true;
            if (!check(TokenType::INTEGER) && !check(TokenType::FLOAT))
                error(std::string("Expected integer value in '") + mod + "(...)'");
            const Token vt = advance();
            int64_t v = vt.type == TokenType::INTEGER
                            ? static_cast<int64_t>(vt.intValue)
                            : static_cast<int64_t>(vt.floatValue);
            if (neg) v = -v;
            consume(TokenType::RPAREN, std::string("Expected ')' after '") + mod + "(...)'");
            return v;
        };

        // Consume modifiers in any order (up to 10 passes to handle any ordering)
        for (int pass = 0; pass < 10; ++pass) {
            if (peekIdent("detached"))  { consumeIdent("detached");  opts.detached = true;  opts.hasModifiers = true; continue; }
            if (peekIdent("joinable"))  { consumeIdent("joinable");  /* joinable is default */ opts.hasModifiers = true; continue; }
            if (peekIdent("move"))      { consumeIdent("move");      /* already default ownership */ opts.hasModifiers = true; continue; }
            if (peekIdent("shared"))    { consumeIdent("shared");    opts.hasModifiers = true; continue; }
            if (peekIdent("pinned"))    { consumeIdent("pinned");    if (opts.affinity < 0) opts.affinity = 0; opts.hasModifiers = true; continue; }
            if (peekIdent("cold"))      { consumeIdent("cold");      opts.cold = true;  opts.hasModifiers = true; continue; }
            if (peekIdent("hot"))       { consumeIdent("hot");       opts.hot  = true;  opts.hasModifiers = true; continue; }
            if (peekIdent("stack")) {
                advance(); // consume "stack"
                int64_t sz = parseIntArg("stack");
                // Accept unit suffixes written as identifiers after the number:
                // already consumed the ')' above, but handle e.g. stack(1MB) as a
                // numeric literal — users write stack(1048576).
                opts.stackSz = sz;
                opts.hasModifiers = true;
                continue;
            }
            if (peekIdent("priority")) {
                advance();
                opts.priority = parseIntArg("priority");
                opts.hasModifiers = true;
                continue;
            }
            if (peekIdent("affinity")) {
                advance();
                opts.affinity = parseIntArg("affinity");
                opts.hasModifiers = true;
                continue;
            }
            break; // no more recognized modifiers
        }

        // ── Now parse the target: fn(arg?) ──────────────────────────────
        // Two sub-forms:
        //   spawn (...) — legacy positional form kept for back-compat
        //   spawn [mods] fnname(arg?)
        if (!opts.hasModifiers && check(TokenType::LPAREN)) {
            // Legacy: spawn(fn) or spawn(fn, arg)
            advance(); // consume '('
            std::vector<std::unique_ptr<Expression>> args;
            args.push_back(parseExpression());
            if (match(TokenType::COMMA)) args.push_back(parseExpression());
            consume(TokenType::RPAREN, "Expected ')' after spawn(...)");
            if (args.empty() || args.size() > 2)
                error("spawn expects one target and optional one argument");
            return makeThreadKeywordCall("thread_create", kw, std::move(args));
        }

        // Parse fn target
        auto targetExpr = parseSpawnTargetCompact();
        if (!match(TokenType::LPAREN))
            error("Expected '(' after spawn target; use: spawn [mods] fn() or spawn [mods] fn(arg)");
        std::unique_ptr<Expression> argExpr;
        if (!check(TokenType::RPAREN)) {
            argExpr = parseExpression();
            if (check(TokenType::COMMA))
                error("spawn target(arg) accepts at most one argument");
        }
        consume(TokenType::RPAREN, "Expected ')' after spawn target argument");

        if (!opts.hasModifiers) {
            // No modifiers: use simple thread_create for zero overhead
            std::vector<std::unique_ptr<Expression>> args;
            args.push_back(std::move(targetExpr));
            if (argExpr) args.push_back(std::move(argExpr));
            return makeThreadKeywordCall("thread_create", kw, std::move(args));
        }

        // ── Build thread_create_ex(fn, arg, flags, stack_sz, priority, affinity) ──
        int64_t flags = 0;
        if (opts.detached) flags |= 1;
        if (opts.cold)     flags |= 4;
        if (opts.hot)      flags |= 8;

        auto makeI64Lit = [&](int64_t v) -> std::unique_ptr<Expression> {
            auto lit = std::make_unique<LiteralExpr>(static_cast<long long>(v));
            lit->line = kw.line;
            lit->column = kw.column;
            return lit;
        };

        std::vector<std::unique_ptr<Expression>> exArgs;
        exArgs.push_back(std::move(targetExpr));                // arg0: fn
        if (argExpr) exArgs.push_back(std::move(argExpr));      // arg1: thread arg
        else         exArgs.push_back(makeI64Lit(0));            // arg1: 0 (no arg)
        exArgs.push_back(makeI64Lit(flags));                     // arg2: flags
        exArgs.push_back(makeI64Lit(opts.stackSz));              // arg3: stack size
        exArgs.push_back(makeI64Lit(opts.priority));             // arg4: priority
        exArgs.push_back(makeI64Lit(opts.affinity));             // arg5: affinity core
        return makeThreadKeywordCall("thread_create_ex", kw, std::move(exArgs));
    }

    if (match(TokenType::JOIN)) {
        const Token kw = tokens[current - 1];
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(parseUnary());
        return makeThreadKeywordCall("thread_join", kw, std::move(args));
    }

    if (match(TokenType::DETACH)) {
        const Token kw = tokens[current - 1];
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(parseUnary());
        return makeThreadKeywordCall("thread_detach", kw, std::move(args));
    }

    if (match(TokenType::LOCK)) {
        const Token kw = tokens[current - 1];
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(parseUnary());
        return makeThreadKeywordCall("mutex_lock", kw, std::move(args));
    }

    if (match(TokenType::UNLOCK)) {
        const Token kw = tokens[current - 1];
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(parseUnary());
        return makeThreadKeywordCall("mutex_unlock", kw, std::move(args));
    }

    if (match(TokenType::TRYLOCK)) {
        const Token kw = tokens[current - 1];
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(parseUnary());
        return makeThreadKeywordCall("mutex_try_lock", kw, std::move(args));
    }

    if (match(TokenType::MINUS) || match(TokenType::NOT) || match(TokenType::TILDE)) {
        const Token opToken = tokens[current - 1];
        auto operand = parseUnary();
        auto node = std::make_unique<UnaryExpr>(opToken.lexeme, std::move(operand));
        node->line = opToken.line;
        node->column = opToken.column;
        return node;
    }

    // `&expr` — address-of / borrow operator (e.g. `&x` in `borrow var j:&i32 = &x;`)
    if (match(TokenType::AMPERSAND)) {
        const Token opToken = tokens[current - 1];
        auto operand = parseUnary();
        auto node = std::make_unique<UnaryExpr>(opToken.lexeme, std::move(operand));
        node->line = opToken.line;
        node->column = opToken.column;
        return node;
    }

    // `*expr` — pointer dereference (e.g. `*p`, `*(p + 1)`)
    // Only treated as dereference in unary position; binary `*` is multiplication.
    if (match(TokenType::STAR)) {
        const Token opToken = tokens[current - 1];
        auto operand = parseUnary();
        auto node = std::make_unique<UnaryExpr>("deref", std::move(operand));
        node->line = opToken.line;
        node->column = opToken.column;
        return node;
    }

    if (match(TokenType::PLUSPLUS) || match(TokenType::MINUSMINUS)) {
        const Token opToken = tokens[current - 1];
        auto operand = parseUnary();
        const bool isDeref =
            operand->type == ASTNodeType::UNARY_EXPR &&
            static_cast<UnaryExpr*>(operand.get())->op == "deref";
        if (operand->type != ASTNodeType::IDENTIFIER_EXPR &&
            operand->type != ASTNodeType::INDEX_EXPR &&
            operand->type != ASTNodeType::FIELD_ACCESS_EXPR &&
            !isDeref) {
            error("Prefix " + opToken.lexeme + " requires an lvalue operand");
        }
        auto node = std::make_unique<PrefixExpr>(opToken.lexeme, std::move(operand));
        node->line = opToken.line;
        node->column = opToken.column;
        return node;
    }

    // `move <expr>` as a prefix expression (e.g. `return move x;`, `a = move b;`)
    if (match(TokenType::MOVE)) {
        const Token kw = tokens[current - 1];
        auto operand = parseUnary();
        auto node = std::make_unique<MoveExpr>(std::move(operand));
        node->line = kw.line;
        node->column = kw.column;
        return node;
    }

    return parsePostfix();
}

std::unique_ptr<Expression> Parser::parsePostfix() {
    auto expr = parseCall();

    while (true) {
        // Handle postfix operators
        if (match(TokenType::PLUSPLUS) || match(TokenType::MINUSMINUS)) {
            const Token opToken = tokens[current - 1];
            const bool isDeref =
                expr->type == ASTNodeType::UNARY_EXPR &&
                static_cast<UnaryExpr*>(expr.get())->op == "deref";
            if (expr->type != ASTNodeType::IDENTIFIER_EXPR &&
                expr->type != ASTNodeType::INDEX_EXPR &&
                expr->type != ASTNodeType::FIELD_ACCESS_EXPR &&
                !isDeref) {
                error("Postfix " + opToken.lexeme + " requires an lvalue operand");
            }
            expr = std::make_unique<PostfixExpr>(opToken.lexeme, std::move(expr));
            expr->line = opToken.line;
            expr->column = opToken.column;
        }
        // Handle array indexing or slice syntax arr[start:end] / arr[start..end]
        else if (match(TokenType::LBRACKET)) {
            const Token bracketToken = tokens[current - 1];
            // ── `arr[:end]` — from-zero colon slice ─────────────────────────
            if (match(TokenType::COLON)) {
                // arr[:end] → array_slice(arr, 0, end)
                auto endIdx = parseExpression();
                consume(TokenType::RBRACKET, "Expected ']' after slice");
                std::vector<std::unique_ptr<Expression>> args;
                args.push_back(std::move(expr));
                auto zeroLit = std::make_unique<LiteralExpr>(static_cast<long long>(0));
                zeroLit->line = bracketToken.line;
                zeroLit->column = bracketToken.column;
                args.push_back(std::move(zeroLit));
                args.push_back(std::move(endIdx));
                auto callExpr = std::make_unique<CallExpr>("array_slice", std::move(args));
                callExpr->fromStdNamespace = true; // compiler-generated (slice syntax)
                callExpr->line = bracketToken.line;
                callExpr->column = bracketToken.column;
                expr = std::move(callExpr);
            // ── `arr[..end]` / `arr[..=end]` — range-style from-zero slice ──
            } else if (match(TokenType::DOT_DOT)) {
                // arr[..end]  → array_slice(arr, 0, end)      exclusive
                // arr[..=end] → array_slice(arr, 0, end + 1)  inclusive
                const bool inclusive = match(TokenType::ASSIGN);
                auto endIdx = parseExpression();
                if (inclusive) {
                    auto oneLit = std::make_unique<LiteralExpr>(static_cast<long long>(1));
                    oneLit->line = bracketToken.line;
                    oneLit->column = bracketToken.column;
                    endIdx = std::make_unique<BinaryExpr>("+", std::move(endIdx), std::move(oneLit));
                }
                consume(TokenType::RBRACKET, "Expected ']' after range slice");
                std::vector<std::unique_ptr<Expression>> args;
                args.push_back(std::move(expr));
                auto zeroLit = std::make_unique<LiteralExpr>(static_cast<long long>(0));
                zeroLit->line = bracketToken.line;
                zeroLit->column = bracketToken.column;
                args.push_back(std::move(zeroLit));
                args.push_back(std::move(endIdx));
                auto callExpr = std::make_unique<CallExpr>("array_slice", std::move(args));
                callExpr->fromStdNamespace = true;
                callExpr->line = bracketToken.line;
                callExpr->column = bracketToken.column;
                expr = std::move(callExpr);
            } else {
                auto index = parseExpression();
                if (match(TokenType::COLON)) {
                    // arr[start:end] → array_slice(arr, start, end)
                    auto endIdx = parseExpression();
                    consume(TokenType::RBRACKET, "Expected ']' after slice");
                    std::vector<std::unique_ptr<Expression>> args;
                    args.push_back(std::move(expr));
                    args.push_back(std::move(index));
                    args.push_back(std::move(endIdx));
                    auto callExpr = std::make_unique<CallExpr>("array_slice", std::move(args));
                    callExpr->fromStdNamespace = true; // compiler-generated (slice syntax)
                    callExpr->line = bracketToken.line;
                    callExpr->column = bracketToken.column;
                    expr = std::move(callExpr);
                } else if (match(TokenType::DOT_DOT)) {
                    // arr[start..end]  → array_slice(arr, start, end)      exclusive
                    // arr[start..=end] → array_slice(arr, start, end + 1)  inclusive
                    const bool inclusive = match(TokenType::ASSIGN);
                    auto endIdx = parseExpression();
                    if (inclusive) {
                        auto oneLit = std::make_unique<LiteralExpr>(static_cast<long long>(1));
                        oneLit->line = bracketToken.line;
                        oneLit->column = bracketToken.column;
                        endIdx = std::make_unique<BinaryExpr>("+", std::move(endIdx), std::move(oneLit));
                    }
                    consume(TokenType::RBRACKET, "Expected ']' after range slice");
                    std::vector<std::unique_ptr<Expression>> args;
                    args.push_back(std::move(expr));
                    args.push_back(std::move(index));
                    args.push_back(std::move(endIdx));
                    auto callExpr = std::make_unique<CallExpr>("array_slice", std::move(args));
                    callExpr->fromStdNamespace = true;
                    callExpr->line = bracketToken.line;
                    callExpr->column = bracketToken.column;
                    expr = std::move(callExpr);
                } else {
                    consume(TokenType::RBRACKET, "Expected ']' after array index");
                    auto indexExpr = std::make_unique<IndexExpr>(std::move(expr), std::move(index));
                    indexExpr->line = bracketToken.line;
                    indexExpr->column = bracketToken.column;
                    expr = std::move(indexExpr);
                }
            }
        }
        // Handle field access (dot notation) or method call (obj.method(args))
        else if (match(TokenType::DOT)) {
            const Token dotToken = tokens[current - 1];
            // Tuple element access: t.0, t.1, t.2, ...
            // An integer literal immediately after '.' is a tuple index.
            if (check(TokenType::INTEGER)) {
                const Token idxToken = advance();
                const std::string idxStr = std::to_string(idxToken.intValue);
                auto fieldExpr = std::make_unique<FieldAccessExpr>(std::move(expr), idxStr);
                fieldExpr->line = dotToken.line;
                fieldExpr->column = dotToken.column;
                expr = std::move(fieldExpr);
            } else {
            // Accept IDENTIFIER or SWAP (and any future keyword that doubles as a
            // field/method name) after '.'.  This mirrors C/C++ where keywords are
            // valid struct member names when disambiguated by the '.' context.
            Token fieldToken = (check(TokenType::IDENTIFIER) || check(TokenType::SWAP))
                                   ? advance()
                                   : consume(TokenType::IDENTIFIER, "Expected field name after '.'");
            // Method call: obj.method(args...) desugars to method(obj, args...)
            if (check(TokenType::LPAREN)) {
                advance(); // consume '('
                std::vector<std::unique_ptr<Expression>> arguments;
                arguments.push_back(std::move(expr)); // receiver becomes first argument
                if (!check(TokenType::RPAREN)) {
                    do {
                        if (match(TokenType::RANGE)) {
                            const Token spreadToken = tokens[current - 1];
                            auto operand = parseExpression();
                            auto node = std::make_unique<SpreadExpr>(std::move(operand));
                            node->line = spreadToken.line;
                            node->column = spreadToken.column;
                            arguments.push_back(std::move(node));
                        } else {
                            arguments.push_back(parseExpression());
                        }
                    } while (match(TokenType::COMMA));
                }
                consume(TokenType::RPAREN, "Expected ')' after method call arguments");
                auto callExpr = std::make_unique<CallExpr>(fieldToken.lexeme, std::move(arguments));
                callExpr->fromStdNamespace = true; // method-call syntax desugaring
                callExpr->line = dotToken.line;
                callExpr->column = dotToken.column;
                expr = std::move(callExpr);
            } else {
                auto fieldExpr = std::make_unique<FieldAccessExpr>(std::move(expr), fieldToken.lexeme);
                fieldExpr->line = dotToken.line;
                fieldExpr->column = dotToken.column;
                expr = std::move(fieldExpr);
            }
            } // end else (non-integer field)
        }
        // Handle C-style arrow operator: ptr->field / ptr->method(args)
        // Identical semantics to ptr.field / ptr.method(args) — OmScript's
        // field access already auto-dereferences pointer-typed variables.
        else if (match(TokenType::ARROW)) {
            const Token arrowToken = tokens[current - 1];
            // Same as DOT: accept IDENTIFIER or keyword tokens as field names.
            Token fieldToken = (check(TokenType::IDENTIFIER) || check(TokenType::SWAP))
                                   ? advance()
                                   : consume(TokenType::IDENTIFIER, "Expected field name after '->'");
            if (check(TokenType::LPAREN)) {
                advance(); // consume '('
                std::vector<std::unique_ptr<Expression>> arguments;
                arguments.push_back(std::move(expr)); // receiver as first arg
                if (!check(TokenType::RPAREN)) {
                    do {
                        if (match(TokenType::RANGE)) {
                            const Token spreadToken = tokens[current - 1];
                            auto operand = parseExpression();
                            auto node = std::make_unique<SpreadExpr>(std::move(operand));
                            node->line = spreadToken.line;
                            node->column = spreadToken.column;
                            arguments.push_back(std::move(node));
                        } else {
                            arguments.push_back(parseExpression());
                        }
                    } while (match(TokenType::COMMA));
                }
                consume(TokenType::RPAREN, "Expected ')' after method call arguments");
                auto callExpr = std::make_unique<CallExpr>(fieldToken.lexeme, std::move(arguments));
                callExpr->fromStdNamespace = true; // method-call via arrow
                callExpr->line = arrowToken.line;
                callExpr->column = arrowToken.column;
                expr = std::move(callExpr);
            } else {
                auto fieldExpr = std::make_unique<FieldAccessExpr>(std::move(expr), fieldToken.lexeme);
                fieldExpr->line = arrowToken.line;
                fieldExpr->column = arrowToken.column;
                expr = std::move(fieldExpr);
            }
        } else {
            break;
        }
    }

    return expr;
}

std::unique_ptr<Expression> Parser::parseCall() {
    auto expr = parsePrimary();

    if (match(TokenType::LPAREN)) {
        // Function call
        if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto idExpr = dynamic_cast<IdentifierExpr*>(expr.get());
            std::vector<std::unique_ptr<Expression>> arguments;

            // Named arguments: foo(width: 4, height: 5)
            // Detect if any argument uses the `name: expr` form.
            // Named and positional args may be mixed: positional args come first,
            // named args can appear in any order after positional args.
            struct NamedArg {
                std::string name;
                std::unique_ptr<Expression> value;
                int line = 0;
                int column = 0;
            };
            std::vector<std::unique_ptr<Expression>> positionalArgs;
            std::vector<NamedArg> namedArgs;
            bool seenNamed = false;

            if (!check(TokenType::RPAREN)) {
                do {
                    // Spread argument: ...expr
                    if (match(TokenType::RANGE)) {
                        const Token spreadToken = tokens[current - 1];
                        auto operand = parseExpression();
                        auto node = std::make_unique<SpreadExpr>(std::move(operand));
                        node->line = spreadToken.line;
                        node->column = spreadToken.column;
                        if (seenNamed) {
                            error("Positional/spread arguments cannot follow named arguments");
                        }
                        positionalArgs.push_back(std::move(node));
                    } else {
                        // Peek: if next is IDENT COLON, it's a named arg
                        bool isNamed = (check(TokenType::IDENTIFIER) &&
                                        current + 1 < tokens.size() &&
                                        tokens[current + 1].type == TokenType::COLON);
                        if (isNamed) {
                            seenNamed = true;
                            const Token argNameTok = advance(); // consume name
                            advance(); // consume ':'
                            auto val = parseExpression();
                            namedArgs.push_back({argNameTok.lexeme, std::move(val), argNameTok.line, argNameTok.column});
                        } else {
                            if (seenNamed) {
                                error("Positional arguments cannot follow named arguments");
                            }
                            positionalArgs.push_back(parseExpression());
                        }
                    }
                } while (match(TokenType::COMMA));
            }

            consume(TokenType::RPAREN, "Expected ')' after arguments");

            // Resolve bare function calls from globally-imported user namespaces.
            std::string calleeName = idExpr->name;
            {
                auto importedNameIt = bareImportedNames_.find(calleeName);
                if (importedNameIt != bareImportedNames_.end())
                    calleeName = importedNameIt->second;
            }

            if (!seenNamed) {
                // Fast path: no named args — use positional args as-is
                arguments = std::move(positionalArgs);
            } else {
                // Reorder named args to match the function's declaration order.
                auto pit = funcParamNames_.find(calleeName);
                if (pit == funcParamNames_.end()) {
                    // Also try the unqualified name (may have been mangled)
                    pit = funcParamNames_.find(idExpr->name);
                }
                if (pit != funcParamNames_.end()) {
                    const auto& paramNames = pit->second;
                    // Start with positional args already in order
                    arguments = std::move(positionalArgs);
                    // Build a map of named args
                    std::unordered_map<std::string, size_t> namedArgIdx;
                    for (size_t k = 0; k < namedArgs.size(); ++k)
                        namedArgIdx[namedArgs[k].name] = k;
                    // Fill remaining positions using param order
                    const size_t posCount = arguments.size();
                    for (size_t k = posCount; k < paramNames.size(); ++k) {
                        auto it = namedArgIdx.find(paramNames[k]);
                        if (it != namedArgIdx.end()) {
                            arguments.push_back(std::move(namedArgs[it->second].value));
                        } else {
                            error("Named argument call missing argument for parameter '" + paramNames[k] + "'");
                        }
                    }
                    // Check for extra named args (names not in the param list)
                    for (const auto& na : namedArgs) {
                        bool found = false;
                        for (const auto& pn : paramNames)
                            if (pn == na.name) { found = true; break; }
                        if (!found && na.value != nullptr) {
                            error("Unknown named argument '" + na.name + "' for function '" + calleeName + "'");
                        }
                    }
                } else {
                    // Unknown function — append named args in the order given
                    if (!namedArgs.empty()) {
                        const NamedArg& firstNamed = namedArgs.front();
                        warnings_.push_back("warning: line " + std::to_string(firstNamed.line) + ":" +
                                            std::to_string(firstNamed.column) +
                                            ": named arguments are ignored for unresolved call '" +
                                            calleeName + "'");
                    }
                    arguments = std::move(positionalArgs);
                    for (auto& na : namedArgs)
                        arguments.push_back(std::move(na.value));
                }
            }

            auto callExpr = std::make_unique<CallExpr>(calleeName, std::move(arguments));
            callExpr->line = expr->line;
            callExpr->column = expr->column;
            // Consume and apply the namespace-resolution flag set by parsePrimary.
            if (lastCallWasNsResolved_) {
                callExpr->fromStdNamespace = true;
                lastCallWasNsResolved_ = false;
            }
            return callExpr;
        } else {
            error("Invalid function call");
        }
    }

    return expr;
}

std::unique_ptr<Expression> Parser::parsePrimary() {
    // 'when' in expression context: when val { 1 => e1, 2 => e2, _ => e3 }
    // Desugars to a ternary chain: (val==1) ? e1 : (val==2) ? e2 : e3
    if (check(TokenType::WHEN)) {
        const Token kw = tokens[current];
        advance(); // consume 'when'
        return parseWhenExpr();
    }

    // Anonymous function expression: fn(params) -> RetType { body }
    // or: fn(params) { body }  (return type inferred as i64/default)
    // Desugars to a lambda — named __lambda_N — exactly like |params| { body }.
    if (check(TokenType::FN) &&
        current + 1 < tokens.size() && tokens[current + 1].type == TokenType::LPAREN) {
        const Token fnTok = tokens[current];
        advance(); // consume 'fn'
        advance(); // consume '('
        std::vector<std::string> params;
        std::vector<std::string> paramTypes;
        if (!check(TokenType::RPAREN)) {
            do {
                if (check(TokenType::RPAREN)) break; // trailing comma
                const Token pname = consume(TokenType::IDENTIFIER, "Expected parameter name");
                std::string ptype = "i64";
                if (match(TokenType::COLON))
                    ptype = parseTypeAnnotation();
                params.push_back(pname.lexeme);
                paramTypes.push_back(ptype);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "Expected ')' after anonymous function parameters");
        // Optional explicit return type: -> RetType
        std::string retType = "i64";
        if (check(TokenType::ARROW)) {
            advance(); // consume '->'
            retType = parseTypeAnnotation();
        }
        // Body must be a block.
        consume(TokenType::LBRACE, "Expected '{' for anonymous function body");
        // Re-use parseLambda body helper — build a block and store as a lambda function.
        const std::string lambdaName = "__lambda_" + std::to_string(lambdaCounter_++);
        std::vector<Parameter> fnParams;
        for (size_t i = 0; i < params.size(); ++i) {
            Parameter p(params[i]);
            p.typeName = paramTypes[i];
            fnParams.push_back(std::move(p));
        }
        // Parse statements until '}'.
        std::vector<std::unique_ptr<Statement>> stmts;
        while (!check(TokenType::RBRACE) && !isAtEnd())
            stmts.push_back(parseStatement());
        consume(TokenType::RBRACE, "Expected '}' to close anonymous function body");
        auto block = std::make_unique<BlockStmt>(std::move(stmts));
        block->line = fnTok.line;
        block->column = fnTok.column;
        auto fnDecl =
            std::make_unique<FunctionDecl>(lambdaName, std::vector<std::string>{}, std::move(fnParams), std::move(block));
        fnDecl->line = fnTok.line;
        fnDecl->column = fnTok.column;
        fnDecl->returnType = retType;
        lambdaFunctions_.push_back(std::move(fnDecl));
        auto nameId = std::make_unique<IdentifierExpr>(lambdaName);
        nameId->line = fnTok.line;
        nameId->column = fnTok.column;
        return nameId;
    }
    // likely(expr) / unlikely(expr) — branch-weight expression hint.
    // Strip the hint and return the inner expression.  The compiler retains
    // correctness; LLVM branch-weight metadata is not added in this path.
    if (check(TokenType::LIKELY) || check(TokenType::UNLIKELY)) {
        // Only treat as expression if followed by '(' (function-call style).
        // Otherwise fall through to the statement-level likely/unlikely handler.
        if (current + 1 < tokens.size() && tokens[current + 1].type == TokenType::LPAREN) {
            advance(); // consume 'likely'/'unlikely'
            advance(); // consume '('
            auto inner = parseExpression();
            consume(TokenType::RPAREN, "Expected ')' after likely()/unlikely() expression");
            return inner;
        }
    }
    // comptime { ... } — compile-time evaluated block expression.
    // Evaluates the block at compile time and returns its result as a constant.
    if (match(TokenType::COMPTIME)) {
        const Token kw = tokens[current - 1];
        consume(TokenType::LBRACE, "Expected '{' after 'comptime'");
        // Re-use parseBlock but we've already consumed the '{'.
        // Collect statements until '}'.
        std::vector<std::unique_ptr<Statement>> stmts;
        while (!check(TokenType::RBRACE) && !isAtEnd()) {
            stmts.push_back(parseStatement());
        }
        consume(TokenType::RBRACE, "Expected '}' to close comptime block");
        auto block = std::make_unique<BlockStmt>(std::move(stmts));
        auto expr = std::make_unique<ComptimeExpr>(std::move(block));
        expr->line = kw.line;
        expr->column = kw.column;
        return expr;
    }

    if (match(TokenType::BYTES_LITERAL)) {
        // 0x"AABBCC" — hex byte array literal.
        // Desugar at parse time into an ArrayExpr of integer literals,
        // so no changes are needed in the type system or codegen.
        const Token token = tokens[current - 1];
        const std::string& hexStr = token.lexeme; // pairs of hex digits, e.g. "DEADBEEF"
        std::vector<std::unique_ptr<Expression>> elems;
        elems.reserve(hexStr.size() / 2);
        for (size_t i = 0; i < hexStr.size(); i += 2) {
            const std::string pair = hexStr.substr(i, 2);
            const long long byteVal = std::stoll(pair, nullptr, 16);
            auto lit = std::make_unique<LiteralExpr>(byteVal);
            lit->line = token.line;
            lit->column = token.column;
            elems.push_back(std::move(lit));
        }
        auto arr = std::make_unique<ArrayExpr>(std::move(elems));
        arr->line = token.line;
        arr->column = token.column;
        return arr;
    }

    if (match(TokenType::INTEGER)) {
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(token.intValue);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    if (match(TokenType::FLOAT)) {
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(token.floatValue);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    // Character literal: 'A', '\n', '\u0041' — yields i32 code point.
    if (match(TokenType::CHAR_LITERAL)) {
        const Token token = tokens[current - 1];
        long long codePoint = std::stoll(token.lexeme);
        auto expr = std::make_unique<LiteralExpr>(codePoint);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    if (match(TokenType::STRING)) {
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(token.lexeme);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    if (match(TokenType::TRUE)) {
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(static_cast<long long>(1));
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    if (match(TokenType::FALSE) || match(TokenType::NULL_LITERAL)) {
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(static_cast<long long>(0));
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    // alloc<T>(x) — smart typed allocator.
    // Decides stack vs heap at compile time based on whether x is a
    // compile-time constant and fits within the stack threshold.
    // Returns a ptr<T> pointing to x contiguous elements of type T.
    // alloc<T>() with no argument allocates exactly one element (Ω spec §4.1).
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "alloc" && current + 1 < tokens.size() &&
        tokens[current + 1].type == TokenType::LT) {
        const Token kw = advance(); // consume 'alloc'
        advance();                  // consume '<'
        std::string elemTypeName = parseTypeAnnotation();
        consume(TokenType::GT, "Expected '>' after type in alloc<T>(...)");
        consume(TokenType::LPAREN, "Expected '(' after alloc<T>");
        // Encode the element type in the callee name so codegen can recover it
        // without modifying the CallExpr AST.
        std::vector<std::unique_ptr<Expression>> args;
        if (!check(TokenType::RPAREN)) {
            // alloc<T>(n) — explicit count
            args.push_back(parseExpression());
        }
        // alloc<T>() — zero args means allocate 1 element (handled in codegen)
        consume(TokenType::RPAREN, "Expected ')' after alloc<T>(...) arguments");
        auto call = std::make_unique<CallExpr>("alloc<" + elemTypeName + ">", std::move(args));
        call->line = kw.line;
        call->column = kw.column;
        return call;
    }

    // new T(count) — zero-initialised allocation (semantically distinct from alloc<T>).
    // Syntax: new <TypeAnnotation>(n)  or  new <TypeAnnotation>  (no parens = 1 element)
    // Examples: new i64(5)   new ptr<i32>   new byte(256)
    //
    // Unlike alloc<T>(n) which returns raw (uninitialised) memory, new T(n) always
    // returns zero-filled memory:
    //   T1/T2 (stack/arena): alloc + memset(0)
    //   T3    (heap):        calloc(count, sizeof(T))
    //
    // new T { field: val, ... } — allocation + in-place construction.
    // Allocates one T (via alloc<T>(1)) and immediately initialises its
    // fields.  Distinguished from the zero-init form by the `{` lookahead.
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "new" && current + 1 < tokens.size() &&
        tokens[current + 1].type == TokenType::IDENTIFIER) {
        const Token kw = advance();                       // consume 'new'
        std::string elemTypeName = parseTypeAnnotation(); // e.g. "MyStruct", "i64"

        // ── new T { field: val, ... } ──────────────────────────────────────
        // Detect brace-initialised form: new TypeName { ... }
        if (check(TokenType::LBRACE)) {
            advance(); // consume '{'
            std::vector<std::pair<std::string, std::unique_ptr<Expression>>> fields;
            while (!check(TokenType::RBRACE) && !isAtEnd()) {
                const Token fname = consume(TokenType::IDENTIFIER, "Expected field name in new T { ... } initialiser");
                consume(TokenType::COLON, "Expected ':' after field name in new T { ... } initialiser");
                auto val = parseExpression();
                fields.emplace_back(fname.lexeme, std::move(val));
                if (check(TokenType::COMMA))
                    advance(); // trailing comma OK
            }
            consume(TokenType::RBRACE, "Expected '}' to close new T { ... } initialiser");
            auto expr = std::make_unique<NewConstructExpr>(elemTypeName, std::move(fields));
            expr->line = kw.line;
            expr->column = kw.column;
            return expr;
        }

        // ── new T(count) / new T  — zero-initialised allocation ──────────────
        // Unlike alloc<T>(n) which returns raw (uninitialised) memory, new T(n)
        // returns zero-initialised memory.  The callee prefix "new_zero<" routes
        // through the same three-tier allocator but appends a zero-fill step:
        //   T1/T2 (stack/arena): memset(ptr, 0, count*sizeof(T))
        //   T3 (heap):           calloc(count, sizeof(T))   — OS-zeroed, no overhead
        std::vector<std::unique_ptr<Expression>> args;
        if (check(TokenType::LPAREN)) {
            advance(); // consume '('
            if (!check(TokenType::RPAREN)) {
                args.push_back(parseExpression());
            }
            consume(TokenType::RPAREN, "Expected ')' after new T(...) argument");
        }
        // No parens → new_zero<T>() → 1 zero-initialised element
        auto call = std::make_unique<CallExpr>("new_zero<" + elemTypeName + ">", std::move(args));
        call->line = kw.line;
        call->column = kw.column;
        return call;
    }

    // pslice_new<T>(ptr, len) — create a fat pointer slice over T elements.
    // The <T> type parameter is encoded into the callee name for codegen recovery.
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "pslice_new" && current + 1 < tokens.size() &&
        tokens[current + 1].type == TokenType::LT) {
        const Token kw = advance(); // consume 'pslice_new'
        advance();                  // consume '<'
        std::string elemTypeName = parseTypeAnnotation();
        consume(TokenType::GT, "Expected '>' after type in pslice_new<T>(...)");
        consume(TokenType::LPAREN, "Expected '(' after pslice_new<T>");
        std::vector<std::unique_ptr<Expression>> args;
        args.push_back(parseExpression()); // ptr argument
        consume(TokenType::COMMA, "Expected ',' between pslice_new<T> arguments");
        args.push_back(parseExpression()); // len argument
        consume(TokenType::RPAREN, "Expected ')' after pslice_new<T>(...) arguments");
        auto call = std::make_unique<CallExpr>("pslice_new<" + elemTypeName + ">", std::move(args));
        call->line = kw.line;
        call->column = kw.column;
        return call;
    }

    // The argument MUST be a type name (identifier), not an expression.
    // Produces an integer literal equal to the byte size of T.
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "sizeof" && current + 1 < tokens.size() &&
        tokens[current + 1].type == TokenType::LPAREN) {
        const Token kw = advance(); // consume 'sizeof'
        advance();                  // consume '('
        // Parse the type annotation inside the parens.
        const std::string typeName = parseTypeAnnotation();
        consume(TokenType::RPAREN, "Expected ')' after type name in sizeof(...)");
        // Compute size at parse time using known bit widths.
        long long byteSize = 8; // default (pointer/int64)
        if (typeName == "bool" || typeName == "i8" || typeName == "u8" || typeName == "byte")
            byteSize = 1;
        else if (typeName == "i16" || typeName == "u16" || typeName == "c_short" || typeName == "c_ushort")
            byteSize = 2;
        else if (typeName == "i32" || typeName == "u32" || typeName == "f32" || typeName == "float32" ||
                 typeName == "c_float" ||
                 typeName == "char" || typeName == "c_int" || typeName == "c_uint")
            byteSize = 4;
        else if (typeName == "i64" || typeName == "u64" || typeName == "int" || typeName == "uint" ||
                 typeName == "float" || typeName == "double" || typeName == "f64" || typeName == "float64" ||
                 typeName == "c_double" || typeName == "c_long_double" ||
                 typeName == "ptr" || typeName.rfind("ptr<", 0) == 0 || typeName == "string" || typeName == "bigint" ||
                 typeName == "usize" || typeName == "isize" ||
                 typeName == "c_long" || typeName == "c_ulong" ||
                 typeName == "c_longlong" || typeName == "c_ulonglong" ||
                 typeName == "c_size_t" || typeName == "c_ssize_t" ||
                 typeName == "intptr_t" || typeName == "uintptr_t" || typeName == "ptrdiff_t" ||
                 typeName == "c_intptr" || typeName == "c_uintptr" || typeName == "c_ptrdiff" ||
                 typeName == "c_FILE" || typeName == "c_dir" || typeName == "c_DIR" || typeName == "c_jmp_buf" ||
                 typeName == "File")
            byteSize = 8;
        else if (typeName == "i128" || typeName == "u128")
            byteSize = 16;
        else if (typeName == "i256" || typeName == "u256")
            byteSize = 32;
        else if (typeName.rfind("tuple<", 0) == 0 && typeName.back() == '>') {
            // Conservative tight-packed tuple size: sum of element sizes.
            // Actual size may be larger due to LLVM struct alignment padding.
            std::string inner = typeName.substr(6, typeName.size() - 7);
            long long total = 0;
            // Split elements on commas (depth-aware to handle nested types).
            int depth = 0;
            std::string cur;
            for (char ch : inner) {
                if (ch == '<') depth++;
                else if (ch == '>') depth--;
                if (ch == ',' && depth == 0) {
                    // Recursively compute element size using same rules.
                    // Re-use the sizeof parse-time table via a helper lambda.
                    auto elemSize = [&](const std::string& t) -> long long {
                        if (t == "bool" || t == "i8" || t == "u8" || t == "byte") return 1;
                        if (t == "i16" || t == "u16") return 2;
                        if (t == "i32" || t == "u32" || t == "f32" || t == "char" || t == "c_int" || t == "c_uint") return 4;
                        if (t == "i128" || t == "u128") return 16;
                        if (t == "i256" || t == "u256") return 32;
                        const char* s = t.c_str();
                        if ((s[0]=='i'||s[0]=='u') && s[1]!='\0') {
                            int bits = std::atoi(s+1);
                            if (bits>=1 && bits<=256) return (bits+7)/8;
                        }
                        return 8; // pointer/int64/float default
                    };
                    // Strip leading/trailing whitespace from cur
                    while (!cur.empty() && cur.front()==' ') cur.erase(0,1);
                    while (!cur.empty() && cur.back()==' ') cur.pop_back();
                    total += elemSize(cur);
                    cur.clear();
                } else {
                    cur += ch;
                }
            }
            if (!cur.empty()) {
                while (!cur.empty() && cur.front()==' ') cur.erase(0,1);
                while (!cur.empty() && cur.back()==' ') cur.pop_back();
                auto elemSize2 = [](const std::string& t) -> long long {
                    if (t == "bool" || t == "i8" || t == "u8" || t == "byte") return 1;
                    if (t == "i16" || t == "u16") return 2;
                    if (t == "i32" || t == "u32" || t == "f32" || t == "char" || t == "c_int" || t == "c_uint") return 4;
                    if (t == "i128" || t == "u128") return 16;
                    if (t == "i256" || t == "u256") return 32;
                    const char* s = t.c_str();
                    if ((s[0]=='i'||s[0]=='u') && s[1]!='\0') { int bits=std::atoi(s+1); if (bits>=1&&bits<=256) return (bits+7)/8; }
                    return 8;
                };
                total += elemSize2(cur);
            }
            byteSize = total;
        } else {
            // iN / uN — compute from bit width
            const char* s = typeName.c_str();
            if ((s[0] == 'i' || s[0] == 'u') && s[1] != '\0') {
                int bits = std::atoi(s + 1);
                if (bits >= 1 && bits <= 256)
                    byteSize = (bits + 7) / 8;
            }
        }
        auto expr = std::make_unique<LiteralExpr>(byteSize);
        expr->line = kw.line;
        expr->column = kw.column;
        return expr;
    }

    if (match(TokenType::IDENTIFIER)) {
        const Token token = tokens[current - 1];
        // Scope resolution: handles single-level (Enum::Member) and multi-level
        // (alias::sub::func) paths.
        //
        // Rules:
        //  - If the chain ends with '(' it is a qualified function call: build a
        //    flat identifier "a__b__c" that parseCall() will wrap into CallExpr.
        //  - If the chain is exactly 2 levels and NOT followed by '(' it is an
        //    enum member access: preserve the existing ScopeResolutionExpr path.
        //  - Any other multi-level use not followed by '(' also becomes a flat
        //    IdentifierExpr (e.g. used as a first-class value, error caught later).
        if (match(TokenType::SCOPE)) {
            // Collect the full scope chain into a segment vector.
            std::vector<std::string> segments;
            segments.push_back(token.lexeme);
            segments.push_back(consume(TokenType::IDENTIFIER, "Expected identifier after '::'").lexeme);
            int depth = 1;
            while (check(TokenType::SCOPE)) {
                advance(); // consume '::'
                segments.push_back(consume(TokenType::IDENTIFIER, "Expected identifier after '::'").lexeme);
                ++depth;
            }

            if (check(TokenType::LPAREN)) {
                // ── Priority 1: built-in type method dispatch ───────────────────
                // Recognises  type::method(args...)  for the core primitive types
                // and desugars to the appropriate BinaryExpr, UnaryExpr, or
                // CallExpr — completely at parse time, zero extra codegen.
                if (segments.size() == 2) {
                    const std::string& tname = segments[0];
                    const std::string& mname = segments[1];

                    // Canonical type groups
                    static const std::unordered_set<std::string> kIntTypes{"int", "i64", "i32", "i16", "i8",
                                                                           "u64", "u32", "u16", "u8",  "uint",
                                                                           "usize", "isize",
                                                                           "c_int", "c_uint", "c_short", "c_ushort",
                                                                           "c_long", "c_ulong", "c_longlong", "c_ulonglong",
                                                                           "c_size_t", "c_ssize_t", "c_char", "c_uchar",
                                                                           "intptr_t", "uintptr_t", "ptrdiff_t",
                                                                           "c_intptr", "c_uintptr", "c_ptrdiff",
                                                                           "char"};
                    static const std::unordered_set<std::string> kFloatTypes{"float", "f64", "f32", "double",
                                                                             "c_float", "c_double", "c_long_double"};
                    static const std::unordered_set<std::string> kStrTypes{"string", "str"};
                    static const std::unordered_set<std::string> kArrTypes{"array", "arr"};
                    static const std::unordered_set<std::string> kBoolTypes{"bool"};

                    const bool isInt = kIntTypes.count(tname) != 0 || isIntWidthTypeName(tname);
                    const bool isFloat = kFloatTypes.count(tname) != 0;
                    const bool isStr = kStrTypes.count(tname) != 0;
                    const bool isArr = kArrTypes.count(tname) != 0;
                    const bool isBool = kBoolTypes.count(tname) != 0;

                    if (isInt || isFloat || isStr || isArr || isBool) {
                        // Determine the kind of this method BEFORE consuming args.
                        // Supported kinds:
                        //   "B:<op>"  — binary expression
                        //   "U:<op>"  — unary expression
                        //   "C:<fn>"  — call to named builtin
                        //   ""        — unknown method for this type, fall through
                        std::string kind;

                        // ── Shared int+float methods ─────────────────────────────
                        if (isInt || isFloat) {
                            if (mname == "add")
                                kind = "B:+";
                            else if (mname == "sub")
                                kind = "B:-";
                            else if (mname == "mul")
                                kind = "B:*";
                            else if (mname == "div")
                                kind = "B:/";
                            else if (mname == "neg")
                                kind = "U:-";
                            else if (mname == "abs")
                                kind = "C:abs";
                            else if (mname == "min")
                                kind = "C:min";
                            else if (mname == "max")
                                kind = "C:max";
                            else if (mname == "pow")
                                kind = "C:pow";
                            else if (mname == "clamp")
                                kind = "C:clamp";
                            else if (mname == "eq")
                                kind = "B:==";
                            else if (mname == "ne")
                                kind = "B:!=";
                            else if (mname == "lt")
                                kind = "B:<";
                            else if (mname == "le")
                                kind = "B:<=";
                            else if (mname == "gt")
                                kind = "B:>";
                            else if (mname == "ge")
                                kind = "B:>=";
                            else if (mname == "to_string")
                                kind = "C:to_string";
                            // Fast-math arithmetic (float-biased, reassoc/nnan)
                            else if (mname == "fast_add")
                                kind = "C:fast_add";
                            else if (mname == "fast_sub")
                                kind = "C:fast_sub";
                            else if (mname == "fast_mul")
                                kind = "C:fast_mul";
                            else if (mname == "fast_div")
                                kind = "C:fast_div";
                            // Precise/strict IEEE arithmetic
                            else if (mname == "precise_add")
                                kind = "C:precise_add";
                            else if (mname == "precise_sub")
                                kind = "C:precise_sub";
                            else if (mname == "precise_mul")
                                kind = "C:precise_mul";
                            else if (mname == "precise_div")
                                kind = "C:precise_div";
                        }
                        // ── Int-only methods ─────────────────────────────────────
                        // For typed integers (i8/i16/i32/u8/u16/u32), width-specific
                        // bit ops use kind "W:op" which generates __tw_<op>_<width>
                        // so codegen emits the narrower LLVM intrinsic directly.
                        // For generic int/i64/u64 the plain "C:op" path is used.
                        if (isInt && kind.empty()) {
                            // Parse the declared bit-width from the type name (e.g. 32 from "i32").
                            // Width 0 means "use generic path".
                            int declaredWidth = 0;
                            if (tname.size() >= 2 && (tname[0] == 'i' || tname[0] == 'u')) {
                                int w = 0;
                                bool ok = true;
                                for (size_t j = 1; j < tname.size(); ++j) {
                                    if (!std::isdigit(static_cast<unsigned char>(tname[j]))) {
                                        ok = false;
                                        break;
                                    }
                                    w = w * 10 + (tname[j] - '0');
                                }
                                if (ok && w >= 1 && w <= 64 && w != 64)
                                    declaredWidth = w;
                            }
                            // Helpers: use width-specific path only when a sub-64-bit width is known.
                            auto widthKind = [&](const char* op) -> std::string {
                                if (declaredWidth > 0)
                                    return std::string("W:") + op;
                                return std::string("C:") + op;
                            };
                            if (mname == "mod")
                                kind = "B:%";
                            else if (mname == "sign")
                                kind = "C:sign";
                            else if (mname == "is_even")
                                kind = "C:is_even";
                            else if (mname == "is_odd")
                                kind = "C:is_odd";
                            else if (mname == "to_float")
                                kind = "C:to_float";
                            else if (mname == "bitand")
                                kind = "B:&";
                            else if (mname == "bitor")
                                kind = "B:|";
                            else if (mname == "bitxor")
                                kind = "B:^";
                            else if (mname == "bitnot")
                                kind = "U:~";
                            else if (mname == "shl")
                                kind = "B:<<";
                            else if (mname == "shr")
                                kind = "B:>>";
                            // Bit-counting: emit width-specific LLVM intrinsic for iN types
                            else if (mname == "popcount")
                                kind = widthKind("popcount");
                            else if (mname == "clz")
                                kind = widthKind("clz");
                            else if (mname == "ctz")
                                kind = widthKind("ctz");
                            else if (mname == "bitreverse")
                                kind = widthKind("bitreverse");
                            else if (mname == "bswap")
                                kind = widthKind("bswap");
                            // Bit rotation (width-specific avoids masking to 63)
                            else if (mname == "rotl" || mname == "rotate_left")
                                kind = widthKind("rotate_left");
                            else if (mname == "rotr" || mname == "rotate_right")
                                kind = widthKind("rotate_right");
                            // Overflow-safe arithmetic (width-specific uses iN sat intrinsics)
                            else if (mname == "saturating_add")
                                kind = widthKind("saturating_add");
                            else if (mname == "saturating_sub")
                                kind = widthKind("saturating_sub");
                            // Number-theory helpers
                            else if (mname == "is_power_of_2")
                                kind = "C:is_power_of_2";
                            else if (mname == "gcd")
                                kind = "C:gcd";
                            else if (mname == "lcm")
                                kind = "C:lcm";
                            // Conversions
                            else if (mname == "to_char")
                                kind = "C:to_char";
                            else if (mname == "char_code")
                                kind = "C:char_code";
                            // High-performance integer arithmetic
                            else if (mname == "mulhi")
                                kind = "C:mulhi";
                            else if (mname == "mulhi_u")
                                kind = "C:mulhi_u";
                            else if (mname == "absdiff")
                                kind = "C:absdiff";
                            // Store width in kind for W: dispatch.
                            if (kind.size() >= 2 && kind[0] == 'W' && declaredWidth > 0)
                                kind += ":" + std::to_string(declaredWidth);
                        }
                        // ── Float-only methods ───────────────────────────────────
                        // For f32, use kind "F:op" to emit 32-bit LLVM intrinsics,
                        // avoiding f32↔f64 conversions entirely.
                        if (isFloat && kind.empty()) {
                            const bool isF32 = (tname == "f32" || tname == "float32");
                            auto floatKind = [&](const char* op) -> std::string {
                                return isF32 ? std::string("F:") + op : std::string("C:") + op;
                            };
                            if (mname == "sqrt")
                                kind = floatKind("sqrt");
                            else if (mname == "floor")
                                kind = "C:floor";
                            else if (mname == "ceil")
                                kind = "C:ceil";
                            else if (mname == "round")
                                kind = "C:round";
                            else if (mname == "to_int")
                                kind = "C:to_int";
                            // Trigonometry (f32 uses 32-bit LLVM intrinsics)
                            else if (mname == "sin")
                                kind = floatKind("sin");
                            else if (mname == "cos")
                                kind = floatKind("cos");
                            else if (mname == "tan")
                                kind = floatKind("tan");
                            else if (mname == "asin")
                                kind = floatKind("asin");
                            else if (mname == "acos")
                                kind = floatKind("acos");
                            else if (mname == "atan")
                                kind = floatKind("atan");
                            else if (mname == "atan2")
                                kind = floatKind("atan2");
                            // Transcendentals
                            else if (mname == "log")
                                kind = floatKind("log");
                            else if (mname == "log2")
                                kind = floatKind("log2");
                            else if (mname == "log10")
                                kind = floatKind("log10");
                            else if (mname == "exp")
                                kind = floatKind("exp");
                            else if (mname == "exp2")
                                kind = floatKind("exp2");
                            else if (mname == "cbrt")
                                kind = floatKind("cbrt");
                            // Multi-arg float ops
                            else if (mname == "hypot")
                                kind = floatKind("hypot");
                            else if (mname == "fma")
                                kind = floatKind("fma");
                            else if (mname == "copysign")
                                kind = floatKind("copysign");
                            // Fast/approximate float intrinsics
                            else if (mname == "fast_sqrt")
                                kind = floatKind("fast_sqrt");
                            // Predicates
                            else if (mname == "is_nan")
                                kind = "C:is_nan";
                            else if (mname == "is_inf")
                                kind = "C:is_inf";
                            else if (mname == "min_float")
                                kind = "C:min_float";
                            else if (mname == "max_float")
                                kind = "C:max_float";
                        }
                        // ── String methods ───────────────────────────────────────
                        if (isStr) {
                            if (mname == "len")
                                kind = "C:len";
                            else if (mname == "concat")
                                kind = "B:+";
                            else if (mname == "eq")
                                kind = "C:str_eq";
                            else if (mname == "contains")
                                kind = "C:str_contains";
                            else if (mname == "starts_with")
                                kind = "C:str_starts_with";
                            else if (mname == "ends_with")
                                kind = "C:str_ends_with";
                            else if (mname == "index_of")
                                kind = "C:str_index_of";
                            else if (mname == "replace")
                                kind = "C:str_replace";
                            else if (mname == "repeat")
                                kind = "C:str_repeat";
                            else if (mname == "char_at")
                                kind = "C:char_at";
                            else if (mname == "to_upper" || mname == "upper")
                                kind = "C:str_upper";
                            else if (mname == "to_lower" || mname == "lower")
                                kind = "C:str_lower";
                            else if (mname == "trim")
                                kind = "C:str_trim";
                            else if (mname == "lstrip")
                                kind = "C:str_lstrip";
                            else if (mname == "rstrip")
                                kind = "C:str_rstrip";
                            else if (mname == "split")
                                kind = "C:str_split";
                            else if (mname == "substr")
                                kind = "C:str_substr";
                            else if (mname == "reverse")
                                kind = "C:str_reverse";
                            else if (mname == "pad_left")
                                kind = "C:str_pad_left";
                            else if (mname == "pad_right")
                                kind = "C:str_pad_right";
                            else if (mname == "count")
                                kind = "C:str_count";
                            else if (mname == "find")
                                kind = "C:str_find";
                            else if (mname == "format")
                                kind = "C:str_format";
                            else if (mname == "chars")
                                kind = "C:str_chars";
                            else if (mname == "remove")
                                kind = "C:str_remove";
                            else if (mname == "join")
                                kind = "C:str_join";
                            else if (mname == "to_int")
                                kind = "C:to_int";
                            else if (mname == "to_float")
                                kind = "C:to_float";
                            else if (mname == "is_alpha")
                                kind = "C:is_alpha";
                            else if (mname == "is_digit")
                                kind = "C:is_digit";
                            else if (mname == "to_string")
                                kind = "C:to_string";
                            else if (mname == "filter")
                                kind = "C:str_filter";
                        }
                        // ── Array methods ────────────────────────────────────────
                        if (isArr) {
                            if (mname == "len")
                                kind = "C:len";
                            else if (mname == "fill")
                                kind = "C:array_fill";
                            else if (mname == "contains")
                                kind = "C:array_contains";
                            else if (mname == "pop")
                                kind = "C:pop";
                            else if (mname == "push")
                                kind = "C:push";
                            else if (mname == "remove")
                                kind = "C:array_remove";
                            else if (mname == "sort")
                                kind = "C:sort";
                            else if (mname == "min")
                                kind = "C:array_min";
                            else if (mname == "max")
                                kind = "C:array_max";
                            else if (mname == "sum")
                                kind = "C:sum";
                            else if (mname == "product")
                                kind = "C:array_product";
                            else if (mname == "mean")
                                kind = "C:array_mean";
                            else if (mname == "reverse")
                                kind = "C:reverse";
                            else if (mname == "index_of")
                                kind = "C:index_of";
                            else if (mname == "last")
                                kind = "C:array_last";
                            else if (mname == "unique")
                                kind = "C:array_unique";
                            else if (mname == "zip")
                                kind = "C:array_zip";
                            else if (mname == "filter")
                                kind = "C:array_filter";
                            else if (mname == "map")
                                kind = "C:array_map";
                            else if (mname == "reduce")
                                kind = "C:array_reduce";
                            else if (mname == "any")
                                kind = "C:array_any";
                            else if (mname == "every")
                                kind = "C:array_every";
                            else if (mname == "count")
                                kind = "C:array_count";
                            else if (mname == "take")
                                kind = "C:array_take";
                            else if (mname == "drop")
                                kind = "C:array_drop";
                            else if (mname == "rotate")
                                kind = "C:array_rotate";
                            else if (mname == "insert")
                                kind = "C:array_insert";
                            else if (mname == "find")
                                kind = "C:array_find";
                            else if (mname == "concat")
                                kind = "C:array_concat";
                            else if (mname == "slice")
                                kind = "C:array_slice";
                            else if (mname == "copy")
                                kind = "C:array_copy";
                        }
                        // ── Bool methods ─────────────────────────────────────────
                        if (isBool) {
                            if (mname == "and")
                                kind = "B:&&";
                            else if (mname == "or")
                                kind = "B:||";
                            else if (mname == "not")
                                kind = "U:!";
                            else if (mname == "to_string")
                                kind = "C:to_string";
                        }

                        if (!kind.empty()) {
                            // Now safe to consume the argument list.
                            advance(); // consume '('
                            std::vector<std::unique_ptr<Expression>> args;
                            if (!check(TokenType::RPAREN)) {
                                do {
                                    args.push_back(parseExpression());
                                } while (match(TokenType::COMMA));
                            }
                            consume(TokenType::RPAREN, "Expected ')' after '" + tname + "::" + mname + "' arguments");

                            const char kp = kind[0];                // 'B', 'U', 'C', 'W', or 'F'
                            const std::string val = kind.substr(2); // op / function name

                            if (kp == 'B') {
                                if (args.size() != 2)
                                    error("'" + tname + "::" + mname + "' requires exactly 2 arguments");
                                auto e = std::make_unique<BinaryExpr>(val, std::move(args[0]), std::move(args[1]));
                                e->line = token.line;
                                e->column = token.column;
                                return e;
                            }
                            if (kp == 'U') {
                                if (args.size() != 1)
                                    error("'" + tname + "::" + mname + "' requires exactly 1 argument");
                                auto e = std::make_unique<UnaryExpr>(val, std::move(args[0]));
                                e->line = token.line;
                                e->column = token.column;
                                return e;
                            }
                            // kp == 'W': width-typed integer intrinsic.
                            // val = "popcount:32"  →  callee "__tw_popcount_32"
                            if (kp == 'W') {
                                const auto colon = val.find(':');
                                const std::string opName = (colon != std::string::npos) ? val.substr(0, colon) : val;
                                const std::string widthStr = (colon != std::string::npos) ? val.substr(colon + 1) : "";
                                const std::string callee = "__tw_" + opName + (widthStr.empty() ? "" : "_" + widthStr);
                                auto e = std::make_unique<CallExpr>(callee, std::move(args));
                                e->fromStdNamespace = true; // type-method desugaring
                                e->line = token.line;
                                e->column = token.column;
                                return e;
                            }
                            // kp == 'F': f32-typed float intrinsic.
                            // val = "sin"  →  callee "__tf_sin"
                            if (kp == 'F') {
                                const std::string callee = "__tf_" + val;
                                auto e = std::make_unique<CallExpr>(callee, std::move(args));
                                e->fromStdNamespace = true; // type-method desugaring
                                e->line = token.line;
                                e->column = token.column;
                                return e;
                            }
                            // kp == 'C': named builtin call
                            auto e = std::make_unique<CallExpr>(val, std::move(args));
                            e->fromStdNamespace = true; // type-method desugaring
                            e->line = token.line;
                            e->column = token.column;
                            return e;
                        }
                        // Unknown method for this type — fall through to other
                        // resolution strategies (namespace registry, flat name).
                    }
                }

                // ── Priority 2: namespace registry resolution ───────────────────
                const std::string resolved = resolveNamespacedPath(segments);
                if (!resolved.empty()) {
                    // Signal parseCall() that the resulting CallExpr was reached
                    // via a namespace-qualified path (e.g. std::abs).
                    lastCallWasNsResolved_ = true;
                    auto e = std::make_unique<IdentifierExpr>(resolved);
                    e->line = token.line;
                    e->column = token.column;
                    return e;
                }
                // ── Priority 3: struct static-method call ──────────────────────
                // If the first segment is a known struct name (e.g. Counter),
                // treat  Counter::new(args)  as a static/class-method call by
                // emitting a CallExpr with the fully-qualified mangled name.
                // Codegen will find it in the function map directly (function was
                // declared as  fn Counter::new(...)  which stores name "Counter::new").
                if (structNames_.count(segments[0])) {
                    // Build mangled callee name: "Counter::new" or deeper.
                    std::string callee = segments[0];
                    for (int si = 1; si < (int)segments.size(); ++si)
                        callee += "::" + segments[si];
                    advance(); // consume '('
                    std::vector<std::unique_ptr<Expression>> args;
                    if (!check(TokenType::RPAREN)) {
                        do {
                            if (check(TokenType::RANGE)) {
                                advance();
                                auto spreadArg = parseExpression();
                                auto se = std::make_unique<SpreadExpr>(std::move(spreadArg));
                                se->line = token.line;
                                se->column = token.column;
                                args.push_back(std::move(se));
                            } else {
                                args.push_back(parseExpression());
                            }
                        } while (match(TokenType::COMMA));
                    }
                    consume(TokenType::RPAREN, "Expected ')' after arguments");
                    auto e = std::make_unique<CallExpr>(callee, std::move(args));
                    e->line = token.line;
                    e->column = token.column;
                    return e;
                }
                // ── Priority 4: unknown namespace — hard error ─────────────────
                // resolveNamespacedPath() returned empty, meaning no namespace
                // prefix in the registry matched.  This is always a compile
                // error: either the namespace was never imported, it was
                // misspelled, or the function does not exist in it.
                {
                    const std::string nsName = segments[0];
                    // Build a helpful candidates list from registered namespaces.
                    std::vector<std::string> knownNS;
                    for (const auto& [ns, _] : importNamespaces_)
                        knownNS.push_back(ns);
                    std::string msg = "Unknown namespace '" + nsName + "'";
                    if (!knownNS.empty()) {
                        msg += ". Known namespaces: ";
                        for (size_t ki = 0; ki < knownNS.size(); ++ki) {
                            if (ki)
                                msg += ", ";
                            msg += knownNS[ki];
                        }
                    } else {
                        msg += ". No namespaces are currently imported "
                               "(use 'import \"file\" as name' to register one, "
                               "or call 'std::synthesize' which is built-in)";
                    }
                    error(msg);
                }
            }

            // Not a call: single-level → check for namespace struct literal, imported global, then classic enum member
            // access.
            if (depth == 1) {
                // Check: is this a namespace-qualified struct literal? e.g. Math::Vec2 { x: 1, y: 2 }
                {
                    const std::string qualName = segments[0] + "::" + segments[1];
                    if (structNames_.count(qualName) && check(TokenType::LBRACE)) {
                        advance(); // consume '{'
                        return parseStructLiteral(qualName, token.line, token.column);
                    }
                }
                // Priority: cross-file global variable access (e.g. math::PI)
                {
                    auto gvNsIt = importedGlobalVars_.find(segments[0]);
                    if (gvNsIt != importedGlobalVars_.end()) {
                        auto gvIt = gvNsIt->second.find(segments[1]);
                        if (gvIt != gvNsIt->second.end()) {
                            auto e = std::make_unique<IdentifierExpr>(gvIt->second);
                            e->line = token.line;
                            e->column = token.column;
                            return e;
                        }
                    }
                }
                // Namespace function reference in non-call position (e.g. as a callback argument).
                // e.g.  array_map(arr, Ops::double_val)  — resolve to IdentifierExpr("Ops::double_val")
                // so that extractFnName() and codegen can locate the function by its qualified name.
                {
                    auto nsIt = importNamespaces_.find(segments[0]);
                    if (nsIt != importNamespaces_.end()) {
                        auto fnIt = nsIt->second.find(segments[1]);
                        if (fnIt != nsIt->second.end()) {
                            auto e = std::make_unique<IdentifierExpr>(fnIt->second);
                            e->line = token.line;
                            e->column = token.column;
                            return e;
                        }
                    }
                }
                // Fallback: classic enum member access — also handle bare enum names that
                // were declared inside a namespace (e.g. `Code::OK` inside namespace Status
                // should emit ScopeResolutionExpr("Status::Code", "OK")).
                {
                    std::string scopeName = segments[0];
                    auto bev = bareEnumNames_.find(scopeName);
                    if (bev != bareEnumNames_.end())
                        scopeName = bev->second;
                    auto e = std::make_unique<ScopeResolutionExpr>(scopeName, segments[1]);
                    e->line = token.line;
                    e->column = token.column;
                    return e;
                }
            }
            // Multi-level value reference without '()': attempt global lookup,
            // then check for struct literal, then try function reference, then error.
            {
                // Try longest-prefix global lookup: e.g. a::b::c
                // where "a::b" is a namespace and "c" is a global variable.
                for (int cut = (int)segments.size() - 1; cut >= 1; --cut) {
                    std::string ns = segments[0];
                    for (int i = 1; i < cut; ++i)
                        ns += "::" + segments[i];
                    std::string varName = segments[cut];
                    for (size_t i = (size_t)cut + 1; i < segments.size(); ++i)
                        varName += "::" + segments[i];
                    auto gvNsIt = importedGlobalVars_.find(ns);
                    if (gvNsIt != importedGlobalVars_.end()) {
                        auto gvIt = gvNsIt->second.find(varName);
                        if (gvIt != gvNsIt->second.end()) {
                            auto e = std::make_unique<IdentifierExpr>(gvIt->second);
                            e->line = token.line;
                            e->column = token.column;
                            return e;
                        }
                    }
                }
                // Try namespace function reference for multi-level paths:
                // e.g. array_map(arr, Ops::Math::square) — resolve to IdentifierExpr("Ops::Math::square").
                // Also check for struct literals: e.g. Geom::Shape::Circle { cx: 0, cy: 0, r: 3 }.
                // Also check for enum access: Ns::EnumType::VARIANT → ScopeResolutionExpr(qualEnum, member).
                // Build prefix path (all segments except last) and check if it's a known enum.
                {
                    std::string enumPrefix;
                    for (size_t i = 0; i + 1 < segments.size(); ++i) {
                        if (i > 0) enumPrefix += "::";
                        enumPrefix += segments[i];
                    }
                    const std::string& memberName = segments.back();
                    // Also try resolving via bareEnumNames_ (e.g. Ns::BareCode::OK where BareCode is short)
                    std::string resolvedEnum = enumPrefix;
                    {
                        auto bev = bareEnumNames_.find(enumPrefix);
                        if (bev != bareEnumNames_.end())
                            resolvedEnum = bev->second;
                    }
                    if (enumNames_.count(resolvedEnum)) {
                        auto e = std::make_unique<ScopeResolutionExpr>(resolvedEnum, memberName);
                        e->line = token.line;
                        e->column = token.column;
                        return e;
                    }
                }
                for (int cut = (int)segments.size() - 1; cut >= 1; --cut) {
                    std::string ns;
                    for (int i = 0; i < cut; ++i) {
                        if (i > 0) ns += "::";
                        ns += segments[i];
                    }
                    auto nsIt = importNamespaces_.find(ns);
                    if (nsIt == importNamespaces_.end()) continue;
                    std::string fn;
                    for (int i = cut; i < (int)segments.size(); ++i) {
                        if (i > cut) fn += "::";
                        fn += segments[i];
                    }
                    auto fnIt = nsIt->second.find(fn);
                    if (fnIt != nsIt->second.end()) {
                        const std::string qualName = fnIt->second;
                        // Multi-level namespace struct literal: Ns::Sub::Struct { ... }
                        if (structNames_.count(qualName) && check(TokenType::LBRACE)) {
                            advance(); // consume '{'
                            return parseStructLiteral(qualName, token.line, token.column);
                        }
                        // Function / variable reference in non-call position
                        auto e = std::make_unique<IdentifierExpr>(qualName);
                        e->line = token.line;
                        e->column = token.column;
                        return e;
                    }
                }
                // No namespace resolved — emit a clear error.
                std::string path = segments[0];
                for (size_t i = 1; i < segments.size(); ++i)
                    path += "::" + segments[i];
                error("Cannot resolve '" + path +
                      "': no matching namespace or global found. "
                      "Import the file with 'import \"file\" as alias' first.");
            }
        }
        // Check if this is a struct literal: StructName { field: value, ... }
        if (structNames_.count(token.lexeme) && check(TokenType::LBRACE)) {
            advance(); // consume '{'
            // Resolve bare name through bare-import map (e.g. Vec2 → Math::Vec2 after import Math;)
            std::string structName = token.lexeme;
            {
                auto importedNameIt = bareImportedNames_.find(structName);
                if (importedNameIt != bareImportedNames_.end())
                    structName = importedNameIt->second;
            }
            return parseStructLiteral(structName, token.line, token.column);
        }
        // `dict { key: val, ... }` — keyword-prefixed dict literal.
        // Equivalent to `{ key: val, ... }` but more explicit.
        if (token.lexeme == "dict" && check(TokenType::LBRACE)) {
            advance(); // consume '{'
            const Token braceToken = tokens[current - 1];
            std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> pairs;
            if (!check(TokenType::RBRACE)) {
                do {
                    if (check(TokenType::RBRACE))
                        break;
                    auto key = parseExpression();
                    consume(TokenType::COLON, "Expected ':' after dict key");
                    auto val = parseExpression();
                    pairs.emplace_back(std::move(key), std::move(val));
                } while (match(TokenType::COMMA));
            }
            consume(TokenType::RBRACE, "Expected '}' after dict literal");
            auto node = std::make_unique<DictExpr>(std::move(pairs));
            node->line = token.line;
            node->column = token.column;
            return node;
        }
        auto expr = std::make_unique<IdentifierExpr>(token.lexeme);
        expr->line = token.line;
        expr->column = token.column;
        // Consume optional :type annotation on identifier in expression context
        // (e.g. x:u32 in function call arguments, assignments, range bounds).
        // Only consume when the type name is a known type to avoid ambiguity
        // with ternary operator colons and other colon uses.
        if (check(TokenType::COLON) && current + 1 < tokens.size() &&
            tokens[current + 1].type == TokenType::IDENTIFIER && isKnownScalarTypeName(tokens[current + 1].lexeme)) {
            advance(); // consume ':'
            advance(); // consume type name
        }
        // Single-parameter arrow lambda without parentheses: x => expr
        // Only trigger when '=>' immediately follows the identifier (no type
        // annotation consumed above) to avoid ambiguity with ternary / when arms.
        if (check(TokenType::FAT_ARROW)) {
            const Token arrowTok = advance(); // consume '=>'
            return parseArrowLambda({token.lexeme}, {"i64"}, arrowTok);
        }
        return expr;
    }

    // Allow keyword tokens that double as builtin function names to be used
    // in expression context (e.g. swap(arr, i, j) as a function call).
    if (match(TokenType::SWAP)) {
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<IdentifierExpr>(token.lexeme);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    // Switch expression: switch(cond) { case v: expr, default: expr }
    // Also supports paren-free form: switch cond { ... }
    // Desugared at parse time to an IIFE call (see parseSwitchExpr).
    if (check(TokenType::SWITCH)) {
        advance(); // consume 'switch'
        return parseSwitchExpr();
    }

    // Arrow lambda: (params) => expr  or  () => expr
    // Detected via look-ahead: LPAREN...RPAREN FAT_ARROW
    if (isArrowLambdaParens()) {
        const Token lpTok = tokens[current]; // '(' not yet consumed
        advance(); // consume '('

        std::vector<std::string> params;
        std::vector<std::string> paramTypes;

        if (!check(TokenType::RPAREN)) {
            // Parse comma-separated  name[:type]  entries
            do {
                if (check(TokenType::RPAREN))
                    break; // trailing comma
                const Token pname = consume(TokenType::IDENTIFIER, "Expected parameter name in arrow lambda");
                std::string ptype = "i64"; // default
                if (match(TokenType::COLON))
                    ptype = parseTypeAnnotation();
                params.push_back(pname.lexeme);
                paramTypes.push_back(ptype);
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RPAREN, "Expected ')' after arrow lambda parameters");
        const Token arrowTok = consume(TokenType::FAT_ARROW, "Expected '=>' in arrow lambda");
        return parseArrowLambda(params, paramTypes, arrowTok);
    }

    if (match(TokenType::LPAREN)) {
        const Token lpTok = tokens[current - 1];
        // Tuple literal: (e1, e2, ...) — two or more comma-separated expressions.
        // Single-element or empty parens are just grouping (parenthesized expression).
        // Strategy: parse the first expression, then check for a comma.
        auto firstExpr = parseExpression();
        if (check(TokenType::COMMA)) {
            // It's a tuple literal — gather all elements.
            std::vector<std::unique_ptr<Expression>> elements;
            elements.push_back(std::move(firstExpr));
            while (match(TokenType::COMMA)) {
                if (check(TokenType::RPAREN)) break; // trailing comma
                elements.push_back(parseExpression());
            }
            consume(TokenType::RPAREN, "Expected ')' after tuple elements");
            auto tupleExpr = std::make_unique<TupleExpr>(std::move(elements));
            tupleExpr->line = lpTok.line;
            tupleExpr->column = lpTok.column;
            return tupleExpr;
        }
        consume(TokenType::RPAREN, "Expected ')' after expression");
        return firstExpr;
    }

    if (match(TokenType::LBRACKET)) {
        const Token bracketToken = tokens[current - 1];
        // `[]T{...}` typed array literal — `[` `]` type-name `{` elems `}`
        // The type annotation is informational only; the array is homogeneous
        // i64 at runtime (same as untyped array literals).  This desugars
        // to a plain ArrayExpr at parse time.
        if (check(TokenType::RBRACKET) && current + 1 < tokens.size() &&
            tokens[current + 1].type == TokenType::IDENTIFIER) {
            // Peek ahead: []TypeName{ — typed array literal
            const size_t saved = current;
            advance(); // consume ']'
            // Parse the type name (may include <T> generics, but we just skip it)
            advance(); // consume type name identifier
            // Skip optional type parameters: e.g. []ptr<Foo>{...}
            if (check(TokenType::LT)) {
                int depth = 1;
                advance(); // consume '<'
                while (depth > 0 && !isAtEnd()) {
                    if (check(TokenType::LT))
                        ++depth;
                    else if (check(TokenType::GT)) {
                        --depth;
                    }
                    advance();
                }
            }
            if (check(TokenType::LBRACE)) {
                // Confirmed: []TypeName{...} form — parse elements inside {}
                advance(); // consume '{'
                auto arrayExpr = std::make_unique<ArrayExpr>(std::vector<std::unique_ptr<Expression>>{});
                if (!check(TokenType::RBRACE)) {
                    do {
                        if (check(TokenType::RBRACE))
                            break;
                        arrayExpr->elements.push_back(parseExpression());
                    } while (match(TokenType::COMMA));
                }
                consume(TokenType::RBRACE, "Expected '}' after typed array literal elements");
                arrayExpr->line = bracketToken.line;
                arrayExpr->column = bracketToken.column;
                return arrayExpr;
            }
            // Not a typed array literal — restore position and fall through to
            // the regular array literal / slice-syntax parsing.
            current = saved;
        }
        auto arrayExpr = parseArrayLiteral();
        arrayExpr->line = bracketToken.line;
        arrayExpr->column = bracketToken.column;
        return arrayExpr;
    }

    // Lambda expression: |params| body
    if (match(TokenType::PIPE)) {
        return parseLambda();
    }

    // Dict literal: {"key": val, "key2": val2, ...}
    // Desugared at parse time into a DictExpr for zero-cost codegen
    // (single malloc + direct stores, no loops).
    // Note: works in expression context; standalone dict-statement uses LBRACE
    // which the statement-level parser already routes to block parsing.
    if (match(TokenType::LBRACE)) {
        const Token braceToken = tokens[current - 1];
        std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> pairs;
        if (!check(TokenType::RBRACE)) {
            do {
                // Allow trailing comma before '}'
                if (check(TokenType::RBRACE))
                    break;
                auto key = parseExpression();
                consume(TokenType::COLON, "Expected ':' after dict key");
                auto val = parseExpression();
                pairs.emplace_back(std::move(key), std::move(val));
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACE, "Expected '}' after dict literal");
        auto node = std::make_unique<DictExpr>(std::move(pairs));
        node->line = braceToken.line;
        node->column = braceToken.column;
        return node;
    }

    // Lambda with || (empty params): || body
    if (match(TokenType::OR)) {
        // || is the empty-parameter lambda: || expr
        const Token orToken = tokens[current - 1];
        auto body = parseExpression();
        auto node = std::make_unique<LambdaExpr>(std::vector<std::string>{}, std::move(body));
        node->line = orToken.line;
        node->column = orToken.column;

        // Desugar to named function
        const std::string lambdaName = "__lambda_" + std::to_string(lambdaCounter_++);
        std::vector<Parameter> fnParams;
        auto returnStmt = std::make_unique<ReturnStmt>(std::move(node->body));
        returnStmt->line = orToken.line;
        returnStmt->column = orToken.column;
        std::vector<std::unique_ptr<Statement>> stmts;
        stmts.push_back(std::move(returnStmt));
        auto block = std::make_unique<BlockStmt>(std::move(stmts));
        block->line = orToken.line;
        block->column = orToken.column;
        auto fnDecl = std::make_unique<FunctionDecl>(lambdaName, std::vector<std::string>{}, std::move(fnParams),
                                                     std::move(block));
        fnDecl->line = orToken.line;
        fnDecl->column = orToken.column;
        lambdaFunctions_.push_back(std::move(fnDecl));

        // Return the lambda name as an identifier (generates LLVM Function* in codegen,
        // works for both funcptr stores and array_map/filter/reduce via extractFnName).
        auto nameId = std::make_unique<IdentifierExpr>(lambdaName);
        nameId->line = orToken.line;
        nameId->column = orToken.column;
        return nameId;
    }

    error("Expected expression");
    return nullptr;
}

std::unique_ptr<Expression> Parser::parseArrayLiteral() {
    std::vector<std::unique_ptr<Expression>> elements;

    if (!check(TokenType::RBRACKET)) {
        do {
            // Handle spread operator: ...expr
            if (match(TokenType::RANGE)) {
                const Token spreadToken = tokens[current - 1];
                auto operand = parseExpression();
                auto node = std::make_unique<SpreadExpr>(std::move(operand));
                node->line = spreadToken.line;
                node->column = spreadToken.column;
                elements.push_back(std::move(node));
            } else {
                elements.push_back(parseExpression());
            }
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::RBRACKET, "Expected ']' after array elements");

    return std::make_unique<ArrayExpr>(std::move(elements));
}

std::unique_ptr<Expression> Parser::parsePipe() {
    auto expr = parseTernary();

    while (match(TokenType::PIPE_FORWARD)) {
        const Token pipeToken = tokens[current - 1];
        const Token fnName = consume(TokenType::IDENTIFIER, "Expected function name after '|>'");
        auto node = std::make_unique<PipeExpr>(std::move(expr), fnName.lexeme);
        node->line = pipeToken.line;
        node->column = pipeToken.column;
        expr = std::move(node);
    }

    return expr;
}

std::unique_ptr<Expression> Parser::parseLambda() {
    const Token pipeToken = tokens[current - 1]; // the opening |
    std::vector<std::string> params;
    std::vector<std::string> paramTypes;

    // Parse lambda parameters: |x| or |x, y| or || or |x:int| or |x:int, y:int|
    if (!check(TokenType::PIPE)) {
        do {
            const Token paramName = consume(TokenType::IDENTIFIER, "Expected parameter name in lambda");
            // Lambda parameters default to i64 when no explicit type annotation is
            // given.  Higher-kinded type inference is not yet available at parse time,
            // so we bind the most common element type.  Users can always annotate
            // explicitly: |x:float| x * 2.0
            std::string paramType = "i64";
            if (match(TokenType::COLON)) {
                paramType = parseTypeAnnotation();
            }
            params.push_back(paramName.lexeme);
            paramTypes.push_back(paramType);
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::PIPE, "Expected '|' after lambda parameters");

    // Desugar: generate a named function and return its name as an identifier.
    const std::string lambdaName = "__lambda_" + std::to_string(lambdaCounter_++);

    // Parse the lambda body: either a block `{ stmts; }` or a single expression.
    // Create the function: fn __lambda_N(params...) { body }
    std::vector<Parameter> fnParams;
    for (size_t i = 0; i < params.size(); ++i) {
        Parameter p(params[i]);
        p.typeName = (i < paramTypes.size()) ? paramTypes[i] : "i64";
        fnParams.push_back(std::move(p));
    }
    std::unique_ptr<BlockStmt> block;
    if (check(TokenType::LBRACE)) {
        // Block-body lambda: |params| { stmts; return val; }
        block = parseBlock();
        block->line = pipeToken.line;
        block->column = pipeToken.column;
    } else {
        // Expression-body lambda: |params| expr  (implicit return)
        auto body = parseExpression();
        auto returnStmt = std::make_unique<ReturnStmt>(std::move(body));
        returnStmt->line = pipeToken.line;
        returnStmt->column = pipeToken.column;
        std::vector<std::unique_ptr<Statement>> stmts;
        stmts.push_back(std::move(returnStmt));
        block = std::make_unique<BlockStmt>(std::move(stmts));
        block->line = pipeToken.line;
        block->column = pipeToken.column;
    }
    auto fnDecl =
        std::make_unique<FunctionDecl>(lambdaName, std::vector<std::string>{}, std::move(fnParams), std::move(block));
    fnDecl->line = pipeToken.line;
    fnDecl->column = pipeToken.column;

    lambdaFunctions_.push_back(std::move(fnDecl));

    // Return the lambda name as an identifier (generates LLVM Function* in codegen,
    // works for both funcptr stores and array_map/filter/reduce via extractFnName).
    auto nameId = std::make_unique<IdentifierExpr>(lambdaName);
    nameId->line = pipeToken.line;
    nameId->column = pipeToken.column;
    return nameId;
}

// ── isArrowLambdaParens ──────────────────────────────────────────────────────
// Look-ahead predicate: returns true when tokens[current] is '(' and the
// matching ')' is immediately followed by '=>' (FAT_ARROW).
// Used in parsePrimary to disambiguate  (expr)  from  (params) => body .
bool Parser::isArrowLambdaParens() const {
    if (tokens[current].type != TokenType::LPAREN)
        return false;
    size_t pos = current + 1; // first token inside the '('
    int depth = 1;
    while (pos < tokens.size() && depth > 0) {
        if (tokens[pos].type == TokenType::LPAREN)
            ++depth;
        else if (tokens[pos].type == TokenType::RPAREN)
            --depth;
        ++pos;
    }
    // pos now points to the token immediately after the closing ')'
    return pos < tokens.size() && tokens[pos].type == TokenType::FAT_ARROW;
}

// ── parseArrowLambda ─────────────────────────────────────────────────────────
// Shared desugar helper: given an already-collected param name / type list
// and the source location token, parse the body expression (after '=>') and
// emit an anonymous named function + return its name as a string literal
// (identical to parseLambda desugar so array_map etc. continue to work).
std::unique_ptr<Expression> Parser::parseArrowLambda(const std::vector<std::string>& params,
                                                      const std::vector<std::string>& paramTypes,
                                                      const Token& arrowTok) {
    // Desugar: generate a named function and return its name as an identifier.
    const std::string lambdaName = "__lambda_" + std::to_string(lambdaCounter_++);

    std::vector<Parameter> fnParams;
    for (size_t i = 0; i < params.size(); ++i) {
        Parameter p(params[i]);
        p.typeName = (i < paramTypes.size() && !paramTypes[i].empty()) ? paramTypes[i] : "i64";
        fnParams.push_back(std::move(p));
    }

    // Parse the lambda body: either a block `{ stmts; }` or a single expression.
    std::unique_ptr<BlockStmt> block;
    if (check(TokenType::LBRACE)) {
        // Block-body arrow lambda: (params) => { stmts; return val; }
        block = parseBlock();
        block->line = arrowTok.line;
        block->column = arrowTok.column;
    } else {
        // Expression-body arrow lambda: (params) => expr  (implicit return)
        auto body = parseExpression();
        auto returnStmt = std::make_unique<ReturnStmt>(std::move(body));
        returnStmt->line = arrowTok.line;
        returnStmt->column = arrowTok.column;
        std::vector<std::unique_ptr<Statement>> stmts;
        stmts.push_back(std::move(returnStmt));
        block = std::make_unique<BlockStmt>(std::move(stmts));
        block->line = arrowTok.line;
        block->column = arrowTok.column;
    }
    auto fnDecl =
        std::make_unique<FunctionDecl>(lambdaName, std::vector<std::string>{}, std::move(fnParams), std::move(block));
    fnDecl->line = arrowTok.line;
    fnDecl->column = arrowTok.column;
    lambdaFunctions_.push_back(std::move(fnDecl));

    // Return the lambda name as an identifier (generates LLVM Function* in codegen,
    // works for both funcptr stores and array_map/filter/reduce via extractFnName).
    auto nameId = std::make_unique<IdentifierExpr>(lambdaName);
    nameId->line = arrowTok.line;
    nameId->column = arrowTok.column;
    return nameId;
}

// ── parseSwitchExpr ──────────────────────────────────────────────────────────
// switch(cond) { case v: expr, case v2, v3: expr, default: expr }
//
// Desugars at parse time to an IIFE: a helper function __switch_N(__sw: i64)
// is generated with a proper switch statement where each arm returns the
// corresponding expression.  The call __switch_N(cond) is returned.
// This strategy cleanly handles side-effectful conditions and is optimised
// by LLVM (the call is inlined and the switch statement is lowered).
std::unique_ptr<Expression> Parser::parseSwitchExpr() {
    const Token kw = tokens[current - 1]; // the 'switch' token

    // Optional parens — paren-free or paren form both accepted.
    const bool hasParen = match(TokenType::LPAREN);
    auto condition = parseExpression();
    if (hasParen)
        consume(TokenType::RPAREN, "Expected ')' after switch expression condition");
    consume(TokenType::LBRACE, "Expected '{' after switch expression condition");

    const std::string switchName = "__switch_" + std::to_string(lambdaCounter_++);
    const std::string paramName  = "__sw";

    std::vector<SwitchCase> cases;
    bool hasDefault = false;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (match(TokenType::CASE)) {
            // One or more comma-separated values followed by ':'
            std::vector<std::unique_ptr<Expression>> caseValues;
            caseValues.push_back(parseExpression());
            while (match(TokenType::COMMA)) {
                if (check(TokenType::COLON))
                    break; // trailing comma before ':'
                caseValues.push_back(parseExpression());
            }
            consume(TokenType::COLON, "Expected ':' after case value(s) in switch expression");
            auto resultExpr = parseExpression();
            auto retStmt    = std::make_unique<ReturnStmt>(std::move(resultExpr));
            retStmt->line   = kw.line;
            retStmt->column = kw.column;
            std::vector<std::unique_ptr<Statement>> bodyStmts;
            bodyStmts.push_back(std::move(retStmt));
            cases.emplace_back(std::move(caseValues), std::move(bodyStmts), false);
            match(TokenType::COMMA); // optional trailing comma between arms
        } else if (match(TokenType::DEFAULT)) {
            if (hasDefault)
                error("Duplicate default case in switch expression");
            hasDefault = true;
            consume(TokenType::COLON, "Expected ':' after 'default' in switch expression");
            auto resultExpr = parseExpression();
            auto retStmt    = std::make_unique<ReturnStmt>(std::move(resultExpr));
            retStmt->line   = kw.line;
            retStmt->column = kw.column;
            std::vector<std::unique_ptr<Statement>> bodyStmts;
            bodyStmts.push_back(std::move(retStmt));
            cases.emplace_back(std::vector<std::unique_ptr<Expression>>{}, std::move(bodyStmts), true);
            match(TokenType::COMMA); // optional trailing comma
        } else {
            error("Expected 'case' or 'default' in switch expression");
        }
    }
    consume(TokenType::RBRACE, "Expected '}' after switch expression body");

    // Ensure there is always a default arm (return 0 if unspecified).
    if (!hasDefault) {
        auto zeroLit    = std::make_unique<LiteralExpr>(static_cast<long long>(0));
        zeroLit->line   = kw.line;
        zeroLit->column = kw.column;
        auto retStmt    = std::make_unique<ReturnStmt>(std::move(zeroLit));
        retStmt->line   = kw.line;
        retStmt->column = kw.column;
        std::vector<std::unique_ptr<Statement>> bodyStmts;
        bodyStmts.push_back(std::move(retStmt));
        cases.emplace_back(std::vector<std::unique_ptr<Expression>>{}, std::move(bodyStmts), true);
    }

    // Build switch statement: switch(__sw) { case ... }
    auto swCond = std::make_unique<IdentifierExpr>(paramName);
    swCond->line   = kw.line;
    swCond->column = kw.column;
    auto switchStmt = std::make_unique<SwitchStmt>(std::move(swCond), std::move(cases));
    switchStmt->line   = kw.line;
    switchStmt->column = kw.column;

    // Wrap in an IIFE function: fn __switch_N(__sw: i64) { switch(__sw) { ... } }
    std::vector<std::unique_ptr<Statement>> fnBodyStmts;
    fnBodyStmts.push_back(std::move(switchStmt));
    auto fnBlock    = std::make_unique<BlockStmt>(std::move(fnBodyStmts));
    fnBlock->line   = kw.line;
    fnBlock->column = kw.column;

    std::vector<Parameter> fnParams;
    Parameter p(paramName);
    p.typeName = "i64";
    fnParams.push_back(std::move(p));

    auto fnDecl = std::make_unique<FunctionDecl>(switchName, std::vector<std::string>{}, std::move(fnParams),
                                                 std::move(fnBlock));
    fnDecl->line   = kw.line;
    fnDecl->column = kw.column;
    lambdaFunctions_.push_back(std::move(fnDecl));

    // Return the call: __switch_N(condition)
    std::vector<std::unique_ptr<Expression>> callArgs;
    callArgs.push_back(std::move(condition));
    auto callExpr    = std::make_unique<CallExpr>(switchName, std::move(callArgs));
    callExpr->line   = kw.line;
    callExpr->column = kw.column;
    return callExpr;
}

OptMaxConfig Parser::parseOptMaxConfig() {
    OptMaxConfig cfg;
    cfg.enabled = true;
    consume(TokenType::LPAREN, "Expected '(' after @optmax");
    if (!check(TokenType::RPAREN)) {
        do {
            // Use advance() so keyword tokens (loop=LOOP, parallel=PARALLEL)
            // are accepted as config keys by their lexeme string.
            const Token key = advance();
            consume(TokenType::ASSIGN, "Expected '=' after key in @optmax config");
            if (key.lexeme == "safety") {
                const Token val = advance();
                if (val.lexeme == "off")
                    cfg.safety = SafetyLevel::Off;
                else if (val.lexeme == "relaxed")
                    cfg.safety = SafetyLevel::Relaxed;
                else
                    cfg.safety = SafetyLevel::On;
            } else if (key.lexeme == "fast_math") {
                const Token val = advance();
                cfg.fastMath = (val.lexeme == "true" || val.type == TokenType::TRUE);
            } else if (key.lexeme == "aggressive_vec") {
                const Token val = advance();
                cfg.aggressiveVec = (val.lexeme == "true" || val.type == TokenType::TRUE);
            } else if (key.lexeme == "report") {
                const Token val = advance();
                cfg.report = (val.lexeme == "true" || val.type == TokenType::TRUE);
            } else if (key.lexeme == "loop") {
                consume(TokenType::LBRACE, "Expected '{' for loop config");
                while (!check(TokenType::RBRACE) && !isAtEnd()) {
                    // advance() handles `parallel` which is a keyword token
                    const Token lk = advance();
                    consume(TokenType::ASSIGN, "Expected '=' in loop config");
                    if (lk.lexeme == "unroll") {
                        const Token v = advance();
                        if (v.type == TokenType::INTEGER)
                            cfg.loop.unrollCount = static_cast<int>(v.intValue);
                    } else if (lk.lexeme == "vectorize") {
                        const Token v = advance();
                        cfg.loop.vectorize = (v.lexeme == "true" || v.type == TokenType::TRUE);
                    } else if (lk.lexeme == "tile") {
                        const Token v = advance();
                        if (v.type == TokenType::INTEGER)
                            cfg.loop.tileSize = static_cast<int>(v.intValue);
                    } else if (lk.lexeme == "parallel") {
                        const Token v = advance();
                        cfg.loop.parallel = (v.lexeme == "true" || v.type == TokenType::TRUE);
                    }
                    if (!check(TokenType::RBRACE))
                        match(TokenType::COMMA);
                }
                consume(TokenType::RBRACE, "Expected '}' after loop config");
            } else if (key.lexeme == "memory") {
                consume(TokenType::LBRACE, "Expected '{' for memory config");
                while (!check(TokenType::RBRACE) && !isAtEnd()) {
                    const Token mk = advance();
                    consume(TokenType::ASSIGN, "Expected '=' in memory config");
                    const Token mv = advance();
                    if (mk.lexeme == "prefetch")
                        cfg.memory.prefetch = (mv.lexeme == "true" || mv.type == TokenType::TRUE);
                    else if (mk.lexeme == "noalias")
                        cfg.memory.noalias = (mv.lexeme == "true" || mv.type == TokenType::TRUE);
                    else if (mk.lexeme == "stack")
                        cfg.memory.preferStack = (mv.lexeme == "true" || mv.type == TokenType::TRUE);
                    if (!check(TokenType::RBRACE))
                        match(TokenType::COMMA);
                }
                consume(TokenType::RBRACE, "Expected '}' after memory config");
            } else if (key.lexeme == "assume") {
                consume(TokenType::LBRACKET, "Expected '[' for assume list");
                while (!check(TokenType::RBRACKET) && !isAtEnd()) {
                    const Token s = consume(TokenType::STRING, "Expected string in assume list");
                    cfg.assumes.push_back(s.lexeme);
                    if (!check(TokenType::RBRACKET))
                        match(TokenType::COMMA);
                }
                consume(TokenType::RBRACKET, "Expected ']' after assume list");
            } else if (key.lexeme == "specialize") {
                consume(TokenType::LBRACKET, "Expected '[' for specialize list");
                while (!check(TokenType::RBRACKET) && !isAtEnd()) {
                    const Token s = consume(TokenType::STRING, "Expected string in specialize list");
                    cfg.specialize.push_back(s.lexeme);
                    if (!check(TokenType::RBRACKET))
                        match(TokenType::COMMA);
                }
                consume(TokenType::RBRACKET, "Expected ']' after specialize list");
            }
            // skip unknown keys
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "Expected ')' after @optmax config");
    return cfg;
}

LoopConfig Parser::parseLoopAnnotation() {
    LoopConfig cfg;
    consume(TokenType::LPAREN, "Expected '(' after @loop");
    if (!check(TokenType::RPAREN)) {
        do {
            const Token key = consume(TokenType::IDENTIFIER, "Expected key in @loop config");
            consume(TokenType::ASSIGN, "Expected '=' after key in @loop");
            if (key.lexeme == "unroll") {
                const Token v = advance();
                if (v.type == TokenType::INTEGER) {
                    cfg.unrollCount = static_cast<int>(v.intValue);
                } else if (v.type == TokenType::IDENTIFIER) {
                    // Allow comptime constant identifiers as unroll count
                    auto it = comptimeConstants_.find(v.lexeme);
                    if (it != comptimeConstants_.end())
                        cfg.unrollCount = static_cast<int>(it->second);
                }
            } else if (key.lexeme == "vectorize") {
                const Token v = advance(); // true/false may be keywords
                cfg.vectorize = (v.lexeme == "true" || v.type == TokenType::TRUE);
                cfg.noVectorize = (v.lexeme == "false" || v.type == TokenType::FALSE);
            } else if (key.lexeme == "tile") {
                const Token v = advance();
                if (v.type == TokenType::INTEGER)
                    cfg.tileSize = static_cast<int>(v.intValue);
            } else if (key.lexeme == "parallel") {
                const Token v = advance();
                cfg.parallel = (v.lexeme == "true" || v.type == TokenType::TRUE);
            } else if (key.lexeme == "independent") {
                const Token v = advance();
                cfg.independent = (v.lexeme == "true" || v.type == TokenType::TRUE);
            } else if (key.lexeme == "fuse") {
                const Token v = advance();
                cfg.fuse = (v.lexeme == "true" || v.type == TokenType::TRUE);
            }
        } while (match(TokenType::COMMA));
    }
    consume(TokenType::RPAREN, "Expected ')' after @loop config");
    return cfg;
}

std::unique_ptr<Statement> Parser::parseAssumeStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'assume'");
    auto cond = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after assume condition");

    std::unique_ptr<Statement> deoptBody = nullptr;
    if (check(TokenType::IDENTIFIER) && peek().lexeme == "else") {
        advance(); // consume 'else'
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "deopt") {
            advance(); // consume 'deopt'
        }
        deoptBody = parseStatement();
    }

    match(TokenType::SEMICOLON); // optional semicolon

    return std::make_unique<AssumeStmt>(std::move(cond), std::move(deoptBody));
}

} // namespace omscript
