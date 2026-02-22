#include "lexer.h"
#include <gtest/gtest.h>

using namespace omscript;

// Helper: tokenize a source string
static std::vector<Token> lex(const std::string& src) {
    Lexer lexer(src);
    return lexer.tokenize();
}

// ---------------------------------------------------------------------------
// Empty / whitespace
// ---------------------------------------------------------------------------

TEST(LexerTest, EmptySource) {
    auto tokens = lex("");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

TEST(LexerTest, WhitespaceOnly) {
    auto tokens = lex("   \t\n\r  ");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::END_OF_FILE);
}

// ---------------------------------------------------------------------------
// Integer literals
// ---------------------------------------------------------------------------

TEST(LexerTest, IntegerLiteral) {
    auto tokens = lex("42");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[0].intValue, 42);
    EXPECT_EQ(tokens[0].lexeme, "42");
}

TEST(LexerTest, IntegerZero) {
    auto tokens = lex("0");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[0].intValue, 0);
}

TEST(LexerTest, IntegerOverflow) {
    EXPECT_THROW(lex("99999999999999999999999"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Float literals
// ---------------------------------------------------------------------------

TEST(LexerTest, FloatLiteral) {
    auto tokens = lex("3.14");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT);
    EXPECT_DOUBLE_EQ(tokens[0].floatValue, 3.14);
    EXPECT_EQ(tokens[0].lexeme, "3.14");
}

TEST(LexerTest, FloatTrailingDot) {
    auto tokens = lex("5.");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT);
    EXPECT_DOUBLE_EQ(tokens[0].floatValue, 5.0);
}

TEST(LexerTest, FloatLeadingDotIsNotFloat) {
    // ".5" should be DOT then INTEGER 5
    auto tokens = lex(".5");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::DOT);
    EXPECT_EQ(tokens[1].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[1].intValue, 5);
}

// ---------------------------------------------------------------------------
// String literals
// ---------------------------------------------------------------------------

TEST(LexerTest, StringLiteral) {
    auto tokens = lex("\"hello\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme, "hello");
}

TEST(LexerTest, StringEscapeSequences) {
    auto tokens = lex("\"a\\nb\\tc\\\\d\\\"e\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme, "a\nb\tc\\d\"e");
}

TEST(LexerTest, UnterminatedString) {
    EXPECT_THROW(lex("\"hello"), std::runtime_error);
}

TEST(LexerTest, EmptyString) {
    auto tokens = lex("\"\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme, "");
}

// ---------------------------------------------------------------------------
// Keywords
// ---------------------------------------------------------------------------

TEST(LexerTest, Keywords) {
    auto tokens = lex("fn return if else while do for var const break continue in");
    // 12 keywords + EOF
    ASSERT_EQ(tokens.size(), 13u);
    EXPECT_EQ(tokens[0].type, TokenType::FN);
    EXPECT_EQ(tokens[1].type, TokenType::RETURN);
    EXPECT_EQ(tokens[2].type, TokenType::IF);
    EXPECT_EQ(tokens[3].type, TokenType::ELSE);
    EXPECT_EQ(tokens[4].type, TokenType::WHILE);
    EXPECT_EQ(tokens[5].type, TokenType::DO);
    EXPECT_EQ(tokens[6].type, TokenType::FOR);
    EXPECT_EQ(tokens[7].type, TokenType::VAR);
    EXPECT_EQ(tokens[8].type, TokenType::CONST);
    EXPECT_EQ(tokens[9].type, TokenType::BREAK);
    EXPECT_EQ(tokens[10].type, TokenType::CONTINUE);
    EXPECT_EQ(tokens[11].type, TokenType::IN);
}

// ---------------------------------------------------------------------------
// Identifiers
// ---------------------------------------------------------------------------

TEST(LexerTest, Identifier) {
    auto tokens = lex("myVar _private foo123");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "myVar");
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "_private");
    EXPECT_EQ(tokens[2].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[2].lexeme, "foo123");
}

// ---------------------------------------------------------------------------
// Operators
// ---------------------------------------------------------------------------

TEST(LexerTest, ArithmeticOperators) {
    auto tokens = lex("+ - * / %");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::PLUS);
    EXPECT_EQ(tokens[1].type, TokenType::MINUS);
    EXPECT_EQ(tokens[2].type, TokenType::STAR);
    EXPECT_EQ(tokens[3].type, TokenType::SLASH);
    EXPECT_EQ(tokens[4].type, TokenType::PERCENT);
}

TEST(LexerTest, ComparisonOperators) {
    auto tokens = lex("== != < <= > >=");
    ASSERT_GE(tokens.size(), 7u);
    EXPECT_EQ(tokens[0].type, TokenType::EQ);
    EXPECT_EQ(tokens[1].type, TokenType::NE);
    EXPECT_EQ(tokens[2].type, TokenType::LT);
    EXPECT_EQ(tokens[3].type, TokenType::LE);
    EXPECT_EQ(tokens[4].type, TokenType::GT);
    EXPECT_EQ(tokens[5].type, TokenType::GE);
}

TEST(LexerTest, LogicalOperators) {
    auto tokens = lex("&& || !");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::AND);
    EXPECT_EQ(tokens[1].type, TokenType::OR);
    EXPECT_EQ(tokens[2].type, TokenType::NOT);
}

TEST(LexerTest, BitwiseOperators) {
    auto tokens = lex("& | ^ ~ << >>");
    ASSERT_GE(tokens.size(), 7u);
    EXPECT_EQ(tokens[0].type, TokenType::AMPERSAND);
    EXPECT_EQ(tokens[1].type, TokenType::PIPE);
    EXPECT_EQ(tokens[2].type, TokenType::CARET);
    EXPECT_EQ(tokens[3].type, TokenType::TILDE);
    EXPECT_EQ(tokens[4].type, TokenType::LSHIFT);
    EXPECT_EQ(tokens[5].type, TokenType::RSHIFT);
}

TEST(LexerTest, IncrementDecrement) {
    auto tokens = lex("++ --");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::PLUSPLUS);
    EXPECT_EQ(tokens[1].type, TokenType::MINUSMINUS);
}

TEST(LexerTest, CompoundAssignment) {
    auto tokens = lex("+= -= *= /= %=");
    ASSERT_GE(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type, TokenType::PLUS_ASSIGN);
    EXPECT_EQ(tokens[1].type, TokenType::MINUS_ASSIGN);
    EXPECT_EQ(tokens[2].type, TokenType::STAR_ASSIGN);
    EXPECT_EQ(tokens[3].type, TokenType::SLASH_ASSIGN);
    EXPECT_EQ(tokens[4].type, TokenType::PERCENT_ASSIGN);
}

TEST(LexerTest, AssignmentAndEquality) {
    auto tokens = lex("= ==");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::ASSIGN);
    EXPECT_EQ(tokens[1].type, TokenType::EQ);
}

// ---------------------------------------------------------------------------
// Delimiters
// ---------------------------------------------------------------------------

TEST(LexerTest, Delimiters) {
    auto tokens = lex("( ) { } [ ] ; , : ? .");
    ASSERT_GE(tokens.size(), 12u);
    EXPECT_EQ(tokens[0].type, TokenType::LPAREN);
    EXPECT_EQ(tokens[1].type, TokenType::RPAREN);
    EXPECT_EQ(tokens[2].type, TokenType::LBRACE);
    EXPECT_EQ(tokens[3].type, TokenType::RBRACE);
    EXPECT_EQ(tokens[4].type, TokenType::LBRACKET);
    EXPECT_EQ(tokens[5].type, TokenType::RBRACKET);
    EXPECT_EQ(tokens[6].type, TokenType::SEMICOLON);
    EXPECT_EQ(tokens[7].type, TokenType::COMMA);
    EXPECT_EQ(tokens[8].type, TokenType::COLON);
    EXPECT_EQ(tokens[9].type, TokenType::QUESTION);
    EXPECT_EQ(tokens[10].type, TokenType::DOT);
}

// ---------------------------------------------------------------------------
// Range operator
// ---------------------------------------------------------------------------

TEST(LexerTest, RangeOperator) {
    auto tokens = lex("0...10");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[1].type, TokenType::RANGE);
    EXPECT_EQ(tokens[2].type, TokenType::INTEGER);
}

// ---------------------------------------------------------------------------
// OPTMAX tokens
// ---------------------------------------------------------------------------

TEST(LexerTest, OptmaxTokens) {
    auto tokens = lex("OPTMAX=: OPTMAX!:");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::OPTMAX_START);
    EXPECT_EQ(tokens[1].type, TokenType::OPTMAX_END);
}

TEST(LexerTest, OptmaxPrefixNotEnoughChars) {
    // "OPTMAX" alone (fewer than 8 chars) should be lexed as an identifier, not crash.
    auto tokens = lex("OPTMAX");
    ASSERT_GE(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[0].lexeme, "OPTMAX");
}

// ---------------------------------------------------------------------------
// Comments
// ---------------------------------------------------------------------------

TEST(LexerTest, LineComment) {
    auto tokens = lex("42 // this is a comment\n7");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[0].intValue, 42);
    EXPECT_EQ(tokens[1].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[1].intValue, 7);
}

TEST(LexerTest, BlockComment) {
    auto tokens = lex("42 /* block */ 7");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[0].intValue, 42);
    EXPECT_EQ(tokens[1].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[1].intValue, 7);
}

TEST(LexerTest, UnterminatedBlockComment) {
    EXPECT_THROW(lex("/* oops"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Line/column tracking
// ---------------------------------------------------------------------------

TEST(LexerTest, LineColumnTracking) {
    auto tokens = lex("fn\nmain");
    ASSERT_GE(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].line, 1);
    EXPECT_EQ(tokens[1].line, 2);
}

// ---------------------------------------------------------------------------
// Error: unexpected character
// ---------------------------------------------------------------------------

TEST(LexerTest, UnexpectedCharacter) {
    EXPECT_THROW(lex("@"), std::runtime_error);
}

TEST(LexerTest, NonAsciiByteIsRejected) {
    const std::string src(1, static_cast<char>(0xFF));
    EXPECT_THROW(lex(src), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Number before range (edge case: 1...10)
// ---------------------------------------------------------------------------

TEST(LexerTest, IntegerBeforeRange) {
    auto tokens = lex("1...10");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[0].intValue, 1);
    EXPECT_EQ(tokens[1].type, TokenType::RANGE);
    EXPECT_EQ(tokens[2].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[2].intValue, 10);
}

// ---------------------------------------------------------------------------
// Full function tokenization
// ---------------------------------------------------------------------------

TEST(LexerTest, FullFunction) {
    auto tokens = lex("fn main() { return 0; }");
    // fn main ( ) { return 0 ; } EOF
    ASSERT_EQ(tokens.size(), 10u);
    EXPECT_EQ(tokens[0].type, TokenType::FN);
    EXPECT_EQ(tokens[1].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].lexeme, "main");
    EXPECT_EQ(tokens[2].type, TokenType::LPAREN);
    EXPECT_EQ(tokens[3].type, TokenType::RPAREN);
    EXPECT_EQ(tokens[4].type, TokenType::LBRACE);
    EXPECT_EQ(tokens[5].type, TokenType::RETURN);
    EXPECT_EQ(tokens[6].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[7].type, TokenType::SEMICOLON);
    EXPECT_EQ(tokens[8].type, TokenType::RBRACE);
    EXPECT_EQ(tokens[9].type, TokenType::END_OF_FILE);
}

// ---------------------------------------------------------------------------
// String escape: unterminated escape at end
// ---------------------------------------------------------------------------

TEST(LexerTest, UnterminatedEscapeAtEnd) {
    EXPECT_THROW(lex("\"abc\\"), std::runtime_error);
}

// ---------------------------------------------------------------------------
// String escape: carriage return
// ---------------------------------------------------------------------------

TEST(LexerTest, StringEscapeCarriageReturn) {
    auto tokens = lex("\"a\\rb\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme, "a\rb");
}

// ---------------------------------------------------------------------------
// String escape: null, backspace, form feed, vertical tab
// ---------------------------------------------------------------------------

TEST(LexerTest, StringEscapeNull) {
    auto tokens = lex("\"a\\0b\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    std::string expected = std::string("a") + '\0' + "b";
    EXPECT_EQ(tokens[0].lexeme, expected);
}

TEST(LexerTest, StringEscapeBackspace) {
    auto tokens = lex("\"a\\bb\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme, "a\bb");
}

TEST(LexerTest, StringEscapeFormFeed) {
    auto tokens = lex("\"a\\fb\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme, "a\fb");
}

TEST(LexerTest, StringEscapeVerticalTab) {
    auto tokens = lex("\"a\\vb\"");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::STRING);
    EXPECT_EQ(tokens[0].lexeme, "a\vb");
}

// ---------------------------------------------------------------------------
// String escape: unknown escape sequence
// ---------------------------------------------------------------------------

TEST(LexerTest, StringEscapeUnknown) {
    // Unknown escape sequences should produce an error instead of being silently accepted.
    EXPECT_THROW(lex("\"a\\zb\""), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Float with double dot: 1.2.3 -> FLOAT 1.2, DOT, INTEGER 3
// ---------------------------------------------------------------------------

TEST(LexerTest, FloatDoubleDot) {
    auto tokens = lex("1.2.3");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::FLOAT);
    EXPECT_DOUBLE_EQ(tokens[0].floatValue, 1.2);
    EXPECT_EQ(tokens[1].type, TokenType::DOT);
    EXPECT_EQ(tokens[2].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[2].intValue, 3);
}

// ---------------------------------------------------------------------------
// Multiple-digit integer before range: 42...100
// ---------------------------------------------------------------------------

TEST(LexerTest, MultipleNumbersBeforeRange) {
    auto tokens = lex("42...100");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[0].intValue, 42);
    EXPECT_EQ(tokens[1].type, TokenType::RANGE);
    EXPECT_EQ(tokens[2].type, TokenType::INTEGER);
    EXPECT_EQ(tokens[2].intValue, 100);
}

// ---------------------------------------------------------------------------
// Single dot token
// ---------------------------------------------------------------------------

TEST(LexerTest, SingleDot) {
    auto tokens = lex(".");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::DOT);
    EXPECT_EQ(tokens[1].type, TokenType::END_OF_FILE);
}

// ---------------------------------------------------------------------------
// Switch/case/default keywords
// ---------------------------------------------------------------------------

TEST(LexerTest, SwitchKeywords) {
    auto tokens = lex("switch case default");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::SWITCH);
    EXPECT_EQ(tokens[1].type, TokenType::CASE);
    EXPECT_EQ(tokens[2].type, TokenType::DEFAULT);
    EXPECT_EQ(tokens[3].type, TokenType::END_OF_FILE);
}

// ---------------------------------------------------------------------------
// Boolean and null keywords
// ---------------------------------------------------------------------------

TEST(LexerTest, TrueKeyword) {
    auto tokens = lex("true");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::TRUE);
    EXPECT_EQ(tokens[0].lexeme, "true");
}

TEST(LexerTest, FalseKeyword) {
    auto tokens = lex("false");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::FALSE);
    EXPECT_EQ(tokens[0].lexeme, "false");
}

TEST(LexerTest, NullKeyword) {
    auto tokens = lex("null");
    ASSERT_GE(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type, TokenType::NULL_LITERAL);
    EXPECT_EQ(tokens[0].lexeme, "null");
}

TEST(LexerTest, BoolKeywordsInContext) {
    auto tokens = lex("var x = true; var y = false; var z = null;");
    int trueCount = 0, falseCount = 0, nullCount = 0;
    for (const auto& t : tokens) {
        if (t.type == TokenType::TRUE)
            trueCount++;
        if (t.type == TokenType::FALSE)
            falseCount++;
        if (t.type == TokenType::NULL_LITERAL)
            nullCount++;
    }
    EXPECT_EQ(trueCount, 1);
    EXPECT_EQ(falseCount, 1);
    EXPECT_EQ(nullCount, 1);
}

// ---------------------------------------------------------------------------
// Bitwise compound assignment operators
// ---------------------------------------------------------------------------

TEST(LexerTest, AmpersandAssign) {
    auto tokens = lex("x &= 5");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type, TokenType::IDENTIFIER);
    EXPECT_EQ(tokens[1].type, TokenType::AMPERSAND_ASSIGN);
    EXPECT_EQ(tokens[1].lexeme, "&=");
}

TEST(LexerTest, PipeAssign) {
    auto tokens = lex("x |= 5");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[1].type, TokenType::PIPE_ASSIGN);
    EXPECT_EQ(tokens[1].lexeme, "|=");
}

TEST(LexerTest, CaretAssign) {
    auto tokens = lex("x ^= 5");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[1].type, TokenType::CARET_ASSIGN);
    EXPECT_EQ(tokens[1].lexeme, "^=");
}

TEST(LexerTest, LShiftAssign) {
    auto tokens = lex("x <<= 2");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[1].type, TokenType::LSHIFT_ASSIGN);
    EXPECT_EQ(tokens[1].lexeme, "<<=");
}

TEST(LexerTest, RShiftAssign) {
    auto tokens = lex("x >>= 2");
    ASSERT_GE(tokens.size(), 4u);
    EXPECT_EQ(tokens[1].type, TokenType::RSHIFT_ASSIGN);
    EXPECT_EQ(tokens[1].lexeme, ">>=");
}

TEST(LexerTest, BitwiseAssignDoesNotConflict) {
    // Ensure &= doesn't conflict with && or &
    auto tokens = lex("a & b && c &= d");
    int ampCount = 0, andCount = 0, ampAssignCount = 0;
    for (const auto& t : tokens) {
        if (t.type == TokenType::AMPERSAND)
            ampCount++;
        if (t.type == TokenType::AND)
            andCount++;
        if (t.type == TokenType::AMPERSAND_ASSIGN)
            ampAssignCount++;
    }
    EXPECT_EQ(ampCount, 1);
    EXPECT_EQ(andCount, 1);
    EXPECT_EQ(ampAssignCount, 1);
}
