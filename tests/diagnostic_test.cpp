#include "diagnostic.h"
#include <gtest/gtest.h>

using namespace omscript;

// ===========================================================================
// Diagnostic severity formatting
// ===========================================================================

TEST(DiagnosticTest, ErrorFormat) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::Error;
    diag.location = {1, 1};
    diag.message = "unexpected token";
    EXPECT_EQ(diag.format(), "error at line 1, column 1: unexpected token");
}

TEST(DiagnosticTest, WarningFormat) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::Warning;
    diag.location = {5, 10};
    diag.message = "unused variable";
    EXPECT_EQ(diag.format(), "warning at line 5, column 10: unused variable");
}

TEST(DiagnosticTest, NoteFormat) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::Note;
    diag.location = {3, 7};
    diag.message = "defined here";
    EXPECT_EQ(diag.format(), "note at line 3, column 7: defined here");
}

TEST(DiagnosticTest, HintFormat) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::Hint;
    diag.location = {10, 5};
    diag.message = "consider using a const";
    EXPECT_EQ(diag.format(), "hint at line 10, column 5: consider using a const");
}

TEST(DiagnosticTest, FormatNoLocation) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::Hint;
    diag.message = "suggestion";
    // line==0 means no location
    EXPECT_EQ(diag.format(), "hint: suggestion");
}

TEST(DiagnosticTest, DiagnosticErrorCarriesDiagnostic) {
    Diagnostic diag;
    diag.severity = DiagnosticSeverity::Error;
    diag.location = {2, 3};
    diag.message = "syntax error";
    DiagnosticError err(diag);

    EXPECT_EQ(err.diagnostic().severity, DiagnosticSeverity::Error);
    EXPECT_EQ(err.diagnostic().location.line, 2);
    EXPECT_EQ(err.diagnostic().location.column, 3);
    EXPECT_EQ(err.diagnostic().message, "syntax error");
    // runtime_error::what() returns the formatted string
    EXPECT_STREQ(err.what(), "error at line 2, column 3: syntax error");
}
