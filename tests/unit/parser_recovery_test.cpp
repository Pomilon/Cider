#include <cstdio>
#include <string>
#include "lexer.h"
#include "parser.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

std::string parseDump(const std::string& src, cider::ErrorReporter& rep) {
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    return cider::dump(*parser.parseProgram());
}

int run() {
    // A bad statement must not swallow the following good ones.
    cider::ErrorReporter rep;
    std::string d = parseDump("var x: int = ;\nfunc f() -> void { }\n", rep);
    CHECK(rep.hasErrors());
    CHECK(d.find("(FunctionDecl f (returns void) (body))") != std::string::npos);

    // Missing semicolon: the incomplete declaration is dropped (parity with
    // main), recovery resumes at the next statement.
    rep.clear();
    d = parseDump("var a: int = 1\nvar b: int = 2;\n", rep);
    CHECK(rep.hasErrors());
    CHECK(d.find("(VarDecl a int = (Number 1))") == std::string::npos);
    CHECK(d.find("(VarDecl b int = (Number 2))") != std::string::npos);

    // Bad expression inside a block: the block still closes and later
    // statements survive.
    rep.clear();
    d = parseDump("func f() {\n  x = ;\n  y();\n}\n", rep);
    CHECK(rep.hasErrors());
    CHECK(d.find("(ExprStmt (Call (Ident y)))") != std::string::npos);

    // Unknown token recovers.
    rep.clear();
    d = parseDump("var ok: int = 1;\n@\nvar ok2: int = 2;\n", rep);
    CHECK(rep.hasErrors());
    CHECK(d.find("(VarDecl ok2 int = (Number 2))") != std::string::npos);
    return 0;
}

int main() { return run(); }
