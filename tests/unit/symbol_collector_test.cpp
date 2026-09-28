#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
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
    // Struct layout uses natural alignment (C ABI): int (4, align 4) at 0,
    // pad 4, char* (8, align 8) at 8, total 16.
    Result r1 = collectSrc("struct Point {\n  var x: int;\n  var y: char*;\n}\n");
    CHECK(r1.errors.empty());
    auto point = findSym(*r1.table, "Point");
    CHECK(point != nullptr);
    auto ps = std::static_pointer_cast<cider::StructSymbol>(point);
    CHECK(ps->totalSize == 16);
    CHECK(ps->fields.size() == 2);
    CHECK(ps->fields[0].offset == 0);
    CHECK(ps->fields[1].offset == 8);

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
    // CIDER-FEAT-002: the enum is a distinct EnumSymbol holding the
    // variant list; variants carry the enum's nominal type and the
    // variant marker (still module-scope consts for the bare spelling).
    auto colorSym = std::static_pointer_cast<cider::EnumSymbol>(findSym(*r5.table, "Color"));
    CHECK(colorSym && colorSym->kind == cider::SymbolKind::ENUM);
    CHECK(colorSym->variants.size() == 3);
    CHECK(colorSym->variants[0].name == "RED" && colorSym->variants[0].value == 0);
    CHECK(colorSym->variants[2].name == "BLUE" && colorSym->variants[2].value == 2);
    CHECK(red->isEnumVariant && red->type.name == "Color");
    int64_t variantValue = 0;
    CHECK(colorSym->findVariant("GREEN", variantValue) && variantValue == 1);
    CHECK(!colorSym->findVariant("PURPLE", variantValue));
    // An enum-typed struct field lays out as the 4-byte underlying value,
    // not the legacy 8-byte unknown-nominal default.
    Result r5b = collectSrc(
        "enum Color { RED, GREEN }\n"
        "struct S {\n  var c: Color;\n  var n: int;\n}\n");
    CHECK(r5b.errors.empty());
    auto enumFieldStruct = std::static_pointer_cast<cider::StructSymbol>(findSym(*r5b.table, "S"));
    CHECK(enumFieldStruct && enumFieldStruct->totalSize == 8 && enumFieldStruct->fields[1].offset == 4);

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

    // CID-005/T8: `type X = T` alias-typed struct fields lay out exactly as
    // the target type. Before the fix, getFieldAlignment/getFieldSize cast
    // the STRUCT-kind alias provider (a plain Symbol, not a StructSymbol) and
    // read past its allocation -> SIGSEGV on any alias-typed field.
    Result r11 = collectSrc(
        "type Handle = *void;\n"
        "type Word = u64;\n"
        "struct Inner {\n  var a: u32;\n  var b: u32;\n}\n"
        "type InnerAlias = Inner;\n"
        "struct Outer {\n"
        "  var h: Handle;\n"
        "  var w: Word;\n"
        "  var in: InnerAlias;\n"
        "  var t: u16;\n"
        "}\n");
    CHECK(r11.errors.empty());
    auto outer = std::static_pointer_cast<cider::StructSymbol>(findSym(*r11.table, "Outer"));
    CHECK(outer != nullptr);
    // h(*void, 8) at 0, w(u64, 8) at 8, in(Inner, 8) at 16, t(u16, 2) at 24,
    // total padded to the 8-byte max alignment: 32.
    CHECK(outer->fields.size() == 4);
    CHECK(outer->fields[0].offset == 0);
    CHECK(outer->fields[1].offset == 8);
    CHECK(outer->fields[2].offset == 16);
    CHECK(outer->fields[3].offset == 24);
    CHECK(outer->totalSize == 32);

    // A mutually recursive alias chain must terminate (layout falls back to
    // the 8-byte nominal size) instead of recursing forever.
    Result r12 = collectSrc(
        "type A = B;\n"
        "type B = A;\n"
        "struct Cyc {\n  var a: A;\n}\n");
    CHECK(r12.errors.empty());
    auto cyc = std::static_pointer_cast<cider::StructSymbol>(findSym(*r12.table, "Cyc"));
    CHECK(cyc != nullptr);
    CHECK(cyc->totalSize == 8);

    // CID-005/T7 fix round 1 (F2): the UEFI examples' protocol struct must
    // place ClearScreen at the spec offset 48 (Reset 0, OutputString 8,
    // TestString 16, QueryMode 24, SetMode 32, SetAttribute 40). A five-slot
    // `_pad` silently moved it to 56; parse the shipped files so the example
    // text itself is pinned, not a copy.
    std::string root = __FILE__;
    auto slash = root.find("/tests/unit/");
    CHECK(slash != std::string::npos);
    root = root.substr(0, slash);
    for (const char* rel : {"examples/uefi/hello.cid", "examples/uefi/info.cid",
                            "examples/uefi/args.cid", "tests/uefi/abi_torture.cid"}) {
        std::ifstream in(root + "/" + rel);
        CHECK(in.good());
        std::ostringstream ss;
        ss << in.rdbuf();
        Result rf = collectSrc(ss.str());
        CHECK(rf.errors.empty());
        auto proto =
            std::static_pointer_cast<cider::StructSymbol>(findSym(*rf.table, "EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL"));
        CHECK(proto != nullptr);
        CHECK(proto->fields.size() == 4);
        CHECK(proto->fields[0].offset == 0);   // Reset
        CHECK(proto->fields[1].offset == 8);   // OutputString
        CHECK(proto->fields[3].offset == 48);  // ClearScreen
    }

    // CIDER-BUG-047: an override follows the inherited method's recorded
    // vtable index, not its position in `methods` (which also holds plain
    // methods). Plain methods before, between and after virtuals must not
    // shift a slot, and a plain method sharing a virtual's name must not
    // capture it (plain methods keep vtableIndex -1).
    Result r13 = collectSrc(
        "class V0 {\n"
        "  func plain() -> int { return 0; }\n"
        "  virtual func a() -> int { return 1; }\n"
        "  func mid() -> int { return 2; }\n"
        "  virtual func b() -> int { return 3; }\n"
        "}\n"
        "class V1 extends V0 {\n"
        "  override func b() -> int { return 4; }\n"
        "  virtual func c() -> int { return 5; }\n"
        "}\n"
        "class V2 extends V1 {\n"
        "  override func a() -> int { return 6; }\n"
        "  override func c() -> int { return 7; }\n"
        "}\n"
        "class W0 {\n"
        "  func m() -> int { return 0; }\n"
        "  virtual func keep() -> int { return 1; }\n"
        "  virtual func m() -> int { return 2; }\n"
        "}\n"
        "class W1 extends W0 {\n"
        "  override func m() -> int { return 3; }\n"
        "}\n");
    CHECK(r13.errors.empty());
    auto v0 = std::static_pointer_cast<cider::StructSymbol>(findSym(*r13.table, "V0"));
    CHECK(v0->vtable.size() == 2 && v0->vtable[0] == "V0_a" && v0->vtable[1] == "V0_b");
    auto v1 = std::static_pointer_cast<cider::StructSymbol>(findSym(*r13.table, "V1"));
    CHECK(v1->vtable.size() == 3);
    CHECK(v1->vtable[0] == "V0_a" && v1->vtable[1] == "V1_b" && v1->vtable[2] == "V1_c");
    auto v2 = std::static_pointer_cast<cider::StructSymbol>(findSym(*r13.table, "V2"));
    CHECK(v2->vtable.size() == 3);
    CHECK(v2->vtable[0] == "V2_a" && v2->vtable[1] == "V1_b" && v2->vtable[2] == "V2_c");
    // A plain `m` (no slot) precedes the virtual `m` (slot 1): the override
    // must rewrite slot 1 and leave `keep`'s slot 0 alone.
    auto w1 = std::static_pointer_cast<cider::StructSymbol>(findSym(*r13.table, "W1"));
    CHECK(w1->vtable.size() == 2 && w1->vtable[0] == "W0_keep" && w1->vtable[1] == "W1_m");
    bool w1Override = false;
    for (auto& m : w1->methods) {
        if (m.name == "m" && m.isOverride) {
            CHECK(m.vtableIndex == 1);
            CHECK(m.mangledName == "W1_m");
            w1Override = true;
        }
    }
    CHECK(w1Override);

    // CIDER-BUG-060: `export` marks module-level declarations importable;
    // unmarked ones stay private. `export type` / `export interface` now
    // parse and mark; enum variants inherit the enum's export; the
    // `export name;` expression form marks a symbol too.
    Result r14 = collectSrc(
        "export func pub() -> int { return 1; }\n"
        "func priv() -> int { return 2; }\n"
        "export var pub_global: int = 3;\n"
        "var priv_global: int = 4;\n"
        "export const PUB_CONST: int = 5;\n"
        "export type Alias = int;\n"
        "type PrivAlias = int;\n"
        "export interface Shape { func area() -> int; }\n"
        "export struct Point { var x: int; }\n"
        "export enum Color { RED, GREEN }\n"
        "export async func af() -> int { return 6; }\n"
        "func marked() -> int { return 7; }\n"
        "export marked;\n");
    CHECK(r14.errors.empty());
    CHECK(findSym(*r14.table, "pub") && findSym(*r14.table, "pub")->isExported);
    CHECK(findSym(*r14.table, "priv") && !findSym(*r14.table, "priv")->isExported);
    CHECK(findSym(*r14.table, "pub_global") && findSym(*r14.table, "pub_global")->isExported);
    CHECK(findSym(*r14.table, "priv_global") && !findSym(*r14.table, "priv_global")->isExported);
    CHECK(findSym(*r14.table, "PUB_CONST") && findSym(*r14.table, "PUB_CONST")->isExported);
    CHECK(findSym(*r14.table, "Alias") && findSym(*r14.table, "Alias")->isExported);
    CHECK(findSym(*r14.table, "PrivAlias") && !findSym(*r14.table, "PrivAlias")->isExported);
    CHECK(findSym(*r14.table, "Shape") && findSym(*r14.table, "Shape")->isExported);
    CHECK(findSym(*r14.table, "Point") && findSym(*r14.table, "Point")->isExported);
    CHECK(findSym(*r14.table, "Color") && findSym(*r14.table, "Color")->isExported);
    CHECK(findSym(*r14.table, "RED") && findSym(*r14.table, "RED")->isExported);
    CHECK(findSym(*r14.table, "GREEN") && findSym(*r14.table, "GREEN")->isExported);
    CHECK(findSym(*r14.table, "af") && findSym(*r14.table, "af")->isExported);
    CHECK(findSym(*r14.table, "marked") && findSym(*r14.table, "marked")->isExported);
    // Locals inside an exported function body are never module members, so
    // they must not be marked exported.
    Result r15 = collectSrc("export func f() -> int { var local: int = 1; return local; }\n");
    CHECK(r15.errors.empty());
    CHECK(findSym(*r15.table, "f") && findSym(*r15.table, "f")->isExported);
    auto localSym = findSym(*r15.table, "local");
    CHECK(!localSym || !localSym->isExported);
    // Exporting an undefined name is diagnosed instead of silently ignored.
    Result r16 = collectSrc("export nonexistent;\n");
    CHECK(!r16.errors.empty());

    return 0;
}

int main() { return run(); }
