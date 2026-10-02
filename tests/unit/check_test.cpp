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

}  // namespace

int main() {
    std::printf("check_test — well-typed programs check\n\n");
    testArithmetic();
    testNames();
    testCalls();
    testInterface();
    testControl();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
