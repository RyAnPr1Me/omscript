#pragma once

#ifndef DIAGNOSTIC_H
#define DIAGNOSTIC_H

/// @file diagnostic.h
/// @brief Diagnostic infrastructure for the OmScript compiler.
///
/// Provides machine-readable ErrorCode identifiers, the Diagnostic struct
/// used to report warnings and errors with source locations, and the
/// DiagnosticEngine that collects and renders diagnostics.

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace omscript {

// ---------------------------------------------------------------------------
// Error codes — machine-readable identifiers for every diagnostic.
// ---------------------------------------------------------------------------
enum class ErrorCode {
    E001_UNDEFINED_VARIABLE,
    E002_TYPE_MISMATCH,
    E003_UNDEFINED_FUNCTION,
    E004_WRONG_ARG_COUNT,
    E005_CONST_MODIFICATION,
    E006_SYNTAX_ERROR,
    E007_IMPORT_NOT_FOUND,
    E008_CIRCULAR_IMPORT,
    E009_DUPLICATE_FIELD,
    E010_UNKNOWN_FIELD,
    E011_DIVISION_BY_ZERO,
    E012_INDEX_OUT_OF_BOUNDS,
    NONE  ///< No specific error code (legacy/fallback).
};

/// Return the string code (e.g. "E001") for a given ErrorCode.
inline const char* errorCodeString(ErrorCode code) {
    switch (code) {
    case ErrorCode::E001_UNDEFINED_VARIABLE:  return "E001";
    case ErrorCode::E002_TYPE_MISMATCH:       return "E002";
    case ErrorCode::E003_UNDEFINED_FUNCTION:  return "E003";
    case ErrorCode::E004_WRONG_ARG_COUNT:     return "E004";
    case ErrorCode::E005_CONST_MODIFICATION:  return "E005";
    case ErrorCode::E006_SYNTAX_ERROR:        return "E006";
    case ErrorCode::E007_IMPORT_NOT_FOUND:    return "E007";
    case ErrorCode::E008_CIRCULAR_IMPORT:     return "E008";
    case ErrorCode::E009_DUPLICATE_FIELD:     return "E009";
    case ErrorCode::E010_UNKNOWN_FIELD:       return "E010";
    case ErrorCode::E011_DIVISION_BY_ZERO:    return "E011";
    case ErrorCode::E012_INDEX_OUT_OF_BOUNDS: return "E012";
    case ErrorCode::NONE:                     return "";
    }
    return "";
}

// ---------------------------------------------------------------------------
// "Did you mean?" suggestion helper
// ---------------------------------------------------------------------------

/// Compute the edit distance between two strings (Levenshtein distance).
/// Accounts for insertions, deletions, and substitutions.
inline size_t editDistance(const std::string& a, const std::string& b) {
    size_t m = a.size(), n = b.size();
    // Fast paths for trivial cases.
    if (m == 0) return n;
    if (n == 0) return m;
    // Use a single-row DP array for space efficiency: O(min(m,n)).
    std::vector<size_t> prev(n + 1), curr(n + 1);
    for (size_t j = 0; j <= n; ++j)
        prev[j] = j;
    for (size_t i = 1; i <= m; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= n; ++j) {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[n];
}

/// Find the most similar name from a list of candidates.
/// Returns empty string if no close match (distance > threshold).
inline std::string suggestSimilar(const std::string& name,
                                  const std::vector<std::string>& candidates,
                                  size_t threshold = 3) {
    std::string best;
    size_t bestDist = threshold + 1;
    for (const auto& c : candidates) {
        const size_t dist = editDistance(name, c);
        if (dist < bestDist) {
            bestDist = dist;
            best = c;
        }
    }
    return best;
}

/// Source location attached to every diagnostic.
struct SourceLocation {
    std::string filename;
    int line = 0;
    int column = 0;
};

/// Severity levels for compiler diagnostics.
enum class DiagnosticSeverity {
    Error,
    Warning,
    Note,
    Hint,
};

/// Structured compiler diagnostic with source location and severity.
///
/// All compiler stages (lexer, parser, codegen) throw DiagnosticError
/// instead of plain std::runtime_error so that tooling can programmatically
/// inspect the error location and severity.
struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    SourceLocation location;
    std::string message;
    ErrorCode code = ErrorCode::NONE;  ///< Machine-readable error code.

    /// Format as "[E001] file:L:C: error: message" (or "error at line L, column C: message" if no file).
    std::string format() const {
        std::string prefix;
        switch (severity) {
        case DiagnosticSeverity::Error:
            prefix = "error";
            break;
        case DiagnosticSeverity::Warning:
            prefix = "warning";
            break;
        case DiagnosticSeverity::Note:
            prefix = "note";
            break;
        case DiagnosticSeverity::Hint:
            prefix = "hint";
            break;
        }
        // Prepend error code if present.
        std::string codePrefix;
        if (code != ErrorCode::NONE) {
            codePrefix = std::string("[") + errorCodeString(code) + "] ";
        }
        if (!location.filename.empty() && location.line > 0) {
            return codePrefix + location.filename + ":" + std::to_string(location.line) + ":" + std::to_string(location.column) +
                   ": " + prefix + ": " + message;
        }
        if (location.line > 0) {
            return codePrefix + prefix + " at line " + std::to_string(location.line) + ", column " +
                   std::to_string(location.column) + ": " + message;
        }
        return codePrefix + prefix + ": " + message;
    }
};

/// Exception type that carries a full Diagnostic payload.
///
/// Inherits from std::runtime_error so that existing catch blocks continue
/// to work without modification, while callers that need structured data
/// can catch DiagnosticError specifically.
class DiagnosticError : public std::runtime_error {
  public:
    explicit DiagnosticError(const Diagnostic& diag) : std::runtime_error(diag.format()), diagnostic_(diag) {}

    const Diagnostic& diagnostic() const noexcept {
        return diagnostic_;
    }

  private:
    Diagnostic diagnostic_;
};

// ---------------------------------------------------------------------------
// Production-grade exception hierarchy
// ---------------------------------------------------------------------------
// These exception types replace generic std::runtime_error throws throughout
// the compiler and driver, enabling callers to catch and handle specific
// failure modes (I/O errors, validation failures, linker failures)
// independently.

/// Thrown when a file cannot be opened, read, or written.
class FileError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

/// Thrown when input validation fails (empty paths, oversized names, etc.).
class ValidationError : public std::invalid_argument {
  public:
    using std::invalid_argument::invalid_argument;
};

/// Thrown when the external linker invocation fails.
class LinkError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

} // namespace omscript

#endif // DIAGNOSTIC_H
