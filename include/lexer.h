#pragma once

#ifndef LEXER_H
#define LEXER_H

/// @file lexer.h
/// @brief Lexical analyser (tokeniser) for OmScript source code.
///
/// The Lexer scans a source string and produces a vector of Tokens that the
/// Parser consumes.  It recognises keywords, identifiers, numeric and string
/// literals, operators, and punctuation.

#include <string>
#include <vector>

namespace omscript {

enum class TokenType {
    // Literals
    INTEGER,
    FLOAT,
    STRING,
    BYTES_LITERAL,  // 0x"AABBCC" — hex byte array literal
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
    TRY,
    CATCH,
    THROW,
    ENUM,
    STRUCT,
    IMPORT,
    MOVE,
    INVALIDATE,
    BORROW,
    FREEZE,
    MUT,
    PREFETCH,
    LIKELY,
    UNLIKELY,
    REGISTER,
    UNLESS,
    UNTIL,
    LOOP,
    REPEAT,
    DEFER,
    GUARD,
    WHEN,
    FOREVER,
    FOREACH,
    ELIF,
    SWAP,
    TIMES,
    WITH,
    PARALLEL,  // parallel — marks a loop for auto-parallelization
    COMPTIME,  // comptime — compile-time evaluation block
    REBORROW,  // reborrow — create a new borrow reference from existing borrow
    PIPELINE,  // pipeline — staged execution pipeline construct
    STAGE,     // stage    — named stage inside a pipeline block
    GLOBAL,    // global   — declare a program-wide variable

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
    STAR_STAR_ASSIGN,
    NULL_COALESCE_ASSIGN,
    QUESTION,
    NULL_COALESCE,
    AMPERSAND,
    PIPE,
    CARET,
    TILDE,
    LSHIFT,
    RSHIFT,
    RANGE,
    DOT_DOT,
    ARROW,
    FAT_ARROW,
    PIPE_FORWARD,
    SPREAD,
    SCOPE,              // :: — scope resolution operator
    AND_ASSIGN,         // &&= — logical AND compound assignment
    OR_ASSIGN,          // ||= — logical OR compound assignment

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
    AT,  // @ — used for function annotations (@inline, @noinline, @cold)
    BACKTICK_IDENT, // `name` — backtick-quoted identifier for infix operator creation
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

    Token(TokenType t, const std::string& lex, int ln, int col) noexcept(false)
        : type(t), lexeme(lex), line(ln), column(col), intValue(0) {}
    Token(TokenType t, std::string&& lex, int ln, int col) noexcept(false)
        : type(t), lexeme(std::move(lex)), line(ln), column(col), intValue(0) {}
};

class Lexer {
  public:
    Lexer(const std::string& source);
    Lexer(std::string&& source);
    [[nodiscard]] std::vector<Token> tokenize();

  private:
    std::string source;
    size_t pos;
    int line;
    int column;

    char peek(int offset = 0) const noexcept;
    char advance() noexcept;
    bool isAtEnd() const noexcept;
    void skipWhitespace();
    void skipComment();
    void skipBlockComment();

    Token makeToken(TokenType type, const std::string& lexeme);
    Token scanNumber();
    Token scanIdentifier();
    Token scanString();
    Token scanMultiLineString();
    void scanInterpolatedString(std::vector<Token>& tokens);
};

} // namespace omscript

#endif // LEXER_H
