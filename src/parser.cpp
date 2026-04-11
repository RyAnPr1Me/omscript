#include "parser.h"
#include "diagnostic.h"
#include "preprocessor.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace omscript {

/// Check if a string is a known type annotation name.
/// Used to disambiguate `x:u32` (type annotation on identifier) from other
/// uses of colon in expression context.
static bool isKnownTypeName(const std::string& name) {
    return name == "u8" || name == "u16" || name == "u32" || name == "u64" ||
           name == "i8" || name == "i16" || name == "i32" || name == "i64" ||
           name == "int" || name == "float" || name == "double" || name == "bool" ||
           name == "string" || name == "dict";
}

Parser::Parser(const std::vector<Token>& tokens)
    : tokens(tokens), current(0), inOptMaxFunction(false),
      importedFiles_(std::make_shared<std::unordered_set<std::string>>()) {}

Parser::Parser(std::vector<Token>&& tokens)
    : tokens(std::move(tokens)), current(0), inOptMaxFunction(false),
      importedFiles_(std::make_shared<std::unordered_set<std::string>>()) {}

const Token& Parser::peek(int offset) const noexcept {
    static const Token eofToken(TokenType::END_OF_FILE, "", 0, 0);
    if (__builtin_expect(tokens.empty(), 0)) {
        return eofToken;
    }
    const size_t index = current + static_cast<size_t>(offset);
    if (__builtin_expect(index >= tokens.size(), 0)) {
        return tokens.back();
    }
    return tokens[index];
}

Token Parser::advance() noexcept {
    if (!isAtEnd()) {
        current++;
    }
    return tokens[current - 1];
}

bool Parser::check(TokenType type) const noexcept {
    if (__builtin_expect(isAtEnd(), 0))
        return false;
    return peek().type == type;
}

bool Parser::match(TokenType type) noexcept {
    if (check(type)) {
        advance();
        return true;
    }
    return false;
}

bool Parser::isAtEnd() const noexcept {
    return peek().type == TokenType::END_OF_FILE;
}

Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) {
        return advance();
    }
    error(message);
    throw std::logic_error("unreachable parser consume() path");
}

[[gnu::cold]] void Parser::error(const std::string& message) {
    Token token = peek();
    throw DiagnosticError(
        Diagnostic{DiagnosticSeverity::Error, {"", token.line, token.column}, "Parse error: " + message});
}

void Parser::synchronize() {
    advance();
    while (!isAtEnd()) {
        // Stop only at tokens that begin a top-level declaration so that
        // statement-level keywords (var, if, for, return, …) inside a broken
        // function body do not cause cascading "Expected 'fn'" errors, and so
        // that OPTMAX_END is never accidentally consumed (which would leave the
        // optMaxTagActive flag set and produce "Unterminated OPTMAX block").
        switch (peek().type) {
        case TokenType::FN:
        case TokenType::OPTMAX_START:
        case TokenType::OPTMAX_END:
            return;
        default:
            break;
        }
        advance();
    }
}

std::unique_ptr<Program> Parser::parse() {
    std::vector<std::unique_ptr<FunctionDecl>> functions;
    std::vector<std::unique_ptr<EnumDecl>> enums;
    std::vector<std::unique_ptr<StructDecl>> structs;
    bool optMaxTagActive = false;
    bool fileNoAlias = false;

    while (!isAtEnd()) {
        if (match(TokenType::IMPORT)) {
            try {
                parseImport(functions, enums, structs);
            } catch (const std::runtime_error& e) {
                errors_.push_back(e.what());
                synchronize();
            }
            continue;
        }
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
        if (match(TokenType::ENUM)) {
            try {
                enums.push_back(parseEnumDecl());
            } catch (const std::runtime_error& e) {
                errors_.push_back(e.what());
                synchronize();
            }
            continue;
        }
        if (match(TokenType::STRUCT)) {
            try {
                structs.push_back(parseStructDecl());
            } catch (const std::runtime_error& e) {
                errors_.push_back(e.what());
                synchronize();
            }
            continue;
        }
        try {
            // Check for file-level @noalias directive (appears before any function
            // keyword and is not followed by 'fn').
            if (check(TokenType::AT) && !fileNoAlias) {
                size_t savedPos = current;
                // Peek ahead: if it's @noalias followed by a newline/struct/enum
                // (not 'fn'), treat it as a file-level directive.
                advance(); // consume '@'
                if (check(TokenType::IDENTIFIER) && peek().lexeme == "noalias") {
                    advance(); // consume 'noalias'
                    // Check if the next thing is NOT a function-related token
                    if (check(TokenType::AT) || check(TokenType::FN) || check(TokenType::STRUCT) || check(TokenType::ENUM) || check(TokenType::OPTMAX_START) || check(TokenType::IMPORT) || isAtEnd()) {
                        // This is a file-level @noalias directive
                        fileNoAlias = true;
                        continue;
                    }
                }
                // Not a file-level directive, restore position
                current = savedPos;
            }
            // Parse optional function annotations: @inline, @noinline, @cold,
            // @hot, @pure, @noreturn, @static, @flatten, @unroll, @nounroll,
            // @restrict, @vectorize, @novectorize, @minsize, @optnone, @nounwind
            bool hintInline = false, hintNoInline = false, hintCold = false;
            bool hintHot = false, hintPure = false, hintNoReturn = false;
            bool hintStatic = false, hintFlatten = false;
            bool hintUnroll = false, hintNoUnroll = false;
            bool hintRestrict = false;
            bool hintVectorize = false, hintNoVectorize = false;
            bool hintParallelize = false, hintNoParallelize = false;
            bool hintMinSize = false, hintOptNone = false, hintNoUnwind = false;
            bool hintConstEval = false;
            while (check(TokenType::AT)) {
                advance(); // consume '@'
                const Token ann = consume(TokenType::IDENTIFIER, "Expected annotation name after '@'");
                if (ann.lexeme == "inline") {
                    hintInline = true;
                } else if (ann.lexeme == "noinline") {
                    hintNoInline = true;
                } else if (ann.lexeme == "cold") {
                    hintCold = true;
                } else if (ann.lexeme == "hot") {
                    hintHot = true;
                } else if (ann.lexeme == "pure") {
                    hintPure = true;
                } else if (ann.lexeme == "noreturn") {
                    hintNoReturn = true;
                } else if (ann.lexeme == "static") {
                    hintStatic = true;
                } else if (ann.lexeme == "flatten") {
                    hintFlatten = true;
                } else if (ann.lexeme == "unroll") {
                    hintUnroll = true;
                } else if (ann.lexeme == "nounroll") {
                    hintNoUnroll = true;
                } else if (ann.lexeme == "restrict") {
                    hintRestrict = true;
                } else if (ann.lexeme == "vectorize") {
                    hintVectorize = true;
                } else if (ann.lexeme == "novectorize") {
                    hintNoVectorize = true;
                } else if (ann.lexeme == "parallel") {
                    hintParallelize = true;
                } else if (ann.lexeme == "noparallel") {
                    hintNoParallelize = true;
                } else if (ann.lexeme == "noalias") {
                    hintRestrict = true;  // @noalias on functions = @restrict
                } else if (ann.lexeme == "minsize") {
                    hintMinSize = true;
                } else if (ann.lexeme == "optnone") {
                    hintOptNone = true;
                } else if (ann.lexeme == "nounwind") {
                    hintNoUnwind = true;
                } else if (ann.lexeme == "const_eval") {
                    hintConstEval = true;
                } else {
                    error("Unknown function annotation '@" + ann.lexeme +
                          "'; supported: @inline, @noinline, @cold, @hot, @pure, @noreturn, @static, @flatten, @unroll, @nounroll, @restrict, @noalias, @vectorize, @novectorize, @parallel, @noparallel, @minsize, @optnone, @nounwind, @const_eval (use @prefetch on parameters)");
                }
            }
            auto func = parseFunction(optMaxTagActive);
            func->hintInline = hintInline;
            func->hintNoInline = hintNoInline;
            func->hintCold = hintCold;
            func->hintHot = hintHot;
            func->hintPure = hintPure;
            func->hintNoReturn = hintNoReturn;
            func->hintStatic = hintStatic;
            func->hintFlatten = hintFlatten;
            func->hintUnroll = hintUnroll;
            func->hintNoUnroll = hintNoUnroll;
            func->hintRestrict = hintRestrict;
            func->hintVectorize = hintVectorize;
            func->hintNoVectorize = hintNoVectorize;
            func->hintParallelize = hintParallelize;
            func->hintNoParallelize = hintNoParallelize;
            func->hintMinSize = hintMinSize;
            func->hintOptNone = hintOptNone;
            func->hintNoUnwind = hintNoUnwind;
            func->hintConstEval = hintConstEval;
            // Warn about conflicting annotations at parse time.
            if (hintOptNone && hintInline) {
                std::cerr << "warning: '@optnone' and '@inline' are mutually exclusive on function '"
                          << func->name << "' — '@inline' will be ignored (optnone requires noinline)\n";
            }
            if (hintOptNone && hintHot) {
                std::cerr << "warning: '@optnone' disables all optimizations on function '"
                          << func->name << "' — '@hot' annotation will have no effect\n";
            }
            functions.push_back(std::move(func));
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
            if (i > 0)
                combined += "\n";
            combined += errors_[i];
        }
        // Errors already contain formatted location information from individual
        // DiagnosticError exceptions; wrap in a DiagnosticError so callers can
        // catch a single exception type for all compilation failures.
        throw DiagnosticError(Diagnostic{DiagnosticSeverity::Error, {"", 0, 0}, combined});
    }

    // Append generated lambda functions to the program
    for (auto& lf : lambdaFunctions_) {
        functions.push_back(std::move(lf));
    }
    lambdaFunctions_.clear();

    return std::make_unique<Program>(std::move(functions), std::move(enums), std::move(structs), fileNoAlias);
}

void Parser::parseImport(std::vector<std::unique_ptr<FunctionDecl>>& functions,
                         std::vector<std::unique_ptr<EnumDecl>>& enums,
                         std::vector<std::unique_ptr<StructDecl>>& structs) {
    const Token fileToken = consume(TokenType::STRING, "Expected filename string after 'import'");
    consume(TokenType::SEMICOLON, "Expected ';' after import statement");

    std::string filename = fileToken.lexeme;
    // Append .om extension if not already present
    if (filename.size() < 3 || filename.compare(filename.size() - 3, 3, ".om") != 0) {
        filename += ".om";
    }

    // Resolve full path relative to the base directory
    std::string fullPath;
    if (baseDir_.empty()) {
        fullPath = filename;
    } else {
        fullPath = baseDir_ + "/" + filename;
    }

    // Normalize the path to a canonical form for circular import detection
    std::error_code ec;
    auto canonicalPath = std::filesystem::weakly_canonical(fullPath, ec);
    const std::string normalizedPath = ec ? fullPath : canonicalPath.string();

    // Check for circular/duplicate imports
    if (importedFiles_->count(normalizedPath)) {
        return; // Already imported, skip
    }
    importedFiles_->insert(normalizedPath);

    // Read the imported file
    std::ifstream file(fullPath);
    if (!file.is_open()) {
        error("Cannot open imported file: " + fullPath);
    }
    std::string source((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    // Preprocess then lex the imported file
    {
        Preprocessor importPP(fullPath);
        source = importPP.process(source);
        for (const auto& w : importPP.warnings()) {
            std::cerr << w << "\n";
        }
    }
    Lexer importLexer(std::move(source));
    std::vector<Token> importTokens = importLexer.tokenize();

    // Parse the imported file
    Parser importParser(std::move(importTokens));
    const std::string importDir = std::filesystem::path(fullPath).parent_path().string();
    importParser.setBaseDir(importDir);
    importParser.importedFiles_ = importedFiles_; // Share import tracking

    auto importedProgram = importParser.parse();

    // Merge imported declarations into the current program
    for (auto& fn : importedProgram->functions) {
        functions.push_back(std::move(fn));
    }
    for (auto& en : importedProgram->enums) {
        enums.push_back(std::move(en));
    }
    for (auto& st : importedProgram->structs) {
        structs.push_back(std::move(st));
    }

    // Also import struct names so struct literals can be parsed
    for (const auto& name : importParser.structNames_) {
        structNames_.insert(name);
    }
}

std::string Parser::parseTypeAnnotation() {
    // Support reference type annotations: &type (e.g., &i32, &f64)
    std::string prefix;
    if (match(TokenType::AMPERSAND)) {
        prefix = "&";
    }
    // Accept identifiers and the 'struct' keyword as type names
    std::string typeName;
    if (check(TokenType::STRUCT)) {
        typeName = advance().lexeme;
    } else {
        typeName = consume(TokenType::IDENTIFIER, "Expected type name").lexeme;
    }
    // Support array type annotations: type[], type[][], etc.
    while (check(TokenType::LBRACKET) && (current + 1 < tokens.size()) &&
           tokens[current + 1].type == TokenType::RBRACKET) {
        advance(); // consume '['
        advance(); // consume ']'
        typeName += "[]";
    }
    // Support dict[KeyType, ValType] generic annotation (e.g., dict[str, int])
    // Only activates when the type name is exactly "dict" followed by '[' with
    // non-empty content — avoids any collision with the existing type[] handling.
    if (typeName == "dict" && check(TokenType::LBRACKET) && current + 1 < tokens.size() &&
        tokens[current + 1].type != TokenType::RBRACKET) {
        advance(); // consume '['
        std::string typeParams;
        int depth = 1;
        while (!isAtEnd() && depth > 0) {
            const Token& t = advance();
            if (t.type == TokenType::LBRACKET) depth++;
            else if (t.type == TokenType::RBRACKET) { depth--; if (depth == 0) break; }
            if (depth > 0) typeParams += t.lexeme;
        }
        typeName += "[" + typeParams + "]";
    }
    return prefix + typeName;
}

std::unique_ptr<FunctionDecl> Parser::parseFunction(bool isOptMax) {
    const bool savedOptMaxState = inOptMaxFunction;
    inOptMaxFunction = isOptMax;
    consume(TokenType::FN, "Expected 'fn'");
    const Token name = consume(TokenType::IDENTIFIER, "Expected function name");

    // Parse optional type parameters: <T, R, ...>
    std::vector<std::string> typeParams;
    if (match(TokenType::LT)) {
        do {
            const Token tp = consume(TokenType::IDENTIFIER, "Expected type parameter name");
            typeParams.push_back(tp.lexeme);
        } while (match(TokenType::COMMA));
        consume(TokenType::GT, "Expected '>' after type parameters");
    }

    consume(TokenType::LPAREN, "Expected '(' after function name");

    std::vector<Parameter> parameters;
    bool hasDefault = false;
    if (!check(TokenType::RPAREN)) {
        do {
            // Parse optional parameter annotation: @prefetch
            bool paramPrefetch = false;
            if (check(TokenType::AT)) {
                advance(); // consume '@'
                // Accept both IDENTIFIER and PREFETCH keyword for @prefetch
                if (check(TokenType::PREFETCH)) {
                    advance();
                    paramPrefetch = true;
                } else {
                    const Token ann = consume(TokenType::IDENTIFIER, "Expected annotation name after '@'");
                    if (ann.lexeme == "prefetch") {
                        paramPrefetch = true;
                    } else {
                        error("Unknown parameter annotation '@" + ann.lexeme +
                              "'; supported: @prefetch");
                    }
                }
            }
            const Token paramName = consume(TokenType::IDENTIFIER, "Expected parameter name");
            std::string typeName;
            if (match(TokenType::COLON)) {
                typeName = parseTypeAnnotation();
            } else if (inOptMaxFunction) {
                error("OPTMAX parameters must include type annotations");
            }
            std::unique_ptr<Expression> defaultVal = nullptr;
            if (match(TokenType::ASSIGN)) {
                defaultVal = parsePrimary();
                if (!dynamic_cast<LiteralExpr*>(defaultVal.get())) {
                    error("Default parameter value must be a literal (integer, float, or string)");
                }
                hasDefault = true;
            } else if (hasDefault) {
                error("Non-default parameter '" + paramName.lexeme + "' cannot follow a default parameter");
            }
            Parameter param(paramName.lexeme, typeName, std::move(defaultVal));
            param.hintPrefetch = paramPrefetch;
            parameters.push_back(std::move(param));
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::RPAREN, "Expected ')' after parameters");

    // Optional return type annotation: -> type
    std::string returnType;
    if (match(TokenType::ARROW)) {
        returnType = parseTypeAnnotation();
    }

    auto body = parseBlock();

    inOptMaxFunction = savedOptMaxState;

    auto funcDecl = std::make_unique<FunctionDecl>(name.lexeme, std::move(typeParams), std::move(parameters), std::move(body), isOptMax, returnType);
    funcDecl->line = name.line;
    funcDecl->column = name.column;
    return funcDecl;
}

std::unique_ptr<Statement> Parser::parseStatement() {
    const RecursionGuard guard(*this);
    // Capture the keyword token position for source location tracking.
    if (match(TokenType::LIKELY) || match(TokenType::UNLIKELY)) {
        const Token hintKw = tokens[current - 1];
        const bool isLikely = (hintKw.type == TokenType::LIKELY);
        if (!match(TokenType::IF)) {
            error("Expected 'if' after '" + hintKw.lexeme + "'");
        }
        const Token kw = tokens[current - 1];
        auto stmt = parseIfStmt();
        auto* ifStmt = dynamic_cast<IfStmt*>(stmt.get());
        if (ifStmt) {
            ifStmt->hintLikely = isLikely;
            ifStmt->hintUnlikely = !isLikely;
        }
        stmt->line = hintKw.line;
        stmt->column = hintKw.column;
        return stmt;
    }
    if (match(TokenType::IF)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseIfStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::WHILE)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseWhileStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::DO)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseDoWhileStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::FOR)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseForStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::RETURN)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseReturnStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::BREAK)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseBreakStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::CONTINUE)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseContinueStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::SWITCH)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseSwitchStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::UNLESS)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseUnlessStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::UNTIL)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseUntilStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::LOOP)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseLoopStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::REPEAT)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseRepeatStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::DEFER)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseDeferStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::GUARD)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseGuardStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::WHEN)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseWhenStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::FOREVER)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseForeverStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::FOREACH)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseForEachStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::ELIF)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseElifStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (check(TokenType::SWAP) && !(current + 1 < tokens.size() && tokens[current + 1].type == TokenType::LPAREN)) {
        advance(); // consume SWAP
        const Token kw = tokens[current - 1];
        auto stmt = parseSwapStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::TIMES)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseTimesStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
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
    if (check(TokenType::LBRACE)) {
        const Token kw = peek();
        auto stmt = parseBlock();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::TRY)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseTryCatchStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::THROW)) {
        const Token kw = tokens[current - 1];
        auto stmt = parseThrowStmt();
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    if (match(TokenType::INVALIDATE)) {
        const Token kw = tokens[current - 1];
        const Token varName = consume(TokenType::IDENTIFIER, "Expected variable name after 'invalidate'");
        consume(TokenType::SEMICOLON, "Expected ';' after invalidate statement");
        auto stmt = std::make_unique<InvalidateStmt>(varName.lexeme);
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }
    // register var — hint to keep variable in a CPU register.
    // Syntax: register var name[:type] = expr;
    if (match(TokenType::REGISTER)) {
        const Token kw = tokens[current - 1];
        const bool isConst = check(TokenType::CONST);
        if (!match(TokenType::VAR) && !match(TokenType::CONST)) {
            error("Expected 'var' or 'const' after 'register'");
        }
        const Token name = consume(TokenType::IDENTIFIER, "Expected variable name after 'register var'");
        std::string typeName;
        if (match(TokenType::COLON)) {
            typeName = parseTypeAnnotation();
        }
        std::unique_ptr<Expression> init = nullptr;
        if (match(TokenType::ASSIGN)) {
            init = parseExpression();
        }
        consume(TokenType::SEMICOLON, "Expected ';' after register variable declaration");
        auto decl = std::make_unique<VarDecl>(name.lexeme, std::move(init), isConst, typeName);
        decl->isRegister = true;
        decl->line = kw.line;
        decl->column = kw.column;
        return decl;
    }
    if (match(TokenType::PREFETCH)) {
        const Token kw = tokens[current - 1];

        // Parse optional byte offset: prefetch+128 or prefetch+64 etc.
        // The '+' immediately after 'prefetch' indicates cache-line-ahead
        // speculative prefetching.  E.g. prefetch+128 fetches 2 cache lines
        // ahead (128 bytes = 2 × 64-byte cache lines on most CPUs).
        int64_t offsetBytes = 0;
        if (match(TokenType::PLUS)) {
            if (!check(TokenType::INTEGER)) {
                error("Expected integer offset after '+' in prefetch");
            }
            const Token offsetTok = advance();
            offsetBytes = std::stoll(offsetTok.lexeme);
        }

        // Parse optional attributes: hot, immut
        bool hintHot = false;
        bool hintImmut = false;
        while (check(TokenType::IDENTIFIER) && !isAtEnd()) {
            const std::string& attrName = peek().lexeme;
            if (attrName == "hot") { hintHot = true; advance(); continue; }
            if (attrName == "immut") { hintImmut = true; advance(); continue; }
            break;
        }
        // Determine if this is a variable declaration or standalone prefetch.
        // Declaration forms:
        //   prefetch [attrs] var name[:type] [= expr];
        //   prefetch [attrs] name:type = expr;       (has '=')
        //   prefetch [attrs] name = expr;             (has '=')
        // Standalone forms (existing variable):
        //   prefetch name[:type];                     (no '=' → just re-prefetch)
        bool isDecl = false;
        if (check(TokenType::VAR)) {
            isDecl = true;
        } else if (check(TokenType::IDENTIFIER)) {
            // Look ahead past optional :type to see if there's an '='
            size_t lookahead = current + 1;
            if (lookahead < tokens.size() && tokens[lookahead].type == TokenType::COLON) {
                lookahead++; // skip ':'
                if (lookahead < tokens.size() && tokens[lookahead].type == TokenType::IDENTIFIER)
                    lookahead++; // skip type name
                // Skip optional array brackets []
                while (lookahead + 1 < tokens.size() &&
                       tokens[lookahead].type == TokenType::LBRACKET &&
                       tokens[lookahead + 1].type == TokenType::RBRACKET)
                    lookahead += 2;
            }
            if (lookahead < tokens.size() && tokens[lookahead].type == TokenType::ASSIGN)
                isDecl = true;
        }
        if (isDecl) {
            // prefetch [attrs] var name:type = expr; OR prefetch [attrs] name:type = expr;
            const bool isConst = hintImmut;
            std::string typeName;
            if (match(TokenType::VAR)) {
                // consumed var
            }
            const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in prefetch declaration");
            if (match(TokenType::COLON)) {
                typeName = parseTypeAnnotation();
            }
            std::unique_ptr<Expression> init = nullptr;
            if (match(TokenType::ASSIGN)) {
                init = parseExpression();
            }
            consume(TokenType::SEMICOLON, "Expected ';' after prefetch declaration");
            auto varDecl = std::make_unique<VarDecl>(name.lexeme, std::move(init), isConst, typeName);
            varDecl->line = kw.line;
            varDecl->column = kw.column;
            auto stmt = std::make_unique<PrefetchStmt>(std::move(varDecl), hintHot, hintImmut, offsetBytes);
            stmt->line = kw.line;
            stmt->column = kw.column;
            return stmt;
        }
        // Standalone prefetch of existing variable: prefetch name[:type];
        if (check(TokenType::IDENTIFIER)) {
            const Token varName = advance();
            // Consume optional :type annotation
            if (match(TokenType::COLON)) {
                parseTypeAnnotation(); // consume and discard
            }
            consume(TokenType::SEMICOLON, "Expected ';' after prefetch statement");
            auto stmt = std::make_unique<PrefetchStmt>(varName.lexeme, offsetBytes);
            stmt->line = kw.line;
            stmt->column = kw.column;
            return stmt;
        }
        error("Expected variable name or declaration after 'prefetch'");
    }
    if (match(TokenType::MOVE)) {
        // move semantics: either `move var x = expr;` or `move x = expr;`
        const Token kw = tokens[current - 1];
        // Could be: move <type> <name> = <expr>;
        // or just: move used as expression context (handled elsewhere)
        if (check(TokenType::VAR) || (check(TokenType::IDENTIFIER) && current + 1 < tokens.size() &&
            tokens[current + 1].type == TokenType::IDENTIFIER)) {
            // `move var x = expr;` or `move int x = expr;`
            std::string typeName;
            if (match(TokenType::VAR)) {
                typeName = "";
            } else {
                typeName = advance().lexeme; // consume type name
            }
            const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in move declaration");
            consume(TokenType::ASSIGN, "Expected '=' in move declaration");
            auto init = parseExpression();
            consume(TokenType::SEMICOLON, "Expected ';' after move declaration");
            auto stmt = std::make_unique<MoveDecl>(name.lexeme, typeName, std::move(init));
            stmt->line = kw.line;
            stmt->column = kw.column;
            return stmt;
        }
        // Otherwise treat as expression statement: move <expr> used in assignment
        // e.g. `x = move y;`  — rewind and let expression parsing handle it
        current--; // put back 'move' token
        return parseExprStmt();
    }
    if (match(TokenType::BORROW)) {
        // `borrow var ref = expr;` or `borrow type ref = expr;` or `borrow ref:type = expr;`
        const Token kw = tokens[current - 1];
        std::string typeName;
        if (match(TokenType::VAR)) {
            typeName = "";
        } else if (check(TokenType::IDENTIFIER) && current + 1 < tokens.size() &&
                   tokens[current + 1].type == TokenType::IDENTIFIER) {
            typeName = advance().lexeme;
        }
        const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in borrow declaration");
        // Support name:type syntax: borrow j:u32 = expr;
        if (typeName.empty() && match(TokenType::COLON)) {
            typeName = parseTypeAnnotation();
        }
        consume(TokenType::ASSIGN, "Expected '=' in borrow declaration");
        auto init = parseExpression();
        consume(TokenType::SEMICOLON, "Expected ';' after borrow declaration");
        // Create a VarDecl with a BorrowExpr wrapper
        auto borrowExpr = std::make_unique<BorrowExpr>(std::move(init));
        borrowExpr->line = kw.line;
        borrowExpr->column = kw.column;
        auto stmt = std::make_unique<VarDecl>(name.lexeme, std::move(borrowExpr), false, typeName);
        stmt->line = kw.line;
        stmt->column = kw.column;
        return stmt;
    }

    return parseExprStmt();
}

std::unique_ptr<BlockStmt> Parser::parseBlock() {
    consume(TokenType::LBRACE, "Expected '{'");

    std::vector<std::unique_ptr<Statement>> statements;
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        // Multi-variable declarations: var a = 1, b = 2;
        if (check(TokenType::VAR) || check(TokenType::CONST)) {
            const bool isConst = check(TokenType::CONST);
            advance(); // consume var/const
            // Check for array destructuring: var [a, b, c] = expr;
            if (check(TokenType::LBRACKET)) {
                auto decls = parseDestructuringDecl(isConst);
                for (auto& d : decls) {
                    statements.push_back(std::move(d));
                }
                consume(TokenType::SEMICOLON, "Expected ';' after destructuring declaration");
            } else {
                statements.push_back(parseVarDecl(isConst));
                while (match(TokenType::COMMA)) {
                    statements.push_back(parseVarDecl(isConst));
                }
                consume(TokenType::SEMICOLON, "Expected ';' after variable declaration");
            }
        } else {
            statements.push_back(parseStatement());
        }
    }

    consume(TokenType::RBRACE, "Expected '}'");

    return std::make_unique<BlockStmt>(std::move(statements));
}

std::unique_ptr<Statement> Parser::parseVarDecl(bool isConst) {
    const Token name = consume(TokenType::IDENTIFIER, "Expected variable name");
    std::string typeName;
    if (match(TokenType::COLON)) {
        typeName = parseTypeAnnotation();
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
    } else if (match(TokenType::ELIF)) {
        // elif (cond) { ... } => else if (cond) { ... }
        auto elifStmt = parseElifStmt();
        elifStmt->line = tokens[current - 1].line;
        elifStmt->column = tokens[current - 1].column;
        elseBranch = std::move(elifStmt);
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

    // Support both: do { ... } while (cond); and do { ... } until (cond);
    bool isUntil = false;
    if (match(TokenType::UNTIL)) {
        isUntil = true;
    } else {
        consume(TokenType::WHILE, "Expected 'while' or 'until' after do block");
    }
    consume(TokenType::LPAREN, isUntil ? "Expected '(' after 'until'" : "Expected '(' after 'while'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after condition");
    consume(TokenType::SEMICOLON, "Expected ';' after do-while/until statement");

    if (isUntil) {
        // Negate: do { ... } until (c) => do { ... } while (!c)
        auto negated = std::make_unique<UnaryExpr>("!", std::move(condition));
        return std::make_unique<DoWhileStmt>(std::move(body), std::move(negated));
    }
    return std::make_unique<DoWhileStmt>(std::move(body), std::move(condition));
}

std::unique_ptr<Statement> Parser::parseForStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'for'");

    // Parse: for (var in start...end) or for (var in start...end...step)
    //    or: for (var in collection)  -- for-each over array
    const Token varName = consume(TokenType::IDENTIFIER, "Expected iterator variable");
    std::string iteratorType;
    if (match(TokenType::COLON)) {
        iteratorType = parseTypeAnnotation();
    } else if (inOptMaxFunction) {
        error("OPTMAX loop variables must include type annotations");
    }
    consume(TokenType::IN, "Expected 'in' after iterator variable");

    auto firstExpr = parseExpression();

    // If next token is '...' or '..', this is a range-based for loop
    if (match(TokenType::RANGE) || match(TokenType::DOT_DOT)) {
        auto end = parseExpression();

        std::unique_ptr<Expression> step = nullptr;
        if (match(TokenType::RANGE)) {
            step = parseExpression();
        }

        consume(TokenType::RPAREN, "Expected ')' after for range");
        auto body = parseStatement();

        return std::make_unique<ForStmt>(varName.lexeme, std::move(firstExpr), std::move(end), std::move(step),
                                         std::move(body), iteratorType);
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
            // Parse multiple comma-separated case values: case 1, 2, 3:
            std::vector<std::unique_ptr<Expression>> caseValues;
            caseValues.push_back(parseExpression());
            while (match(TokenType::COMMA)) {
                caseValues.push_back(parseExpression());
            }
            consume(TokenType::COLON, "Expected ':' after case value(s)");

            std::vector<std::unique_ptr<Statement>> body;
            while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) && !check(TokenType::RBRACE) && !isAtEnd()) {
                body.push_back(parseStatement());
            }
            cases.emplace_back(std::move(caseValues), std::move(body), false);
        } else if (match(TokenType::DEFAULT)) {
            if (hasDefault) {
                error("Duplicate default case in switch statement");
            }
            hasDefault = true;
            consume(TokenType::COLON, "Expected ':' after 'default'");

            std::vector<std::unique_ptr<Statement>> body;
            while (!check(TokenType::CASE) && !check(TokenType::DEFAULT) && !check(TokenType::RBRACE) && !isAtEnd()) {
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

std::unique_ptr<Statement> Parser::parseTryCatchStmt() {
    auto tryBlock = parseBlock();
    consume(TokenType::CATCH, "Expected 'catch' after try block");
    consume(TokenType::LPAREN, "Expected '(' after 'catch'");
    const Token varToken = consume(TokenType::IDENTIFIER, "Expected error variable name in catch");
    consume(TokenType::RPAREN, "Expected ')' after catch variable");
    auto catchBlock = parseBlock();
    return std::make_unique<TryCatchStmt>(std::move(tryBlock), varToken.lexeme, std::move(catchBlock));
}

std::unique_ptr<Statement> Parser::parseThrowStmt() {
    auto value = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after throw expression");
    return std::make_unique<ThrowStmt>(std::move(value));
}

// unless (condition) { ... } else { ... }
// Desugars to: if (!condition) { ... } else { ... }
std::unique_ptr<Statement> Parser::parseUnlessStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'unless'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after condition");

    auto thenBranch = parseStatement();
    std::unique_ptr<Statement> elseBranch = nullptr;

    if (match(TokenType::ELSE)) {
        elseBranch = parseStatement();
    }

    // Negate the condition: unless (c) => if (!c)
    auto negated = std::make_unique<UnaryExpr>("!", std::move(condition));
    return std::make_unique<IfStmt>(std::move(negated), std::move(thenBranch), std::move(elseBranch));
}

// until (condition) { ... }
// Desugars to: while (!condition) { ... }
std::unique_ptr<Statement> Parser::parseUntilStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'until'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after condition");

    auto body = parseStatement();

    // Negate the condition: until (c) => while (!c)
    auto negated = std::make_unique<UnaryExpr>("!", std::move(condition));
    return std::make_unique<WhileStmt>(std::move(negated), std::move(body));
}

// loop { ... }
// Desugars to: while (true) { ... }
std::unique_ptr<Statement> Parser::parseLoopStmt() {
    auto body = parseStatement();

    // Infinite loop: loop { ... } => while (true) { ... }
    auto trueVal = std::make_unique<LiteralExpr>(static_cast<long long>(1));
    return std::make_unique<WhileStmt>(std::move(trueVal), std::move(body));
}

// repeat N { ... }  or  repeat (expr) { ... }
// Desugars to: for (__repeat_i in 0...N) { ... }
std::unique_ptr<Statement> Parser::parseRepeatStmt() {
    // Allow optional parens: repeat N { ... } or repeat (N) { ... }
    bool hasParen = match(TokenType::LPAREN);
    auto count = parseExpression();
    if (hasParen) {
        consume(TokenType::RPAREN, "Expected ')' after repeat count");
    }

    auto body = parseStatement();

    // Desugar to: for (__repeat_N in 0...count) { body }
    static int repeatCounter = 0;
    std::string iterVar = "__repeat_" + std::to_string(repeatCounter++);
    auto start = std::make_unique<LiteralExpr>(static_cast<long long>(0));
    return std::make_unique<ForStmt>(iterVar, std::move(start), std::move(count), nullptr, std::move(body));
}

// defer statement;  or  defer { ... }
// Stores the statement to be executed at the end of the enclosing block
std::unique_ptr<Statement> Parser::parseDeferStmt() {
    auto body = parseStatement();
    return std::make_unique<DeferStmt>(std::move(body));
}

// guard (condition) else { ... }
// Desugars to: if (!condition) { body }
// Used for early-exit / precondition patterns:
//   guard (x > 0) else { return -1; }
std::unique_ptr<Statement> Parser::parseGuardStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'guard'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after guard condition");
    consume(TokenType::ELSE, "Expected 'else' after guard condition");

    auto elseBody = parseStatement();

    // Negate the condition: guard (c) else { ... } => if (!c) { ... }
    auto negated = std::make_unique<UnaryExpr>("!", std::move(condition));
    return std::make_unique<IfStmt>(std::move(negated), std::move(elseBody));
}

// when (expr) { val1 => { stmts }, val2, val3 => { stmts }, _ => { stmts } }
// Desugars to a switch statement with fat-arrow syntax
std::unique_ptr<Statement> Parser::parseWhenStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'when'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after when expression");
    consume(TokenType::LBRACE, "Expected '{' after when expression");

    std::vector<SwitchCase> cases;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        // Check for default case: _ => { ... }
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
            advance(); // consume '_'
            consume(TokenType::FAT_ARROW, "Expected '=>' after '_' in when clause");
            std::vector<std::unique_ptr<Statement>> body;
            body.push_back(parseStatement());
            cases.emplace_back(std::vector<std::unique_ptr<Expression>>{}, std::move(body), true);
        } else {
            // Parse one or more case values: val1, val2, val3 => { ... }
            std::vector<std::unique_ptr<Expression>> values;
            values.push_back(parseExpression());
            while (match(TokenType::COMMA)) {
                // Check if next is '=>' (end of value list)
                if (check(TokenType::FAT_ARROW)) break;
                values.push_back(parseExpression());
            }
            consume(TokenType::FAT_ARROW, "Expected '=>' after value(s) in when clause");
            std::vector<std::unique_ptr<Statement>> body;
            body.push_back(parseStatement());
            cases.emplace_back(std::move(values), std::move(body), false);
        }

        // Allow optional comma between arms
        match(TokenType::COMMA);
    }

    consume(TokenType::RBRACE, "Expected '}' to close when block");
    return std::make_unique<SwitchStmt>(std::move(condition), std::move(cases));
}

// forever { ... }
// Desugars to: while (true) { ... }
std::unique_ptr<Statement> Parser::parseForeverStmt() {
    auto body = parseStatement();

    // Infinite loop: forever { ... } => while (true) { ... }
    auto trueVal = std::make_unique<LiteralExpr>(static_cast<long long>(1));
    return std::make_unique<WhileStmt>(std::move(trueVal), std::move(body));
}

// foreach item in collection { ... }
// Desugars to: for (item in collection) { ... } (ForEachStmt)
std::unique_ptr<Statement> Parser::parseForEachStmt() {
    // Allow optional parens: foreach item in arr or foreach (item in arr)
    bool hasParen = match(TokenType::LPAREN);
    const Token varName = consume(TokenType::IDENTIFIER, "Expected iterator variable after 'foreach'");
    consume(TokenType::IN, "Expected 'in' after foreach variable");
    auto collection = parseExpression();
    if (hasParen) {
        consume(TokenType::RPAREN, "Expected ')' after foreach collection");
    }
    auto body = parseStatement();
    return std::make_unique<ForEachStmt>(varName.lexeme, std::move(collection), std::move(body));
}

// elif (condition) { ... } [elif (...) { ... }] [else { ... }]
// Desugars to: if (condition) { ... } [else if (...) { ... }] [else { ... }]
std::unique_ptr<Statement> Parser::parseElifStmt() {
    consume(TokenType::LPAREN, "Expected '(' after 'elif'");
    auto condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after elif condition");

    auto thenBranch = parseStatement();
    std::unique_ptr<Statement> elseBranch = nullptr;

    if (match(TokenType::ELIF)) {
        elseBranch = parseElifStmt();
    } else if (match(TokenType::ELSE)) {
        elseBranch = parseStatement();
    }

    return std::make_unique<IfStmt>(std::move(condition), std::move(thenBranch), std::move(elseBranch));
}

// swap a, b;
// Desugars to: { var __swap_tmp = a; a = b; b = __swap_tmp; }
std::unique_ptr<Statement> Parser::parseSwapStmt() {
    auto lhs = parseExpression();
    consume(TokenType::COMMA, "Expected ',' between swap operands");
    auto rhs = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after swap statement");

    // We need the names of the variables being swapped
    auto* lhsIdent = dynamic_cast<IdentifierExpr*>(lhs.get());
    auto* rhsIdent = dynamic_cast<IdentifierExpr*>(rhs.get());
    if (!lhsIdent || !rhsIdent) {
        error("swap operands must be variable names");
    }

    // Desugar to: { var __swap_tmp = a; a = b; b = __swap_tmp; }
    static int swapCounter = 0;
    std::string tmpName = "__swap_" + std::to_string(swapCounter++);

    std::vector<std::unique_ptr<Statement>> stmts;

    // var __swap_tmp = a;
    auto tmpInit = std::make_unique<IdentifierExpr>(lhsIdent->name);
    stmts.push_back(std::make_unique<VarDecl>(tmpName, std::move(tmpInit)));

    // a = b;
    auto assignLhs = std::make_unique<AssignExpr>(lhsIdent->name, std::make_unique<IdentifierExpr>(rhsIdent->name));
    stmts.push_back(std::make_unique<ExprStmt>(std::move(assignLhs)));

    // b = __swap_tmp;
    auto assignRhs = std::make_unique<AssignExpr>(rhsIdent->name, std::make_unique<IdentifierExpr>(tmpName));
    stmts.push_back(std::make_unique<ExprStmt>(std::move(assignRhs)));

    return std::make_unique<BlockStmt>(std::move(stmts));
}

// times (N) { ... }  or  times N { ... }
// Desugars to: for (__times_i in 0...N) { ... }
std::unique_ptr<Statement> Parser::parseTimesStmt() {
    // Allow optional parens: times N { ... } or times (N) { ... }
    bool hasParen = match(TokenType::LPAREN);
    auto count = parseExpression();
    if (hasParen) {
        consume(TokenType::RPAREN, "Expected ')' after times count");
    }

    auto body = parseStatement();

    // Desugar to: for (__times_N in 0...count) { body }
    static int timesCounter = 0;
    std::string iterVar = "__times_" + std::to_string(timesCounter++);
    auto start = std::make_unique<LiteralExpr>(static_cast<long long>(0));
    return std::make_unique<ForStmt>(iterVar, std::move(start), std::move(count), nullptr, std::move(body));
}

// var [a, b, c] = expr;  or  const [a, b, c] = expr;
// Desugars to:
//   { var __destructure_N = expr;
//     var a = __destructure_N[0];
//     var b = __destructure_N[1];
//     var c = __destructure_N[2]; }
// Use '_' to skip an element: var [a, _, c] = expr;
std::vector<std::unique_ptr<Statement>> Parser::parseDestructuringDecl(bool isConst) {
    const Token lbracket = consume(TokenType::LBRACKET, "Expected '[' for array destructuring");

    std::vector<std::string> names;
    if (!check(TokenType::RBRACKET)) {
        // First name or '_'
        if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
            advance();
            names.push_back("_");
        } else {
            const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in destructuring");
            names.push_back(name.lexeme);
        }
        while (match(TokenType::COMMA)) {
            if (check(TokenType::RBRACKET)) break; // trailing comma
            if (check(TokenType::IDENTIFIER) && peek().lexeme == "_") {
                advance();
                names.push_back("_");
            } else {
                const Token name = consume(TokenType::IDENTIFIER, "Expected variable name in destructuring");
                names.push_back(name.lexeme);
            }
        }
    }
    consume(TokenType::RBRACKET, "Expected ']' after destructuring names");

    if (names.empty()) {
        error("Destructuring must have at least one variable name");
    }

    consume(TokenType::ASSIGN, "Expected '=' after destructuring pattern");
    auto initializer = parseExpression();

    // Desugar to a block of individual assignments
    static int destructureCounter = 0;
    std::string tmpName = "__destructure_" + std::to_string(destructureCounter++);

    std::vector<std::unique_ptr<Statement>> stmts;

    // var __destructure_N = expr;
    auto tmpDecl = std::make_unique<VarDecl>(tmpName, std::move(initializer), false);
    tmpDecl->line = lbracket.line;
    tmpDecl->column = lbracket.column;
    stmts.push_back(std::move(tmpDecl));

    // var a = __destructure_N[0]; var b = __destructure_N[1]; ...
    for (size_t i = 0; i < names.size(); ++i) {
        if (names[i] == "_") continue; // skip placeholder

        auto tmpRef = std::make_unique<IdentifierExpr>(tmpName);
        auto idx = std::make_unique<LiteralExpr>(static_cast<long long>(i));
        auto indexExpr = std::make_unique<IndexExpr>(std::move(tmpRef), std::move(idx));

        auto varDecl = std::make_unique<VarDecl>(names[i], std::move(indexExpr), isConst);
        varDecl->line = lbracket.line;
        varDecl->column = lbracket.column;
        stmts.push_back(std::move(varDecl));
    }

    return stmts;
}

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
    const Token nameToken = consume(TokenType::IDENTIFIER, "Expected enum name");
    consume(TokenType::LBRACE, "Expected '{' after enum name");

    std::vector<std::pair<std::string, long long>> members;
    long long nextValue = 0;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        const Token memberToken = consume(TokenType::IDENTIFIER, "Expected enum member name");
        long long memberValue = nextValue;
        if (match(TokenType::ASSIGN)) {
            const Token valToken = consume(TokenType::INTEGER, "Expected integer value for enum member");
            memberValue = valToken.intValue;
        }
        members.push_back({memberToken.lexeme, memberValue});
        nextValue = memberValue + 1;
        // Allow optional trailing comma
        match(TokenType::COMMA);
    }

    consume(TokenType::RBRACE, "Expected '}' after enum body");
    return std::make_unique<EnumDecl>(nameToken.lexeme, std::move(members));
}

std::unique_ptr<StructDecl> Parser::parseStructDecl() {
    const Token nameToken = consume(TokenType::IDENTIFIER, "Expected struct name");
    consume(TokenType::LBRACE, "Expected '{' after struct name");

    std::vector<std::string> fields;
    std::vector<StructField> fieldDecls;
    std::vector<OperatorOverload> operators;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        // Check for operator overload: fn operator+(...) -> Type { ... }
        if (check(TokenType::FN) && current + 1 < tokens.size() &&
            tokens[current + 1].type == TokenType::IDENTIFIER &&
            tokens[current + 1].lexeme == "operator") {
            advance(); // consume 'fn'
            advance(); // consume 'operator'

            // Parse the operator symbol: +, -, *, /, ==, !=, <, >, <=, >=
            std::string opStr;
            if (check(TokenType::PLUS)) { opStr = "+"; advance(); }
            else if (check(TokenType::MINUS)) { opStr = "-"; advance(); }
            else if (check(TokenType::STAR)) { opStr = "*"; advance(); }
            else if (check(TokenType::SLASH)) { opStr = "/"; advance(); }
            else if (check(TokenType::PERCENT)) { opStr = "%"; advance(); }
            else if (check(TokenType::EQ)) { opStr = "=="; advance(); }
            else if (check(TokenType::NE)) { opStr = "!="; advance(); }
            else if (check(TokenType::LT)) { opStr = "<"; advance(); }
            else if (check(TokenType::GT)) { opStr = ">"; advance(); }
            else if (check(TokenType::LE)) { opStr = "<="; advance(); }
            else if (check(TokenType::GE)) { opStr = ">="; advance(); }
            else {
                error("Expected operator symbol after 'operator' (e.g., +, -, *, /, ==, !=, <, >)");
            }

            consume(TokenType::LPAREN, "Expected '(' after operator");
            const Token paramName = consume(TokenType::IDENTIFIER, "Expected parameter name");
            std::string paramType;
            if (match(TokenType::COLON)) {
                paramType = parseTypeAnnotation();
            }
            consume(TokenType::RPAREN, "Expected ')' after operator parameter");

            std::string returnType;
            if (match(TokenType::ARROW)) {
                returnType = parseTypeAnnotation();
            }

            // Parse the operator body as a function.
            // Synthesize a function name: __op_StructName_opname
            std::string funcName = "__op_" + nameToken.lexeme + "_" + opStr;
            // Create parameters: self (implicit) + explicit param
            std::vector<Parameter> params;
            params.emplace_back("self", nameToken.lexeme);
            params.emplace_back(paramName.lexeme, paramType);

            auto body = parseBlock();

            auto funcDecl = std::make_unique<FunctionDecl>(
                funcName, std::vector<std::string>{}, std::move(params),
                std::move(body), false, returnType);
            funcDecl->line = nameToken.line;
            funcDecl->column = nameToken.column;
            funcDecl->hintInline = true; // Operator overloads should be inlined

            OperatorOverload overload;
            overload.op = opStr;
            overload.paramName = paramName.lexeme;
            overload.paramType = paramType;
            overload.returnType = returnType;
            overload.impl = std::move(funcDecl);
            operators.push_back(std::move(overload));
            continue;
        }

        // Parse optional field attributes before the field name.
        FieldAttrs attrs;
        while ((check(TokenType::IDENTIFIER) || check(TokenType::MOVE)) && !isAtEnd()) {
            // Handle 'move' keyword as field attribute first
            if (check(TokenType::MOVE)) { attrs.isMove = true; advance(); continue; }
            const std::string& kw = peek().lexeme;
            if (kw == "hot") { attrs.hot = true; advance(); continue; }
            if (kw == "cold") { attrs.cold = true; advance(); continue; }
            if (kw == "noalias") { attrs.noalias = true; advance(); continue; }
            if (kw == "immut") { attrs.immut = true; advance(); continue; }
            if (kw == "align") {
                advance(); // consume 'align'
                consume(TokenType::LPAREN, "Expected '(' after 'align'");
                const Token val = consume(TokenType::INTEGER, "Expected integer alignment");
                attrs.align = static_cast<int>(val.intValue);
                consume(TokenType::RPAREN, "Expected ')' after alignment value");
                continue;
            }
            if (kw == "range") {
                advance(); // consume 'range'
                consume(TokenType::LPAREN, "Expected '(' after 'range'");
                const Token minVal = consume(TokenType::INTEGER, "Expected integer min");
                consume(TokenType::COMMA, "Expected ',' between range values");
                const Token maxVal = consume(TokenType::INTEGER, "Expected integer max");
                consume(TokenType::RPAREN, "Expected ')' after range values");
                attrs.hasRange = true;
                attrs.rangeMin = minVal.intValue;
                attrs.rangeMax = maxVal.intValue;
                continue;
            }
            break; // not an attribute, must be field name or type
        }

        // Parse optional type name before field name
        std::string typeName;
        // If current is an identifier and next is also an identifier, current is the type
        if (check(TokenType::IDENTIFIER) && current + 1 < tokens.size() &&
            tokens[current + 1].type == TokenType::IDENTIFIER) {
            typeName = advance().lexeme;
        }

        const Token fieldToken = consume(TokenType::IDENTIFIER, "Expected field name");
        fields.push_back(fieldToken.lexeme);
        // Skip optional type annotation (e.g. x:int) — alternative syntax
        if (match(TokenType::COLON)) {
            typeName = parseTypeAnnotation();
        }
        fieldDecls.push_back(StructField(fieldToken.lexeme, typeName, attrs));
        match(TokenType::COMMA);
    }

    consume(TokenType::RBRACE, "Expected '}' after struct body");
    structNames_.insert(nameToken.lexeme);
    auto decl = std::make_unique<StructDecl>(nameToken.lexeme, std::move(fields), std::move(fieldDecls));
    decl->operators = std::move(operators);
    return decl;
}

std::unique_ptr<Expression> Parser::parseStructLiteral(const std::string& name, int line, int col) {
    std::vector<std::pair<std::string, std::unique_ptr<Expression>>> fieldValues;

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        const Token fieldToken = consume(TokenType::IDENTIFIER, "Expected field name in struct literal");
        consume(TokenType::COLON, "Expected ':' after field name");
        auto value = parseExpression();
        fieldValues.push_back({fieldToken.lexeme, std::move(value)});
        match(TokenType::COMMA);
    }

    consume(TokenType::RBRACE, "Expected '}' after struct literal");
    auto expr = std::make_unique<StructLiteralExpr>(name, std::move(fieldValues));
    expr->line = line;
    expr->column = col;
    return expr;
}

std::unique_ptr<Statement> Parser::parseExprStmt() {
    const Token start = peek();
    auto expr = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after expression");

    auto stmt = std::make_unique<ExprStmt>(std::move(expr));
    stmt->line = start.line;
    stmt->column = start.column;
    return stmt;
}

std::unique_ptr<Expression> Parser::parseExpression() {
    const RecursionGuard guard(*this);
    return parseAssignment();
}

std::unique_ptr<Expression> Parser::parseAssignment() {
    auto expr = parsePipe();

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
            auto node = std::make_unique<IndexAssignExpr>(std::move(arrClone), std::move(idxClone), std::move(value));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else if (expr->type == ASTNodeType::FIELD_ACCESS_EXPR) {
            auto* fieldExpr = static_cast<FieldAccessExpr*>(expr.get());
            auto value = parseAssignment();
            auto objClone = std::move(fieldExpr->object);
            const std::string fieldName = fieldExpr->fieldName;
            auto node = std::make_unique<FieldAssignExpr>(std::move(objClone), fieldName, std::move(value));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else {
            error("Invalid assignment target");
        }
    }

    // Compound assignment operators: +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=
    if (match(TokenType::PLUS_ASSIGN) || match(TokenType::MINUS_ASSIGN) || match(TokenType::STAR_ASSIGN) ||
        match(TokenType::SLASH_ASSIGN) || match(TokenType::PERCENT_ASSIGN) || match(TokenType::AMPERSAND_ASSIGN) ||
        match(TokenType::PIPE_ASSIGN) || match(TokenType::CARET_ASSIGN) || match(TokenType::LSHIFT_ASSIGN) ||
        match(TokenType::RSHIFT_ASSIGN) || match(TokenType::STAR_STAR_ASSIGN) ||
        match(TokenType::NULL_COALESCE_ASSIGN)) {
        const TokenType opType = tokens[current - 1].type;
        const std::string opLexeme = tokens[current - 1].lexeme;

        // Determine the binary operator from the compound operator
        std::string binOp;
        switch (opType) {
        case TokenType::PLUS_ASSIGN:
            binOp = "+";
            break;
        case TokenType::MINUS_ASSIGN:
            binOp = "-";
            break;
        case TokenType::STAR_ASSIGN:
            binOp = "*";
            break;
        case TokenType::SLASH_ASSIGN:
            binOp = "/";
            break;
        case TokenType::PERCENT_ASSIGN:
            binOp = "%";
            break;
        case TokenType::AMPERSAND_ASSIGN:
            binOp = "&";
            break;
        case TokenType::PIPE_ASSIGN:
            binOp = "|";
            break;
        case TokenType::CARET_ASSIGN:
            binOp = "^";
            break;
        case TokenType::LSHIFT_ASSIGN:
            binOp = "<<";
            break;
        case TokenType::RSHIFT_ASSIGN:
            binOp = ">>";
            break;
        case TokenType::STAR_STAR_ASSIGN:
            binOp = "**";
            break;
        case TokenType::NULL_COALESCE_ASSIGN:
            binOp = "??";
            break;
        default:
            error("Unknown compound assignment operator: " + opLexeme);
            break;
        }

        if (expr->type == ASTNodeType::IDENTIFIER_EXPR) {
            auto idExpr = dynamic_cast<IdentifierExpr*>(expr.get());
            const std::string name = idExpr->name;
            auto rhs = parseAssignment();

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
            // Desugar: arr[i] += expr  =>  arr[i] = arr[i] + expr
            auto* indexExpr = static_cast<IndexExpr*>(expr.get());
            auto rhs = parseAssignment();

            // We need to re-read arr and i for the RHS since the originals will be moved.
            // The array and index expressions are moved into both the read and write sides.
            // We duplicate the array/index by creating fresh identifier references.
            // Note: This only works for simple array[index] patterns; the array expression
            // and index expression are AST subtrees that we cannot clone generically.
            // However, the common case is identifier[expr], which we handle here.
            auto arrClone = std::move(indexExpr->array);
            auto idxClone = std::move(indexExpr->index);

            // Build the read side: create a new IndexExpr for arr[i] on the RHS.
            // We need separate copies of the array and index expressions.
            // Since we can't clone arbitrary AST nodes, we extract identifier names
            // and rebuild the expressions. The array must be an identifier.
            // For the index, we support integer literals and identifiers.
            std::unique_ptr<Expression> arrRef2;
            std::unique_ptr<Expression> idxRef2;

            if (arrClone->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* arrId = static_cast<IdentifierExpr*>(arrClone.get());
                arrRef2 = std::make_unique<IdentifierExpr>(arrId->name);
                arrRef2->line = arrClone->line;
                arrRef2->column = arrClone->column;
            } else {
                error("Compound assignment to array elements requires a simple array variable");
            }

            if (idxClone->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* idxId = static_cast<IdentifierExpr*>(idxClone.get());
                idxRef2 = std::make_unique<IdentifierExpr>(idxId->name);
                idxRef2->line = idxClone->line;
                idxRef2->column = idxClone->column;
            } else if (idxClone->type == ASTNodeType::LITERAL_EXPR) {
                auto* litIdx = static_cast<LiteralExpr*>(idxClone.get());
                if (litIdx->literalType == LiteralExpr::LiteralType::INTEGER) {
                    idxRef2 = std::make_unique<LiteralExpr>(litIdx->intValue);
                } else {
                    error("Array index must be an integer, not a float");
                }
                idxRef2->line = idxClone->line;
                idxRef2->column = idxClone->column;
            } else {
                error("Compound assignment to array elements requires a simple index expression");
            }

            // Build: arr[i] + rhs
            auto readExpr = std::make_unique<IndexExpr>(std::move(arrRef2), std::move(idxRef2));
            readExpr->line = expr->line;
            readExpr->column = expr->column;
            auto binExpr = std::make_unique<BinaryExpr>(binOp, std::move(readExpr), std::move(rhs));
            binExpr->line = expr->line;
            binExpr->column = expr->column;

            // Build: arr[i] = (arr[i] + rhs)
            auto node = std::make_unique<IndexAssignExpr>(std::move(arrClone), std::move(idxClone), std::move(binExpr));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else if (expr->type == ASTNodeType::FIELD_ACCESS_EXPR) {
            // Desugar: s.field += expr  =>  s.field = s.field + expr
            auto* fieldExpr = static_cast<FieldAccessExpr*>(expr.get());
            auto rhs = parseAssignment();

            auto objClone = std::move(fieldExpr->object);
            const std::string fieldName = fieldExpr->fieldName;

            // Build the read side: create a new FieldAccessExpr for s.field on the RHS.
            // The object must be a simple identifier so we can duplicate it.
            std::unique_ptr<Expression> objRef2;
            if (objClone->type == ASTNodeType::IDENTIFIER_EXPR) {
                auto* objId = static_cast<IdentifierExpr*>(objClone.get());
                objRef2 = std::make_unique<IdentifierExpr>(objId->name);
                objRef2->line = objClone->line;
                objRef2->column = objClone->column;
            } else {
                error("Compound assignment to struct fields requires a simple struct variable (e.g., 's.x += 1')");
            }

            // Build: s.field + rhs
            auto readExpr = std::make_unique<FieldAccessExpr>(std::move(objRef2), fieldName);
            readExpr->line = expr->line;
            readExpr->column = expr->column;
            auto binExpr = std::make_unique<BinaryExpr>(binOp, std::move(readExpr), std::move(rhs));
            binExpr->line = expr->line;
            binExpr->column = expr->column;

            // Build: s.field = (s.field + rhs)
            auto node = std::make_unique<FieldAssignExpr>(std::move(objClone), fieldName, std::move(binExpr));
            node->line = expr->line;
            node->column = expr->column;
            return node;
        } else {
            error("Compound assignment (e.g., '+=') is only supported on variables, array elements (arr[i]), and struct fields (s.x)");
        }
    }

    return expr;
}

std::unique_ptr<Expression> Parser::parseTernary() {
    auto expr = parseNullCoalesce();

    if (match(TokenType::QUESTION)) {
        const Token questionToken = tokens[current - 1];
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

std::unique_ptr<Expression> Parser::parseNullCoalesce() {
    auto left = parseLogicalOr();

    while (match(TokenType::NULL_COALESCE)) {
        auto right = parseLogicalOr();
        // x ?? y  desugars to  x != 0 ? x : y  (ternary expression)
        // We clone the left expression by re-wrapping it
        auto node = std::make_unique<BinaryExpr>("??", std::move(left), std::move(right));
        left = std::move(node);
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseLogicalOr() {
    auto left = parseLogicalAnd();

    while (match(TokenType::OR)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseLogicalAnd();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseLogicalAnd() {
    auto left = parseBitwiseOr();

    while (match(TokenType::AND)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseBitwiseOr();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseBitwiseOr() {
    auto left = parseBitwiseXor();

    while (match(TokenType::PIPE)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseBitwiseXor();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseBitwiseXor() {
    auto left = parseBitwiseAnd();

    while (match(TokenType::CARET)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseBitwiseAnd();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseBitwiseAnd() {
    auto left = parseEquality();

    while (match(TokenType::AMPERSAND)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseEquality();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseEquality() {
    auto left = parseComparison();

    while (match(TokenType::EQ) || match(TokenType::NE)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseComparison();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseComparison() {
    auto left = parseShift();

    while (match(TokenType::LT) || match(TokenType::LE) || match(TokenType::GT) || match(TokenType::GE)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseShift();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseShift() {
    auto left = parseAddition();

    while (match(TokenType::LSHIFT) || match(TokenType::RSHIFT)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseAddition();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseAddition() {
    auto left = parseMultiplication();

    while (match(TokenType::PLUS) || match(TokenType::MINUS)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parseMultiplication();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseMultiplication() {
    auto left = parsePower();

    while (match(TokenType::STAR) || match(TokenType::SLASH) || match(TokenType::PERCENT)) {
        const std::string op = tokens[current - 1].lexeme;
        auto right = parsePower();
        left = std::make_unique<BinaryExpr>(op, std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parsePower() {
    auto left = parseUnary();

    if (match(TokenType::STAR_STAR)) {
        // Right-associative: 2 ** 3 ** 2 = 2 ** (3 ** 2) = 2 ** 9 = 512
        auto right = parsePower();
        left = std::make_unique<BinaryExpr>("**", std::move(left), std::move(right));
    }

    return left;
}

std::unique_ptr<Expression> Parser::parseUnary() {
    if (match(TokenType::MINUS) || match(TokenType::NOT) || match(TokenType::TILDE)) {
        const Token opToken = tokens[current - 1];
        auto operand = parseUnary();
        auto node = std::make_unique<UnaryExpr>(opToken.lexeme, std::move(operand));
        node->line = opToken.line;
        node->column = opToken.column;
        return node;
    }

    // `&expr` — address-of / borrow operator (e.g. `&x` in `borrow var j:&i32 = &x;`)
    if (match(TokenType::AMPERSAND)) {
        const Token opToken = tokens[current - 1];
        auto operand = parseUnary();
        auto node = std::make_unique<UnaryExpr>(opToken.lexeme, std::move(operand));
        node->line = opToken.line;
        node->column = opToken.column;
        return node;
    }

    if (match(TokenType::PLUSPLUS) || match(TokenType::MINUSMINUS)) {
        const Token opToken = tokens[current - 1];
        auto operand = parseUnary();
        if (operand->type != ASTNodeType::IDENTIFIER_EXPR && operand->type != ASTNodeType::INDEX_EXPR) {
            error("Prefix " + opToken.lexeme + " requires an lvalue operand");
        }
        auto node = std::make_unique<PrefixExpr>(opToken.lexeme, std::move(operand));
        node->line = opToken.line;
        node->column = opToken.column;
        return node;
    }

    // `move <expr>` as a prefix expression (e.g. `return move x;`, `a = move b;`)
    if (match(TokenType::MOVE)) {
        const Token kw = tokens[current - 1];
        auto operand = parseUnary();
        auto node = std::make_unique<MoveExpr>(std::move(operand));
        node->line = kw.line;
        node->column = kw.column;
        return node;
    }

    return parsePostfix();
}

std::unique_ptr<Expression> Parser::parsePostfix() {
    auto expr = parseCall();

    while (true) {
        // Handle postfix operators
        if (match(TokenType::PLUSPLUS) || match(TokenType::MINUSMINUS)) {
            const Token opToken = tokens[current - 1];
            if (expr->type != ASTNodeType::IDENTIFIER_EXPR && expr->type != ASTNodeType::INDEX_EXPR) {
                error("Postfix " + opToken.lexeme + " requires an lvalue operand");
            }
            expr = std::make_unique<PostfixExpr>(opToken.lexeme, std::move(expr));
            expr->line = opToken.line;
            expr->column = opToken.column;
        }
        // Handle array indexing
        else if (match(TokenType::LBRACKET)) {
            const Token bracketToken = tokens[current - 1];
            auto index = parseExpression();
            consume(TokenType::RBRACKET, "Expected ']' after array index");
            auto indexExpr = std::make_unique<IndexExpr>(std::move(expr), std::move(index));
            indexExpr->line = bracketToken.line;
            indexExpr->column = bracketToken.column;
            expr = std::move(indexExpr);
        }
        // Handle field access (dot notation)
        else if (match(TokenType::DOT)) {
            const Token dotToken = tokens[current - 1];
            const Token fieldToken = consume(TokenType::IDENTIFIER, "Expected field name after '.'");
            auto fieldExpr = std::make_unique<FieldAccessExpr>(std::move(expr), fieldToken.lexeme);
            fieldExpr->line = dotToken.line;
            fieldExpr->column = dotToken.column;
            expr = std::move(fieldExpr);
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
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(token.intValue);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    if (match(TokenType::FLOAT)) {
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(token.floatValue);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    if (match(TokenType::STRING)) {
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(token.lexeme);
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    if (match(TokenType::TRUE)) {
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(static_cast<long long>(1));
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    if (match(TokenType::FALSE) || match(TokenType::NULL_LITERAL)) {
        const Token token = tokens[current - 1];
        auto expr = std::make_unique<LiteralExpr>(static_cast<long long>(0));
        expr->line = token.line;
        expr->column = token.column;
        return expr;
    }

    if (match(TokenType::IDENTIFIER)) {
        const Token token = tokens[current - 1];
        // Check if this is a struct literal: StructName { field: value, ... }
        if (structNames_.count(token.lexeme) && check(TokenType::LBRACE)) {
            advance(); // consume '{'
            return parseStructLiteral(token.lexeme, token.line, token.column);
        }
        auto expr = std::make_unique<IdentifierExpr>(token.lexeme);
        expr->line = token.line;
        expr->column = token.column;
        // Consume optional :type annotation on identifier in expression context
        // (e.g. x:u32 in function call arguments, assignments, range bounds).
        // Only consume when the type name is a known type to avoid ambiguity
        // with ternary operator colons and other colon uses.
        if (check(TokenType::COLON) && current + 1 < tokens.size() &&
            tokens[current + 1].type == TokenType::IDENTIFIER &&
            isKnownTypeName(tokens[current + 1].lexeme)) {
            advance(); // consume ':'
            advance(); // consume type name
        }
        return expr;
    }

    // Allow keyword tokens that double as builtin function names to be used
    // in expression context (e.g. swap(arr, i, j) as a function call).
    if (match(TokenType::SWAP)) {
        const Token token = tokens[current - 1];
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
        const Token bracketToken = tokens[current - 1];
        auto arrayExpr = parseArrayLiteral();
        arrayExpr->line = bracketToken.line;
        arrayExpr->column = bracketToken.column;
        return arrayExpr;
    }

    // Lambda expression: |params| body
    if (match(TokenType::PIPE)) {
        return parseLambda();
    }

    // Dict literal: {"key": val, "key2": val2, ...}
    // Desugared at parse time into a DictExpr for zero-cost codegen
    // (single malloc + direct stores, no loops).
    // Note: works in expression context; standalone dict-statement uses LBRACE
    // which the statement-level parser already routes to block parsing.
    if (match(TokenType::LBRACE)) {
        const Token braceToken = tokens[current - 1];
        std::vector<std::pair<std::unique_ptr<Expression>, std::unique_ptr<Expression>>> pairs;
        if (!check(TokenType::RBRACE)) {
            do {
                // Allow trailing comma before '}'
                if (check(TokenType::RBRACE)) break;
                auto key = parseExpression();
                consume(TokenType::COLON, "Expected ':' after dict key");
                auto val = parseExpression();
                pairs.emplace_back(std::move(key), std::move(val));
            } while (match(TokenType::COMMA));
        }
        consume(TokenType::RBRACE, "Expected '}' after dict literal");
        auto node = std::make_unique<DictExpr>(std::move(pairs));
        node->line = braceToken.line;
        node->column = braceToken.column;
        return node;
    }

    // Lambda with || (empty params): || body
    if (match(TokenType::OR)) {
        // || is the empty-parameter lambda: || expr
        const Token orToken = tokens[current - 1];
        auto body = parseExpression();
        auto node = std::make_unique<LambdaExpr>(std::vector<std::string>{}, std::move(body));
        node->line = orToken.line;
        node->column = orToken.column;

        // Desugar to named function
        const std::string lambdaName = "__lambda_" + std::to_string(lambdaCounter_++);
        std::vector<Parameter> fnParams;
        auto returnStmt = std::make_unique<ReturnStmt>(std::move(node->body));
        returnStmt->line = orToken.line;
        returnStmt->column = orToken.column;
        std::vector<std::unique_ptr<Statement>> stmts;
        stmts.push_back(std::move(returnStmt));
        auto block = std::make_unique<BlockStmt>(std::move(stmts));
        block->line = orToken.line;
        block->column = orToken.column;
        auto fnDecl = std::make_unique<FunctionDecl>(lambdaName, std::vector<std::string>{}, std::move(fnParams), std::move(block));
        fnDecl->line = orToken.line;
        fnDecl->column = orToken.column;
        lambdaFunctions_.push_back(std::move(fnDecl));

        auto nameLit = std::make_unique<LiteralExpr>(lambdaName);
        nameLit->line = orToken.line;
        nameLit->column = orToken.column;
        return nameLit;
    }

    error("Expected expression");
    return nullptr;
}

std::unique_ptr<Expression> Parser::parseArrayLiteral() {
    std::vector<std::unique_ptr<Expression>> elements;

    if (!check(TokenType::RBRACKET)) {
        do {
            // Handle spread operator: ...expr
            if (match(TokenType::RANGE)) {
                const Token spreadToken = tokens[current - 1];
                auto operand = parseExpression();
                auto node = std::make_unique<SpreadExpr>(std::move(operand));
                node->line = spreadToken.line;
                node->column = spreadToken.column;
                elements.push_back(std::move(node));
            } else {
                elements.push_back(parseExpression());
            }
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::RBRACKET, "Expected ']' after array elements");

    return std::make_unique<ArrayExpr>(std::move(elements));
}

std::unique_ptr<Expression> Parser::parsePipe() {
    auto expr = parseTernary();

    while (match(TokenType::PIPE_FORWARD)) {
        const Token pipeToken = tokens[current - 1];
        const Token fnName = consume(TokenType::IDENTIFIER, "Expected function name after '|>'");
        auto node = std::make_unique<PipeExpr>(std::move(expr), fnName.lexeme);
        node->line = pipeToken.line;
        node->column = pipeToken.column;
        expr = std::move(node);
    }

    return expr;
}

std::unique_ptr<Expression> Parser::parseLambda() {
    const Token pipeToken = tokens[current - 1]; // the opening |
    std::vector<std::string> params;

    // Parse lambda parameters: |x| or |x, y| or || or |x:int| or |x:int, y:int|
    if (!check(TokenType::PIPE)) {
        do {
            const Token paramName = consume(TokenType::IDENTIFIER, "Expected parameter name in lambda");
            params.push_back(paramName.lexeme);
            // Skip optional type annotation (e.g. |x:int|)
            if (match(TokenType::COLON)) {
                parseTypeAnnotation();
            }
        } while (match(TokenType::COMMA));
    }

    consume(TokenType::PIPE, "Expected '|' after lambda parameters");

    // Parse the lambda body expression
    auto body = parseExpression();

    auto node = std::make_unique<LambdaExpr>(std::move(params), std::move(body));
    node->line = pipeToken.line;
    node->column = pipeToken.column;

    // Desugar: generate a named function and return its name as a string literal
    const std::string lambdaName = "__lambda_" + std::to_string(lambdaCounter_++);

    // Create the function: fn __lambda_N(params...) { return body; }
    std::vector<Parameter> fnParams;
    for (const auto& p : node->params) {
        fnParams.push_back(Parameter(p));
    }
    auto returnStmt = std::make_unique<ReturnStmt>(std::move(node->body));
    returnStmt->line = pipeToken.line;
    returnStmt->column = pipeToken.column;
    std::vector<std::unique_ptr<Statement>> stmts;
    stmts.push_back(std::move(returnStmt));
    auto block = std::make_unique<BlockStmt>(std::move(stmts));
    block->line = pipeToken.line;
    block->column = pipeToken.column;
    auto fnDecl = std::make_unique<FunctionDecl>(lambdaName, std::vector<std::string>{}, std::move(fnParams), std::move(block));
    fnDecl->line = pipeToken.line;
    fnDecl->column = pipeToken.column;

    lambdaFunctions_.push_back(std::move(fnDecl));

    // Return the lambda name as a string literal (for use with array_map, etc.)
    auto nameLit = std::make_unique<LiteralExpr>(lambdaName);
    nameLit->line = pipeToken.line;
    nameLit->column = pipeToken.column;
    return nameLit;
}

} // namespace omscript
