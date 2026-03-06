#ifndef DIAGNOSTIC_H
#define DIAGNOSTIC_H

#include <stdexcept>
#include <string>

namespace omscript {

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

    /// Format as "file:L:C: error: message" (or "error at line L, column C: message" if no file).
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
        if (!location.filename.empty() && location.line > 0) {
            return location.filename + ":" + std::to_string(location.line) + ":" + std::to_string(location.column) +
                   ": " + prefix + ": " + message;
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
