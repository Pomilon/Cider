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
    std::string d;

    // var/const declarations.
    rep.clear();
    d = parseDump("var x: int = 5;\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(VarDecl x int = (Number 5))") != std::string::npos);
    rep.clear();
    d = parseDump("const C: int = 10;\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(ConstDecl C int = (Number 10))") != std::string::npos);

    // CID-006: function-pointer type spelling.
    rep.clear();
    d = parseDump("var f: fn(int, int) -> int = add;\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(VarDecl f fn(int, int) -> int = (Ident add))") != std::string::npos);

    rep.clear();
    d = parseDump("func pick(cb: fn(int) -> void) -> fn(int) -> int { return cb; }\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(FunctionDecl pick (params (cb fn(int) -> void)) (returns fn(int) -> int) (body") != std::string::npos);

    rep.clear();
    d = parseDump("var t: [fn(int) -> int; 2] = [a, b];\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(VarDecl t fn(int) -> int[2]") != std::string::npos);

    rep.clear();
    d = parseDump("func f(p: fn(int) -> int) -> int { return p(1); }\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(FunctionDecl f (params (p fn(int) -> int)) (returns int)") != std::string::npos);

    // CID-005: ABI modifiers on definitions and fn types.
    rep.clear();
    d = parseDump("msabi func efi_main(image: EFI_HANDLE, st: *EFI_SYSTEM_TABLE) -> u64 { return 0; }\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(FunctionDecl efi_main msabi (params") != std::string::npos);

    rep.clear();
    d = parseDump("msabi extern func puts(s: *u16) -> u64;\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(FunctionDecl puts msabi extern (params") != std::string::npos);

    rep.clear();
    d = parseDump("func f(p: msabi fn(int, float) -> u64) -> int { return 0; }\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(p msabi fn(int, float) -> u64)") != std::string::npos);

    rep.clear();
    d = parseDump("func f(p: sysvabi fn(int) -> void) -> int { return 0; }\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(p sysvabi fn(int) -> void)") != std::string::npos);

    rep.clear();
    d = parseDump("var t: [msabi fn(int) -> int; 2] = [a, b];\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("msabi fn(int) -> int[2]") != std::string::npos);
    CHECK(d.find("msabi fn(int) -> int[2]") != std::string::npos);

    // Assignment forms incl. compound.
    rep.clear();
    d = parseDump("func f() {\n  x += 2;\n  a.b[0] = foo();\n}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Assign + (Ident x) (Number 2))") != std::string::npos);
    CHECK(d.find("(Assign = (Index (Member (Ident a) b) (Number 0)) (Call (Ident foo)))") != std::string::npos);

    // if/else, while, for, for-in, break, continue.
    rep.clear();
    d = parseDump("func f() {\n  if (a) { b(); } else { c(); }\n  while (x < 10) { x = x + 1; }\n}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(If (Ident a) (then (ExprStmt (Call (Ident b)))) (else (ExprStmt (Call (Ident c)))))") != std::string::npos);
    CHECK(d.find("(While (Binary < (Ident x) (Number 10)) (Assign = (Ident x) (Binary + (Ident x) (Number 1))))") != std::string::npos);

    rep.clear();
    d = parseDump(
        "func f() {\n"
        "  for (var i: int = 0; i < n; i = i + 1) { break; }\n"
        "  for (var x in items) { continue; }\n"
        "}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(For (VarDecl i int = (Number 0)) (Binary < (Ident i) (Ident n)) "
                 "(Assign = (Ident i) (Binary + (Ident i) (Number 1))) (Break))") != std::string::npos);
    CHECK(d.find("(ForEach x (Ident items) (Continue))") != std::string::npos);

    // return/throw/try-catch/defer/asm/match.
    rep.clear();
    d = parseDump(
        "func f() {\n"
        "  defer close(f);\n"
        "  try { g(); } catch (e) { h(e); }\n"
        "  asm { \"mov rax, 60\" }\n"
        "  match (x) { 1 => a(); 2 => b(); }\n"
        "}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Defer (ExprStmt (Call (Ident close) (Ident f))))") != std::string::npos);
    CHECK(d.find("(TryCatch e (try (ExprStmt (Call (Ident g)))) (catch (ExprStmt (Call (Ident h) (Ident e)))))") != std::string::npos);
    CHECK(d.find("(Asm \"mov rax, 60\")") != std::string::npos);
    CHECK(d.find("(Match (Ident x) (arm (Number 1) (ExprStmt (Call (Ident a)))) (arm (Number 2) (ExprStmt (Call (Ident b)))))") != std::string::npos);

    // struct (incl. union), enum, class (extends), type alias.
    rep.clear();
    d = parseDump("struct Point {\n  var x: int;\n  var y: int;\n}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Struct Point (x int) (y int))") != std::string::npos);

    rep.clear();
    d = parseDump("union U {\n  var a: int;\n  var b: float;\n}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Union U (a int) (b float))") != std::string::npos);

    rep.clear();
    d = parseDump("class A extends B {\n  var v: int;\n  func m() -> void { }\n}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Class A extends B (VarDecl v int) (FunctionDecl m (returns void) (body)))") != std::string::npos);

    rep.clear();
    d = parseDump("enum Color { RED, GREEN, BLUE }\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Enum Color RED GREEN BLUE)") != std::string::npos);

    rep.clear();
    d = parseDump("type MyInt = int;\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(TypeAlias MyInt int)") != std::string::npos);

    // interface, imports, export.
    rep.clear();
    d = parseDump("interface Shape {\n  func area() -> float;\n}\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Interface Shape (method area (params) (returns float)))") != std::string::npos);

    rep.clear();
    d = parseDump("import std.io;\nfrom std.vector import Vec;\nexport func f() -> void { }\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Import \"std.io\")") != std::string::npos);
    CHECK(d.find("(FromImport \"std.vector\" Vec)") != std::string::npos);
    CHECK(d.find("(Export (FunctionDecl f (returns void) (body)))") != std::string::npos);

    // strict header + destructor + templates.
    rep.clear();
    d = parseDump("strict cider;\nfunc ~A() { }\nfunc g<T>(x: T) -> void { }\n", rep);
    CHECK(!rep.hasErrors());
    CHECK(d.find("(Program strict") != std::string::npos);
    CHECK(d.find("(FunctionDecl ~A (returns void) (body))") != std::string::npos);
    CHECK(d.find("(FunctionDecl g <T> (params (x T)) (returns void) (body))") != std::string::npos);
    return 0;
}

int main() { return run(); }
