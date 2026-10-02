// lexer_test.cpp — one character in, one token out, with a position.
//
// LAYERS.md §3.1: lexing the same text always produces the same token stream,
// and a malformed literal is a diagnostic at its position, never a
// silently-dropped token.

#include <cstdio>
#include <string>
#include <vector>

#include "lexer/lexer.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool holds, const char* what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++failures;
    }
}

std::vector<cider::Token> lex(const std::string& text) {
    cider::Lexer lexer("test.cid", text);
    return lexer.lex();
}

cider::Token only(std::vector<cider::Token> tokens, const char* what) {
    if (tokens.size() != 2) {
        ++checks;
        std::printf("  FAIL %s (want 1 token + End, got %lu)\n", what,
                    static_cast<unsigned long>(tokens.size()));
        return cider::Token{};
    }
    return tokens[0];
}

// Empty input is one End token, and positions start at 1:1.
void testEmpty() {
    std::printf("empty input\n");
    std::vector<cider::Token> tokens = lex("");
    check(tokens.size() == 1 && tokens[0].kind == cider::TokenKind::End, "only End");
    check(tokens[0].pos.line == 1 && tokens[0].pos.column == 1, "End is at 1:1");
}

// A declaration lexes to its spellings, each with a position.
void testDeclaration() {
    std::printf("a declaration\n");
    std::vector<cider::Token> tokens = lex("var x: int = 42;");
    check(tokens.size() == 8, "seven tokens plus End");
    check(tokens[0].kind == cider::TokenKind::KwVar, "var is a keyword");
    check(tokens[1].kind == cider::TokenKind::Ident && tokens[1].text == "x", "x is an identifier");
    check(tokens[2].kind == cider::TokenKind::Colon, "then a colon");
    check(tokens[3].kind == cider::TokenKind::KwInt, "int is a keyword");
    check(tokens[4].kind == cider::TokenKind::Eq, "then =");
    check(tokens[5].kind == cider::TokenKind::IntLit, "42 is an integer literal");
    check(tokens[6].kind == cider::TokenKind::Semi, "then ;");
    check(tokens[1].pos.column == 5, "x starts at column 5");
    check(tokens[5].pos.column == 14, "42 starts at column 14");
}

// Integer literals decode once, here: base, digits without _ or prefix, no type.
void testIntegers() {
    std::printf("integer literals decode to base plus digits\n");
    cider::Token plain = only(lex("42"), "42");
    check(plain.lit.intBase == 10, "decimal base");
    check(plain.lit.intDigits == "42", "decimal digits");
    cider::Token grouped = only(lex("1_000_000"), "1_000_000");
    check(grouped.lit.intBase == 10, "grouped base");
    check(grouped.lit.intDigits == "1000000", "underscores are stripped, once, here");
    check(grouped.text == "1_000_000", "the spelling is kept for diagnostics");
    cider::Token hex = only(lex("0xFF"), "0xFF");
    check(hex.lit.intBase == 16, "hex base");
    check(hex.lit.intDigits == "FF", "hex digits without prefix");
    cider::Token bin = only(lex("0b1010"), "0b1010");
    check(bin.lit.intBase == 2, "binary base");
    check(bin.lit.intDigits == "1010", "binary digits");
    cider::Token big = only(lex("9000000000"), "9000000000");
    check(big.lit.intDigits == "9000000000", "a u64 literal survives the lexer intact");
}

// A float literal keeps its exact text and gets no type here either.
void testFloats() {
    std::printf("float literals keep their text\n");
    cider::Token f = only(lex("3.14"), "3.14");
    check(f.kind == cider::TokenKind::FloatLit, "a float");
    check(f.lit.floatDigits == "3.14", "exact text");
    cider::Token e = only(lex("1.0e-9"), "1.0e-9");
    check(e.lit.floatDigits == "1.0e-9", "exponent kept");
}

// Characters decode to a code point; an escape is one character.
void testChars() {
    std::printf("characters decode to a code point\n");
    cider::Token c = only(lex("'x'"), "'x'");
    check(c.kind == cider::TokenKind::CharLit, "a character");
    check(c.lit.charValue == 120, "x is 120");
    cider::Token nl = only(lex("'\\n'"), "'\\n'");
    check(nl.lit.charValue == 10, "newline is 10");
}

// Interpolation is recorded as parts, not re-parsed later.
void testStrings() {
    std::printf("strings record their interpolation sites\n");
    cider::Token s = only(lex("\"text\""), "\"text\"");
    check(s.kind == cider::TokenKind::StringLit, "a string");
    check(s.lit.parts.size() == 1 && !s.lit.parts[0].isExpr, "one text part");
    check(s.lit.parts[0].text == "text", "the bytes");
    cider::Token interp = only(lex("\"x = {e}\""), "interpolated");
    check(interp.lit.parts.size() == 2, "text then expression");
    check(!interp.lit.parts[0].isExpr && interp.lit.parts[0].text == "x = ", "the literal fragment");
    check(interp.lit.parts[1].isExpr && interp.lit.parts[1].text == "e", "the embedded name");
    cider::Token esc = only(lex("\"a\\{b\""), "escaped brace");
    check(esc.lit.parts.size() == 1 && esc.lit.parts[0].text == "a{b", "an escaped brace is text");
}

// The lexer does not fold unary minus: -128 is two tokens, and the parser folds.
void testMinus() {
    std::printf("unary minus is two tokens here\n");
    std::vector<cider::Token> tokens = lex("-128");
    check(tokens.size() == 3, "minus, literal, End");
    check(tokens[0].kind == cider::TokenKind::Minus, "a minus");
    check(tokens[1].kind == cider::TokenKind::IntLit, "then the literal");
}

// `->` does not exist: it lexes as two tokens, and the parser rejects it.
void testNoArrow() {
    std::printf("there is no arrow\n");
    std::vector<cider::Token> tokens = lex("p->f");
    check(tokens.size() == 5, "p, minus, greater, f, End");
    check(tokens[1].kind == cider::TokenKind::Minus, "a minus, not an arrow");
    check(tokens[2].kind == cider::TokenKind::Gt, "then a >");
}

// Multi-character operators lex longest-first.
void testOperators() {
    std::printf("operators lex longest-first\n");
    std::vector<cider::Token> tokens = lex("== != <= >= << >> && || += <<= => :: ... #");
    cider::TokenKind want[] = {
        cider::TokenKind::EqEq, cider::TokenKind::BangEq, cider::TokenKind::LtEq,
        cider::TokenKind::GtEq, cider::TokenKind::LtLt, cider::TokenKind::GtGt,
        cider::TokenKind::AndAnd, cider::TokenKind::OrOr, cider::TokenKind::PlusEq,
        cider::TokenKind::LtLtEq, cider::TokenKind::FatArrow, cider::TokenKind::ColonColon,
        cider::TokenKind::Ellipsis, cider::TokenKind::Hash,
    };
    check(tokens.size() == sizeof(want) / sizeof(want[0]) + 1, "fourteen operators plus End");
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); ++i) {
        if (tokens[i].kind != want[i]) {
            ++checks;
            std::printf("  FAIL operator %lu\n", static_cast<unsigned long>(i));
            ++failures;
        } else {
            ++checks;
        }
    }
    std::printf("  ok   every operator matched\n");
}

// Comments are skipped, positions cross lines, and errors are tokens.
void testCommentsLinesErrors() {
    std::printf("comments, lines and errors\n");
    std::vector<cider::Token> tokens = lex("var a = 1; // a comment\nvar b = 2;");
    check(tokens.size() == 11, "the comment vanishes, the newline does not");
    check(tokens[5].kind == cider::TokenKind::KwVar && tokens[5].pos.line == 2, "second var is on line 2");
    check(tokens[5].pos.column == 1, "at column 1");
    cider::Lexer bad("test.cid", "\"unterminated");
    std::vector<cider::Token> out = bad.lex();
    check(!bad.errors().empty(), "an unterminated string is a diagnostic");
    check(out[0].kind == cider::TokenKind::Error, "and an Error token, not a dropped one");
    check(bad.errors()[0].pos.line == 1, "at its position");
    cider::Lexer at("test.cid", "@");
    at.lex();
    check(!at.errors().empty(), "@ is a diagnostic");
}

}  // namespace

int main() {
    std::printf("lexer_test — one character in, one token out\n\n");
    testEmpty();
    testDeclaration();
    testIntegers();
    testFloats();
    testChars();
    testStrings();
    testMinus();
    testNoArrow();
    testOperators();
    testCommentsLinesErrors();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
