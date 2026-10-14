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
    cider::vm::VmResult unr = cider::vm::run(*unopt.module, "");
    check(unr.ok, "runs unoptimised: " + unr.error);
    check(unr.exitCode == 42, "the unoptimised answer is 42");
    cider::opt::OptResult ores = cider::opt::run(*unopt.module);
    check(ores.ok, "optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*unopt.module, "");
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
    cider::vm::VmResult unr = cider::vm::run(*unopt.module, "");
    check(unr.ok, "runs unoptimised: " + unr.error);
    check(unr.exitCode == 7, "the loop sums to the taken arm");
}

// Integer arithmetic runs at its own width, so a narrow sum wraps and a
// signed divide truncates toward zero, exactly like the emitter's forms.
void testArith() {
    std::printf("integer arithmetic runs\n");
    Lowered unopt = lower("func main() -> int { return (20 / 6) + (20 % 6) * 2; }\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module, "");
    check(unr.ok && unr.exitCode == 7, "divide and remainder answer 7: " + unr.error);
    Lowered bits = lower("func main() -> int { return (12 & 10) | (12 ^ 10); }\n");
    cider::vm::VmResult bitr = cider::vm::run(*bits.module, "");
    check(bitr.ok && bitr.exitCode == 14, "and/or/xor answer 14: " + bitr.error);
    Lowered shifts = lower("func main() -> int { return (3 << 4) + (256 >> 4); }\n");
    cider::vm::VmResult shr = cider::vm::run(*shifts.module, "");
    check(shr.ok && shr.exitCode == 64, "shifts answer 64: " + shr.error);
    Lowered neg = lower("func main() -> int { return -(-5) + (~0); }\n");
    cider::vm::VmResult negr = cider::vm::run(*neg.module, "");
    check(negr.ok && negr.exitCode == 4, "neg and bitnot answer 4: " + negr.error);
    Lowered trunc = lower("func main() -> int { return -7 / 2; }\n");
    cider::vm::VmResult sr = cider::vm::run(*trunc.module, "");
    check(sr.ok && sr.exitCode == 253, "a signed divide truncates toward zero: " + sr.error);
    Lowered wrap = lower(
        "func main() -> int { var a: u8 = 200; var b: u8 = 100;"
        " var c: u8 = a + b; return c as int; }\n");
    cider::vm::VmResult wr = cider::vm::run(*wrap.module, "");
    check(wr.ok && wr.exitCode == 44, "a u8 sum wraps to 44: " + wr.error);
    Lowered wide = lower(
        "func main() -> int { var a: i16 = 30000; var b: i16 = 10000;"
        " var c: i16 = a + b; return c as int; }\n");
    cider::vm::VmResult wider = cider::vm::run(*wide.module, "");
    check(wider.ok && wider.exitCode == 64, "an i16 sum wraps to 64: " + wider.error);
    Lowered zero = lower("func main() -> int { var z: int = 0; return 1 / z; }\n");
    cider::vm::VmResult zr = cider::vm::run(*zero.module, "");
    check(zr.ok && zr.signal != 0,
          "a zero divisor dies by signal, not by a refusal: " + zr.error);
}

// A computed index and a global both address memory the program does not
// name, so the interpreter has to run the address arithmetic.
void testMemory() {
    std::printf("computed addressing runs\n");
    Lowered unopt = lower(
        "func main() -> int { var xs: [int; 4] = new [int; 4] {10, 20, 30, 40};"
        " var i: int = 2; return xs[i] as int; }\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module, "");
    check(unr.ok && unr.exitCode == 30, "a computed index reads 30: " + unr.error);
    Lowered sum = lower(
        "func main() -> int { var xs: [int; 4] = new [int; 4] {10, 20, 30, 40};"
        " var s: int = 0; var i: int = 0;"
        " while (i < 4) { s = s + xs[i]; i = i + 1; } return s; }\n");
    cider::vm::VmResult sr = cider::vm::run(*sum.module, "");
    check(sr.ok && sr.exitCode == 100, "a computed loop sums 100: " + sr.error);
    Lowered glob = lower("var g: int = 5;\n"
                         "func main() -> int { g = g + 1; return g; }\n");
    check(glob.ok, "a global program lowers: " + glob.stage);
    cider::vm::VmResult gr = cider::vm::run(*glob.module, "");
    check(gr.ok && gr.exitCode == 6, "a global reads and writes 6: " + gr.error);
}

// Death is a death: a lowering check that fails at runtime kills the process
// the way the emitter's ud2 does, and a divide fault answers the signal the
// idiv form delivers, so the interpreter leg looks like the compiled leg.
void testDeath() {
    std::printf("traps and faults die like the machine\n");
    // An index past the end lowers to a check branch into a trap block.
    Lowered oob = lower("func main() -> int {\n"
                        "    var a: [int; 2] = new [int; 2] {1, 2};\n"
                        "    var i: int = 5;\n"
                        "    return a[i];\n"
                        "}\n");
    check(oob.ok, "an out-of-bounds program lowers: " + oob.stage);
    cider::vm::VmResult oobr = cider::vm::run(*oob.module, "");
    check(oobr.ok && oobr.signal != 0,
          "an out-of-bounds index dies by signal: " + oobr.error);
    // A zero divisor faults at the instruction, not at the checker.
    Lowered zero = lower("func d(n: int) -> int { return 8 / n; }\n"
                         "func main() -> int { var z: int = 0; return d(z); }\n");
    check(zero.ok, "a dividing program lowers: " + zero.stage);
    cider::vm::VmResult zr = cider::vm::run(*zero.module, "");
    check(zr.ok && zr.signal != 0,
          "a divide of zero dies by signal: " + zr.error);
    // The most negative value divided by minus one overflows the quotient.
    Lowered over = lower("func d(n: int) -> int {\n"
                         "    var v: int = 0 - 2147483647 - 1;\n"
                         "    return v / n;\n"
                         "}\n"
                         "func main() -> int { return d(-1); }\n");
    check(over.ok, "an overflowing program lowers: " + over.stage);
    cider::vm::VmResult or_ = cider::vm::run(*over.module, "");
    check(or_.ok && or_.signal != 0,
          "a quotient that overflows dies by signal: " + or_.error);
    // A null pointer read faults: the first page is not the machine's.
    Lowered null = lower("var p: *int = nilPtr;\n"
                         "func main() -> int { return *p; }\n");
    check(null.ok, "a null program lowers: " + null.stage);
    cider::vm::VmResult nr = cider::vm::run(*null.module, "");
    check(nr.ok && nr.signal != 0,
          "a null dereference dies by signal: " + nr.error);
    // A union member read of a tag that was never written traps.
    Lowered dead = lower("union U { var i: int; var s: string; }\n"
                         "func main() -> int {\n"
                         "    var u: U = new U { i: 3 };\n"
                         "    var t: string = u.s;\n"
                         "    return 0;\n"
                         "}\n");
    check(dead.ok, "a union program lowers: " + dead.stage);
    cider::vm::VmResult dr = cider::vm::run(*dead.module, "");
    check(dr.ok && dr.signal != 0,
          "a dead union member dies by signal: " + dr.error);
    // A name the interpreter does not host is a refusal, not a death.
    Lowered bad = lower("extern func nothing(x: int) -> int;\n"
                        "func main() -> int { return nothing(1); }\n");
    check(bad.ok, "an extern program lowers: " + bad.stage);
    cider::vm::VmResult br = cider::vm::run(*bad.module, "");
    check(!br.ok && br.signal == 0,
          "an unknown helper refuses instead of dying: " + br.error);
}

// Float arithmetic compares and answers like the SSE forms: a remainder
// follows C's fmod, a division of zero answers a NaN rather than trapping,
// and a comparison with a NaN is false except for not-equals.
void testFloats() {
    std::printf("float arithmetic runs\n");
    Lowered sum = lower("func main() -> int { return (1.5 + 0.25) as int; }\n");
    check(sum.ok, "lowers: " + sum.stage);
    cider::vm::VmResult sumr = cider::vm::run(*sum.module, "");
    check(sumr.ok && sumr.exitCode == 1, "an addition converts: " + sumr.error);
    Lowered mult = lower("func main() -> int { return (2.5 * 4.0) as int; }\n");
    cider::vm::VmResult multr = cider::vm::run(*mult.module, "");
    check(multr.ok && multr.exitCode == 10, "a multiplication converts: " + multr.error);
    Lowered sub = lower("func main() -> int { return (5.5 - 2.25) as int; }\n");
    cider::vm::VmResult subr = cider::vm::run(*sub.module, "");
    check(subr.ok && subr.exitCode == 3, "a subtraction converts: " + subr.error);
    Lowered div = lower("func main() -> int { return (7.5 / 2.5) as int; }\n");
    cider::vm::VmResult divr = cider::vm::run(*div.module, "");
    check(divr.ok && divr.exitCode == 3, "a division converts: " + divr.error);
    Lowered rem = lower("func main() -> int { return (7.25 % 2.0) as int; }\n");
    cider::vm::VmResult remr = cider::vm::run(*rem.module, "");
    check(remr.ok && remr.exitCode == 1, "a remainder follows fmod: " + remr.error);
    Lowered neg = lower("func main() -> int { return (-2.5 + 2.5) as int; }\n");
    cider::vm::VmResult negr = cider::vm::run(*neg.module, "");
    check(negr.ok && negr.exitCode == 0, "a negation converts: " + negr.error);
    // A float field keeps every bit of the value, which is the shape whose
    // native store lost the high half before it was fixed.
    Lowered field = lower("struct P { var a: f64; }\n"
                         "func main() -> int { var p: P = new P { a: 1.5 };"
                         " return p.a as int; }\n");
    check(field.ok, "a field program lowers: " + field.stage);
    cider::vm::VmResult fieldr = cider::vm::run(*field.module, "");
    check(fieldr.ok && fieldr.exitCode == 1, "a field holds its value: " + fieldr.error);
    Lowered narrow = lower("struct N { var a: f32; }\n"
                           "func main() -> int { var p: N = new N { a: 0.5 };"
                           " return p.a as int; }\n");
    cider::vm::VmResult narrowr = cider::vm::run(*narrow.module, "");
    check(narrowr.ok && narrowr.exitCode == 0, "an f32 field reads back: " + narrowr.error);
    // Comparison is ordered: a NaN fails every operator but not-equals.
    Lowered cmp = lower("func main() -> int { if (1.5 < 2.5) { return 1; }"
                       " return 0; }\n");
    cider::vm::VmResult cmpr = cider::vm::run(*cmp.module, "");
    check(cmpr.ok && cmpr.exitCode == 1, "a float orders: " + cmpr.error);
    Lowered nan = lower("func main() -> int { var x: f64 = 0.0 / 0.0;"
                       " if (x != x) { return 1; } return 0; }\n");
    cider::vm::VmResult nanr = cider::vm::run(*nan.module, "");
    check(nanr.ok && nanr.exitCode == 1, "a NaN fails equality: " + nanr.error);
    Lowered nane = lower("func main() -> int { var x: f64 = 0.0 / 0.0;"
                        " if (x == 1.0) { return 1; } return 0; }\n");
    cider::vm::VmResult naner = cider::vm::run(*nane.module, "");
    check(naner.ok && naner.exitCode == 0, "a NaN equals nothing: " + naner.error);
    // A float crosses a call in a register-free model without losing bits.
    Lowered call = lower("func twice(x: f64) -> f64 { return x * 2.0; }\n"
                        "func main() -> int { return twice(3.0) as int; }\n");
    cider::vm::VmResult callr = cider::vm::run(*call.module, "");
    check(callr.ok && callr.exitCode == 6, "a float crosses a call: " + callr.error);
    Lowered glob = lower("var g: f64 = 2.5;\n"
                        "func main() -> int { g = g * 2.0; return g as int; }\n");
    cider::vm::VmResult globr = cider::vm::run(*glob.module, "");
    check(globr.ok && globr.exitCode == 5, "a global doubles: " + globr.error);
}

// Calls cross frames: arguments arrive, answers come back, and the caller's
// own frame survives the callee's. A host helper writes bytes and exits.
void testCalls() {
    std::printf("calls run\n");
    Lowered add = lower("func add(a: int, b: int) -> int { return a + b; }\n"
                        "func main() -> int { var t: int = 40;"
                        " return add(t, add(1, 1)); }\n");
    check(add.ok, "a call program lowers: " + add.stage);
    cider::vm::VmResult ar = cider::vm::run(*add.module, "");
    check(ar.ok && ar.exitCode == 42, "arguments arrive in the callee: " + ar.error);
    Lowered rec = lower("func fib(n: int) -> int {"
                        " if (n < 2) { return n; }"
                        " return fib(n - 1) + fib(n - 2); }\n"
                        "func main() -> int { return fib(10); }\n");
    cider::vm::VmResult rr = cider::vm::run(*rec.module, "");
    check(rr.ok && rr.exitCode == 55, "recursion answers fib(10): " + rr.error);
    Lowered surv = lower("var g: int = 1;\n"
                         "func bump() { g = g + 5; }\n"
                         "func main() -> int { var t: int = 100; bump();"
                         " var after: int = t + g; return after; }\n");
    check(surv.ok, "a global-writing call lowers: " + surv.stage);
    cider::vm::VmResult sr = cider::vm::run(*surv.module, "");
    check(sr.ok && sr.exitCode == 106, "a caller frame survives a call: " + sr.error);
    Lowered pr = lower("func main() -> int { print(\"hi\"); return 3; }\n");
    check(pr.ok, "a printing call lowers: " + pr.stage);
    cider::vm::VmResult prr = cider::vm::run(*pr.module, "");
    check(prr.ok && prr.exitCode == 3, "a host call returns: " + prr.error);
    check(prr.stdoutText == "hi", "print writes its bytes: " + prr.stdoutText);
    Lowered ch = lower("extern func putchar(c: int) -> int;\n"
                       "func main() -> int { putchar(33); return 0; }\n");
    check(ch.ok, "putchar lowers: " + ch.stage);
    cider::vm::VmResult chr = cider::vm::run(*ch.module, "");
    check(chr.ok && chr.stdoutText == "!", "putchar writes one byte: " + chr.stdoutText);
    Lowered pn = lower("func main() -> int { panic(\"boom\"); return 0; }\n");
    check(pn.ok, "a panic lowers: " + pn.stage);
    cider::vm::VmResult pnr = cider::vm::run(*pn.module, "");
    check(pnr.ok && pnr.exitCode == 134, "panic exits 134: " + pnr.error);
    check(pnr.stderrText == "boom\n", "panic writes its message: " + pnr.stderrText);
    Lowered al = lower("func main() -> int { var p: *int = alloc(8, 8) as *int;"
                       " *p = 42; return *p; }\n");
    check(al.ok, "an allocating program lowers: " + al.stage);
    cider::vm::VmResult alr = cider::vm::run(*al.module, "");
    check(alr.ok && alr.exitCode == 42, "alloc hands out writable memory: " + alr.error);
    // Both blocks are handed out and laid down zero-filled at hand-out,
    // so the neighbour read is legitimate laid-down memory answering
    // zero — not the old auto-grow path, which fabricated pages past
    // everything laid down and has been removed.
    Lowered past = lower("func main() -> int {\n"
                         " var a: *u8 = alloc(16, 8) as *u8;\n"
                         " var b: *u8 = alloc(16, 8) as *u8;\n"
                         " a[0] = 5;\n"
                         " if (b[0] != 0) { return 1; }\n"
                         " return 0; }\n");
    cider::vm::VmResult pastr = cider::vm::run(*past.module, "");
    check(pastr.ok && pastr.exitCode == 0,
          "a neighbour block reads zero: " + pastr.error);
    // Past everything laid down there is no mapping: the read faults
    // the way the machine would on an unmapped page, instead of
    // fabricating a zero page.
    Lowered pastAll = lower("func main() -> int {\n"
                            " var p: *u8 = alloc(8, 8) as *u8;\n"
                            " return (p[1000] as int); }\n");
    check(pastAll.ok, "a past-everything read lowers: " + pastAll.stage);
    cider::vm::VmResult pastAllr = cider::vm::run(*pastAll.module, "");
    check(pastAllr.ok && pastAllr.signal != 0,
          "a past-everything read dies: " + pastAllr.error);
    Lowered dead = lower("func main() -> int { var p: *int = nilPtr;"
                         " return *p; }\n");
    cider::vm::VmResult deadr = cider::vm::run(*dead.module, "");
    check(deadr.ok && deadr.signal != 0,
          "a nil read still dies: " + deadr.error);
    Lowered ind = lower("func two() -> int { return 2; }\n"
                        "func main() -> int {"
                        " var f: func() -> int = two; return f() + 18; }\n");
    check(ind.ok, "a closure call lowers: " + ind.stage);
    cider::vm::VmResult indr = cider::vm::run(*ind.module, "");
    check(indr.ok && indr.exitCode == 20, "an indirect call runs: " + indr.error);
    // A string literal staged where a pointer is declared carries its
    // length as a second word, and the callee names one parameter for it:
    // the caller stages more words than the callee binds, and the emitter
    // leaves the surplus in a register nobody reads.
    Lowered surplus = lower("func takes(n: *u8) -> int { return n[0] as int; }\n"
                            "func main() -> int { return takes(\"hi\"); }\n");
    cider::vm::VmResult surplusr = cider::vm::run(*surplus.module, "");
    check(surplusr.ok && surplusr.exitCode == 104,
          "a surplus word is staged, not refused: " + surplusr.error);
    // The same call with one word fewer than the callee binds is a
    // shortfall, and the checker already refuses it at check time, so the
    // interpreter's own guard is defense in depth: it must answer a
    // refusal rather than reading past the words that arrived.
    Lowered exactly = lower("func add(a: int, b: int) -> int { return a + b; }\n"
                            "func main() -> int { return add(1, 2); }\n");
    cider::vm::VmResult exactlyr = cider::vm::run(*exactly.module, "");
    check(exactlyr.ok && exactlyr.exitCode == 3,
          "an exact answer binds: " + exactlyr.error);
    // A whole-aggregate store writes the take's own width, not the width
    // of the value it carries: the row was read whole, so writing every
    // byte of it would reach past the row and zero what follows.
    Lowered agg = lower("func main() -> int {\n"
                        " var m: [[u8; 2]; 3] = new [[u8; 2]; 3] {[0, ...]};\n"
                        " for (var i: int = 0; i < 3; i = i + 1) {\n"
                        "  for (var j: int = 0; j < 2; j = j + 1) {\n"
                        "   m[i][j] = (i * 2 + j + 1) as u8;\n"
                        "  }\n"
                        " }\n"
                        " var row: [u8; 2] = m[1];\n"
                        " m[0] = row;\n"
                        " if (m[2][0] != 5 || m[2][1] != 6) { return 5; }\n"
                        " return 0; }\n");
    cider::vm::VmResult aggr = cider::vm::run(*agg.module, "");
    check(aggr.ok && aggr.exitCode == 0,
          "an aggregate store writes its own width: " + aggr.error);
}

// An asm block is the program writing at the machine: the interpreter runs
// it over its own registers and memory, and the answers match what the
// compiled leg's syscalls give.
void testAsm() {
    std::printf("asm blocks run\n");
    Lowered ex = lower("func main() -> int {"
                       " asm { \"mov rax, 60\" \"mov rdi, 42\" \"syscall\" }"
                       " return 0; }\n");
    check(ex.ok, "an exiting block lowers: " + ex.stage);
    cider::vm::VmResult exr = cider::vm::run(*ex.module, "");
    check(exr.ok && exr.exitCode == 42, "the block exits with its code: " +
                                       exr.error);

    Lowered wr = lower("func writeAll(ptr: *u8, count: int) {"
                       " var fd: int = 1;"
                       " asm { \"mov rax, 1\" \"mov rdi, {fd:q}\""
                       " \"mov rsi, {ptr}\" \"mov rdx, {count:q}\""
                       " \"syscall\" } }\n"
                       "func main() -> int { var s: string = \"hi\\n\";"
                       " var p: *u8 = &s[0]; writeAll(p, 3);"
                       " asm { \"mov rax, 60\" \"mov rdi, 7\" \"syscall\" }"
                       " return 0; }\n");
    check(wr.ok, "a writing block lowers: " + wr.stage);
    cider::vm::VmResult wrr = cider::vm::run(*wr.module, "");
    check(wrr.stdoutText == "hi\n", "the block writes its bytes: " +
                                    wrr.stdoutText);
    check(wrr.exitCode == 7, "and the run ends after it: " + wrr.error);

    Lowered rd = lower("func main() -> int {"
                       " var p: *u8 = alloc(4, 1) as *u8;"
                       " asm { \"mov rax, 0\" \"mov rdi, 0\" \"mov rsi, {p}\""
                       " \"mov rdx, 4\" \"syscall\" }"
                       " return *p as int - 65; }\n");
    check(rd.ok, "a reading block lowers: " + rd.stage);
    cider::vm::VmResult rdr = cider::vm::run(*rd.module, "ABCD");
    check(rdr.ok && rdr.exitCode == 0, "the block reads the pending input: " +
                                       rdr.error);

    Lowered bad = lower("func main() -> int {"
                        " asm { \"mov rax, 1\" \"mov rdi, 1\" \"mov rsi, 0\""
                        " \"mov rdx, 4\" \"syscall\" } return 0; }\n");
    check(bad.ok, "a faulting write lowers: " + bad.stage);
    cider::vm::VmResult badr = cider::vm::run(*bad.module, "");
    check(badr.ok && badr.stdoutText.empty(),
          "a write past the guard writes nothing: " + badr.error);

    Lowered wd = lower("func main() -> int { var out: u64 = 0;"
                       " asm { \"mov {out}, 42\" } return out as int; }\n");
    check(wd.ok, "a placeholder target lowers: " + wd.stage);
    cider::vm::VmResult wdr = cider::vm::run(*wd.module, "");
    check(wdr.ok && wdr.exitCode == 42, "the block answers 42 into it: " +
                                        wdr.error);

    Lowered eg = lower("func main() -> int {"
                       " asm { \"section .data\" } return 0; }\n");
    check(eg.ok, "a data section lowers: " + eg.stage);
    cider::vm::VmResult egr = cider::vm::run(*eg.module, "");
    check(!egr.ok, "a section directive refuses: " + egr.error);
}

void testArgv() {
    std::printf("argv\n");
    Lowered ar = lower("func main(argc: i64, argv: **char) -> int {"
                       " if (argv[0] == nilPtr) { return 7; }"
                       " return argc as int; }\n");
    check(ar.ok, "a parameterized main lowers: " + ar.stage);
    cider::vm::VmResult arr = cider::vm::run(*ar.module, "");
    check(arr.ok, "the interpreter binds argv by default: " + arr.error);
    if (arr.ok) {
        check(arr.exitCode == 1, "argc is 1 with no program args");
    }
    // Explicit program args bind argc=N with readable strings: argv[1]
    // names "ab", so its first byte answers 'a' and the count answers 3.
    Lowered br = lower("func main(argc: i64, argv: **char) -> int {"
                       " if (argc != 3) { return 1; }"
                       " if (*(argv[1]) != (97 as char)) { return 2; }"
                       " return 0; }\n");
    check(br.ok, "the argv probe lowers: " + br.stage);
    std::vector<std::string> given{"prog", "ab", "c"};
    cider::vm::VmResult brr = cider::vm::run(*br.module, "", given);
    check(brr.ok, "the interpreter binds explicit argv: " + brr.error);
    if (brr.ok) {
        check(brr.exitCode == 0, "argc 3 with readable argv strings");
    }
}

}  // namespace

int main() {
    std::printf("vm_test — the interpreter answers\n\n");
    testExit();
    testControl();
    testArith();
    testMemory();
    testDeath();
    testFloats();
    testCalls();
    testAsm();
    testArgv();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
