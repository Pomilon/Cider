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
void testGlobalZero() {
    // A zero repeat names every byte (zero), so it is a constant a global
    // may hold: no startup code, no order problem (FEATURES §8.3).
    std::printf("a zero-filled array may live in a global\n");
    Checked zero = check(
        "var g: [u8; 3] = new [u8; 3] {[0, ...]};\n"
        "func main() -> int { return g[0] as int; }\n");
    check(zero.ok, "a zero-repeat global checks: " + zero.stage);
    Checked nested = check(
        "var g: [[u8; 2]; 2] = new [[u8; 2]; 2] {[0, ...]};\n"
        "func main() -> int { return 0; }\n");
    check(nested.ok, "a nested zero-repeat global checks: " + nested.stage);
    Checked explicitInit = check(
        "var g: [u8; 3] = new [u8; 3] {1, 2, 3};\n"
        "func main() -> int { return 0; }\n");
    check(explicitInit.ok, "a literal global array checks: " + explicitInit.stage);
    Checked runtimeInit = check(
        "func f() -> u8 { return 1; }\n"
        "var g: [u8; 2] = new [u8; 2] {f(), 8};\n"
        "func main() -> int { return 0; }\n");
    check(!runtimeInit.ok && runtimeInit.stage == "check",
          "a runtime element still needs a constant");
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
        "    var a: [*Rect; 2] = new [*Rect; 2] {new Rect(3, 4), new Rect(1, 2)};\n"
        "    return get(a[0:2]);\n"
        "}\n");
    check(!sliceArg.ok && sliceArg.stage == "check", "a slice is not a scalar interface");
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
        "    func iter() -> RIt { return new RIt { cur: (*self).lo, end: (*self).hi, out: 0 }; }\n"
        "    func next(it: *RIt) -> *int {\n"
        "        if ((*it).cur >= (*it).end) { return nilPtr; }\n"
        "        (*it).out = (*it).cur;\n"
        "        (*it).cur = (*it).cur + 1;\n"
        "        return &((*it).out);\n"
        "    }\n"
        "}\n";
    Checked good = check(
        std::string(pair) +
        "func main() -> int { var r: Rng = new Rng { lo: 0, hi: 3 }; var s: int = 0; for (var x in r) { s = s + x; } return s; }\n");
    check(good.ok, "an iter/next pair checks: " + good.stage);
    Checked noIter = check(
        "struct Rng { var lo: int; var hi: int;\n"
        "    func next(it: *Rng) -> *int { return nilPtr; }\n"
        "}\n"
        "func main() -> int { var r: Rng = new Rng { lo: 0, hi: 1 }; for (var x in r) { } return 0; }\n");
    check(!noIter.ok && noIter.stage == "check", "next without iter fails");
    Checked noNext = check(
        "struct Rng { var lo: int;\n"
        "    func iter() -> Rng { return new Rng { lo: (*self).lo }; }\n"
        "}\n"
        "func main() -> int { var r: Rng = new Rng { lo: 0 }; for (var x in r) { } return 0; }\n");
    check(!noNext.ok && noNext.stage == "check", "iter without next fails");
    Checked iterArg = check(
        "struct Rng { var lo: int;\n"
        "    func iter(n: int) -> Rng { return new Rng { lo: n }; }\n"
        "    func next(it: *Rng) -> *int { return nilPtr; }\n"
        "}\n"
        "func main() -> int { var r: Rng = new Rng { lo: 0 }; for (var x in r) { } return 0; }\n");
    check(!iterArg.ok && iterArg.stage == "check", "iter takes no arguments");
    Checked iterInt = check(
        "struct Rng { var lo: int; var hi: int;\n"
        "    func iter() -> int { return (*self).lo; }\n"
        "    func next(it: *int) -> *int { return nilPtr; }\n"
        "}\n"
        "func main() -> int { var r: Rng = new Rng { lo: 0, hi: 1 }; for (var x in r) { } return 0; }\n");
    check(!iterInt.ok && iterInt.stage == "check", "iter answers a nominal iterator");
    Checked nextVal = check(
        "struct RIt { var cur: int; var end: int; }\n"
        "struct Rng { var lo: int; var hi: int;\n"
        "    func iter() -> RIt { return new RIt { cur: (*self).lo, end: (*self).hi }; }\n"
        "    func next(it: RIt) -> *int { return nilPtr; }\n"
        "}\n"
        "func main() -> int { var r: Rng = new Rng { lo: 0, hi: 1 }; for (var x in r) { } return 0; }\n");
    check(!nextVal.ok && nextVal.stage == "check", "next takes a pointer to the iterator");
    Checked nextOther = check(
        "struct RIt { var cur: int; var end: int; }\n"
        "struct OIt { var cur: int; }\n"
        "struct Rng { var lo: int; var hi: int;\n"
        "    func iter() -> RIt { return new RIt { cur: (*self).lo, end: (*self).hi }; }\n"
        "    func next(it: *OIt) -> *int { return nilPtr; }\n"
        "}\n"
        "func main() -> int { var r: Rng = new Rng { lo: 0, hi: 1 }; for (var x in r) { } return 0; }\n");
    check(!nextOther.ok && nextOther.stage == "check", "next takes iter's own iterator");
    Checked nextVoid = check(
        "struct RIt { var cur: int; var end: int; }\n"
        "struct Rng { var lo: int; var hi: int;\n"
        "    func iter() -> RIt { return new RIt { cur: (*self).lo, end: (*self).hi }; }\n"
        "    func next(it: *RIt) -> void { return; }\n"
        "}\n"
        "func main() -> int { var r: Rng = new Rng { lo: 0, hi: 1 }; for (var x in r) { } return 0; }\n");
    check(!nextVoid.ok && nextVoid.stage == "check", "next answers an element pointer");
    Checked ptrArray = check(
        "func main() -> int { var a: [int; 3] = new [int; 3] {1, 2, 3}; var p: *[int; 3] = &a; var s: int = 0; for (var x in p) { s = s + x; } return s; }\n");
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

// A by-value cycle has no finite size: the declaration itself is refused,
// while a pointer to self stays legal (the reference is 8 bytes).
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
        "func main() -> int { var c: char = 'a' < 'b'; return 0; }\n");
    check(!rel.ok && rel.stage == "check", "ordering on chars fails");
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
        "    var u: U = new U { a: 1 };\n"
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
        "    var u: U = new U { a: 1 };\n"
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
        "    var p: P = new P { x: 1 };\n"
        "    match (p) { P { x: x } => { return x; } default => { return 0; } }\n"
        "}\n");
    check(typePat.ok, "a type pattern binds its fields: " + typePat.stage);
    Checked typeConst = check(
        "struct P { var x: int; var y: int; }\n"
        "func main() -> int {\n"
        "    var p: P = new P { x: 1, y: 2 };\n"
        "    match (p) { P { x: 1, y: 2 } => { return 10; } default => { return 0; } }\n"
        "}\n");
    check(typeConst.ok, "a struct pattern with constants checks: " + typeConst.stage);
    Checked typeBind = check(
        "struct P { var x: int; var y: int; }\n"
        "func main() -> int {\n"
        "    var p: P = new P { x: 1, y: 2 };\n"
        "    match (p) { P { x: a, y: _ } => { return a; } default => { return 0; } }\n"
        "}\n");
    check(typeBind.ok, "a struct pattern binds fields: " + typeBind.stage);
    Checked typePartial = check(
        "struct P { var x: int; var y: int; }\n"
        "func main() -> int {\n"
        "    var p: P = new P { x: 1, y: 2 };\n"
        "    match (p) { P { x: 1 } => { return 10; } default => { return 0; } }\n"
        "}\n");
    check(!typePartial.ok && typePartial.stage == "check",
          "a struct pattern must list every field");
    Checked typeNoField = check(
        "struct P { var x: int; }\n"
        "func main() -> int {\n"
        "    var p: P = new P { x: 1 };\n"
        "    match (p) { P { z: 1 } => { return 10; } default => { return 0; } }\n"
        "}\n");
    check(!typeNoField.ok && typeNoField.stage == "check",
          "an unknown field in a pattern fails");
    Checked typeNoType = check(
        "struct P { var x: int; }\n"
        "func main() -> int {\n"
        "    var p: P = new P { x: 1 };\n"
        "    match (p) { Q { x: 1 } => { return 10; } default => { return 0; } }\n"
        "}\n");
    check(!typeNoType.ok && typeNoType.stage == "check",
          "an unknown type in a pattern fails");
    Checked typeDead = check(
        "struct P { var x: int; }\n"
        "struct Q { var x: int; }\n"
        "func main() -> int {\n"
        "    var p: P = new P { x: 1 };\n"
        "    match (p) { Q { x: 1 } => { return 10; } default => { return 0; } }\n"
        "}\n");
    check(typeDead.ok, "a pattern for another struct is dead, not an error");
    Checked memberPat = check(
        "union U { var a: int; var b: bool; }\n"
        "func main() -> int {\n"
        "    var u: U = new U { a: 7 };\n"
        "    match (u) { U.a => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(memberPat.ok, "a union member pattern checks: " + memberPat.stage);
    Checked memberNoMember = check(
        "union U { var a: int; }\n"
        "func main() -> int {\n"
        "    var u: U = new U { a: 7 };\n"
        "    match (u) { U.z => { return 1; } default => { return 0; } }\n"
        "}\n");
    check(!memberNoMember.ok && memberNoMember.stage == "check",
          "an unknown union member in a pattern fails");
    Checked emptyPat = check(
        "struct E {}\n"
        "func main() -> int {\n"
        "    var e: E = new E {};\n"
        "    match (e) { E {} => { return 5; } default => { return 0; } }\n"
        "}\n");
    check(emptyPat.ok, "an empty struct takes an empty pattern: " + emptyPat.stage);
    Checked emptyNonEmpty = check(
        "struct P { var x: int; }\n"
        "func main() -> int {\n"
        "    var p: P = new P { x: 1 };\n"
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
        "    var u: U = new U { a: 7 };\n"
        "    match (u) { U.a => { return 1; } U.b => { return 2; } }\n"
        "}\n");
    check(unionCovered.ok, "every union member listed needs no default: " + unionCovered.stage);
    Checked unionMissing = check(
        "union U { var a: int; var b: bool; }\n"
        "func main() -> int {\n"
        "    var u: U = new U { a: 7 };\n"
        "    match (u) { U.a => { return 1; } }\n"
        "}\n");
    check(!unionMissing.ok && unionMissing.stage == "check",
          "a missing union member needs a default");
    Checked unionOther = check(
        "union U { var a: int; var b: bool; }\n"
        "union V { var a: int; }\n"
        "func main() -> int {\n"
        "    var u: U = new U { a: 7 };\n"
        "    match (u) { U.a => { return 1; } V.a => { return 2; } }\n"
        "}\n");
    check(!unionOther.ok && unionOther.stage == "check",
          "another union's member does not cover");
    Checked unionSingle = check(
        "union One { var a: int; }\n"
        "func main() -> int {\n"
        "    var u: One = new One { a: 7 };\n"
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
        "    var p: P = new P { x: 1 };\n"
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
        "    var b: [u8; 8] = new [u8; 8] {[0, ...]};\n"
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
        "    var b: [u8; 4] = new [u8; 4] {[0, ...]};\n"
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
    check(!bare.ok && bare.stage == "check", "bare enumerator stays unbound");
}

void testGlobalConstWidth() {
    // A folded integer names image bytes, so one that does not fit the
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
        "    var a: [u8; 5] = new [u8; 5] { 10, 42, 43, 44, 50 };\n"
        "    var s: [u8; 3] = a[1:4];\n"
        "    return s[0] as int;\n"
        "}\n");
    check(same.ok, "a slice copies into a same-element array: " + same.stage);
    Checked wide = check(
        "func main() -> int {\n"
        "    var a: [u8; 5] = new [u8; 5] { 10, 42, 43, 44, 50 };\n"
        "    var s: [u16; 3] = a[1:4];\n"
        "    return 0;\n"
        "}\n");
    check(!wide.ok && wide.stage == "check",
          "a slice refuses a different element type");
}

}  // namespace

int main() {
    std::printf("check_test — well-typed programs check\n\n");
    testArithmetic();
    testNames();
    testGlobalZero();
    testFloatGlobal();
    testEnumValues();
    testGlobalConstWidth();
    testCalls();
    testInterface();
    testGenericCall();
    testGenericStruct();
    testUnions();
    testSelfEmbed();
    testFnAddr();
    testIncrDecr();
    testBoolCompare();
    testCharCompare();
    testStringCompare();
    testMatch();
    testAlternation();
    testGuard();
    testEnumMatch();
    testPointerArith();
    testRepeatZero();
    testSliceCopy();
    testAsm();
    testDelete();
    testLen();
    testFuncLitChecks();
    testControl();
    testThrowRefused();
    testForInProtocol();
    testInitAssign();
    testLoopMatchRefused();
    testRange();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
