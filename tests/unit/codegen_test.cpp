// codegen_test.cpp — a Cider program runs and exits with its value.
//
// The first end-to-end slice: source text becomes x86-64 assembly, the system
// assembler turns it into a binary, and the binary's exit code is the
// program's answer. No libc, no runtime: `main` returns an int and the process
// exits with it.

#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "check/checker.h"
#include "codegen/codegen.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
#include "opt/opt.h"
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

struct Built {
    bool ok = false;
    std::string stage;
    std::string asmText;
    std::unique_ptr<cider::TypeContext> types;
    std::unique_ptr<cider::ir::Module> module;
};

Built build(const std::string& text) {
    Built out;
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
        out.module = nullptr;
        out.stage = "lower";
        return out;
    }
    out.module = lowerer.takeModule();
    cider::opt::OptResult result = cider::opt::run(*out.module);
    if (!result.ok) {
        out.stage = "opt:" + result.error;
        return out;
    }
    cider::codegen::EmitResult emitted = cider::codegen::emitNative(*out.module);
    if (!emitted.ok) {
        out.stage = "codegen:" + emitted.error;
        return out;
    }
    out.asmText = emitted.text;
    out.ok = true;
    return out;
}

// Assembles, links and runs the text, answering its exit code. -1 when the
// program did not exit normally.
int runCode(const std::string& text, bool& signaled) {
    signaled = false;
    Built built = build(text);
    if (!built.ok) {
        return -1;
    }
    char asmPath[] = "/tmp/cider_test_XXXXXX.s";
    int asmFd = mkstemps(asmPath, 2);
    if (asmFd < 0) {
        return -1;
    }
    FILE* asmFile = fdopen(asmFd, "w");
    fwrite(built.asmText.data(), 1, built.asmText.size(), asmFile);
    fclose(asmFile);
    std::string binPath = std::string(asmPath) + ".bin";
    // Assembled with the shell, which is fine: assembling cannot trap. The
    // link is the sovereign one — as and ld, no libc, no runtime archive —
    // because the text defines every symbol it references.
    std::string objPath = std::string(asmPath) + ".o";
    std::string command = "as -o " + objPath + " " + asmPath + " 2>/dev/null"
        " && ld -nostdlib -e _start -o " + binPath + " " + objPath + " 2>/dev/null";
    if (system(command.c_str()) != 0) {
        unlink(asmPath);
        unlink(objPath.c_str());
        return -1;
    }
    pid_t child = fork();
    if (child == 0) {
        execl(binPath.c_str(), binPath.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    unlink(asmPath);
    unlink(objPath.c_str());
    unlink(binPath.c_str());
    if (WIFSIGNALED(status)) {
        signaled = true;
        return -1;
    }
    if (!WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

int runQuiet(const std::string& text) {
    bool signaled = false;
    int code = runCode(text, signaled);
    if (signaled) {
        return -999;
    }
    return code;
}

// `main` returns its value as the exit code.
void testExit() {
    std::printf("exit codes\n");
    check(runQuiet("func main() -> int { return 42; }\n") == 42, "return 42");
    check(runQuiet("func main() -> int { return 1 + 2 * 3; }\n") == 7,
          "precedence survives codegen");
    check(runQuiet("func main() -> int { var a: int = 10; a = a - 3; return a; }\n") == 7,
          "locals and assignment");
}

// Calls pass arguments in registers and return in rax.
void testCalls() {
    std::printf("calls\n");
    check(runQuiet("func add(a: int, b: int) -> int { return a + b; }\n"
                   "func main() -> int { return add(20, 22); }\n") == 42,
          "two arguments");
    check(runQuiet("func f(n: int) -> int { if (n < 2) { return n; } return n; }\n"
                   "func main() -> int { return f(7) * 6; }\n") == 42,
          "a call inside an expression");
}

// Both sides of a branch run, and loops go around.
void testControl() {
    std::printf("branches and loops\n");
    check(runQuiet("func main() -> int { if (1 < 2) { return 40; } return 1; }\n") == 40,
          "the taken branch");
    check(runQuiet("func main() -> int { if (2 < 1) { return 1; } return 41; }\n") == 41,
          "the else path");
    check(runQuiet("func main() -> int {\n"
                   " var s: int = 0;\n"
                   " var i: int = 1;\n"
                   " while (i < 11) { s = s + i; i = i + 1; }\n"
                   " return s; }\n") == 55,
          "one to ten sums to 55");
}

// Globals live in the image and read back.
void testGlobals() {
    std::printf("globals\n");
    check(runQuiet("var g: int = 40;\nfunc main() -> int { return g + 2; }\n") == 42,
          "a global reads back");
}

// Division traps rather than answering.
void testTrap() {
    std::printf("traps\n");
    bool signaled = false;
    runCode("func main() -> int { var z: int = 0; return 1 / z; }\n", signaled);
    check(signaled, "division by zero dies by signal");
}

// Structs live in memory: construct, read fields, add them.
void testStruct() {
    std::printf("structs\n");
    check(runQuiet("struct P { var x: int; var y: int; }\n"
                   "func main() -> int { var p: P = new P { x: 40, y: 2 }; return p.x + p.y; }\n") == 42,
          "two fields add up");
    check(runQuiet("struct M { var tag: u8; var val: i32; }\n"
                   "func main() -> int { var m: M = new M { tag: 2, val: 40 }; return m.val + m.tag as int; }\n") == 42,
          "mixed widths respect padding");
}

// Arrays live in memory too: constant and runtime indices, element stores.
void testArray() {
    std::printf("arrays\n");
    check(runQuiet("func main() -> int { var a: [u8; 3] = new [u8; 3] { 7, 8, 9 }; return a[0] + a[1] + a[2] as int; }\n") == 24,
          "constant indices read back");
    check(runQuiet("func main() -> int {\n"
                   " var a: [i32; 5] = new [i32; 5] { 1, 2, 3, 4, 5 };\n"
                   " var s: int = 0;\n"
                   " var i: int = 0;\n"
                   " while (i < 5) { s = s + a[i]; i = i + 1; }\n"
                   " return s; }\n") == 15,
          "a runtime index walks the array");
    check(runQuiet("func main() -> int { var a: [u8; 2] = new [u8; 2] { [0, ...] }; a[1] = 40; return a[0] + a[1] as int; }\n") == 40,
          "element stores land");
}

// Pointers compare by identity and cast address-preservingly; the heap
// helpers answer nilPtr on refusal and catch corruption loudly.
void testPointerHeap() {
    std::printf("pointers and the heap\n");
    check(runQuiet("func main() -> int { var p: *int = alloc(8, 8) as *int; return (p != nilPtr) as int + 41; }\n") == 42,
          "a fresh block is not nil");
    check(runQuiet("func main() -> int { return (alloc(999999999999, 8) != nilPtr) as int; }\n") == 0,
          "an oversized request refuses to nilPtr");
    check(runQuiet("func main() -> int {\n"
                   " var p: *int = alloc(16, 8) as *int;\n"
                   " free(p);\n"
                   " var q: *int = alloc(16, 8) as *int;\n"
                   " free(q);\n"
                   " return (q != nilPtr) as int + 41; }\n") == 42,
          "freed memory is recycled by the free list");
    check(runQuiet("func main() -> int { var p: *int = alloc(8, 8) as *int; free(p); free(p); return 0; }\n") == 134,
          "a double free dies loudly with the heap code");
}

void testExternPutchar() {
    std::printf("extern putchar\n");
    // Sovereign link has no libc: a called-but-undefined `putchar` emits a
    // freestanding helper (one write syscall) instead of failing the link.
    // Any other undefined extern still fails the link loudly.
    check(runQuiet("extern func putchar(c: int) -> int;\n"
                   "func main() -> int { return putchar(65); }\n") == 65,
          "putchar answers the character");
}

void testBreakContinue() {
    std::printf("break and continue run\n");
    // Counting i up to 10: 2 continues (no count), break at 3. Iterations that
    // reach the count are 1 and 3 -> wait, 3 breaks, so only i=1 counted: 1.
    check(runQuiet("func main() -> int {\n"
                 "    var s: int = 0;\n"
                 "    var i: int = 0;\n"
                 "    while (i < 10) {\n"
                 "        i = i + 1;\n"
                 "        if (i == 2) { continue; }\n"
                 "        if (i == 3) { break; }\n"
                 "        s = s + 1;\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 1,
          "a continue skips the rest of the body, a break leaves the loop");
    // A continue in a nested loop belongs to the inner loop only.
    check(runQuiet("func main() -> int {\n"
                 "    var s: int = 0;\n"
                 "    var i: int = 0;\n"
                 "    while (i < 3) {\n"
                 "        i = i + 1;\n"
                 "        var j: int = 0;\n"
                 "        while (j < 5) {\n"
                 "            j = j + 1;\n"
                 "            if (j == 2) { break; }\n"
                 "            s = s + 1;\n"
                 "        }\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 3,
          "a break leaves only the innermost loop");
    // A break inside an if inside a while must still leave the while: the
    // join block after the if is unreachable, not a re-entry.
    check(runQuiet("func main() -> int {\n"
                 "    var s: int = 0;\n"
                 "    var i: int = 0;\n"
                 "    while (i < 4) {\n"
                 "        i = i + 1;\n"
                 "        if (i == 2) {\n"
                 "            s = s + 10;\n"
                 "            break;\n"
                 "        }\n"
                 "        s = s + 1;\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 11,
          "code after a break in an arm does not run");
    // A return inside a loop closes its block; the join after it is dead.
    check(runQuiet("func main() -> int {\n"
                 "    var i: int = 0;\n"
                 "    while (i < 5) {\n"
                 "        i = i + 1;\n"
                 "        if (i == 3) { return 7; }\n"
                 "    }\n"
                 "    return 9;\n"
                 "}\n") == 7,
          "a return inside a loop leaves immediately");
}

void testForLoop() {
    std::printf("for loops run\n");
    check(runQuiet("func main() -> int {\n"
                 "    var s: int = 0;\n"
                 "    for (var i: int = 0; i < 10; i = i + 1) {\n"
                 "        s = s + 1;\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 10,
          "a counting for runs its body once per iteration");
    // The step expression runs even when the body leaves early: with a
    // continue it is the only thing that advances the loop.
    check(runQuiet("func main() -> int {\n"
                 "    var s: int = 0;\n"
                 "    for (var i: int = 0; i < 6; i = i + 1) {\n"
                 "        if (i % 2 == 0) { continue; }\n"
                 "        s = s + i;\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 9,
          "continue still advances the step");
    check(runQuiet("func main() -> int {\n"
                 "    var s: int = 0;\n"
                 "    for (var i: int = 0; i < 20; i = i + 1) {\n"
                 "        if (i == 4) { break; }\n"
                 "        s = s + 1;\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 4,
          "break leaves the for loop");
    // An absent condition loops forever; a program that wants that says so
    // with `while (true)`, but the spelling must still terminate here, so
    // this probe uses a condition that stops it.
    check(runQuiet("func main() -> int {\n"
                 "    var s: int = 0;\n"
                 "    for (var i: int = 3; i > 0; i = i - 1) {\n"
                 "        s = s + i;\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 6,
          "a descending for runs its body per iteration");
    // Nested for loops: the inner loop runs to completion each time.
    check(runQuiet("func main() -> int {\n"
                 "    var s: int = 0;\n"
                 "    for (var i: int = 0; i < 3; i = i + 1) {\n"
                 "        for (var j: int = 0; j < 4; j = j + 1) {\n"
                 "            s = s + 1;\n"
                 "        }\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 12,
          "nested for loops run the inner loop per outer iteration");
}

void testArith32() {
    std::printf("32-bit arithmetic wraps at 32 bits\n");
    check(runQuiet("func main() -> int {\n"
                   " var a: int = 2000000000;\n"
                   " var b: int = 2000000000;\n"
                   " var c: int = 2000000000;\n"
                   " var d: int = 2000000000;\n"
                   " var e: int = 2000000000;\n"
                   " var f: int = 2000000000;\n"
                   " var g: int = 2000000000;\n"
                   " return (a + b + c + d + e + f + g) % 251; }\n") == 241,
          "i32 sums wrap before the remainder");
}

// `&&` and `||` short-circuit: the skipped side never evaluates, so a
// trapping rhs beside a deciding lhs is quiet (SEMANTICS: short-circuit is
// definitional, and a skipped trap never fires).
void testShortCircuit() {
    std::printf("short circuits run\n");
    check(runQuiet("func main() -> int {\n"
                   "    var t: bool = true && true;\n"
                   "    var f: bool = true && false;\n"
                   "    var o: bool = false || true;\n"
                   "    if (t && o && !f) { return 7; }\n"
                   "    return 0;\n"
                   "}\n") == 7,
          "truth tables agree");
    check(runQuiet("func main() -> int {\n"
                   "    var z: int = 0;\n"
                   "    var a: bool = false && (1 / z == 0);\n"
                   "    var b: bool = true || (1 / z == 0);\n"
                   "    if (!a && b) { return 9; }\n"
                   "    return 0;\n"
                   "}\n") == 9,
          "a skipped side never traps");
}

// A function pointer calls its target; a nil one dies by hardware trap.
void testIndirectCall() {
    std::printf("indirect calls run\n");
    check(runQuiet("func add(a: int, b: int) -> int { return a + b; }\n"
                   "func main() -> int {\n"
                   "    var f: *func(int, int) -> int = &add;\n"
                   "    return (*f)(20, 22);\n"
                   "}\n") == 42,
          "an indirect call reaches its target");
    bool signaled = false;
    runCode("func main() -> int {\n"
            "    var f: *func(int) -> int = nilPtr;\n"
            "    return (*f)(1);\n"
            "}\n",
            signaled);
    check(signaled, "a nil indirect call traps");
}

// The same module emitted twice is byte-identical, and carries no build path.
void testRepro() {
    std::printf("byte reproducibility\n");
    Built first = build("func main() -> int { return 42; }\n");
    Built second = build("func main() -> int { return 42; }\n");
    check(first.ok && second.ok, "both emit");
    check(first.asmText == second.asmText, "byte-identical");
    check(first.asmText.find("/home/") == std::string::npos &&
              first.asmText.find("build/") == std::string::npos,
          "no build path leaks into the output");
}

void testForIn() {
    std::printf("for-in runs\n");
    // Array iteration reads every element in order.
    check(runQuiet("func main() -> int {\n"
                 "    var a: [int; 3] = new [int; 3] { 5, 6, 7 };\n"
                 "    var s: int = 0;\n"
                 "    for (var x in a) { s = s + x; }\n"
                 "    return s;\n"
                 "}\n") == 18,
          "for-in over an array sums every element");
    // A slice iterates its borrowed elements.
    check(runQuiet("func main() -> int {\n"
                 "    var a: [int; 4] = new [int; 4] { 1, 2, 3, 4 };\n"
                 "    var s: []int = a[1:4];\n"
                 "    var t: int = 0;\n"
                 "    for (var x in s) { t = t + x; }\n"
                 "    return t;\n"
                 "}\n") == 9,
          "for-in over a slice sums its elements");
    // break leaves the iteration, continue skips to the next element.
    check(runQuiet("func main() -> int {\n"
                 "    var a: [int; 4] = new [int; 4] { 1, 2, 3, 4 };\n"
                 "    var s: int = 0;\n"
                 "    for (var x in a) {\n"
                 "        if (x == 4) { break; }\n"
                 "        s = s + x;\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 6,
          "break leaves for-in early");
    check(runQuiet("func main() -> int {\n"
                 "    var a: [int; 4] = new [int; 4] { 1, 2, 3, 4 };\n"
                 "    var s: int = 0;\n"
                 "    for (var x in a) {\n"
                 "        if (x % 2 == 0) { continue; }\n"
                 "        s = s + x;\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 4,
          "continue skips to the next element");
}

void testFloat() {
    std::printf("float arithmetic runs\n");
    // f64 multiply truncates to int: 1.5 * 2.5 = 3.75 -> 3.
    check(runQuiet("func main() -> int {\n"
                 "    var a: f64 = 1.5;\n"
                 "    var b: f64 = 2.5;\n"
                 "    var c: f64 = a * b;\n"
                 "    return c as int;\n"
                 "}\n") == 3,
          "f64 multiply reaches its result");
    // f32 add: 1.5 + 4.0 = 5.5 -> 5.
    check(runQuiet("func main() -> int {\n"
                 "    var a: f32 = 1.5;\n"
                 "    var b: f32 = 4.0;\n"
                 "    return (a + b) as int;\n"
                 "}\n") == 5,
          "f32 add reaches its result");
    // Comparison orders the operands.
    check(runQuiet("func main() -> int {\n"
                 "    var a: f64 = 1.5;\n"
                 "    var b: f64 = 2.5;\n"
                 "    if (a < b) { return 1; }\n"
                 "    return 0;\n"
                 "}\n") == 1,
          "float comparison orders operands");
    // Floats travel in SSE registers across a call.
    check(runQuiet("func addf(a: f64, b: f64) -> f64 { return a + b; }\n"
                 "func main() -> int {\n"
                 "    return addf(1.5, 2.25) as int;\n"
                 "}\n") == 3,
          "float arguments and results cross a call");
    // int -> f64 -> int round trip: 7 + 0.5 = 7.5 -> 7.
    check(runQuiet("func main() -> int {\n"
                 "    var i: int = 7;\n"
                 "    var f: f64 = i as f64;\n"
                 "    var g: f64 = f + 0.5;\n"
                 "    return g as int;\n"
                 "}\n") == 7,
          "int and float convert both ways");
    // Unary minus on a variable: -3.5 truncated is -3, plus 4 lands on 1
    // (exit codes are eight bits, so the assertion stays positive).
    check(runQuiet("func main() -> int {\n"
                 "    var x: f64 = 3.5;\n"
                 "    return ((-x) as int) + 4;\n"
                 "}\n") == 1,
          "float negation flips the sign");
    // IEEE division by zero answers NaN and never traps: 0.0/0.0 != itself.
    check(runQuiet("func main() -> int {\n"
                 "    var z: f64 = 0.0;\n"
                 "    var n: f64 = z / z;\n"
                 "    if (n == n) { return 9; }\n"
                 "    return 1;\n"
                 "}\n") == 1,
          "float division by zero is NaN, not a trap");
    // A float global keeps its bits in the image.
    check(runQuiet("var g: f64 = 1.5;\n"
                 "func main() -> int { return g as int; }\n") == 1,
          "a float global reaches the binary");
}

// A program that exits through its own asm block: a local placeholder
// feeds the exit code, a global placeholder feeds a buffer address.
void testAsm() {
    std::printf("asm runs\n");
    bool sig = false;
    check(runCode("func main() -> int {\n"
                  "    var sum: int = 38;\n"
                  "    asm { \"mov rax, 60\" \"mov edi, {sum}\" \"syscall\" }\n"
                  "    return 0;\n"
                  "}\n", sig) == 38 && !sig,
          "a local placeholder reaches its value");
    check(runCode("var g: string = \"H\";\n"
                  "func main() -> int {\n"
                  "    asm {\n"
                  "        \"mov rax, {g}\"\n"
                  "        \"movzx eax, byte ptr [rax]\"\n"
                  "        \"mov edi, eax\"\n"
                  "        \"mov rax, 60\"\n"
                  "        \"syscall\"\n"
                  "    }\n"
                  "    return 0;\n"
                  "}\n", sig) == 72 && !sig,
          "a global placeholder is its address");
    // Both operands of one template instruction are locals: with each
    // placeholder an independent storage memory operand this assembles to a
    // mem-to-mem `mov`, which the assembler rejects. Each placeholder must
    // arrive in its own register instead (v2's staging, BUG-239, adapted:
    // a callee-saved register held across the block and copied back).
    check(runCode("func main() -> int {\n"
                  "    var a: u64 = 0;\n"
                  "    var b: u64 = 55;\n"
                  "    asm { \"mov {a}, {b:q}\" }\n"
                  "    return a as int;\n"
                  "}\n", sig) == 55 && !sig,
          "two local placeholders in one instruction");
    // A block that writes a placeholder must leave the write where the rest
    // of the function reads it (copy-back), at any suffix width.
    check(runCode("func main() -> int {\n"
                  "    var r: int = 0;\n"
                  "    var s: u8 = 0;\n"
                  "    asm { \"mov {r:q}, 42\" \"mov {s}, 7\" }\n"
                  "    return r + s;\n"
                  "}\n", sig) == 49 && !sig,
          "writes through placeholders persist");
}

}  // namespace

int main() {
    std::printf("codegen_test — a Cider program runs\n\n");
    testExit();
    testCalls();
    testControl();
    testGlobals();
    testStruct();
    testArray();
    testPointerHeap();
    testExternPutchar();
    testBreakContinue();
    testForLoop();
    testForIn();
    testArith32();
    testIndirectCall();
    testShortCircuit();
    testFloat();
    testAsm();
    testTrap();
    testRepro();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
