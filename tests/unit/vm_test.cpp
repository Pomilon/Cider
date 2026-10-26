// vm_test.cpp — the interpreter answers without a toolchain.
//
// The first VM slice: source text lowers to an IR module, the interpreter
// runs the source-defined `_start`, and the exit code is the program's
// answer. No assembler, no linker, no fork. Both legs are reachable here:
// the unoptimised module the lowerer produced, and the same module through
// the optimiser.

#include <cstdio>
#include <string>

#include "alloc_fixture.h"
#include "entry_fixture.h"
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

Lowered lower(const std::string& text, bool checks = false,
              bool entryFixture = true) {
    Lowered out;
    out.types = std::make_unique<cider::TypeContext>();
    std::string source = cider_test::withAllocationFixture(text);
    if (entryFixture) {
        source = cider_test::withMainEntry(source);
    }
    cider::Lexer lexer("test.cid", source);
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
    cider::lower::Lowerer lowerer(*out.types, *resolver, *checker,
                                  cider::target::Kind::Native, checks);
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
        "func main() -> int { var xs: [int; 4] = [10, 20, 30, 40];"
        " var i: int = 2; return xs[i] as int; }\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module, "");
    check(unr.ok && unr.exitCode == 30, "a computed index reads 30: " + unr.error);
    Lowered sum = lower(
        "func main() -> int { var xs: [int; 4] = [10, 20, 30, 40];"
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
                        "    var a: [int; 2] = [1, 2];\n"
                        "    var i: int = 5;\n"
                        "    return a[i];\n"
                        "}\n", true);
    check(oob.ok, "an out-of-bounds program lowers: " + oob.stage);
    cider::vm::VmResult oobr = cider::vm::run(*oob.module, "");
    check(oobr.ok && oobr.signal == 0 && oobr.exitCode == 2 &&
              oobr.stderrText == "vm error: index out of bounds\n",
          "an out-of-bounds index diagnoses: " + oobr.stderrText + oobr.error);
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
    check(nr.ok && nr.signal == 0 && nr.exitCode == 2 &&
              nr.stderrText == "vm error: null pointer dereference\n",
          "a null dereference diagnoses: " + nr.stderrText + nr.error);
    // A union member read of a tag that was never written traps.
    Lowered dead = lower("union U { var i: int; var s: string; }\n"
                         "func main() -> int {\n"
                         "    var u: U = U { i: 3 };\n"
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
                         "func main() -> int { var p: P = P { a: 1.5 };"
                         " return p.a as int; }\n");
    check(field.ok, "a field program lowers: " + field.stage);
    cider::vm::VmResult fieldr = cider::vm::run(*field.module, "");
    check(fieldr.ok && fieldr.exitCode == 1, "a field holds its value: " + fieldr.error);
    Lowered narrow = lower("struct N { var a: f32; }\n"
                           "func main() -> int { var p: N = N { a: 0.5 };"
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
    Lowered pr = lower("func main() -> int {\n"
                       "    var msg: string = \"hi\";\n"
                       "    var ptr: *u8 = &msg[0] as *u8;\n"
                       "    var len: int = 2;\n"
                       "    asm {\n"
                       "        \"mov rax, 1\"\n"
                       "        \"mov rdi, 1\"\n"
                       "        \"mov rsi, {ptr}\"\n"
                       "        \"mov rdx, {len:q}\"\n"
                       "        \"syscall\"\n"
                       "    }\n"
                       "    return 3;\n"
                       "}\n");
    check(pr.ok, "an explicit write lowers: " + pr.stage);
    cider::vm::VmResult prr = cider::vm::run(*pr.module, "");
    check(prr.ok && prr.exitCode == 3, "the explicit write returns: " + prr.error);
    check(prr.stdoutText == "hi", "the explicit write emits bytes: " + prr.stdoutText);
    Lowered ch = lower("extern func putchar(c: int) -> int;\n"
                       "func main() -> int { putchar(33); return 0; }\n");
    check(ch.ok, "putchar lowers: " + ch.stage);
    cider::vm::VmResult chr = cider::vm::run(*ch.module, "");
    check(chr.ok && chr.stdoutText == "!", "putchar writes one byte: " + chr.stdoutText);
    Lowered dead = lower("func main() -> int { var p: *int = nilPtr;"
                         " return *p; }\n");
    cider::vm::VmResult deadr = cider::vm::run(*dead.module, "");
    check(deadr.ok && deadr.signal == 0 && deadr.exitCode == 2 &&
              deadr.stderrText == "vm error: null pointer dereference\n",
          "a nil read diagnoses: " + deadr.stderrText + deadr.error);
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
                        " var m: [[u8; 2]; 3] = [[0, ...], [0, ...], [0, ...]];\n"
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
                       " var p: *u8 = &s[0] as *u8; writeAll(p, 3);"
                       " asm { \"mov rax, 60\" \"mov rdi, 7\" \"syscall\" }"
                       " return 0; }\n");
    check(wr.ok, "a writing block lowers: " + wr.stage);
    cider::vm::VmResult wrr = cider::vm::run(*wr.module, "");
    check(wrr.stdoutText == "hi\n", "the block writes its bytes: " +
                                    wrr.stdoutText);
    check(wrr.exitCode == 7, "and the run ends after it: " + wrr.error);

    Lowered rd = lower("func main() -> int {"
                       " var buf: [u8; 4] = [0, ...];"
                       " var p: *u8 = &buf[0];"
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
    // End of input still validates the buffer first: the machine faults
    // a bad buffer before it ever reaches end-of-input, so a nil buffer
    // with a nonzero count answers EFAULT (-14), not success.
    Lowered eofbad = lower("func main() -> int {"
                           " var ret: i64 = 0;"
                           " asm { \"mov rax, 0\" \"mov rdi, 0\" \"mov rsi, 0\""
                           " \"mov rdx, 4\" \"syscall\" \"mov {ret:q}, rax\" }"
                           " if (ret == (0 - 14)) { return 0; }"
                           " return 1; }\n");
    check(eofbad.ok, "an EOF bad-buffer read lowers: " + eofbad.stage);
    cider::vm::VmResult eofbadr = cider::vm::run(*eofbad.module, "");
    check(eofbadr.ok && eofbadr.exitCode == 0,
          "an EOF bad-buffer read faults: " + eofbadr.error);

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

    // A function placeholder has an address word but no laid-down label:
    // the interpreter has no code address to hand out, so staging it
    // must refuse instead of answering silent zero.
    Lowered fn = lower("func two() -> int { return 2; }\n"
                       "func main() -> int {"
                       " asm { \"mov rax, {two}\" } return 0; }\n");
    check(fn.ok, "a function placeholder lowers: " + fn.stage);
    cider::vm::VmResult fnr = cider::vm::run(*fn.module, "");
    check(!fnr.ok, "a function placeholder refuses: " + fnr.error);
}

// File syscalls reach host files: create, write, seek, read back,
// close, and unlink, with errors answered the way native answers.
void testFileSyscalls() {
    std::printf("file syscalls run\n");
    std::remove("/tmp/cider_vm_fs_test.txt");
    Lowered fs = lower("func main() -> int {"
                       " var path: string = \"/tmp/cider_vm_fs_test.txt\";"
                       " var p: *u8 = &path[0] as *u8;"
                       " var fd: int = 0;"
                       " asm { \"mov rax, 2\" \"mov rdi, {p}\" \"mov rsi, 577\""
                       " \"mov rdx, 420\" \"syscall\" \"mov {fd}, eax\" }"
                       " if (fd < 0) { return 1; }"
                       " var msg: string = \"ABCD\";"
                       " var mp: *u8 = &msg[0] as *u8;"
                       " var nw: int = 0;"
                       " asm { \"mov rax, 1\" \"mov rdi, {fd}\""
                       " \"mov rsi, {mp}\" \"mov rdx, 4\""
                       " \"syscall\" \"mov {nw}, eax\" }"
                       " if (nw != 4) { return 2; }"
                       " var rc: int = 0;"
                       " asm { \"mov rax, 3\" \"mov rdi, {fd}\""
                       " \"syscall\" \"mov {rc}, eax\" }"
                       " if (rc != 0) { return 3; }"
                       " var fd2: int = 0;"
                       " asm { \"mov rax, 2\" \"mov rdi, {p}\""
                       " \"mov rsi, 0\" \"mov rdx, 0\""
                       " \"syscall\" \"mov {fd2}, eax\" }"
                       " if (fd2 < 0) { return 4; }"
                       " var off: int = 0;"
                       " asm { \"mov rax, 8\" \"mov rdi, {fd2}\""
                       " \"mov rsi, 1\" \"mov rdx, 0\""
                       " \"syscall\" \"mov {off}, eax\" }"
                       " if (off != 1) { return 5; }"
                       " var buf: [u8; 4] = [0, ...];"
                       " var bp: *u8 = &buf[0];"
                       " var nr: int = 0;"
                       " asm { \"mov rax, 0\" \"mov rdi, {fd2}\""
                       " \"mov rsi, {bp}\" \"mov rdx, 3\""
                       " \"syscall\" \"mov {nr}, eax\" }"
                       " if (nr != 3) { return 6; }"
                       " if (buf[0] != 66) { return 7; }"
                       " var rc2: int = 0;"
                       " asm { \"mov rax, 3\" \"mov rdi, {fd2}\""
                       " \"syscall\" \"mov {rc2}, eax\" }"
                       " if (rc2 != 0) { return 8; }"
                       " var ur: int = 0;"
                       " asm { \"mov rax, 87\" \"mov rdi, {p}\""
                       " \"syscall\" \"mov {ur}, eax\" }"
                       " if (ur != 0) { return 9; }"
                       " var fd3: int = 0;"
                       " asm { \"mov rax, 2\" \"mov rdi, {p}\""
                       " \"mov rsi, 0\" \"mov rdx, 0\""
                       " \"syscall\" \"mov {fd3}, eax\" }"
                       " if (fd3 >= 0) { return 10; }"
                       " return 0; }\n");
    check(fs.ok, "a file round-trip lowers: " + fs.stage);
    cider::vm::VmResult fsr = cider::vm::run(*fs.module, "");
    check(fsr.ok && fsr.exitCode == 0, "the round-trip answers: " +
                                       fsr.error);
    FILE* gone = fopen("/tmp/cider_vm_fs_test.txt", "r");
    check(gone == nullptr, "unlink removed the file");
    if (gone != nullptr) {
        fclose(gone);
    }
}

// v4 starts at the source-defined _start, never at main and never with
// compiler-bound argv. The test harness wraps ordinary main fixtures, so
// these cases use an unwrapped module to prove the VM rejects the old form.
void testEntry() {
    std::printf("source-defined entry\n");
    Lowered wrapped = lower("func main() -> int { return 5; }\n");
    check(wrapped.ok, "a harness-wrapped main lowers: " + wrapped.stage);
    cider::vm::VmResult wr = cider::vm::run(*wrapped.module, "");
    check(wr.ok && wr.exitCode == 5,
          "the source _start exits with main's value: " + wr.error);

    Lowered noEntry = lower("func main() -> int { return 0; }\n", false, false);
    check(noEntry.ok, "main alone still lowers as an ordinary function");
    cider::vm::VmResult nr = cider::vm::run(*noEntry.module, "");
    check(!nr.ok && nr.error.find("_start") != std::string::npos,
          "main alone is not an entry: " + nr.error);

    Lowered params =
        lower("func _start(x: int) -> void { }\n", false, false);
    check(params.ok, "a wrong _start still lowers");
    cider::vm::VmResult pr = cider::vm::run(*params.module, "");
    check(!pr.ok && pr.error.find("takes no parameters") != std::string::npos,
          "the VM rejects _start parameters: " + pr.error);

    Lowered ret = lower("func _start() -> int { return 0; }\n", false, false);
    check(ret.ok, "a value-returning _start still lowers");
    cider::vm::VmResult rr = cider::vm::run(*ret.module, "");
    check(!rr.ok && rr.error.find("must return void") != std::string::npos,
          "the VM rejects a value-returning _start: " + rr.error);
}

// super_init calls the base constructor on the same receiver (4.8.3), and
// both legs of the VM agree on the result.
void testSuperInit() {
    std::printf("super_init runs on both legs\n");
    Lowered unopt = lower("class Shape {\n"
                          "    var sides: int;\n"
                          "    func init(s: int) { this->sides = s; }\n"
                          "}\n"
                          "class Square : Shape {\n"
                          "    var w: int;\n"
                          "    func init(s: int, w: int) { super_init(s); this->w = w; }\n"
                          "}\n"
                          "func main() -> int {\n"
                          "    var q: *Square = new Square(4, 5);\n"
                          "    return q->sides * 10 + q->w;\n"
                          "}\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module, "");
    check(unr.ok && unr.exitCode == 45,
          "the unoptimised chain answers 45: " + unr.error);
    cider::opt::OptResult ores = cider::opt::run(*unopt.module);
    check(ores.ok, "optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*unopt.module, "");
    check(opr.ok && opr.exitCode == 45,
          "the optimised chain answers 45: " + opr.error);
    // 4.8.3 with 4.8's layout: the derived class introduces the table, so
    // its plain base sits after the vptr word and super_init must enter the
    // base subobject at that composed offset.
    Lowered shifted = lower("class P {\n"
                            "    var p: int = 7;\n"
                            "    func init() { this->p = this->p + 1; }\n"
                            "    func get() -> int { return this->p; }\n"
                            "}\n"
                            "class C : P {\n"
                            "    var c: int = 9;\n"
                            "    func init() { super_init(); }\n"
                            "    virtual func f() -> int { return this->c; }\n"
                            "}\n"
                            "func main() -> int {\n"
                            "    var c: *C = new C();\n"
                            "    return c->get() * 10 + c->f();\n"
                            "}\n");
    check(shifted.ok, "the shifted base lowers: " + shifted.stage);
    cider::vm::VmResult sr = cider::vm::run(*shifted.module, "");
    check(sr.ok && sr.exitCode == 89,
          "the unoptimised shifted base answers 89: " + sr.error);
    cider::opt::OptResult sres = cider::opt::run(*shifted.module);
    check(sres.ok, "the shifted base optimises: " + sres.error);
    cider::vm::VmResult sopr = cider::vm::run(*shifted.module, "");
    check(sopr.ok && sopr.exitCode == 89,
          "the optimised shifted base answers 89: " + sopr.error);
}

// `try_new` answers Result<*T, AllocError> and constructs only on success.
void testTryNew() {
    std::printf("try_new answers a Result\n");
    const std::string source =
        "var pool: [u8; 64] = [0, ...];\n"
        "var fail: bool = false;\n"
        "func operator new(size: i64, align: i64) -> *void {\n"
        "    if (fail) { return nilPtr; }\n"
        "    return &pool[0] as *void;\n"
        "}\n"
        "struct S { var x: int; var y: int; }\n"
        "class C { var x: int; func init(v: int) { (*self).x = v; } }\n"
        "func main() -> int {\n"
        "    var r: Result<*S, AllocError> = try_new S { x: 40, y: 2 };\n"
        "    if (!r.is_ok) { return 1; }\n"
        "    if (r.value->x + r.value->y != 42) { return 2; }\n"
        "    fail = true;\n"
        "    var e: Result<*S, AllocError> = try_new S { x: 1, y: 2 };\n"
        "    if (e.is_ok) { return 3; }\n"
        "    if (e.error != AllocError.OutOfMemory) { return 4; }\n"
        "    fail = false;\n"
        "    var c: Result<*C, AllocError> = try_new C(7);\n"
        "    if (!c.is_ok) { return 5; }\n"
        "    if (c.value->x != 7) { return 6; }\n"
        "    fail = true;\n"
        "    var d: Result<*C, AllocError> = try_new C(9);\n"
        "    if (d.is_ok) { return 7; }\n"
        "    if (d.error != AllocError.OutOfMemory) { return 8; }\n"
        "    return 0;\n"
        "}\n";
    Lowered unopt = lower(source);
    check(unopt.ok, "lowers: " + unopt.stage);
    if (!unopt.ok) {
        return;
    }
    cider::vm::VmResult unr = cider::vm::run(*unopt.module, "");
    check(unr.ok && unr.exitCode == 0,
          "the unoptimised try_new answers 0: " + unr.error);
    cider::opt::OptResult ores = cider::opt::run(*unopt.module);
    check(ores.ok, "optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*unopt.module, "");
    check(opr.ok && opr.exitCode == 0,
          "the optimised try_new answers 0: " + opr.error);
}

// 4.8.3: delete runs the destructor chain, and a hierarchy that carries a
// table starts it dynamically — both legs of the VM agree.
void testPolymorphicDelete() {
    std::printf("polymorphic delete on both legs\n");
    Lowered unopt = lower("var trace: int = 0;\n"
                          "class Shape {\n"
                          "    var w: int;\n"
                          "    func init(w: int) { this->w = w; }\n"
                          "    virtual func area() -> int { return this->w; }\n"
                          "    func deinit() { trace = trace * 10 + 2; }\n"
                          "}\n"
                          "class Square : Shape {\n"
                          "    var sides: int;\n"
                          "    func init(w: int, s: int) { super_init(w); this->sides = s; }\n"
                          "    override func area() -> int { return this->w * this->sides; }\n"
                          "    func deinit() { trace = trace * 10 + 1; }\n"
                          "}\n"
                          "func main() -> int {\n"
                          "    var q: *Square = new Square(4, 5);\n"
                          "    var s: *Shape = q as *Shape;\n"
                          "    delete s;\n"
                          "    return trace;\n"
                          "}\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module, "");
    check(unr.ok && unr.exitCode == 12,
          "the unoptimised chain answers 12: " + unr.error);
    cider::opt::OptResult ores = cider::opt::run(*unopt.module);
    check(ores.ok, "optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*unopt.module, "");
    check(opr.ok && opr.exitCode == 12,
          "the optimised chain answers 12: " + opr.error);
}

// A global class value carries its declared vtable in the data image, so
// the loader resolves the vptr relocation before dispatch runs.
void testGlobalClassVptr() {
    std::printf("global class vptr on both legs\n");
    Lowered unopt = lower("class V {\n"
                          "    var x: int;\n"
                          "    virtual func f() -> int { return 1; }\n"
                          "    virtual func g() -> int { return 10; }\n"
                          "}\n"
                          "var gv: V = V { x: 7 };\n"
                          "var ga: [V; 2] = [V { x: 8 }, V { x: 9 }];\n"
                          "func main() -> int {\n"
                          "    var vp: *V = &gv;\n"
                          "    var ap: *V = &ga[1];\n"
                          "    return vp->f() * 100 + ap->g() * 10 + vp->x;\n"
                          "}\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module, "");
    check(unr.ok && unr.exitCode == 207,
          "the unoptimised global answers 207: " + unr.error);
    cider::opt::OptResult ores = cider::opt::run(*unopt.module);
    check(ores.ok, "optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*unopt.module, "");
    check(opr.ok && opr.exitCode == 207,
          "the optimised global answers 207: " + opr.error);
}

void testMultipleBases() {
    std::printf("several bases on both legs\n");
    Lowered unopt = lower("class A {\n"
                          "    var a: int;\n"
                          "    func fromA() -> int { return this->a; }\n"
                          "}\n"
                          "class B {\n"
                          "    var b: i64;\n"
                          "    func fromB() -> int { return this->b as int; }\n"
                          "}\n"
                          "class C : A, B {\n"
                          "    var c: int;\n"
                          "    func init() { this->c = 3; }\n"
                          "}\n"
                          "func main() -> int {\n"
                          "    var x: *C = new C();\n"
                          "    x->a = 4;\n"
                          "    x->b = 5;\n"
                          "    var pa: *A = x as *A;\n"
                          "    var pb: *B = x as *B;\n"
                          "    return pb->fromB() * 10 + pa->fromA() + x->c;\n"
                          "}\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module, "");
    check(unr.ok && unr.exitCode == 57,
          "the unoptimised shape answers 57: " + unr.error);
    cider::opt::OptResult ores = cider::opt::run(*unopt.module);
    check(ores.ok, "optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*unopt.module, "");
    check(opr.ok && opr.exitCode == 57,
          "the optimised shape answers 57: " + opr.error);
}

void testNilFnptrCall() {
    std::printf("a nil function pointer diagnoses\n");
    Lowered none = lower(
        "func dbl(x: int) -> int { return x * 2; }\n"
        "func main() -> int {\n"
        "    var g: *func(int) -> int = &dbl;\n"
        "    if (g(3) != 6) { return 1; }\n"
        "    g = nilPtr;\n"
        "    return g(3);\n"
        "}\n");
    check(none.ok, "a nil-callee program lowers: " + none.stage);
    cider::vm::VmResult unr = cider::vm::run(*none.module, "");
    check(unr.ok && unr.signal == 0 && unr.exitCode == 2 &&
              unr.stderrText == "vm error: call: nil function pointer\n",
          "the unoptimised call diagnoses: " + unr.stderrText + unr.error);
    cider::opt::OptResult ores = cider::opt::run(*none.module);
    check(ores.ok, "optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*none.module, "");
    check(opr.ok && opr.signal == 0 && opr.exitCode == 2 &&
              opr.stderrText == "vm error: call: nil function pointer\n",
          "the optimised call diagnoses: " + opr.stderrText + opr.error);
}

// CIDER-BUG-049: defaults fill every construction before a constructor or
// a literal store can overwrite them.
void testFieldDefaults() {
    std::printf("field defaults on both legs\n");
    Lowered def = lower("class P {\n"
                        "    var x: int = 3;\n"
                        "    var y: int = 4;\n"
                        "    func sum() -> int { return this->x * 10 + this->y; }\n"
                        "}\n"
                        "class Q {\n"
                        "    var x: int = 3;\n"
                        "    var y: int;\n"
                        "    func init() { this->y = 9; }\n"
                        "    func sum() -> int { return this->x * 10 + this->y; }\n"
                        "}\n"
                        "func main() -> int {\n"
                        "    var a: *P = new P { };\n"
                        "    var b: *P = new P { x: 9 };\n"
                        "    var c: P = uninit;\n"
                        "    var q: *Q = new Q();\n"
                        "    return a->sum() + b->sum() + c.sum() + q->sum();\n"
                        "}\n");
    check(def.ok, "a defaulted program lowers: " + def.stage);
    cider::vm::VmResult unr = cider::vm::run(*def.module, "");
    check(unr.ok && unr.exitCode == 201,
          "the unoptimised defaults answer 201: " + unr.error);
    cider::opt::OptResult ores = cider::opt::run(*def.module);
    check(ores.ok, "optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*def.module, "");
    check(opr.ok && opr.exitCode == 201,
          "the optimised defaults answer 201: " + opr.error);
}

void testOperatorOverloadsVm() {
    std::printf("free operators run\n");
    Lowered p = lower(
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
    check(p.ok, "an operator program lowers: " + p.stage);
    if (!p.ok) {
        return;
    }
    cider::vm::VmResult unr = cider::vm::run(*p.module, "");
    check(unr.ok && unr.exitCode == 10,
          "the unoptimised operator answers 10: " + unr.error);
    cider::opt::OptResult ores = cider::opt::run(*p.module);
    check(ores.ok, "the operator program optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*p.module, "");
    check(opr.ok && opr.exitCode == 10,
          "the optimised operator answers 10: " + opr.error);

    Lowered u = lower(
        "struct Vec { var x: int; var y: int; }\n"
        "func operator-(v: Vec) -> Vec {\n"
        "    return Vec { x: -v.x, y: -v.y };\n"
        "}\n"
        "func main() -> int {\n"
        "    var a: Vec = Vec { x: 1, y: 2 };\n"
        "    var b: Vec = -a;\n"
        "    return 0 - (b.x + b.y);\n"
        "}\n");
    check(u.ok, "a unary operator program lowers: " + u.stage);
    if (!u.ok) {
        return;
    }
    cider::vm::VmResult uunr = cider::vm::run(*u.module, "");
    check(uunr.ok && uunr.exitCode == 3,
          "the unoptimised unary operator answers 3: " + uunr.error);
    cider::opt::OptResult uores = cider::opt::run(*u.module);
    check(uores.ok, "the unary operator program optimises: " + uores.error);
    cider::vm::VmResult uopr = cider::vm::run(*u.module, "");
    check(uopr.ok && uopr.exitCode == 3,
          "the optimised unary operator answers 3: " + uopr.error);

    Lowered ix = lower(
        "struct Vec { var x: int; var y: int;\n"
        "    func operator[](i: int) -> int {\n"
        "        if (i == 0) { return (*self).x; }\n"
        "        return (*self).y;\n"
        "    }\n"
        "}\n"
        "func main() -> int {\n"
        "    var v: Vec = Vec { x: 1, y: 2 };\n"
        "    return v[0] + v[1];\n"
        "}\n");
    check(ix.ok, "an index operator program lowers: " + ix.stage);
    if (!ix.ok) {
        return;
    }
    cider::vm::VmResult iunr = cider::vm::run(*ix.module, "");
    check(iunr.ok && iunr.exitCode == 3,
          "the unoptimised index operator answers 3: " + iunr.error);
    cider::opt::OptResult iores = cider::opt::run(*ix.module);
    check(iores.ok, "the index operator program optimises: " + iores.error);
    cider::vm::VmResult iopr = cider::vm::run(*ix.module, "");
    check(iopr.ok && iopr.exitCode == 3,
          "the optimised index operator answers 3: " + iopr.error);

    Lowered member = lower(
        "struct C { var x: int;\n"
        "    func operator+(a: C) -> C {\n"
        "        return C { x: (*self).x + a.x };\n"
        "    }\n"
        "    func operator-() -> C { return C { x: -(*self).x }; }\n"
        "}\n"
        "func main() -> int {\n"
        "    var a: C = C { x: 1 };\n"
        "    var b: C = C { x: 2 };\n"
        "    var c: C = a + b;\n"
        "    var d: C = -c;\n"
        "    return 0 - d.x;\n"
        "}\n");
    check(member.ok, "a member operator program lowers: " + member.stage);
    if (!member.ok) {
        return;
    }
    cider::vm::VmResult munr = cider::vm::run(*member.module, "");
    check(munr.ok && munr.exitCode == 3,
          "the unoptimised member operator answers 3: " + munr.error);
    cider::opt::OptResult mores = cider::opt::run(*member.module);
    check(mores.ok, "the member operator program optimises: " + mores.error);
    cider::vm::VmResult mopr = cider::vm::run(*member.module, "");
    check(mopr.ok && mopr.exitCode == 3,
          "the optimised member operator answers 3: " + mopr.error);

    Lowered callOp = lower(
        "struct Adder { var base: int;\n"
        "    func operator()(x: int) -> int { return (*self).base + x; }\n"
        "}\n"
        "func main() -> int {\n"
        "    var a: Adder = Adder { base: 4 };\n"
        "    return a(6);\n"
        "}\n");
    check(callOp.ok, "a call operator program lowers: " + callOp.stage);
    if (!callOp.ok) {
        return;
    }
    cider::vm::VmResult cunr = cider::vm::run(*callOp.module, "");
    check(cunr.ok && cunr.exitCode == 10,
          "the unoptimised call operator answers 10: " + cunr.error);
    cider::opt::OptResult cores = cider::opt::run(*callOp.module);
    check(cores.ok, "the call operator program optimises: " + cores.error);
    cider::vm::VmResult copr = cider::vm::run(*callOp.module, "");
    check(copr.ok && copr.exitCode == 10,
          "the optimised call operator answers 10: " + copr.error);

    Lowered assignOp = lower(
        "struct C { var x: int;\n"
        "    func operator=(other: C) -> void { (*self).x = other.x + 1; }\n"
        "}\n"
        "func main() -> int {\n"
        "    var a: C = C { x: 1 };\n"
        "    var b: C = C { x: 2 };\n"
        "    a = b;\n"
        "    return a.x;\n"
        "}\n");
    check(assignOp.ok, "an assignment operator program lowers: " + assignOp.stage);
    if (!assignOp.ok) {
        return;
    }
    cider::vm::VmResult aunr = cider::vm::run(*assignOp.module, "");
    check(aunr.ok && aunr.exitCode == 3,
          "the unoptimised assignment operator answers 3: " + aunr.error);
    cider::opt::OptResult aores = cider::opt::run(*assignOp.module);
    check(aores.ok, "the assignment operator program optimises: " + aores.error);
    cider::vm::VmResult aopr = cider::vm::run(*assignOp.module, "");
    check(aopr.ok && aopr.exitCode == 3,
          "the optimised assignment operator answers 3: " + aopr.error);

    Lowered compoundOp = lower(
        "struct C { var x: int;\n"
        "    func operator+=(other: C) -> void { (*self).x = (*self).x + other.x; }\n"
        "}\n"
        "func main() -> int {\n"
        "    var a: C = C { x: 1 };\n"
        "    var b: C = C { x: 2 };\n"
        "    a += b;\n"
        "    return a.x;\n"
        "}\n");
    check(compoundOp.ok,
          "a compound assignment program lowers: " + compoundOp.stage);
    if (!compoundOp.ok) {
        return;
    }
    cider::vm::VmResult punr = cider::vm::run(*compoundOp.module, "");
    check(punr.ok && punr.exitCode == 3,
          "the unoptimised compound assignment answers 3: " + punr.error);
    cider::opt::OptResult pores = cider::opt::run(*compoundOp.module);
    check(pores.ok, "the compound assignment optimises: " + pores.error);
    cider::vm::VmResult popr = cider::vm::run(*compoundOp.module, "");
    check(popr.ok && popr.exitCode == 3,
          "the optimised compound assignment answers 3: " + popr.error);
}

}  // namespace

void testArrowOperatorVm() {
    std::printf("arrow operator runs\n");
    Lowered p = lower(
        "struct Inner { var x: int; }\n"
        "class Ptr { var p: *Inner;\n"
        "    func operator->() -> *Inner { return this->p; }\n"
        "}\n"
        "func main() -> int {\n"
        "    var i: Inner = Inner { x: 7 };\n"
        "    var p: Ptr = Ptr { p: &i };\n"
        "    return p->x;\n"
        "}\n");
    check(p.ok, "an arrow operator program lowers: " + p.stage);
    if (!p.ok) {
        return;
    }
    cider::vm::VmResult unr = cider::vm::run(*p.module, "");
    check(unr.ok && unr.exitCode == 7,
          "the unoptimised arrow answers 7: " + unr.error);
    cider::opt::OptResult ores = cider::opt::run(*p.module);
    check(ores.ok, "the arrow program optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*p.module, "");
    check(opr.ok && opr.exitCode == 7,
          "the optimised arrow answers 7: " + opr.error);

    Lowered stored = lower(
        "struct Inner { var x: int; }\n"
        "class Ptr { var p: *Inner;\n"
        "    func operator->() -> *Inner { return this->p; }\n"
        "}\n"
        "func main() -> int {\n"
        "    var i: Inner = Inner { x: 7 };\n"
        "    var p: Ptr = Ptr { p: &i };\n"
        "    p->x = 3;\n"
        "    return p->x;\n"
        "}\n");
    check(stored.ok, "a store through operator-> lowers: " + stored.stage);
    if (stored.ok) {
        cider::vm::VmResult unrStore =
            cider::vm::run(*stored.module, "");
        check(unrStore.ok && unrStore.exitCode == 3,
              "the unoptimised arrow store answers 3: " + unrStore.error);
        cider::opt::OptResult storeOpt = cider::opt::run(*stored.module);
        check(storeOpt.ok, "the arrow store program optimises: " +
              storeOpt.error);
        cider::vm::VmResult optStore =
            cider::vm::run(*stored.module, "");
        check(optStore.ok && optStore.exitCode == 3,
              "the optimised arrow store answers 3: " + optStore.error);
    }
}

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
    testFileSyscalls();
    testEntry();
    testSuperInit();
    testTryNew();
    testPolymorphicDelete();
    testGlobalClassVptr();
    testMultipleBases();
    testNilFnptrCall();
    testFieldDefaults();
    testOperatorOverloadsVm();
    testArrowOperatorVm();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
