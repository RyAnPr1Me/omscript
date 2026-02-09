#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"
#include <vector>
#include <memory>

namespace omscript {

class Parser {
public:
    Parser(const std::vector<Token>& tokens);
    std::unique_ptr<Program> parse();
    
private:
    std::vector<Token> tokens;
    size_t current;
    bool inOptMaxFunction;
    
    Token peek(int offset = 0) const;
    Token advance();
    bool check(TokenType type) const;
    bool match(TokenType type);
    bool isAtEnd() const;
    Token consume(TokenType type, const std::string& message);
    
    // Parsing methods
    std::unique_ptr<FunctionDecl> parseFunction(bool isOptMax);
    std::unique_ptr<Statement> parseStatement();
    std::unique_ptr<Statement> parseBlock();
    std::unique_ptr<Statement> parseVarDecl(bool isConst);
    std::unique_ptr<Statement> parseIfStmt();
    std::unique_ptr<Statement> parseWhileStmt();
    std::unique_ptr<Statement> parseForStmt();
    std::unique_ptr<Statement> parseReturnStmt();
    std::unique_ptr<Statement> parseBreakStmt();
    std::unique_ptr<Statement> parseContinueStmt();
    std::unique_ptr<Statement> parseExprStmt();
    
    std::unique_ptr<Expression> parseExpression();
    std::unique_ptr<Expression> parseAssignment();
    std::unique_ptr<Expression> parseLogicalOr();
    std::unique_ptr<Expression> parseLogicalAnd();
    std::unique_ptr<Expression> parseEquality();
    std::unique_ptr<Expression> parseComparison();
    std::unique_ptr<Expression> parseAddition();
    std::unique_ptr<Expression> parseMultiplication();
    std::unique_ptr<Expression> parseUnary();
    std::unique_ptr<Expression> parsePostfix();
    std::unique_ptr<Expression> parseCall();
    std::unique_ptr<Expression> parsePrimary();
    std::unique_ptr<Expression> parseArrayLiteral();
    
    void error(const std::string& message);
};

} // namespace omscript

#endif // PARSER_H
