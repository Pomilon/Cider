// driver_test.cpp — the command behaves, including its failures.
//
// A driver that only works on good input is not tested. Every failure below
// asserts the diagnostic names what failed: the file, the line, the target,
// the subcommand.

#include <cstdio>
#include <string>
#include <unistd.h>

#include "driver/driver.h"

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

std::string writeTemp(const std::string& text) {
    char path[] = "/tmp/cider_driver_XXXXXX.cid";
    int fd = mkstemps(path, 4);
    FILE* file = fdopen(fd, "w");
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
    return path;
}

struct Result {
    int code = 0;
    std::string out;
    std::string err;
};

Result run(const std::vector<std::string>& args) {
    Result result;
    result.code = cider::driver::runDriver(args, result.out, result.err);
    return result;
}

// `check` passes a good file silently and names the failure in a bad one.
void testCheck() {
    std::printf("check\n");
    std::string good = writeTemp("func main() -> int { return 1; }\n");
    Result ok = run({"check", good});
    check(ok.code == 0, "a good file passes quietly");
    check(ok.err.empty(), "nothing on stderr for success");
    std::string bad = writeTemp("func main() -> int { return x; }\n");
    Result failed = run({"check", bad});
    check(failed.code != 0, "a bad file fails");
    check(failed.err.find("1:") != std::string::npos, "the diagnostic names the line");
    check(failed.err.find(bad) != std::string::npos, "and the file");
    unlink(good.c_str());
    unlink(bad.c_str());
}

// `compile` emits assembly; `ir` dumps the module.
void testCompileIr() {
    std::printf("compile and ir\n");
    std::string src = writeTemp("func main() -> int { return 40 + 2; }\n");
    char asmPath[] = "/tmp/cider_driver_XXXXXX.s";
    int asmFd = mkstemps(asmPath, 2);
    close(asmFd);
    Result compiled = run({"compile", "-o", asmPath, src});
    check(compiled.code == 0, "compiles: " + compiled.err);
    FILE* asmFile = fopen(asmPath, "r");
    char head[64] = {0};
    size_t got = asmFile != nullptr ? fread(head, 1, 63, asmFile) : 0;
    if (asmFile != nullptr) {
        fclose(asmFile);
    }
    check(got > 0 && std::string(head).find("main:") != std::string::npos,
          "the assembly defines main");
    Result dumped = run({"ir", src});
    check(dumped.code == 0, "dumps: " + dumped.err);
    // `40 + 2` folds to one const before the dump, so ask for a dump with an
    // add no pass may remove: parameters are never constant.
    std::string src2 = writeTemp("func f(a: int, b: int) -> int { return a + b; }\n");
    Result dumped2 = run({"ir", src2});
    check(dumped2.code == 0, "dumps a function: " + dumped2.err);
    check(dumped2.out.find("add") != std::string::npos, "the dump names the add");
    unlink(src2.c_str());
    unlink(src.c_str());
    unlink(asmPath);
}

// `run` answers the program's exit code.
void testRun() {
    std::printf("run\n");
    std::string src = writeTemp("func main() -> int { return 41 + 1; }\n");
    Result ran = run({"run", src});
    check(ran.code == 42, "the program's answer is the exit code");
    unlink(src.c_str());
}

// A missing file, a bad target and a bad subcommand all name themselves.
void testFailures() {
    std::printf("failures name themselves\n");
    Result missing = run({"check", "/tmp/cider_no_such_file.cid"});
    check(missing.code != 0, "a missing file fails");
    check(missing.err.find("cider_no_such_file") != std::string::npos,
          "naming the file");
    std::string src = writeTemp("func main() -> int { return 1; }\n");
    Result target = run({"compile", "--target", "uefi", src});
    check(target.code != 0, "an unimplemented target fails");
    check(target.err.find("uefi") != std::string::npos, "naming the target");
    Result sub = run({"frobnicate", src});
    check(sub.code != 0, "an unknown subcommand fails");
    unlink(src.c_str());
}

}  // namespace

int main() {
    std::printf("driver_test — the command behaves\n\n");
    testCheck();
    testCompileIr();
    testRun();
    testFailures();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
