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
