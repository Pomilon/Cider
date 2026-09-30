#include <cstdio>
#include <string>
#include "lexer.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    cider::ErrorReporter rep;
    cider::Lexer lex("var x: int = 42 + \"hi\\n\" * 3.5;", rep);
    auto next = [&] { return lex.getNextToken(); };

    CHECK(next().type == cider::TokenType::VAR);
    auto id = next();
    CHECK(id.type == cider::TokenType::IDENTIFIER && id.value == "x");
    CHECK(next().type == cider::TokenType::COLON);
    CHECK(next().type == cider::TokenType::INT);
    CHECK(next().type == cider::TokenType::ASSIGN);
    auto num = next();
    CHECK(num.type == cider::TokenType::NUMBER && num.value == "42");
    CHECK(next().type == cider::TokenType::PLUS);
    auto str = next();
    CHECK(str.type == cider::TokenType::STRING && str.value == "hi\n");
    CHECK(next().type == cider::TokenType::MULTIPLY);
    auto fnum = next();
    CHECK(fnum.type == cider::TokenType::NUMBER && fnum.value == "3.5");
    CHECK(next().type == cider::TokenType::SEMICOLON);
    CHECK(next().type == cider::TokenType::END_OF_FILE);
    CHECK(!rep.hasErrors());

    // Operator token forms.
    cider::ErrorReporter rep3;
    cider::Lexer lex3("a <= b >= c == d != e && f || g << h >> i ++ j -- k -> l", rep3);
    auto n3 = [&] { return lex3.getNextToken(); };
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::LE);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::GE);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::EQ);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::NE);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::AND);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::OR);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::LSHIFT);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::RSHIFT);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::INCREMENT);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::DECREMENT);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::ARROW);
    CHECK(n3().type == cider::TokenType::IDENTIFIER);
    CHECK(n3().type == cider::TokenType::END_OF_FILE);
    CHECK(!rep3.hasErrors());

    // Char literals with escapes.
    cider::ErrorReporter rep4;
    cider::Lexer lex4("'a' '\\n' '\\''", rep4);
    auto n4 = [&] { return lex4.getNextToken(); };
    CHECK(n4().value == "a");
    CHECK(n4().value == "\n");
    CHECK(n4().value == "'");
    CHECK(!rep4.hasErrors());

    // CID-005: wide literals. Adjacent `u"` / `u'` only.
    cider::ErrorReporter repW;
    cider::Lexer lexW("u\"AB\" u'\\u0041' u \"x\" u", repW);
    auto nW = [&] { return lexW.getNextToken(); };
    auto wstrTok = nW();
    CHECK(wstrTok.type == cider::TokenType::WIDE_STRING);
    CHECK(wstrTok.value.size() == 6);  // 3 code units, UTF-16LE, incl. NUL
    CHECK((unsigned char)wstrTok.value[0] == 'A' && wstrTok.value[1] == 0);
    auto wcharTok = nW();
    CHECK(wcharTok.type == cider::TokenType::WIDE_CHAR_LITERAL);
    CHECK(wcharTok.value.size() == 2 && (unsigned char)wcharTok.value[0] == 'A');
    auto uIdent = nW();
    CHECK(uIdent.type == cider::TokenType::IDENTIFIER && uIdent.value == "u");  // `u` alone
    CHECK(nW().type == cider::TokenType::STRING);   // `u "x"` is ident + string
    CHECK(nW().type == cider::TokenType::IDENTIFIER);  // trailing `u`
    CHECK(!repW.hasErrors());

    // CID-005 fix round 1: malformed wide char literals are rejected — the
    // reader must not consume the closing quote as content or accept extra
    // code units. `u''` and `u'AB'` yield UNKNOWN plus a lexer error.
    cider::ErrorReporter repWBad1;
    cider::Lexer lexWBad1("u''", repWBad1);
    CHECK(lexWBad1.getNextToken().type == cider::TokenType::UNKNOWN);
    CHECK(repWBad1.hasErrors());
    cider::ErrorReporter repWBad2;
    cider::Lexer lexWBad2("u'AB'", repWBad2);
    CHECK(lexWBad2.getNextToken().type == cider::TokenType::UNKNOWN);
    CHECK(repWBad2.hasErrors());

    // CIDER-FEAT-008: the shift compound assignments. The trailing `=` is
    // only consumed once the `<<` / `>>` pair is confirmed, so the plain
    // shift, the comparison and two separate `<` / `>` tokens are unaffected.
    cider::ErrorReporter repSA;
    cider::Lexer lexSA("a <<= b >>= c << d >> e < f <  g > h >= i <= j", repSA);
    auto nSA = [&] { return lexSA.getNextToken(); };
    CHECK(nSA().type == cider::TokenType::IDENTIFIER);
    auto shlAssign = nSA();
    CHECK(shlAssign.type == cider::TokenType::LSHIFT_ASSIGN && shlAssign.value == "<<=");
    CHECK(nSA().type == cider::TokenType::IDENTIFIER);
    auto shrAssign = nSA();
    CHECK(shrAssign.type == cider::TokenType::RSHIFT_ASSIGN && shrAssign.value == ">>=");
    CHECK(nSA().type == cider::TokenType::IDENTIFIER);
    CHECK(nSA().type == cider::TokenType::LSHIFT);   // `<<` is still a shift
    CHECK(nSA().type == cider::TokenType::IDENTIFIER);
    CHECK(nSA().type == cider::TokenType::RSHIFT);   // `>>` is still a shift
    CHECK(nSA().type == cider::TokenType::IDENTIFIER);
    CHECK(nSA().type == cider::TokenType::LT);       // `<` alone
    CHECK(nSA().type == cider::TokenType::IDENTIFIER);
    CHECK(nSA().type == cider::TokenType::LT);       // `a <  <b` is two `<`
    CHECK(nSA().type == cider::TokenType::IDENTIFIER);
    CHECK(nSA().type == cider::TokenType::GT);       // `>` alone
    CHECK(nSA().type == cider::TokenType::IDENTIFIER);
    CHECK(nSA().type == cider::TokenType::GE);       // `>=` is still a compare
    CHECK(nSA().type == cider::TokenType::IDENTIFIER);
    CHECK(nSA().type == cider::TokenType::LE);       // `<=` is still a compare
    CHECK(nSA().type == cider::TokenType::IDENTIFIER);
    CHECK(nSA().type == cider::TokenType::END_OF_FILE);
    CHECK(!repSA.hasErrors());

    // Line/column tracking and unknown-char handling (parity: UNKNOWN token,
    // no lexer error — the parser reports on UNKNOWN).
    cider::ErrorReporter rep2;
    cider::Lexer lex2("a\n  b @", rep2);
    auto t1 = lex2.getNextToken();
    CHECK(t1.line == 1 && t1.column == 1);
    auto t2 = lex2.getNextToken();
    CHECK(t2.line == 2 && t2.column == 3);
    auto t3 = lex2.getNextToken();
    CHECK(t3.type == cider::TokenType::UNKNOWN && t3.value == "@" && t3.line == 2);
    CHECK(!rep2.hasErrors());
    return 0;
}

int main() { return run(); }
