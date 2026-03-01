#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "lexer.h"
#include <memory>
#include <vector>

namespace omscript {

class Parser {
  public:
    Parser(const std::vector<Token>& tokens);
    Parser(std::vector<Token>&& tokens);
    std::unique_ptr<Program> parse();

    /// Returns collected parse errors (populated when multi-error mode is active).
    const std::vector<std::string>& errors() const {
        return errors_;
    }

  private:
    std::vector<Token> tokens;
    size_t current;
    bool inOptMaxFunction;
    std::vector<std::string> errors_;
    int lambdaCounter_ = 0;

    /// Generated lambda functions to be appended to the program.
    std::vector<std::unique_ptr<FunctionDecl>> lambdaFunctions_;

    Token peek(int offset = 0) const;
    Token advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    bool isAtEnd() const;
    Token consume(TokenType type, const std::string& message);

    /// Synchronize: skip tokens until we reach a statement boundary.
    void synchronize();

    // Parsing methods
    std::unique_ptr<FunctionDecl> parseFunction(bool isOptMax);
    std::unique_ptr<Statement> parseStatement();
    std::unique_ptr<BlockStmt> parseBlock();
    std::unique_ptr<Statement> parseVarDecl(bool isConst);
    std::unique_ptr<Statement> parseIfStmt();
    std::unique_ptr<Statement> parseWhileStmt();
    std::unique_ptr<Statement> parseDoWhileStmt();
    std::unique_ptr<Statement> parseForStmt();
    std::unique_ptr<Statement> parseReturnStmt();
    std::unique_ptr<Statement> parseBreakStmt();
    std::unique_ptr<Statement> parseContinueStmt();
    std::unique_ptr<Statement> parseSwitchStmt();
    std::unique_ptr<Statement> parseTryCatchStmt();
    std::unique_ptr<Statement> parseThrowStmt();
    std::unique_ptr<EnumDecl> parseEnumDecl();
    std::unique_ptr<Statement> parseExprStmt();

    std::unique_ptr<Expression> parseExpression();
    std::unique_ptr<Expression> parseAssignment();
    std::unique_ptr<Expression> parseTernary();
    std::unique_ptr<Expression> parseNullCoalesce();
    std::unique_ptr<Expression> parseLogicalOr();
    std::unique_ptr<Expression> parseLogicalAnd();
    std::unique_ptr<Expression> parseBitwiseOr();
    std::unique_ptr<Expression> parseBitwiseXor();
    std::unique_ptr<Expression> parseBitwiseAnd();
    std::unique_ptr<Expression> parseEquality();
    std::unique_ptr<Expression> parseComparison();
    std::unique_ptr<Expression> parseShift();
    std::unique_ptr<Expression> parseAddition();
    std::unique_ptr<Expression> parseMultiplication();
    std::unique_ptr<Expression> parsePower();
    std::unique_ptr<Expression> parseUnary();
    std::unique_ptr<Expression> parsePostfix();
    std::unique_ptr<Expression> parseCall();
    std::unique_ptr<Expression> parsePrimary();
    std::unique_ptr<Expression> parseArrayLiteral();
    std::unique_ptr<Expression> parseLambda();
    std::unique_ptr<Expression> parsePipe();

    [[noreturn]] void error(const std::string& message);
};

} // namespace omscript

#endif // PARSER_H
