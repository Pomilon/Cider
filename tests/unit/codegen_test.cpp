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
    out.module = lowerer.lowerModule("main", *program);
    if (out.module == nullptr) {
        out.stage = "lower";
        return out;
    }
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
    // Assembled with the shell, which is fine: assembling cannot trap. Run
    // directly afterwards, because a signal death does not survive system() —
    // sh dies by the same signal and the test would read 127 either way.
    std::string command = "cc -o " + binPath + " " + asmPath + " 2>/dev/null";
    if (system(command.c_str()) != 0) {
        unlink(asmPath);
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

}  // namespace

int main() {
    std::printf("codegen_test — a Cider program runs\n\n");
    testExit();
    testCalls();
    testControl();
    testGlobals();
    testStruct();
    testArray();
    testTrap();
    testRepro();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
