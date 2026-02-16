#include "parser.h"
#include <stdexcept>
#include <iostream>

namespace omscript {

Parser::Parser(const std::vector<Token>& tokens)
    : tokens(tokens), current(0), inOptMaxFunction(false) {}

Token Parser::peek(int offset) const {
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

bool Parser::check(TokenType type) const {
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

bool Parser::isAtEnd() const {
    return peek().type == TokenType::END_OF_FILE;
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) {
        return advance();
    }
    error(message);
}

void Parser::error(const std::string& message) {
    Token token = peek();
    std::string errorMsg = "Parse error at line " + std::to_string(token.line) +
                          ", column " + std::to_string(token.column) + ": " + message;
    throw std::runtime_error(errorMsg);
}

void Parser::synchronize() {
    advance();
    while (!isAtEnd()) {
        // After a semicolon, we're likely at a new statement.
        if (current > 0 && tokens[current - 1].type == TokenType::SEMICOLON) return;
        // Before a keyword that starts a new statement/declaration.
        switch (peek().type) {
            case TokenType::FN:
            case TokenType::VAR:
            case TokenType::CONST:
            case TokenType::IF:
            case TokenType::WHILE:
            case TokenType::DO:
            case TokenType::FOR:
            case TokenType::RETURN:
            case TokenType::SWITCH:
                return;
            default:
                break;
        }
        advance();
    }
}

std::unique_ptr<Program> Parser::parse() {
    std::vector<std::unique_ptr<FunctionDecl>> functions;
    bool optMaxTagActive = false;
    
    while (!isAtEnd()) {
        if (match(TokenType::OPTMAX_START)) {
            if (optMaxTagActive) {
                error("Nested OPTMAX blocks are not allowed");
            }
            optMaxTagActive = true;
            continue;
        }
        if (match(TokenType::OPTMAX_END)) {
            if (!optMaxTagActive) {
                error("OPTMAX end tag without matching start tag");
            }
            optMaxTagActive = false;
            continue;
        }
        try {
            functions.push_back(parseFunction(optMaxTagActive));
        } catch (const std::runtime_error& e) {
            errors_.push_back(e.what());
            synchronize();
        }
    }

    if (optMaxTagActive) {
        errors_.push_back("Parse error: Unterminated OPTMAX block");
    }

    if (!errors_.empty()) {
        std::string combined;
        for (size_t i = 0; i < errors_.size(); ++i) {
            if (i > 0) combined += "\n";
            combined += errors_[i];
        }
        throw std::runtime_error(combined);
    }
    
    return std::make_unique<Program>(std::move(functions));
}

std::unique_ptr<FunctionDecl> Parser::parseFunction(bool isOptMax) {
    bool savedOptMaxState = inOptMaxFunction;
    inOptMaxFunction = isOptMax;
    consume(TokenType::FN, "Expected 'fn'");
    Token name = consume(TokenType::IDENTIFIER, "Expected function name");
    
    consume(TokenType::LPAREN, "Expected '(' after function name");
    
    std::vector<Parameter> parameters;
    if (!check(TokenType::RPAREN)) {
        do {
            Token paramName = consume(TokenType::IDENTIFIER, "Expected parameter name");
            std::string typeName;
            if (match(TokenType::COLON)) {
                typeName = consume(TokenType::IDENTIFIER, "Expected type name after ':'").lexeme;
            } else if (inOptMaxFunction) {
                error("OPTMAX parameters must include type annotations");
            }
            parameters.push_back(Parameter(paramName.lexeme, typeName));
        } while (match(TokenType::COMMA));
    }
    
    consume(TokenType::RPAREN, "Expected ')' after parameters");
    
    auto blockStmt = parseBlock();
    auto* blockPtr = dynamic_cast<BlockStmt*>(blockStmt.get());
    if (!blockPtr) {
        error("Expected block statement in function body");
    }
    blockStmt.release();
    auto body = std::unique_ptr<BlockStmt>(blockPtr);
    
    inOptMaxFunction = savedOptMaxState;
    
    return std::make_unique<FunctionDecl>(name.lexeme, std::move(parameters), std::move(body), isOptMax);
}

std::unique_ptr<Statement> Parser::parseStatement() {
    if (match(TokenType::IF)) return parseIfStmt();
    if (match(TokenType::WHILE)) return parseWhileStmt();
    if (match(TokenType::DO)) return parseDoWhileStmt();
    if (match(TokenType::FOR)) return parseForStmt();
    if (match(TokenType::RETURN)) return parseReturnStmt();
    if (match(TokenType::BREAK)) return parseBreakStmt();
    if (match(TokenType::CONTINUE)) return parseContinueStmt();
    if (match(TokenType::SWITCH)) return parseSwitchStmt();
    if (match(TokenType::VAR)) {
        auto decl = parseVarDecl(false);
        consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");
        return decl;
    }
    if (match(TokenType::CONST)) {
        auto decl = parseVarDecl(true);
        consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");
        return decl;
    }
    if (check(TokenType::LBRACE)) return parseBlock();
    
    return parseExprStmt();
}

std::unique_ptr<Statement> Parser::parseBlock() {
    consume(TokenType::LBRACE, "Expected '{'");
    
    std::vector<std::unique_ptr<Statement>> statements;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        // Multi-variable declarations: var a = 1, b = 2;
        if (check(TokenType::VAR) || check(TokenType::CONST)) {
            bool isConst = check(TokenType::CONST);
            advance(); // consume var/const
            statements.push_back(parseVarDecl(isConst));
            while (match(TokenType::COMMA)) {
                statements.push_back(parseVarDecl(isConst));
            }
            consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");
        } else {
            statements.push_back(parseStatement());
        }
    }
    
    consume(TokenType::RBRACE, "Expected '}'");
    
    return std::make_unique<BlockStmt>(std::move(statements));
}

std::unique_ptr<Statement> Parser::parseVarDecl(bool isConst) {
    Token name = consume(TokenType::IDENTIFIER, "Expected variable name");
    std::string typeName;
    if (match(TokenType::COLON)) {
        typeName = consume(TokenType::IDENTIFIER, "Expected type name after ':'").lexeme;
    } else if (inOptMaxFunction) {
        error("OPTMAX variables must include type annotations");
    }

    std::unique_ptr<Expression> initializer = nullptr;
    if (match(TokenType::ASSIGN)) {
        initializer = parseExpression();
    }

    auto decl = std::make_unique<VarDecl>(name.lexeme, std::move(initializer), isConst, typeName);
    decl->line = name.line;
    decl->column = name.column;
    return decl;
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

std::unique_ptr<Statement> Parser::parseDoWhileStmt() {
    auto body = parseStatement();
    consume(TokenType::WHILE, "Expected 'while' after do-while body");
    consume(TokenType::LPAREN, "Expected '(' after 'while'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after condition");
    consume(TokenType::SEMICOLON, "Expected ';' after do-while statement");
    
    return std::make_unique<DoWhileStmt>(std::move(body), std::move(condition));
}

std::unique_ptr<Statement> Parser::parseForStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'for'");
    
    // Parse: for (var in start...end) or for (var in start...end...step)
    //    or: for (var in collection)  -- for-each over array
    Token varName = consume(TokenType::IDENTIFIER, "Expected iterator variable");
    std::string iteratorType;
    if (match(TokenType::COLON)) {
        iteratorType = consume(TokenType::IDENTIFIER, "Expected type name after ':'").lexeme;
    } else if (inOptMaxFunction) {
        error("OPTMAX loop variables must include type annotations");
    }
    consume(TokenType::IN, "Expected 'in' after iterator variable");
    
    auto firstExpr = parseExpression();
    
    // If next token is '...', this is a range-based for loop
    if (match(TokenType::RANGE)) {
        auto end = parseExpression();
        
        std::unique_ptr<Expression> step = nullptr;
        if (match(TokenType::RANGE)) {
            step = parseExpression();
        }
        
        consume(TokenType::RPAREN, "Expected ')' after for range");
        auto body = parseStatement();
        
        return std::make_unique<ForStmt>(varName.lexeme, std::move(firstExpr), std::move(end), std::move(step), std::move(body), iteratorType);
    }
    
    // Otherwise this is a for-each loop: for (var in collection)
    consume(TokenType::RPAREN, "Expected ')' after for-each collection");
    auto body = parseStatement();
    return std::make_unique<ForEachStmt>(varName.lexeme, std::move(firstExpr), std::move(body));
}

std::unique_ptr<Statement> Parser::parseBreakStmt() {
    consume(TokenType::SEMICOLON, "Expected ';' after 'break'");
    return std::make_unique<BreakStmt>();
}

std::unique_ptr<Statement> Parser::parseContinueStmt() {
    consume(TokenType::SEMICOLON, "Expected ';' after 'continue'");
    return std::make_unique<ContinueStmt>();
}

std::unique_ptr<Statement> Parser::parseSwitchStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'switch'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after switch condition");
    consume(TokenType::LBRACE, "Expected '{' after switch condition");
    
    std::vector<SwitchCase> cases;
    bool hasDefault = false;
    
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (match(TokenType::CASE)) {
            auto value = parseExpression();
            consume(TokenType::COLON, "Expected ':' after case value");
            
            std::vector<std::unique_ptr<Statement>> body;
            while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) &&
                   !check(TokenType::RBRACE) && !isAtEnd()) {
                body.push_back(parseStatement());
            }
            cases.emplace_back(std::move(value), std::move(body), false);
        } else if (match(TokenType::DEFAULT)) {
            if (hasDefault) {
                error("Duplicate default case in switch statement");
            }
            hasDefault = true;
            consume(TokenType::COLON, "Expected ':' after 'default'");
            
            std::vector<std::unique_ptr<Statement>> body;
            while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) &&
                   !check(TokenType::RBRACE) && !isAtEnd()) {
                body.push_back(parseStatement());
            }
            cases.emplace_back(nullptr, std::move(body), true);
        } else {
            error("Expected 'case' or 'default' in switch statement");
        }
    }
    
    consume(TokenType::RBRACE, "Expected '}' after switch body");
    return std::make_unique<SwitchStmt>(std::move(condition), std::move(cases));
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
    auto expr = parseTernary();
    
    if (match(TokenType::ASSIGN)) {
        // Check if left side is an identifier
        if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto idExpr = dynamic_cast<IdentifierExpr*>(expr.get());
            auto value = parseAssignment();
            auto node = std::make_unique<AssignExpr>(idExpr->name, std::move(value));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else if (expr->type == ASTNodeType::INDEX_EXPR) {
            auto* indexExpr = static_cast<IndexExpr*>(expr.get());
            auto value = parseAssignment();
            auto arrClone = std::move(indexExpr->array);
            auto idxClone = std::move(indexExpr->index);
            auto node = std::make_unique<IndexAssignExpr>(
                std::move(arrClone), std::move(idxClone), std::move(value));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else {
            error("Invalid assignment target");
        }
    }
    
    // Compound assignment operators: +=, -=, *=, /=, %=
    if (match(TokenType::PLUS_ASSIGN) || match(TokenType::MINUS_ASSIGN) ||
        match(TokenType::STAR_ASSIGN) || match(TokenType::SLASH_ASSIGN) ||
        match(TokenType::PERCENT_ASSIGN)) {
        TokenType opType = tokens[current - 1].type;
        std::string opLexeme = tokens[current - 1].lexeme;
        if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto idExpr = dynamic_cast<IdentifierExpr*>(expr.get());
            std::string name = idExpr->name;
            auto rhs = parseAssignment();
            
            // Determine the binary operator from the compound operator
            std::string binOp;
            switch (opType) {
                case TokenType::PLUS_ASSIGN:    binOp = "+"; break;
                case TokenType::MINUS_ASSIGN:   binOp = "-"; break;
                case TokenType::STAR_ASSIGN:    binOp = "*"; break;
                case TokenType::SLASH_ASSIGN:   binOp = "/"; break;
                case TokenType::PERCENT_ASSIGN: binOp = "%"; break;
                default: error("Unknown compound assignment operator: " + opLexeme); break;
            }
            
            // Desugar: x += expr  =>  x = x + expr
            auto lhsRef = std::make_unique<IdentifierExpr>(name);
            lhsRef->line = expr->line;
            lhsRef->column = expr->column;
            auto binExpr = std::make_unique<BinaryExpr>(binOp, std::move(lhsRef), std::move(rhs));
            binExpr->line = expr->line;
            binExpr->column = expr->column;
            auto node = std::make_unique<AssignExpr>(name, std::move(binExpr));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else if (expr->type == ASTNodeType::INDEX_EXPR) {
            error("Compound assignment to array elements is not yet supported; use arr[i] = arr[i] " +
                  opLexeme.substr(0, opLexeme.size() - 1) + " value instead");
        } else {
            error("Invalid compound assignment target");
        }
    }
    
    return expr;
}

std::unique_ptr<Expression> Parser::parseTernary() {
    auto expr = parseLogicalOr();
    
    if (match(TokenType::QUESTION)) {
        Token questionToken = tokens[current - 1];
        auto thenExpr = parseExpression();
        consume(TokenType::COLON, "Expected ':' in ternary expression");
        auto elseExpr = parseTernary();
        auto node = std::make_unique<TernaryExpr>(std::move(expr), std::move(thenExpr), std::move(elseExpr));
        node->line = questionToken.line;
        node->column = questionToken.column;
        return node;
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
    auto left = parseBitwiseOr();
    
    while (match(TokenType::AND)) {
        std::string op = tokens[current - 1].lexeme;
        auto right = parseBitwiseOr();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parseBitwiseOr() {
    auto left = parseBitwiseXor();
    
    while (match(TokenType::PIPE)) {
        std::string op = tokens[current - 1].lexeme;
        auto right = parseBitwiseXor();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parseBitwiseXor() {
    auto left = parseBitwiseAnd();
    
    while (match(TokenType::CARET)) {
        std::string op = tokens[current - 1].lexeme;
        auto right = parseBitwiseAnd();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parseBitwiseAnd() {
    auto left = parseEquality();
    
    while (match(TokenType::AMPERSAND)) {
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
    auto left = parseShift();
    
    while (match(TokenType::LT) || match(TokenType::LE) ||
           match(TokenType::GT) || match(TokenType::GE)) {
        std::string op = tokens[current - 1].lexeme;
        auto right = parseShift();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }
    
    return left;
}

std::unique_ptr<Expression> Parser::parseShift() {
    auto left = parseAddition();
    
    while (match(TokenType::LSHIFT) || match(TokenType::RSHIFT)) {
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
    if (match(TokenType::MINUS) || match(TokenType::NOT) || match(TokenType::TILDE)) {
        Token opToken = tokens[current - 1];
        auto operand = parseUnary();
        auto node = std::make_unique<UnaryExpr>(opToken.lexeme, std::move(operand));
        node->line = opToken.line;
        node->column = opToken.column;
        return node;
    }
    
    if (match(TokenType::PLUSPLUS) || match(TokenType::MINUSMINUS)) {
        Token opToken = tokens[current - 1];
        auto operand = parseUnary();
        auto node = std::make_unique<PrefixExpr>(opToken.lexeme, std::move(operand));
        node->line = opToken.line;
        node->column = opToken.column;
        return node;
    }
    
    return parsePostfix();
}

std::unique_ptr<Expression> Parser::parsePostfix() {
    auto expr = parseCall();
    
    while (true) {
        // Handle postfix operators
        if (match(TokenType::PLUSPLUS) || match(TokenType::MINUSMINUS)) {
            Token opToken = tokens[current - 1];
            expr = std::make_unique<PostfixExpr>(opToken.lexeme, std::move(expr));
            expr->line = opToken.line;
            expr->column = opToken.column;
        }
        // Handle array indexing
        else if (match(TokenType::LBRACKET)) {
            Token bracketToken = tokens[current - 1];
            auto index = parseExpression();
            consume(TokenType::RBRACKET, "Expected ']' after array index");
            auto indexExpr = std::make_unique<IndexExpr>(std::move(expr), std::move(index));
            indexExpr->line = bracketToken.line;
            indexExpr->column = bracketToken.column;
            expr = std::move(indexExpr);
        } else {
            break;
        }
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
            
            auto callExpr = std::make_unique<CallExpr>(idExpr->name, std::move(arguments));
            callExpr->line = expr->line;
            callExpr->column = expr->column;
            return callExpr;
        } else {
            error("Invalid function call");
        }
    }
    
    return expr;
}

std::unique_ptr<Expression> Parser::parsePrimary() {
    if (match(TokenType::INTEGER)) {
        Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(token.intValue);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }
    
    if (match(TokenType::FLOAT)) {
        Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(token.floatValue);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }
    
    if (match(TokenType::STRING)) {
        Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(token.lexeme);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }
    
    if (match(TokenType::IDENTIFIER)) {
        Token token = tokens[current - 1];
        auto expr = std::make_unique<IdentifierExpr>(token.lexeme);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }
    
    if (match(TokenType::LPAREN)) {
        auto expr = parseExpression();
        consume(TokenType::RPAREN, "Expected ')' after expression");
        return expr;
    }
    
    if (match(TokenType::LBRACKET)) {
        Token bracketToken = tokens[current - 1];
        auto arrayExpr = parseArrayLiteral();
        arrayExpr->line = bracketToken.line;
        arrayExpr->column = bracketToken.column;
        return arrayExpr;
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
