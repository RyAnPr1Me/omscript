#include "lexer.h"
#include <cctype>
#include <unordered_map>

namespace omscript {

static std::unordered_map<std::string, TokenType> keywords = {
    {"fn", TokenType::FN},
    {"return", TokenType::RETURN},
    {"if", TokenType::IF},
    {"else", TokenType::ELSE},
    {"while", TokenType::WHILE},
    {"for", TokenType::FOR},
    {"var", TokenType::VAR},
    {"const", TokenType::CONST},
    {"break", TokenType::BREAK},
    {"continue", TokenType::CONTINUE},
    {"in", TokenType::IN}
};

Lexer::Lexer(const std::string& source)
    : source(source), pos(0), line(1), column(1) {}

char Lexer::peek(int offset) {
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

bool Lexer::isAtEnd() {
    return pos >= source.length();
}

void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            skipComment();
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
        token.floatValue = std::stod(num);
    } else {
        token.intValue = std::stoll(num);
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
    advance(); // Skip opening quote
    
    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\\') {
            advance();
            char escaped = advance();
            switch (escaped) {
                case 'n': str += '\n'; break;
                case 't': str += '\t'; break;
                case 'r': str += '\r'; break;
                case '\\': str += '\\'; break;
                case '"': str += '"'; break;
                default: str += escaped; break;
            }
        } else {
            str += advance();
        }
    }
    
    if (!isAtEnd()) {
        advance(); // Skip closing quote
    }
    
    return makeToken(TokenType::STRING, str);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    
    while (!isAtEnd()) {
        skipWhitespace();
        if (isAtEnd()) break;
        
        char c = peek();
        
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
        advance();
        switch (c) {
            case '+':
                if (peek() == '+') {
                    advance();
                    tokens.push_back(makeToken(TokenType::PLUSPLUS, "++"));
                } else {
                    tokens.push_back(makeToken(TokenType::PLUS, "+"));
                }
                break;
            case '-':
                if (peek() == '-') {
                    advance();
                    tokens.push_back(makeToken(TokenType::MINUSMINUS, "--"));
                } else {
                    tokens.push_back(makeToken(TokenType::MINUS, "-"));
                }
                break;
            case '*': tokens.push_back(makeToken(TokenType::STAR, "*")); break;
            case '/': tokens.push_back(makeToken(TokenType::SLASH, "/")); break;
            case '%': tokens.push_back(makeToken(TokenType::PERCENT, "%")); break;
            case '(': tokens.push_back(makeToken(TokenType::LPAREN, "(")); break;
            case ')': tokens.push_back(makeToken(TokenType::RPAREN, ")")); break;
            case '{': tokens.push_back(makeToken(TokenType::LBRACE, "{")); break;
            case '}': tokens.push_back(makeToken(TokenType::RBRACE, "}")); break;
            case '[': tokens.push_back(makeToken(TokenType::LBRACKET, "[")); break;
            case ']': tokens.push_back(makeToken(TokenType::RBRACKET, "]")); break;
            case ';': tokens.push_back(makeToken(TokenType::SEMICOLON, ";")); break;
            case ',': tokens.push_back(makeToken(TokenType::COMMA, ",")); break;
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
                } else {
                    tokens.push_back(makeToken(TokenType::LT, "<"));
                }
                break;
            
            case '>':
                if (peek() == '=') {
                    advance();
                    tokens.push_back(makeToken(TokenType::GE, ">="));
                } else {
                    tokens.push_back(makeToken(TokenType::GT, ">"));
                }
                break;
            
            case '&':
                if (peek() == '&') {
                    advance();
                    tokens.push_back(makeToken(TokenType::AND, "&&"));
                } else {
                    tokens.push_back(makeToken(TokenType::INVALID, "&"));
                }
                break;
            
            case '|':
                if (peek() == '|') {
                    advance();
                    tokens.push_back(makeToken(TokenType::OR, "||"));
                } else {
                    tokens.push_back(makeToken(TokenType::INVALID, "|"));
                }
                break;
            
            default:
                tokens.push_back(makeToken(TokenType::INVALID, std::string(1, c)));
                break;
        }
    }
    
    tokens.push_back(makeToken(TokenType::END_OF_FILE, ""));
    return tokens;
}

} // namespace omscript
