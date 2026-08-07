#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"
#include "symbol_collector.h"
#include "symbols.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

struct Result {
    std::unique_ptr<cider::SymbolTable> table;
    std::vector<std::string> errors;
};

Result collectSrc(const std::string& src) {
    Result r;
    r.table = std::make_unique<cider::SymbolTable>();
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(*r.table, rep, resolver);
    collector.collect(*prog);
    for (auto& e : rep.errors()) r.errors.push_back(e.message);
    return r;
}

std::shared_ptr<cider::Symbol> findSym(cider::SymbolTable& table, const std::string& name) {
    for (auto& s : table.getAllSymbols()) {
        if (s->name == name) return s;
    }
    return nullptr;
}

int run() {
    // Struct layout is dense (no padding): int (4) + char* (8) = 12.
    Result r1 = collectSrc("struct Point {\n  var x: int;\n  var y: char*;\n}\n");
    CHECK(r1.errors.empty());
    auto point = findSym(*r1.table, "Point");
    CHECK(point != nullptr);
    auto ps = std::static_pointer_cast<cider::StructSymbol>(point);
    CHECK(ps->totalSize == 12);
    CHECK(ps->fields.size() == 2);
    CHECK(ps->fields[0].offset == 0);
    CHECK(ps->fields[1].offset == 4);

    // Single int field: 4 bytes.
    Result r2 = collectSrc("struct Small {\n  var a: int;\n}\n");
    auto small = std::static_pointer_cast<cider::StructSymbol>(findSym(*r2.table, "Small"));
    CHECK(small->totalSize == 4);

    // Union takes max size.
    Result r3 = collectSrc("union U {\n  var a: int;\n  var b: float;\n}\n");
    auto u = std::static_pointer_cast<cider::StructSymbol>(findSym(*r3.table, "U"));
    CHECK(u->totalSize == 4);
    CHECK(u->fields[0].offset == 0 && u->fields[1].offset == 0);

    // Class inheritance + vtable: vptr (8) + field v (8) = 16; virtual m -> vtable[0].
    Result r4 = collectSrc(
        "class B {\n"
        "  var w: int;\n"
        "}\n"
        "class A extends B {\n"
        "  var v: int;\n"
        "  virtual func m() -> void { }\n"
        "}\n");
    CHECK(r4.errors.empty());
    auto a = std::static_pointer_cast<cider::StructSymbol>(findSym(*r4.table, "A"));
    CHECK(a->parentName == "B");
    CHECK(a->vtable.size() == 1);
    CHECK(a->vtable[0] == "A_m");
    CHECK(a->methods.size() == 1 && a->methods[0].vtableIndex == 0);
    CHECK(a->totalSize == 16);

    // Enum variants become const values.
    Result r5 = collectSrc("enum Color { RED, GREEN, BLUE }\n");
    CHECK(r5.errors.empty());
    auto red = findSym(*r5.table, "RED");
    auto green = findSym(*r5.table, "GREEN");
    CHECK(red && red->isConst && red->constValue == 0);
    CHECK(green && green->isConst && green->constValue == 1);

    // Function symbol with params/return.
    Result r6 = collectSrc("func f(x: int) -> char* { return nilPtr; }\n");
    CHECK(r6.errors.empty());
    auto f = std::static_pointer_cast<cider::FunctionSymbol>(findSym(*r6.table, "f"));
    CHECK(f->parameterTypes.size() == 1 && f->parameterTypes[0].name == "int");
    CHECK(f->returnType.name == "char" && f->returnType.pointerLevel == 1);

    // Interface satisfaction (classes implement interfaces; structs hold no methods).
    Result r7 = collectSrc(
        "interface I {\n  func m() -> void;\n}\n"
        "class S {\n  func m() -> void { }\n}\n");
    auto sSym = std::static_pointer_cast<cider::StructSymbol>(findSym(*r7.table, "S"));
    CHECK(sSym != nullptr);
    CHECK(sSym->methods.size() == 1 && sSym->methods[0].mangledName == "S_m");

    // Global var + const.
    Result r8 = collectSrc("var g: int;\nconst C: int = 5;\n");
    CHECK(r8.errors.empty());
    auto g = findSym(*r8.table, "g");
    auto c = findSym(*r8.table, "C");
    CHECK(g && g->kind == cider::SymbolKind::VARIABLE);
    CHECK(c && c->isConst);
    CHECK(c->kind == cider::SymbolKind::VARIABLE);

    // Duplicate definition -> error.
    Result r9 = collectSrc("var x: int;\nvar x: float;\n");
    CHECK(r9.errors.size() == 1);
    CHECK(r9.errors[0].find("Redeclaration of variable 'x'") != std::string::npos);

    // Method of a class is mangled with the class name.
    Result r10 = collectSrc("class K {\n  func go() -> void { }\n}\n");
    auto go = findSym(*r10.table, "K_go");
    CHECK(go != nullptr);
    auto goFn = std::static_pointer_cast<cider::FunctionSymbol>(go);
    CHECK(goFn->parameterTypes.size() == 1 && goFn->parameterTypes[0].name == "K");
    return 0;
}

int main() { return run(); }
