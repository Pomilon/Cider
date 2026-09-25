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
    cider::ErrorReporter rep;
    // Precedence: 1 + 2 * 3 -> 1 + (2 * 3)
    std::string d = parseDump("func f() -> int {\n  return 1 + 2 * 3;\n}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Return (Binary + (Number 1) (Binary * (Number 2) (Number 3))))") != std::string::npos);

    // Low-precedence cast: (48 + v) as char binds at SUM level.
    rep.clear();
    d = parseDump("func f(v: int) -> char {\n  return (48 + v) as char;\n}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Cast (Group (Binary + (Number 48) (Ident v))) -> char)") != std::string::npos);

    // CID-005: ABI modifiers in cast targets.
    rep.clear();
    d = parseDump("func f(p: *void) -> void {\n  var g: msabi fn(int) -> int = p as msabi fn(int) -> int;\n}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Cast (Ident p) -> msabi fn(int) -> int)") != std::string::npos);

    rep.clear();
    d = parseDump("func f(p: *void) -> void {\n  var g: sysvabi fn(int) -> void = p as sysvabi fn(int) -> void;\n}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Cast (Ident p) -> sysvabi fn(int) -> void)") != std::string::npos);

    // Ternary, member access, index, call.
    rep.clear();
    d = parseDump(
        "func f() -> int {\n"
        "  var a: [int; 3];\n"
        "  var b: int = a[0] == 1 ? foo(2) : st.field;\n"
        "}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Ternary (Binary == (Index (Ident a) (Number 0)) (Number 1)) "
                 "(Call (Ident foo) (Number 2)) (Member (Ident st) field))") != std::string::npos);

    // Unary + prefix, sizeof, struct literal, pointer deref/addr.
    rep.clear();
    d = parseDump(
        "func f(p: int*) -> int {\n"
        "  var x: int = -!flag + sizeof(int) + *p + &q;\n"
        "  var s: Point = Point{x: 1, y: 2};\n"
        "}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Binary + (Binary + (Binary + (Unary - (Unary ! (Ident flag))) (Sizeof int)) "
                 "(Unary * (Ident p))) (Unary & (Ident q)))") != std::string::npos);
    CHECK(d.find("(Struct Point (x (Number 1)) (y (Number 2)))") != std::string::npos);

    // Slices, list/table literals, string escapes.
    rep.clear();
    d = parseDump(
        "func f() -> void {\n"
        "  var a: int = xs[1:3];\n"
        "  var l: int = [1, 2, 3];\n"
        "  var t: int = {\"k\": 1, \"j\": 2};\n"
        "}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Slice (Ident xs) (Number 1) (Number 3))") != std::string::npos);
    CHECK(d.find("(List (Number 1) (Number 2) (Number 3))") != std::string::npos);
    CHECK(d.find("(Table (String \"k\")=(Number 1) (String \"j\")=(Number 2))") != std::string::npos);

    // Templates on identifiers: Vec<int>.
    rep.clear();
    d = parseDump("func f(v: Vec<int>) -> void { }\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(params (v Vec<int>))") != std::string::npos);
    return 0;
}

int main() { return run(); }
