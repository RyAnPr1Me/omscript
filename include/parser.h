#pragma once

#ifndef PARSER_H
#define PARSER_H

/// @file parser.h
/// @brief Recursive-descent parser for OmScript.
///
/// The Parser consumes a token stream produced by the Lexer and builds an
/// AST (see ast.h).  It supports operator precedence, type annotations,
/// generics, and structured error recovery.

#include "ast.h"
#include "lexer.h"
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace omscript {

class Parser {
  public:
    Parser(const std::vector<Token>& tokens);
    Parser(std::vector<Token>&& tokens);
    [[nodiscard]] std::unique_ptr<Program> parse();

    /// Set the base directory for resolving import paths.
    void setBaseDir(const std::string& dir) { baseDir_ = dir; }

    /// Returns collected parse errors (populated when multi-error mode is active).
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept {
        return errors_;
    }

  private:
    std::vector<Token> tokens;
    size_t current;
    bool inOptMaxFunction;
    std::vector<std::string> errors_;
    int lambdaCounter_ = 0;
    int recursionDepth_ = 0;

    /// Maximum allowed parser recursion depth to prevent stack overflow on
    /// adversarial or deeply-nested input.
    static constexpr int kMaxRecursionDepth = 256;

    /// RAII guard that increments recursionDepth_ on construction and
    /// decrements it on destruction.  Throws if the limit is exceeded.
    class RecursionGuard {
      public:
        explicit RecursionGuard(Parser& p) : parser_(p) {
            if (++parser_.recursionDepth_ > kMaxRecursionDepth) {
                parser_.error("Maximum nesting depth exceeded (limit " + std::to_string(kMaxRecursionDepth) + ")");
            }
        }
        ~RecursionGuard() {
            --parser_.recursionDepth_;
        }
        RecursionGuard(const RecursionGuard&) = delete;
        RecursionGuard& operator=(const RecursionGuard&) = delete;

      private:
        Parser& parser_;
    };

    /// Generated lambda functions to be appended to the program.
    std::vector<std::unique_ptr<FunctionDecl>> lambdaFunctions_;

    /// Base directory for resolving import paths.
    std::string baseDir_;

    /// Set of already-imported file paths (prevents circular imports).
    std::shared_ptr<std::unordered_set<std::string>> importedFiles_;

    /// Parse an import statement and return the imported program.
    void parseImport(std::vector<std::unique_ptr<FunctionDecl>>& functions,
                     std::vector<std::unique_ptr<EnumDecl>>& enums,
                     std::vector<std::unique_ptr<StructDecl>>& structs);

    const Token& peek(int offset = 0) const noexcept;
    Token advance() noexcept;
    [[nodiscard]] bool check(TokenType type) const noexcept;
    bool match(TokenType type) noexcept;
    [[nodiscard]] bool isAtEnd() const noexcept;
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
    std::unique_ptr<Statement> parseUnlessStmt();
    std::unique_ptr<Statement> parseUntilStmt();
    std::unique_ptr<Statement> parseLoopStmt();
    std::unique_ptr<Statement> parseRepeatStmt();
    std::unique_ptr<Statement> parseDeferStmt();
    std::unique_ptr<Statement> parseGuardStmt();
    std::unique_ptr<Statement> parseWhenStmt();
    std::unique_ptr<Statement> parseForeverStmt();
    std::unique_ptr<Statement> parseForEachStmt();
    std::unique_ptr<Statement> parseElifStmt();
    std::unique_ptr<Statement> parseSwapStmt();
    std::unique_ptr<Statement> parseTimesStmt();
    std::unique_ptr<EnumDecl> parseEnumDecl();
    std::unique_ptr<StructDecl> parseStructDecl();
    std::unique_ptr<Expression> parseStructLiteral(const std::string& name, int line, int col);
    std::unique_ptr<Statement> parseExprStmt();

    /// Known struct names for parsing struct literals.
    std::unordered_set<std::string> structNames_;

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

    /// Parse a type annotation (e.g. "int", "int[]", "string[][]", "Point").
    std::string parseTypeAnnotation();

    [[noreturn]] void error(const std::string& message);
};

} // namespace omscript

#endif // PARSER_H
