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

// `from prelude import free` names the implicit import (FEATURES §10.2:
// the prelude IS an implicit `from prelude import ...`, so the explicit
// form must mean the same thing). The alias keeps the prelude mark, or the
// linker would look for a module-qualified symbol that is never emitted.
void testPreludeImport() {
    std::printf("the prelude can be named explicitly\n");
    Resolved r = resolve("from prelude import free;\n");
    check(r.ok, "resolves");
    const cider::resolve::Decl* found = find(*r.resolver, "free");
    check(found != nullptr && found->isPrelude, "the alias stays prelude");
    Resolved bad = resolve("from prelude import nosuch;\n");
    check(!bad.ok, "an unknown prelude name is rejected");
    // The mark survives a second hop: m re-exports prelude free, main
    // takes it from m.
    cider::resolve::Resolver::ModuleLoader load = [](const std::string& name)
        -> std::unique_ptr<cider::Program> {
        if (name == "m") {
            cider::Lexer lexer("m.cid", "from prelude import free;\n");
            cider::Parser parser("m.cid", lexer.lex());
            return parser.parse();
        }
        return nullptr;
    };
    Resolved hop = resolve("from m import free;\n", load);
    check(hop.ok, "re-export resolves");
    const cider::resolve::Decl* again = find(*hop.resolver, "free");
    check(again != nullptr && again->isPrelude, "the mark survives the hop");
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
        "var a: [u8; N] = new [u8; N] { [0, ...] };\n");
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

// The prelude is bound without an import: one implicit import, not a hole.
void testPrelude() {
    std::printf("the prelude is bound\n");
    Resolved r = resolve("var g: int = 0;\n");
    check(r.ok, "resolves");
    check(find(*r.resolver, "print") != nullptr, "print is bound");
    check(find(*r.resolver, "alloc") != nullptr, "alloc is bound");
    check(find(*r.resolver, "free") != nullptr, "free is bound");
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

// A class value opens with its vptr word: one int field sits at offset 8
// in a 16-byte object, and a derived class lays its chain out after the
// same single vptr word, most-base first (FEATURES 2.2).
void testClassSize() {
    std::printf("class sizes reserve the vptr\n");
    Resolved r = resolve(
        "class Node { var v: int; }\n"
        "class Base { var x: int; }\n"
        "class Derived : Base { var y: int; }\n");
    check(r.ok, "resolves");
    const cider::resolve::Decl* node =
        r.resolver->lookup(r.resolver->moduleScope(), "Node");
    const cider::resolve::Decl* derived =
        r.resolver->lookup(r.resolver->moduleScope(), "Derived");
    check(r.resolver->classSize(node->id) == 16, "vptr plus one int is sixteen bytes");
    check(r.resolver->fieldOffset(node->id, "v") == 8, "the field follows the vptr");
    check(r.resolver->classSize(derived->id) == 16, "a chain packs after the one vptr");
    check(r.resolver->fieldOffset(derived->id, "x") == 8, "the base field follows the vptr");
    check(r.resolver->fieldOffset(derived->id, "y") == 12, "the own field follows the base");
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
    testMethodInit();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
