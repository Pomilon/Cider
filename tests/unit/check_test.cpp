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
#include "target/target.h"
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

Checked checkAs(const std::string& text, cider::target::Kind target,
                cider::resolve::Resolver::ModuleLoader load = nullptr) {
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
    out.resolver = std::make_unique<cider::resolve::Resolver>(
        *out.types, std::move(load), target);
    if (!out.resolver->resolveProgram(*out.program, "main")) {
        out.stage = "resolve";
        return out;
    }
    out.checker = std::make_unique<cider::check::Checker>(*out.types, *out.resolver, target);
    // Whole-program provenance before checking (single module here),
    // mirroring the driver: callee bodies see caller proofs.
    out.checker->prescanProvenance({"main"});
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

Checked check(const std::string& text,
              cider::resolve::Resolver::ModuleLoader load = nullptr) {
    return checkAs(text, cider::target::Kind::Native, std::move(load));
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
void testGlobalZero() {
    // A zero repeat names every byte (zero), so it is a constant a global
    // may hold: no startup code, no order problem (FEATURES §8.3).
    std::printf("a zero-filled array may live in a global\n");
    Checked zero = check(
        "var g: [u8; 3] = [0, ...];\n"
        "func main() -> int { return g[0] as int; }\n");
    check(zero.ok, "a zero-repeat global checks: " + zero.stage);
    Checked nested = check(
        "var g: [[u8; 2]; 2] = [[0, ...], [0, ...]];\n"
        "func main() -> int { return 0; }\n");
    check(nested.ok, "a nested zero-repeat global checks: " + nested.stage);
    Checked explicitInit = check(
        "var g: [u8; 3] = [1, 2, 3];\n"
        "func main() -> int { return 0; }\n");
    check(explicitInit.ok, "a literal global array checks: " + explicitInit.stage);
    Checked runtimeInit = check(
        "func f() -> u8 { return 1; }\n"
        "var g: [u8; 2] = [f(), 8];\n"
        "func main() -> int { return 0; }\n");
    check(!runtimeInit.ok && runtimeInit.stage == "check",
          "a runtime element still needs a constant");
    // Honest new (Unified Standard 5): `new [T; N] { … }` is a heap value,
    // and a heap allocation is not a constant, so a global refuses it.
    Checked heapGlobal = check(
        "var g: [u8; 3] = new [u8; 3] {[0, ...]};\n"
        "func main() -> int { return 0; }\n");
    check(!heapGlobal.ok && heapGlobal.stage == "check",
          "a heap array is refused as a global initialiser: " + heapGlobal.stage);
}

void testGlobalStruct() {
    // A struct value with every field constant folds to image bytes,
    // so it may live in a global like an array can (FEATURES §8.3).
    std::printf("a const struct may live in a global\n");
    Checked good = check(
        "struct Point { var x: int; var y: int; }\n"
        "var g: Point = Point { x: 3, y: 4 };\n"
        "func main() -> int { return g.x; }\n");
    check(good.ok, "a const struct global checks: " + good.stage);
    Checked runtimeField = check(
        "struct Point { var x: int; var y: int; }\n"
        "func f() -> int { return 1; }\n"
        "var g: Point = Point { x: f(), y: 4 };\n"
        "func main() -> int { return 0; }\n");
    check(!runtimeField.ok && runtimeField.stage == "check",
          "a runtime field still needs a constant");
    Checked ctorCall = check(
        "class Box { var v: int; }\n"
        "func make(v: int) -> Box { return new Box { v: v }; }\n"
        "var g: Box = make(1);\n"
        "func main() -> int { return 0; }\n");
    check(!ctorCall.ok && ctorCall.stage == "check",
          "a call result still needs a constant");
    Checked nilField = check(
        "struct Box { var p: *int; var q: *int; }\n"
        "var g: Box = Box { p: nilPtr, q: nilPtr };\n"
        "func main() -> int { return 0; }\n");
    check(nilField.ok, "a nil pointer field is constant: " + nilField.stage);
    Checked enumGlobal = check(
        "enum Color { RED, GREEN }\n"
        "var g: Color = Color.GREEN;\n"
        "func main() -> int { return g as int; }\n");
    check(enumGlobal.ok, "an enumerator folds into a global: " + enumGlobal.stage);
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
    // CIDER-BUG-255: an aggregate in a scalar interface position must not
    // bind its first element. v3 has no bare list literal, so the nearest
    // shapes are an array literal and a slice, in declaration and argument
    // position alike; all three are refused at check time.
    Checked arrDecl = check(
        "interface Shape { func area() -> int; }\n"
        "class Rect { var w: int; var h: int;\n"
        " func init(w: int, h: int) -> void { (*self).w = w; (*self).h = h; }\n"
        " func area() -> int { return (*self).w * (*self).h; } }\n"
        "func main() -> int {\n"
        "    var r: *Rect = new Rect(3, 4);\n"
        "    var i: Shape = new [*Rect; 2] {r, r};\n"
        "    return 0;\n"
        "}\n");
    check(!arrDecl.ok && arrDecl.stage == "check", "an array is not a scalar interface");
    Checked arrArg = check(
        "interface Shape { func area() -> int; }\n"
        "class Rect { var w: int; var h: int;\n"
        " func init(w: int, h: int) -> void { (*self).w = w; (*self).h = h; }\n"
        " func area() -> int { return (*self).w * (*self).h; } }\n"
        "func get(s: Shape) -> int { return s.area(); }\n"
        "func main() -> int {\n"
        "    var r: *Rect = new Rect(3, 4);\n"
        "    return get(new [*Rect; 2] {r, r});\n"
        "}\n");
    check(!arrArg.ok && arrArg.stage == "check", "an array argument is not a scalar interface");
    Checked sliceArg = check(
        "interface Shape { func area() -> int; }\n"
        "class Rect { var w: int; var h: int;\n"
        " func init(w: int, h: int) -> void { (*self).w = w; (*self).h = h; }\n"
        " func area() -> int { return (*self).w * (*self).h; } }\n"
        "func get(s: Shape) -> int { return s.area(); }\n"
        "func main() -> int {\n"
        "    var a: [*Rect; 2] = [new Rect(3, 4), new Rect(1, 2)];\n"
        "    return get(a[0:2]);\n"
        "}\n");
    check(!sliceArg.ok && sliceArg.stage == "check", "a slice is not a scalar interface");
    // TYPES §4.3: a class value boxes by address, so returning one as an
    // interface would dangle the dead frame. Small structs inline and stay
    // legal (see above); class values refuse at the return site, where the
    // escape happens. The pointer form stays the legal spelling.
    Checked classValueReturn = check(
        "interface Shape { func area() -> int; }\n"
        "class Sq { var s: int;\n"
        " func area() -> int { return (*self).s * (*self).s; } }\n"
        "func f(c: Sq) -> Shape { return c; }\n");
    check(!classValueReturn.ok && classValueReturn.stage == "check",
          "a class value cannot return as an interface");
    Checked classPtrReturn = check(
        "interface Shape { func area() -> int; }\n"
        "class Sq { var s: int;\n"
        " func area() -> int { return (*self).s * (*self).s; } }\n"
        "func f(c: *Sq) -> Shape { return c; }\n");
    check(classPtrReturn.ok, "a class pointer returns as one: " + classPtrReturn.stage);
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

// v4 removed try/catch/throw from the grammar, so the checker must never see
// them. The old forms fail before any semantic question is asked.
void testExceptionSyntaxRemoved() {
    std::printf("exception syntax is gone\n");
    Checked thrown = check("func f() -> int { throw 1; return 0; }\n");
    check(!thrown.ok && thrown.stage == "parse",
          "throw fails at parse");
    Checked caught = check(
        "func f() -> int { try { return 1; } catch { return 0; } }\n");
    check(!caught.ok && caught.stage == "parse",
          "try/catch fails at parse");
}

void testOperatorOverloads() {
    std::printf("free binary operator overloading\n");
    Checked good = check(
        "struct Vec { var x: int; var y: int; }\n"
        "func operator+(a: Vec, b: Vec) -> Vec {\n"
        "    return Vec { x: a.x + b.x, y: a.y + b.y };\n"
        "}\n"
        "func main() -> int {\n"
        "    var a: Vec = Vec { x: 1, y: 2 };\n"
        "    var b: Vec = Vec { x: 3, y: 4 };\n"
        "    var c: Vec = a + b;\n"
        "    return c.x + c.y;\n"
        "}\n");
    check(good.ok, "a free operator on a struct checks: " + good.stage);

    Checked mismatch = check(
        "struct Vec { var x: int; var y: int; }\n"
        "func operator+(a: Vec, b: Vec) -> Vec { return a; }\n"
        "func main() -> int {\n"
        "    var a: Vec = Vec { x: 1, y: 2 };\n"
        "    var c: Vec = a + 1;\n"
        "    return c.x;\n"
        "}\n");
    check(!mismatch.ok && mismatch.stage == "check",
          "an operator call needs exact operand types");

    Checked noNominal = check(
        "func operator+(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int { return 1 + 2; }\n");
    check(!noNominal.ok && noNominal.stage == "check",
          "an operator declaration needs a class, struct or enum operand");

    Checked member = check(
        "class C { var x: int;\n"
        "    func operator+(a: C) -> C { return a; }\n"
        "}\n");
    check(!member.ok && member.stage == "check",
          "member operator overloading is not implemented yet");
}

void testInitAssign() {
    std::printf("init assigns every field\n");
    Checked gap = check(
        "class C { var x: int; var y: int;\n"
        "    func init(v: int) -> void { (*self).x = v; } }\n"
        "func main() -> int { return 0; }\n");
    check(!gap.ok && gap.stage == "check", "an init leaving a field unassigned fails");
    Checked whole = check(
        "class C { var x: int; var y: int;\n"
        "    func init(v: int) -> void { (*self).x = v; (*self).y = v; } }\n"
        "func main() -> int { return 0; }\n");
    check(whole.ok, "a full init checks: " + whole.stage);
    Checked branch = check(
        "class C { var x: int; var y: int;\n"
        "    func init(v: int) -> void {\n"
        "        if (v > 0) { (*self).x = v; } else { (*self).x = 0; }\n"
        "        (*self).y = v; } }\n"
        "func main() -> int { return 0; }\n");
    check(branch.ok, "branches assigning every path check: " + branch.stage);
    Checked half = check(
        "class C { var x: int; var y: int;\n"
        "    func init(v: int) -> void {\n"
        "        if (v > 0) { (*self).x = v; (*self).y = v; } } }\n"
        "func main() -> int { return 0; }\n");
    check(!half.ok && half.stage == "check", "a half-assigned branch fails");
    // A call inside init may assign fields invisibly — or not. The proof
    // cannot see through it, so the init fails closed even when every
    // field is stored beside the call (fail-hard: unprovable is refused).
    Checked opaque = check(
        "func touch() -> void { }\n"
        "class C { var x: int;\n"
        "    func init() -> void { touch(); (*self).x = 1; } }\n"
        "func main() -> int { return 0; }\n");
    check(!opaque.ok && opaque.stage == "check", "a call inside init fails the proof");
}

// A range matches integers between two inclusive bounds: live over an
// integer scrutinee, dead over anything else, and never exhaustive —
// even `0 ... 10` names no finite case list, so the default stays
// required. Only integer literals bound it; anything else is refused
// where it is written.
void testRange() {
    std::printf("ranges match integers between two bounds\n");
    Checked live = check(
        "func f(x: int) -> int { match x { 0 ... 10 => { return 1; } default => { return 0; } } }\n");
    check(live.ok, "an int range checks: " + live.stage);
    Checked noDefault = check(
        "func f(x: int) -> int { match x { 0 ... 10 => { return 1; } } }\n");
    check(!noDefault.ok && noDefault.stage == "check", "a range still needs a default");
    Checked overBool = check(
        "func f(b: bool) -> int { match b { 0 ... 1 => { return 1; } default => { return 0; } } }\n");
    check(overBool.ok, "a range over a bool is dead, not wrong: " + overBool.stage);
    Checked overString = check(
        "func f(s: string) -> int { match s { 0 ... 1 => { return 1; } default => { return 0; } } }\n");
    check(overString.ok, "a range over a string is dead, not wrong: " + overString.stage);
    Checked inverted = check(
        "func f(x: int) -> int { match x { 10 ... 0 => { return 1; } default => { return 0; } } }\n");
    check(inverted.ok, "an inverted range is dead, not wrong: " + inverted.stage);
    Checked bigUnsigned = check(
        "func f(x: u64) -> int { match x { 0 ... 18446744073709551615 => { return 1; } default => { return 0; } } }\n");
    check(bigUnsigned.ok, "a full-width u64 range checks: " + bigUnsigned.stage);
    Checked bigSigned = check(
        "func f(x: i64) -> int { match x { 0 ... 18446744073709551615 => { return 1; } default => { return 0; } } }\n");
    check(!bigSigned.ok && bigSigned.stage == "check",
          "a u64-wide bound over a signed type fails");
}

void testLoopMatchRefused() {    std::printf("unimplemented control flow refuses loudly\n");
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
    check(ma.ok, "match checks now that dispatch lands it");
}

// A for-in sequence is an array, a slice, or a struct/class carrying the
// iteration protocol: iter() answers the iterator value once, next() takes
// a pointer to it and answers the next element or nilPtr (FEATURES 8.2).
// One without the other does not iterate.
void testForInProtocol() {
    std::printf("for-in over iter/next pairs\n");
    const char* pair =
        "struct RIt { var cur: int; var end: int; var out: int; }\n"
        "struct Rng { var lo: int; var hi: int;\n"
        "    func iter() -> RIt { return RIt { cur: (*self).lo, end: (*self).hi, out: 0 }; }\n"
        "    func next(it: *RIt) -> *int {\n"
        "        if ((*it).cur >= (*it).end) { return nilPtr; }\n"
        "        (*it).out = (*it).cur;\n"
        "        (*it).cur = (*it).cur + 1;\n"
        "        return &((*it).out);\n"
        "    }\n"
        "}\n";
    Checked good = check(
        std::string(pair) +
        "func main() -> int { var r: Rng = Rng { lo: 0, hi: 3 }; var s: int = 0; for (var x in r) { s = s + x; } return s; }\n");
    check(good.ok, "an iter/next pair checks: " + good.stage);
    Checked noIter = check(
        "struct Rng { var lo: int; var hi: int;\n"
        "    func next(it: *Rng) -> *int { return nilPtr; }\n"
        "}\n"
        "func main() -> int { var r: Rng = Rng { lo: 0, hi: 1 }; for (var x in r) { } return 0; }\n");
    check(!noIter.ok && noIter.stage == "check", "next without iter fails");
    Checked noNext = check(
        "struct Rng { var lo: int;\n"
        "    func iter() -> Rng { return Rng { lo: (*self).lo }; }\n"
        "}\n"
        "func main() -> int { var r: Rng = Rng { lo: 0 }; for (var x in r) { } return 0; }\n");
    check(!noNext.ok && noNext.stage == "check", "iter without next fails");
    Checked iterArg = check(
        "struct Rng { var lo: int;\n"
        "    func iter(n: int) -> Rng { return Rng { lo: n }; }\n"
        "    func next(it: *Rng) -> *int { return nilPtr; }\n"
        "}\n"
        "func main() -> int { var r: Rng = Rng { lo: 0 }; for (var x in r) { } return 0; }\n");
    check(!iterArg.ok && iterArg.stage == "check", "iter takes no arguments");
    Checked iterInt = check(
        "struct Rng { var lo: int; var hi: int;\n"
        "    func iter() -> int { return (*self).lo; }\n"
        "    func next(it: *int) -> *int { return nilPtr; }\n"
        "}\n"
        "func main() -> int { var r: Rng = Rng { lo: 0, hi: 1 }; for (var x in r) { } return 0; }\n");
    check(!iterInt.ok && iterInt.stage == "check", "iter answers a nominal iterator");
    Checked nextVal = check(
        "struct RIt { var cur: int; var end: int; }\n"
        "struct Rng { var lo: int; var hi: int;\n"
        "    func iter() -> RIt { return RIt { cur: (*self).lo, end: (*self).hi }; }\n"
        "    func next(it: RIt) -> *int { return nilPtr; }\n"
        "}\n"
        "func main() -> int { var r: Rng = Rng { lo: 0, hi: 1 }; for (var x in r) { } return 0; }\n");
    check(!nextVal.ok && nextVal.stage == "check", "next takes a pointer to the iterator");
    Checked nextOther = check(
        "struct RIt { var cur: int; var end: int; }\n"
        "struct OIt { var cur: int; }\n"
        "struct Rng { var lo: int; var hi: int;\n"
        "    func iter() -> RIt { return RIt { cur: (*self).lo, end: (*self).hi }; }\n"
        "    func next(it: *OIt) -> *int { return nilPtr; }\n"
        "}\n"
        "func main() -> int { var r: Rng = Rng { lo: 0, hi: 1 }; for (var x in r) { } return 0; }\n");
    check(!nextOther.ok && nextOther.stage == "check", "next takes iter's own iterator");
    Checked nextVoid = check(
        "struct RIt { var cur: int; var end: int; }\n"
        "struct Rng { var lo: int; var hi: int;\n"
        "    func iter() -> RIt { return RIt { cur: (*self).lo, end: (*self).hi }; }\n"
        "    func next(it: *RIt) -> void { return; }\n"
        "}\n"
        "func main() -> int { var r: Rng = Rng { lo: 0, hi: 1 }; for (var x in r) { } return 0; }\n");
    check(!nextVoid.ok && nextVoid.stage == "check", "next answers an element pointer");
    Checked ptrArray = check(
        "func main() -> int { var a: [int; 3] = [1, 2, 3]; var p: *[int; 3] = &a; var s: int = 0; for (var x in p) { s = s + x; } return s; }\n");
    check(!ptrArray.ok && ptrArray.stage == "check", "a pointer to an array does not iterate");
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
    // The call records its spec for the lowerer: find the init call in
    // main and ask the checker which specialization it instantiated.
    Checked spec = check(
        "func id<T>(x: T) -> T { return x; }\n"
        "func main() -> int { var a: int = id::<int>(42); return a; }\n");
    const cider::Call* specCall = nullptr;
    if (spec.ok) {
        for (const auto& decl : spec.program->decls) {
            const auto* fn = dynamic_cast<const cider::FuncDecl*>(decl.get());
            if (fn == nullptr || fn->name != "main" || fn->body == nullptr) {
                continue;
            }
            for (const auto& stmt : fn->body->stmts) {
                const auto* vd =
                    dynamic_cast<const cider::VarDecl*>(stmt.get());
                if (vd != nullptr && vd->init != nullptr) {
                    specCall =
                        dynamic_cast<const cider::Call*>(vd->init.get());
                }
            }
        }
    }
    check(specCall != nullptr, "the init is a call");
    check(specCall != nullptr &&
              spec.checker->specOf(specCall, std::nullopt).has_value(),
          "a generic call records its spec");
}

// A generic struct stores per-instantiation members; methods check too.
void testGenericStruct() {
    std::printf("generic structs specialize\n");
    Checked good = check(
        "struct Box<T> { var v: T; }\n"
        "func main() -> int { var b: Box<int> = Box<int> { v: 41 }; return b.v + 1; }\n");
    check(good.ok, "checks: " + good.stage);
    Checked method = check(
        "struct Box<T> { var v: T; func get() -> T { return (*self).v; } }\n"
        "func main() -> int { var b: Box<int> = Box<int> { v: 41 }; return b.get(); }\n");
    check(method.ok, "methods specialize: " + method.stage);
    Checked bareLocal = check(
        "struct Box<T> { var v: T; }\n"
        "func main() -> int { var b: Box = Box<int> { v: 1 }; return 0; }\n");
    check(!bareLocal.ok && bareLocal.stage == "check", "a bare generic local fails");
    // A self-pointer field (`next: *Node<T>`) substitutes T with T on the
    // way through: identity substitution is a no-op, not an infinite
    // recursion (which used to segfault the compiler instead of checking).
    Checked selfPtr = check(
        "struct Node<T> { var v: T; var next: *Node<T>; }\n"
        "func main() -> int {\n"
        "    var node: Node<int> = Node<int> { v: 11, next: nilPtr };\n"
        "    return node.v;\n"
        "}\n");
    check(selfPtr.ok, "a generic self-pointer checks: " + selfPtr.stage);
    // An alias can name a nested instantiation before either struct is
    // declared: member recording precedes all instantiation, so
    // declaration order never poisons a spec with empty fields.
    Checked fwdAlias = check(
        "type Bare1 = Wrap<One<int>>;\n"
        "struct Wrap<T> { var v: T; }\n"
        "struct One<A> { var a: A; }\n"
        "func main() -> int {\n"
        "    var w: Bare1 = Wrap<One<int>> { v: One<int> { a: 1 } };\n"
        "    return w.v.a;\n"
        "}\n");
    check(fwdAlias.ok, "a forward alias checks: " + fwdAlias.stage);
}

// A union constructs by naming one live member; hasM asks about liveness;
// a cast wider than the payload is rejected, not read past.
void testUnions() {
    std::printf("unions name one live member\n");
    Checked good = check(
        "union Value { var i: int; var s: string; }\n"
        "func main() -> int { var v: Value = Value { i: 42 }; return v.i; }\n");
    check(good.ok, "checks: " + good.stage);
    Checked two = check(
        "union Value { var i: int; var s: string; }\n"
        "func main() -> int { var v: Value = Value { i: 1, s: \"x\" }; return 0; }\n");
    check(!two.ok && two.stage == "check", "two members fail");
    Checked member = check(
        "union Value { var i: int; }\n"
        "func main() -> int { var v: Value = Value { z: 1 }; return 0; }\n");
    check(!member.ok && member.stage == "check", "an unknown member fails");
    Checked has = check(
        "union Value { var i: int; var s: string; }\n"
        "func main() -> int { var v: Value = Value { i: 1 }; if (v.hasS()) { return 1; } return 0; }\n");
    check(has.ok, "hasM checks: " + has.stage);
    Checked hasBad = check(
        "union Value { var i: int; }\n"
        "func main() -> int { var v: Value = Value { i: 1 }; return v.hasZ(); }\n");
    check(!hasBad.ok && hasBad.stage == "check", "hasM of nothing fails");
    Checked wide = check(
        "union Small { var b: int; }\n"
        "func main() -> int { var v: Small = Small { b: 1 }; var s: string = v as string; return 0; }\n");
    check(!wide.ok && wide.stage == "check", "a cast wider than the payload fails");
}

// A by-value cycle has no finite size: the declaration itself is refused,
// while a pointer to self stays legal (the reference is 8 bytes).
void testSuperCall() {
    std::printf("super.method calls\n");
    Checked good = check(
        "class Base { var v: int; func get() -> int { return (*self).v; } }\n"
        "class Derived : Base {\n"
        "    func get2() -> int { return super.get() + 1; } }\n"
        "func main() -> int { return 0; }\n");
    check(good.ok, "super.m checks: " + good.stage);
    Checked noMethod = check(
        "class Base { var v: int; }\n"
        "class Derived : Base {\n"
        "    func get2() -> int { return super.nope(); } }\n"
        "func main() -> int { return 0; }\n");
    check(!noMethod.ok && noMethod.stage == "check",
          "super.m without a base method fails");
    Checked noBase = check(
        "class Solo { var v: int; func f() -> int { return super.g(); } }\n"
        "func main() -> int { return 0; }\n");
    check(!noBase.ok && noBase.stage == "check", "super in a baseless class fails");
}

void testConditionalVptr() {
    std::printf("conditional vptr\n");
    Checked sizes = check(
        "class Plain { var a: int; var b: int; }\n"
        "class Virt { var a: int; var b: int; virtual func f() -> int { return 1; } }\n"
        "func main() -> int { return ((sizeof(Virt) - sizeof(Plain)) as int) - 8; }\n");
    check(sizes.ok, "a virtual class is one word larger: " + sizes.stage);
    Checked flat = check(
        "class Plain { var a: int; }\n"
        "func main() -> int { var p: Plain = Plain { a: 5 }; return p.a - 5; }\n");
    check(flat.ok, "a concrete-only class constructs: " + flat.stage);
}

void testOverride() {
    std::printf("the override keyword\n");
    Checked good = check(
        "class Base { var v: int; virtual func f() -> int { return 1; } }\n"
        "class Derived : Base { override func f() -> int { return 2; } }\n"
        "func main() -> int { return 0; }\n");
    check(good.ok, "override over a virtual checks: " + good.stage);
    Checked bare = check(
        "class Base { var v: int; virtual func f() -> int { return 1; } }\n"
        "class Derived : Base { func f() -> int { return 2; } }\n"
        "func main() -> int { return 0; }\n");
    check(bare.ok, "an implicit override still checks: " + bare.stage);
    Checked noBase = check(
        "class Derived { var v: int; override func f() -> int { return 2; } }\n"
        "func main() -> int { return 0; }\n");
    check(!noBase.ok && noBase.stage == "check",
          "override without a base virtual fails: " + noBase.stage);
    Checked shadow = check(
        "class Base { var v: int; virtual func f() -> int { return 1; } }\n"
        "class Mid : Base { }\n"
        "class Derived : Mid { override func f() -> int { return 2; } }\n"
        "func main() -> int { return 0; }\n");
    check(shadow.ok, "override through an indirect base checks: " + shadow.stage);
}

void testSelfEmbed() {
    std::printf("self-embedding nominals are refused\n");
    Checked self = check(
        "struct A { var a: A; }\n"
        "func main() -> int { return 0; }\n");
    check(!self.ok && self.stage == "check", "a struct holding itself fails");
    Checked mutual = check(
        "struct A { var x: B; }\n"
        "struct B { var y: A; }\n"
        "func main() -> int { return 0; }\n");
    check(!mutual.ok && mutual.stage == "check", "mutual embedding fails");
    Checked arr = check(
        "struct A { var xs: [A; 4]; }\n"
        "func main() -> int { return 0; }\n");
    check(!arr.ok && arr.stage == "check", "a self array fails");
    Checked unionSelf = check(
        "union U { var a: U; }\n"
        "func main() -> int { return 0; }\n");
    check(!unionSelf.ok && unionSelf.stage == "check", "a union holding itself fails");
    Checked classSelf = check(
        "class C { var c: C; }\n"
        "func main() -> int { return 0; }\n");
    check(!classSelf.ok && classSelf.stage == "check", "a class holding itself fails");
    Checked ptr = check(
        "struct A { var p: *A; var n: int; }\n"
        "func main() -> int { return 0; }\n");
    check(ptr.ok, "a pointer to self checks: " + ptr.stage);
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
        "    var a: [int; 3] = [ 1, 2, 3 ];\n"
        "    var s: []int = a[0:2];\n"
        "    return s.len as int;\n"
        "}\n");
    check(slice.ok, "a slice has a length");
    Checked array = check(
        "func main() -> int {\n"
        "    var a: [int; 3] = [ 1, 2, 3 ];\n"
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

    Checked uefi = checkAs(
        "func efi_main() -> int { asm { \"nop\" } return 0; }\n",
        cider::target::Kind::Uefi);
    check(!uefi.ok && uefi.stage == "check", "uefi rejects asm at check time");
    bool named = false;
    if (uefi.checker != nullptr) {
        for (const cider::check::CheckError& error : uefi.checker->errors()) {
            if (error.message.find("uefi") != std::string::npos) {
                named = true;
            }
        }
    }
    check(named, "the asm refusal names uefi");
}

void testTargetGlobals() {
    std::printf("target globals\n");
    Checked uefi = checkAs(
        "func efi_main() -> int {\n"
        "    var image: *void = image_handle;\n"
        "    var system: *void = system_table;\n"
        "    return 0;\n"
        "}\n",
        cider::target::Kind::Uefi);
    check(uefi.ok, "uefi target globals check: " + uefi.stage);

    Checked native = check(
        "func main() -> int { var system: *void = system_table; return 0; }\n");
    check(!native.ok && native.stage == "check",
          "target globals are rejected on native");
    bool named = false;
    if (native.checker != nullptr) {
        for (const cider::check::CheckError& error : native.checker->errors()) {
            if (error.message.find("uefi") != std::string::npos) {
                named = true;
            }
        }
    }
    check(named, "the target-global rejection names uefi");
}

// `delete` takes a class pointer: the checker constrains the operand
// (FEAT-032), and the path lowers and runs.
void testDelete() {
    std::printf("delete needs a class pointer\n");
    Checked good = check(
        "class C { var x: int; func init(x: int) -> void { (*self).x = x; } }\n"
        "func main() -> int { var p: *C = new C(1); delete p; return 0; }\n");
    check(good.ok, "deleting a class pointer checks: " + good.stage);
    Checked scalar = check(
        "func main() -> int { var p: *int = alloc(8, 8) as *int; delete p; return 0; }\n");
    check(!scalar.ok && scalar.stage == "check", "deleting a non-class pointer fails");
    Checked nonptr = check("func main() -> int { var x: int = 3; delete x; return 0; }\n");
    check(!nonptr.ok && nonptr.stage == "check", "deleting a non-pointer fails");
    Checked nil = check("func main() -> int { delete nilPtr; return 0; }\n");
    check(!nil.ok && nil.stage == "check", "deleting nil fails");
}

// A redeclaration of a base method: an override when the base slot is
// virtual and the signature agrees; an error otherwise (SEMANTICS 10).
void testOverrides() {
    std::printf("override rules check\n");
    Checked good = check(
        "class Base {\n"
        "    var id: int;\n"
        "    virtual func noise() -> int { return 11; }\n"
        "}\n"
        "class Derived : Base {\n"
        "    func noise() -> int { return 22; }\n"
        "}\n"
        "func main() -> int {\n"
        "    var d: Derived = Derived { id: 1 };\n"
        "    var bp: *Base = &d;\n"
        "    return (*bp).noise();\n"
        "}\n");
    check(good.ok, "an override of a virtual slot checks: " + good.stage);
    Checked notVirtual = check(
        "class Base { var id: int; func noise() -> int { return 11; } }\n"
        "class Derived : Base { func noise() -> int { return 22; } }\n"
        "func main() -> int { return 0; }\n");
    check(!notVirtual.ok && notVirtual.stage == "check",
          "redeclaring a non-virtual base method fails");
    Checked badSig = check(
        "class Base { var id: int; virtual func noise() -> int { return 11; } }\n"
        "class Derived : Base { func noise(x: int) -> int { return 22; } }\n"
        "func main() -> int { return 0; }\n");
    check(!badSig.ok && badSig.stage == "check",
          "an incompatible override signature fails");
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

void testCharCompare() {
    std::printf("chars compare for equality\n");
    Checked good = check(
        "func main() -> int {\n"
        "    var a: char = 'x';\n"
        "    if (a == 'x' && a != 'y') { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(good.ok, "== and != on chars: " + good.stage);
    Checked rel = check(
        "func main() -> int { if ('a' < 'b') { return 1; } return 0; }\n");
    check(rel.ok, "ordering on chars checks: " + rel.stage);
    Checked mixed = check(
        "func main() -> int { var c: char = 'a'; if (c == 97) { return 1; } return 0; }\n");
    check(!mixed.ok && mixed.stage == "check", "char == int fails without as");
    Checked shift = check(
        "func main() -> int { var c: char = 'a'; return c << 1; }\n");
    check(!shift.ok && shift.stage == "check", "a char does not shift");
    Checked byteShift = check(
        "func main() -> int { var b: byte = 6; return b << 2; }\n");
    check(byteShift.ok, "a byte shifts: " + byteShift.stage);
}

void testFnPtrCompare() {
    std::printf("function pointers compare for equality\n");
    Checked good = check(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int {\n"
        "    var f: *func(int, int) -> int = &add;\n"
        "    if (f == nilPtr) { return 1; }\n"
        "    if (f != nilPtr) { return 2; }\n"
        "    return 0;\n"
        "}\n");
    check(good.ok, "== and != against nilPtr: " + good.stage);
    Checked same = check(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int {\n"
        "    var f: *func(int, int) -> int = &add;\n"
        "    var g: *func(int, int) -> int = &add;\n"
        "    if (f != g) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(same.ok, "same-signature function pointers compare: " + same.stage);
    Checked mixed = check(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func neg(a: int) -> int { return 0 - a; }\n"
        "func main() -> int {\n"
        "    var f: *func(int, int) -> int = &add;\n"
        "    var g: *func(int) -> int = &neg;\n"
        "    if (f == g) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(!mixed.ok && mixed.stage == "check", "different signatures fail");
    Checked narrow = check(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int {\n"
        "    var f: *func(int, int) -> int = &add;\n"
        "    if (f == 0) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(!narrow.ok && narrow.stage == "check", "function pointer == int fails");
}

void testStringCompare() {
    std::printf("strings compare for equality\n");
    Checked good = check(
        "func main() -> int {\n"
        "    var a: string = \"ab\";\n"
        "    var b: string = \"ab\";\n"
        "    if (a == b && a != \"ac\") { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(good.ok, "== and != on strings: " + good.stage);
    Checked litChar = check(
        "func main() -> int {\n"
        "    var p: *char = \"hi\";\n"
        "    if (*p == 'h') { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(litChar.ok, "a string literal converts to *char: " + litChar.stage);
    Checked varChar = check(
        "func main() -> int {\n"
        "    var s: string = \"hi\";\n"
        "    var p: *char = s;\n"
        "    return 0;\n"
        "}\n");
    check(!varChar.ok && varChar.stage == "check",
          "a string variable still needs &s[0] for *char");
    Checked litVoid = check(
        "func main() -> int {\n"
        "    var v: *void = \"hi\";\n"
        "    return 0;\n"
        "}\n");
    check(litVoid.ok, "a string literal converts to *void: " + litVoid.stage);
    Checked rel = check(
        "func main() -> int { var s: string = \"a\" < \"b\"; return 0; }\n");
    check(!rel.ok && rel.stage == "check", "ordering on strings fails");
    Checked mixed = check(
        "func main() -> int { var s: string = \"a\"; if (s == 1) { return 1; } return 0; }\n");
    check(!mixed.ok && mixed.stage == "check", "string == int fails");
    Checked nil = check(
        "func main() -> int {\n"
        "    var a: string = \"ab\";\n"
        "    var n: string = nilPtr as string;\n"
        "    if (a == nilPtr) { return 1; }\n"
        "    if (n != nilPtr) { return 2; }\n"
        "    if (!(n == n)) { return 3; }\n"
        "    return 0;\n"
        "}\n");
    check(nil.ok, "a string checks against nilPtr: " + nil.stage);
    Checked nilVar = check(
        "func main() -> int { var p: *void = nilPtr; var s: string = \"a\"; if (s == p) { return 1; } return 0; }\n");
    check(!nilVar.ok && nilVar.stage == "check", "string == *void variable fails");
    Checked addr = check(
        "func main() -> int { var p: *u8 = \"hi\"; return 0; }\n");
    check(addr.ok, "a literal checks beside *u8: " + addr.stage);
    Checked addrVar = check(
        "func main() -> int { var s: string = \"hi\"; var p: *u8 = s; return 0; }\n");
    check(!addrVar.ok && addrVar.stage == "check", "a string variable needs &s[0]");
}

// 2.4 / 6.2: a string's element is a char, so &s[0] is the byte pointer a
// C callee takes; len is an int on strings, slices and arrays.
void testStringBytes() {
    std::printf("string byte pointers and int len\n");
    Checked addr = check(
        "func take(p: *char) -> int { return *p as int; }\n"
        "func main() -> int {\n"
        "    var s: string = \"hi\";\n"
        "    var p: *char = &s[0];\n"
        "    return take(p);\n"
        "}\n");
    check(addr.ok, "&s[0] is a *char: " + addr.stage);
    Checked elem = check(
        "func main() -> int {\n"
        "    var s: string = \"hi\";\n"
        "    var c: char = s[0];\n"
        "    return c as int;\n"
        "}\n");
    check(elem.ok, "a string's element is a char: " + elem.stage);
    Checked len = check(
        "func main() -> int {\n"
        "    var s: string = \"hi\";\n"
        "    var a: [u8; 3] = [1, 2, 3];\n"
        "    var sl: []u8 = a[0:2];\n"
        "    var length: int = s.len;\n"
        "    var bytes: int = a.len;\n"
        "    var part: int = sl.len;\n"
        "    return length + bytes + part;\n"
        "}\n");
    check(len.ok, "len is an int on string, array and slice: " + len.stage);
    Checked wide = check(
        "func main() -> int { var s: string = \"hi\"; var n: u64 = s.len; return 0; }\n");
    check(!wide.ok && wide.stage == "check", "len is not a u64");
}

// 11: sizeof and alignof are compile-time integers; a constant of any
// integer type converts to an integer type whose range contains it.
void testAlignof() {
    std::printf("alignof and constant conversion\n");
    Checked al = check(
        "struct Node { var x: int; var y: i64; }\n"
        "func main() -> int {\n"
        "    var bytes: u64 = sizeof(Node);\n"
        "    var align: u64 = alignof(Node);\n"
        "    var askew: int = alignof(Node) + sizeof(int);\n"
        "    return 0;\n"
        "}\n");
    check(al.ok, "sizeof and alignof bind to u64 and int: " + al.stage);
    Checked exact = check(
        "struct Node { var x: int; var y: i64; }\n"
        "func main() -> int {\n"
        "    var a: [u8; alignof(Node)] = uninit;\n"
        "    return a.len;\n"
        "}\n");
    check(exact.ok, "an alignment answers an array bound: " + exact.stage);
    Checked narrowed = check(
        "func main() -> int { var b: u8 = 300; return 0; }\n");
    check(!narrowed.ok && narrowed.stage == "check",
          "a constant past the target's range still fails");
    Checked live = check(
        "func main() -> int { var n: int = 3; var b: u8 = n; return 0; }\n");
    check(!live.ok && live.stage == "check",
          "a variable still does not convert");
}

// 8: the Standard's annotated loop variable is the sequence's element.
void testTypedForIn() {
    std::printf("typed for..in\n");
    Checked ok = check(
        "func main() -> int {\n"
        "    var a: [u8; 3] = [1, 2, 3];\n"
        "    var total: u8 = 0;\n"
        "    for (var b: u8 in a) { total = total + b; }\n"
        "    return total as int;\n"
        "}\n");
    check(ok.ok, "a typed loop variable is the element: " + ok.stage);
    Checked bad = check(
        "func main() -> int {\n"
        "    var a: [u8; 3] = [1, 2, 3];\n"
        "    for (var b: i32 in a) { b = b; }\n"
        "    return 0;\n"
        "}\n");
    check(!bad.ok && bad.stage == "check",
          "a loop variable of another type fails");
    Checked cstyle = check(
        "func main() -> int {\n"
        "    var t: int = 0;\n"
        "    for (var i: int = 0; i < 3; i = i + 1) { t = t + i; }\n"
        "    return t;\n"
        "}\n");
    check(cstyle.ok, "the C-style head still parses: " + cstyle.stage);
}

// 7.10: a fini block checks like a function body; it has no answer, and a
// return inside would leave it half-run.
// 4.8.4: fields are public by default; `private` restricts every read and
// store to the module that declares the class or struct.
void testPrivateFields() {
    std::printf("private and public fields\n");
    Checked same = check(
        "struct Box { private secret: int; var shown: int; }\n"
        "func main() -> int {\n"
        "    var b: Box = Box { secret: 1, shown: 2 };\n"
        "    b.secret = 3;\n"
        "    return b.secret + b.shown;\n"
        "}\n");
    check(same.ok, "a private field is visible in its module: " + same.stage);

    cider::resolve::Resolver::ModuleLoader load = [](const std::string& name)
        -> std::unique_ptr<cider::Program> {
        if (name != "lib") {
            return nullptr;
        }
        cider::Lexer lexer(
            "lib.cid",
            "export struct Box { private secret: int; var shown: int; }\n"
            "export func make(v: int) -> Box {\n"
            "    return Box { secret: v, shown: v };\n"
            "}\n");
        cider::Parser parser("lib.cid", lexer.lex());
        return parser.parse();
    };
    Checked hidden = check(
        "import lib;\n"
        "func main() -> int {\n"
        "    var b: lib.Box = lib.make(1);\n"
        "    return b.secret;\n"
        "}\n",
        load);
    check(!hidden.ok && hidden.stage == "check",
          "a cross-module private read refuses");
    Checked store = check(
        "import lib;\n"
        "func main() -> int {\n"
        "    var b: lib.Box = lib.make(1);\n"
        "    b.secret = 4;\n"
        "    return 0;\n"
        "}\n",
        load);
    check(!store.ok && store.stage == "check",
          "a cross-module private store refuses");
    Checked open = check(
        "import lib;\n"
        "func main() -> int {\n"
        "    var b: lib.Box = lib.make(1);\n"
        "    return b.shown;\n"
        "}\n",
        load);
    check(open.ok, "a cross-module public read checks: " + open.stage);
    Checked literal = check(
        "import lib;\n"
        "func main() -> int {\n"
        "    var b: lib.Box = lib.Box { secret: 1, shown: 2 };\n"
        "    return 0;\n"
        "}\n",
        load);
    check(!literal.ok && literal.stage == "check",
          "a cross-module literal naming a private field refuses");
}

void testPrivateMethods() {
    std::printf("private and public methods\n");
    cider::resolve::Resolver::ModuleLoader load = [](const std::string& name)
        -> std::unique_ptr<cider::Program> {
        if (name != "lib") {
            return nullptr;
        }
        cider::Lexer lexer(
            "lib.cid",
            "export class C {\n"
            "    private func f() -> int { return 7; }\n"
            "    func g() -> int { return this->f(); }\n"
            "}\n"
            "export func make() -> *C { return new C { }; }\n");
        cider::Parser parser("lib.cid", lexer.lex());
        return parser.parse();
    };
    Checked hidden = check(
        "import lib;\n"
        "func main() -> int {\n"
        "    var c: *lib.C = lib.make();\n"
        "    return c->f();\n"
        "}\n",
        load);
    check(!hidden.ok && hidden.stage == "check",
          "a cross-module private method call refuses");
    Checked open = check(
        "import lib;\n"
        "func main() -> int {\n"
        "    var c: *lib.C = lib.make();\n"
        "    return c->g();\n"
        "}\n",
        load);
    check(open.ok, "a cross-module public method call checks: " + open.stage);
    Checked ordered = check(
        "class B { virtual func f() -> int { return 1; } }\n"
        "class C : B { public func g() -> int { return 2; } }\n"
        "class D : B { private override func f() -> int { return 3; } }\n"
        "class E : B { override private func f() -> int { return 4; } }\n"
        "func main() -> int {\n"
        "    var c: C = C { }; var d: D = D { }; var e: E = E { };\n"
        "    return c.g();\n"
        "}\n");
    check(ordered.ok, "method modifiers may come in any order: " + ordered.stage);
}

void testSuperInit() {
    std::printf("super_init base constructor calls\n");
    Checked chain = check(
        "class Shape {\n"
        "    var sides: int;\n"
        "    func init(s: int) { this->sides = s; }\n"
        "}\n"
        "class Square : Shape {\n"
        "    var w: int;\n"
        "    func init(s: int, w: int) { super_init(s); this->w = w; }\n"
        "}\n"
        "func main() -> int {\n"
        "    var q: *Square = new Square(4, 5);\n"
        "    return q->sides * 100 + q->w;\n"
        "}\n");
    check(chain.ok, "a super_init chain checks: " + chain.stage);
    Checked badArg = check(
        "class Shape {\n"
        "    var sides: int;\n"
        "    func init(s: int) { this->sides = s; }\n"
        "}\n"
        "class Square : Shape {\n"
        "    var w: int;\n"
        "    func init() { super_init(\"four\"); this->w = 1; }\n"
        "}\n"
        "func main() -> int { return 0; }\n");
    check(!badArg.ok && badArg.stage == "check",
          "a super_init argument mismatch refuses");
    Checked outside = check(
        "class Shape { var sides: int; func init(s: int) { this->sides = s; } }\n"
        "class Square : Shape {\n"
        "    var w: int;\n"
        "    func poke() { super_init(1); }\n"
        "    func init() { this->w = 1; }\n"
        "}\n"
        "func main() -> int { return 0; }\n");
    check(!outside.ok && outside.stage == "check",
          "super_init outside init refuses");
    Checked noBase = check(
        "class Lone {\n"
        "    var a: int;\n"
        "    func init() { super_init(); this->a = 1; }\n"
        "}\n"
        "func main() -> int { return 0; }\n");
    check(!noBase.ok && noBase.stage == "check",
          "super_init without a base refuses");
    Checked noInit = check(
        "class Plain { var a: int; }\n"
        "class Kid : Plain {\n"
        "    var b: int;\n"
        "    func init() { super_init(); this->b = 1; }\n"
        "}\n"
        "func main() -> int { return 0; }\n");
    check(!noInit.ok && noInit.stage == "check",
          "super_init without a base init refuses");
}

// 4.8: several bases lay out one after another. A name two bases provide
// refuses by name instead of picking one, `super` names a single base, and
// only the first base may carry the table.
void testMultipleBases() {
    std::printf("several bases refuse ambiguous names\n");
    Checked ambiguousMethod = check(
        "class L { var v: int; func pick() -> int { return this->v; } }\n"
        "class R { var w: int; func pick() -> int { return this->w; } }\n"
        "class D : L, R { var d: int; }\n"
        "func main() -> int {\n"
        "    var x: D = D { v: 1, w: 2, d: 3 };\n"
        "    return x.pick();\n"
        "}\n");
    check(!ambiguousMethod.ok && ambiguousMethod.stage == "check",
          "a method two bases provide refuses");
    Checked ambiguousField = check(
        "class L { var v: int; }\n"
        "class R { var v: int; }\n"
        "class D : L, R { var d: int; func init() { this->d = 1; } }\n"
        "func main() -> int { var x: *D = new D(); return x->v; }\n");
    check(!ambiguousField.ok && ambiguousField.stage == "check",
          "a field two bases provide refuses");
    Checked superTwo = check(
        "class L { var v: int; func f() -> int { return this->v; } }\n"
        "class R { var w: int; func f() -> int { return this->w; } }\n"
        "class D : L, R {\n"
        "    var d: int;\n"
        "    func init() { this->d = 1; }\n"
        "    func g() -> int { return super.f(); }\n"
        "}\n"
        "func main() -> int { var x: *D = new D(); return x->g(); }\n");
    check(!superTwo.ok && superTwo.stage == "check",
          "super with several bases refuses");
    Checked twice = check(
        "class A { var a: int; }\n"
        "class D : A, A { var d: int; }\n"
        "func main() -> int { return 0; }\n");
    check(!twice.ok && twice.stage == "check",
          "a base named twice refuses");
    Checked lateTable = check(
        "class P { var p: int; }\n"
        "class H { var h: int; virtual func f() -> int { return this->h; } }\n"
        "class X : P, H { var x: int; }\n"
        "func main() -> int { return 0; }\n");
    check(!lateTable.ok && lateTable.stage == "check",
          "a later headed base refuses");
}

// CIDER-BUG-049: a field may spell a default. Defaults run for every
// construction before the constructor or literal stores, and a defaulted
// field is exempt from the constructor's assignment proof.
void testFieldDefaults() {
    std::printf("field default initializers\n");
    Checked omitted = check(
        "class C { var x: int = 3; var y: int; }\n"
        "func main() -> int { var c: C = C { y: 5 }; return c.x + c.y; }\n");
    check(omitted.ok, "a defaulted field may be omitted: " + omitted.stage);
    Checked missing = check(
        "class C { var x: int = 3; var y: int; }\n"
        "func main() -> int { var c: C = C { x: 1 }; return c.y; }\n");
    check(!missing.ok && missing.stage == "check",
          "a field without a default is still required");
    Checked twice = check(
        "class C { var x: int = 3; var y: int; }\n"
        "func main() -> int {\n"
        "    var c: C = C { x: 1, x: 2, y: 3 };\n"
        "    return c.x;\n"
        "}\n");
    check(!twice.ok && twice.stage == "check",
          "a field named twice refuses");
    Checked exempt = check(
        "class C { var x: int = 3; var y: int; func init() { this->y = 5; } }\n"
        "func main() -> int { var c: *C = new C(); return c->x; }\n");
    check(exempt.ok,
          "a default exempts the field from init coverage: " + exempt.stage);
    Checked mistyped = check(
        "class C { var x: int = \"three\"; }\n"
        "func main() -> int { return 0; }\n");
    check(!mistyped.ok && mistyped.stage == "check",
          "a mistyped default refuses");
    Checked global = check(
        "const K: int = 7;\n"
        "class C { var x: int = K; }\n"
        "func main() -> int { var c: C = C { }; return c.x; }\n");
    check(global.ok, "a default may name a constant: " + global.stage);
}

// 7.6: `_` is the default arm. 7.2: increment is a statement, never a
// value, so the only legal positions are a whole expression statement and
// a C-style for step.
void testWildcardAndIncrement() {
    std::printf("match wildcard and standalone increment\n");
    Checked wild = check(
        "func main() -> int {\n"
        "  var x: int = 5;\n"
        "  match (x) {\n"
        "    0 | 1 => { return 1; }\n"
        "    2 ... 9 => { return 2; }\n"
        "    _ => { return 3; }\n"
        "  }\n"
        "  return 0;\n"
        "}\n");
    check(wild.ok, "the _ arm is the default: " + wild.stage);
    Checked standalone = check(
        "func main() -> int { var i: int = 0; i++; i--; return i; }\n");
    check(standalone.ok, "a standalone increment checks: " + standalone.stage);
    Checked step = check(
        "func main() -> int {\n"
        "  var t: int = 0;\n"
        "  for (var i: int = 0; i < 3; i++) { t = t + i; }\n"
        "  return t;\n"
        "}\n");
    check(step.ok, "a for-step increment checks: " + step.stage);
    Checked embedded = check(
        "func main() -> int { var x: int = 3; var y: int = x++; return y; }\n");
    check(!embedded.ok && embedded.stage == "check",
          "an increment inside a value refuses");
    Checked nested = check(
        "func f(x: int) -> int { return x; }\n"
        "func main() -> int { var x: int = 3; return f(x++); }\n");
    check(!nested.ok && nested.stage == "check",
          "an increment inside an argument refuses");
}

void testFiniRemoved() {
    std::printf("fini is not core\n");
    Checked r = check(
        "fini { }\n"
        "func main() -> int { return 0; }\n");
    check(!r.ok && r.stage == "parse", "fini is not a declaration");
}

void testInterfaceNil() {
    std::printf("null interfaces check\n");
    Checked cast = check(
        "interface Shape { func area() -> int; }\n"
        "func main() -> int {\n"
        "    var n: Shape = nilPtr as Shape;\n"
        "    return 0;\n"
        "}\n");
    check(cast.ok, "nilPtr casts to an interface: " + cast.stage);
    Checked cmp = check(
        "interface Shape { func area() -> int; }\n"
        "func main() -> int {\n"
        "    var n: Shape = nilPtr as Shape;\n"
        "    if (n == nilPtr) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(cmp.ok, "an interface checks against nilPtr: " + cmp.stage);
    Checked rel = check(
        "interface Shape { func area() -> int; }\n"
        "func main() -> int { var n: Shape = nilPtr as Shape; return n < n; }\n");
    check(!rel.ok && rel.stage == "check", "ordering on interfaces fails");
    Checked mixed = check(
        "interface Shape { func area() -> int; }\n"
        "func main() -> int { var n: Shape = nilPtr as Shape; if (n == 1) { return 1; } return 0; }\n");
    check(!mixed.ok && mixed.stage == "check", "interface == int fails");
    Checked voidVar = check(
        "interface Shape { func area() -> int; }\n"
        "func main() -> int {\n"
        "    var v: *void = nilPtr;\n"
        "    var n: Shape = v as Shape;\n"
        "    return 0;\n"
        "}\n");
    check(!voidVar.ok && voidVar.stage == "check",
          "a *void variable does not cast to an interface");
    Checked globalNil = check(
        "interface Shape { func area() -> int; }\n"
        "var g: Shape = nilPtr as Shape;\n"
        "func main() -> int { return 0; }\n");
    check(globalNil.ok, "a nil interface global is constant: " + globalNil.stage);
    Checked globalLive = check(
        "interface Shape { func area() -> int; }\n"
        "struct Sq { var s: int; func area() -> int { return (*self).s; } }\n"
        "var g: Shape = Sq { s: 1 };\n"
        "func main() -> int { return 0; }\n");
    check(!globalLive.ok && globalLive.stage == "check",
          "a live interface global is not constant");
    Checked nullArray = check(
        "interface Shape { func area() -> int; }\n"
        "var g: [Shape; 2] = [nilPtr as Shape, nilPtr as Shape];\n"
        "func main() -> int { return 0; }\n");
    check(nullArray.ok, "a null-interface array global checks: " + nullArray.stage);
}

// One arm may list several alternatives: each is checked on its own, and
// no alternative may bind a fresh name, because the body cannot know
// which alternative's value it would hold.
void testAlternation() {
    std::printf("alternation patterns check\n");
    Checked good = check(
        "func main() -> int {\n"
        "    var c: int = 2;\n"
        "    match (c) { 1 | 2 => { return 10; } default => { return 30; } }\n"
        "}\n");
    check(good.ok, "constants alternate: " + good.stage);
    Checked exhaustive = check(
        "func main() -> int {\n"
        "    var b: bool = true;\n"
        "    match (b) { true | false => { return 1; } }\n"
        "}\n");
    check(exhaustive.ok, "an alternation covers without a default: " + exhaustive.stage);
    Checked fresh = check(
        "func main() -> int {\n"
        "    match (7) { x | 1 => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(!fresh.ok && fresh.stage == "check", "a fresh name in an alternation fails");
    Checked compared = check(
        "func main() -> int {\n"
        "    var v: int = 5;\n"
        "    match (5) { v | 6 => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(compared.ok, "a bound name still compares: " + compared.stage);
    Checked members = check(
        "union U { var a: int; var b: int; }\n"
        "func main() -> int {\n"
        "    var u: U = U { a: 1 };\n"
        "    match (u) { U.a | U.b => { return 1; } }\n"
        "}\n");
    check(members.ok, "members alternate without a default: " + members.stage);
}

// A guard is a bool over the arm's scope: it runs when the pattern fits,
// and a guarded arm never covers for exhaustiveness.
void testGuard() {
    std::printf("match guards check\n");
    Checked good = check(
        "func main() -> int {\n"
        "    var c: int = 2;\n"
        "    var flag: bool = true;\n"
        "    match (c) { 1 if flag => { return 10; } default => { return 30; } }\n"
        "}\n");
    check(good.ok, "a bool guard checks: " + good.stage);
    Checked bound = check(
        "func main() -> int {\n"
        "    match (2) { x if x > 1 => { return 10; } default => { return 30; } }\n"
        "}\n");
    check(bound.ok, "a guard reads the bound name: " + bound.stage);
    Checked notBool = check(
        "func main() -> int {\n"
        "    match (2) { 1 if 7 => { return 10; } default => { return 30; } }\n"
        "}\n");
    check(!notBool.ok && notBool.stage == "check", "a non-bool guard fails");
    Checked guardedBool = check(
        "func main() -> int {\n"
        "    var flag: bool = true;\n"
        "    match (flag) { true if flag => { return 1; } false => { return 0; } }\n"
        "}\n");
    check(!guardedBool.ok && guardedBool.stage == "check",
          "a guarded bool arm does not cover");
    Checked guardedUnion = check(
        "union U { var a: int; var b: int; }\n"
        "func main() -> int {\n"
        "    var u: U = U { a: 1 };\n"
        "    var flag: bool = true;\n"
        "    match (u) { U.a if flag => { return 1; } U.b => { return 2; } }\n"
        "}\n");
    check(!guardedUnion.ok && guardedUnion.stage == "check",
          "a guarded member does not cover");
}

// A match dispatches on the first arm whose pattern fits: constants by
// value, a bound name by its ==, a fresh name always, else the default.
void testMatch() {
    std::printf("matches check\n");
    Checked good = check(
        "func main() -> int {\n"
        "    var c: int = 2;\n"
        "    match (c) { 1 => { return 10; } 2 => { return 20; } default => { return 30; } }\n"
        "}\n");
    check(good.ok, "int constants with a default: " + good.stage);
    Checked exhaustive = check(
        "func main() -> int {\n"
        "    var b: bool = true;\n"
        "    match (b) { true => { return 1; } false => { return 2; } }\n"
        "}\n");
    check(exhaustive.ok, "true+false needs no default: " + exhaustive.stage);
    Checked missing = check(
        "func main() -> int {\n"
        "    var b: bool = true;\n"
        "    match (b) { true => { return 1; } }\n"
        "}\n");
    check(!missing.ok && missing.stage == "check", "a missing arm fails");
    Checked noDefault = check(
        "func main() -> int {\n"
        "    var c: int = 2;\n"
        "    match (c) { 1 => { return 10; } }\n"
        "}\n");
    check(!noDefault.ok && noDefault.stage == "check", "a missing default fails");
    Checked value = check(
        "func main() -> int {\n"
        "    var v: int = 5;\n"
        "    match (5) { v => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(value.ok, "a bound name compares by value: " + value.stage);
    Checked fresh = check(
        "func main() -> int {\n"
        "    match (7) { x => { return x; } default => { return 0; } }\n"
        "}\n");
    check(fresh.ok, "a fresh name binds: " + fresh.stage);
    Checked strings = check(
        "func main() -> int {\n"
        "    var s: string = \"hi\";\n"
        "    match (s) { \"hi\" => { return 3; } default => { return 0; } }\n"
        "}\n");
    check(strings.ok, "a string constant matches: " + strings.stage);
    Checked mismatch = check(
        "func main() -> int {\n"
        "    var s: string = \"hi\";\n"
        "    match (s) { 1 => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(mismatch.ok, "an int pattern on a string is dead, not an error");
    Checked typePat = check(
        "struct P { var x: int; }\n"
        "func main() -> int {\n"
        "    var p: P = P { x: 1 };\n"
        "    match (p) { P { x: x } => { return x; } default => { return 0; } }\n"
        "}\n");
    check(typePat.ok, "a type pattern binds its fields: " + typePat.stage);
    Checked typeConst = check(
        "struct P { var x: int; var y: int; }\n"
        "func main() -> int {\n"
        "    var p: P = P { x: 1, y: 2 };\n"
        "    match (p) { P { x: 1, y: 2 } => { return 10; } default => { return 0; } }\n"
        "}\n");
    check(typeConst.ok, "a struct pattern with constants checks: " + typeConst.stage);
    Checked typeBind = check(
        "struct P { var x: int; var y: int; }\n"
        "func main() -> int {\n"
        "    var p: P = P { x: 1, y: 2 };\n"
        "    match (p) { P { x: a, y: _ } => { return a; } default => { return 0; } }\n"
        "}\n");
    check(typeBind.ok, "a struct pattern binds fields: " + typeBind.stage);
    Checked typePartial = check(
        "struct P { var x: int; var y: int; }\n"
        "func main() -> int {\n"
        "    var p: P = P { x: 1, y: 2 };\n"
        "    match (p) { P { x: 1 } => { return 10; } default => { return 0; } }\n"
        "}\n");
    check(!typePartial.ok && typePartial.stage == "check",
          "a struct pattern must list every field");
    Checked typeNoField = check(
        "struct P { var x: int; }\n"
        "func main() -> int {\n"
        "    var p: P = P { x: 1 };\n"
        "    match (p) { P { z: 1 } => { return 10; } default => { return 0; } }\n"
        "}\n");
    check(!typeNoField.ok && typeNoField.stage == "check",
          "an unknown field in a pattern fails");
    Checked typeNoType = check(
        "struct P { var x: int; }\n"
        "func main() -> int {\n"
        "    var p: P = P { x: 1 };\n"
        "    match (p) { Q { x: 1 } => { return 10; } default => { return 0; } }\n"
        "}\n");
    check(!typeNoType.ok && typeNoType.stage == "check",
          "an unknown type in a pattern fails");
    Checked typeDead = check(
        "struct P { var x: int; }\n"
        "struct Q { var x: int; }\n"
        "func main() -> int {\n"
        "    var p: P = P { x: 1 };\n"
        "    match (p) { Q { x: 1 } => { return 10; } default => { return 0; } }\n"
        "}\n");
    check(typeDead.ok, "a pattern for another struct is dead, not an error");
    Checked memberPat = check(
        "union U { var a: int; var b: bool; }\n"
        "func main() -> int {\n"
        "    var u: U = U { a: 7 };\n"
        "    match (u) { U.a => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(memberPat.ok, "a union member pattern checks: " + memberPat.stage);
    Checked memberNoMember = check(
        "union U { var a: int; }\n"
        "func main() -> int {\n"
        "    var u: U = U { a: 7 };\n"
        "    match (u) { U.z => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(!memberNoMember.ok && memberNoMember.stage == "check",
          "an unknown union member in a pattern fails");
    Checked emptyPat = check(
        "struct E {}\n"
        "func main() -> int {\n"
        "    var e: E = E {};\n"
        "    match (e) { E {} => { return 5; } default => { return 0; } }\n"
        "}\n");
    check(emptyPat.ok, "an empty struct takes an empty pattern: " + emptyPat.stage);
    Checked emptyNonEmpty = check(
        "struct P { var x: int; }\n"
        "func main() -> int {\n"
        "    var p: P = P { x: 1 };\n"
        "    match (p) { P {} => { return 5; } default => { return 0; } }\n"
        "}\n");
    check(!emptyNonEmpty.ok && emptyNonEmpty.stage == "check",
          "an empty pattern on a non-empty struct fails");
    Checked inLoop = check(
        "func main() -> int {\n"
        "    var i: int = 0;\n"
        "    while (i < 3) { match (i) { 1 => { break; } default => { i = i + 1; } } }\n"
        "    return i;\n"
        "}\n");
    check(inLoop.ok, "break in an arm reaches the loop: " + inLoop.stage);
    Checked unionCovered = check(
        "union U { var a: int; var b: bool; }\n"
        "func main() -> int {\n"
        "    var u: U = U { a: 7 };\n"
        "    match (u) { U.a => { return 1; } U.b => { return 2; } }\n"
        "}\n");
    check(unionCovered.ok, "every union member listed needs no default: " + unionCovered.stage);
    Checked unionMissing = check(
        "union U { var a: int; var b: bool; }\n"
        "func main() -> int {\n"
        "    var u: U = U { a: 7 };\n"
        "    match (u) { U.a => { return 1; } }\n"
        "}\n");
    check(!unionMissing.ok && unionMissing.stage == "check",
          "a missing union member needs a default");
    Checked unionOther = check(
        "union U { var a: int; var b: bool; }\n"
        "union V { var a: int; }\n"
        "func main() -> int {\n"
        "    var u: U = U { a: 7 };\n"
        "    match (u) { U.a => { return 1; } V.a => { return 2; } }\n"
        "}\n");
    check(!unionOther.ok && unionOther.stage == "check",
          "another union's member does not cover");
    Checked unionSingle = check(
        "union One { var a: int; }\n"
        "func main() -> int {\n"
        "    var u: One = One { a: 7 };\n"
        "    match (u) { One.a => { return 1; } }\n"
        "}\n");
    check(unionSingle.ok, "a one-member union needs one arm: " + unionSingle.stage);
}

// An enumerator pattern names its case: `Color.RED` matches the RED value
// of the same enum, any other shape beside an enum is dead, and every case
// listed is exhaustive (FEATURES §8.4).
void testEnumMatch() {
    std::printf("enum member patterns check\n");
    Checked good = check(
        "enum Color { RED, GREEN }\n"
        "func main() -> int {\n"
        "    var c: Color = 0 as Color;\n"
        "    match (c) { Color.RED => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(good.ok, "an enum member pattern checks: " + good.stage);
    Checked bareArm = check(
        "enum Color { RED, GREEN }\n"
        "func main() -> int {\n"
        "    var c: Color = 1 as Color;\n"
        "    match (c) { RED => { return 1; } GREEN => { return 2; } default => { return 0; } }\n"
        "}\n");
    check(bareArm.ok, "a bare enumerator names its case: " + bareArm.stage);
    Checked noMember = check(
        "enum Color { RED, GREEN }\n"
        "func main() -> int {\n"
        "    var c: Color = 0 as Color;\n"
        "    match (c) { Color.PURPLE => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(!noMember.ok && noMember.stage == "check",
          "an unknown enumerator in a pattern fails");
    Checked otherEnum = check(
        "enum Color { RED, GREEN }\n"
        "enum Size { BIG, SMALL }\n"
        "func main() -> int {\n"
        "    var c: Color = 0 as Color;\n"
        "    match (c) { Color.RED => { return 1; } Size.BIG => { return 2; } default => { return 0; } }\n"
        "}\n");
    check(otherEnum.ok, "another enum's case is dead, not an error");
    Checked enumOnInt = check(
        "enum Color { RED, GREEN }\n"
        "func main() -> int {\n"
        "    var c: int = 0;\n"
        "    match (c) { Color.RED => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(enumOnInt.ok, "an enum case on an int is dead, not an error");
    Checked intOnEnum = check(
        "enum Color { RED, GREEN }\n"
        "func main() -> int {\n"
        "    var c: Color = 0 as Color;\n"
        "    match (c) { 0 => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(intOnEnum.ok, "a raw number never names an enum case");
    Checked structTag = check(
        "struct P { var x: int; }\n"
        "func main() -> int {\n"
        "    var p: P = P { x: 1 };\n"
        "    match (p) { P.x => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(!structTag.ok && structTag.stage == "check",
          "a struct tag is not a member pattern");
    Checked covered = check(
        "enum Color { RED, GREEN }\n"
        "func main() -> int {\n"
        "    var c: Color = 0 as Color;\n"
        "    match (c) { Color.RED => { return 1; } Color.GREEN => { return 2; } }\n"
        "}\n");
    check(covered.ok, "every case listed needs no default: " + covered.stage);
    Checked missing = check(
        "enum Color { RED, GREEN, BLUE = 5 }\n"
        "func main() -> int {\n"
        "    var c: Color = 0 as Color;\n"
        "    match (c) { Color.RED => { return 1; } Color.GREEN => { return 2; } }\n"
        "}\n");
    check(!missing.ok && missing.stage == "check",
          "a missing case needs a default");
    Checked otherNoCover = check(
        "enum Color { RED, GREEN }\n"
        "enum Size { BIG, SMALL }\n"
        "func main() -> int {\n"
        "    var c: Color = 0 as Color;\n"
        "    match (c) { Color.RED => { return 1; } Size.BIG => { return 2; } Size.SMALL => { return 3; } }\n"
        "}\n");
    check(!otherNoCover.ok && otherNoCover.stage == "check",
          "another enum's cases do not cover");
}

void testPointerArith() {
    std::printf("pointer arithmetic checks\n");
    Checked add = check(
        "func main() -> int {\n"
        "    var b: [u8; 8] = [0, ...];\n"
        "    var p: *u8 = &b[0];\n"
        "    var q: *u8 = p + 2;\n"
        "    var r: *u8 = 2 + p;\n"
        "    var s: *u8 = q - 1;\n"
        "    var n: i64 = q - p;\n"
        "    return (n as int) + (s[0] as int) - (r[0] as int);\n"
        "}\n");
    check(add.ok, "p+n, n+p, p-n, p-q, p[i] check: " + add.stage);
    Checked mix = check(
        "func main() -> int { var p: *u8 = nilPtr; var q: *i32 = p + 1; return 0; }\n");
    check(!mix.ok && mix.stage == "check", "cross-pointer + int fails");
    Checked sub = check(
        "func main() -> int { var a: *u8 = nilPtr; var b: *i32 = nilPtr; var n: i64 = a - b; return 0; }\n");
    check(!sub.ok && sub.stage == "check", "different-pointee - fails");
    Checked voidArith = check(
        "func main() -> int { var p: *void = nilPtr; var q: *void = p + 1; return 0; }\n");
    check(!voidArith.ok && voidArith.stage == "check", "*void arithmetic fails");
    Checked mul = check(
        "func main() -> int { var p: *u8 = nilPtr; var q: *u8 = p; return 0; }\n");
    check(mul.ok, "plain pointer copy checks: " + mul.stage);
    Checked intMinus = check(
        "func main() -> int { var p: *u8 = nilPtr; var q: *u8 = 1 - p; return 0; }\n");
    check(!intMinus.ok && intMinus.stage == "check", "int - ptr fails");
    Checked compound = check(
        "func main() -> int {\n"
        "    var b: [u8; 4] = [0, ...];\n"
        "    var p: *u8 = &b[0];\n"
        "    p += 2;\n"
        "    p -= 1;\n"
        "    return 0;\n"
        "}\n");
    check(compound.ok, "p += n and p -= n check: " + compound.stage);
    Checked addrDeref = check(
        "func main() -> int {\n"
        "    var x: int = 41;\n"
        "    var p: *int = &x;\n"
        "    var pp: **int = &p;\n"
        "    var q: **int = &*pp;\n"
        "    return **q;\n"
        "}\n");
    check(addrDeref.ok, "&*pp nets to **int: " + addrDeref.stage);
    Checked addrDerefOne = check(
        "func main() -> int {\n"
        "    var x: int = 41;\n"
        "    var p: *int = &x;\n"
        "    var q: *int = &*p;\n"
        "    return *q;\n"
        "}\n");
    check(addrDerefOne.ok, "&*p nets to *int: " + addrDerefOne.stage);
    Checked addrDerefMix = check(
        "func main() -> int {\n"
        "    var x: int = 41;\n"
        "    var p: *int = &x;\n"
        "    var q: **int = &*p;\n"
        "    return 0;\n"
        "}\n");
    check(!addrDerefMix.ok && addrDerefMix.stage == "check", "&*p is not **int");
    Checked alias = check(
        "func main() -> int { var p: *char = nil; var q: *char = nilPtr; if (p != q) { return 1; } return 0; }\n");
    check(alias.ok, "nil is nilPtr: " + alias.stage);
}

void testUninit() {
    std::printf("= uninit declarations\n");
    Checked u = check("func f() { var p: *u8 = uninit; }");
    check(u.ok, "a declared uninit pointer checks: " + u.stage);
    Checked r = check("func f() -> *u8 { return uninit; }");
    check(!r.ok && r.stage == "check", "uninit is refused as a value");
}

void testBareLiteral() {
    std::printf("bare struct literals\n");
    Checked v = check("struct S { x: int; } func main() -> int { var s: S = S { x: 3 }; return s.x - 3; }");
    check(v.ok, "a bare literal is a stack value: " + v.stage);
    Checked m = check("func main() -> int { var flag: bool = true; match flag { true => { return 0; } default => { return 1; } } }");
    check(m.ok, "a match scrutinee is not a literal head: " + m.stage);
}

void testAddrCasts() {
    std::printf("universal address casts\n");
    Checked mem = check("func main() -> int { var p: *u16 = 0xB8000 as *u16; return 0; }");
    check(mem.ok, "a physical address casts to *u16: " + mem.stage);
    Checked rt = check("func add(a: int, b: int) -> int { return a + b; }\n"
                       "func main() -> int {\n"
                       "  var addr: u64 = (&add) as u64;\n"
                       "  var fp: *func(int, int) -> int = addr as *func(int, int) -> int;\n"
                       "  return fp(2, 3) - 5;\n}\n");
    check(rt.ok, "a symbol address round-trips to a call: " + rt.stage);
}

// 8.4: conventions are strictly incompatible types; an explicit cast is the
// one bridge the Standard blesses, and it must check.
void testAbiCasts() {
    std::printf("abi compatibility and explicit casts\n");
    Checked assign = check(
        "msabi func f(x: int) -> int { return x; }\n"
        "func main() -> int {\n"
        "  var p: *sysv func(int) -> int = &f;\n"
        "  return 0;\n}\n");
    check(!assign.ok && assign.stage == "check",
          "assigning msabi to a sysv pointer fails");
    Checked cast = check(
        "msabi func f(x: int) -> int { return x; }\n"
        "func main() -> int {\n"
        "  var p: *sysv func(int) -> int = &f as *sysv func(int) -> int;\n"
        "  return p(3);\n}\n");
    check(cast.ok, "an explicit cast crosses conventions: " + cast.stage);
}

void testFloatSuffix() {
    std::printf("float suffix\n");
    Checked f = check("func main() -> int { var x: float = 3.14f; return 0; }");
    check(f.ok, "3.14f as float: " + f.stage);
    Checked w = check("func main() -> int { var x: f64 = 3.14f; return 0; }");
    check(w.ok, "3.14f adapts to f64: " + w.stage);
}

void testBitcast() {
    std::printf("@bitcast(T, x)\n");
    Checked ok = check("func main() -> int { var u: u64 = @bitcast(u64, 1.0); return 0; }");
    check(ok.ok, "@bitcast(u64, 1.0) checks: " + ok.stage);
    Checked bad = check("func main() -> int { var u: u32 = @bitcast(u32, 1.0); return 0; }");
    check(!bad.ok && bad.stage == "check", "a size mismatch fails");
}

void testCharArith() {
    std::printf("char arithmetic\n");
    Checked up = check(
        "func up(c: char) -> char { if (c >= 'a' && c <= 'z') { return c - 'a' + 'A'; } return c; }\n"
        "func main() -> int { return 0; }\n");
    check(up.ok, "range compare and char math check: " + up.stage);
}

void testThisKeyword() {
    std::printf("this and self are synonyms\n");
    Checked t = check("class C { var x: int; func get() -> int { return (*this).x; } } func main() -> int { return 0; }");
    check(t.ok, "(*this).x checks in a method: " + t.stage);
}

void testArrowMember() {
    std::printf("arrow member access checks\n");
    Checked arrow = check("struct S { var x: int; } func f(p: *S) -> int { return p->x; } func main() -> int { return 0; }");
    check(arrow.ok, "p->x checks like (*p).x: " + arrow.stage);
    Checked dot = check("struct S { var x: int; } func f(p: *S) -> int { return p.x; } func main() -> int { return 0; }");
    check(!dot.ok && dot.stage == "check", "p.x on a pointer stays rejected");
}

void testFloatGlobal() {
    // A float expression over constants names every image byte once
    // folded, so a float global may hold it (FEATURES §8.3): no startup
    // code, no order problem. Integer subtrees keep integer semantics
    // first — `5 / 2` is 2, then 2.0 — so the fold matches the local
    // path exactly.
    std::printf("float expressions fold into globals\n");
    Checked arith = check(
        "var g: f64 = 1.0 + 2.0;\n"
        "func main() -> int { return g as int; }\n");
    check(arith.ok, "a float addition global checks: " + arith.stage);
    // An integer operation is not implicitly a float (§13: only literals
    // and binary operands convert): the conversion is written `as f64`,
    // and the fold keeps integer semantics first — `(5 / 2) as f64` is
    // 2, then 2.0, never 2.5.
    Checked intdiv = check(
        "var g: f64 = (5 / 2) as f64;\n"
        "func main() -> int { return g as int; }\n");
    check(intdiv.ok, "an integer division global checks: " + intdiv.stage);
    Checked barediv = check(
        "var g: f64 = 5 / 2;\n"
        "func main() -> int { return g as int; }\n");
    check(!barediv.ok && barediv.stage == "check",
          "a bare integer division is not a float: " + barediv.stage);
    Checked cast = check(
        "var g: f64 = (0.1 as float) as f64;\n"
        "func main() -> int { return 0; }\n");
    check(cast.ok, "an as-float global checks: " + cast.stage);
    Checked barecast = check(
        "var g: f64 = 0.1 as float;\n"
        "func main() -> int { return 0; }\n");
    check(!barecast.ok && barecast.stage == "check",
          "a bare f32 is not implicitly an f64: " + barecast.stage);
    Checked runtime = check(
        "func f() -> f64 { return 1.0; }\n"
        "var g: f64 = f() + 2.0;\n"
        "func main() -> int { return 0; }\n");
    check(!runtime.ok && runtime.stage == "check",
          "a runtime float still needs a constant");
}

void testEnumValues() {
    std::printf("enumerators denote their integer values\n");
    Checked good = check(
        "enum Color { RED, GREEN, BLUE = 5 }\n"
        "func main() -> int {\n"
        "    var c: int = Color.RED;\n"
        "    if (c != 0) { return 1; }\n"
        "    c = Color.GREEN;\n"
        "    if (c != 1) { return 2; }\n"
        "    c = Color.BLUE;\n"
        "    if (c != 5) { return 3; }\n"
        "    return 0;\n"
        "}\n");
    check(good.ok, "qualified enumerators check: " + good.stage);
    Checked unknown = check(
        "enum Color { RED }\n"
        "func main() -> int { var c: int = Color.NOPE; return c; }\n");
    check(!unknown.ok && unknown.stage == "check", "unknown enumerator fails");
    Checked bare = check(
        "enum Color { RED }\n"
        "func main() -> int { var c: int = RED; return c; }\n");
    check(bare.ok, "a bare enumerator resolves: " + bare.stage);
    Checked ambiguous = check(
        "enum A { SAME }\n"
        "enum B { SAME }\n"
        "func main() -> int { var c: int = SAME; return c; }\n");
    check(!ambiguous.ok && ambiguous.stage == "check",
          "an enumerator named by two enums stays unbound");
    Checked enumToInt = check(
        "enum Color { RED }\n"
        "func take(v: int) -> int { return v; }\n"
        "func main() -> int { var c: Color = Color.RED; return take(c); }\n");
    check(enumToInt.ok, "an enum flows to an integer target: " + enumToInt.stage);
    Checked intToEnum = check(
        "enum Color { RED }\n"
        "func main() -> int { var c: Color = 0; return 0; }\n");
    check(!intToEnum.ok && intToEnum.stage == "check",
          "an integer needs `as` for an enum target");
    Checked sameEnum = check(
        "enum Color { RED, GREEN }\n"
        "func main() -> int { var c: Color = Color.RED; if (c == Color.GREEN) { return 1; } return 0; }\n");
    check(sameEnum.ok, "one enum compares against itself: " + sameEnum.stage);
    Checked crossEnum = check(
        "enum A { X }\n"
        "enum B { Y }\n"
        "func main() -> int { var a: A = A.X; var b: B = B.Y; if (a == b) { return 1; } return 0; }\n");
    check(!crossEnum.ok && crossEnum.stage == "check",
          "two enums never compare");
    Checked aliasQual = check(
        "enum Color { RED, GREEN }\n"
        "type Shade = Color;\n"
        "func main() -> int { var c: Color = Shade.GREEN; return c as int; }\n");
    check(aliasQual.ok, "an alias names its enum's variants: " + aliasQual.stage);
}

void testWideLit() {
    std::printf("wide literals check against u16 shapes\n");
    Checked arr = check(
        "func main() -> int {\n"
        "    var s: [u16; 3] = u\"AB\";\n"
        "    return s[0] as int;\n"
        "}\n");
    check(arr.ok, "a wide literal fills its array: " + arr.stage);
    Checked ptr = check(
        "func take(p: *u16) -> int { return p[0] as int; }\n"
        "func main() -> int { return take(u\"Y\"); }\n");
    check(ptr.ok, "a wide literal answers a u16 pointer: " + ptr.stage);
    Checked ch = check(
        "func main() -> int { var c: u16 = u'Z'; return c as int; }\n");
    check(ch.ok, "a wide character is a u16: " + ch.stage);
    Checked badLen = check(
        "func main() -> int {\n"
        "    var s: [u16; 2] = u\"AB\";\n"
        "    return 0;\n"
        "}\n");
    check(!badLen.ok && badLen.stage == "check",
          "a short array still fails");
}

void testClosureVoid() {
    std::printf("closures round-trip through *void with provenance\n");
    Checked good = check(
        "func main() -> int {\n"
        "    var c: func(int) -> int = func(x: int) -> int { return x * 2; };\n"
        "    var v: *void = c;\n"
        "    var f: func(int) -> int = v as func(int) -> int;\n"
        "    return f(21);\n"
        "}\n");
    check(good.ok, "a proven closure casts back: " + good.stage);
    Checked late = check(
        "func main() -> int {\n"
        "    var c: func(int) -> int = func(x: int) -> int { return x * 2; };\n"
        "    var v: *void = nilPtr;\n"
        "    v = c;\n"
        "    var f: func(int) -> int = v as func(int) -> int;\n"
        "    return f(21);\n"
        "}\n");
    check(late.ok, "a late assignment proves: " + late.stage);
    Checked copy = check(
        "func main() -> int {\n"
        "    var c: func(int) -> int = func(x: int) -> int { return x * 2; };\n"
        "    var v: *void = c;\n"
        "    var w: *void = v;\n"
        "    var f: func(int) -> int = w as func(int) -> int;\n"
        "    return f(21);\n"
        "}\n");
    check(copy.ok, "a copy chain keeps provenance: " + copy.stage);
    Checked nil = check(
        "func main() -> int {\n"
        "    var f: func(int) -> int = nilPtr as func(int) -> int;\n"
        "    if (f != nilPtr) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(nil.ok, "nilPtr casts to a null closure: " + nil.stage);
    Checked raw = check(
        "func add(x: int) -> int { return x; }\n"
        "func main() -> int {\n"
        "    var p: *void = &add as *void;\n"
        "    var f: func(int) -> int = p as func(int) -> int;\n"
        "    return f(1);\n"
        "}\n");
    check(!raw.ok && raw.stage == "check", "a raw pointer never launders");
    Checked unproven = check(
        "func invoke(v: *void) -> int {\n"
        "    var f: func(int) -> int = v as func(int) -> int;\n"
        "    return f(21);\n"
        "}\n"
        "func main() -> int { return 0; }\n");
    check(!unproven.ok && unproven.stage == "check",
          "an unproven parameter refuses");
    // Documented conservative limit: provenance tracks the syntactic
    // chain, so reading through a different chain that aliases the same
    // location at runtime stays unproven and refuses.
    Checked crossChain = check(
        "struct Slot { var v: *void; }\n"
        "func main() -> int {\n"
        "    var c: func(int) -> int = func(x: int) -> int { return x; };\n"
        "    var s: Slot = Slot { v: nilPtr };\n"
        "    var ps: *Slot = &s;\n"
        "    s.v = c;\n"
        "    var f: func(int) -> int = (*ps).v as func(int) -> int;\n"
        "    return f(1);\n"
        "}\n");
    check(!crossChain.ok && crossChain.stage == "check",
          "a different aliasing chain stays unproven");
    Checked computedIndex = check(
        "func main() -> int {\n"
        "    var c: func(int) -> int = func(x: int) -> int { return x; };\n"
        "    var a: [*void; 2] = [nilPtr, nilPtr];\n"
        "    var i: int = 0;\n"
        "    a[0] = c;\n"
        "    var f: func(int) -> int = a[i] as func(int) -> int;\n"
        "    return f(1);\n"
        "}\n");
    check(!computedIndex.ok && computedIndex.stage == "check",
          "a computed index stays unproven");
}

void testDefer() {
    std::printf("defer checks at scope exit\n");
    Checked good = check(
        "var out: int = 0;\n"
        "func main() -> int {\n"
        "    defer { out = 1; }\n"
        "    return 0;\n"
        "}\n");
    check(good.ok, "a defer checks: " + good.stage);
    Checked retIn = check(
        "func main() -> int {\n"
        "    defer { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(!retIn.ok && retIn.stage == "check",
          "a return inside a defer fails");
    Checked brkIn = check(
        "func main() -> int {\n"
        "    while (true) { defer { break; } }\n"
        "    return 0;\n"
        "}\n");
    check(!brkIn.ok && brkIn.stage == "check",
          "a break inside a defer fails");
    Checked global = check(
        "defer { }\n"
        "func main() -> int { return 0; }\n");
    check(!global.ok && global.stage == "parse",
          "a global defer fails");
}

void testGlobalConstWidth() {    // A folded integer names image bytes, so one that does not fit the
    // declared width is provably overflowing and rejects — unless wrap
    // defines the value, in which case the low bits are kept (SEMANTICS
    // §2.3, FEATURES §11.5). A fitting fold always passes.
    std::printf("global constants fit their width\n");
    Checked fits = check(
        "var g: u32 = (2000000000 as u32) + (1000000000 as u32);\n"
        "func main() -> int { return 0; }\n");
    check(fits.ok, "a fitting fold checks: " + fits.stage);
    Checked overflows = check(
        "var g: u32 = (3000000000 as u32) + (3000000000 as u32);\n"
        "func main() -> int { return 0; }\n");
    check(!overflows.ok && overflows.stage == "check",
          "an overflowing fold rejects without wrap: " + overflows.stage);
}

void testRepeatZero() {
    // `new [T; N] { [0, ...] }` makes every element zero (FEATURES §14.2),
    // and zero reaches through nested arrays: the filler is the scalar
    // zero, not an element-typed value.
    std::printf("zero repeat fills nested arrays\n");
    Checked nested = check(
        "func main() -> int {\n"
        "    var m: [[u8; 2]; 3] = new [[u8; 2]; 3] { [0, ...] };\n"
        "    return m[2][1] as int;\n"
        "}\n");
    check(nested.ok, "zero repeat fills a nested array: " + nested.stage);
    Checked nonzero = check(
        "func main() -> int {\n"
        "    var m: [[u8; 2]; 3] = new [[u8; 2]; 3] { [1, ...] };\n"
        "    return 0;\n"
        "}\n");
    check(!nonzero.ok && nonzero.stage == "check",
          "a nonzero repeat over an aggregate element fails");
}

void testSliceCopy() {
    // A slice copies into an array of the same element type: the length
    // is checked where the copy happens, so a mismatch traps instead of
    // truncating (SEMANTICS §5, slice-to-array row).
    std::printf("a slice copies into an array\n");
    Checked same = check(
        "func main() -> int {\n"
        "    var a: [u8; 5] = [10, 42, 43, 44, 50];\n"
        "    var s: [u8; 3] = a[1:4];\n"
        "    return s[0] as int;\n"
        "}\n");
    check(same.ok, "a slice copies into a same-element array: " + same.stage);
    Checked wide = check(
        "func main() -> int {\n"
        "    var a: [u8; 5] = [10, 42, 43, 44, 50];\n"
        "    var s: [u16; 3] = a[1:4];\n"
        "    return 0;\n"
        "}\n");
    check(!wide.ok && wide.stage == "check",
          "a slice refuses a different element type");
}

void testShiftCount() {
    std::printf("a constant shift count is bounded by the operand width\n");
    Checked wide = check(
        "func f() -> int {\n"
        "    var a: u8 = 1;\n"
        "    return (a << 33) as int;\n"
        "}\n");
    check(!wide.ok && wide.stage == "check",
          "a constant shift count past the width is refused");
    bool named = false;
    if (wide.checker != nullptr) {
        for (const cider::check::CheckError& e : wide.checker->errors()) {
            if (e.message.find("shift count") != std::string::npos) {
                named = true;
            }
        }
    }
    check(named, "the shift count diagnostic names the count");
    Checked wideRight = check(
        "func f() -> int {\n"
        "    var a: i32 = 1;\n"
        "    return a >> 32;\n"
        "}\n");
    check(!wideRight.ok && wideRight.stage == "check",
          "a right shift count past the width is refused");
    Checked runtime = check(
        "func f(n: u8) -> int {\n"
        "    var a: u8 = 1;\n"
        "    return (a << n) as int;\n"
        "}\n");
    check(runtime.ok, "a runtime shift count is accepted: " + runtime.stage);
    Checked edge = check(
        "func f() -> int {\n"
        "    var a: i32 = 1;\n"
        "    return a << 31;\n"
        "}\n");
    check(edge.ok, "an in-range constant shift count checks: " + edge.stage);
}

}  // namespace

// v3 mangles one code label per method name (<type>__<name>), so a type
// cannot declare two methods with the same name (plain and virtual, or two
// signatures): the second would collide at codegen. The checker refuses it
// by name instead of letting the assembler fail.
void testDuplicateMethods() {
    std::printf("a duplicate method name is refused\n");
    Checked dup = check(
        "class C {\n"
        "    func pick() -> int { return 1; }\n"
        "    virtual func pick() -> int { return 2; }\n"
        "}\n"
        "func main() -> int { return 0; }\n");
    check(!dup.ok && dup.stage == "check", "duplicate method is refused");
    bool named = false;
    if (dup.checker != nullptr) {
        for (const cider::check::CheckError& e : dup.checker->errors()) {
            if (e.message.find("duplicate method") != std::string::npos) {
                named = true;
            }
        }
    }
    check(named, "duplicate method diagnostic names the method");
}

void testSizeofExpr() {
    std::printf("sizeof expression\n");
    Checked expr = check(
        "func touch() -> int { return 7; }\n"
        "func main() -> int { var n: int = sizeof touch() as int; return n; }\n");
    check(expr.ok, "sizeof expression form checks: " + expr.stage);
    Checked type = check(
        "func main() -> int { var n: int = sizeof(int); return n; }\n");
    check(type.ok, "sizeof(type) still checks: " + type.stage);
    Checked callParens = check(
        "func touch() -> int { return 7; }\n"
        "func main() -> int { var n: int = sizeof(touch()); return n; }\n");
    check(!callParens.ok, "sizeof(value) remains a type-form rejection");
}

void testExternVar() {
    std::printf("extern var\n");
    Checked ok = check(
        "extern var ext: int;\n"
        "func main() -> int { return ext; }\n");
    check(ok.ok, "extern var checks: " + ok.stage);
    Checked init = check(
        "extern var ext: int = 1;\n"
        "func main() -> int { return 0; }\n");
    check(!init.ok, "extern var refuses an initializer");
}

void testModuleAsm() {
    std::printf("module asm\n");
    Checked native = check(
        "asm {\n"
        "  \"module_data:\"\n"
        "  \"  .quad 42\"\n"
        "}\n"
        "func main() -> int { return 0; }\n");
    check(native.ok, "native module asm checks: " + native.stage);
    Checked uefi = checkAs(
        "asm { \"nop\" }\n"
        "func efi_main() -> int { return 0; }\n",
        cider::target::Kind::Uefi);
    check(!uefi.ok && uefi.stage == "check",
          "uefi module asm is rejected at check time");
    bool named = false;
    if (uefi.checker != nullptr) {
        for (const cider::check::CheckError& error : uefi.checker->errors()) {
            if (error.message.find("uefi") != std::string::npos) {
                named = true;
            }
        }
    }
    check(named, "the module-asm rejection names uefi");
    Checked vm = checkAs(
        "asm { \"nop\" }\n"
        "func main() -> int { return 0; }\n",
        cider::target::Kind::Vm);
    check(!vm.ok && vm.stage == "check",
          "the vm rejects module-scope asm rather than ignoring it");
    bool vmNamed = false;
    if (vm.checker != nullptr) {
        for (const cider::check::CheckError& error : vm.checker->errors()) {
            if (error.message.find("vm") != std::string::npos) {
                vmNamed = true;
            }
        }
    }
    check(vmNamed, "the module-asm vm rejection names vm");
}

void testLocalAggregates() {
    std::printf("local aggregates\n");
    Checked ok = check(
        "func main() -> int {\n"
        "  struct Local { var x: int; }\n"
        "  var a: Local = Local { x: 1 };\n"
        "  return a.x;\n"
        "}\n");
    check(ok.ok, "local struct checks: " + ok.stage);

    Checked outside = check(
        "func main() -> int {\n"
        "  { struct Local { var x: int; } }\n"
        "  var a: Local = Local { x: 1 };\n"
        "  return 0;\n"
        "}\n");
    check(!outside.ok && outside.stage == "check",
          "a local struct does not escape its block");
}

int main() {
    std::printf("check_test — well-typed programs check\n\n");
    testArithmetic();
    testNames();
    testGlobalZero();
    testGlobalStruct();
    testFloatGlobal();
    testEnumValues();
    testWideLit();
    testDefer();
    testClosureVoid();
    testGlobalConstWidth();
    testCalls();
    testInterface();
    testGenericCall();
    testGenericStruct();
    testUnions();
    testSelfEmbed();
    testOverride();
    testConditionalVptr();
    testSuperCall();
    testFnAddr();
    testIncrDecr();
    testBoolCompare();
    testCharCompare();
    testFnPtrCompare();
    testStringCompare();
    testStringBytes();
    testAlignof();
    testTypedForIn();
    testSizeofExpr();
    testExternVar();
    testModuleAsm();
    testLocalAggregates();
    testPrivateFields();
    testPrivateMethods();
    testSuperInit();
    testMultipleBases();
    testFieldDefaults();
    testWildcardAndIncrement();
    testFiniRemoved();
    testInterfaceNil();
    testMatch();
    testAlternation();
    testGuard();
    testEnumMatch();
    testPointerArith();
    testArrowMember();
    testCharArith();
    testBitcast();
    testAddrCasts();
    testAbiCasts();
    testBareLiteral();
    testFloatSuffix();
    testUninit();
    testThisKeyword();
    testRepeatZero();
    testSliceCopy();
    testAsm();
    testTargetGlobals();
    testDelete();
    testOverrides();
    testLen();
    testFuncLitChecks();
    testControl();
    testExceptionSyntaxRemoved();
    testOperatorOverloads();
    testForInProtocol();
    testInitAssign();
    testLoopMatchRefused();
    testRange();
    testDuplicateMethods();
    testShiftCount();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
