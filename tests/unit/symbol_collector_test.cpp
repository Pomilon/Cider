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

    // CIDER-BUG-126: an `override` that bound no virtual base slot is
    // reported at the declaration instead of silently staying a direct
    // call. Covered: plain base method, unknown base method name, no base at
    // all, and a plain base declared after the class (CIDER-BUG-120
    // rebuild).
    Result r13b = collectSrc(
        "class P { func f() -> int { return 1; } }\n"
        "class D extends P { override func f() -> int { return 2; } }\n"
        "class N { func g() -> int { return 1; } }\n"
        "class U extends N { override func h() -> int { return 2; } }\n"
        "class O { override func k() -> int { return 3; } }\n"
        "class FD extends FwdBase { override func f() -> int { return 4; } }\n"
        "class FwdBase { func f() -> int { return 1; } }\n");
    CHECK(r13b.errors.size() == 4);
    bool sawPlain = false, sawUnknown = false, sawOrphan = false, sawForward = false;
    for (auto& e : r13b.errors) {
        if (e.find("Method 'f' of class 'D' is marked 'override' but base method 'f' is not virtual") !=
            std::string::npos) sawPlain = true;
        if (e.find("Method 'h' of class 'U' is marked 'override' but no base method named 'h' exists") !=
            std::string::npos) sawUnknown = true;
        if (e.find("Method 'k' of class 'O' is marked 'override' but no base method named 'k' exists") !=
            std::string::npos) sawOrphan = true;
        if (e.find("Method 'f' of class 'FD' is marked 'override' but base method 'f' is not virtual") !=
            std::string::npos) sawForward = true;
    }
    CHECK(sawPlain && sawUnknown && sawOrphan && sawForward);

    // A legitimate override still binds its slot, including over a
    // forward-declared virtual base (no diagnostic).
    Result r13c = collectSrc(
        "class VB { virtual func f() -> int { return 1; } }\n"
        "class VD extends VB { override func f() -> int { return 2; } }\n"
        "class FVD extends FVB { override func f() -> int { return 3; } }\n"
        "class FVB { virtual func f() -> int { return 1; } }\n");
    CHECK(r13c.errors.empty());
    auto vd = std::static_pointer_cast<cider::StructSymbol>(findSym(*r13c.table, "VD"));
    CHECK(vd->vtable.size() == 1 && vd->vtable[0] == "VD_f");
    auto fvd = std::static_pointer_cast<cider::StructSymbol>(findSym(*r13c.table, "FVD"));
    CHECK(fvd->vtable.size() == 1 && fvd->vtable[0] == "FVD_f");

    // CIDER-BUG-141: an override that BINDS a virtual slot but redeclares
    // a different signature is reported at the declaration instead of
    // silently displacing the base method (parameter type, parameter count
    // and return type are all part of the contract). A same-signature
    // override and an alias spelling of the parameter type (`Num` for
    // `int`) are the controls that must keep binding the slot.
    Result r13d = collectSrc(
        "type Num = int;\n"
        "class SB { virtual func f(x: int) -> int { return x; } }\n"
        "class P1 extends SB { override func f(x: f64) -> int { return 1; } }\n"
        "class P2 extends SB { override func f() -> int { return 2; } }\n"
        "class P3 extends SB { override func f(x: int) -> f64 { return 1.5; } }\n"
        "class OK extends SB { override func f(x: Num) -> int { return 3; } }\n");
    CHECK(r13d.errors.size() == 3);
    bool sawParam = false, sawArity = false, sawReturn = false;
    for (auto& e : r13d.errors) {
        if (e.find("Method 'f' of class 'P1' is marked 'override' but its signature does not "
                   "match base method 'f' of class 'SB'") != std::string::npos)
            sawParam = true;
        if (e.find("Method 'f' of class 'P2' is marked 'override' but its signature does not "
                   "match base method 'f' of class 'SB'") != std::string::npos)
            sawArity = true;
        if (e.find("Method 'f' of class 'P3' is marked 'override' but its signature does not "
                   "match base method 'f' of class 'SB'") != std::string::npos)
            sawReturn = true;
    }
    CHECK(sawParam && sawArity && sawReturn);
    auto ok = std::static_pointer_cast<cider::StructSymbol>(findSym(*r13d.table, "OK"));
    CHECK(ok != nullptr);
    CHECK(ok->vtable.size() == 1 && ok->vtable[0] == "OK_f");

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

    // CIDER-BUG-119: function/block-local aggregate declarations are
    // registered with finalized layouts, keyed by their AST node, for the
    // checker and IR generator (which never walk the collector's transient
    // local scope). Both a local struct and a local class must be present.
    {
        cider::ErrorReporter rep;
        cider::Lexer lex("func main() -> void {\n"
                         "  struct S { var a: int; var b: int; }\n"
                         "  class C { var x: int; }\n"
                         "}\n", rep);
        cider::Parser parser(lex, rep);
        auto prog = parser.parseProgram();
        cider::SymbolTable table;
        cider::ModuleResolver resolver;
        cider::SymbolCollector collector(table, rep, resolver);
        collector.collect(*prog);
        CHECK(rep.errors().empty());
        const auto& locals = collector.getLocalAggregates();
        CHECK(locals.size() == 2);
        bool sawStruct = false;
        bool sawClass = false;
        for (const auto& [decl, sym] : locals) {
            (void)decl;
            if (sym->name == "S") {
                sawStruct = true;
                CHECK(sym->kind == cider::SymbolKind::STRUCT);
                CHECK(sym->totalSize == 8);
            } else if (sym->name == "C") {
                sawClass = true;
                CHECK(sym->kind == cider::SymbolKind::CLASS);
                CHECK(sym->totalSize == 4);
            }
        }
        CHECK(sawStruct && sawClass);
    }

    // CIDER-BUG-119: a local struct template has no reachable declaring
    // scope after collection, so it is rejected at the declaration instead
    // of failing later with a misleading member-access diagnostic.
    Result r17 = collectSrc("func main() -> void {\n"
                            "  struct Box<T> { var v: T; }\n"
                            "}\n");
    bool localTemplateRejected = false;
    for (const auto& e : r17.errors) {
        if (e.find("cannot be declared inside a function body") != std::string::npos)
            localTemplateRejected = true;
    }
    CHECK(localTemplateRejected);

    // CIDER-BUG-119: same-named local classes in two functions get distinct
    // mangling prefixes, so their method labels cannot collide; a
    // module-scope class keeps the bare name (legacy mangling).
    {
        cider::ErrorReporter rep;
        cider::Lexer lex("class M { var v: int; }\n"
                         "func a() -> void { class L { var x: int; func f() -> int { return this.x; } } }\n"
                         "func b() -> void { class L { var y: int; func f() -> int { return this.y; } } }\n",
                         rep);
        cider::Parser parser(lex, rep);
        auto prog = parser.parseProgram();
        cider::SymbolTable table;
        cider::ModuleResolver resolver;
        cider::SymbolCollector collector(table, rep, resolver);
        collector.collect(*prog);
        CHECK(rep.errors().empty());
        auto moduleM = std::static_pointer_cast<cider::StructSymbol>(findSym(table, "M"));
        CHECK(moduleM && moduleM->manglePrefix.empty());
        std::vector<std::string> localPrefixes;
        for (const auto& [decl, sym] : collector.getLocalAggregates()) {
            (void)decl;
            if (sym->name != "L") continue;
            CHECK(!sym->manglePrefix.empty());
            CHECK(sym->methods.size() == 1);
            CHECK(sym->methods[0].mangledName == sym->manglePrefix + "_f");
            localPrefixes.push_back(sym->manglePrefix);
        }
        CHECK(localPrefixes.size() == 2);
        CHECK(localPrefixes[0] != localPrefixes[1]);
    }

    // CIDER-BUG-119: a function-local aggregate may not shadow an enclosing
    // type — including a module type declared textually later — because
    // local type identity is the bare written name.
    Result r18 = collectSrc("func f() -> void { struct T { var a: int; } }\n"
                            "struct T { var a: int; var b: int; }\n");
    bool shadowRejected = false;
    for (const auto& e : r18.errors) {
        if (e.find("cannot be shadowed inside a function body") != std::string::npos)
            shadowRejected = true;
    }
    CHECK(shadowRejected);

    // CIDER-BUG-243: a `var` declaration resolves its interface-ness during
    // collection, like every other collected type position. Until now ONLY the
    // class field, the struct field and the function parameter carried the
    // flag, so a global's `sym->type.isInterface` was false and every
    // value-read site had to recover the 16-byte fat pointer by name (the
    // ir_gen/type_checker recoveries added for CIDER-BUG-230/238). The
    // contrast below is the whole bug: same interface, same collector, one
    // position flagged and the other not.
    {
        const std::string iface = "interface Shape {\n  func area() -> int;\n}\n";
        auto isIfaceVar = [](cider::SymbolTable& t, const char* name, bool* found) {
            *found = false;
            for (auto& s : t.getAllSymbols()) {
                if (s->kind != cider::SymbolKind::VARIABLE || s->name != name) continue;
                *found = true;
                return s->type.isInterface;
            }
            return false;
        };
        bool found = false;

        // Interface declared BEFORE the variable: the eager pass resolves it.
        Result early = collectSrc(iface + "var g: Shape;\n");
        CHECK(early.errors.empty());
        CHECK(isIfaceVar(*early.table, "g", &found) && found);

        // Interface declared AFTER the variable: the eager pass has nothing to
        // resolve against, so this is the CIDER-BUG-209 deferred pass (extended
        // to variables) that has to add the flag — declaration order must stop
        // mattering for a variable's representation too.
        Result late = collectSrc("var g: Shape;\n" + iface);
        CHECK(late.errors.empty());
        CHECK(isIfaceVar(*late.table, "g", &found) && found);

        // Alias chain (`type S = Shape`, then alias-of-alias), each hop
        // resolved in its own defining module (CIDER-BUG-078/102/128).
        Result alias = collectSrc(iface + "type S = Shape;\ntype S2 = S;\nvar g: S2;\n");
        CHECK(alias.errors.empty());
        CHECK(isIfaceVar(*alias.table, "g", &found) && found);

        // CIDER-BUG-112 (chain form): an array target anywhere on the alias
        // chain keeps the WHOLE spelling an aggregate. The walk used to
        // overwrite its remembered target at every hop, so `A2 = [S; 2];
        // A3 = A2` ended on the scalar S and flagged the aggregate — a
        // 16-byte interface slot for a 32-byte array, on the field, parameter
        // and variable positions alike.
        Result arrAlias = collectSrc(
            iface + "type S = Shape;\ntype A2 = [S; 2];\ntype A3 = A2;\n"
                    "var g: A3;\nvar h: A2;\nstruct H3 { var f: A3; }\n"
                    "struct H2 { var f: A2; }\nfunc f3(x: A3) -> void {}\n");
        CHECK(arrAlias.errors.empty());
        bool haveG = false, haveH = false;
        CHECK(!isIfaceVar(*arrAlias.table, "g", &haveG) && haveG);
        CHECK(!isIfaceVar(*arrAlias.table, "h", &haveH) && haveH);
        bool sawH3 = false, sawH2 = false, sawF3 = false;
        for (auto& s : arrAlias.table->getAllSymbols()) {
            if ((s->kind == cider::SymbolKind::STRUCT || s->kind == cider::SymbolKind::CLASS) &&
                !s->isTypeAlias && (s->name == "H2" || s->name == "H3")) {
                auto ss = std::static_pointer_cast<cider::StructSymbol>(s);
                CHECK(ss->fields.size() == 1);
                CHECK(!ss->fields[0].type.isInterface);
                // Sized as two 16-byte fat-pointer elements, not one interface.
                CHECK(ss->totalSize == 32);
                if (s->name == "H3") sawH3 = true;
                if (s->name == "H2") sawH2 = true;
            } else if (s->kind == cider::SymbolKind::FUNCTION && s->name == "f3") {
                auto fs = std::static_pointer_cast<cider::FunctionSymbol>(s);
                CHECK(fs->parameterTypes.size() == 1);
                CHECK(!fs->parameterTypes[0].isInterface);
                sawF3 = true;
            }
        }
        CHECK(sawH2 && sawH3 && sawF3);

        // A pointer and an array wrapper are NOT interface values themselves
        // (CIDER-BUG-069/112) — the same rule the field path already follows.
        // A wrongly flagged wrapper would size and store it as one 16-byte
        // interface, so pin the negative side too.
        Result neg = collectSrc(iface + "var p: *Shape;\nvar a: [Shape; 2];\n");
        CHECK(neg.errors.empty());
        bool haveP = false, haveA = false;
        CHECK(!isIfaceVar(*neg.table, "p", &haveP) && haveP);
        CHECK(!isIfaceVar(*neg.table, "a", &haveA) && haveA);

        // The contrast that made the bug visible: one interface, one collector,
        // four positions. The field, the parameter and now the variable all
        // carry the flag; the array wrapper must not.
        Result contrast = collectSrc(
            iface +
            "class C {\n  var f: Shape;\n}\n"
            "struct S {\n  var f: Shape;\n}\n"
            "func take(x: Shape) -> int { return x.area(); }\n"
            "var v: Shape;\n"
            "var arr: [Shape; 2];\n");
        CHECK(contrast.errors.empty());
        bool haveV = false, haveArr = false;
        CHECK(isIfaceVar(*contrast.table, "v", &haveV) && haveV);
        CHECK(!isIfaceVar(*contrast.table, "arr", &haveArr) && haveArr);
        bool haveField = false, haveParam = false;
        for (auto& s : contrast.table->getAllSymbols()) {
            if (s->kind == cider::SymbolKind::CLASS) {
                auto cs = std::static_pointer_cast<cider::StructSymbol>(s);
                CHECK(cs->fields.size() == 1);
                CHECK(cs->fields[0].type.isInterface);
                CHECK(cs->fields[0].offset == 0);
                CHECK(cs->totalSize == 16);
                haveField = true;
            } else if (s->kind == cider::SymbolKind::STRUCT) {
                auto ss = std::static_pointer_cast<cider::StructSymbol>(s);
                CHECK(ss->fields.size() == 1);
                CHECK(ss->fields[0].type.isInterface);
                haveField = true;
            } else if (s->kind == cider::SymbolKind::FUNCTION && s->name == "take") {
                auto fs = std::static_pointer_cast<cider::FunctionSymbol>(s);
                CHECK(fs->parameterTypes.size() == 1);
                CHECK(fs->parameterTypes[0].isInterface);
                haveParam = true;
            }
        }
        CHECK(haveField && haveParam);
    }

    return 0;
}

int main() { return run(); }
