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
    checker->prescanProvenance({"main"});
    cider::lower::Lowerer lowerer(*out.types, *resolver, *checker);
    if (!lowerer.lowerModule("main", *program)) {
        out.module = nullptr;
        out.stage = "lower";
        return out;
    }
    // Deferred monomorphization, mirroring the driver: specs emit after
    // all modules lower.
    if (!lowerer.emitSpecBodies()) {
        out.module = nullptr;
        out.stage = "lower-spec";
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

// Like runCode, but also captures the program's stderr into err.
int runCapture(const std::string& text, bool& signaled, std::string& err) {
    signaled = false;
    err.clear();
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
    std::string objPath = std::string(asmPath) + ".o";
    std::string command = "as -o " + objPath + " " + asmPath + " 2>/dev/null"
        " && ld -nostdlib -e _start -o " + binPath + " " + objPath + " 2>/dev/null";
    if (system(command.c_str()) != 0) {
        unlink(asmPath);
        unlink(objPath.c_str());
        return -1;
    }
    int errPipe[2] = {-1, -1};
    if (pipe(errPipe) != 0) {
        unlink(asmPath);
        unlink(objPath.c_str());
        unlink(binPath.c_str());
        return -1;
    }
    pid_t child = fork();
    if (child == 0) {
        dup2(errPipe[1], STDERR_FILENO);
        close(errPipe[0]);
        close(errPipe[1]);
        execl(binPath.c_str(), binPath.c_str(), nullptr);
        _exit(127);
    }
    close(errPipe[1]);
    char chunk[256];
    for (;;) {
        ssize_t got = read(errPipe[0], chunk, sizeof(chunk));
        if (got <= 0) {
            break;
        }
        err.append(chunk, static_cast<size_t>(got));
    }
    close(errPipe[0]);
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
void testStackArgs() {
    std::printf("stack arguments spill\n");
    check(runQuiet("func add8(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int) -> int {\n"
                   "    return a + b + c + d + e + f + g + h;\n"
                   "}\n"
                   "func main() -> int { return add8(1, 2, 3, 4, 5, 6, 7, 8); }\n") == 36,
          "eight integer arguments add up");
    check(runQuiet("func mix(a: int, b: int, c: int, d: int, e: int, g: int, s: string) -> int {\n"
                   "    return a + b + c + d + e + g + s.len as int;\n"
                   "}\n"
                   "func main() -> int { return mix(1, 2, 3, 4, 5, 6, \"abc\"); }\n") == 24,
          "a string tail rides the stack");
    check(runQuiet("func f8(a: int, b: int, c: int, d: int, e: int, g: int, h: int, i: int) -> float {\n"
                   "    return 1.0;\n"
                   "}\n"
                   "func use() -> int {\n"
                   "    var f: float = f8(1, 2, 3, 4, 5, 6, 7, 8);\n"
                   "    return f as int;\n"
                   "}\n"
                   "func main() -> int { return use(); }\n") == 1,
          "a float between register args still passes");
}

void testCalls() {
    std::printf("calls\n");
    check(runQuiet("func add(a: int, b: int) -> int { return a + b; }\n"
                   "func main() -> int { return add(20, 22); }\n") == 42,
          "two arguments");
    check(runQuiet("func f(n: int) -> int { if (n < 2) { return n; } return n; }\n"
                   "func main() -> int { return f(7) * 6; }\n") == 42,
          "a call inside an expression");
    // CIDER-BUG-252 audit lock: narrow arguments arrive width-correct.
    // v3 has no stack-spill call path (more than six arguments is a loud
    // refusal), so the stale-upper-bytes class cannot occur; every narrow
    // argument rides a register staged at its own width.
    check(runQuiet("func take(a: u8, b: i16, c: bool) -> int { return (a as int) + (b as int) + (c as int); }\n"
                   "func main() -> int { return take(200, -300, true); }\n") == 157,
          "narrow arguments arrive intact");
    check(runQuiet("func take(a: i8, b: u16, c: i16) -> int { return (a as int) + (b as int) + (c as int); }\n"
                   "func main() -> int { return take(-5, 60000, -300); }\n") == 47,
          "narrow arguments sign- and zero-extend");
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
    check(runQuiet("enum Color { RED, GREEN }\n"
                   "var g: Color = Color.GREEN;\n"
                   "func main() -> int { return g as int; }\n") == 1,
          "an enum global folds its enumerator");
    // An aggregate member past the first eight-byte line straddles it, so
    // the store and the read go through a computed address off the label
    // rather than the direct label-plus-offset form.
    check(runQuiet("struct P { var a: int; var b: int; }\n"
                   "struct Outer { var tail: int; var pair: P; }\n"
                   "var g: Outer = new Outer { tail: 0, pair: new P { a: 0, b: 0 } };\n"
                   "func main() -> int {\n"
                   "    var q: P = new P { a: 62, b: 63 };\n"
                   "    g.pair = q;\n"
                   "    return g.pair.a + g.pair.b;\n"
                   "}\n") == 125,
          "a straddling global member stores and reads back");
    // A computed index on a global array addresses through the label, not
    // a slot: the Gep base is a computed address value (see emitGep).
    check(runQuiet("var g: [u8; 4] = new [u8; 4] {[0, ...]};\n"
                   "func main() -> int {\n"
                   "    var j: int = 2;\n"
                   "    g[j] = 9;\n"
                   "    return g[j] as int;\n"
                   "}\n") == 9,
          "a computed global store reads back");
    // A literal construction names every image byte, so the global holds
    // its values with no startup code (FEATURES §8.3): u8 packs four to a
    // quad, u16 two, and narrow reads mask so padding is convention-safe.
    check(runQuiet("var g: [u8; 4] = new [u8; 4] {10, 20, 30, 40};\n"
                   "var w: [u16; 2] = new [u16; 2] {1000, 2000};\n"
                   "func main() -> int {\n"
                   "    return (g[0] as int) + (g[3] as int) + (w[1] as int);\n"
                   "}\n") == (10 + 40 + 2000) % 256,
          "a literal global array reads back");
    // A const struct global holds its fields at the layout's offsets
    // with no startup code: the image bytes match what `new` builds.
    check(runQuiet("struct Point { var x: int; var y: int; }\n"
                   "var g: Point = new Point { x: 3, y: 4 };\n"
                   "func main() -> int { return g.x + g.y; }\n") == 7,
          "a const struct global reads back");
    // A const class global opens with a null vtable word the way `new`
    // writes one, so its first field reads at word one, byte zero.
    check(runQuiet("class Box { var v: int; }\n"
                   "var g: Box = new Box { v: 5 };\n"
                   "func main() -> int { return g.v; }\n") == 5,
          "a const class global reads back");
}

// Division traps rather than answering.
void testInterp() {
    std::printf("interpolation builds the string\n");
    bool sig = false;
    check(runCode("func main() -> int {\n"
                  "    var v: string = \"ab{12}c\";\n"
                  "    return v.len as int;\n"
                  "}\n", sig) == 5 && !sig,
          "text and an integer join with the right length");
    check(runCode("func main() -> int {\n"
                  "    var v: string = \"ab{12}c\";\n"
                  "    return v[2] as int * 100 + v[3] as int;\n"
                  "}\n", sig) == (49 * 100 + 50) % 256 && !sig,
          "the integer prints decimal");
    check(runCode("func main() -> int {\n"
                  "    var t: bool = true;\n"
                  "    var v: string = \"v={t}!\";\n"
                  "    return v.len as int * 100 + v[2] as int;\n"
                  "}\n", sig) == (7 * 100 + 116) % 256 && !sig,
          "a bool prints true");
    check(runCode("func main() -> int {\n"
                  "    var ch: char = '!';\n"
                  "    var name: string = \"w\";\n"
                  "    var v: string = \"{ch}{name}?\";\n"
                  "    return v.len as int * 100 + v[0] as int;\n"
                  "}\n", sig) == (3 * 100 + 33) % 256 && !sig,
          "a char prints its byte beside a string");
    check(runCode("func main() -> int {\n"
                  "    var n: i64 = -7;\n"
                  "    var v: string = \"{n}\";\n"
                  "    return v.len as int * 100 + v[0] as int;\n"
                  "}\n", sig) == (2 * 100 + 45) % 256 && !sig,
          "a negative integer keeps its sign");
    check(runCode("func main() -> int {\n"
                  "    var f: f64 = 1.5;\n"
                  "    var v: string = \"{f}\";\n"
                  "    return v.len as int * 100 + v[0] as int;\n"
                  "}\n", sig) == (8 * 100 + 49) % 256 && !sig,
          "a float prints whole, point and six digits");
    check(runCode("func main() -> int {\n"
                  "    var f: f64 = 1.5;\n"
                  "    var v: string = \"{f}\";\n"
                  "    return v[1] as int * 100 + v[2] as int;\n"
                  "}\n", sig) == (46 * 100 + 53) % 256 && !sig,
          "the point and first fraction digit print");
    check(runCode("func main() -> int {\n"
                  "    var f: float = 0.5;\n"
                  "    var v: string = \"f={f}\";\n"
                  "    return v.len as int * 100 + v[2] as int;\n"
                  "}\n", sig) == (10 * 100 + 48) % 256 && !sig,
          "a float widens and zero-pads");
    check(runCode("func main() -> int {\n"
                  "    var f: f64 = -2.25;\n"
                  "    var v: string = \"{f}\";\n"
                  "    return v.len as int * 100 + v[0] as int;\n"
                  "}\n", sig) == (9 * 100 + 45) % 256 && !sig,
          "a negative float keeps its sign");
}

void testTrap() {
    std::printf("traps\n");
    bool signaled = false;
    runCode("func main() -> int { var z: int = 0; return 1 / z; }\n", signaled);
    check(signaled, "division by zero dies by signal");
}

// Panic ends the program end to end: the message lands on stderr and the
// process answers the panic code.
void testPanic() {
    std::printf("panic ends the program\n");
    bool signaled = false;
    std::string err;
    int code = runCapture("func main() -> int { panic(\"boom\"); return 0; }\n",
                          signaled, err);
    check(!signaled, "panic exits, it does not trap");
    check(code == 134, "panic answers 134");
    check(err == "boom\n", "panic writes its message to stderr");
}

// `&` on an element answers the element's address: stores through it land
// in the sequence, and an out-of-range address traps like the read would.
// Prefix and postfix `++` / `--` read-modify-write their place: a
// statement discards the value, postfix answers the old one, prefix the
// new one, and all of them work through pointers and in for-steps.
void testIncrDecr() {
    std::printf("incdec runs\n");
    check(runQuiet("func main() -> int {\n"
                   "    var i: int = 0;\n"
                   "    i++;\n"
                   "    ++i;\n"
                   "    i--;\n"
                   "    --i;\n"
                   "    ++i;\n"
                   "    return i;\n"
                   "}\n") == 1,
          "statements net one up");
    check(runQuiet("func main() -> int {\n"
                   "    var i: int = 5;\n"
                   "    var a: int = i++;\n"
                   "    var b: int = ++i;\n"
                   "    var c: int = i--;\n"
                   "    var d: int = --i;\n"
                   "    return a * 32 + b * 8 + c * 2 + d;\n"
                   "}\n") == 235,
          "postfix answers old, prefix answers new");
    check(runQuiet("func bump(p: *int) { (*p)++; }\n"
                   "func main() -> int {\n"
                   "    var x: int = 41;\n"
                   "    bump(&x);\n"
                   "    return x;\n"
                   "}\n") == 42,
          "increments through a pointer");
    check(runQuiet("func main() -> int {\n"
                   "    var s: int = 0;\n"
                   "    for (var i: int = 0; i < 10; i++) { s = s + 1; }\n"
                   "    return s;\n"
                   "}\n") == 10,
          "a for-step can increment");
}

void testElementAddr() {
    std::printf("element addresses\n");
    check(runQuiet("func main() -> int {\n"
                   "    var b: [u8; 4] = new [u8; 4] { 10, 20, 30, 40 };\n"
                   "    var j: int = 2;\n"
                   "    var p: *u8 = &b[j];\n"
                   "    *p = 99;\n"
                   "    return b[2] as int;\n"
                   "}\n") == 99,
          "a store through &b[j] lands");
    check(runQuiet("func main() -> int {\n"
                   "    var s: string = \"Hi\";\n"
                   "    var p: *u8 = &s[1];\n"
                   "    return *p as int;\n"
                   "}\n") == 105,
          "&s[1] reads the string byte");
    bool signaled = false;
    runCode("func main() -> int {\n"
            "    var s: string = \"Hi\";\n"
            "    var p: *u8 = &s[9];\n"
            "    return *p as int;\n"
            "}\n",
            signaled);
    check(signaled, "an out-of-range element address traps");
    // A cast over an address expression lowers the address first: `as`
    // binds looser than `&`, so without parens this would read as the
    // address of a cast, which the checker rejects.
    check(runQuiet("func get(p: *u8) -> u8 { return *p; }\n"
                   "func main() -> int {\n"
                   "    var b: [u8; 2] = new [u8; 2] { 65, 66 };\n"
                   "    var v: *void = (&b[1]) as *void;\n"
                   "    return get(v as *u8) as int;\n"
                   "}\n") == 66,
          "a cast of &b[1] round-trips through *void");
}

void testComputedIndex() {
    // A computed array index lowers on both sides with the same guards
    // the address-of path owns: SEMANTICS §8 checks every access, so a
    // read past the end traps instead of answering garbage (the old
    // read path emitted no check at all).
    std::printf("computed array indexes\n");
    check(runQuiet("func main() -> int {\n"
                   "    var a: [int; 4] = new [int; 4] { [0, ...] };\n"
                   "    var j: int = 0;\n"
                   "    while (j < 4) { a[j] = j * 10 + 1; j = j + 1; }\n"
                   "    return a[0] + a[1] + a[2] + a[3];\n"
                   "}\n") == 64,
          "computed stores fill and folded reads answer");
    // A packed narrow element compares by its own bytes only: loading
    // arr[1] as a full word would also read arr[2]'s bytes into the
    // comparison, so materialising the neighbour must not change the
    // answer (the old qword load failed this once the neighbour held
    // a nonzero value).
    check(runQuiet("func main() -> int {\n"
                   "    var arr: [u32; 3] = new [u32; 3] {[1, 2, 3]};\n"
                   "    arr[1] <<= 3;\n"
                   "    var p: *u32 = &arr[2];\n"
                   "    *p <<= 5;\n"
                   "    if (arr[1] != 16) { return 32; }\n"
                   "    if (arr[2] != 96) { return 33; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "a packed u32 compares without its neighbour's bytes");
    check(runQuiet("func main() -> int {\n"
                   "    var a: [int; 4] = new [int; 4] { 5, 6, 7, 8 };\n"
                   "    var j: int = 3;\n"
                   "    return a[j];\n"
                   "}\n") == 8,
          "a computed read answers the element");
    bool signaled = false;
    runCode("func main() -> int {\n"
            "    var a: [int; 4] = new [int; 4] { 5, 6, 7, 8 };\n"
            "    var j: int = 9;\n"
            "    return a[j];\n"
            "}\n",
            signaled);
    check(signaled, "a computed read past the end traps");
    signaled = false;
    runCode("func main() -> int {\n"
            "    var a: [int; 4] = new [int; 4] { 5, 6, 7, 8 };\n"
            "    var j: int = 9;\n"
            "    a[j] = 1;\n"
            "    return 0;\n"
            "}\n",
            signaled);
    check(signaled, "a computed store past the end traps");
    // A chained index over a computed inner one addresses through the
    // inner address: `m[i][j]` is the j-th element past the i-th row's
    // address, with both bounds checked.
    check(runQuiet("func main() -> int {\n"
                   "    var z: [u8; 2] = new [u8; 2] {0, 0};\n"
                   "    var m: [[u8; 2]; 3] = new [[u8; 2]; 3] {z, z, z};\n"
                   "    var i: int = 1;\n"
                   "    var j: int = 0;\n"
                   "    m[i][j] = 7;\n"
                   "    return (m[i][j] as int) + (m[2][1] as int);\n"
                   "}\n") == 7,
          "a chained store and read through a computed inner index");
    check(runQuiet("func main() -> int {\n"
                   "    var z: [u8; 2] = new [u8; 2] {0, 0};\n"
                   "    var m: [[u8; 2]; 3] = new [[u8; 2]; 3] {z, z, z};\n"
                   "    var i: int = 2;\n"
                   "    m[i][1] = 5;\n"
                   "    return (m[2][1] as int);\n"
                   "}\n") == 5,
          "a folded outer index advances a computed inner address");
    signaled = false;
    runCode("func main() -> int {\n"
            "    var z: [u8; 2] = new [u8; 2] {0, 0};\n"
            "    var m: [[u8; 2]; 3] = new [[u8; 2]; 3] {z, z, z};\n"
            "    var i: int = 9;\n"
            "    m[i][0] = 1;\n"
            "    return 0;\n"
            "}\n",
            signaled);
    check(signaled, "a chained store past the inner end traps");
    // A computed index through a pointer dereference addresses off the
    // loaded address: `(*p)[i]` is the i-th element past the pointer's
    // value, with the same bounds check the slot route owns.
    check(runQuiet("func at(p: *[int; 3], i: int) -> int { return (*p)[i]; }\n"
                   "func main() -> int {\n"
                   "    var a: [int; 3] = new [int; 3] { 4, 5, 6 };\n"
                   "    return at(&a, 2);\n"
                   "}\n") == 6,
          "a computed read through a pointer answers the element");
    check(runQuiet("struct S { var items: [int; 3]; }\n"
                   "func at(q: *S, i: int) -> int { return (*q).items[i]; }\n"
                   "func main() -> int {\n"
                   "    var s: S = new S { items: new [int; 3] { 4, 5, 6 } };\n"
                   "    return at(&s, 1);\n"
                   "}\n") == 5,
          "a computed read through a pointer field answers the element");
    check(runQuiet("func set(p: *[int; 3], i: int, v: int) { (*p)[i] = v; }\n"
                   "func main() -> int {\n"
                   "    var a: [int; 3] = new [int; 3] { 4, 5, 6 };\n"
                   "    var j: int = 1;\n"
                   "    set(&a, j, 42);\n"
                   "    return a[1];\n"
                   "}\n") == 42,
          "a computed store through a pointer lands");
    signaled = false;
    runCode("func at(p: *[int; 3], i: int) -> int { return (*p)[i]; }\n"
            "func main() -> int {\n"
            "    var a: [int; 3] = new [int; 3] { 4, 5, 6 };\n"
            "    return at(&a, 9);\n"
            "}\n",
            signaled);
    check(signaled, "a computed read past the end through a pointer traps");
}

void testPointerField() {
    // A field through a pointer element reads and writes the element,
    // not a neighbour: the store lands at the element address plus the
    // field offset, and the load is the field's own width.
    std::printf("fields through pointer elements\n");
    check(runQuiet("struct Pair { var lo: int; var hi: int; }\n"
                   "func main() -> int {\n"
                   "    var p: *Pair = alloc(16, 8) as *Pair;\n"
                   "    var tmp: Pair = new Pair { lo: 11, hi: 22 };\n"
                   "    p[0] = tmp;\n"
                   "    p[0].lo = 33;\n"
                   "    if (p[0].lo != 33) { return 1; }\n"
                   "    if (p[0].hi != 22) { return 2; }\n"
                   "    if (p[1].lo != 0) { return 3; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "a field store and read through a pointer index");
}

void testClassBox() {
    // A class value in an interface position boxes its address: the pair
    // aliases the value, so dispatch reads the live object, and a store
    // through a dereferenced class pointer re-boxes the pointed-to object.
    std::printf("class values box by reference\n");
    check(runQuiet("interface Shape { func area() -> int; }\n"
                   "class Sq { var s: int;"
                   " func area() -> int { return (*self).s * (*self).s; } }\n"
                   "func main() -> int {\n"
                   "    var a: Sq = new Sq { s: 9 };\n"
                   "    var t: Shape = a;\n"
                   "    return t.area();\n"
                   "}\n") == 81,
          "a class value boxes by reference");
    check(runQuiet("interface Shape { func area() -> int; }\n"
                   "class Rect { var w: int; var h: int;"
                   " func area() -> int { return (*self).w * (*self).h; } }\n"
                   "class Sq { var s: int;"
                   " func area() -> int { return (*self).s * (*self).s; } }\n"
                   "func main() -> int {\n"
                   "    var r: Rect = new Rect { w: 2, h: 5 };\n"
                   "    var s: Sq = new Sq { s: 1 };\n"
                   "    var t: Shape = s;\n"
                   "    var p: *Shape = &t;\n"
                   "    var q: *Rect = &r;\n"
                   "    *p = *q;\n"
                   "    return t.area();\n"
                   "}\n") == 10,
          "a dereferenced class pointer re-boxes");
}

void testMethodGlobalStore() {
    // A method's store to a global is observable outside the function, so
    // the optimizer must keep it: the store names a label, not a slot,
    // and no slot read can prove it dead.
    std::printf("method stores to globals\n");
    check(runQuiet("var fired: int = 0;\n"
                   "class N {\n"
                   "    var x: int;\n"
                   "    func init(v: int) { (*self).x = v; }\n"
                   "    func bump() -> int { fired = 7; return 7; }\n"
                   "}\n"
                   "func main() -> int {\n"
                   "    var n: *N = new N(0);\n"
                   "    var r: int = (*n).bump();\n"
                   "    if (fired != 7) { return 1; }\n"
                   "    return r;\n"
                   "}\n") == 7,
          "a method store to a global survives optimization");
}

void testRepeatZero() {
    // A zero repeat reaches through nested arrays: every scalar is zero,
    // and a store into the zeroed shape reads back.
    std::printf("zero repeat fills nested arrays\n");
    check(runQuiet("func main() -> int {\n"
                   "    var m: [[u8; 2]; 3] = new [[u8; 2]; 3] { [0, ...] };\n"
                   "    return (m[2][1] as int) + (m[0][0] as int);\n"
                   "}\n") == 0,
          "a zeroed nested array reads zero");
    check(runQuiet("func main() -> int {\n"
                   "    var m: [[u8; 2]; 3] = new [[u8; 2]; 3] { [0, ...] };\n"
                   "    m[1][0] = 41;\n"
                   "    return m[1][0] as int;\n"
                   "}\n") == 41,
          "a store into the zeroed shape reads back");
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
    check(runQuiet("struct A { var p: *A; var n: int; }\n"
                   "func main() -> int { var q: *A = alloc(16, 8) as *A; return (q != nilPtr) as int; }\n") == 1,
          "a pointer to self still declares and runs");
    // An array field reads back every element, including ones past the
    // first word: the take splitter shifts sub-word takes by bits.
    check(runQuiet("struct Box { var leaf: int; var values: [u16; 3]; }\n"
                   "func main() -> int {\n"
                   "    var b: Box = new Box { leaf: 41, values: new [u16; 3] { 5, 6, 7 } };\n"
                   "    return (b.values[2] as int);\n"
                   "}\n") == 7,
          "a struct array tail element reads back");
}

// Arrays live in memory too: constant and runtime indices, element stores.
void testArray() {
    std::printf("arrays\n");
    check(runQuiet("func main() -> int { var a: [u8; 3] = new [u8; 3] { 7, 8, 9 }; return a[0] + a[1] + a[2] as int; }\n") == 24,
          "constant indices read back");
    // Narrow takes pack byte-contiguously across value boundaries: an
    // element copy and an element passed straight to a call must read
    // back every byte, not just the first take.
    check(runQuiet("func main() -> int {\n"
                   "    var na: [[u8; 3]; 2] = new [[u8; 3]; 2] { new [u8; 3] { 7, 8, 9 }, new [u8; 3] { 10, 11, 12 } };\n"
                   "    var row: [u8; 3] = na[0];\n"
                   "    return row[2] as int;\n"
                   "}\n") == 9,
          "an element copy keeps its tail bytes");
    check(runQuiet("func sumRow(a: [u8; 3]) -> int { return (a[0] as int) + (a[1] as int) + (a[2] as int); }\n"
                   "func main() -> int {\n"
                   "    var na: [[u8; 3]; 2] = new [[u8; 3]; 2] { new [u8; 3] { 7, 8, 9 }, new [u8; 3] { 10, 11, 12 } };\n"
                   "    return sumRow(na[0]);\n"
                   "}\n") == 24,
          "an element passed to a call keeps its tail bytes");
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
    check(runQuiet("func main() -> int {\n"
                   "    var x: int = 41;\n"
                   "    var p: *int = &x;\n"
                   "    var pp: **int = &p;\n"
                   "    var q: **int = &*pp;\n"
                   "    return **q;\n"
                   "}\n") == 41,
          "address of a dereference nets one level and runs");
    check(runQuiet("func main() -> int { var p: *int = alloc(8, 8) as *int; free(p); free(p); return 0; }\n") == 134,
          "a double free dies loudly with the heap code");
}

// `delete` runs the deinit once, then releases the storage (FEATURES §5.3).
void testDelete() {
    std::printf("delete runs deinit, then releases\n");
    bool sig = false;
    check(runCode("var seen: int = 0;\n"
                  "class C {\n"
                  "    var x: int;\n"
                  "    func init(x: int) -> void { (*self).x = x; }\n"
                  "    func deinit() -> void { seen = (*self).x; }\n"
                  "}\n"
                  "func main() -> int {\n"
                  "    var p: *C = new C(41);\n"
                  "    delete p;\n"
                  "    return seen + 1;\n"
                  "}\n", sig) == 42 && !sig,
          "a deleted object's deinit ran");
}

// The heap's total-byte write must use an explicit `qword ptr`: GAS
// silently shifts a bare `qword [rip + label]` reference by 8 bytes, so
// the 1 GiB cap never fired and every request mapped (found live: two
// 900 MB maps, zero refusal). The text pin guards the exact site; the
// corpus cap_overflow/mmap_fail programs pin the behavior.
void testHeapCapText() {
    std::printf("heap helpers name sizes explicitly\n");
    Built built = build("func main() -> int {\n"
                        "    var p: *void = alloc(8, 8);\n"
                        "    if (p == nilPtr) { return 1; }\n"
                        "    free(p);\n"
                        "    return 0;\n"
                        "}\n");
    check(built.ok, "builds");
    check(built.asmText.find("qword ptr [rip + cider_heap_total]") !=
              std::string::npos,
          "the total-byte write is explicit");
    for (const char* bare : {"qword [", "dword [", "byte [", "word ["}) {
        check(built.asmText.find(bare) == std::string::npos,
              "no bare size names a memory operand");
    }
}

// A string literal's rodata bytes end in a zero byte: `*char` code
// reads to NUL (strlen), so the terminator is part of the literal, not
// luck in the following bytes. The length word excludes it.
void testLiteralNul() {
    std::printf("literals are NUL-terminated\n");
    Built built = build("func main() -> int {\n"
                        "    var p: *char = \"hi\";\n"
                        "    return (*p == \'h\') ? 1 : 0;\n"
                        "}\n");
    check(built.ok, "builds");
    check(built.asmText.find(".byte 104,105,0") != std::string::npos,
          "the rodata bytes end in zero");
    check(runQuiet("func main() -> int {\n"
                   "    var p: *char = \"hi\";\n"
                   "    return (*(p + 2) == (0 as char)) ? 1 : 0;\n"
                   "}\n") == 1,
          "the byte past the end reads zero");
}

void testExternPutchar() {    std::printf("extern putchar\n");
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
    // A u32 sum wraps at 32 bits too: the backend computes narrow results
    // at 64 bits, so the low 32 must be taken explicitly — a full 64-bit
    // store leaves 6000000000 where the type promises 1705032704.
    check(runQuiet("# experimental(wrap)\n"
                   "func main() -> int {\n"
                   " var a: u32 = (3000000000 as u32) + (3000000000 as u32);\n"
                   " if (a == 1705032704) { return 1; }\n"
                   " return 0; }\n") == 1,
          "u32 sums wrap before the comparison");
    // A global folds the same sum at compile time: under wrap the image
    // holds the low 32 bits, matching the local path exactly.
    check(runQuiet("# experimental(wrap)\n"
                   "var g: u32 = (3000000000 as u32) + (3000000000 as u32);\n"
                   "func main() -> int {\n"
                   " if (g == 1705032704) { return 1; }\n"
                   " return 0; }\n") == 1,
          "u32 global folds wrap before the comparison");
    // Mixed-sign pairs compare in the checker's common type (SEMANTICS
    // §2.3: same width goes unsigned, a wider signed operand stays
    // signed) — never in the left operand's type, which is what the v2
    // backend did and what mixed_signed_cmp pins down.
    check(runQuiet("func main() -> int {\n"
                   " var a: i32 = (0 - 1) as i32;\n"
                   " var b: u32 = 1;\n"
                   " if (a < b) { return 1; }\n"
                   " if (a <= b) { return 2; }\n"
                   " if (b > a) { return 3; }\n"
                   " return 0; }\n") == 0,
          "i32(-1) beside u32 compares unsigned");
    check(runQuiet("func main() -> int {\n"
                   " var t: u8 = 200;\n"
                   " var u: i32 = (0 - 1) as i32;\n"
                   " if (t > u) { return 1; }\n"
                   " return 0; }\n") == 1,
          "a wider signed operand keeps the signed domain");
    // Same-width cross-sign `as` normalizes the cell: i32(-1) `as` u32
    // is 0xFFFFFFFF, not the sign-extended cell the source keeps.
    check(runQuiet("func main() -> int {\n"
                   " var u: u32 = ((0 - 1) as i32) as u32;\n"
                   " if (u == 4294967295) { return 1; }\n"
                   " return 0; }\n") == 1,
          "i32(-1) as u32 reads all 32 low bits");
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
    // The word spellings ride the same ops: truth, precedence, and the
    // skipped side never evaluating (FEATURES §8.1).
    check(runQuiet("func main() -> int {\n"
                   "    var t: bool = true and true;\n"
                   "    var f: bool = true and false;\n"
                   "    var o: bool = false or true;\n"
                   "    if (t and o and not f) { return 7; }\n"
                   "    return 0;\n"
                   "}\n") == 7,
          "word truth tables agree");
    check(runQuiet("func main() -> int {\n"
                   "    var z: int = 0;\n"
                   "    var a: bool = false and (1 / z == 0);\n"
                   "    var b: bool = true or (1 / z == 0);\n"
                   "    if (not a or b) { return 9; }\n"
                   "    return 0;\n"
                   "}\n") == 9,
          "a skipped word side never traps");
}

void testBoolCompare() {
    std::printf("bool equality runs\n");
    check(runQuiet("var g: bool = false;\n"
                   "func main() -> int {\n"
                   "    g = !g;\n"
                   "    if (g != true) { return 1; }\n"
                   "    g = !g;\n"
                   "    if (g != false) { return 2; }\n"
                   "    var b: bool = true;\n"
                   "    b = !b;\n"
                   "    if (b != false) { return 3; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "negation and equality agree");
}

void testNotInCondition() {
    std::printf("bang-not in branch conditions runs\n");
    // CIDER-BUG-275: `if (!b)` segfaulted the driver (rc=135, SIGBUS)
    // while value-position `!b` refused cleanly. Both arms of the ticket
    // repro must run, plus the while-condition shape.
    check(runQuiet("func main() -> int {\n"
                   "    var b: bool = true;\n"
                   "    if (!b) { return 1; }\n"
                   "    return 2;\n"
                   "}\n") == 2,
          "negated-true takes the else arm");
    check(runQuiet("func main() -> int {\n"
                   "    var b: bool = false;\n"
                   "    if (!b) { return 1; }\n"
                   "    return 2;\n"
                   "}\n") == 1,
          "negated-false takes the then arm");
    check(runQuiet("func main() -> int {\n"
                   "    var b: bool = false;\n"
                   "    while (!b) { return 7; }\n"
                   "    return 2;\n"
                   "}\n") == 7,
          "negated-false enters the loop");
}

void testCharCompare() {
    std::printf("char equality runs\n");
    check(runQuiet("func main() -> int {\n"
                   "    var a: char = 'x';\n"
                   "    if (a != 'x') { return 1; }\n"
                   "    if (a == 'y') { return 2; }\n"
                   "    var g: char = a;\n"
                   "    if (g == 'x') { return 3; }\n"
                   "    return 0;\n"
                   "}\n") == 3,
          "equality distinguishes char values");
}

void testStringCompare() {
    std::printf("string equality runs\n");
    check(runQuiet("func main() -> int {\n"
                   "    var a: string = \"ab\";\n"
                   "    var b: string = \"ab\";\n"
                   "    var c: string = \"ac\";\n"
                   "    if (a != b) { return 1; }\n"
                   "    if (a == c) { return 2; }\n"
                   "    if (b == c) { return 3; }\n"
                   "    if (a == \"ab\") { return 4; }\n"
                   "    return 0;\n"
                   "}\n") == 4,
          "equality distinguishes string values");
    check(runQuiet("func main() -> int {\n"
                   "    var e: string = \"\";\n"
                   "    var n: string = nilPtr as string;\n"
                   "    var m: string = nilPtr as string;\n"
                   "    if (e != \"\") { return 1; }\n"
                   "    if (!(n == m)) { return 2; }\n"
                   "    if (n == e) { return 3; }\n"
                   "    if (e == n) { return 4; }\n"
                   "    if (n == \"x\") { return 5; }\n"
                   "    var p: *u8 = \"hi\";\n"
                   "    var q: *u8 = \"hi\";\n"
                   "    if (p == q) { return 6; }\n"
                   "    var r: *u8 = p;\n"
                   "    if (r != p) { return 7; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "empty, null, and address strings behave");
}

void testInterfaceNil() {
    std::printf("null interfaces run\n");
    check(runQuiet("interface Shape { func area() -> int; }\n"
                   "struct Sq { var s: int; func area() -> int { return (*self).s; } }\n"
                   "func main() -> int {\n"
                   "    var n: Shape = nilPtr as Shape;\n"
                   "    if (n == nilPtr) { return 1; }\n"
                   "    return 0;\n"
                   "}\n") == 1,
          "a null interface equals nilPtr");
    check(runQuiet("interface Shape { func area() -> int; }\n"
                   "struct Sq { var s: int; func area() -> int { return (*self).s; } }\n"
                   "func main() -> int {\n"
                   "    var s: Sq = new Sq { s: 7 };\n"
                   "    var i: Shape = s;\n"
                   "    if (i == nilPtr) { return 2; }\n"
                   "    if (i != nilPtr) { return i.area(); }\n"
                   "    return 0;\n"
                   "}\n") == 7,
          "a boxed interface differs from nilPtr and dispatches");
    check(runQuiet("interface Shape { func area() -> int; }\n"
                   "var g: Shape = nilPtr as Shape;\n"
                   "func main() -> int {\n"
                   "    if (g == nilPtr) { return 1; }\n"
                   "    return 0;\n"
                   "}\n") == 1,
          "a null interface global reads null");
    check(runQuiet("interface Shape { func area() -> int; }\n"
                   "struct Sq { var s: int; func area() -> int { return (*self).s; } }\n"
                   "var g: Shape = nilPtr as Shape;\n"
                   "func main() -> int {\n"
                   "    var s: Sq = new Sq { s: 7 };\n"
                   "    g = s;\n"
                   "    return g.area();\n"
                   "}\n") == 7,
          "a stored interface global dispatches");
    check(runQuiet("interface Shape { func area() -> int; }\n"
                   "struct Cell { var id: int; var mark: Shape; }\n"
                   "var g: Cell = new Cell { id: 3, mark: nilPtr as Shape };\n"
                   "func main() -> int {\n"
                   "    if (g.mark == nilPtr) { return g.id; }\n"
                   "    return 0;\n"
                   "}\n") == 3,
          "a null interface struct field folds into a global");
    check(runQuiet("interface Shape { func area() -> int; }\n"
                   "var g: [Shape; 2] = new [Shape; 2] { nilPtr as Shape, nilPtr as Shape };\n"
                   "func main() -> int {\n"
                   "    if (g[0] == nilPtr) { return 1; }\n"
                   "    return 0;\n"
                   "}\n") == 1,
          "a null interface array element reads null");
}

// A match runs the first arm whose pattern fits and skips the rest.
void testMatch() {
    std::printf("matches run\n");
    check(runQuiet("func main() -> int {\n"
                   "    var c: int = 2;\n"
                   "    match (c) { 1 | 2 => { return 7; } default => { return 0; } }\n"
                   "}\n") == 7,
          "the second alternative answers");
    check(runQuiet("func main() -> int {\n"
                   "    var c: int = 1;\n"
                   "    match (c) { 1 | 2 => { return 7; } default => { return 0; } }\n"
                   "}\n") == 7,
          "the first alternative answers");
    check(runQuiet("func main() -> int {\n"
                   "    var c: int = 9;\n"
                   "    match (c) { 1 | 2 => { return 7; } default => { return 0; } }\n"
                   "}\n") == 0,
          "a miss takes the default");
    check(runQuiet("func main() -> int {\n"
                   "    var b: bool = false;\n"
                   "    match (b) { true | false => { return 5; } }\n"
                   "}\n") == 5,
          "an alternation covers without a default");
    check(runQuiet("func main() -> int {\n"
                   "    var c: int = 2;\n"
                   "    match (c) { 1 => { return 10; } 2 => { return 20; } default => { return 30; } }\n"
                   "}\n") == 20,
          "a constant arm answers");
    check(runQuiet("func main() -> int {\n"
                   "    var c: int = 9;\n"
                   "    match (c) { 1 => { return 10; } default => { return 30; } }\n"
                   "}\n") == 30,
          "the default answers when nothing matches");
    check(runQuiet("func main() -> int {\n"
                   "    match (1) { 1 => { return 10; } 1 => { return 20; } default => { return 30; } }\n"
                   "}\n") == 10,
          "the first matching arm wins");
    check(runQuiet("func main() -> int {\n"
                   "    match (7) { x => { return x + 1; } default => { return 0; } }\n"
                   "}\n") == 8,
          "a fresh name binds the value");
    check(runQuiet("func main() -> int {\n"
                   "    var v: int = 5;\n"
                   "    var a: int = 0;\n"
                   "    match (5) { v => { a = 1; } default => { a = 2; } }\n"
                   "    match (6) { v => { a = a + 10; } default => { a = a + 20; } }\n"
                   "    return a;\n"
                   "}\n") == 21,
          "a bound name compares by value");
    check(runQuiet("func main() -> int {\n"
                   "    var s: string = \"hi\";\n"
                   "    match (s) { \"hi\" => { return 3; } default => { return 0; } }\n"
                   "}\n") == 3,
          "a string constant matches by content");
    check(runQuiet("func main() -> int {\n"
                   "    var n: string = nilPtr as string;\n"
                   "    match (n) { \"\" => { return 41; } default => { return 0; } }\n"
                   "}\n") == 0,
          "null is not the empty string");
    check(runQuiet("func main() -> int {\n"
                   "    var b: [u8; 2] = new [u8; 2] {104, 105};\n"
                   "    var p: *u8 = &b[0];\n"
                   "    match (p) { \"hi\" => { return 44; } default => { return 0; } }\n"
                   "}\n") == 0,
          "a dead string pattern takes the default");
    check(runQuiet("func main() -> int {\n"
                   "    var b: bool = true;\n"
                   "    match (b) { true => { return 1; } false => { return 2; } }\n"
                   "}\n") == 1,
          "a bool matches without a default");
    check(runQuiet("func main() -> int {\n"
                   "    var i: int = 0;\n"
                   "    var n: int = 0;\n"
                   "    while (i < 5) {\n"
                   "        match (i) { 2 => { break; } default => { n = n + 1; } }\n"
                   "        i = i + 1;\n"
                   "    }\n"
                   "    return n;\n"
                   "}\n") == 2,
          "break in an arm leaves the loop");
    check(runQuiet("struct P { var x: int; var y: int; }\n"
                   "func main() -> int {\n"
                   "    var p: P = new P { x: 1, y: 2 };\n"
                   "    match (p) { P { x: 1, y: 2 } => { return 10; } default => { return 30; } }\n"
                   "}\n") == 10,
          "a struct pattern matches its constants");
    check(runQuiet("struct P { var x: int; var y: int; }\n"
                   "func main() -> int {\n"
                   "    var p: P = new P { x: 1, y: 9 };\n"
                   "    match (p) { P { x: 1, y: 2 } => { return 10; } default => { return 30; } }\n"
                   "}\n") == 30,
          "a mismatched field takes the default");
    check(runQuiet("struct P { var x: int; var y: int; }\n"
                   "func main() -> int {\n"
                   "    var p: P = new P { x: 3, y: 4 };\n"
                   "    match (p) { P { x: a, y: _ } => { return a * 10; } default => { return 0; } }\n"
                   "}\n") == 30,
          "a struct pattern binds its fields");
    check(runQuiet("struct P { var x: int; }\n"
                   "struct Q { var x: int; }\n"
                   "func main() -> int {\n"
                   "    var p: P = new P { x: 1 };\n"
                   "    match (p) { Q { x: 1 } => { return 10; } default => { return 30; } }\n"
                   "}\n") == 30,
          "a dead struct pattern takes the default");
    check(runQuiet("union U { var a: int; var b: bool; }\n"
                   "func main() -> int {\n"
                   "    var u: U = new U { a: 7 };\n"
                   "    match (u) { U.a => { return 1; } U.b => { return 2; } default => { return 0; } }\n"
                   "}\n") == 1,
          "a union member pattern reads the tag");
    check(runQuiet("union U { var a: int; var b: bool; }\n"
                   "func main() -> int {\n"
                   "    var u: U = new U { b: true };\n"
                   "    match (u) { U.a => { return 1; } U.b => { return 2; } default => { return 0; } }\n"
                   "}\n") == 2,
          "the live member answers, not the first arm");
    check(runQuiet("union U { var a: int; var b: bool; }\n"
                   "func main() -> int {\n"
                   "    var u: U = new U { a: 7 };\n"
                   "    match (u) { U.a => { return 1; } U.b => { return 2; } }\n"
                   "}\n") == 1,
          "every union member listed needs no default");
    check(runQuiet("union U { var a: int; var b: bool; }\n"
                   "func main() -> int {\n"
                   "    var u: U = new U { b: true };\n"
                   "    match (u) { U.a => { return 1; } U.b => { return 2; } }\n"
                   "}\n") == 2,
          "the second member answers without a default");
    check(runQuiet("struct Inner { var x: int; }\n"
                   "struct Outer { var inner: Inner; var y: int; }\n"
                   "func main() -> int {\n"
                   "    var o: Outer = new Outer { inner: new Inner { x: 5 }, y: 6 };\n"
                   "    match (o) { Outer { inner: Inner { x: 5 }, y: _ } => { return 1; } default => { return 0; } }\n"
                   "}\n") == 1,
          "a nested struct pattern recurses");
    check(runQuiet("struct S { var s: string; var n: int; }\n"
                   "func main() -> int {\n"
                   "    var v: S = new S { s: \"hi\", n: 4 };\n"
                   "    match (v) { S { s: \"hi\", n: k } => { return k; } default => { return 0; } }\n"
                   "}\n") == 4,
          "a string field matches by content and binds beside it");
    check(runQuiet("union U { var a: int; var b: bool; }\n"
                   "struct P { var u: U; var n: int; }\n"
                   "func main() -> int {\n"
                   "    var p: P = new P { u: new U { b: true }, n: 3 };\n"
                   "    match (p) { P { u: U.a, n: _ } => { return 1; } P { u: U.b, n: m } => { return m; } default => { return 0; } }\n"
                   "}\n") == 3,
          "a member sub-pattern reads the tag and binds beside it");
    check(runQuiet("struct P { var x: int; var y: int; }\n"
                   "func main() -> int {\n"
                   "    var v: int = 1;\n"
                   "    var p: P = new P { x: 1, y: 2 };\n"
                   "    match (p) { P { x: v, y: _ } => { return 7; } default => { return 0; } }\n"
                   "}\n") == 7,
          "a bound name in a field compares by value");
    check(runQuiet("struct P { var x: int; var y: int; }\n"
                   "func main() -> int {\n"
                   "    var v: int = 9;\n"
                   "    var p: P = new P { x: 1, y: 2 };\n"
                   "    match (p) { P { x: v, y: _ } => { return 7; } default => { return 0; } }\n"
                   "}\n") == 0,
          "a mismatched bound name takes the default");
    check(runQuiet("struct E {}\n"
                   "func main() -> int {\n"
                   "    var e: E = new E {};\n"
                   "    match (e) { E {} => { return 5; } default => { return 0; } }\n"
                   "}\n") == 5,
          "an empty struct matches its empty pattern");
}

// A guard runs when the pattern fits: a false guard falls to the next arm,
// and the guard reads the arm's bound names.
void testGuard() {
    std::printf("match guards run\n");
    check(runQuiet("func main() -> int {\n"
                   "    var flag: bool = true;\n"
                   "    match (1) { 1 if flag => { return 7; } default => { return 0; } }\n"
                   "}\n") == 7,
          "a true guard takes the arm");
    check(runQuiet("func main() -> int {\n"
                   "    var flag: bool = false;\n"
                   "    match (1) { 1 if flag => { return 7; } default => { return 0; } }\n"
                   "}\n") == 0,
          "a false guard falls to the default");
    check(runQuiet("func main() -> int {\n"
                   "    match (2) { x if x > 1 => { return 10; } default => { return 30; } }\n"
                   "}\n") == 10,
          "a guard reads the freshly bound name");
    check(runQuiet("func main() -> int {\n"
                   "    match (2) { x if x > 5 => { return 10; } default => { return 30; } }\n"
                   "}\n") == 30,
          "a failing guard over a bind falls through");
    check(runQuiet("struct P { var x: int; var y: int; }\n"
                   "func main() -> int {\n"
                   "    var p: P = new P { x: 3, y: 4 };\n"
                   "    match (p) { P { x: a, y: _ } if a > 2 => { return 1; } default => { return 0; } }\n"
                   "}\n") == 1,
          "a guard reads a bound field");
    check(runQuiet("struct P { var x: int; var y: int; }\n"
                   "func main() -> int {\n"
                   "    var p: P = new P { x: 3, y: 4 };\n"
                   "    match (p) { P { x: a, y: _ } if a > 9 => { return 1; } default => { return 0; } }\n"
                   "}\n") == 0,
          "a failing guard over a field falls through");
}

// A range tests both bounds inclusive: below misses, above misses, the
// edges take the arm, and the comparison widens instead of truncating —
// a u8 255 is inside `0 ... 300`, and a negative i32 is outside `0 ...
// 10`, because the cell converts to 64 bits before either comparison.
void testRange() {
    std::printf("range patterns run\n");
    check(runQuiet("func main() -> int { match (5) { 0 ... 10 => { return 1; } default => { return 0; } } }\n") == 1,
          "inside the range takes the arm");
    check(runQuiet("func main() -> int { match (0) { 0 ... 10 => { return 1; } default => { return 0; } } }\n") == 1,
          "the lower bound is inclusive");
    check(runQuiet("func main() -> int { match (10) { 0 ... 10 => { return 1; } default => { return 0; } } }\n") == 1,
          "the upper bound is inclusive");
    check(runQuiet("func main() -> int { var x: int = 0 - 1; match (x) { 0 ... 10 => { return 1; } default => { return 0; } } }\n") == 0,
          "below the range falls through");
    check(runQuiet("func main() -> int { match (11) { 0 ... 10 => { return 1; } default => { return 0; } } }\n") == 0,
          "above the range falls through");
    check(runQuiet("func main() -> int { var x: u8 = 255; match (x) { 0 ... 300 => { return 1; } default => { return 0; } } }\n") == 1,
          "an out-of-width bound still matches what fits");
    check(runQuiet("func main() -> int { var x: i32 = 0 - 1; match (x) { 0 ... 10 => { return 1; } default => { return 0; } } }\n") == 0,
          "a negative value misses, signed");
    check(runQuiet("func main() -> int { var x: u64 = 18446744073709551615; match (x) { 0 ... 18446744073709551615 => { return 1; } default => { return 0; } } }\n") == 1,
          "a full-width u64 range matches");
    check(runQuiet("struct P { var x: int; }\n"
                   "func main() -> int {\n"
                   "    var p: P = new P { x: 2 };\n"
                   "    match (p) { P { x: 1 ... 3 } => { return 1; } default => { return 0; } }\n"
                   "}\n") == 1,
          "a nested range tests the field");
    check(runQuiet("func main() -> int { match (7) { 0 ... 5 | 7 => { return 1; } default => { return 0; } } }\n") == 1,
          "a range alternates with a constant");
    check(runQuiet("func main() -> int {\n"
                   "    var ok: bool = true;\n"
                   "    match (4) { 0 ... 10 if ok => { return 1; } default => { return 0; } }\n"
                   "}\n") == 1,
          "a guard still gates a range");
}

// An enumerator pattern compares the cell with the named case: the first
// match in source order wins, a dead arm never fires, and every case
// listed needs no default (FEATURES §8.4).
void testEnumMatch() {
    std::printf("enum member patterns run\n");
    check(runQuiet("enum Color { RED, GREEN, BLUE = 5 }\n"
                   "func main() -> int {\n"
                   "    var c: Color = 1 as Color;\n"
                   "    match (c) { Color.RED => { return 1; } Color.GREEN => { return 2; } Color.BLUE => { return 3; } default => { return 0; } }\n"
                   "}\n") == 2,
          "the named case answers");
    check(runQuiet("enum Color { RED, GREEN, BLUE = 5 }\n"
                   "func main() -> int {\n"
                   "    var c: Color = 5 as Color;\n"
                   "    match (c) { Color.RED => { return 1; } Color.GREEN => { return 2; } Color.BLUE => { return 3; } default => { return 0; } }\n"
                   "}\n") == 3,
          "an explicit enumerator value answers");
    check(runQuiet("enum Color { RED, GREEN }\n"
                   "enum Size { BIG, SMALL }\n"
                   "func main() -> int {\n"
                   "    var c: Color = 0 as Color;\n"
                   "    match (c) { Size.BIG => { return 9; } Color.RED => { return 1; } default => { return 0; } }\n"
                   "}\n") == 1,
          "a dead arm never fires");
    check(runQuiet("enum Color { RED, GREEN }\n"
                   "func main() -> int {\n"
                   "    var c: Color = 1 as Color;\n"
                   "    match (c) { Color.RED => { return 1; } Color.GREEN => { return 2; } }\n"
                   "}\n") == 2,
          "every case listed needs no default at runtime");
    check(runQuiet("enum Color { RED, GREEN }\n"
                   "func main() -> int {\n"
                   "    var c: Color = 1 as Color;\n"
                   "    match (c) { 1 => { return 9; } default => { return 7; } }\n"
                   "}\n") == 7,
          "a raw number never names an enum case at runtime");
    check(runQuiet("enum Color { RED, GREEN, BLUE }\n"
                   "func main() -> int {\n"
                   "    var c: Color = 2 as Color;\n"
                   "    match (c) { RED => { return 1; } Color.GREEN => { return 2; } BLUE => { return 3; } default => { return 0; } }\n"
                   "}\n") == 3,
          "bare enumerators compare by value");
}

void testPointerArith() {
    std::printf("pointer arithmetic runs\n");
    check(runQuiet("func main() -> int {\n"
                   "    var b: [u8; 8] = new [u8; 8] {[0, ...]};\n"
                   "    var p: *u8 = &b[0];\n"
                   "    *(p + 2) = 7;\n"
                   "    if (((*(2 + p)) as int) != 7) { return 6; }\n"
                   "    var q: *u8 = p + 2;\n"
                   "    var n: i64 = q - p;\n"
                   "    var v: u8 = p[2];\n"
                   "    p[1] = 9;\n"
                   "    if (v != 7) { return 1; }\n"
                   "    if ((n as int) != 2) { return 2; }\n"
                   "    if ((b[2] as int) != 7) { return 3; }\n"
                   "    if ((b[1] as int) != 9) { return 4; }\n"
                   "    if ((*q as int) != 7) { return 5; }\n"
                   "    p += 1;\n"
                   "    *p = 11;\n"
                   "    p -= 1;\n"
                   "    if ((b[1] as int) != 11) { return 6; }\n"
                   "    if ((*p as int) != 0) { return 7; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "p+n, n+p, p-q, p[i], and stores agree");
}

void testEnumValues() {
    std::printf("enumerators run as integers\n");
    check(runQuiet("enum Color { RED, GREEN, BLUE = 5 }\n"
                   "func main() -> int {\n"
                   "    var c: int = Color.RED;\n"
                   "    if (c != 0) { return 1; }\n"
                   "    c = Color.GREEN;\n"
                   "    if (c != 1) { return 2; }\n"
                   "    c = Color.BLUE;\n"
                   "    if (c != 5) { return 3; }\n"
                   "    return c;\n"
                   "}\n") == 5,
          "enumerators sequence and honor explicit values");
    check(runQuiet("enum Color { RED, GREEN, BLUE = 5 }\n"
                   "func main() -> int {\n"
                   "    var c: Color = GREEN;\n"
                   "    if ((c as int) != 1) { return 1; }\n"
                   "    var i: int = BLUE;\n"
                   "    if (i != 5) { return 2; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "bare enumerators lower to their values");
}

// An enum holds its underlying i32: casts both ways lower, and inventing
// a value no enumerator names traps (FEATURES §2, enum ↔ int row).
void testEnumCast() {
    std::printf("enums cast to and from integers\n");
    check(runQuiet("enum Color { RED, GREEN, BLUE = 5 }\n"
                   "func main() -> int {\n"
                   "    var c: Color = Color.GREEN as Color;\n"
                   "    return c as int;\n"
                   "}\n") == 1,
          "an enumerator round-trips through its enum");
    check(runQuiet("enum Color { RED, GREEN, BLUE = 5 }\n"
                   "func main() -> int {\n"
                   "    var c: Color = Color.BLUE as Color;\n"
                   "    return c as int;\n"
                   "}\n") == 5,
          "an explicit enumerator value survives the cast");
    check(runQuiet("enum Color { RED, GREEN, BLUE = 5 }\n"
                   "func main() -> int {\n"
                   "    var n: int = 1;\n"
                   "    var c: Color = n as Color;\n"
                   "    return c as int;\n"
                   "}\n") == 1,
          "a runtime integer naming an enumerator converts");
    bool signaled = false;
    runCode("enum Color { RED, GREEN, BLUE = 5 }\n"
            "func main() -> int {\n"
            "    var n: int = 7;\n"
            "    var c: Color = n as Color;\n"
            "    return 0;\n"
            "}\n",
            signaled);
    check(signaled, "an undeclared enumerator value traps");
    // Negative enumerators pin the cell canonical: every enum cell is a
    // sign-extended i32, so -1 survives stores, loads and the way back.
    check(runQuiet("enum Temp { NEG = 0 - 1, ZERO, POS = 5 }\n"
                   "func main() -> int {\n"
                   "    var t: Temp = Temp.NEG as Temp;\n"
                   "    return (t as int) + 1;\n"
                   "}\n") == 0,
          "a negative enumerator round-trips");
    check(runQuiet("enum Temp { NEG = 0 - 1, ZERO, POS = 5 }\n"
                   "func main() -> int {\n"
                   "    var n: int = 0 - 1;\n"
                   "    var t: Temp = n as Temp;\n"
                   "    return (t as int) + 1;\n"
                   "}\n") == 0,
          "a runtime -1 converts and reads back");
    check(runQuiet("enum Color { RED, GREEN, BLUE }\n"
                   "func main() -> int {\n"
                   "    var c: Color = Color.GREEN;\n"
                   "    var w: u64 = c as u64;\n"
                   "    if ((w as int) != 1) { return 1; }\n"
                   "    var n: u64 = 2;\n"
                   "    var d: Color = n as Color;\n"
                   "    if ((d as int) != 2) { return 2; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "enums cast to and from wide integers");
}

void testGenericFn() {
    std::printf("generic functions monomorphize\n");
    check(runQuiet("func id<T>(x: T) -> T { return x; }\n"
                   "func main() -> int {\n"
                   "    var a: int = id::<int>(42);\n"
                   "    if (a != 42) { return 1; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "one spec runs");
    check(runQuiet("func id<T>(x: T) -> T { return x; }\n"
                   "func main() -> int {\n"
                   "    var a: int = id::<int>(1);\n"
                   "    var b: u64 = id::<u64>(2);\n"
                   "    if ((a as u64) + b != 3) { return 1; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "two specs coexist");
    check(runQuiet("struct Box<T> { var v: T; var tag: int; }\n"
                   "func main() -> int {\n"
                   "    var b: Box<int> = new Box<int> { v: 40, tag: 1 };\n"
                   "    if (b.v + b.tag != 41) { return 1; }\n"
                   "    if (sizeof(Box<int>) != 8) { return 2; }\n"
                   "    var p: *Box<int> = alloc(sizeof(Box<int>), 8) as *Box<int>;\n"
                   "    (*p).v = 7;\n"
                   "    if (((*p).v as int) != 7) { return 3; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "struct specs construct, size and point");
    check(runQuiet("func id<T>(x: T) -> T { return x; }\n"
                   "func wrap<T>(x: T) -> T { return id::<T>(x); }\n"
                   "func main() -> int {\n"
                   "    if (wrap::<int>(1) != 1) { return 1; }\n"
                   "    if (wrap::<u64>(2) != 2) { return 2; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "nested specs keep their own args");
}

void testWideRun() {
    std::printf("wide literals run as u16 data\n");
    check(runQuiet("func main() -> int {\n"
                   "    var s: [u16; 3] = u\"AB\";\n"
                   "    if (s[0] != 65) { return 1; }\n"
                   "    if (s[1] != 66) { return 2; }\n"
                   "    if (s[2] != 0) { return 3; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "a wide literal fills its array");
    check(runQuiet("func take(p: *u16) -> int { return p[0] as int; }\n"
                   "func main() -> int {\n"
                   "    if (take(u\"Y\") != 89) { return 1; }\n"
                   "    var c: u16 = u'Z';\n"
                   "    if ((c as int) != 90) { return 2; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "wide pointers and characters read back");
    check(runQuiet("func main() -> int {\n"
                   "    if (u\"X\"[0] != 88) { return 1; }\n"
                   "    var q: *u16 = &u\"QR\"[0];\n"
                   "    if (q[1] != 82) { return 2; }\n"
                   "    if (q[2] != 0) { return 3; }\n"
                   "    var cs: [u16; 3] = u\"AB\" as [u16; 3];\n"
                   "    if (cs[1] != 66) { return 4; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "wide indexing, addresses and casts");
    check(runQuiet("var wg: [u16; 3] = u\"GH\";\n"
                   "var wp: *u16 = u\"IJ\";\n"
                   "func main() -> int {\n"
                   "    if (wg[1] != 72) { return 1; }\n"
                   "    if (wg[2] != 0) { return 2; }\n"
                   "    if (wp[0] != 73) { return 3; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "wide globals fold");
}

void testDeferRun() {
    std::printf("defers run at scope exit\n");
    check(runQuiet("var out: int = 0;\n"
                   "func digit(d: int) { out = out * 10 + d; }\n"
                   "func main() -> int {\n"
                   "    defer { digit(1); }\n"
                   "    defer { digit(2); }\n"
                   "    if (out != 0) { return 1; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "defers run LIFO at block exit");
    check(runQuiet("var out: int = 0;\n"
                   "func digit(d: int) { out = out * 10 + d; }\n"
                   "func early(x: int) -> int {\n"
                   "    defer { digit(1); }\n"
                   "    if (x > 0) { return 7; }\n"
                   "    return 0;\n"
                   "}\n"
                   "func main() -> int {\n"
                   "    if (early(1) != 7) { return 1; }\n"
                   "    if (out != 1) { return 2; }\n"
                   "    if (early(0) != 0) { return 3; }\n"
                   "    if (out != 11) { return 4; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "defers run on early return after the value");
    check(runQuiet("var out: int = 0;\n"
                   "func digit(d: int) { out = out * 10 + d; }\n"
                   "func main() -> int {\n"
                   "    for (var i: int = 0; i < 3; i = i + 1) {\n"
                   "        defer { digit(1); }\n"
                   "        if (i == 1) { continue; }\n"
                   "    }\n"
                   "    if (out != 111) { return 1; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "defers run per iteration and on continue");
}

void testClosureVoidRun() {
    std::printf("closures round-trip through *void at runtime\n");
    check(runQuiet("func main() -> int {\n"
                   "    var c: func(int) -> int = func(x: int) -> int { return x * 2; };\n"
                   "    var v: *void = c;\n"
                   "    var f: func(int) -> int = v as func(int) -> int;\n"
                   "    if (f(21) != 42) { return 1; }\n"
                   "    var w: *void = nilPtr;\n"
                   "    w = c;\n"
                   "    var g: func(int) -> int = w as func(int) -> int;\n"
                   "    if (g(21) != 42) { return 2; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "proven carriers round-trip and run");
}

void testBitNot() {
    std::printf("bitwise not runs\n");
    check(runQuiet("func main() -> int {\n"
                   "    var e: int = 0;\n"
                   "    if (~e != 0 - 1) { return 1; }\n"
                   "    var b: u8 = 7;\n"
                   "    if ((~b as int) != 248) { return 2; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "bitwise not inverts at the operand width");
    check(runQuiet("func main() -> int { var b: byte = 6; return (b << 2) as int; }\n") == 24,
          "a byte shifts");
    check(runQuiet("func main() -> int { var c: char = 'a'; return ((c as u8) << 2) as int; }\n") == 132,
          "a char shifts after an explicit as");
}

void testTernary() {
    std::printf("ternaries run\n");
    check(runQuiet("func main() -> int {\n"
                   "    var a: int = true ? 3 : 4;\n"
                   "    var b: int = false ? 3 : 4;\n"
                   "    return a * 10 + b;\n"
                   "}\n") == 34,
          "each arm answers its own value");
    check(runQuiet("func main() -> int {\n"
                   "    var z: int = 0;\n"
                   "    var a: int = true ? 5 : 1 / z;\n"
                   "    var b: int = false ? 1 / z : 6;\n"
                   "    return a + b;\n"
                   "}\n") == 11,
          "an untaken arm never evaluates");
    // A ternary answers inside a larger expression (binary operands, call
    // arguments): the scalar dispatcher must reach it, not only the
    // statement-level path.
    check(runQuiet("func id(x: int) -> int { return x; }\n"
                   "func main() -> int {\n"
                   "    var v: int = 7;\n"
                   "    var a: int = v + (v > 0 ? 1 : 0);\n"
                   "    return a + id(v > 0 ? 10 : 20);\n"
                   "}\n") == 18,
          "a ternary answers in scalar position");
    // A ternary as a statement runs one void arm for effect: the
    // statement path must reach the ternary, not only value positions.
    check(runQuiet("func setFlag(p: *bool, v: bool) -> void { *p = v; return; }\n"
                   "func main() -> int {\n"
                   "    var flag: bool = false;\n"
                   "    true ? setFlag(&flag, true) : setFlag(&flag, false);\n"
                   "    if (flag) { return 1; }\n"
                   "    return 0;\n"
                   "}\n") == 1,
          "a statement ternary runs the taken arm");
    check(runQuiet("func setFlag(p: *bool, v: bool) -> void { *p = v; return; }\n"
                   "func main() -> int {\n"
                   "    var flag: bool = true;\n"
                   "    false ? setFlag(&flag, true) : setFlag(&flag, false);\n"
                   "    if (flag) { return 1; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "a statement ternary skips the untaken arm");
    // A non-void ternary as a statement lowers and discards the answer.
    check(runQuiet("func main() -> int {\n"
                   "    true ? 1 : 2;\n"
                   "    return 7;\n"
                   "}\n") == 7,
          "a discarded ternary answer still lowers");
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

void testFnAddrGlobal() {
    std::printf("function addresses fold into globals\n");
    check(runQuiet("func add(a: int, b: int) -> int { return a + b; }\n"
                   "var g: *void = &add as *void;\n"
                   "func main() -> int {\n"
                   "    if (g == nilPtr) { return 1; }\n"
                   "    if (g != (&add as *void)) { return 2; }\n"
                   "    return 0;\n"
                   "}\n") == 0,
          "a global holds a function address");
}

void testFnPtrCompare() {
    std::printf("function pointer comparison runs\n");
    check(runQuiet("func main() -> int {\n"
                   "    var f: *func(int) -> int = nilPtr;\n"
                   "    if (f == nilPtr) { return 1; }\n"
                   "    return 0;\n"
                   "}\n") == 1,
          "a nil function pointer equals nilPtr");
    check(runQuiet("func add(a: int, b: int) -> int { return a + b; }\n"
                   "func sub(a: int, b: int) -> int { return a - b; }\n"
                   "func main() -> int {\n"
                   "    var f: *func(int, int) -> int = &add;\n"
                   "    var g: *func(int, int) -> int = &sub;\n"
                   "    var n: *func(int, int) -> int = nilPtr;\n"
                   "    if (n != nilPtr) { return 1; }\n"
                   "    if (f == nilPtr) { return 2; }\n"
                   "    if (f != f) { return 3; }\n"
                   "    if (f == g) { return 4; }\n"
                   "    if (f != g) { return 5; }\n"
                   "    return 0;\n"
                   "}\n") == 5,
          "non-nil function pointers compare by address");
}

// A closure literal returned from a function keeps the literal's own
// return type while its body checks: the inner return answers int even
// though the enclosing function answers a closure.
void testClosure() {
    std::printf("closures run\n");
    check(runQuiet("func makeAdder(base: int) -> func(int) -> int {\n"
                   "    return func(x: int) -> int { return x + base; };\n"
                   "}\n"
                   "func main() -> int {\n"
                   "    var add10: func(int) -> int = makeAdder(10);\n"
                   "    return add10(5);\n"
                   "}\n") == 15,
          "a returned closure captures its environment");
}

void testClosureElement() {
    std::printf("closures in aggregates run\n");
    bool sig = false;
    check(runCode("func main() -> int {\n"
                  "    var base: int = 10;\n"
                  "    var add: func(int) -> int = func(v: int) -> int { return v + base; };\n"
                  "    var cell: [func(int) -> int; 1] = new [func(int) -> int; 1] { add };\n"
                  "    return cell[0](5);\n"
                  "}\n",
                  sig) == 15 && !sig,
          "a closure called through an array element");
    check(runCode("struct Holder {\n"
                  "    var base: int;\n"
                  "    var add: func(int) -> int;\n"
                  "}\n"
                  "func main() -> int {\n"
                  "    var b: int = 10;\n"
                  "    var mk: func(int) -> int = func(v: int) -> int { return v + b; };\n"
                  "    var h: Holder = new Holder { base: 0, add: mk };\n"
                  "    return h.add(5);\n"
                  "}\n",
                  sig) == 15 && !sig,
          "a closure called through a struct field");
}

// Two concretes through one interface prove dynamic dispatch: the call
// answers per the value's own method, not the static type.
void testInterface() {
    std::printf("interface dispatch runs\n");
    bool sig = false;
    check(runCode("interface Shape {\n"
                  "    func area() -> int;\n"
                  "}\n"
                  "struct Sq {\n"
                  "    var s: int;\n"
                  "    func area() -> int { return (*self).s; }\n"
                  "}\n"
                  "struct Rect {\n"
                  "    var w: int;\n"
                  "    var h: int;\n"
                  "    func area() -> int { return (*self).w * (*self).h; }\n"
                  "}\n"
                  "func get(s: Shape) -> int { return s.area(); }\n"
                  "func main() -> int {\n"
                  "    var a: Sq = new Sq { s: 9 };\n"
                  "    var b: Rect = new Rect { w: 3, h: 4 };\n"
                  "    return get(a) + get(b);\n"
                  "}\n",
                  sig) == 21 && !sig,
          "two concretes dispatch through one interface");
}

void testInterfaceClass() {
    std::printf("interface class dispatch runs\n");
    bool sig = false;
    check(runCode("interface Shape {\n"
                  "    func area() -> int;\n"
                  "}\n"
                  "struct Sq {\n"
                  "    var s: int;\n"
                  "    func area() -> int { return (*self).s; }\n"
                  "}\n"
                  "class Circle {\n"
                  "    var r: int;\n"
                  "    func init(r: int) -> void { (*self).r = r; }\n"
                  "    func area() -> int { return (*self).r * 2; }\n"
                  "}\n"
                  "func get(s: Shape) -> int { return s.area(); }\n"
                  "func main() -> int {\n"
                  "    var a: Sq = new Sq { s: 9 };\n"
                  "    var c: *Circle = new Circle(5);\n"
                  "    return get(a) + get(c);\n"
                  "}\n",
                  sig) == 19 && !sig,
          "a struct and a class dispatch through one interface");
}

void testInterfaceField() {
    std::printf("interface fields dispatch\n");
    bool sig = false;
    check(runCode("interface Shape {\n"
                  "    func area() -> int;\n"
                  "}\n"
                  "struct Sq {\n"
                  "    var s: int;\n"
                  "    func area() -> int { return (*self).s; }\n"
                  "}\n"
                  "struct Holder {\n"
                  "    var id: int;\n"
                  "    var shape: Shape;\n"
                  "    var tail: int;\n"
                  "}\n"
                  "func main() -> int {\n"
                  "    var a: Sq = new Sq { s: 9 };\n"
                  "    var h: Holder = new Holder { id: 1, shape: a, tail: 2 };\n"
                  "    return h.shape.area() + h.id + h.tail;\n"
                  "}\n",
                  sig) == 12 && !sig,
          "an interface field dispatches at its offset");
}

void testInterfaceDynIndex() {
    std::printf("interface dynamic index dispatches\n");
    bool sig = false;
    check(runCode("interface Shape {\n"
                  "    func area() -> int;\n"
                  "}\n"
                  "struct Sq {\n"
                  "    var s: int;\n"
                  "    func area() -> int { return (*self).s; }\n"
                  "}\n"
                  "struct Rect {\n"
                  "    var w: int;\n"
                  "    var h: int;\n"
                  "    func area() -> int { return (*self).w * (*self).h; }\n"
                  "}\n"
                  "func pick(arr: [Shape; 2], k: int) -> int {\n"
                  "    return arr[k].area();\n"
                  "}\n"
                  "func main() -> int {\n"
                  "    var a: Sq = new Sq { s: 9 };\n"
                  "    var b: Rect = new Rect { w: 3, h: 4 };\n"
                  "    var arr: [Shape; 2] = new [Shape; 2] { a, b };\n"
                  "    return pick(arr, 0) + pick(arr, 1);\n"
                  "}\n",
                  sig) == 21 && !sig,
          "a computed element dispatches through its own vtable");
}

void testInterfaceDowncast() {
    std::printf("interface downcasts read back\n");
    bool sig = false;
    check(runCode("interface Shape {\n"
                  "    func area() -> int;\n"
                  "}\n"
                  "struct Sq {\n"
                  "    var s: int;\n"
                  "    func area() -> int { return (*self).s; }\n"
                  "}\n"
                  "struct Rect {\n"
                  "    var w: int;\n"
                  "    var h: int;\n"
                  "    func area() -> int { return (*self).w * (*self).h; }\n"
                  "}\n"
                  "func main() -> int {\n"
                  "    var a: Sq = new Sq { s: 9 };\n"
                  "    var b: Rect = new Rect { w: 3, h: 4 };\n"
                  "    var sa: Shape = a;\n"
                  "    var sb: Shape = b;\n"
                  "    var back: Sq = sa as Sq;\n"
                  "    return back.s + sb.area();\n"
                  "}\n",
                  sig) == 21 && !sig,
          "a downcast reads back the concrete value");
}

void testStringAdd() {
    std::printf("string concatenation runs\n");
    bool sig = false;
    check(runCode("func main() -> int {\n"
                  "    var s: string = \"he\";\n"
                  "    var t: string = \"llo\";\n"
                  "    var u: string = s + t;\n"
                  "    return u.len as int;\n"
                  "}\n",
                  sig) == 5 && !sig,
          "two strings concatenate to one");
}

// The same module emitted twice is byte-identical, and carries no build path.
void testMultiWordReturn() {
    std::printf("multi-word returns\n");
    bool sig = false;
    // A two-word array comes home in rax/rdx (IR.md §6): both words
    // survive the call boundary and read back in order.
    check(runCode("func make_row() -> [u64; 2] {\n"
                  "    var r: [u64; 2] = new [u64; 2] {11, 22};\n"
                  "    return r;\n"
                  "}\n"
                  "func main() -> int {\n"
                  "    var made: [u64; 2] = make_row();\n"
                  "    return (made[0] + made[1]) as int;\n"
                  "}\n",
                  sig) == 33 &&
              !sig,
          "a two-word array return reads back whole");
    // A string is two integer words (address, length): the same row.
    check(runCode("func greet() -> string { return \"hi\"; }\n"
                  "func main() -> int { return greet().len as int; }\n", sig) == 2 &&
              !sig,
          "a string return carries address and length");
    // Three words exceed the register row: they ride the hidden
    // pointer instead of splitting.
    check(runCode("func triple() -> [u64; 3] {\n"
                  "    var t: [u64; 3] = new [u64; 3] {1, 2, 3};\n"
                  "    return t;\n"
                  "}\n"
                  "func main() -> int {\n"
                  "    var m: [u64; 3] = triple();\n"
                  "    return (m[0] + m[1] + m[2]) as int;\n"
                  "}\n",
                  sig) == 6 && !sig,
          "a three-word return comes home whole");
    // A one-word aggregate still answers its word's type, not the
    // aggregate's: the scalar path cannot name a two-byte array's width.
    check(runCode("func make_row() -> [u8; 2] {\n"
                  "    var r: [u8; 2] = new [u8; 2] {11, 22};\n"
                  "    return r;\n"
                  "}\n"
                  "func main() -> int {\n"
                  "    var made: [u8; 2] = make_row();\n"
                  "    return (made[0] as int) + (made[1] as int);\n"
                  "}\n",
                  sig) == 33 &&
              !sig,
          "a one-word array return reads back whole");
    // `sizeof` is a compile-time byte count, including through nesting.
    check(runQuiet("func main() -> int {\n"
                   "    return sizeof([[u8; 2]; 3]) as int;\n"
                   "}\n") == 6,
          "sizeof answers a nested array's bytes");
}

void testBigReturn() {
    std::printf("wide returns ride a hidden pointer\n");
    bool sig = false;
    check(runCode("struct Big {\n"
                  "    var a: int;\n"
                  "    var b: int;\n"
                  "    var c: int;\n"
                  "    var d: int;\n"
                  "    var e: int;\n"
                  "}\n"
                  "func make() -> Big {\n"
                  "    return new Big { a: 1, b: 2, c: 3, d: 4, e: 5 };\n"
                  "}\n"
                  "func main() -> int {\n"
                  "    var m: Big = make();\n"
                  "    return m.a + m.b + m.c + m.d + m.e;\n"
                  "}\n",
                  sig) == 15 && !sig,
          "a five-field answer comes home whole");
}

void testSliceCopy() {
    // A slice copies into a same-element array: the length is checked at
    // the copy, so a short slice traps instead of leaving garbage.
    std::printf("a slice copies into an array\n");
    bool sig = false;
    check(runCode("type Row = [u8; 3];\n"
                  "func main() -> int {\n"
                  "    var a: [u8; 5] = new [u8; 5] { 10, 42, 43, 44, 50 };\n"
                  "    var s: Row = a[1:4];\n"
                  "    if (s[0] != 42 || s[2] != 44) { return 1; }\n"
                  "    if (a[1] != 42 || a[3] != 44) { return 2; }\n"
                  "    s[1] = 7;\n"
                  "    if (s[1] != 7 || a[2] != 43) { return 3; }\n"
                  "    return 0;\n"
                  "}\n",
                  sig) == 0 && !sig,
          "a slice copies elementwise and detaches");
    check(runCode("func main() -> int {\n"
                  "    var a: [u8; 5] = new [u8; 5] { 10, 42, 43, 44, 50 };\n"
                  "    var s: [u8; 3] = a[0:2];\n"
                  "    return s[0] as int;\n"
                  "}\n",
                  sig) == -1 && sig,
          "a short slice traps instead of truncating");
    check(runCode("func main() -> int {\n"
                  "    var a: [u8; 5] = new [u8; 5] { 10, 42, 43, 44, 50 };\n"
                  "    var s: [u8; 3] = a[1:4];\n"
                  "    s = a[2:5];\n"
                  "    return s[0] as int;\n"
                  "}\n",
                  sig) == 43 && !sig,
          "a slice reassigns an array");
    // A slice through a pointer dereference addresses off the loaded
    // address: `(*p)[0:2]` names the elements past the pointer's value,
    // with the same bounds checks the slot route owns (BUG-279; the
    // dynamic-index twin was BUG-278).
    check(runQuiet("func at(p: *[int; 3]) -> int {\n"
                   "    var s: []int = (*p)[0:2];\n"
                   "    return s[1];\n"
                   "}\n"
                   "func main() -> int {\n"
                   "    var a: [int; 3] = new [int; 3] { 4, 5, 6 };\n"
                   "    return at(&a);\n"
                   "}\n") == 5,
          "a folded slice through a pointer answers the element");
    check(runQuiet("struct S { var items: [int; 3]; }\n"
                   "func get(q: *S, i: int) -> int { var s: []int = (*q).items[0:3]; return s[i]; }\n"
                   "func main() -> int {\n"
                   "    var s: S = new S { items: new [int; 3] { 4, 5, 6 } };\n"
                   "    return get(&s, 2);\n"
                   "}\n") == 6,
          "a folded slice through a pointer field answers the element");
    check(runQuiet("func at(p: *[int; 4], lo: int, hi: int) -> int {\n"
                   "    var s: []int = (*p)[lo:hi];\n"
                   "    return s[1];\n"
                   "}\n"
                   "func main() -> int {\n"
                   "    var a: [int; 4] = new [int; 4] { 4, 5, 6, 7 };\n"
                   "    return at(&a, 1, 3);\n"
                   "}\n") == 6,
          "a computed slice through a pointer answers the element");
    sig = false;
    check(runCode("func at(p: *[int; 3], l: int, h: int) -> int {\n"
                  "    var s: []int = (*p)[l:h];\n"
                  "    return s[0];\n"
                  "}\n"
                  "func main() -> int {\n"
                  "    var a: [int; 3] = new [int; 3] { 4, 5, 6 };\n"
                  "    return at(&a, 0, 9);\n"
                  "}\n",
                  sig) == -1 && sig,
          "a slice past the end through a pointer traps");
    // A string global indexes like a local: the global's address is
    // materialized once and the element reads through it.
    check(runCode("var g: string = \"abc\";\n"
                  "func main() -> int {\n"
                  "    return g[1] as int;\n"
                  "}\n",
                  sig) == 98 && !sig,
          "a string global indexes");
}

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
    // The iteration protocol: iter() answers a value once, next() takes a
    // pointer to it and answers each element or nilPtr (FEATURES 8.2).
    const char* range =
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
    check(runQuiet(std::string(range) +
                 "func main() -> int {\n"
                 "    var r: Rng = new Rng { lo: 0, hi: 3 };\n"
                 "    var s: int = 0;\n"
                 "    for (var x in r) { s = s + x; }\n"
                 "    return s;\n"
                 "}\n") == 3,
          "for-in over iter/next sums every step");
    check(runQuiet(std::string(range) +
                 "func main() -> int {\n"
                 "    var r: Rng = new Rng { lo: 1, hi: 6 };\n"
                 "    var s: int = 0;\n"
                 "    for (var x in r) {\n"
                 "        if (x == 3) { break; }\n"
                 "        s = s + x;\n"
                 "    }\n"
                 "    return s;\n"
                 "}\n") == 3,
          "break leaves a protocol loop early");
    check(runQuiet(std::string(range) +
                 "func main() -> int {\n"
                 "    var r: Rng = new Rng { lo: 0, hi: 4 };\n"
                 "    var s: int = 0;\n"
                 "    for (x in r) { s = s + x; }\n"
                 "    return s;\n"
                 "}\n") == 6,
          "a protocol loop binds immutably without var");
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
    // f32 remainder truncates the quotient: 5.5 % 2.0 = 1.5 -> 1.
    check(runQuiet("func main() -> int {\n"
                 "    var a: f32 = 5.5;\n"
                 "    var b: f32 = 2.0;\n"
                 "    return (a % b) as int;\n"
                 "}\n") == 1,
          "f32 remainder reaches its result");
    // f64 remainder at wider range: 7.5 % 2.0 = 1.5 -> 1.
    check(runQuiet("func main() -> int {\n"
                 "    var a: f64 = 7.5;\n"
                 "    var b: f64 = 2.0;\n"
                 "    return (a % b) as int;\n"
                 "}\n") == 1,
          "f64 remainder reaches its result");
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
    // A float field stores its own width: a double written into a struct
    // slot and read back must answer its value, not the low half of its
    // bit pattern (movss where movsd was needed).
    check(runQuiet("struct P { var a: f64; }\n"
                 "func main() -> int {\n"
                 "    var p: P = new P{ a: 1.5 };\n"
                 "    return p.a as int;\n"
                 "}\n") == 1,
          "an f64 field keeps every bit of its pattern");
    check(runQuiet("struct P { var a: i32; var b: f64; }\n"
                 "func main() -> int {\n"
                 "    var p: P = new P{ a: 3, b: 1.5 };\n"
                 "    return p.a + (p.b as int);\n"
                 "}\n") == 4,
          "an f64 field beside an int field keeps both");
    check(runQuiet("struct P { var a: f32; }\n"
                 "func main() -> int {\n"
                 "    var p: P = new P{ a: 2.5 };\n"
                 "    return (p.a + 0.5) as int;\n"
                 "}\n") == 3,
          "an f32 field reads back at its own width");
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
    // A folded float expression global matches the local path exactly:
    // 1.0 + 2.0 is 3.0 in f64, and `(5 / 2) as f64` stays integer
    // division (2) before the written conversion, never 2.5.
    check(runQuiet("var g: f64 = 1.0 + 2.0;\n"
                 "func main() -> int { return g as int; }\n") == 3,
          "a float addition global folds");
    check(runQuiet("var g: f64 = (5 / 2) as f64;\n"
                 "func main() -> int { return g as int; }\n") == 2,
          "an integer division global converts, not 2.5");
    // Exit codes are 8 bits: -3 leaves as 253, 16777217 (= 0x1000001)
    // leaves as 1. Both are compared in range instead.
    check(runQuiet("var g: f64 = (-7 / 2) as f64;\n"
                 "func main() -> int {\n"
                 "    if ((g as int) != -3) { return 1; }\n"
                 "    return 0;\n"
                 "}\n") == 0,
          "a signed division global converts");
    // `(0.1 as float) as f64` rounds once to f32 in the image, exactly
    // as the local path rounds it: the two must compare equal, and
    // unequal to the unrounded double.
    check(runQuiet("var g: f64 = (0.1 as float) as f64;\n"
                 "func main() -> int {\n"
                 "    var l: f64 = (0.1 as float) as f64;\n"
                 "    if (g != l) { return 1; }\n"
                 "    if (g == 0.1) { return 2; }\n"
                 "    return 0;\n"
                 "}\n") == 0,
          "an as-float global rounds like the local path");
    // 16777217 needs 25 bits: exact in f64, so the mixed add keeps it.
    check(runQuiet("var g: f64 = 16777217.0 + (0.0 as f64);\n"
                 "func main() -> int {\n"
                 "    if ((g as int) != 16777217) { return 1; }\n"
                 "    return 0;\n"
                 "}\n") == 0,
          "a mixed-width addition global keeps its value");
    // Integer leaves in a float array convert at the element width:
    // the image holds 1.0 and 2.0, never 1 and 2.
    check(runQuiet("var g: [f64; 2] = new [f64; 2] { 1, 2 };\n"
                 "func main() -> int {\n"
                 "    if (g[0] != 1.0 || g[1] != 2.0) { return 1; }\n"
                 "    return 0;\n"
                 "}\n") == 0,
          "a float array global converts its leaves");
}

// A program that exits through its own asm block: a local placeholder
// feeds the exit code, a global placeholder feeds a buffer address.
void testRegisterEscape() {
    std::printf("register-named functions escape\n");
    bool sig = false;
    // `cs` is a segment register: GAS rejects every textual reference
    // to the bare spelling, so the emitter renames the symbol at every
    // site that names it.
    check(runCode("func cs() -> int { return 8; }\n"
                  "func main() -> int { return cs(); }\n",
                  sig) == 8 && !sig,
          "a function named like a register still calls");
    // `add` is only a mnemonic: GAS accepts it bare, so the emitter
    // keeps the spelling instead of renaming every function.
    check(runCode("func add(a: int, b: int) -> int { return a + b; }\n"
                  "func main() -> int { return add(20, 22); }\n",
                  sig) == 42 && !sig,
          "a function named like a mnemonic keeps its spelling");
    Built text = build("func cs() -> int { return 8; }\n"
                       "func add(a: int, b: int) -> int { return a + b; }\n"
                       "func main() -> int { return cs() + add(1, 1); }\n");
    check(text.ok, "the escape program builds");
    check(text.asmText.find("call cs_") != std::string::npos,
          "a call names the escaped symbol");
    check(text.asmText.find("cs_:") != std::string::npos,
          "the definition names the escaped symbol");
    check(text.asmText.find("call add") != std::string::npos,
          "a mnemonic call keeps its spelling");
}

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
    // A block is treated exactly like a call (FEATURES §4.4): values live
    // across it survive whatever caller-saved registers it clobbers.
    check(runCode("func main() -> int {\n"
                  "    var a: int = 10;\n"
                  "    var b: int = 20;\n"
                  "    asm { \"mov eax, 999\" \"mov ecx, 888\" \"mov edx, 777\" }\n"
                  "    return a + b;\n"
                  "}\n", sig) == 30 && !sig,
          "values live across a clobbering block survive");
}

}  // namespace

int main() {
    std::printf("codegen_test — a Cider program runs\n\n");
    testExit();
    testStackArgs();
    testCalls();
    testControl();
    testGlobals();
    testStruct();
    testArray();
    testPointerHeap();
    testDelete();
    testHeapCapText();
    testLiteralNul();
    testExternPutchar();
    testBreakContinue();
    testForLoop();
    testForIn();
    testArith32();
    testIndirectCall();
    testFnAddrGlobal();
    testFnPtrCompare();
    testClosure();
    testClosureElement();
    testInterface();
    testInterfaceClass();
    testInterfaceField();
    testInterfaceDynIndex();
    testInterfaceDowncast();
    testStringAdd();
    testShortCircuit();
    testBoolCompare();
    testNotInCondition();
    testCharCompare();
    testStringCompare();
    testInterfaceNil();
    testMatch();
    testGuard();
    testRange();
    testEnumMatch();
    testPointerArith();
    testEnumValues();
    testEnumCast();
    testGenericFn();
    testWideRun();
    testDeferRun();
    testClosureVoidRun();
    testBitNot();
    testTernary();
    testInterp();
    testFloat();
    testRegisterEscape();
    testAsm();
    testTrap();
    testPanic();
    testElementAddr();
    testComputedIndex();
    testPointerField();
    testMethodGlobalStore();
    testClassBox();
    testRepeatZero();
    testIncrDecr();
    testMultiWordReturn();
    testBigReturn();
    testSliceCopy();
    testRepro();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
