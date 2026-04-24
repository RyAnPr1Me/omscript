#pragma once

#ifndef PREPROCESSOR_H
#define PREPROCESSOR_H

/// @file preprocessor.h
/// @brief Source-level preprocessor for OmScript.
///
/// Runs before the lexer and handles:
///   - Standard: #define, #undef, #ifdef, #ifndef, #if, #elif, #else, #endif,
///               #error, #warning
///   - Predefined macros: __FILE__, __LINE__, __VERSION__, __OS__, __ARCH__,
///                        __COUNTER__
///   - New (not in C): #info, #assert, #require

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace omscript {

class Preprocessor {
  public:
    /// @param filename  Source file name, used in diagnostics and __FILE__.
    explicit Preprocessor(std::string filename = "<input>");

    /// Run the preprocessor on @p source and return the processed text.
    /// Throws DiagnosticError on #error / failed #assert / #require.
    std::string process(const std::string& source);

    /// Warnings accumulated during processing (from #warning / #info).
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }

    /// Perform macro substitution on @p text (not a directive line).
    /// Public so ExprEval can call it; declared const.
    std::string substituteMacros(const std::string& text, int lineNo,
                                  int depth = 0) const;

    /// The macro definitions (exposed for ExprEval::defined() check).
    struct MacroDef {
        bool isFunctionLike = false;
        bool isCounter      = false; ///< auto-increments on use
        std::vector<std::string> params;
        std::string body;
        mutable int counterValue = 0;
        // ── Type-awareness fields (all optional; default = "untyped") ──
        /// Per-parameter type annotation, parallel to `params`.  Empty
        /// string means "any" (no checking).  Recognised values: "int",
        /// "uint", "float", "string", "bool", "any".
        std::vector<std::string> paramTypes;
        /// Return type annotation (informational only — no checking).
        std::string returnType;
        // ── Safety fields ──
        /// True for macros installed by the Preprocessor itself (predefs
        /// like __FILE__, __SIMD_AVX2__, __VECTOR_WIDTH__, …).  Reserved
        /// macros cannot be redefined or undefined by user code.
        bool isReserved = false;
    };

    const std::unordered_map<std::string, MacroDef>& macroMap() const noexcept { return macros_; }

  private:
    std::string filename_;
    std::unordered_map<std::string, MacroDef> macros_;
    std::vector<std::string> warnings_;
    int globalCounter_ = 0; ///< backing store for __COUNTER__
    /// Set of macros currently being expanded — used by substituteMacros
    /// to detect cycles like `#define A B` / `#define B A` and surface a
    /// proper diagnostic instead of silently truncating at the depth
    /// limit.  Mutable because substituteMacros / expandSimple are const.
    mutable std::unordered_set<std::string> expanding_;

    void handleDefine(const std::string& rest, int lineNo);

    std::string expandSimple(const std::string& name, int lineNo,
                              int depth) const;

    static std::vector<std::string> collectArgs(const std::string& text,
                                                 size_t& pos);

    long long evalExpr(const std::string& expr, int lineNo) const;

    static int cmpVersion(const std::string& a, const std::string& b);

    static bool isIdentStart(char c) noexcept;
    static bool isIdentChar(char c)  noexcept;
    static std::string trimLeft(const std::string& s);
    static std::string trim(const std::string& s);

    /// Lightweight argument-shape classifier used by typed function-like
    /// macros.  Returns one of "int", "float", "string", "bool",
    /// "ident", "expr", or "" (empty/unknown).  The result is compared
    /// against the declared param type in `validateMacroArgs`.
    static std::string classifyArg(const std::string& a);

    /// Validate function-like macro invocation: arity + per-arg types.
    /// Throws DiagnosticError on mismatch.  Adds warnings for footguns
    /// like multi-evaluation of a function-call argument.
    void validateMacroCall(const std::string& name, const MacroDef& def,
                           const std::vector<std::string>& args,
                           int lineNo) const;
};

} // namespace omscript

#endif // PREPROCESSOR_H
