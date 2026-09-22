#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"
#include "symbol_collector.h"
#include "symbols.h"
#include "type_checker.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

std::vector<std::string> checkSrc(const std::string& src) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    rep.clear();
    cider::TypeChecker checker(table, rep);
    checker.check(*prog);
    std::vector<std::string> out;
    for (auto& e : rep.errors()) out.push_back(e.message);
    return out;
}

bool hasError(const std::vector<std::string>& errors, const std::string& sub) {
    for (auto& e : errors) {
        if (e.find(sub) != std::string::npos) return true;
    }
    return false;
}

int run() {
    // Clean program with main.
    auto ok = checkSrc("func main() -> void { var x: int = 1; }\n");
    CHECK(ok.empty());

    // String -> int mismatch.
    auto e1 = checkSrc("func main() -> void { var x: int = \"str\"; }\n");
    CHECK(hasError(e1, "Type mismatch in declaration of 'x'"));

    // int -> char* mismatch.
    auto e2 = checkSrc("func main() -> void { var x: int = 1; var y: char* = x; }\n");
    CHECK(hasError(e2, "Type mismatch in declaration of 'y'"));

    // int -> float widening is OK.
    auto ok2 = checkSrc("func main() -> void { var x: float = 1; }\n");
    CHECK(ok2.empty());

    // CID-003: f32 is an alias of float; f64 is a real type.
    auto okF64 = checkSrc(
        "func main() -> int {\n"
        "  var a: f32 = 1.5;\n"
        "  var b: f64 = 2.5;\n"
        "  var c: f64 = 7;\n"
        "  var d: float = b;\n"
        "  var e: f64 = a + b;\n"
        "  return 0;\n"
        "}\n");
    if (!okF64.empty()) {
        std::fprintf(stderr, "FAIL f64 declarations: %s\n", okF64[0].c_str());
        return 1;
    }

    // CID-003 review: bitwise operators reject float-family operands even
    // though arithmetic promotes them.
    auto eBit1 = checkSrc(
        "func main() -> void { var a: float = 1.0; var b: float = 2.0; var c: float = a & b; }\n");
    CHECK(hasError(eBit1, "Bitwise operator '&' requires integer operands"));
    auto eBit2 = checkSrc(
        "func main() -> void { var a: f64 = 1.0; var b: f64 = 2.0; var c: f64 = a | b; }\n");
    CHECK(hasError(eBit2, "Bitwise operator '|' requires integer operands"));
    auto eBit3 = checkSrc(
        "func main() -> void { var a: f64 = 1.0; var b: f64 = 2.0; var c: f64 = a << b; }\n");
    CHECK(hasError(eBit3, "Bitwise operator '<<' requires integer operands"));
    auto eBit4 = checkSrc(
        "func main() -> void { var a: int = 1; var b: f64 = 2.0; var c: int = a & b; }\n");
    CHECK(hasError(eBit4, "Bitwise operator '&' requires integer operands"));

    // Integer bitwise operations are unaffected.
    auto okBit = checkSrc(
        "func main() -> void { var a: int = 6; var b: int = 3; var c: int = a & b; var d: int = a << b; }\n");
    CHECK(okBit.empty());

    // Return values are typed but NOT checked against the function's
    // return type (parity with main — the IR phase enforces it).
    auto e3 = checkSrc("func f() -> int { return \"s\"; }\nfunc main() -> void { }\n");
    CHECK(e3.empty());

    // Undefined identifier.
    auto e4 = checkSrc("func main() -> void { var x: int = nope; }\n");
    CHECK(hasError(e4, "Undefined identifier 'nope'"));

    // Assignment to undeclared variable.
    auto e5 = checkSrc("func main() -> void { undeclared = 5; }\n");
    CHECK(hasError(e5, "Undefined identifier 'undeclared'"));

    // Const assignment.
    auto e6 = checkSrc("func main() -> void { const C: int = 1; C = 2; }\n");
    CHECK(hasError(e6, "Cannot assign to constant 'C'"));

    // Class pointer covariance.
    auto ok3 = checkSrc(
        "class Base { var a: int; }\n"
        "class Derived extends Base { var b: int; }\n"
        "func main() -> void { var p: Base* = d; var d: Derived* = nilPtr; }\n");
    // Note: d must be declared before use; reorder:
    ok3 = checkSrc(
        "class Base { var a: int; }\n"
        "class Derived extends Base { var b: int; }\n"
        "func main() -> void { var d: Derived* = nilPtr; var p: Base* = d; }\n");
    CHECK(ok3.empty());

    // Reversed covariance -> error.
    auto e7 = checkSrc(
        "class Base { var a: int; }\n"
        "class Derived extends Base { var b: int; }\n"
        "func main() -> void { var b: Base* = nilPtr; var d: Derived* = b; }\n");
    CHECK(hasError(e7, "Type mismatch"));

    // Missing entry point.
    auto e8 = checkSrc("func f() -> void { }\n");
    CHECK(hasError(e8, "Missing entry point"));

    // Undefined type in declaration.
    auto e9 = checkSrc("func main() -> void { var x: Nope = nilPtr; }\n");
    CHECK(e9.size() >= 1);

    // Interface field assignment works when satisfied.
    auto ok4 = checkSrc(
        "interface I { func m() -> void; }\n"
        "class C { func m() -> void { } }\n"
        "func main() -> void { var i: I = c; var c: C* = nilPtr; }\n");
    ok4 = checkSrc(
        "interface I { func m() -> void; }\n"
        "class C { func m() -> void { } }\n"
        "func main() -> void { var c: C* = nilPtr; var i: I = c; }\n");
    CHECK(ok4.empty());
    return 0;
}

int main() { return run(); }
