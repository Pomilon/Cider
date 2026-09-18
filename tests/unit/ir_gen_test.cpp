#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "ir_gen.h"
#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"
#include "symbol_collector.h"
#include "symbols.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

std::string genSrc(const std::string& src, std::vector<std::string>* errors = nullptr) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    if (errors) {
        for (auto& e : rep.errors()) errors->push_back(e.message);
    }
    cider::IRGenerator gen(table);
    auto module = gen.generate(*prog);
    std::ostringstream ss;
    std::streambuf* old = std::cout.rdbuf(ss.rdbuf());
    module.print();
    std::cout.rdbuf(old);
    return ss.str();
}

int run() {
    // Function with typed arithmetic.
    std::string d = genSrc("func f(a: int) -> int {\n  return a + 1;\n}\n");
    CHECK(d.find("define f() {") != std::string::npos);
    CHECK(d.find("entry:") != std::string::npos);
    CHECK(d.find("%v0 = alloca 4") != std::string::npos);
    CHECK(d.find("add") != std::string::npos);
    CHECK(d.find("ret %v") != std::string::npos);

    // Global variable.
    d = genSrc("var g: int = 5;\nfunc main() -> void { }\n");
    CHECK(d.find("define main() {") != std::string::npos);

    // Local var + assignment: alloca/store/load sequence.
    d = genSrc("func main() -> void {\n  var x: int = 1;\n  x = x + 1;\n}\n");
    CHECK(d.find("alloca") != std::string::npos);
    CHECK(d.find("store") != std::string::npos);
    CHECK(d.find("load") != std::string::npos);

    // if/else produces two successor blocks with a merge.
    d = genSrc("func main() -> void {\n  if (c) { a(); } else { b(); }\n}\n");
    CHECK(d.find("br ") != std::string::npos);
    CHECK(d.find("main_then") != std::string::npos);
    CHECK(d.find("main_else") != std::string::npos);
    CHECK(d.find("main_ifcont") != std::string::npos);

    // Method call on a class -> mangled target.
    d = genSrc(
        "class K {\n  func go() -> void { }\n}\n"
        "func main() -> void {\n  var k: K* = nilPtr;\n  k.go();\n}\n");
    CHECK(d.find("call K_go") != std::string::npos);

    // Virtual method -> vcall with vtable index.
    d = genSrc(
        "class V {\n  virtual func m() -> void { }\n}\n"
        "func main() -> void {\n  var v: V* = nilPtr;\n  v.m();\n}\n");
    CHECK(d.find("vcall") != std::string::npos);

    // Struct field access loads at the field offset.
    d = genSrc(
        "struct P {\n  var x: int;\n  var y: char*;\n}\n"
        "func main() -> void {\n  var p: P* = nilPtr;\n  var a: int = p.x;\n}\n");
    CHECK(d.find("add %v") != std::string::npos);
    CHECK(d.find("load") != std::string::npos);

    // While loop structure.
    d = genSrc("func main() -> void {\n  while (x < 10) { x = x + 1; }\n}\n");
    CHECK(d.find("main_while_header") != std::string::npos);
    CHECK(d.find("main_while_body") != std::string::npos);
    CHECK(d.find("main_while_exit") != std::string::npos);
    return 0;
}

int main() { return run(); }
