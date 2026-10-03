// runtime_test.cpp — the program can observe more than its exit code.
//
// print() writes bytes to stdout; alloc/realloc/free manage the heap with
// canaries the process dies on. Tests 1-6 run Cider programs end to end and
// read their stdout. Tests 7-9 drive the C runtime directly: corruption must
// abort, alignment must hold, content must survive realloc.

#include <cstdio>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "check/checker.h"
#include "driver/driver.h"
#include "codegen/codegen.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
#include "opt/opt.h"
#include "parse/parser.h"
#include "types/context.h"

#ifndef CIDER_RT_ARCHIVE
#define CIDER_RT_ARCHIVE ""
#endif

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
};

Built build(const std::string& text) {
    Built out;
    auto types = std::make_unique<cider::TypeContext>();
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
    auto resolver = std::make_unique<cider::resolve::Resolver>(*types);
    if (!resolver->resolveProgram(*program, "main")) {
        out.stage = "resolve";
        return out;
    }
    auto checker = std::make_unique<cider::check::Checker>(*types, *resolver);
    cider::lower::Lowerer lowerer(*types, *resolver, *checker);
    std::unique_ptr<cider::ir::Module> module = lowerer.lowerModule("main", *program);
    if (module == nullptr) {
        out.stage = "lower";
        return out;
    }
    cider::opt::OptResult result = cider::opt::run(*module);
    if (!result.ok) {
        out.stage = "opt:" + result.error;
        return out;
    }
    cider::codegen::EmitResult emitted = cider::codegen::emitNative(*module);
    if (!emitted.ok) {
        out.stage = "codegen:" + emitted.error;
        return out;
    }
    out.asmText = emitted.text;
    out.ok = true;
    return out;
}

std::string writeTemp(const std::string& text, const std::string& suffix) {
    std::string pattern = "/tmp/cider_rt_XXXXXX" + suffix;
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back(0);
    int fd = mkstemps(path.data(), static_cast<int>(suffix.size()));
    FILE* file = fdopen(fd, "w");
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
    return path.data();
}

struct Ran {
    bool built = false;
    bool signaled = false;
    int signal = 0;
    int code = -1;
    std::string captured;
};

// Links with the runtime archive and runs with stdout captured.
Ran runCapture(const std::string& cider) {
    Ran out;
    Built built = build(cider);
    if (!built.ok) {
        return out;
    }
    std::string asmPath = writeTemp(built.asmText, ".s");
    std::string binPath = asmPath + ".bin";
    std::string command =
        "cc -o " + binPath + " " + asmPath + " " + CIDER_RT_ARCHIVE + " 2>/dev/null";
    if (system(command.c_str()) != 0) {
        unlink(asmPath.c_str());
        return out;
    }
    out.built = true;
    int fds[2];
    pipe(fds);
    pid_t child = fork();
    if (child == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl(binPath.c_str(), binPath.c_str(), nullptr);
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t got = 0;
    while ((got = read(fds[0], buf, sizeof(buf))) > 0) {
        out.captured.append(buf, static_cast<size_t>(got));
    }
    close(fds[0]);
    int status = 0;
    waitpid(child, &status, 0);
    unlink(asmPath.c_str());
    unlink(binPath.c_str());
    if (WIFSIGNALED(status)) {
        out.signaled = true;
        out.signal = WTERMSIG(status);
        return out;
    }
    out.code = WEXITSTATUS(status);
    return out;
}

// Compiles and runs a C file against the runtime archive, in a forked child
// so abort() becomes an observable signal instead of a dead test runner.
Ran runC(const std::string& body) {
    Ran out;
    std::string srcPath = writeTemp(body, ".c");
    std::string binPath = srcPath + ".bin";
    std::string command =
        "cc -o " + binPath + " " + srcPath + " " + CIDER_RT_ARCHIVE + " 2>/dev/null";
    if (system(command.c_str()) != 0) {
        unlink(srcPath.c_str());
        return out;
    }
    out.built = true;
    pid_t child = fork();
    if (child == 0) {
        execl(binPath.c_str(), binPath.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    waitpid(child, &status, 0);
    unlink(srcPath.c_str());
    unlink(binPath.c_str());
    if (WIFSIGNALED(status)) {
        out.signaled = true;
        out.signal = WTERMSIG(status);
        return out;
    }
    out.code = WEXITSTATUS(status);
    return out;
}

void testPrintLiteral() {
    std::printf("print a literal\n");
    Ran r = runCapture("func main() -> int { print(\"hello\"); return 0; }\n");
    check(r.built, "builds and links");
    check(!r.signaled && r.code == 0, "exits 0");
    check(r.captured == "hello", "stdout is exactly hello");
}

void testPrintVar() {
    std::printf("print a variable\n");
    Ran r = runCapture(
        "func main() -> int { var s: string = \"hi\"; print(s); return 0; }\n");
    check(r.built, "builds and links");
    check(r.captured == "hi", "stdout is exactly hi");
}

void testPrintEmpty() {
    std::printf("print empty and print twice\n");
    Ran r = runCapture(
        "func main() -> int { print(\"\"); print(\"a\"); print(\"b\"); return 0; }\n");
    check(r.built, "builds and links");
    check(r.captured == "ab", "empty prints nothing, order kept");
}

void testAllocFree() {
    std::printf("alloc and free\n");
    Ran r = runCapture("func main() -> int { free(alloc(16, 8)); return 0; }\n");
    check(r.built, "builds and links");
    check(!r.signaled && r.code == 0, "clean alloc/free exits 0");
}

void testFreeNil() {
    std::printf("free of nil is a no-op\n");
    Ran r = runCapture("func main() -> int { free(nilPtr); return 0; }\n");
    check(r.built, "builds and links");
    check(!r.signaled && r.code == 0, "exits 0");
}

void testDriverRun() {
    std::printf("the driver runs a printing program\n");
    // Flush first: the program under test writes through the same terminal,
    // and buffered test output must not interleave with it.
    fflush(stdout);
    std::string src = writeTemp(
        "func main() -> int { print(\"hi\"); return 7; }\n", ".cid");
    std::string out;
    std::string err;
    int code = cider::driver::runDriver(
        {"run", "--rt", CIDER_RT_ARCHIVE, src}, out, err);
    check(code == 7, "exit code passes through" + err);
    unlink(src.c_str());
}

void testCanaryRear() {
    std::printf("rear canary corruption aborts\n");
    Ran r = runC(
        "#include <stdint.h>\n"
        "extern void* cider_alloc(int64_t size, int64_t align);\n"
        "extern void cider_free(void* p);\n"
        "int main() { char* p = (char*)cider_alloc(16, 8); p[16] = 'x'; cider_free(p); return 0; }\n");
    check(r.built, "builds and links");
    check(r.signaled, "corruption aborts instead of returning");
}

void testCanaryFront() {
    std::printf("front canary corruption aborts\n");
    Ran r = runC(
        "#include <stdint.h>\n"
        "extern void* cider_alloc(int64_t size, int64_t align);\n"
        "extern void cider_free(void* p);\n"
        "int main() { char* p = (char*)cider_alloc(16, 8); p[-1] = 'x'; cider_free(p); return 0; }\n");
    check(r.built, "builds and links");
    check(r.signaled, "corruption aborts instead of returning");
}

void testAlignRealloc() {
    std::printf("alignment holds and realloc keeps content\n");
    Ran r = runC(
        "#include <stdint.h>\n"
        "extern void* cider_alloc(int64_t size, int64_t align);\n"
        "extern void* cider_realloc(void* p, int64_t size, int64_t align);\n"
        "extern void cider_free(void* p);\n"
        "int main() {\n"
        "  char* p = (char*)cider_alloc(16, 8);\n"
        "  if (((uintptr_t)p & 15) != 0) return 1;\n"
        "  char* q = (char*)cider_alloc(1, 32);\n"
        "  if (((uintptr_t)q & 31) != 0) return 2;\n"
        "  for (int i = 0; i < 16; ++i) p[i] = (char)(i + 1);\n"
        "  char* r2 = (char*)cider_realloc(p, 64, 8);\n"
        "  for (int i = 0; i < 16; ++i) if (r2[i] != (char)(i + 1)) return 3;\n"
        "  cider_free(q); cider_free(r2); return 0;\n"
        "}\n");
    check(r.built, "builds and links");
    check(!r.signaled && r.code == 0, "aligned, preserved, freed");
}

}  // namespace

int main() {
    std::printf("runtime_test — observation beyond exit codes\n\n");
    if (std::string(CIDER_RT_ARCHIVE).empty()) {
        std::printf("CIDER_RT_ARCHIVE unset: cannot link\n");
        return 1;
    }
    testPrintLiteral();
    testPrintVar();
    testPrintEmpty();
    testAllocFree();
    testFreeNil();
    testDriverRun();
    testCanaryRear();
    testCanaryFront();
    testAlignRealloc();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
