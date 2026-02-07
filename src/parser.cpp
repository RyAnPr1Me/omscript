#include "parser.h"
#include <stdexcept>
#include <iostream>

namespace omscript {

Parser::Parser(const std::vector<Token>& tokens)
    : tokens(tokens), current(0) {}

Token Parser::peek(int offset) {
    size_t index = current + offset;
    if (index >= tokens.size()) {
        return tokens.back();
    }
    return tokens[index];
}

Token Parser::advance() {
    if (!isAtEnd()) {
        current++;
    }
    return tokens[current - 1];
}

bool Parser::check(TokenType type) {
    if (isAtEnd()) return false;
    return peek().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::isAtEnd() {
    return peek().type == TokenType::END_OF_FILE;
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) {
        return advance();
    }
    error(message);
    return peek();
}

void Parser::error(const std::string& message) {
    Token token = peek();
    std::string errorMsg = "Parse error at line " + std::to_string(token.line) +
                          ", column " + std::to_string(token.column) + ": " + message;
    throw std::runtime_error(errorMsg);
}

std::unique_ptr<Program> Parser::parse() {
    std::vector<std::unique_ptr<FunctionDecl>> functions;
    
    while (!isAtEnd()) {
        functions.push_back(parseFunction());
    }
    
    return std::make_unique<Program>(std::move(functions));
}

std::unique_ptr<FunctionDecl> Parser::parseFunction() {
    consume(TokenType::FN, "Expected 'fn'");
    Token name = consume(TokenType::IDENTIFIER, "Expected function name");
    
    consume(TokenType::LPAREN, "Expected '(' after function name");
    
    std::vector<Parameter> parameters;
    if (!check(TokenType::RPAREN)) {
        do {
            Token paramName = consume(TokenType::IDENTIFIER, "Expected parameter name");
            parameters.push_back(Parameter(paramName.lexeme));
        } while (match(TokenType::COMMA));
    }
    
    consume(TokenType::RPAREN, "Expected ')' after parameters");
    
    auto body = std::unique_ptr<BlockStmt>(dynamic_cast<BlockStmt*>(parseBlock().release()));
    
    return std::make_unique<FunctionDecl>(name.lexeme, std::move(parameters), std::move(body));
}

std::unique_ptr<Statement> Parser::parseStatement() {
    if (match(TokenType::IF)) return parseIfStmt();
    if (match(TokenType::WHILE)) return parseWhileStmt();
    if (match(TokenType::FOR)) return parseForStmt();
    if (match(TokenType::RETURN)) return parseReturnStmt();
    if (match(TokenType::BREAK)) return parseBreakStmt();
    if (match(TokenType::CONTINUE)) return parseContinueStmt();
    if (match(TokenType::VAR) || match(TokenType::CONST)) {
        return parseVarDecl();
    }
    if (check(TokenType::LBRACE)) return parseBlock();
    
    return parseExprStmt();
}

std::unique_ptr<Statement> Parser::parseBlock() {
    consume(TokenType::LBRACE, "Expected '{'");
    
    std::vector<std::unique_ptr<Statement>> statements;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        statements.push_back(parseStatement());
    }
    
    consume(TokenType::RBRACE, "Expected '}'");
    
    return std::make_unique<BlockStmt>(std::move(statements));
}

std::unique_ptr<Statement> Parser::parseVarDecl() {
    bool isConst = tokens[current - 1].type == TokenType::CONST;
    Token name = consume(TokenType::IDENTIFIER, "Expected variable name");
    
    std::unique_ptr<Expression> initializer = nullptr;
    if (match(TokenType::ASSIGN)) {
        initializer = parseExpression();
    }
    
    consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");
    
    return std::make_unique<VarDecl>(name.lexeme, std::move(initializer), isConst);
}

std::unique_ptr<Statement> Parser::parseIfStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'if'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after condition");
    
    auto thenBranch = parseStatement();
    std::unique_ptr<Statement> elseBranch = nullptr;
    
    if (match(TokenType::ELSE)) {
        elseBranch = parseStatement();
    }
    
    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

std::unique_ptr<Statement> Parser::parseWhileStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'while'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after condition");
    
    auto body = parseStatement();
    
    return std::make_unique<WhileStmt>(std::move(condition), std::move(body));
}

std::unique_ptr<Statement> Parser::parseForStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'for'");
    
    // Parse: for (var in start...end) or for (var in start...end...step)
    Token varName = consume(TokenType::IDENTIFIER, "Expected iterator variable");
    consume(TokenType::IN, "Expected 'in' after iterator variable");
    
    auto start = parseExpression();
    consume(TokenType::RANGE, "Expected '...' in for range");
    auto end = parseExpression();
    
    std::unique_ptr<Expression> step = nullptr;
    if (match(TokenType::RANGE)) {
        step = parseExpression();
    }
    
    consume(TokenType::RPAREN, "Expected ')' after for range");
    
    auto body = parseStatement();
    
    return std::make_unique<ForStmt>(varName.lexeme, std::move(start), std::move(end), std::move(step), std::move(body));
}

std::unique_ptr<Statement> Parser::parseBreakStmt() {
    consume(TokenType::SEMICOLON, "Expected ';' after 'break'");
    return std::make_unique<BreakStmt>();
}

std::unique_ptr<Statement> Parser::parseContinueStmt() {
    consume(TokenType::SEMICOLON, "Expected ';' after 'continue'");
    return std::make_unique<ContinueStmt>();
}

std::unique_ptr<Statement> Parser::parseReturnStmt() {
    std::unique_ptr<Expression> value = nullptr;
    
    if (!check(TokenType::SEMICOLON)) {
        value = parseExpression();
    }
    
    consume(TokenType::SEMICOLON, "Expected ';' after return statement");
    
    return std::make_unique<ReturnStmt>(std::move(value));
}

std::unique_ptr<Statement> Parser::parseExprStmt() {
    auto expr = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after expression");
    
    return std::make_unique<ExprStmt>(std::move(expr));
}

std::unique_ptr<Expression> Parser::parseExpression() {
    return parseAssignment();
}

std::unique_ptr<Expression> Parser::parseAssignment() {
    auto expr = parseLogicalOr();
    
    if (match(TokenType::ASSIGN)) {
        // Check if left side is an identifier
        if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto idExpr = dynamic_cast<IdentifierExpr*>(expr.get());
            auto value = parseAssignment();
            return std::make_unique<AssignExpr>(idExpr->name, std::move(value));
        } else {
            error("Invalid assignment target");
        }
    }
    
    return expr;
}

std::unique_ptr<Expression> Parser::parseLogicalOr() {
    auto left = parseLogicalAnd();
    
    while (match(TokenType::OR)) {
        std::string op = tokens[current - 1].lexeme;
        auto right = parseLogicalAnd();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parseLogicalAnd() {
    auto left = parseEquality();
    
    while (match(TokenType::AND)) {
        std::string op = tokens[current - 1].lexeme;
        auto right = parseEquality();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parseEquality() {
    auto left = parseComparison();
    
    while (match(TokenType::EQ) || match(TokenType::NE)) {
        std::string op = tokens[current - 1].lexeme;
        auto right = parseComparison();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parseComparison() {
    auto left = parseAddition();
    
    while (match(TokenType::LT) || match(TokenType::LE) ||
           match(TokenType::GT) || match(TokenType::GE)) {
        std::string op = tokens[current - 1].lexeme;
        auto right = parseAddition();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parseAddition() {
    auto left = parseMultiplication();
    
    while (match(TokenType::PLUS) || match(TokenType::MINUS)) {
        std::string op = tokens[current - 1].lexeme;
        auto right = parseMultiplication();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parseMultiplication() {
    auto left = parseUnary();
    
    while (match(TokenType::STAR) || match(TokenType::SLASH) || match(TokenType::PERCENT)) {
        std::string op = tokens[current - 1].lexeme;
        auto right = parseUnary();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parseUnary() {
    if (match(TokenType::MINUS) || match(TokenType::NOT)) {
        std::string op = tokens[current - 1].lexeme;
        auto operand = parseUnary();
        return std::make_unique<UnaryExpr>(op, std::move(operand));
    }
    
    return parsePostfix();
}

std::unique_ptr<Expression> Parser::parsePostfix() {
    auto expr = parseCall();
    
    // Handle postfix operators
    if (match(TokenType::PLUSPLUS) || match(TokenType::MINUSMINUS)) {
        std::string op = tokens[current - 1].lexeme;
        return std::make_unique<PostfixExpr>(op, std::move(expr));
    }
    
    // Handle array indexing
    while (match(TokenType::LBRACKET)) {
        auto index = parseExpression();
        consume(TokenType::RBRACKET, "Expected ']' after array index");
        expr = std::make_unique<IndexExpr>(std::move(expr), std::move(index));
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
            
            if (!check(TokenType::RPAREN)) {
                do {
                    arguments.push_back(parseExpression());
                } while (match(TokenType::COMMA));
            }
            
            consume(TokenType::RPAREN, "Expected ')' after arguments");
            
            return std::make_unique<CallExpr>(idExpr->name, std::move(arguments));
        } else {
            error("Invalid function call");
        }
    }
    
    return expr;
}

std::unique_ptr<Expression> Parser::parsePrimary() {
    if (match(TokenType::INTEGER)) {
        Token token = tokens[current - 1];
        return std::make_unique<LiteralExpr>(token.intValue);
    }
    
    if (match(TokenType::FLOAT)) {
        Token token = tokens[current - 1];
        return std::make_unique<LiteralExpr>(token.floatValue);
    }
    
    if (match(TokenType::STRING)) {
        Token token = tokens[current - 1];
        return std::make_unique<LiteralExpr>(token.lexeme);
    }
    
    if (match(TokenType::IDENTIFIER)) {
        Token token = tokens[current - 1];
        return std::make_unique<IdentifierExpr>(token.lexeme);
    }
    
    if (match(TokenType::LPAREN)) {
        auto expr = parseExpression();
        consume(TokenType::RPAREN, "Expected ')' after expression");
        return expr;
    }
    
    if (match(TokenType::LBRACKET)) {
        return parseArrayLiteral();
    }
    
    error("Expected expression");
    return nullptr;
}

std::unique_ptr<Expression> Parser::parseArrayLiteral() {
    std::vector<std::unique_ptr<Expression>> elements;
    
    if (!check(TokenType::RBRACKET)) {
        do {
            elements.push_back(parseExpression());
        } while (match(TokenType::COMMA));
    }
    
    consume(TokenType::RBRACKET, "Expected ']' after array elements");
    
    return std::make_unique<ArrayExpr>(std::move(elements));
}

} // namespace omscript
