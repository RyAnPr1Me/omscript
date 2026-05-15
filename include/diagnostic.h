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
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define OMSC_ISATTY(fd) _isatty(fd)
#define OMSC_STDERR_FILENO 2
#else
#include <unistd.h>
#define OMSC_ISATTY(fd) isatty(fd)
#define OMSC_STDERR_FILENO STDERR_FILENO
#endif

namespace omscript {

// ---------------------------------------------------------------------------
// ANSI terminal color helpers
// ---------------------------------------------------------------------------

/// Returns true if stderr is a TTY and colors should be emitted by default.
inline bool stderrIsTerminal() noexcept {
    return OMSC_ISATTY(OMSC_STDERR_FILENO) != 0;
}

/// ANSI escape-code constants.  Use AnsiColor::reset etc.
struct AnsiColor {
    static constexpr const char* reset   = "\033[0m";
    static constexpr const char* bold    = "\033[1m";
    static constexpr const char* red     = "\033[1;31m"; ///< bold red    — errors
    static constexpr const char* yellow  = "\033[1;33m"; ///< bold yellow — warnings
    static constexpr const char* cyan    = "\033[1;36m"; ///< bold cyan   — notes / hints
    static constexpr const char* blue    = "\033[1;34m"; ///< bold blue   — location prefix
    static constexpr const char* green   = "\033[1;32m"; ///< bold green  — hints
    static constexpr const char* white   = "\033[1;37m"; ///< bold white  — source text
    static constexpr const char* dim     = "\033[2m";    ///< dim         — line-number gutter
};


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
    E013_REGION_NOT_INVALIDATED,      ///< Region variable created with newRegion() not invalidated before function end
    E014_REGION_USE_AFTER_INVALIDATE, ///< Region variable referenced after it was invalidated

    // ── Borrow checker errors (E015–E021) ─────────────────────────────────
    E015_USE_AFTER_MOVE,            ///< Variable read/written after ownership was moved out
    E016_BORROW_WRITE_CONFLICT,     ///< Write to variable with active immutable borrow(s)
    E017_DOUBLE_MUT_BORROW,         ///< Mutable borrow of already mutably-borrowed variable
    E018_MOVE_WHILE_BORROWED,       ///< Move of variable with active borrow(s)
    E019_DOUBLE_INVALIDATE,         ///< invalidate called on already-invalidated variable (Ω spec §6.1)
    E020_WRITE_TO_SHARED,           ///< Write to variable in shared ownership state (Ω spec §3.1)
    E021_OWN_ON_FROZEN,             ///< own called on frozen variable — freeze is irreversible (Ω spec §3.1)
    E022_INVALIDATE_WHILE_BORROWED, ///< invalidate called while active borrow(s) exist (Ω spec §6.2)

    NONE ///< No specific error code (legacy/fallback).
};

/// Return the string code (e.g. "E001") for a given ErrorCode.
inline const char* errorCodeString(ErrorCode code) {
    switch (code) {
    case ErrorCode::E001_UNDEFINED_VARIABLE:
        return "E001";
    case ErrorCode::E002_TYPE_MISMATCH:
        return "E002";
    case ErrorCode::E003_UNDEFINED_FUNCTION:
        return "E003";
    case ErrorCode::E004_WRONG_ARG_COUNT:
        return "E004";
    case ErrorCode::E005_CONST_MODIFICATION:
        return "E005";
    case ErrorCode::E006_SYNTAX_ERROR:
        return "E006";
    case ErrorCode::E007_IMPORT_NOT_FOUND:
        return "E007";
    case ErrorCode::E008_CIRCULAR_IMPORT:
        return "E008";
    case ErrorCode::E009_DUPLICATE_FIELD:
        return "E009";
    case ErrorCode::E010_UNKNOWN_FIELD:
        return "E010";
    case ErrorCode::E011_DIVISION_BY_ZERO:
        return "E011";
    case ErrorCode::E012_INDEX_OUT_OF_BOUNDS:
        return "E012";
    case ErrorCode::E013_REGION_NOT_INVALIDATED:
        return "E013";
    case ErrorCode::E014_REGION_USE_AFTER_INVALIDATE:
        return "E014";
    case ErrorCode::E015_USE_AFTER_MOVE:
        return "E015";
    case ErrorCode::E016_BORROW_WRITE_CONFLICT:
        return "E016";
    case ErrorCode::E017_DOUBLE_MUT_BORROW:
        return "E017";
    case ErrorCode::E018_MOVE_WHILE_BORROWED:
        return "E018";
    case ErrorCode::E019_DOUBLE_INVALIDATE:
        return "E019";
    case ErrorCode::E020_WRITE_TO_SHARED:
        return "E020";
    case ErrorCode::E021_OWN_ON_FROZEN:
        return "E021";
    case ErrorCode::E022_INVALIDATE_WHILE_BORROWED:
        return "E022";
    case ErrorCode::NONE:
        return "";
    }
    return "";
}

// ---------------------------------------------------------------------------
// "Did you mean?" suggestion helper
// ---------------------------------------------------------------------------

/// Compute the edit distance between two strings (Levenshtein distance).
/// Accounts for insertions, deletions, and substitutions.
inline size_t editDistance(const std::string& a, const std::string& b) {
    const size_t m = a.size(), n = b.size();
    // Fast paths for trivial cases.
    if (m == 0)
        return n;
    if (n == 0)
        return m;
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
inline std::string suggestSimilar(const std::string& name, const std::vector<std::string>& candidates,
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
    ErrorCode code = ErrorCode::NONE; ///< Machine-readable error code.

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
            return codePrefix + location.filename + ":" + std::to_string(location.line) + ":" +
                   std::to_string(location.column) + ": " + prefix + ": " + message;
        }
        if (location.line > 0) {
            return codePrefix + prefix + " at line " + std::to_string(location.line) + ", column " +
                   std::to_string(location.column) + ": " + message;
        }
        return codePrefix + prefix + ": " + message;
    }

    /// Format a rich, human-readable diagnostic with optional ANSI colors and a
    /// source-code snippet showing the error location (clang / rustc style).
    ///
    /// @param color        When true, emit ANSI escape codes.
    /// @param sourceLines  The source file split into lines (1-indexed via [line-1]).
    ///
    /// Example output (colors stripped):
    /// @code
    ///   error[E006]: Parse error: expected ';'
    ///    --> src/main.om:15:8
    ///     |
    ///  15 |   var x = foo bar
    ///     |               ^
    ///     |
    /// @endcode
    std::string formatRich(bool color, const std::vector<std::string>& sourceLines) const {
        // ── ANSI helpers ────────────────────────────────────────────────────
        auto c = [&](const char* code) -> std::string { return color ? code : ""; };

        // Severity label + color.
        std::string severityLabel;
        const char* severityColor = "";
        switch (severity) {
        case DiagnosticSeverity::Error:
            severityLabel = "error";
            severityColor = AnsiColor::red;
            break;
        case DiagnosticSeverity::Warning:
            severityLabel = "warning";
            severityColor = AnsiColor::yellow;
            break;
        case DiagnosticSeverity::Note:
            severityLabel = "note";
            severityColor = AnsiColor::cyan;
            break;
        case DiagnosticSeverity::Hint:
            severityLabel = "hint";
            severityColor = AnsiColor::green;
            break;
        }

        std::string codeStr;
        if (code != ErrorCode::NONE) {
            codeStr = std::string("[") + errorCodeString(code) + "]";
        }

        std::ostringstream out;

        // ── Header: "error[E006]: message" ──────────────────────────────────
        out << c(severityColor) << severityLabel;
        if (!codeStr.empty()) {
            out << codeStr;
        }
        out << c(AnsiColor::reset) << c(AnsiColor::bold) << ": " << message << c(AnsiColor::reset) << "\n";

        const bool hasLocation = location.line > 0;
        if (!hasLocation) {
            return out.str();
        }

        // ── Location arrow: " --> file:line:col" ───────────────────────────
        std::string locStr;
        if (!location.filename.empty()) {
            locStr = location.filename + ":" + std::to_string(location.line) + ":" + std::to_string(location.column);
        } else {
            locStr = "line " + std::to_string(location.line) + ", column " + std::to_string(location.column);
        }
        out << c(AnsiColor::blue) << " --> " << c(AnsiColor::reset) << locStr << "\n";

        // ── Source snippet ───────────────────────────────────────────────────
        const int lineIdx = location.line - 1; // 0-based
        if (lineIdx < 0 || static_cast<size_t>(lineIdx) >= sourceLines.size()) {
            return out.str();
        }
        const std::string& srcLine = sourceLines[static_cast<size_t>(lineIdx)];

        // Compute the width of the line-number column.
        const int lineNo      = location.line;
        const int lineNoWidth = static_cast<int>(std::to_string(lineNo).size());

        // Gutter helper: "  5 | " or "    | "
        auto gutterLine = [&](int ln) -> std::string {
            std::string num = (ln > 0) ? std::to_string(ln) : "";
            std::string padded(static_cast<size_t>(lineNoWidth) - num.size(), ' ');
            return c(AnsiColor::dim) + " " + padded + num + " | " + c(AnsiColor::reset);
        };
        auto gutterBlank = [&]() -> std::string {
            std::string spaces(static_cast<size_t>(lineNoWidth), ' ');
            return c(AnsiColor::dim) + " " + spaces + " | " + c(AnsiColor::reset);
        };

        out << gutterBlank() << "\n";
        out << gutterLine(lineNo) << c(AnsiColor::white) << srcLine << c(AnsiColor::reset) << "\n";

        // Build the caret line under the column.
        const int col = (location.column > 0) ? location.column : 1;
        const int caretOffset = std::max(0, col - 1);
        std::string caretLine(static_cast<size_t>(caretOffset), ' ');
        caretLine += c(severityColor) + "^" + c(AnsiColor::reset);

        out << gutterBlank() << caretLine << "\n";

        return out.str();
    }

    /// Serialize the diagnostic to a single-line JSON object (without trailing newline).
    /// Suitable for --error-format=json output: one object per line.
    std::string formatJson() const {
        auto jsonEscape = [](const std::string& s) -> std::string {
            std::string out;
            out.reserve(s.size() + 2);
            for (char ch : s) {
                switch (ch) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:   out += ch;     break;
                }
            }
            return out;
        };

        std::string sev;
        switch (severity) {
        case DiagnosticSeverity::Error:   sev = "error";   break;
        case DiagnosticSeverity::Warning: sev = "warning"; break;
        case DiagnosticSeverity::Note:    sev = "note";    break;
        case DiagnosticSeverity::Hint:    sev = "hint";    break;
        }

        std::ostringstream j;
        j << "{\"severity\":\"" << sev << "\"";
        if (code != ErrorCode::NONE) {
            j << ",\"code\":\"" << errorCodeString(code) << "\"";
        }
        j << ",\"message\":\"" << jsonEscape(message) << "\"";
        if (!location.filename.empty()) {
            j << ",\"file\":\"" << jsonEscape(location.filename) << "\"";
        }
        if (location.line > 0) {
            j << ",\"line\":" << location.line << ",\"column\":" << location.column;
        }
        j << "}";
        return j.str();
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
// Source-text utilities
// ---------------------------------------------------------------------------

/// Split a source string into a vector of lines (without trailing newline chars).
/// Used to provide source snippets to Diagnostic::formatRich().
inline std::vector<std::string> splitSourceLines(const std::string& source) {
    std::vector<std::string> lines;
    std::string line;
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '\n') {
            lines.push_back(line);
            line.clear();
        } else if (source[i] == '\r') {
            // Handle \r\n — skip the '\n' if it follows.
            lines.push_back(line);
            line.clear();
            if (i + 1 < source.size() && source[i + 1] == '\n') {
                ++i;
            }
        } else {
            line += source[i];
        }
    }
    // Push the last line even if there's no trailing newline.
    lines.push_back(line);
    return lines;
}

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
