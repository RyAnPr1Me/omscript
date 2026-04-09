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
#include <vector>

namespace omscript {

class Preprocessor {
  public:
    /// @param filename  Source file name, used in diagnostics and __FILE__.
    explicit Preprocessor(std::string filename = "<input>");

    /// Run the preprocessor on @p source and return the processed text.
    /// Throws std::runtime_error on #error / failed #assert / #require.
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
    };

    const std::unordered_map<std::string, MacroDef>& macroMap() const noexcept { return macros_; }

  private:
    std::string filename_;
    std::unordered_map<std::string, MacroDef> macros_;
    std::vector<std::string> warnings_;
    int globalCounter_ = 0; ///< backing store for __COUNTER__

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
};

} // namespace omscript

#endif // PREPROCESSOR_H
