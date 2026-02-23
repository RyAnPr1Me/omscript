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
    DO,
    FOR,
    VAR,
    CONST,
    BREAK,
    CONTINUE,
    IN,
    TRUE,
    FALSE,
    NULL_LITERAL,
    OPTMAX_START,
    OPTMAX_END,
    SWITCH,
    CASE,
    DEFAULT,

    // Operators
    PLUS,
    MINUS,
    STAR,
    STAR_STAR,
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
    PLUSPLUS,
    MINUSMINUS,
    PLUS_ASSIGN,
    MINUS_ASSIGN,
    STAR_ASSIGN,
    SLASH_ASSIGN,
    PERCENT_ASSIGN,
    AMPERSAND_ASSIGN,
    PIPE_ASSIGN,
    CARET_ASSIGN,
    LSHIFT_ASSIGN,
    RSHIFT_ASSIGN,
    QUESTION,
    AMPERSAND,
    PIPE,
    CARET,
    TILDE,
    LSHIFT,
    RSHIFT,
    RANGE,
    ARROW,

    // Delimiters
    LPAREN,
    RPAREN,
    LBRACE,
    RBRACE,
    LBRACKET,
    RBRACKET,
    SEMICOLON,
    COMMA,
    COLON,
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

    char peek(int offset = 0) const;
    char advance();
    bool isAtEnd() const;
    void skipWhitespace();
    void skipComment();
    void skipBlockComment();

    Token makeToken(TokenType type, const std::string& lexeme);
    Token scanNumber();
    Token scanIdentifier();
    Token scanString();
};

} // namespace omscript

#endif // LEXER_H
