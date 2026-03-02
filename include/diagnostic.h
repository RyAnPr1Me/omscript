#ifndef DIAGNOSTIC_H
#define DIAGNOSTIC_H

#include <stdexcept>
#include <string>

namespace omscript {

/// Source location attached to every diagnostic.
struct SourceLocation {
    int line = 0;
    int column = 0;
};

/// Severity levels for compiler diagnostics.
enum class DiagnosticSeverity {
    Error,
    Warning,
    Note,
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

    /// Format as "error at line L, column C: message" (or "warning …").
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
        }
        if (location.line > 0) {
            return prefix + " at line " + std::to_string(location.line) + ", column " +
                   std::to_string(location.column) + ": " + message;
        }
        return prefix + ": " + message;
    }
};

/// Exception type that carries a full Diagnostic payload.
///
/// Inherits from std::runtime_error so that existing catch blocks continue
/// to work without modification, while callers that need structured data
/// can catch DiagnosticError specifically.
class DiagnosticError : public std::runtime_error {
  public:
    explicit DiagnosticError(const Diagnostic& diag)
        : std::runtime_error(diag.format()), diagnostic_(diag) {}

    const Diagnostic& diagnostic() const noexcept {
        return diagnostic_;
    }

  private:
    Diagnostic diagnostic_;
};

} // namespace omscript

#endif // DIAGNOSTIC_H
