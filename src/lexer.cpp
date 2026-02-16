#include "lexer.h"
#include <cctype>
#include <stdexcept>
#include <unordered_map>

namespace omscript {

static const std::unordered_map<std::string, TokenType> keywords = {
    {"fn", TokenType::FN},
    {"return", TokenType::RETURN},
    {"if", TokenType::IF},
    {"else", TokenType::ELSE},
    {"while", TokenType::WHILE},
    {"do", TokenType::DO},
    {"for", TokenType::FOR},
    {"var", TokenType::VAR},
    {"const", TokenType::CONST},
    {"break", TokenType::BREAK},
    {"continue", TokenType::CONTINUE},
    {"in", TokenType::IN},
    {"switch", TokenType::SWITCH},
    {"case", TokenType::CASE},
    {"default", TokenType::DEFAULT}
};

Lexer::Lexer(const std::string& source)
    : source(source), pos(0), line(1), column(1) {}

char Lexer::peek(int offset) const {
    size_t index = pos + offset;
    if (index >= source.length()) {
        return '\0';
    }
    return source[index];
}

char Lexer::advance() {
    if (isAtEnd()) return '\0';
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
    throw std::runtime_error("Unterminated block comment starting at line " +
                             std::to_string(startLine) + ", column " +
                             std::to_string(startColumn));
}

Token Lexer::makeToken(TokenType type, const std::string& lexeme) {
    return Token(type, lexeme, line, column - lexeme.length());
}

Token Lexer::scanNumber() {
    std::string num;
    bool isFloat = false;
    
    while (!isAtEnd() && (isdigit(peek()) || peek() == '.')) {
        if (peek() == '.') {
            // Don't consume the dot if it's part of a range operator (...)
            if (peek(1) == '.' && peek(2) == '.') {
                break;
            }
            if (isFloat) break; // Second dot, stop
            isFloat = true;
        }
        num += advance();
    }
    
    Token token = makeToken(isFloat ? TokenType::FLOAT : TokenType::INTEGER, num);
    if (isFloat) {
        try {
            token.floatValue = std::stod(num);
        } catch (const std::out_of_range&) {
            throw std::runtime_error("Float literal out of range at line " +
                                     std::to_string(token.line) + ", column " +
                                     std::to_string(token.column) + ": " + num);
        }
    } else {
        try {
            token.intValue = std::stoll(num);
        } catch (const std::out_of_range&) {
            throw std::runtime_error("Integer literal out of range at line " +
                                     std::to_string(token.line) + ", column " +
                                     std::to_string(token.column) + ": " + num);
        }
    }
    return token;
}

Token Lexer::scanIdentifier() {
    std::string id;
    
    while (!isAtEnd() && (isalnum(peek()) || peek() == '_')) {
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
                throw std::runtime_error("Unterminated escape sequence in string at line " +
                                         std::to_string(startLine) + ", column " +
                                         std::to_string(startColumn));
            }
            char escaped = advance();
            switch (escaped) {
                case 'n': str += '\n'; break;
                case 't': str += '\t'; break;
                case 'r': str += '\r'; break;
                case '0': str += '\0'; break;
                case 'b': str += '\b'; break;
                case 'f': str += '\f'; break;
                case 'v': str += '\v'; break;
                case '\\': str += '\\'; break;
                case '"': str += '"'; break;
                default:
                    throw std::runtime_error(
                        "Unknown escape sequence '\\" + std::string(1, escaped) +
                        "' in string at line " + std::to_string(line) +
                        ", column " + std::to_string(column));
            }
        } else {
            str += advance();
        }
    }
    
    if (isAtEnd()) {
        throw std::runtime_error("Unterminated string literal at line " +
                                 std::to_string(startLine) + ", column " +
                                 std::to_string(startColumn));
    }
    
    advance(); // Skip closing quote
    
    Token token(TokenType::STRING, str, startLine, startColumn);
    return token;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    
    while (!isAtEnd()) {
        skipWhitespace();
        if (isAtEnd()) break;
        
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
        if (isdigit(c)) {
            tokens.push_back(scanNumber());
            continue;
        }
        
        // Identifiers and keywords
        if (isalpha(c) || c == '_') {
            tokens.push_back(scanIdentifier());
            continue;
        }
        
        // String literals
        if (c == '"') {
            tokens.push_back(scanString());
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
                } else {
                    tokens.push_back(makeToken(TokenType::MINUS, "-"));
                }
                break;
            case '*':
                if (peek() == '=') {
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
            case '(': tokens.push_back(makeToken(TokenType::LPAREN, "(")); break;
            case ')': tokens.push_back(makeToken(TokenType::RPAREN, ")")); break;
            case '{': tokens.push_back(makeToken(TokenType::LBRACE, "{")); break;
            case '}': tokens.push_back(makeToken(TokenType::RBRACE, "}")); break;
            case '[': tokens.push_back(makeToken(TokenType::LBRACKET, "[")); break;
            case ']': tokens.push_back(makeToken(TokenType::RBRACKET, "]")); break;
            case ';': tokens.push_back(makeToken(TokenType::SEMICOLON, ";")); break;
            case ',': tokens.push_back(makeToken(TokenType::COMMA, ",")); break;
            case ':': tokens.push_back(makeToken(TokenType::COLON, ":")); break;
            case '?': tokens.push_back(makeToken(TokenType::QUESTION, "?")); break;
            case '.': tokens.push_back(makeToken(TokenType::DOT, ".")); break;
            
            case '=':
                if (peek() == '=') {
                    advance();
                    tokens.push_back(makeToken(TokenType::EQ, "=="));
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
                    tokens.push_back(makeToken(TokenType::LSHIFT, "<<"));
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
                    tokens.push_back(makeToken(TokenType::RSHIFT, ">>"));
                } else {
                    tokens.push_back(makeToken(TokenType::GT, ">"));
                }
                break;
            
            case '&':
                if (peek() == '&') {
                    advance();
                    tokens.push_back(makeToken(TokenType::AND, "&&"));
                } else {
                    tokens.push_back(makeToken(TokenType::AMPERSAND, "&"));
                }
                break;
            
            case '|':
                if (peek() == '|') {
                    advance();
                    tokens.push_back(makeToken(TokenType::OR, "||"));
                } else {
                    tokens.push_back(makeToken(TokenType::PIPE, "|"));
                }
                break;
            
            case '^':
                tokens.push_back(makeToken(TokenType::CARET, "^"));
                break;
            
            case '~':
                tokens.push_back(makeToken(TokenType::TILDE, "~"));
                break;
            
            default:
                throw std::runtime_error("Unexpected character '" + std::string(1, c) +
                                         "' at line " + std::to_string(tokenLine) +
                                         ", column " + std::to_string(tokenColumn));
                break;
        }
    }
    
    tokens.push_back(makeToken(TokenType::END_OF_FILE, ""));
    return tokens;
}

} // namespace omscript
