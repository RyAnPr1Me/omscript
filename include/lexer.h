#ifndef LEXER_H
#define LEXER_H

#include <string>
#include <vector>

namespace omscript {

enum class TokenType {
    // Literals
    INTEGER,
    FLOAT,
    STRING,
    IDENTIFIER,
    
    // Keywords
    FN,
    RETURN,
    IF,
    ELSE,
    WHILE,
    FOR,
    VAR,
    CONST,
    
    // Operators
    PLUS,
    MINUS,
    STAR,
    SLASH,
    PERCENT,
    ASSIGN,
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE,
    AND,
    OR,
    NOT,
    
    // Delimiters
    LPAREN,
    RPAREN,
    LBRACE,
    RBRACE,
    LBRACKET,
    RBRACKET,
    SEMICOLON,
    COMMA,
    DOT,
    
    // Special
    END_OF_FILE,
    INVALID
};

struct Token {
    TokenType type;
    std::string lexeme;
    int line;
    int column;
    
    // For literals
    union {
        long long intValue;
        double floatValue;
    };
    
    Token(TokenType t, const std::string& lex, int ln, int col)
        : type(t), lexeme(lex), line(ln), column(col), intValue(0) {}
};

class Lexer {
public:
    Lexer(const std::string& source);
    std::vector<Token> tokenize();
    
private:
    std::string source;
    size_t pos;
    int line;
    int column;
    
    char peek(int offset = 0);
    char advance();
    bool isAtEnd();
    void skipWhitespace();
    void skipComment();
    
    Token makeToken(TokenType type, const std::string& lexeme);
    Token scanNumber();
    Token scanIdentifier();
    Token scanString();
};

} // namespace omscript

#endif // LEXER_H
