// check_test.cpp — well-typed programs check, the rest are rejected.
//
// The checker writes each expression's type and checks it in place. Every test
// parses real source, resolves it, then checks it — a failure at an earlier
// layer fails the test for that layer's reason, so a passing test proves all
// three.

#include <cstdio>
#include <string>

#include "check/checker.h"
#include "lexer/lexer.h"
#include "parse/parser.h"
#include "types/context.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool holds, const std::string& what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++failures;
    }
}

struct Checked {
    bool ok = false;
    std::string stage;
    std::unique_ptr<cider::TypeContext> types;
    std::unique_ptr<cider::Program> program;
    std::unique_ptr<cider::resolve::Resolver> resolver;
    std::unique_ptr<cider::check::Checker> checker;
};

Checked check(const std::string& text) {
    Checked out;
    out.types = std::make_unique<cider::TypeContext>();
    cider::Lexer lexer("test.cid", text);
    std::vector<cider::Token> tokens = lexer.lex();
    if (!lexer.errors().empty()) {
        out.stage = "lex";
        return out;
    }
    cider::Parser parser("test.cid", std::move(tokens));
    out.program = parser.parse();
    if (!parser.ok()) {
        out.stage = "parse";
        return out;
    }
    out.resolver = std::make_unique<cider::resolve::Resolver>(*out.types);
    if (!out.resolver->resolveProgram(*out.program, "main")) {
        out.stage = "resolve";
        return out;
    }
    out.checker = std::make_unique<cider::check::Checker>(*out.types, *out.resolver);
    out.ok = out.checker->checkProgram(*out.program, "main");
    if (!out.ok) {
        out.stage = "check";
        for (const cider::check::CheckError& e : out.checker->errors()) {
            std::printf("    diagnostic %u:%u %s\n", e.pos.line, e.pos.column,
                        e.message.c_str());
        }
    }
    return out;
}

// Mixed arithmetic converts to a common type, stated as a type.
void testArithmetic() {
    std::printf("arithmetic converts, then checks\n");
    Checked good =
        check("func f(a: int, b: int) -> int { return a + b * 2; }\n");
    check(good.ok, "checks: " + good.stage);
    Checked promo =
        check("func f(a: u8) -> i32 { return a + 10; }\n");
    check(promo.ok, "u8 + int is i32: " + promo.stage);
    Checked bad = check("func f() -> int { return 1.5; }\n");
    check(!bad.ok && bad.stage == "check", "a float is not an int");
    Checked narrow = check("func f() { var x: u8 = 300; }\n");
    check(!narrow.ok && narrow.stage == "check", "300 does not fit u8");
}

// Names resolve through locals first, then the module scope.
void testNames() {
    std::printf("names bind inside out\n");
    Checked good = check(
        "var g: int = 1;\n"
        "func f() -> int { var g: int = 2; return g; }\n");
    check(good.ok, "a local shadows a global: " + good.stage);
    Checked bad = check("func f() -> int { return x; }\n");
    check(!bad.ok && bad.stage == "check", "an unbound name fails");
    Checked param = check("func f(a: int) -> int { return a; }\n");
    check(param.ok, "parameters bind: " + param.stage);
}

// Calls check arity and argument types; dereference is explicit.
void testCalls() {
    std::printf("calls check arity, dereference is explicit\n");
    Checked arity = check(
        "func g(a: int) -> int { return a; }\n"
        "func f() -> int { return g(1, 2); }\n");
    check(!arity.ok && arity.stage == "check", "two arguments for one parameter fails");
    Checked ptr = check("func f(p: *int) -> int { return p; }\n");
    check(!ptr.ok && ptr.stage == "check", "a pointer is not an int");
    Checked deref = check("func f(p: *int) -> int { return (*p); }\n");
    check(deref.ok, "a dereference is: " + deref.stage);
    Checked direct = check(
        "func g(a: int) -> int { return a; }\n"
        "func f() -> int { return g(1); }\n");
    check(direct.ok, "a direct call checks: " + direct.stage);
}

// A struct with every method converts; without one it does not.
void testInterface() {
    std::printf("concrete converts to interface when it implements it\n");
    Checked good = check(
        "interface Drawable { func draw() -> string; }\n"
        "struct Rect : Drawable { var w: int; var h: int;\n"
        " func draw() -> string { return \"r\"; } }\n"
        "func d(r: Rect) -> Drawable { return r; }\n");
    check(good.ok, "checks: " + good.stage);
    Checked bad = check(
        "interface Drawable { func draw() -> string; }\n"
        "struct Point { var x: int; var y: int; }\n"
        "func d(p: Point) -> Drawable { return p; }\n");
    check(!bad.ok && bad.stage == "check", "without draw it fails");
}

// Conditions are bool, and the branches of a ternary match.
void testControl() {
    std::printf("control flow checks its conditions\n");
    Checked cond = check("func f() -> void { while (1) { } }\n");
    check(!cond.ok && cond.stage == "check", "1 is not a condition");
    Checked tern = check("func f(c: bool) -> int { return c ? 1 : 2; }\n");
    check(tern.ok, "matching branches check: " + tern.stage);
    Checked mixed = check("func f(c: bool) -> int { return c ? 1 : 1.5; }\n");
    check(!mixed.ok && mixed.stage == "check", "mismatched branches fail");
    Checked brk = check("func f() -> void { break; }\n");
    check(!brk.ok && brk.stage == "check", "break outside a loop fails");
}

void testThrowRefused() {
    std::printf("unimplemented exceptions refuse loudly\n");
    Checked th = check("func f() -> int { throw 42; return 0; }\n");
    check(!th.ok && th.stage == "check", "throw refuses until transport lands");
    Checked tr = check("func f() -> int { try { return 1; } catch { return 0; } }\n");
    check(!tr.ok && tr.stage == "check", "try refuses until transport lands");
}

void testLoopMatchRefused() {
    std::printf("unimplemented control flow refuses loudly\n");
    // Lowering landed for `for` (FEAT-042), so the pure-check path accepts it.
    Checked fo =
        check("func f() -> int { var s: int = 0; for (var i: int = 0; i < 10; i = i + 1) { s = s + 1; } return s; }\n");
    check(fo.ok, "for checks: " + fo.stage);
    // for-in and match lowerings have not landed yet.
    Checked fi =
        check("func f(a: [int; 3]) -> int { var s: int = 0; for (var x in a) { s = s + x; } return s; }\n");
    check(fi.ok, "for-in checks now that lowering lands it");
    Checked ma =
        check("func f(x: int) -> int { match x { 1 => { return 10; } default => { return 30; } } }\n");
    check(!ma.ok && ma.stage == "check", "match refuses until dispatch lands");
}

// A generic call checks its body with the arguments substituted.
void testGenericCall() {
    std::printf("generic calls substitute\n");
    Checked good = check(
        "func max<T>(a: T, b: T) -> T { return a > b ? a : b; }\n"
        "func main() -> int { return max::<int>(3, 9); }\n");
    check(good.ok, "checks: " + good.stage);
    Checked bad = check(
        "func max<T>(a: T, b: T) -> T { return a > b ? a : b; }\n"
        "func main() -> int { return max::<int>(3, \"x\"); }\n");
    check(!bad.ok && bad.stage == "check", "a wrong argument fails");
    Checked bare = check(
        "func max<T>(a: T, b: T) -> T { return a > b ? a : b; }\n"
        "func main() -> int { return max; }\n");
    check(!bare.ok && bare.stage == "check", "a bare generic name fails");
}

// A generic struct stores per-instantiation members; methods check too.
void testGenericStruct() {
    std::printf("generic structs specialize\n");
    Checked good = check(
        "struct Box<T> { var v: T; }\n"
        "func main() -> int { var b: Box<int> = new Box<int> { v: 41 }; return b.v + 1; }\n");
    check(good.ok, "checks: " + good.stage);
    Checked method = check(
        "struct Box<T> { var v: T; func get() -> T { return (*self).v; } }\n"
        "func main() -> int { var b: Box<int> = new Box<int> { v: 41 }; return b.get(); }\n");
    check(method.ok, "methods specialize: " + method.stage);
    Checked bareLocal = check(
        "struct Box<T> { var v: T; }\n"
        "func main() -> int { var b: Box = new Box<int> { v: 1 }; return 0; }\n");
    check(!bareLocal.ok && bareLocal.stage == "check", "a bare generic local fails");
}

// A union constructs by naming one live member; hasM asks about liveness;
// a cast wider than the payload is rejected, not read past.
void testUnions() {
    std::printf("unions name one live member\n");
    Checked good = check(
        "union Value { var i: int; var s: string; }\n"
        "func main() -> int { var v: Value = new Value { i: 42 }; return v.i; }\n");
    check(good.ok, "checks: " + good.stage);
    Checked two = check(
        "union Value { var i: int; var s: string; }\n"
        "func main() -> int { var v: Value = new Value { i: 1, s: \"x\" }; return 0; }\n");
    check(!two.ok && two.stage == "check", "two members fail");
    Checked member = check(
        "union Value { var i: int; }\n"
        "func main() -> int { var v: Value = new Value { z: 1 }; return 0; }\n");
    check(!member.ok && member.stage == "check", "an unknown member fails");
    Checked has = check(
        "union Value { var i: int; var s: string; }\n"
        "func main() -> int { var v: Value = new Value { i: 1 }; if (v.hasS()) { return 1; } return 0; }\n");
    check(has.ok, "hasM checks: " + has.stage);
    Checked hasBad = check(
        "union Value { var i: int; }\n"
        "func main() -> int { var v: Value = new Value { i: 1 }; return v.hasZ(); }\n");
    check(!hasBad.ok && hasBad.stage == "check", "hasM of nothing fails");
    Checked wide = check(
        "union Small { var b: int; }\n"
        "func main() -> int { var v: Small = new Small { b: 1 }; var s: string = v as string; return 0; }\n");
    check(!wide.ok && wide.stage == "check", "a cast wider than the payload fails");
}

// A closure literal checks against its spelling, in its own scope.
void testFuncLitChecks() {
    std::printf("closure literals check\n");
    Checked good = check(
        "func main() -> int {\n"
        "    var f: func(int) -> int = func(x: int) -> int { return x + 1; };\n"
        "    return 0;\n"
        "}\n");
    check(good.ok, "a matching literal checks: " + good.stage);
    Checked bad = check(
        "func main() -> int {\n"
        "    var f: func(int) -> int = func(x: int) -> int { return true; };\n"
        "    return 0;\n"
        "}\n");
    check(!bad.ok && bad.stage == "check", "a mistyped body fails");
}

// Length reads the element count of sized values.
void testLen() {
    std::printf("len reads element counts\n");
    Checked slice = check(
        "func main() -> int {\n"
        "    var a: [int; 3] = new [int; 3] { 1, 2, 3 };\n"
        "    var s: []int = a[0:2];\n"
        "    return s.len as int;\n"
        "}\n");
    check(slice.ok, "a slice has a length");
    Checked array = check(
        "func main() -> int {\n"
        "    var a: [int; 3] = new [int; 3] { 1, 2, 3 };\n"
        "    return a.len as int;\n"
        "}\n");
    check(array.ok, "an array has a length");
    Checked string = check(
        "func main() -> int { var s: string = \"hey\"; return s.len as int; }\n");
    check(string.ok, "a string has a length");
    Checked scalar = check("func main() -> int { var x: int = 3; return x.len; }\n");
    check(!scalar.ok && scalar.stage == "check", "a scalar has no length");
}

// The address of a plain function is a function pointer, not a closure.
void testFnAddr() {
    std::printf("function addresses are function pointers\n");
    Checked good = check(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int {\n"
        "    var f: *func(int, int) -> int = &add;\n"
        "    return (*f)(20, 22);\n"
        "}\n");
    check(good.ok, "address of a function binds to *func: " + good.stage);
    Checked nilCall = check(
        "func main() -> int {\n"
        "    var f: *func(int) -> int = nilPtr;\n"
        "    return (*f)(1);\n"
        "}\n");
    check(nilCall.ok, "nilPtr converts to *func: " + nilCall.stage);
}

// Every `{placeholder}` in an asm block must name something in scope: a
// local, a parameter, or a module-level declaration. Anything else fails
// at check time with the name in the message.
void testAsm() {
    std::printf("asm placeholders resolve\n");
    Checked good = check(
        "var g: int = 5;\n"
        "func main() -> int {\n"
        "    var sum: int = 3;\n"
        "    asm { \"mov edi, {sum}\" \"mov eax, {g}\" }\n"
        "    return 0;\n"
        "}\n");
    check(good.ok, "bound placeholders check: " + good.stage);
    Checked param = check(
        "func f(count: int) -> void { asm { \"mov edi, {count}\" } }\n");
    check(param.ok, "a parameter placeholder checks: " + param.stage);
    Checked bad = check(
        "func main() -> int { asm { \"mov edi, {nope}\" } return 0; }\n");
    check(!bad.ok && bad.stage == "check", "an unbound placeholder fails");
}

// `++i` needs an integer place; floats and rvalues are rejected where used.
void testIncrDecr() {
    std::printf("increment and decrement check\n");
    Checked good = check(
        "func main() -> int {\n"
        "    var i: int = 1;\n"
        "    var p: *int = &i;\n"
        "    ++i; i--; --(*p);\n"
        "    return i;\n"
        "}\n");
    check(good.ok, "integer places check: " + good.stage);
    Checked fp = check(
        "func main() -> int { var f: f32 = 1.0; ++f; return 0; }\n");
    check(!fp.ok && fp.stage == "check", "a float operand fails");
    Checked rvalue = check(
        "func main() -> int { var i: int = 1; ++(i + 1); return 0; }\n");
    check(!rvalue.ok && rvalue.stage == "check", "an rvalue fails");
}

// Bool equality compares values; ordering on bools has no meaning.
void testBoolCompare() {
    std::printf("bools compare for equality\n");
    Checked good = check(
        "func main() -> int {\n"
        "    var b: bool = true;\n"
        "    if (b == true && b != false) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(good.ok, "== and != on bools: " + good.stage);
    Checked global = check(
        "var g: bool = false;\n"
        "func main() -> int {\n"
        "    g = !g;\n"
        "    if (g != true) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(global.ok, "a global bool negates and compares: " + global.stage);
    Checked rel = check(
        "func main() -> int { var b: bool = true < false; return 0; }\n");
    check(!rel.ok && rel.stage == "check", "ordering on bools fails");
    Checked mixed = check(
        "func main() -> int { var b: bool = true == 1; return 0; }\n");
    check(!mixed.ok && mixed.stage == "check", "bool == int fails");
}

}  // namespace

int main() {
    std::printf("check_test — well-typed programs check\n\n");
    testArithmetic();
    testNames();
    testCalls();
    testInterface();
    testGenericCall();
    testGenericStruct();
    testUnions();
    testFnAddr();
    testIncrDecr();
    testBoolCompare();
    testAsm();
    testLen();
    testFuncLitChecks();
    testControl();
    testThrowRefused();
    testLoopMatchRefused();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
