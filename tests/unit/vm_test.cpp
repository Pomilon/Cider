// vm_test.cpp — the interpreter answers without a toolchain.
//
// The first VM slice: source text lowers to an IR module, the interpreter
// runs main, and the exit code is the program's answer. No assembler, no
// linker, no fork. Both legs are reachable here: the unoptimised module the
// lowerer produced, and the same module through the optimiser.

#include <cstdio>
#include <string>

#include "check/checker.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
#include "opt/opt.h"
#include "parse/parser.h"
#include "types/context.h"
#include "vm/vm.h"

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

struct Lowered {
    bool ok = false;
    std::string stage;
    std::unique_ptr<cider::TypeContext> types;
    std::unique_ptr<cider::ir::Module> module;
};

Lowered lower(const std::string& text) {
    Lowered out;
    out.types = std::make_unique<cider::TypeContext>();
    cider::Lexer lexer("test.cid", text);
    std::vector<cider::Token> tokens = lexer.lex();
    if (!lexer.errors().empty()) {
        out.stage = "lex";
        return out;
    }
    cider::Parser parser("test.cid", std::move(tokens));
    std::unique_ptr<cider::Program> program = parser.parse();
    if (!parser.ok()) {
        out.stage = "parse";
        return out;
    }
    auto resolver = std::make_unique<cider::resolve::Resolver>(*out.types);
    if (!resolver->resolveProgram(*program, "main")) {
        out.stage = "resolve";
        return out;
    }
    auto checker = std::make_unique<cider::check::Checker>(*out.types, *resolver);
    cider::lower::Lowerer lowerer(*out.types, *resolver, *checker);
    if (!lowerer.lowerModule("main", *program)) {
        out.stage = "lower";
        return out;
    }
    out.module = lowerer.takeModule();
    out.ok = true;
    return out;
}

// Straight-line integer code answers its exit code on both legs.
void testExit() {
    std::printf("the interpreter answers\n");
    Lowered unopt = lower("func main() -> int { return 40 + 2; }\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module);
    check(unr.ok, "runs unoptimised: " + unr.error);
    check(unr.exitCode == 42, "the unoptimised answer is 42");
    cider::opt::OptResult ores = cider::opt::run(*unopt.module);
    check(ores.ok, "optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*unopt.module);
    check(opr.ok, "runs optimised: " + opr.error);
    check(opr.exitCode == 42, "the optimised answer is 42");
}

// Branches and loops run: if/else answers, while accumulates.
void testControl() {
    std::printf("branches and loops run\n");
    Lowered unopt = lower("func main() -> int {\n"
                          "    var t: int = 0;\n"
                          "    var i: int = 1;\n"
                          "    while (i <= 10) { t = t + i; i = i + 1; }\n"
                          "    if (t == 55) { return 7; }\n"
                          "    return 8;\n"
                          "}\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module);
    check(unr.ok, "runs unoptimised: " + unr.error);
    check(unr.exitCode == 7, "the loop sums to the taken arm");
}

// Integer arithmetic runs at its own width, so a narrow sum wraps and a
// signed divide truncates toward zero, exactly like the emitter's forms.
void testArith() {
    std::printf("integer arithmetic runs\n");
    Lowered unopt = lower("func main() -> int { return (20 / 6) + (20 % 6) * 2; }\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module);
    check(unr.ok && unr.exitCode == 7, "divide and remainder answer 7: " + unr.error);
    Lowered bits = lower("func main() -> int { return (12 & 10) | (12 ^ 10); }\n");
    cider::vm::VmResult bitr = cider::vm::run(*bits.module);
    check(bitr.ok && bitr.exitCode == 14, "and/or/xor answer 14: " + bitr.error);
    Lowered shifts = lower("func main() -> int { return (3 << 4) + (256 >> 4); }\n");
    cider::vm::VmResult shr = cider::vm::run(*shifts.module);
    check(shr.ok && shr.exitCode == 64, "shifts answer 64: " + shr.error);
    Lowered neg = lower("func main() -> int { return -(-5) + (~0); }\n");
    cider::vm::VmResult negr = cider::vm::run(*neg.module);
    check(negr.ok && negr.exitCode == 4, "neg and bitnot answer 4: " + negr.error);
    Lowered trunc = lower("func main() -> int { return -7 / 2; }\n");
    cider::vm::VmResult sr = cider::vm::run(*trunc.module);
    check(sr.ok && sr.exitCode == 253, "a signed divide truncates toward zero: " + sr.error);
    Lowered wrap = lower(
        "func main() -> int { var a: u8 = 200; var b: u8 = 100;"
        " var c: u8 = a + b; return c as int; }\n");
    cider::vm::VmResult wr = cider::vm::run(*wrap.module);
    check(wr.ok && wr.exitCode == 44, "a u8 sum wraps to 44: " + wr.error);
    Lowered wide = lower(
        "func main() -> int { var a: i16 = 30000; var b: i16 = 10000;"
        " var c: i16 = a + b; return c as int; }\n");
    cider::vm::VmResult wider = cider::vm::run(*wide.module);
    check(wider.ok && wider.exitCode == 64, "an i16 sum wraps to 64: " + wider.error);
    Lowered zero = lower("func main() -> int { var z: int = 0; return 1 / z; }\n");
    cider::vm::VmResult zr = cider::vm::run(*zero.module);
    check(!zr.ok, "a zero divisor is refused, not answered: " + zr.error);
}

// A computed index and a global both address memory the program does not
// name, so the interpreter has to run the address arithmetic.
void testMemory() {
    std::printf("computed addressing runs\n");
    Lowered unopt = lower(
        "func main() -> int { var xs: [int; 4] = new [int; 4] {10, 20, 30, 40};"
        " var i: int = 2; return xs[i] as int; }\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module);
    check(unr.ok && unr.exitCode == 30, "a computed index reads 30: " + unr.error);
    Lowered sum = lower(
        "func main() -> int { var xs: [int; 4] = new [int; 4] {10, 20, 30, 40};"
        " var s: int = 0; var i: int = 0;"
        " while (i < 4) { s = s + xs[i]; i = i + 1; } return s; }\n");
    cider::vm::VmResult sr = cider::vm::run(*sum.module);
    check(sr.ok && sr.exitCode == 100, "a computed loop sums 100: " + sr.error);
    Lowered glob = lower("var g: int = 5;\n"
                         "func main() -> int { g = g + 1; return g; }\n");
    check(glob.ok, "a global program lowers: " + glob.stage);
    cider::vm::VmResult gr = cider::vm::run(*glob.module);
    check(gr.ok && gr.exitCode == 6, "a global reads and writes 6: " + gr.error);
}

}  // namespace

int main() {
    std::printf("vm_test — the interpreter answers\n\n");
    testExit();
    testControl();
    testArith();
    testMemory();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
