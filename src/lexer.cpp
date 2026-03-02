#include "lexer.h"
#include "diagnostic.h"
#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace omscript {

namespace {
inline bool isDigit(char c) {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}
inline bool isHexDigit(char c) {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}
inline bool isAlpha(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}
inline bool isAlnum(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
}
} // namespace

static const std::unordered_map<std::string, TokenType> keywords = {
    {"fn", TokenType::FN},         {"return", TokenType::RETURN},     {"if", TokenType::IF},
    {"else", TokenType::ELSE},     {"while", TokenType::WHILE},       {"do", TokenType::DO},
    {"for", TokenType::FOR},       {"var", TokenType::VAR},           {"const", TokenType::CONST},
    {"break", TokenType::BREAK},   {"continue", TokenType::CONTINUE}, {"in", TokenType::IN},
    {"true", TokenType::TRUE},     {"false", TokenType::FALSE},       {"null", TokenType::NULL_LITERAL},
    {"switch", TokenType::SWITCH}, {"case", TokenType::CASE},         {"default", TokenType::DEFAULT},
    {"try", TokenType::TRY},       {"catch", TokenType::CATCH},       {"throw", TokenType::THROW},
    {"enum", TokenType::ENUM}};

/// Throw a DiagnosticError with the given message and source location.
[[noreturn]] static void lexError(const std::string& msg, int ln, int col) {
    throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {ln, col}, msg});
}

Lexer::Lexer(const std::string& source) : source(source), pos(0), line(1), column(1) {}

char Lexer::peek(int offset) const {
    size_t index = pos + offset;
    if (index >= source.length()) {
        return '\0';
    }
    return source[index];
}

char Lexer::advance() {
    if (isAtEnd())
        return '\0';
    char c = source[pos++];
    if (c == '\n') {
        line++;
        column = 1;
    } else {
        column++;
    }
    return c;
}

bool Lexer::isAtEnd() const {
    return pos >= source.length();
}

void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
        char c = peek();
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
    int startLine = line;
    int startColumn = column;
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
        char prefix = peek(1);
        if (prefix == 'x' || prefix == 'X') {
            // Hex literal: 0x...
            num += advance(); // '0'
            num += advance(); // 'x'/'X'
            if (!isHexDigit(peek())) {
                lexError("Expected hex digit after '0x'", line, column);
            }
            while (!isAtEnd() && (isHexDigit(peek()) || peek() == '_')) {
                char c = advance();
                if (c != '_')
                    num += c;
            }
            Token token = makeToken(TokenType::INTEGER, num);
            try {
                token.intValue = std::stoll(num, nullptr, 16);
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
                char c = advance();
                if (c != '_')
                    num += c;
            }
            Token token = makeToken(TokenType::INTEGER, num);
            try {
                token.intValue = std::stoll(num.substr(2), nullptr, 8);
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
                char c = advance();
                if (c != '_')
                    num += c;
            }
            Token token = makeToken(TokenType::INTEGER, num);
            try {
                token.intValue = std::stoll(num.substr(2), nullptr, 2);
            } catch (const std::out_of_range&) {
                lexError("Integer literal out of range: " + num, token.line, token.column);
            }
            return token;
        }
    }

    while (!isAtEnd() && (isDigit(peek()) || peek() == '.' || peek() == '_')) {
        if (peek() == '_') {
            advance(); // consume underscore but don't add to num
            continue;
        }
        if (peek() == '.') {
            // Don't consume the dot if it's part of a range operator (...)
            if (peek(1) == '.' && peek(2) == '.') {
                break;
            }
            if (isFloat)
                break; // Second dot, stop
            isFloat = true;
        }
        num += advance();
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
    std::string id;

    while (!isAtEnd() && (isAlnum(peek()) || peek() == '_')) {
        id += advance();
    }

    auto it = keywords.find(id);
    if (it != keywords.end()) {
        return makeToken(it->second, id);
    }

    return makeToken(TokenType::IDENTIFIER, id);
}

Token Lexer::scanString() {
    std::string str;
    int startLine = line;
    int startColumn = column;
    advance(); // Skip opening quote

    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') {
            advance();
            if (isAtEnd()) {
                lexError("Unterminated escape sequence in string", startLine, startColumn);
            }
            char escaped = advance();
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
                str += '\0';
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
                char h1 = advance();
                if (isAtEnd() || !isHexDigit(peek())) {
                    lexError("Expected two hex digits after '\\x' in string", line, column);
                }
                char h2 = advance();
                std::string hex{h1, h2};
                str += static_cast<char>(std::stoi(hex, nullptr, 16));
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
    int startLine = line;
    int startColumn = column;
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

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    // Heuristic pre-allocation: most source characters produce roughly
    // one token per ~4 characters.  This avoids repeated reallocations
    // for typical inputs.
    tokens.reserve(source.length() / 4 + 16);

    while (!isAtEnd()) {
        skipWhitespace();
        if (isAtEnd())
            break;

        char c = peek();

        if (c == 'O' && pos + 8 <= source.length()) {
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
        if (isDigit(c)) {
            tokens.push_back(scanNumber());
            continue;
        }

        // Identifiers and keywords
        if (isAlpha(c) || c == '_') {
            tokens.push_back(scanIdentifier());
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

        // Check for range operator ...
        if (c == '.') {
            // peek() gives current position, peek(1) gives next
            if (peek(1) == '.' && peek(2) == '.') {
                advance(); // consume first .
                advance(); // consume second .
                advance(); // consume third .
                tokens.push_back(makeToken(TokenType::RANGE, "..."));
                continue;
            }
        }

        // Single character tokens
        int tokenLine = line;
        int tokenColumn = column;
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
                tokens.push_back(makeToken(TokenType::STAR_STAR, "**"));
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
            tokens.push_back(makeToken(TokenType::COLON, ":"));
            break;
        case '?':
            if (peek() == '?') {
                advance();
                tokens.push_back(makeToken(TokenType::NULL_COALESCE, "??"));
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
                tokens.push_back(makeToken(TokenType::AND, "&&"));
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
                tokens.push_back(makeToken(TokenType::OR, "||"));
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

        default:
            lexError("Unexpected character '" + std::string(1, c) + "'", tokenLine, tokenColumn);
            break;
        }
    }

    tokens.push_back(makeToken(TokenType::END_OF_FILE, ""));
    return tokens;
}

} // namespace omscript
