// resolve_test.cpp — names bind, or the program is rejected.
//
// LAYERS.md §3.3: every name a program mentions is bound to exactly one Decl,
// or the program is rejected. Module scope is order-independent, imports
// resolve recursively, and a diagnostic names the use. Each test parses real
// source text (proving the layers compose) and then resolves it.

#include <cstdio>
#include <string>

#include "eval/eval.h"
#include "lexer/lexer.h"
#include "parse/parser.h"
#include "resolve/resolver.h"
#include "types/context.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool holds, const char* what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++failures;
    }
}

struct Resolved {
    bool ok = false;
    std::unique_ptr<cider::Program> program;
    std::unique_ptr<cider::TypeContext> types;
    std::unique_ptr<cider::resolve::Resolver> resolver;
};

Resolved resolve(const std::string& text,
                 cider::resolve::Resolver::ModuleLoader load = nullptr) {
    Resolved out;
    out.types = std::make_unique<cider::TypeContext>();
    cider::Lexer lexer("main.cid", text);
    std::vector<cider::Token> tokens = lexer.lex();
    cider::Parser parser("main.cid", std::move(tokens));
    out.program = parser.parse();
    if (!parser.ok() || !lexer.errors().empty()) {
        return out;
    }
    out.resolver = std::make_unique<cider::resolve::Resolver>(*out.types, std::move(load));
    out.ok = out.resolver->resolveProgram(*out.program, "main");
    if (!out.ok) {
        for (const cider::resolve::ResolveError& e : out.resolver->errors()) {
            std::printf("    diagnostic %u:%u %s\n", e.pos.line, e.pos.column,
                        e.message.c_str());
        }
    }
    return out;
}

const cider::resolve::Decl* find(const cider::resolve::Resolver& resolver, const std::string& name) {
    return resolver.lookup(resolver.moduleScope(), name);
}

// Globals resolve to their declared types, whatever the order.
void testGlobals() {
    std::printf("globals resolve in any order\n");
    Resolved r = resolve("var h: u8 = 250;\nvar g: int = 0;\n");
    check(r.ok, "resolves");
    check(cider::typeName(cider::TypeRef{find(*r.resolver, "g")->type}) == "i32",
          "g is i32");
    check(cider::typeName(cider::TypeRef{find(*r.resolver, "h")->type}) == "u8",
          "h is u8");
    check(find(*r.resolver, "missing") == nullptr, "unknown names find nothing");
}

// A second declaration of one name is an error naming it.
void testDuplicate() {
    std::printf("duplicates are rejected\n");
    Resolved r = resolve("var g: int = 0;\nvar g: int = 1;\n");
    check(!r.ok, "fails");
    check(r.resolver->errors().size() == 1, "exactly one diagnostic");
}

// A type that names nothing is an error at the use.
void testUnknownType() {
    std::printf("unknown types are rejected at the use\n");
    Resolved r = resolve("var x: Nosuch = 0;\n");
    check(!r.ok, "fails");
}

// Functions resolve their signatures; bodies wait for the checker.
void testFunc() {
    std::printf("function signatures resolve\n");
    Resolved r = resolve("func add(a: int, b: int) -> int { return a; }\n");
    check(r.ok, "resolves");
    const cider::resolve::Decl* add = find(*r.resolver, "add");
    check(add != nullptr && add->kind == cider::resolve::DeclKind::Func, "add is a function");
    check(add->sig.arity() == 2, "two parameters");
    check(cider::typeName(cider::TypeRef{add->sig.returnType()}) == "i32",
          "returns i32");
}

// `*func(int) -> int` is a FunctionPtr; `func(int) -> int` is a Closure.
void testFuncPtrClosure() {
    std::printf("function pointers and closures are different Kinds\n");
    Resolved r = resolve(
        "var f: *func(int) -> int = nilPtr;\n"
        "func take(c: func(int) -> int) -> void {}\n");
    check(r.ok, "resolves");
    check(find(*r.resolver, "f")->type.type->kind() == cider::Kind::FunctionPtr,
          "*func is a FunctionPtr");
}

// Imports resolve recursively: main -> a -> b resolves b. The v2 hole was a
// `b` that was never resolved and never checked.
void testModules() {
    std::printf("imports resolve recursively\n");
    cider::resolve::Resolver::ModuleLoader load = [](const std::string& name)
        -> std::unique_ptr<cider::Program> {
        if (name == "a") {
            cider::Lexer lexer("a.cid", "from b import v;\nvar w: int = 0;\n");
            cider::Parser parser("a.cid", lexer.lex());
            return parser.parse();
        }
        if (name == "b") {
            cider::Lexer lexer("b.cid", "var v: Nosuch = 0;\n");
            cider::Parser parser("b.cid", lexer.lex());
            return parser.parse();
        }
        return nullptr;
    };
    Resolved r = resolve("from a import w;\n", load);
    check(!r.ok, "b's error surfaces through main");
}

// There is no prelude module: the transitional allocator names are gone,
// and `from prelude import ...` is just an unknown module.
void testPreludeImport() {
    std::printf("the prelude cannot be imported\n");
    Resolved free = resolve("from prelude import free;\n");
    check(!free.ok, "free is not a prelude name");
    Resolved alloc = resolve("from prelude import alloc;\n");
    check(!alloc.ok, "alloc is not a prelude name");
    Resolved noPrint = resolve("from prelude import print;\n");
    check(!noPrint.ok, "print is not a prelude name");
    Resolved noPanic = resolve("from prelude import panic;\n");
    check(!noPanic.ok, "panic is not a prelude name");
}

// An import cycle names the cycle instead of hanging.
void testCycle() {
    std::printf("import cycles are rejected\n");
    cider::resolve::Resolver::ModuleLoader load = [](const std::string& name)
        -> std::unique_ptr<cider::Program> {
        cider::Lexer lexer(name + ".cid",
                           name == "a" ? "from b import x;\n" : "from a import x;\n");
        cider::Parser parser(name + ".cid", lexer.lex());
        return parser.parse();
    };
    Resolved r = resolve("from a import x;\n", load);
    check(!r.ok, "fails");
}

// Array lengths fold constants, and const cycles name the cycle.
void testConstLength() {
    std::printf("lengths fold, cycles are named\n");
    Resolved r = resolve(
        "const N: int = 4;\n"
        "var a: [u8; N] = [0, ...];\n");
    check(r.ok, "resolves");
    check(cider::typeName(cider::TypeRef{find(*r.resolver, "a")->type}) == "[u8; 4]",
          "the length folded");
    Resolved bad = resolve("const A: int = B;\nconst B: int = A;\n");
    check(!bad.ok, "a const cycle fails");
}

// Enumerators count C-style: 0, then previous plus one.
void testEnum() {
    std::printf("enumerators count from zero\n");
    Resolved r = resolve("enum E { A, B = 4, C }\n");
    check(r.ok, "resolves");
    check(r.resolver->enumValue("main.E", "A") == 0, "A is 0");
    check(r.resolver->enumValue("main.E", "B") == 4, "B is 4");
    check(r.resolver->enumValue("main.E", "C") == 5, "C follows B");
}

// Core types are bound without an import; the removed prelude names are
// absent, so a program gets no hidden allocator or I/O.
void testPrelude() {
    std::printf("core types replace the prelude\n");
    Resolved r = resolve("var g: int = 0;\n");
    check(r.ok, "resolves");
    check(find(*r.resolver, "print") == nullptr, "print is not bound");
    check(find(*r.resolver, "panic") == nullptr, "panic is not bound");
    check(find(*r.resolver, "alloc") == nullptr, "alloc is not bound");
    check(find(*r.resolver, "realloc") == nullptr, "realloc is not bound");
    check(find(*r.resolver, "free") == nullptr, "free is not bound");
    check(find(*r.resolver, "Result") != nullptr, "Result is a core type");
    check(find(*r.resolver, "AllocError") != nullptr, "AllocError is a core type");
    Resolved result = resolve("var r: Result<int, AllocError> = uninit;\n");
    check(result.ok, "Result<..., AllocError> resolves");
    Resolved member = resolve("var e: AllocError = AllocError.OutOfMemory;\n");
    check(member.ok, "AllocError.OutOfMemory resolves");
}

// A generic applied with the right arity resolves to its pending marker; the
// wrong arity is an error. Instantiation itself belongs to the checker.
void testGenericArity() {
    std::printf("generic arity is checked at the use\n");
    Resolved r = resolve(
        "struct Box<T> { var v: T; }\n"
        "var b: Box<int> = new Box<int> { v: 1 };\n");
    check(r.ok, "right arity resolves");
    Resolved bad = resolve(
        "struct Box<T> { var v: T; }\n"
        "var b: Box = new Box { v: 1 };\n");
    check(!bad.ok, "missing arguments fail");
}

// An applied generic names a real specialization, not a placeholder.
void testSpecialize() {
    std::printf("applied generics specialize\n");
    Resolved r = resolve(
        "struct Box<T> { var v: T; }\n"
        "var a: Box<int> = new Box<int> { v: 1 };\n"
        "var b: Box<u8> = new Box<u8> { v: 2 };\n");
    check(r.ok, "resolves");
    const cider::resolve::Decl* ta =
        r.resolver->lookup(r.resolver->moduleScope(), "a");
    const cider::resolve::Decl* tb =
        r.resolver->lookup(r.resolver->moduleScope(), "b");
    check(ta->type.type->kind() == cider::Kind::Struct, "an applied generic is a struct");
    check(!(ta->type == tb->type), "two arguments are two types");
    check(cider::typeName(tb->type) != "?", "not the pending marker");
}

// Nominal sizes recurse into member structs instead of answering -1, and a
// struct naming itself answers -1 instead of hanging the resolver.
void testNominalSize() {
    std::printf("nominal sizes recurse\n");
    Resolved r = resolve(
        "struct Inner { var v: int; }\n"
        "struct Outer { var i: Inner; var w: int; }\n");
    check(r.ok, "resolves");
    const cider::resolve::Decl* outer =
        r.resolver->lookup(r.resolver->moduleScope(), "Outer");
    const cider::resolve::Decl* inner =
        r.resolver->lookup(r.resolver->moduleScope(), "Inner");
    check(r.resolver->structSize(inner->id) == 4, "one int is four bytes");
    check(r.resolver->structSize(outer->id) == 8, "nested struct packs flat");
    check(r.resolver->fieldOffset(outer->id, "w") == 4, "second field follows the first");
    check(r.resolver->nominalSize(outer->type.type) == 8, "nominal size sees the struct");
    Resolved cyc = resolve("struct A { var a: A; }\n");
    const cider::resolve::Decl* a =
        cyc.resolver->lookup(cyc.resolver->moduleScope(), "A");
    check(cyc.resolver->structSize(a->id) == -1, "a self-embedding struct has no size");
}

// Under 4.8.5 the vptr word exists only when the chain has virtual slots:
// a concrete-only class opens at offset 0, and a virtual class reserves
// the leading word.
void testClassSize() {
    std::printf("class sizes reserve the vptr only when virtual\n");
    Resolved r = resolve(
        "class Node { var v: int; }\n"
        "class Empty { }\n"
        "class Base { var x: int; }\n"
        "class Derived : Base { var y: int; }\n"
        "class VNode { var v: int; virtual func f() -> int { return 1; } }\n"
        "class VBase { var x: int; virtual func f() -> int { return 1; } }\n"
        "class VDerived : VBase { var y: int; }\n");
    check(r.ok, "resolves");
    const cider::resolve::Decl* node =
        r.resolver->lookup(r.resolver->moduleScope(), "Node");
    const cider::resolve::Decl* derived =
        r.resolver->lookup(r.resolver->moduleScope(), "Derived");
    const cider::resolve::Decl* vnode =
        r.resolver->lookup(r.resolver->moduleScope(), "VNode");
    const cider::resolve::Decl* vderived =
        r.resolver->lookup(r.resolver->moduleScope(), "VDerived");
    check(r.resolver->classSize(node->id) == 4, "a concrete class is just its fields");
    check(r.resolver->fieldOffset(node->id, "v") == 0, "the field opens the object");
    const cider::resolve::Decl* empty =
        r.resolver->lookup(r.resolver->moduleScope(), "Empty");
    check(r.resolver->classSize(empty->id) == 1, "an empty concrete class occupies one byte");
    check(r.resolver->classAlign(empty->id) == 1, "an empty concrete class aligns to its byte");
    check(r.resolver->classSize(derived->id) == 8, "a concrete chain has no header");
    check(r.resolver->fieldOffset(derived->id, "x") == 0, "the base field is first");
    check(r.resolver->fieldOffset(derived->id, "y") == 4, "the own field follows");
    check(r.resolver->classSize(vnode->id) == 16, "vptr plus one int is sixteen bytes");
    check(r.resolver->fieldOffset(vnode->id, "v") == 8, "the field follows the vptr");
    check(r.resolver->classSize(vderived->id) == 16, "a virtual chain packs after the vptr");
    check(r.resolver->fieldOffset(vderived->id, "x") == 8, "the base field follows the vptr");
    check(r.resolver->fieldOffset(vderived->id, "y") == 12, "the own field follows the base");
    check(r.resolver->classAlign(node->id) == 4, "a concrete class aligns to its fields");
    check(r.resolver->classAlign(derived->id) == 4, "a concrete chain aligns to its fields");
    check(r.resolver->classAlign(vnode->id) == 8, "a virtual class aligns to the vptr");
    check(r.resolver->classAlign(vderived->id) == 8, "a virtual chain keeps the vptr alignment");
}

// 4.5: a @packed struct drops every padding byte; an unpacked twin keeps
// natural alignment. The nested u32 proves the packed alignment is one:
// the field after the header sits at the raw byte offset.
void testPackedStruct() {
    std::printf("packed structs drop padding\n");
    Resolved r = resolve(
        "@packed struct EthernetHeader {\n"
        "  dest_mac: [u8; 6];\n"
        "  src_mac: [u8; 6];\n"
        "  ethertype: u16;\n"
        "}\n"
        "struct EthernetLoose {\n"
        "  tag: u8;\n"
        "  ethertype: u16;\n"
        "  dest_mac: [u8; 6];\n"
        "}\n"
        "@packed struct LoosePacked {\n"
        "  tag: u8;\n"
        "  ethertype: u16;\n"
        "  dest_mac: [u8; 6];\n"
        "}\n"
        "@packed struct WithHeader {\n"
        "  var tag: u8;\n"
        "  var eth: EthernetHeader;\n"
        "  var word: u32;\n"
        "}\n");
    check(r.ok, "resolves");
    if (!r.ok) {
        return;
    }
    const cider::resolve::Decl* packed =
        r.resolver->lookup(r.resolver->moduleScope(), "EthernetHeader");
    const cider::resolve::Decl* loose =
        r.resolver->lookup(r.resolver->moduleScope(), "EthernetLoose");
    const cider::resolve::Decl* outer =
        r.resolver->lookup(r.resolver->moduleScope(), "WithHeader");
    const cider::resolve::Decl* tight =
        r.resolver->lookup(r.resolver->moduleScope(), "LoosePacked");
    check(r.resolver->structSize(packed->id) == 14, "a packed struct is its raw sum");
    check(r.resolver->fieldOffset(packed->id, "ethertype") == 12, "packed fields sit byte-exact");
    check(r.resolver->structSize(loose->id) == 10, "an unpacked struct keeps padding");
    check(r.resolver->fieldOffset(loose->id, "ethertype") == 2, "an unpacked field follows its alignment");
    check(r.resolver->structSize(tight->id) == 9, "the packed twin drops the pad byte");
    check(r.resolver->fieldOffset(tight->id, "ethertype") == 1, "the packed twin has no pad byte");
    check(r.resolver->fieldOffset(outer->id, "eth") == 1, "a packed member aligns to one");
    check(r.resolver->fieldOffset(outer->id, "word") == 15, "the packed container packs the tail");
    check(r.resolver->structSize(outer->id) == 19, "the packed container sums raw extents");
}

// 4.1: bitfields pack into storage units of their declared type, earliest
// field at the low bits; a base-type change or a full unit opens a new one.
void testBitfields() {
    std::printf("bitfields pack into storage units\n");
    Resolved r = resolve(
        "struct Flags {\n"
        "  a: int:3;\n"
        "  b: int:5;\n"
        "  c: bool:1;\n"
        "  d: int;\n"
        "}\n"
        "struct Split {\n"
        "  x: u32:30;\n"
        "  y: u32:5;\n"
        "}\n");
    check(r.ok, "resolves");
    if (!r.ok) {
        return;
    }
    const cider::resolve::Decl* flags =
        r.resolver->lookup(r.resolver->moduleScope(), "Flags");
    const cider::resolve::Decl* split =
        r.resolver->lookup(r.resolver->moduleScope(), "Split");
    int64_t bit = -1;
    int64_t width = -1;
    check(r.resolver->structSize(flags->id) == 12, "two units and the tail pack to twelve");
    check(r.resolver->fieldOffset(flags->id, "a") == 0, "the first unit opens the struct");
    check(r.resolver->fieldOffset(flags->id, "b") == 0, "the second bitfield shares the unit");
    check(r.resolver->fieldOffset(flags->id, "c") == 4, "a new base type opens a new unit");
    check(r.resolver->fieldOffset(flags->id, "d") == 8, "the plain field follows the units");
    check(r.resolver->bitField(flags->id, "a", bit, width) && bit == 0 && width == 3,
          "a takes the low three bits");
    check(r.resolver->bitField(flags->id, "b", bit, width) && bit == 3 && width == 5,
          "b follows a in the same unit");
    check(!r.resolver->bitField(flags->id, "d", bit, width), "a plain field is not a bitfield");
    check(r.resolver->structSize(split->id) == 8, "a full unit pushes the next field up");
    check(r.resolver->fieldOffset(split->id, "y") == 4, "the overflow field opens a second unit");
}

// 8.4: the msabi/sysv spellings ride the signature and the function
// pointer type, so calls, externs and fn-ptr values agree on a row.
void testAbiSpellings() {
    std::printf("abi spellings ride signatures\n");
    Resolved r = resolve(
        "msabi func ms(x: int) -> int { return x; }\n"
        "sysv func sv(x: int) -> int { return x; }\n"
        "msabi extern func ef(x: int) -> int;\n"
        "var g: *msabi func(x: int) -> int = nilPtr;\n");
    check(r.ok, "resolves");
    if (!r.ok) {
        return;
    }
    const cider::resolve::Decl* ms =
        r.resolver->lookup(r.resolver->moduleScope(), "ms");
    const cider::resolve::Decl* sv =
        r.resolver->lookup(r.resolver->moduleScope(), "sv");
    const cider::resolve::Decl* ef =
        r.resolver->lookup(r.resolver->moduleScope(), "ef");
    const cider::resolve::Decl* g =
        r.resolver->lookup(r.resolver->moduleScope(), "g");
    check(ms != nullptr && ms->sig.abi() == cider::Abi::MsAbi,
          "a msabi function carries the ABI");
    check(sv != nullptr && sv->sig.abi() == cider::Abi::SysV,
          "a sysv function stays on SysV");
    check(ef != nullptr && ef->sig.abi() == cider::Abi::MsAbi,
          "a msabi extern carries the ABI");
    check(g != nullptr && !g->type.isError() &&
              g->type.type->kind() == cider::Kind::FunctionPtr &&
              g->type.type->signature() != nullptr &&
              g->type.type->signature()->abi() == cider::Abi::MsAbi,
          "a msabi function pointer type carries the ABI");
    const std::string spelled =
        g != nullptr && !g->type.isError() ? cider::typeName(g->type) : "";
    check(spelled.find("msabi") != std::string::npos,
          "the ABI shows in the type's name");
}

void testMethodInit() {
    std::printf("init and deinit signatures are recorded\n");
    Resolved r = resolve(
        "class Counter { var v: int;\n"
        " func init(n: int) { (*self).v = n; } }\n");
    check(r.ok, "resolves");
    const cider::resolve::Decl* counter =
        r.resolver->lookup(r.resolver->moduleScope(), "Counter");
    const cider::Signature* init = r.resolver->methodSig(counter->id, "init");
    check(init != nullptr, "init has a signature");
    check(init != nullptr && init->arity() == 1, "init takes one argument");
}

}  // namespace

// 4.8.3: a virtual hierarchy's table carries the destructor slot first, so
// delete can dispatch through slot zero; user virtuals keep their order
// after it, and a chain with no virtuals forms no table at all.
void testDeleteSlot() {
    std::printf("virtual slots carry the destructor first\n");
    Resolved r = resolve("class Shape {\n"
                         "    var w: int;\n"
                         "    virtual func area() -> int { return this->w; }\n"
                         "    func init() { this->w = 1; }\n"
                         "}\n"
                         "class Circle : Shape {\n"
                         "    override func area() -> int { return this->w; }\n"
                         "}\n");
    check(r.ok, "resolves");
    const cider::resolve::Decl* shape = find(*r.resolver, "Shape");
    const cider::resolve::Decl* circle = find(*r.resolver, "Circle");
    std::vector<std::string> slots = r.resolver->virtualNames(shape->id);
    std::vector<std::string> want{"deinit", "area"};
    check(slots == want, "deinit rides slot zero");
    check(r.resolver->virtualIndexOf(circle->id, "area") == 1,
          "the inherited override keeps the shifted slot");
    Resolved plain = resolve("class Data {\n"
                             "    var a: int;\n"
                             "    func init() { this->a = 1; }\n"
                             "}\n");
    check(plain.ok, "a plain class resolves");
    check(plain.resolver->virtualNames(find(*plain.resolver, "Data")->id).empty(),
          "a chain with no virtuals keeps no table");
}

// 4.8: several bases lay out one after another in declaration order, each
// subobject at its own alignment; the offsets compose through every path,
// a name two bases provide is ambiguous, and only the first base may carry
// the table.
void testMultipleBases() {
    std::printf("several bases lay out in declaration order\n");
    Resolved r = resolve("class A {\n"
                         "    var a: int;\n"
                         "    func fromA() -> int { return this->a; }\n"
                         "}\n"
                         "class B {\n"
                         "    var b: i64;\n"
                         "    func fromB() -> int { return this->b as int; }\n"
                         "}\n"
                         "class C : A, B {\n"
                         "    var c: int;\n"
                         "}\n");
    check(r.ok, "resolves");
    const cider::resolve::Decl* a = find(*r.resolver, "A");
    const cider::resolve::Decl* b = find(*r.resolver, "B");
    const cider::resolve::Decl* c = find(*r.resolver, "C");
    check(r.resolver->fieldOffset(a->id, "a") == 0,
          "the first base starts at zero");
    check(r.resolver->fieldOffset(b->id, "b") == 0,
          "each base's own view starts at zero");
    check(r.resolver->baseOffset(c->id, a->id) == 0,
          "the first base sits at zero");
    check(r.resolver->baseOffset(c->id, b->id) == 8,
          "the second base follows at its alignment");
    check(r.resolver->fieldOffset(c->id, "a") == 0, "a keeps its place");
    check(r.resolver->fieldOffset(c->id, "b") == 8, "b reads through the base");
    check(r.resolver->fieldOffset(c->id, "c") == 16, "c follows the bases");
    check(r.resolver->classSize(c->id) == 24, "the whole object rounds to 24");
    std::optional<cider::DeclId> homeB = r.resolver->methodHome(c->id, "fromB");
    check(homeB.has_value() && sameDecl(*homeB, b->id),
          "a method homes in the base that declares it");
    check(!r.resolver->methodAmbiguous(c->id, "fromB"),
          "one base providing a name is not ambiguous");

    Resolved amb = resolve("class L {\n"
                           "    var v: int;\n"
                           "    func pick() -> int { return this->v; }\n"
                           "}\n"
                           "class R {\n"
                           "    var v: int;\n"
                           "    func pick() -> int { return this->v; }\n"
                           "}\n"
                           "class D : L, R {\n"
                           "    var d: int;\n"
                           "}\n");
    check(amb.ok, "the ambiguous shape still resolves");
    const cider::resolve::Decl* d = find(*amb.resolver, "D");
    check(amb.resolver->methodAmbiguous(d->id, "pick"),
          "two bases declaring the method are ambiguous");
    check(amb.resolver->fieldAmbiguous(d->id, "v"),
          "two bases declaring the field are ambiguous");
    check(amb.resolver->fieldOffset(d->id, "v") == -1,
          "an ambiguous field has no single slot");
    Resolved diamond = resolve("class Root {\n"
                               "    var r: int;\n"
                               "}\n"
                               "class Left : Root {\n"
                               "    var l: int;\n"
                               "}\n"
                               "class Right : Root {\n"
                               "    var rr: int;\n"
                               "}\n"
                               "class Tip : Left, Right {\n"
                               "    var t: int;\n"
                               "}\n");
    check(diamond.ok, "the diamond resolves");
    const cider::resolve::Decl* root = find(*diamond.resolver, "Root");
    const cider::resolve::Decl* tip = find(*diamond.resolver, "Tip");
    check(diamond.resolver->baseAmbiguous(tip->id, root->id),
          "two paths to one ancestor are ambiguous");
    check(!diamond.resolver->baseOffset(tip->id, root->id).has_value(),
          "an ambiguous base answers no offset");

    Resolved late = resolve("class P {\n"
                            "    var p: int;\n"
                            "}\n"
                            "class H {\n"
                            "    var h: int;\n"
                            "    virtual func f() -> int { return this->h; }\n"
                            "}\n"
                            "class X : P, H {\n"
                            "    var x: int;\n"
                            "}\n");
    check(late.ok, "the late-headed shape resolves");
    const cider::resolve::Decl* x = find(*late.resolver, "X");
    check(late.resolver->classSize(x->id) == -1,
          "a later headed base refuses the layout");

    Resolved adds = resolve("class Plain {\n"
                            "    var p: int;\n"
                            "}\n"
                            "class V : Plain {\n"
                            "    var v: int;\n"
                            "    virtual func f() -> int { return this->v; }\n"
                            "}\n");
    check(adds.ok, "a derived table over a plain base resolves");
    const cider::resolve::Decl* plain = find(*adds.resolver, "Plain");
    const cider::resolve::Decl* v = find(*adds.resolver, "V");
    check(adds.resolver->baseOffset(v->id, plain->id) == 8,
          "the plain base shifts past the shared table word");
    check(adds.resolver->fieldOffset(v->id, "p") == 8,
          "its field reads through the shift");
    check(adds.resolver->classSize(v->id) == 16,
          "the object keeps both words");
}

int main() {
    std::printf("resolve_test — names bind, or the program is rejected\n\n");
    testGlobals();
    testDuplicate();
    testUnknownType();
    testFunc();
    testFuncPtrClosure();
    testModules();
    testCycle();
    testPreludeImport();
    testConstLength();
    testEnum();
    testPrelude();
    testGenericArity();
    testSpecialize();
    testNominalSize();
    testClassSize();
    testPackedStruct();
    testBitfields();
    testAbiSpellings();
    testMethodInit();
    testDeleteSlot();
    testMultipleBases();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
