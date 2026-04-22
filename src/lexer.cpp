#include "lexer.h"
#include "diagnostic.h"
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace omscript {

namespace {
[[nodiscard]] [[gnu::always_inline]] inline bool isDigit(char c) noexcept {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}
[[nodiscard]] [[gnu::always_inline]] inline bool isHexDigit(char c) noexcept {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}
[[nodiscard]] [[gnu::always_inline]] inline bool isAlpha(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}
[[nodiscard]] [[gnu::always_inline]] inline bool isAlnum(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}
} // namespace

// Use string_view keys so keyword lookups avoid allocating a std::string
// for every identifier token.  The string literals have static storage
// duration so the views remain valid for the lifetime of the program.
static const std::unordered_map<std::string_view, TokenType> keywords = {
    {"fn", TokenType::FN},         {"return", TokenType::RETURN},     {"if", TokenType::IF},
    {"else", TokenType::ELSE},     {"while", TokenType::WHILE},       {"do", TokenType::DO},
    {"for", TokenType::FOR},       {"var", TokenType::VAR},           {"const", TokenType::CONST},
    {"break", TokenType::BREAK},   {"continue", TokenType::CONTINUE}, {"in", TokenType::IN},
    {"true", TokenType::TRUE},     {"false", TokenType::FALSE},       {"null", TokenType::NULL_LITERAL},
    {"switch", TokenType::SWITCH}, {"case", TokenType::CASE},         {"default", TokenType::DEFAULT},
    {"try", TokenType::TRY},       {"catch", TokenType::CATCH},       {"throw", TokenType::THROW},
    {"enum", TokenType::ENUM},
    {"struct", TokenType::STRUCT},
    {"import", TokenType::IMPORT},
    {"move", TokenType::MOVE},
    {"invalidate", TokenType::INVALIDATE},
    {"borrow", TokenType::BORROW},
    {"freeze", TokenType::FREEZE},
    {"mut", TokenType::MUT},
    {"prefetch", TokenType::PREFETCH},
    {"likely", TokenType::LIKELY},
    {"unlikely", TokenType::UNLIKELY},
    {"register", TokenType::REGISTER},
    {"unless", TokenType::UNLESS},
    {"until", TokenType::UNTIL},
    {"loop", TokenType::LOOP},
    {"repeat", TokenType::REPEAT},
    {"defer", TokenType::DEFER},
    {"guard", TokenType::GUARD},
    {"when", TokenType::WHEN},
    {"forever", TokenType::FOREVER},
    {"foreach", TokenType::FOREACH},
    {"elif", TokenType::ELIF},
    {"swap", TokenType::SWAP},
    {"times", TokenType::TIMES},
    {"with", TokenType::WITH},
    {"parallel", TokenType::PARALLEL},
    {"comptime", TokenType::COMPTIME},
    {"reborrow", TokenType::REBORROW},
    {"pipeline", TokenType::PIPELINE},
    {"stage",    TokenType::STAGE},
    {"global",   TokenType::GLOBAL}};

/// Throw a DiagnosticError with the given message and source location.
[[noreturn]] [[gnu::cold]] static void lexError(const std::string& msg, int ln, int col) {
    throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", ln, col}, msg});
}

Lexer::Lexer(const std::string& source) : source(source), pos(0), line(1), column(1) {}

Lexer::Lexer(std::string&& source) : source(std::move(source)), pos(0), line(1), column(1) {}

char Lexer::peek(int offset) const noexcept {
    const size_t index = pos + offset;
    if (index >= source.length()) {
        return '\0';
    }
    return source[index];
}

char Lexer::advance() noexcept {
    if (isAtEnd())
        return '\0';
    const char c = source[pos++];
    if (c == '\n') {
        line++;
        column = 1;
    } else {
        column++;
    }
    return c;
}

bool Lexer::isAtEnd() const noexcept {
    return pos >= source.length();
}

void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
        const char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            skipComment();
        } else if (c == '/' && peek(1) == '*') {
            skipBlockComment();
        } else {
            break;
        }
    }
}

void Lexer::skipComment() {
    // Skip //
    advance();
    advance();
    while (!isAtEnd() && peek() != '\n') {
        advance();
    }
}

void Lexer::skipBlockComment() {
    const int startLine = line;
    const int startColumn = column;
    // Skip /*
    advance();
    advance();
    while (!isAtEnd()) {
        if (peek() == '*' && peek(1) == '/') {
            advance(); // Skip *
            advance(); // Skip /
            return;
        }
        advance();
    }
    lexError("Unterminated block comment", startLine, startColumn);
}

Token Lexer::makeToken(TokenType type, const std::string& lexeme) {
    return Token(type, lexeme, line, column - lexeme.length());
}

Token Lexer::scanNumber() {
    std::string num;
    bool isFloat = false;

    // Check for hex (0x), octal (0o), or binary (0b) prefix
    if (peek() == '0') {
        const char prefix = peek(1);
        if (prefix == 'x' || prefix == 'X') {
            // Peek further: 0x"..." is a bytes literal, 0x<hexdigit> is an integer
            if (peek(2) == '"') {
                // Bytes literal: 0x"AABBCC..." — pairs of hex digits become byte values
                advance(); // '0'
                advance(); // 'x'/'X'
                advance(); // '"'
                std::string hexBytes;
                while (!isAtEnd() && peek() != '"') {
                    const char c1 = peek();
                    if (c1 == ' ' || c1 == '\t' || c1 == '_') {
                        advance(); // allow whitespace/underscore separators
                        continue;
                    }
                    if (!isHexDigit(c1)) {
                        lexError("Expected hex digit in bytes literal", line, column);
                    }
                    advance();
                    const char c2 = peek();
                    if (isAtEnd() || c2 == '"') {
                        lexError("Bytes literal requires pairs of hex digits (odd digit count)", line, column);
                    }
                    if (!isHexDigit(c2)) {
                        lexError("Expected second hex digit in bytes literal pair", line, column);
                    }
                    advance();
                    hexBytes += c1;
                    hexBytes += c2;
                }
                if (isAtEnd()) {
                    lexError("Unterminated bytes literal", line, column);
                }
                advance(); // closing '"'
                return makeToken(TokenType::BYTES_LITERAL, hexBytes);
            }
            // Hex literal: 0x...
            num += advance(); // '0'
            num += advance(); // 'x'/'X'
            if (!isHexDigit(peek())) {
                lexError("Expected hex digit after '0x'", line, column);
            }
            while (!isAtEnd() && (isHexDigit(peek()) || peek() == '_')) {
                const char c = advance();
                if (c == '_') {
                    if (!isHexDigit(peek())) {
                        lexError("Invalid underscore placement in numeric literal", line, column);
                    }
                    continue;
                }
                num += c;
            }
            Token token = makeToken(TokenType::INTEGER, num);
            try {
                token.intValue = static_cast<int64_t>(std::stoull(num, nullptr, 16));
            } catch (const std::out_of_range&) {
                lexError("Integer literal out of range: " + num, token.line, token.column);
            }
            return token;
        } else if (prefix == 'o' || prefix == 'O') {
            // Octal literal: 0o...
            num += advance(); // '0'
            num += advance(); // 'o'/'O'
            if (isAtEnd() || peek() < '0' || peek() > '7') {
                lexError("Expected octal digit after '0o'", line, column);
            }
            while (!isAtEnd() && ((peek() >= '0' && peek() <= '7') || peek() == '_')) {
                const char c = advance();
                if (c == '_') {
                    if (peek() < '0' || peek() > '7') {
                        lexError("Invalid underscore placement in numeric literal", line, column);
                    }
                    continue;
                }
                num += c;
            }
            Token token = makeToken(TokenType::INTEGER, num);
            try {
                token.intValue = static_cast<int64_t>(std::stoull(num.substr(2), nullptr, 8));
            } catch (const std::out_of_range&) {
                lexError("Integer literal out of range: " + num, token.line, token.column);
            }
            return token;
        } else if (prefix == 'b' || prefix == 'B') {
            // Binary literal: 0b...
            num += advance(); // '0'
            num += advance(); // 'b'/'B'
            if (isAtEnd() || (peek() != '0' && peek() != '1')) {
                lexError("Expected binary digit after '0b'", line, column);
            }
            while (!isAtEnd() && (peek() == '0' || peek() == '1' || peek() == '_')) {
                const char c = advance();
                if (c == '_') {
                    if (peek() != '0' && peek() != '1') {
                        lexError("Invalid underscore placement in numeric literal", line, column);
                    }
                    continue;
                }
                num += c;
            }
            Token token = makeToken(TokenType::INTEGER, num);
            try {
                token.intValue = static_cast<int64_t>(std::stoull(num.substr(2), nullptr, 2));
            } catch (const std::out_of_range&) {
                lexError("Integer literal out of range: " + num, token.line, token.column);
            }
            return token;
        }
    }

    // Fast path for common decimal numbers: record start position and
    // try to scan all digits/dots without underscores in one pass.
    const size_t numStart = pos;
    bool hasUnderscore = false;

    while (!isAtEnd() && (isDigit(peek()) || peek() == '.' || peek() == '_')) {
        if (peek() == '_') {
            hasUnderscore = true;
            advance(); // consume underscore but don't add to num
            continue;
        }
        if (peek() == '.') {
            // Don't consume the dot if it's part of a range operator (... or ..)
            if (peek(1) == '.' && peek(2) == '.') {
                break;
            }
            if (peek(1) == '.') {
                break; // Don't consume dot if it's part of '..' range operator
            }
            if (isFloat)
                break; // Second dot, stop
            isFloat = true;
        }
        advance();
    }

    for (size_t i = numStart; i < pos; i++) {
        if (source[i] != '_') {
            continue;
        }
        if (i == numStart || i + 1 >= pos ||
            !isDigit(source[i - 1]) || !isDigit(source[i + 1])) {
            lexError("Invalid underscore placement in numeric literal", line, column);
        }
    }

    if (!hasUnderscore) {
        num = source.substr(numStart, pos - numStart);
    } else {
        // Slow path: rebuild string skipping underscores
        for (size_t i = numStart; i < pos; i++) {
            if (source[i] != '_')
                num += source[i];
        }
    }

    // Scientific notation: e.g. 1e5, 1.5e-3, 2E10, 3e+2
    // Consume 'e' or 'E' followed by optional '+'/'-' and digits.
    if (!isAtEnd() && (peek() == 'e' || peek() == 'E')) {
        // Only treat as exponent if followed by a digit or +/- then digit.
        const char next1 = peek(1);
        const bool hasExp = isDigit(next1) ||
                            ((next1 == '+' || next1 == '-') && isDigit(peek(2)));
        if (hasExp) {
            isFloat = true;
            num += advance(); // consume 'e'/'E'
            if (peek() == '+' || peek() == '-') {
                num += advance(); // consume sign
            }
            while (!isAtEnd() && isDigit(peek())) {
                num += advance();
            }
        }
    }

    Token token = makeToken(isFloat ? TokenType::FLOAT : TokenType::INTEGER, num);
    if (isFloat) {
        try {
            token.floatValue = std::stod(num);
        } catch (const std::out_of_range&) {
            lexError("Float literal out of range: " + num, token.line, token.column);
        }
    } else {
        try {
            token.intValue = std::stoll(num);
        } catch (const std::out_of_range&) {
            lexError("Integer literal out of range: " + num, token.line, token.column);
        }
    }
    return token;
}

Token Lexer::scanIdentifier() {
    const size_t start = pos;

    while (!isAtEnd() && (isAlnum(peek()) || peek() == '_')) {
        advance();
    }

    // Look up the identifier in the keyword map using a string_view
    // to avoid allocating a std::string for every keyword check.
    const std::string_view idView(source.data() + start, pos - start);
    auto it = keywords.find(idView);
    if (it != keywords.end()) {
        return makeToken(it->second, std::string(idView));
    }

    return makeToken(TokenType::IDENTIFIER, std::string(idView));
}

Token Lexer::scanString() {
    std::string str;
    const int startLine = line;
    const int startColumn = column;
    advance(); // Skip opening quote

    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') {
            advance();
            if (isAtEnd()) {
                lexError("Unterminated escape sequence in string", startLine, startColumn);
            }
            const char escaped = advance();
            switch (escaped) {
            case 'n':
                str += '\n';
                break;
            case 't':
                str += '\t';
                break;
            case 'r':
                str += '\r';
                break;
            case '0':
                // Reject embedded null bytes — they would silently truncate
                // C-strings at runtime, causing data loss or subtle bugs.
                // This matches the \x00 rejection below for consistency.
                lexError("Null byte '\\0' is not allowed in string literals", line, column);
                break;
            case 'b':
                str += '\b';
                break;
            case 'f':
                str += '\f';
                break;
            case 'v':
                str += '\v';
                break;
            case '\\':
                str += '\\';
                break;
            case '"':
                str += '"';
                break;
            case 'x': {
                // Hex escape: \xHH (exactly two hex digits)
                if (isAtEnd() || !isHexDigit(peek())) {
                    lexError("Expected hex digit after '\\x' in string", line, column);
                }
                const char h1 = advance();
                if (isAtEnd() || !isHexDigit(peek())) {
                    lexError("Expected two hex digits after '\\x' in string", line, column);
                }
                const char h2 = advance();
                const std::string hex{h1, h2};
                const int val = std::stoi(hex, nullptr, 16);
                // Reject embedded null bytes — they would silently truncate
                // C-strings at runtime, causing data loss or subtle bugs.
                if (val == 0) {
                    lexError("Null byte '\\x00' is not allowed in string literals", line, column);
                }
                str += static_cast<char>(val);
                break;
            }
            default:
                lexError("Unknown escape sequence '\\" + std::string(1, escaped) + "' in string", line, column);
            }
        } else {
            str += advance();
        }
    }

    if (isAtEnd()) {
        lexError("Unterminated string literal", startLine, startColumn);
    }

    advance(); // Skip closing quote

    Token token(TokenType::STRING, str, startLine, startColumn);
    return token;
}

Token Lexer::scanMultiLineString() {
    std::string str;
    const int startLine = line;
    const int startColumn = column;
    // Skip opening """
    advance(); // first "
    advance(); // second "
    advance(); // third "

    while (!isAtEnd()) {
        if (peek() == '"' && peek(1) == '"' && peek(2) == '"') {
            advance(); // first "
            advance(); // second "
            advance(); // third "
            Token token(TokenType::STRING, str, startLine, startColumn);
            return token;
        }
        str += advance();
    }

    lexError("Unterminated multi-line string literal", startLine, startColumn);
}

[[gnu::hot]] std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    // Heuristic pre-allocation: most source characters produce roughly
    // one token per ~4 characters.  This avoids repeated reallocations
    // for typical inputs.
    tokens.reserve(source.length() / 4 + 16);

    while (!isAtEnd()) {
        skipWhitespace();
        if (isAtEnd())
            break;

        const char c = peek();

        if (__builtin_expect(c == 'O' && pos + 8 <= source.length(), 0)) {
            if (source.compare(pos, 8, "OPTMAX=:") == 0) {
                for (int i = 0; i < 8; i++) {
                    advance();
                }
                tokens.push_back(makeToken(TokenType::OPTMAX_START, "OPTMAX=:"));
                continue;
            }
            if (source.compare(pos, 8, "OPTMAX!:") == 0) {
                for (int i = 0; i < 8; i++) {
                    advance();
                }
                tokens.push_back(makeToken(TokenType::OPTMAX_END, "OPTMAX!:"));
                continue;
            }
        }

        // Numbers
        if (__builtin_expect(isDigit(c), 0)) {
            tokens.push_back(scanNumber());
            continue;
        }

        // Identifiers and keywords
        if (__builtin_expect(isAlpha(c) || c == '_', 1)) {
            tokens.push_back(scanIdentifier());
            continue;
        }

        // String interpolation: $"..." desugars into concatenation chain
        if (c == '$' && peek(1) == '"') {
            scanInterpolatedString(tokens);
            continue;
        }

        // String literals (check triple-quote first for multi-line strings)
        if (c == '"') {
            if (peek(1) == '"' && peek(2) == '"') {
                tokens.push_back(scanMultiLineString());
            } else {
                tokens.push_back(scanString());
            }
            continue;
        }

        // Check for range operator ... or ..
        if (c == '.') {
            // peek() gives current position, peek(1) gives next
            if (peek(1) == '.' && peek(2) == '.') {
                advance(); // consume first .
                advance(); // consume second .
                advance(); // consume third .
                tokens.push_back(makeToken(TokenType::RANGE, "..."));
                continue;
            }
            if (peek(1) == '.') {
                advance(); // consume first .
                advance(); // consume second .
                tokens.push_back(makeToken(TokenType::DOT_DOT, ".."));
                continue;
            }
        }

        // Single character tokens
        const int tokenLine = line;
        const int tokenColumn = column;
        advance();
        switch (c) {
        case '+':
            if (peek() == '+') {
                advance();
                tokens.push_back(makeToken(TokenType::PLUSPLUS, "++"));
            } else if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::PLUS_ASSIGN, "+="));
            } else {
                tokens.push_back(makeToken(TokenType::PLUS, "+"));
            }
            break;
        case '-':
            if (peek() == '-') {
                advance();
                tokens.push_back(makeToken(TokenType::MINUSMINUS, "--"));
            } else if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::MINUS_ASSIGN, "-="));
            } else if (peek() == '>') {
                advance();
                tokens.push_back(makeToken(TokenType::ARROW, "->"));
            } else {
                tokens.push_back(makeToken(TokenType::MINUS, "-"));
            }
            break;
        case '*':
            if (peek() == '*') {
                advance();
                if (peek() == '=') {
                    advance();
                    tokens.push_back(makeToken(TokenType::STAR_STAR_ASSIGN, "**="));
                } else {
                    tokens.push_back(makeToken(TokenType::STAR_STAR, "**"));
                }
            } else if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::STAR_ASSIGN, "*="));
            } else {
                tokens.push_back(makeToken(TokenType::STAR, "*"));
            }
            break;
        case '/':
            if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::SLASH_ASSIGN, "/="));
            } else {
                tokens.push_back(makeToken(TokenType::SLASH, "/"));
            }
            break;
        case '%':
            if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::PERCENT_ASSIGN, "%="));
            } else {
                tokens.push_back(makeToken(TokenType::PERCENT, "%"));
            }
            break;
        case '(':
            tokens.push_back(makeToken(TokenType::LPAREN, "("));
            break;
        case ')':
            tokens.push_back(makeToken(TokenType::RPAREN, ")"));
            break;
        case '{':
            tokens.push_back(makeToken(TokenType::LBRACE, "{"));
            break;
        case '}':
            tokens.push_back(makeToken(TokenType::RBRACE, "}"));
            break;
        case '[':
            tokens.push_back(makeToken(TokenType::LBRACKET, "["));
            break;
        case ']':
            tokens.push_back(makeToken(TokenType::RBRACKET, "]"));
            break;
        case ';':
            tokens.push_back(makeToken(TokenType::SEMICOLON, ";"));
            break;
        case ',':
            tokens.push_back(makeToken(TokenType::COMMA, ","));
            break;
        case ':':
            if (peek() == ':') {
                advance();
                tokens.push_back(makeToken(TokenType::SCOPE, "::"));
            } else {
                tokens.push_back(makeToken(TokenType::COLON, ":"));
            }
            break;
        case '?':
            if (peek() == '?') {
                advance();
                if (peek() == '=') {
                    advance();
                    tokens.push_back(makeToken(TokenType::NULL_COALESCE_ASSIGN, "?\?="));
                } else {
                    tokens.push_back(makeToken(TokenType::NULL_COALESCE, "??"));
                }
            } else {
                tokens.push_back(makeToken(TokenType::QUESTION, "?"));
            }
            break;
        case '.':
            tokens.push_back(makeToken(TokenType::DOT, "."));
            break;

        case '=':
            if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::EQ, "=="));
            } else if (peek() == '>') {
                advance();
                tokens.push_back(makeToken(TokenType::FAT_ARROW, "=>"));
            } else {
                tokens.push_back(makeToken(TokenType::ASSIGN, "="));
            }
            break;

        case '!':
            if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::NE, "!="));
            } else {
                tokens.push_back(makeToken(TokenType::NOT, "!"));
            }
            break;

        case '<':
            if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::LE, "<="));
            } else if (peek() == '<') {
                advance();
                if (peek() == '=') {
                    advance();
                    tokens.push_back(makeToken(TokenType::LSHIFT_ASSIGN, "<<="));
                } else {
                    tokens.push_back(makeToken(TokenType::LSHIFT, "<<"));
                }
            } else {
                tokens.push_back(makeToken(TokenType::LT, "<"));
            }
            break;

        case '>':
            if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::GE, ">="));
            } else if (peek() == '>') {
                advance();
                if (peek() == '=') {
                    advance();
                    tokens.push_back(makeToken(TokenType::RSHIFT_ASSIGN, ">>="));
                } else {
                    tokens.push_back(makeToken(TokenType::RSHIFT, ">>"));
                }
            } else {
                tokens.push_back(makeToken(TokenType::GT, ">"));
            }
            break;

        case '&':
            if (peek() == '&') {
                advance();
                if (peek() == '=') {
                    advance();
                    tokens.push_back(makeToken(TokenType::AND_ASSIGN, "&&="));
                } else {
                    tokens.push_back(makeToken(TokenType::AND, "&&"));
                }
            } else if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::AMPERSAND_ASSIGN, "&="));
            } else {
                tokens.push_back(makeToken(TokenType::AMPERSAND, "&"));
            }
            break;

        case '|':
            if (peek() == '|') {
                advance();
                if (peek() == '=') {
                    advance();
                    tokens.push_back(makeToken(TokenType::OR_ASSIGN, "||="));
                } else {
                    tokens.push_back(makeToken(TokenType::OR, "||"));
                }
            } else if (peek() == '>') {
                advance();
                tokens.push_back(makeToken(TokenType::PIPE_FORWARD, "|>"));
            } else if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::PIPE_ASSIGN, "|="));
            } else {
                tokens.push_back(makeToken(TokenType::PIPE, "|"));
            }
            break;

        case '^':
            if (peek() == '=') {
                advance();
                tokens.push_back(makeToken(TokenType::CARET_ASSIGN, "^="));
            } else {
                tokens.push_back(makeToken(TokenType::CARET, "^"));
            }
            break;

        case '~':
            tokens.push_back(makeToken(TokenType::TILDE, "~"));
            break;

        case '@':
            tokens.push_back(makeToken(TokenType::AT, "@"));
            break;

        case '`': {
            // Backtick-quoted infix operator name: `op`
            // Scans all characters up to the closing backtick.
            const int btLine = tokenLine;
            const int btCol  = tokenColumn;
            std::string btIdent;
            while (!isAtEnd() && peek() != '`') {
                btIdent += advance();
            }
            if (isAtEnd()) {
                lexError("Unterminated backtick operator name (missing closing '`')", btLine, btCol);
            }
            advance(); // consume closing '`'
            if (btIdent.empty()) {
                lexError("Empty backtick operator name", btLine, btCol);
            }
            tokens.push_back(Token(TokenType::BACKTICK_IDENT, btIdent, btLine, btCol));
            break;
        }

        default:
            lexError("Unexpected character '" + std::string(1, c) + "'", tokenLine, tokenColumn);
            break;
        }
    }

    tokens.push_back(makeToken(TokenType::END_OF_FILE, ""));
    return tokens;
}

// ---------------------------------------------------------------------------
// String interpolation: $"hello {expr} world"
// Desugars into a chain of '+' operations:
//   "" + (expr1) + "literal" + (expr2) + ...
// The leading empty string ensures we are always in string-concatenation
// context, so integer/float expressions are auto-converted to strings.
// ---------------------------------------------------------------------------
void Lexer::scanInterpolatedString(std::vector<Token>& tokens) {
    const int startLine = line;
    const int startColumn = column;
    advance(); // skip '$'
    advance(); // skip opening '"'

    // Collect alternating literal/expression segments.
    struct Segment {
        bool isLiteral;
        std::string text;
    };
    std::vector<Segment> segments;
    std::string currentLiteral;

    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') {
            // Escape sequences — same rules as normal strings, plus \{ and \}.
            advance(); // skip backslash
            if (isAtEnd()) {
                lexError("Unterminated escape sequence in interpolated string", startLine, startColumn);
            }
            const char escaped = advance();
            switch (escaped) {
            case 'n':
                currentLiteral += '\n';
                break;
            case 't':
                currentLiteral += '\t';
                break;
            case 'r':
                currentLiteral += '\r';
                break;
            case '0':
                lexError("Null byte '\\0' is not allowed in string literals", line, column);
                break;
            case 'b':
                currentLiteral += '\b';
                break;
            case 'f':
                currentLiteral += '\f';
                break;
            case 'v':
                currentLiteral += '\v';
                break;
            case '\\':
                currentLiteral += '\\';
                break;
            case '"':
                currentLiteral += '"';
                break;
            case '{':
                currentLiteral += '{';
                break;
            case '}':
                currentLiteral += '}';
                break;
            case 'x': {
                if (isAtEnd() || !isHexDigit(peek())) {
                    lexError("Expected hex digit after '\\x' in string", line, column);
                }
                const char h1 = advance();
                if (isAtEnd() || !isHexDigit(peek())) {
                    lexError("Expected two hex digits after '\\x' in string", line, column);
                }
                const char h2 = advance();
                const std::string hex{h1, h2};
                const int val = std::stoi(hex, nullptr, 16);
                if (val == 0) {
                    lexError("Null byte '\\x00' is not allowed in string literals", line, column);
                }
                currentLiteral += static_cast<char>(val);
                break;
            }
            default:
                lexError("Unknown escape sequence '\\" + std::string(1, escaped) + "' in interpolated string",
                         line, column);
            }
        } else if (peek() == '{') {
            advance(); // skip '{'
            // Save current literal as a segment.
            segments.push_back({true, currentLiteral});
            currentLiteral.clear();

            // Scan expression text until matching '}', tracking brace depth
            // and skipping over nested string literals so that braces inside
            // strings are not mis-counted.
            std::string exprText;
            int depth = 1;
            while (!isAtEnd() && depth > 0) {
                const char ch = peek();
                if (ch == '{') {
                    depth++;
                    exprText += advance();
                } else if (ch == '}') {
                    depth--;
                    if (depth == 0)
                        break;
                    exprText += advance();
                } else if (ch == '"') {
                    // Nested string literal inside the expression — scan it
                    // verbatim so that any '{' or '}' inside is not counted.
                    exprText += advance(); // opening quote
                    while (!isAtEnd() && peek() != '"') {
                        if (peek() == '\\') {
                            exprText += advance(); // backslash
                        }
                        if (!isAtEnd())
                            exprText += advance();
                    }
                    if (!isAtEnd())
                        exprText += advance(); // closing quote
                } else {
                    exprText += advance();
                }
            }
            if (isAtEnd()) {
                lexError("Unterminated expression in interpolated string: missing closing '}'", startLine, startColumn);
            }
            advance(); // skip closing '}'
            segments.push_back({false, exprText});
        } else {
            currentLiteral += advance();
        }
    }

    if (isAtEnd()) {
        lexError("Unterminated interpolated string literal", startLine, startColumn);
    }
    advance(); // skip closing '"'

    // Don't forget the trailing literal.
    if (!currentLiteral.empty()) {
        segments.push_back({true, currentLiteral});
    }

    // Empty interpolated string: $""
    if (segments.empty()) {
        tokens.push_back(Token(TokenType::STRING, "", startLine, startColumn));
        return;
    }

    // Ensure first segment is a literal so we start in string context.
    // This guarantees that subsequent '+' operations use string concatenation
    // (which auto-converts integers/floats to strings).
    if (!segments[0].isLiteral) {
        segments.insert(segments.begin(), Segment{true, ""});
    }

    // Emit tokens: seg0 + seg1 + seg2 + ...
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            tokens.push_back(Token(TokenType::PLUS, "+", startLine, startColumn));
        }
        if (segments[i].isLiteral) {
            tokens.push_back(Token(TokenType::STRING, segments[i].text, startLine, startColumn));
        } else {
            // Wrap expression in parentheses to preserve evaluation order.
            tokens.push_back(Token(TokenType::LPAREN, "(", startLine, startColumn));
            // Re-lex the expression text.
            Lexer exprLexer(segments[i].text);
            const auto exprTokens = exprLexer.tokenize();
            for (const auto& tok : exprTokens) {
                if (tok.type == TokenType::END_OF_FILE)
                    break;
                tokens.push_back(tok);
            }
            tokens.push_back(Token(TokenType::RPAREN, ")", startLine, startColumn));
        }
    }
}

} // namespace omscript
