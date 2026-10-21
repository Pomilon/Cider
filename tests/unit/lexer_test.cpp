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
    cider::Token oct = only(lex("0o755"), "0o755");
    check(oct.lit.intBase == 8, "octal base");
    check(oct.lit.intDigits == "755", "octal digits");
    cider::Token badOct = only(lex("0o8"), "0o8");
    check(badOct.kind == cider::TokenKind::Error, "8 is not an octal digit");
}

// The Standard's primitive aliases lex to the canonical type token.
void testTypeAliases() {
    std::printf("type aliases\n");
    check(only(lex("uint"), "uint").kind == cider::TokenKind::KwU32,
          "uint is the u32 alias");
    check(only(lex("float"), "float").kind == cider::TokenKind::KwF32,
          "float is the f32 alias");
    check(only(lex("byte"), "byte").kind == cider::TokenKind::KwByte,
          "byte is the u8 alias");
}

// A float literal keeps its exact text and gets no type here either.
void testFloats() {
    std::printf("float literals keep their text\n");
    cider::Token f = only(lex("3.14"), "3.14");
    check(f.kind == cider::TokenKind::FloatLit, "a float");
    check(f.lit.floatDigits == "3.14", "exact text");
    cider::Token e = only(lex("1.0e-9"), "1.0e-9");
    check(e.lit.floatDigits == "1.0e-9", "exponent kept");
    cider::Token suf = only(lex("3.14f"), "3.14f");
    check(suf.kind == cider::TokenKind::FloatLit, "a float suffix");
    check(suf.lit.floatDigits == "3.14", "the suffix is not part of the digits");
    check(suf.lit.floatNarrow, "and it asks for f32");
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

// FEATURES §1.4: `{{` is a literal brace, a `{` that cannot open an
// interpolation is literal, and `\xHH` names a byte. Anything else after
// `\` or an unterminated interpolation is a diagnostic.
void testBraces() {
    std::printf("braces and hex escapes\n");
    cider::Token doubled = only(lex("\"k1{{k2}}\""), "doubled braces");
    check(doubled.kind == cider::TokenKind::StringLit, "still a string");
    check(doubled.lit.parts.size() == 1 && !doubled.lit.parts[0].isExpr, "no interpolation");
    check(doubled.lit.parts[0].text == "k1{k2}", "each pair is one brace");
    cider::Token lone = only(lex("\"{\""), "lone open brace");
    check(lone.kind == cider::TokenKind::StringLit, "still a string");
    check(lone.lit.parts.size() == 1 && lone.lit.parts[0].text == "{", "a lone brace is text");
    cider::Token hex = only(lex("\"a\\x7Bb\""), "hex escape");
    check(hex.kind == cider::TokenKind::StringLit, "still a string");
    check(hex.lit.parts.size() == 1 && hex.lit.parts[0].text == "a{b", "hex names the byte");
    cider::Token badHex = only(lex("\"a\\xq\""), "bad hex escape");
    check(badHex.kind == cider::TokenKind::Error, "a non-hex digit is an error");
    cider::Token interp = only(lex("\"x = {y}\""), "real interpolation");
    check(interp.lit.parts.size() == 2 && interp.lit.parts[1].isExpr, "a name still interpolates");
}

// Wide literals decode to UTF-16 units: `u"AB"` is two units, `\uXXXX`
// names one raw unit (even a lone surrogate — no pairing, no validation
// past four hex digits), UTF-8 source encodes, and `{{`/`}}` spell braces.
// A lone `{` would interpolate, which wide literals do not do, so it is a
// diagnostic rather than a guess.
void testWide() {
    std::printf("wide literals decode to units\n");
    cider::Token s = only(lex("u\"AB\""), "u\"AB\"");
    check(s.kind == cider::TokenKind::WideStringLit, "a wide string");
    check(s.lit.wideUnits.size() == 2 && s.lit.wideUnits[0] == 65 &&
              s.lit.wideUnits[1] == 66,
          "AB is 65, 66");
    cider::Token esc = only(lex("u\"C\\u0044\""), "u escape");
    check(esc.kind == cider::TokenKind::WideStringLit, "still wide");
    check(esc.lit.wideUnits.size() == 2 && esc.lit.wideUnits[1] == 68,
          "\\u0044 is 68");
    cider::Token surr = only(lex("u\"\\uD83D\\uDE00\""), "surrogates");
    check(surr.lit.wideUnits.size() == 2 && surr.lit.wideUnits[0] == 0xD83D &&
              surr.lit.wideUnits[1] == 0xDE00,
          "surrogates pass through raw");
    cider::Token utf = only(lex("u\"\xc3\xa9\""), "utf-8 source");
    check(utf.lit.wideUnits.size() == 1 && utf.lit.wideUnits[0] == 0xE9,
          "U+00E9 encodes as one unit");
    cider::Token c = only(lex("u'Z'"), "u'Z'");
    check(c.kind == cider::TokenKind::WideCharLit, "a wide character");
    check(c.lit.charValue == 90, "Z is 90");
    cider::Token brace = only(lex("u\"a{{b}}\""), "doubled braces");
    check(brace.lit.wideUnits.size() == 4 && brace.lit.wideUnits[1] == '{' &&
              brace.lit.wideUnits[3] == '}',
          "each pair is one brace");
    cider::Lexer interp("test.cid", "u\"{e}\"");
    std::vector<cider::Token> iout = interp.lex();
    check(!interp.errors().empty(), "an interpolation is a diagnostic");
    check(iout[0].kind == cider::TokenKind::Error, "and an Error token");
    cider::Lexer badEsc("test.cid", "u\"\\q\"");
    badEsc.lex();
    check(!badEsc.errors().empty(), "a bad escape is a diagnostic");
    std::vector<cider::Token> bare = lex("u");
    check(bare.size() == 2 && bare[0].kind == cider::TokenKind::Ident,
          "a lone u stays an identifier");
}

// The lexer does not fold unary minus: -128 is two tokens, and the parser folds.
void testMinus() {
    std::printf("unary minus is two tokens here\n");
    std::vector<cider::Token> tokens = lex("-128");
    check(tokens.size() == 3, "minus, literal, End");
    check(tokens[0].kind == cider::TokenKind::Minus, "a minus");
    check(tokens[1].kind == cider::TokenKind::IntLit, "then the literal");
}

// `->` is one Arrow token.
void testArrowToken() {
    std::printf("the arrow token\n");
    std::vector<cider::Token> tokens = lex("p->f");
    check(tokens.size() == 4, "p, arrow, f, End");
    check(tokens[1].kind == cider::TokenKind::Arrow, "the arrow is one token");
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
    std::vector<cider::Token> atOut = at.lex();
    check(at.errors().empty(), "@ is a token now");
    check(atOut[0].kind == cider::TokenKind::At, "the At token");
}

}  // namespace

int main() {
    std::printf("lexer_test — one character in, one token out\n\n");
    testEmpty();
    testDeclaration();
    testTypeAliases();
    testIntegers();
    testFloats();
    testChars();
    testStrings();
    testBraces();
    testMinus();
    testArrowToken();
    testOperators();
    testWide();
    testCommentsLinesErrors();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
