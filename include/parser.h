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
#include "diagnostic.h"
#include "lexer.h"
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omscript {

class Parser {
  public:
    Parser(const std::vector<Token>& tokens);
    Parser(std::vector<Token>&& tokens);
    [[nodiscard]] std::unique_ptr<Program> parse();

    /// Set the base directory for resolving import paths.
    void setBaseDir(const std::string& dir) {
        baseDir_ = dir;
    }

    /// Set the source file name, making it available as the built-in `FILE` comptime string.
    void setSourceFile(const std::string& file) {
        currentFile_ = file;
    }

    /// Inject an integer comptime constant before parsing begins.
    /// Equivalent to prepending `comptime { const name: int = val; }` to the source.
    /// Used to pass -D NAME[=VALUE] CLI flags.
    void setComptimeInt(const std::string& name, long long val) {
        comptimeConstants_[name] = val;
    }

    /// Inject a string comptime constant before parsing begins.
    void setComptimeString(const std::string& name, const std::string& val) {
        comptimeStrings_[name] = val;
    }

    /// Returns collected parse errors (populated when multi-error mode is active).
    [[nodiscard]] const std::vector<std::string>& errors() const noexcept {
        return errors_;
    }

    /// Returns structured diagnostics for all parse errors collected so far.
    /// Preserves source location (line, column) and error codes, enabling rich
    /// diagnostic display (source snippets, colors) by the compiler driver.
    [[nodiscard]] const std::vector<omscript::Diagnostic>& diagnostics() const noexcept {
        return diagnostics_;
    }

    /// Returns non-fatal parse warnings (e.g. conflicting annotations).
    /// Populated alongside errors() during parse().
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept {
        return warnings_;
    }

  private:
    std::vector<Token> tokens;
    size_t current;
    bool inOptMaxFunction;
    std::vector<std::string> errors_;
    std::vector<omscript::Diagnostic> diagnostics_; ///< Structured errors with source locations.
    std::vector<std::string> warnings_;
    int lambdaCounter_ = 0;
    int recursionDepth_ = 0;

    /// Set to true by parsePrimary() when it resolves a namespace-qualified
    /// call (e.g. std::abs).  Read and immediately cleared by parseCall() to
    /// set CallExpr::fromStdNamespace on the resulting node.
    bool lastCallWasNsResolved_ = false;

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

    /// Source file name — exposed as the built-in `FILE` comptime string constant.
    std::string currentFile_;

    /// Set of already-imported file paths (prevents circular imports).
    std::shared_ptr<std::unordered_set<std::string>> importedFiles_;

    /// Parse an import statement and return the imported program.
    void parseImport(std::vector<std::unique_ptr<FunctionDecl>>& functions,
                     std::vector<std::unique_ptr<EnumDecl>>& enums, std::vector<std::unique_ptr<StructDecl>>& structs,
                     std::vector<std::unique_ptr<VarDecl>>& globals);

    /// Parse a user-defined namespace block: namespace Name { fn/struct/enum/namespace ... }
    /// @param nsPrefix  Fully-qualified name of the enclosing namespace, or empty string at
    ///                  the top level.  Used to form qualified names for nested namespaces.
    void parseNamespace(std::vector<std::unique_ptr<FunctionDecl>>& functions,
                        std::vector<std::unique_ptr<EnumDecl>>& enums,
                        std::vector<std::unique_ptr<StructDecl>>& structs,
                        std::vector<std::unique_ptr<VarDecl>>& globals,
                        const std::string& nsPrefix = "");

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
    std::unique_ptr<Statement> parseVarDeclWithInheritedType(bool isConst, const std::string& inheritedType);
    std::unique_ptr<Statement> parseIfStmt();
    std::unique_ptr<Statement> parseWhileStmt();
    std::unique_ptr<Statement> parseDoWhileStmt();
    std::unique_ptr<Statement> parseForStmt();
    std::unique_ptr<Statement> parseReturnStmt();
    std::unique_ptr<Statement> parseBreakStmt();
    std::unique_ptr<Statement> parseContinueStmt();
    std::unique_ptr<Statement> parseJmpStmt();
    std::unique_ptr<Statement> parseLabelStmt();
    std::unique_ptr<Statement> parseSwitchStmt();
    std::unique_ptr<Statement> parseCatchStmt();
    std::unique_ptr<Statement> parseThrowStmt();
    std::unique_ptr<Statement> parseEnsureStmt();
    std::unique_ptr<Statement> parseUnlessStmt();
    std::unique_ptr<Statement> parseUntilStmt();
    std::unique_ptr<Statement> parseLoopStmt();
    std::unique_ptr<Statement> parseRepeatStmt();
    std::unique_ptr<Statement> parseDeferStmt();
    std::unique_ptr<Statement> parseGuardStmt();
    std::unique_ptr<Statement> parseWhenStmt();
    std::unique_ptr<Expression> parseWhenExpr();
    std::unique_ptr<Statement> parseForeverStmt();
    std::unique_ptr<Statement> parseForEachStmt();
    std::unique_ptr<Statement> parseAssumeStmt();
    OptMaxConfig parseOptMaxConfig();
    LoopConfig parseLoopAnnotation();
    std::unique_ptr<Statement> parseElifStmt();
    std::unique_ptr<Statement> parseSwapStmt();
    std::unique_ptr<Statement> parseTimesStmt();
    std::unique_ptr<Statement> parseWithStmt();
    std::unique_ptr<Statement> parsePipelineStmt();
    std::vector<std::unique_ptr<Statement>> parseDestructuringDecl(bool isConst);
    std::vector<std::unique_ptr<Statement>> parseTupleDestructuringDecl(bool isConst);
    std::unique_ptr<Statement> parseTupleDestrAssign();
    std::unique_ptr<EnumDecl> parseEnumDecl();
    std::unique_ptr<StructDecl> parseStructDecl(StructRepr repr = StructRepr::Auto, int reprAlignN = 0, const std::string& forcedName = "");
    void parseImplBlock(std::vector<std::unique_ptr<FunctionDecl>>& functions);
    std::unique_ptr<Expression> parseStructLiteral(const std::string& name, int line, int col);
    std::unique_ptr<Statement> parseExprStmt();
    std::unique_ptr<VarDecl> parseGlobalDecl();

    /// Known struct names for parsing struct literals.
    std::unordered_set<std::string> structNames_;

    /// Compile-time integer constants defined by top-level `comptime { const X = val; }` blocks.
    /// Used to resolve constant references in type annotations (@loop(unroll=N), u64x{N}, etc.)
    std::unordered_map<std::string, long long> comptimeConstants_;

    /// Compile-time string constants defined by top-level `comptime { const X: string = "v"; }`
    /// blocks.  Kept separately so the evaluator can distinguish int-valued from string-valued
    /// comptime vars when resolving `if (COND)` branches.
    std::unordered_map<std::string, std::string> comptimeStrings_;

    /// Type aliases defined by top-level `type X = Y` declarations.
    /// Used to resolve type names in variable declarations and function signatures.
    std::unordered_map<std::string, std::string> typeAliases_;

    /// Custom operator symbols registered from struct operator definitions.
    /// Used by tryMatchCustomOperator() to recognize multi-token operators
    /// (e.g. "<=>" defined as fn operator<=>(other: T)) at expression-parse time.
    std::set<std::string> customOperatorSymbols_;

    /// importedGlobalVars_[alias][varName] = mangledName
    /// Populated when an import brings in global variables.
    /// mangledName = alias + "__" + varName
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> importedGlobalVars_;

    /// Globals from imported files, drained into Program by parse().
    std::vector<std::unique_ptr<VarDecl>> pendingGlobals_;

    /// Pre-scan all tokens for `operator SYMBOL (` patterns and populate
    /// customOperatorSymbols_ before the main parse pass.
    void prescanCustomOperators();
    void prescanFunctionParams();

    /// Try to match a registered custom operator at the current token position.
    /// Returns the longest matching operator symbol string, or "" if none match.
    std::string tryMatchCustomOperator() const;

    /// Return the number of tokens consumed by the given custom operator symbol
    /// starting at the current position.
    size_t customOpTokenCount(const std::string& opSym) const;

    /// True when the token(s) at the current position form the start of a
    /// custom operator that is strictly longer than `standardLen` chars.
    bool isStartOfLongerCustomOp(size_t standardLen) const;

    /// Known enum names for scope resolution validation.
    std::unordered_set<std::string> enumNames_;

    /// Maps bare enum short-names to their qualified names when enums are declared
    /// inside a namespace block.  e.g.  "Code" → "Status::Code" after:
    ///   namespace Status { enum Code { OK, ERR } }
    /// Used to resolve `Code::OK` as `ScopeResolutionExpr("Status::Code", "OK")`.
    std::unordered_map<std::string, std::string> bareEnumNames_;

    /// Parameter names for user-defined functions, populated during the
    /// forward-declaration pre-scan pass.  Used to resolve named call
    /// arguments: foo(height: 3, width: 4) → reorder to match decl order.
    std::unordered_map<std::string, std::vector<std::string>> funcParamNames_;

    /// Namespace registry populated by 'import "file" as alias' statements.
    ///
    /// importNamespaces_[alias][funcName] = actualFuncName
    ///
    /// The alias key uses '__' as separator for multi-level aliases:
    ///   import "file" as john::int  →  key "john__int"
    ///
    /// At call sites, a::b::c(args) is resolved via longest-prefix matching:
    /// try "a__b" → c, then "a" → b__c, and use the first hit.
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> importNamespaces_;

    /// Namespaces that have been globally imported via `import std;` (identifier form).
    /// Members of these namespaces are accessible without the `namespace::` qualifier.
    std::unordered_set<std::string> globallyImportedNamespaces_;

    /// Bare-import map: populated when `import NSName;` is processed for user-defined
    /// namespaces. Maps unqualified name → fully-qualified name for namespace members
    /// (for example functions and structs), so `add(x)` or `Vec2 { ... }` after
    /// `import Math;` resolve to `Math::add` and `Math::Vec2`.
    std::unordered_map<std::string, std::string> bareImportedNames_;

    /// Resolve a scope chain (segments separated by ::) to an actual function
    /// name using the importNamespaces_ registry.
    ///
    /// Tries longest-prefix namespace matching.  Returns the resolved function
    /// name on success.  Calls error() if a namespace is found but the function
    /// is absent.  Returns an empty string when no namespace prefix matches
    /// (caller emits a hard error for unrecognised namespaces).
    std::string resolveNamespacedPath(const std::vector<std::string>& segments);

    /// Pre-register the built-in `std` namespace in importNamespaces_.
    /// Called from both constructors so every Parser instance has `std::` available
    /// without requiring an explicit import statement.
    void registerStdNamespace();

    std::unique_ptr<Expression> parseExpression();
    std::unique_ptr<Expression> parseAssignment();
    std::unique_ptr<Expression> parseTernary();
    std::unique_ptr<Expression> parseNullCoalesce();
    std::unique_ptr<Expression> parseCustomOp(); // user-defined arbitrary-symbol operators
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
    std::unique_ptr<Expression> parseCast();
    std::unique_ptr<Expression> parseUnary();
    std::unique_ptr<Expression> parsePostfix();
    std::unique_ptr<Expression> parseCall();
    std::unique_ptr<Expression> parsePrimary();
    std::unique_ptr<Expression> parseArrayLiteral();
    std::unique_ptr<Expression> parseLambda();
    std::unique_ptr<Expression> parseArrowLambda(const std::vector<std::string>& params,
                                                  const std::vector<std::string>& paramTypes, const Token& arrowTok);
    std::unique_ptr<Expression> parseSwitchExpr();
    std::unique_ptr<Expression> parsePipe();

    /// Returns true when the current token is '(' and the matching ')'
    /// is immediately followed by '=>' (FAT_ARROW) — indicating an arrow lambda.
    bool isArrowLambdaParens() const;
    bool isTupleDestrAssign() const;

    /// Parse a type annotation (e.g. "int", "int[]", "string[][]", "Point").
    std::string parseTypeAnnotation();

    [[noreturn]] void error(const std::string& message);
};

} // namespace omscript

#endif // PARSER_H
