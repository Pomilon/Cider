// driver_test.cpp — the command behaves, including its failures.
//
// A driver that only works on good input is not tested. Every failure below
// asserts the diagnostic names what failed: the file, the line, the target,
// the subcommand.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/driver.h"

#include "entry_fixture.h"

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

std::string writeTemp(const std::string& text, bool entryFixture = true) {
    const std::string source =
        entryFixture ? cider_test::withMainEntry(text) : text;
    char path[] = "/tmp/cider_driver_XXXXXX.cid";
    int fd = mkstemps(path, 4);
    FILE* file = fdopen(fd, "w");
    fwrite(source.data(), 1, source.size(), file);
    fclose(file);
    return path;
}

// Sibling-module tests write their entry file by hand. The fixture text is
// appended at the source level, exactly as writeTemp does, so the compiler
// never has to synthesize an entry.
void appendMainEntry(const std::string& path) {
    FILE* file = fopen(path.c_str(), "a");
    if (file == nullptr) {
        return;
    }
    const char* fixture = cider_test::mainEntryFixture();
    fwrite(fixture, 1, strlen(fixture), file);
    fclose(file);
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
    // A resolve error raised from inside checking (bare generic in a body)
    // still fails the build and names the rule, rather than vanishing.
    std::string generic =
        writeTemp("struct Box<T> { var v: T; }\n"
                  "func main() -> int { var b: Box = new Box<int> { v: 1 }; return 0; }\n");
    Result genericFailed = run({"check", generic});
    check(genericFailed.code != 0, "a bare generic local fails check");
    check(genericFailed.err.find("takes 1 arguments") != std::string::npos,
          "the diagnostic names the arity");
    unlink(generic.c_str());
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
    check(got > 0 && std::string(head).find("_start:") != std::string::npos,
          "the assembly defines the entry");
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

// `run` answers the program's exit code. The link is sovereign: nasm and
// ld only, no runtime archive, so this test also proves the trampoline and
// the helper emission assemble into a working binary.
void testRun() {
    std::printf("run\n");
    std::string src = writeTemp("func main() -> int { return 41 + 1; }\n");
    Result ran = run({"run", src});
    check(ran.code == 42, "the program's answer is the exit code " + ran.err);
    unlink(src.c_str());
}

// A program spread over sibling modules checks and runs: the driver loads
// each import beside the entry file, checks every module, and links every
// function into one binary. A dotted import names the missing module.
void testImportAlias() {
    std::printf("imported aliases\n");
    char dir[] = "/tmp/cider_impalias_XXXXXX";
    check(mkdtemp(dir) != nullptr, "a scratch directory exists");
    std::string lib = std::string(dir) + "/lib.cid";
    FILE* libFile = fopen(lib.c_str(), "w");
    const char* libText =
        "export struct R { var v: int; }\n"
        "export type A = R;\n";
    fwrite(libText, 1, strlen(libText), libFile);
    fclose(libFile);
    std::string main = std::string(dir) + "/main.cid";
    FILE* mainFile = fopen(main.c_str(), "w");
    const char* mainText =
        "from lib import A;\n"
        "func main() -> int {\n"
        "    var x: A = A { v: 5 };\n"
        "    return x.v;\n"
        "}\n";
    fwrite(mainText, 1, strlen(mainText), mainFile);
    fclose(mainFile);
    appendMainEntry(main);
    Result ok = run({"check", main});
    check(ok.code == 0, "a from-imported alias checks: " + ok.err);
    Result ran = run({"run", main});
    check(ran.code == 5, "a from-imported alias constructs: " + ran.err);
    unlink(main.c_str());
    unlink(lib.c_str());
    rmdir(dir);
}

void testModFnValue() {
    std::printf("module function values\n");
    char dir[] = "/tmp/cider_modfn_XXXXXX";
    check(mkdtemp(dir) != nullptr, "a scratch directory exists");
    std::string dep = std::string(dir) + "/b.cid";
    FILE* depFile = fopen(dep.c_str(), "w");
    const char* depText =
        "export func inc(x: int) -> int { return x + 1; }\n";
    fwrite(depText, 1, strlen(depText), depFile);
    fclose(depFile);
    std::string main = std::string(dir) + "/main.cid";
    FILE* mainFile = fopen(main.c_str(), "w");
    const char* mainText =
        "import b;\n"
        "var gg: func(int) -> int = b.inc;\n"
        "func dbl(x: int) -> int { return x * 2; }\n"
        "var lo: func(int) -> int = dbl;\n"
        "var fg: *func(int) -> int = &dbl;\n"
        "var fh: *func(int) -> int = nilPtr;\n"
        "func main() -> int {\n"
        "    var g: func(int) -> int = b.inc;\n"
        "    if (g(4) != 5) { return 1; }\n"
        "    if ((b.inc)(4) != 5) { return 2; }\n"
        "    if (gg(4) != 5) { return 3; }\n"
        "    if ((*fg)(21) != 42) { return 4; }\n"
        "    if (fh != nilPtr) { return 5; }\n"
        "    if (lo(21) != 42) { return 6; }\n"
        "    return 0;\n"
        "}\n";
    fwrite(mainText, 1, strlen(mainText), mainFile);
    fclose(mainFile);
    appendMainEntry(main);
    Result ok = run({"check", main});
    check(ok.code == 0, "a module function value checks: " + ok.err);
    Result ran = run({"run", main});
    check(ran.code == 0, "a module function value calls: " + ran.err);
    unlink(main.c_str());
    unlink(dep.c_str());
    rmdir(dir);
}

void testImports() {
    std::printf("imports\n");
    char dir[] = "/tmp/cider_imports_XXXXXX";
    check(mkdtemp(dir) != nullptr, "a scratch directory exists");
    std::string main = std::string(dir) + "/main.cid";
    std::string dep = std::string(dir) + "/da.cid";
    FILE* depFile = fopen(dep.c_str(), "w");
    fwrite("export func aval() -> int { return 42; }\n", 1, 41, depFile);
    fclose(depFile);
    FILE* mainFile = fopen(main.c_str(), "w");
    fwrite("import da;\nfunc main() -> int { return da.aval(); }\n", 1, 51,
           mainFile);
    fclose(mainFile);
    appendMainEntry(main);
    Result ok = run({"check", main});
    check(ok.code == 0, "sibling modules check: " + ok.err);
    Result ran = run({"run", main});
    check(ran.code == 42, "a qualified call answers across modules: " + ran.err);
    std::string types = std::string(dir) + "/types.cid";
    std::string tymain = std::string(dir) + "/tymain.cid";
    FILE* typesFile = fopen(types.c_str(), "w");
    const char* typesText = "export struct S { var v: int; }\n";
    fwrite(typesText, 1, strlen(typesText), typesFile);
    fclose(typesFile);
    FILE* tymainFile = fopen(tymain.c_str(), "w");
    const char* tymainText =
        "import types;\n"
        "type V = types.S;\n"
        "func main() -> int {\n"
        "    var x: V = types.S { v: 9 };\n"
        "    return x.v;\n"
        "}\n";
    fwrite(tymainText, 1, strlen(tymainText), tymainFile);
    fclose(tymainFile);
    appendMainEntry(tymain);
    Result tyCheck = run({"check", tymain});
    check(tyCheck.code == 0, "a dot-qualified alias checks: " + tyCheck.err);
    Result tyRun = run({"run", tymain});
    check(tyRun.code == 9, "a dot-qualified value runs: " + tyRun.err);
    std::string dotted = std::string(dir) + "/dot.cid";
    FILE* dotFile = fopen(dotted.c_str(), "w");
    const char* dottedText =
        "import no.such.stdlib;\n"
        "func main() -> int { return 0; }\n";
    fwrite(dottedText, 1, strlen(dottedText), dotFile);
    fclose(dotFile);
    // The search roots now include the working tree's modules/ directory,
    // so `std.io` may resolve without CIDER_LIB. Use a name no tree owns:
    // the point is a loud, module-naming failure.
    const char* saved = getenv("CIDER_LIB");
    if (saved != nullptr) {
        std::string keep = saved;
        unsetenv("CIDER_LIB");
        Result missing = run({"check", dotted});
        setenv("CIDER_LIB", keep.c_str(), 1);
        check(missing.code != 0, "a missing dotted import fails");
        check(missing.err.find("no.such.stdlib") != std::string::npos,
              "naming the module");
    } else {
        Result missing = run({"check", dotted});
        check(missing.code != 0, "a missing dotted import fails");
        check(missing.err.find("no.such.stdlib") != std::string::npos,
              "naming the module");
    }
    unlink(main.c_str());
    unlink(dep.c_str());
    unlink(dotted.c_str());
    rmdir(dir);
}

// A from-imported global is usable by field: the dependency's globals
// register before any dependent lowers, so a store through the alias
// reaches the label instead of refusing on a missing binding.
void testImportGlobal() {
    std::printf("imported globals\n");
    char dir[] = "/tmp/cider_impglob_XXXXXX";
    check(mkdtemp(dir) != nullptr, "a scratch directory exists");
    std::string dep = std::string(dir) + "/lib.cid";
    FILE* depFile = fopen(dep.c_str(), "w");
    const char* depText =
        "export struct P { var x: int; }\n"
        "export var gp: P = P { x: 0 };\n";
    fwrite(depText, 1, strlen(depText), depFile);
    fclose(depFile);
    std::string main = std::string(dir) + "/main.cid";
    FILE* mainFile = fopen(main.c_str(), "w");
    const char* mainText =
        "from lib import gp;\n"
        "func main() -> int {\n"
        "    gp.x = 7;\n"
        "    return gp.x;\n"
        "}\n";
    fwrite(mainText, 1, strlen(mainText), mainFile);
    fclose(mainFile);
    appendMainEntry(main);
    Result ok = run({"check", main});
    check(ok.code == 0, "an imported global checks: " + ok.err);
    Result ran = run({"run", main});
    check(ran.code == 7,
          "a store through an imported global reads back: " + ran.err);
    unlink(main.c_str());
    unlink(dep.c_str());
    rmdir(dir);
}

// A qualified global is usable by field: `lib.gq` resolves in the
// defining scope to the same label a bare spelling would use.
void testImportQualifiedGlobal() {
    std::printf("qualified globals\n");
    char dir[] = "/tmp/cider_impqglob_XXXXXX";
    check(mkdtemp(dir) != nullptr, "a scratch directory exists");
    std::string dep = std::string(dir) + "/lib.cid";
    FILE* depFile = fopen(dep.c_str(), "w");
    const char* depText =
        "export struct Q { var y: int; }\n"
        "export var gq: Q = Q { y: 0 };\n";
    fwrite(depText, 1, strlen(depText), depFile);
    fclose(depFile);
    std::string main = std::string(dir) + "/main.cid";
    FILE* mainFile = fopen(main.c_str(), "w");
    const char* mainText =
        "import lib;\n"
        "func main() -> int {\n"
        "    lib.gq.y = 11;\n"
        "    return lib.gq.y;\n"
        "}\n";
    fwrite(mainText, 1, strlen(mainText), mainFile);
    fclose(mainFile);
    appendMainEntry(main);
    Result ok = run({"check", main});
    check(ok.code == 0, "a qualified global checks: " + ok.err);
    Result ran = run({"run", main});
    check(ran.code == 11,
          "a store through a qualified global reads back: " + ran.err);
    std::string main2 = std::string(dir) + "/main2.cid";
    FILE* main2File = fopen(main2.c_str(), "w");
    const char* main2Text =
        "import lib;\n"
        "from lib import Q;\n"
        "func main() -> int {\n"
        "    lib.gq.y = 5;\n"
        "    var r: Q = lib.gq;\n"
        "    return r.y;\n"
        "}\n";
    fwrite(main2Text, 1, strlen(main2Text), main2File);
    fclose(main2File);
    appendMainEntry(main2);
    Result ok2 = run({"check", main2});
    check(ok2.code == 0, "a bare qualified read checks: " + ok2.err);
    Result ran2 = run({"run", main2});
    check(ran2.code == 5,
          "a bare qualified global reads back: " + ran2.err);
    unlink(main2.c_str());
    unlink(main.c_str());
    unlink(dep.c_str());
    rmdir(dir);
}

// A module-level `const` has no storage: reads fold its initialiser
// where used, including through another const.
void testModuleConst() {
    std::printf("module constants\n");
    char dir[] = "/tmp/cider_modconst_XXXXXX";
    check(mkdtemp(dir) != nullptr, "a scratch directory exists");
    std::string main = std::string(dir) + "/main.cid";
    FILE* mainFile = fopen(main.c_str(), "w");
    const char* mainText =
        "const C: int = 55;\n"
        "const D: int = C + 2;\n"
        "func main() -> int {\n"
        "    return C + D - 100;\n"
        "}\n";
    fwrite(mainText, 1, strlen(mainText), mainFile);
    fclose(mainFile);
    appendMainEntry(main);
    Result ran = run({"run", main});
    check(ran.code == 12,
          "a module const reads back: " + ran.err);
    // A use before the declaration lowers identically: literal leaves
    // fold against the declared type instead of a recorded one.
    std::string fwd = std::string(dir) + "/fwd.cid";
    FILE* fwdFile = fopen(fwd.c_str(), "w");
    const char* fwdText =
        "func main() -> int { return C; }\n"
        "const C: int = 55;\n";
    fwrite(fwdText, 1, strlen(fwdText), fwdFile);
    fclose(fwdFile);
    appendMainEntry(fwd);
    Result fwdRan = run({"run", fwd});
    check(fwdRan.code == 55,
          "a forward const read lowers: " + fwdRan.err);
    // A forward use of a compound initialiser still refuses loudly:
    // its subtree types are recorded only when checking reaches it.
    std::string fwdc = std::string(dir) + "/fwdc.cid";
    FILE* fwdcFile = fopen(fwdc.c_str(), "w");
    const char* fwdcText =
        "func main() -> int { return D; }\n"
        "const D: int = C + 2;\n"
        "const C: int = 5;\n";
    fwrite(fwdcText, 1, strlen(fwdcText), fwdcFile);
    fclose(fwdcFile);
    appendMainEntry(fwdc);
    Result fwdcRan = run({"run", fwdc});
    check(fwdcRan.code != 0,
          "a forward compound const still refuses: " + fwdcRan.err);
    unlink(fwdc.c_str());
    unlink(fwd.c_str());
    unlink(main.c_str());
    rmdir(dir);
}

// A module-qualified const folds through its defining module in both
// the checker gate and lowering: `lib.K` in a const position answers
// the member init folded in the home scope.
void testQualifiedConst() {
    std::printf("qualified constants\n");
    char dir[] = "/tmp/cider_qualconst_XXXXXX";
    check(mkdtemp(dir) != nullptr, "a scratch directory exists");
    std::string dep = std::string(dir) + "/lib.cid";
    FILE* depFile = fopen(dep.c_str(), "w");
    const char* depText = "export const K: int = 11;\n";
    fwrite(depText, 1, strlen(depText), depFile);
    fclose(depFile);
    std::string main = std::string(dir) + "/main.cid";
    FILE* mainFile = fopen(main.c_str(), "w");
    const char* mainText =
        "import lib;\n"
        "var g: int = lib.K;\n"
        "func main() -> int { return g * 2; }\n";
    fwrite(mainText, 1, strlen(mainText), mainFile);
    fclose(mainFile);
    appendMainEntry(main);
    Result ran = run({"run", main});
    check(ran.code == 22,
          "a qualified const folds in a global: " + ran.err);
    unlink(main.c_str());
    unlink(dep.c_str());
    rmdir(dir);
}

// A dotted import resolves through CIDER_LIB: <lib>/x/y.cid answers
// `import x.y`. Without the variable the same import still fails loudly.
void testLibSearch() {
    std::printf("library search\n");
    char lib[] = "/tmp/cider_lib_XXXXXX";
    check(mkdtemp(lib) != nullptr, "a library directory exists");
    std::string sub = std::string(lib) + "/x";
    check(mkdir(sub.c_str(), 0700) == 0, "a library package exists");
    std::string mod = sub + "/y.cid";
    FILE* modFile = fopen(mod.c_str(), "w");
    fwrite("export func mval() -> int { return 7; }\n", 1, 40, modFile);
    fclose(modFile);
    char dir[] = "/tmp/cider_libuse_XXXXXX";
    check(mkdtemp(dir) != nullptr, "a scratch directory exists");
    // A local tree with the same dotted name must not shadow CIDER_LIB.
    std::string local = std::string(dir) + "/x";
    check(mkdir(local.c_str(), 0700) == 0, "a local package exists");
    std::string localMod = local + "/y.cid";
    FILE* localFile = fopen(localMod.c_str(), "w");
    fwrite("export func mval() -> int { return 99; }\n", 1, 41, localFile);
    fclose(localFile);
    std::string main = std::string(dir) + "/main.cid";
    FILE* mainFile = fopen(main.c_str(), "w");
    fwrite("import x.y;\nfunc main() -> int { return y.mval(); }\n", 1, 52,
           mainFile);
    fclose(mainFile);
    appendMainEntry(main);
    const char* saved = getenv("CIDER_LIB");
    std::string keep = saved != nullptr ? saved : "";
    setenv("CIDER_LIB", lib, 1);
    Result ok = run({"check", main});
    check(ok.code == 0, "a library module checks: " + ok.err);
    Result ran = run({"run", main});
    check(ran.code == 7, "a library call answers: " + ran.err);
    if (saved != nullptr) {
        setenv("CIDER_LIB", keep.c_str(), 1);
    } else {
        unsetenv("CIDER_LIB");
    }
    unlink(main.c_str());
    unlink(localMod.c_str());
    rmdir(local.c_str());
    unlink(mod.c_str());
    rmdir(sub.c_str());
    rmdir(lib);
    rmdir(dir);
}

// A dotted import falls back to the entry directory: `import a.util`
// beside `<dir>/a/util.cid` answers without CIDER_LIB. Library names
// keep their priority, so `std.io` still answers from the library.
void testSubdirImport() {
    std::printf("subdirectory imports\n");
    char dir[] = "/tmp/cider_subimp_XXXXXX";
    check(mkdtemp(dir) != nullptr, "a scratch directory exists");
    std::string sub = std::string(dir) + "/a";
    check(mkdir(sub.c_str(), 0700) == 0, "a sibling package exists");
    std::string mod = sub + "/util.cid";
    FILE* modFile = fopen(mod.c_str(), "w");
    const char* modText =
        "export struct Box { var v: int; }\n"
        "export func mval() -> int { return 9; }\n"
        "export func bget(b: Box) -> int { return b.v; }\n"
        "export enum Color { RED, GREEN, BLUE }\n"
        "export type Alias = Color;\n";
    fwrite(modText, 1, strlen(modText), modFile);
    fclose(modFile);
    std::string main = std::string(dir) + "/main.cid";
    FILE* mainFile = fopen(main.c_str(), "w");
    const char* mainText =
        "import a.util;\n"
        "from a.util import Box;\n"
        "func main() -> int {\n"
        "    if (util.mval() != 9) { return 1; }\n"
        "    var b: util.Box = Box { v: 9 };\n"
        "    if (util.bget(b) != 9) { return 2; }\n"
        "    var e: int = util.Alias.GREEN;\n"
        "    if (e != 1) { return 3; }\n"
        "    return 0;\n"
        "}\n";
    fwrite(mainText, 1, strlen(mainText), mainFile);
    fclose(mainFile);
    appendMainEntry(main);
    Result ok = run({"check", main});
    check(ok.code == 0, "a sibling module checks: " + ok.err);
    Result ran = run({"run", main});
    check(ran.code == 0, "a sibling call answers: code=" + std::to_string(ran.code) + " out=" + ran.out + " err=" + ran.err);
    unlink(main.c_str());
    unlink(mod.c_str());
    rmdir(sub.c_str());
    rmdir(dir);
}

void testModuleAutodiscovery() {
    std::printf("module autodiscovery\n");
    char root[] = "/tmp/cider_discover_XXXXXX";
    check(mkdtemp(root) != nullptr, "a discovery root exists");
    std::string rootPath = root;
    std::string srcDir = rootPath + "/src";
    std::string modules = rootPath + "/modules";
    check(mkdir(srcDir.c_str(), 0700) == 0, "the source directory exists");
    check(mkdir(modules.c_str(), 0700) == 0, "the modules directory exists");
    std::string lib = modules + "/lib.cid";
    FILE* libFile = fopen(lib.c_str(), "w");
    const char* libText = "export func answer() -> int { return 42; }\n";
    fwrite(libText, 1, strlen(libText), libFile);
    fclose(libFile);
    std::string main = srcDir + "/main.cid";
    FILE* mainFile = fopen(main.c_str(), "w");
    const char* mainText =
        "import lib;\n"
        "func main() -> int { return lib.answer(); }\n";
    fwrite(mainText, 1, strlen(mainText), mainFile);
    fclose(mainFile);
    appendMainEntry(main);

    char previous[4096];
    check(getcwd(previous, sizeof previous) != nullptr,
          "the current directory is readable");
    check(chdir(rootPath.c_str()) == 0, "the discovery root is entered");
    Result checked = run({"check", main});
    check(checked.code == 0,
          "a module in the root's modules/ is found: " + checked.err);
    Result ran = run({"run", main});
    check(ran.code == 42,
          "a discovered module call answers: " + ran.err);
    check(chdir(previous) == 0, "the current directory is restored");

    unlink(main.c_str());
    unlink(lib.c_str());
    rmdir(srcDir.c_str());
    rmdir(modules.c_str());
    rmdir(rootPath.c_str());
}

// v2 accepted `linux` and `x86_64-linux` as spellings of the native target.
void testNativeTargetAliases() {
    std::printf("native target aliases\n");
    std::string src = writeTemp("func main() -> int { return 7; }\n");
    for (const char* alias : {"linux", "x86_64-linux"}) {
        char asmPath[] = "/tmp/cider_driver_XXXXXX.s";
        int asmFd = mkstemps(asmPath, 2);
        close(asmFd);
        Result compiled =
            run({"compile", "--target", alias, "-o", asmPath, src});
        check(compiled.code == 0,
              std::string("compile --target ") + alias + ": " + compiled.err);
        FILE* asmFile = fopen(asmPath, "r");
        char head[64] = {0};
        size_t got = asmFile != nullptr ? fread(head, 1, 63, asmFile) : 0;
        if (asmFile != nullptr) {
            fclose(asmFile);
        }
        check(got > 0 && std::string(head).find("_start:") != std::string::npos,
              std::string("--target ") + alias + " emits native assembly");
        unlink(asmPath);
    }
    unlink(src.c_str());
}

// Target names the toolchain knows reach the front end, and each target's
// entry is source-defined: main is never accepted as UEFI's firmware entry.
void testKnownTargetsReachFrontEnd() {
    std::printf("known targets reach the front end\n");
    std::string src = writeTemp("func main() -> int { return 0; }\n", false);
    Result uefiCheck = run({"check", "--target", "uefi", src});
    check(uefiCheck.code != 0, "check --target uefi rejects main-only source");
    check(uefiCheck.err.find("efi_main") != std::string::npos,
          "the missing-entry diagnostic names efi_main");
    Result uefiCompile = run({"compile", "--target", "uefi", src});
    check(uefiCompile.code != 0, "compile --target uefi refuses");
    check(uefiCompile.err.find("efi_main") != std::string::npos,
          "naming efi_main");
    check(uefiCompile.err.find("native only") == std::string::npos,
          "not claiming uefi is native only");
    Result bogus = run({"check", "--target", "bogus", src});
    check(bogus.code != 0, "an unknown target fails");
    check(bogus.err.find("bogus") != std::string::npos,
          "naming the unknown target");
    unlink(src.c_str());

    std::string asmSrc = writeTemp(
        "msabi func efi_main(image_handle: *void, system_table: *void) -> int {\n"
        "    asm { \"nop\" }\n"
        "    return 0;\n"
        "}\n",
        false);
    Result uefiAsm = run({"check", "--target", "uefi", asmSrc});
    check(uefiAsm.code == 0, "uefi accepts raw inline asm through the driver: " +
                                 uefiAsm.err);
    unlink(asmSrc.c_str());

    std::string badAsm = writeTemp(
        "msabi func efi_main(image_handle: *void, system_table: *void) -> int {\n"
        "    asm { \"mov edi, {nope}\" }\n"
        "    return 0;\n"
        "}\n",
        false);
    Result uefiBadAsm = run({"check", "--target", "uefi", badAsm});
    check(uefiBadAsm.code != 0, "uefi validates asm placeholders");
    check(uefiBadAsm.err.find("nope") != std::string::npos,
          "the placeholder diagnostic names the name");
    unlink(badAsm.c_str());

    std::string entry = writeTemp(
        "msabi func efi_main(image_handle: *void, system_table: *void) -> int {\n"
        "    return 0;\n"
        "}\n",
        false);
    Result uefiRun = run({"run", "--target", "uefi", entry});
    check(uefiRun.code != 0, "run --target uefi names the PE link path");
    check(uefiRun.err.find("link_uefi.sh") != std::string::npos,
          "the run refusal points at link_uefi.sh");
    unlink(entry.c_str());
}

// --checks is a front-end option, not a run-only switch: check, compile and
// run all accept it, and the runtime result is the diagnosed bounds exit.
void testChecksFlag() {
    std::printf("--checks\n");
    std::string src = writeTemp(
        "var idx: int = 0;\n"
        "func main() -> int {\n"
        "    var a: [int; 3] = [10, 20, 30];\n"
        "    idx = 5;\n"
        "    return a[idx];\n"
        "}\n");
    Result checked = run({"check", "--checks", src});
    check(checked.code == 0, "check --checks accepts the flag: " + checked.err);

    char asmPath[] = "/tmp/cider_driver_checks_XXXXXX.s";
    int asmFd = mkstemps(asmPath, 2);
    close(asmFd);
    Result compiled = run({"compile", "--checks", "-o", asmPath, src});
    check(compiled.code == 0, "compile --checks accepts the flag: " + compiled.err);

    Result ran = run({"run", "--target", "vm", "--checks", src});
    check(ran.code == 2, "run --checks answers the diagnosed exit");
    check(ran.err == "vm error: index out of bounds\n",
          "run --checks writes the bounds line");

    Result bogus = run({"check", "--not-a-flag", src});
    check(bogus.code != 0, "an unknown flag still fails");
    check(bogus.err.find("--not-a-flag") != std::string::npos,
          "the unknown flag is named");
    unlink(src.c_str());
    unlink(asmPath);
}

// OOM injection crosses the stage boundary as the two C++ allocation
// shapes; the top-level catch in main maps both to the same compiler line.
// A failed write names the destination and exits 2 instead of reporting a
// generic I/O failure.
void testOomAndWriteFailure() {
    std::printf("allocation and write failures\n");
    std::string src = writeTemp("func main() -> int { return 1; }\n");

    bool badAlloc = false;
    setenv("CIDER_FAULT_ALLOC", "check", 1);
    try {
        run({"check", src});
    } catch (const std::bad_alloc&) {
        badAlloc = true;
    }
    unsetenv("CIDER_FAULT_ALLOC");
    check(badAlloc, "a stage allocation failure throws bad_alloc");

    bool lengthError = false;
    setenv("CIDER_FAULT_ALLOC", "check:len", 1);
    try {
        run({"check", src});
    } catch (const std::length_error&) {
        lengthError = true;
    }
    unsetenv("CIDER_FAULT_ALLOC");
    check(lengthError, "a stage length failure throws length_error");

    char dir[] = "/tmp/cider_driver_out_XXXXXX";
    check(mkdtemp(dir) != nullptr, "a scratch output directory exists");
    Result failed = run({"compile", src, "-o", dir});
    check(failed.code == 2, "a failed write exits 2");
    check(failed.err == "compiler: cannot write '" + std::string(dir) + "'\n",
          "a failed write names the destination");
    rmdir(dir);
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
    check(target.code != 0, "a target with the wrong entry fails");
    check(target.err.find("uefi") != std::string::npos, "naming the target");
    Result sub = run({"frobnicate", src});
    check(sub.code != 0, "an unknown subcommand fails");
    unlink(src.c_str());
}

// The interpreter runs without a toolchain; there is nothing to emit for it.
void testVmTarget() {
    std::printf("the vm target interprets\n");
    std::string src = writeTemp("func main() -> int { return 40 + 2; }\n");
    Result runVm = run({"run", "--target", "vm", src});
    check(runVm.code == 42, "run --target vm answers the exit code");
    Result compileVm = run({"compile", "--target", "vm", src});
    check(compileVm.code != 0, "compile --target vm fails");
    check(compileVm.err.find("vm") != std::string::npos, "naming the target");
    unlink(src.c_str());
}

}  // namespace

int main() {
    std::printf("driver_test — the command behaves\n\n");
    testCheck();
    testCompileIr();
    testRun();
    testImports();
    testImportAlias();
    testModFnValue();
    testImportGlobal();
    testImportQualifiedGlobal();
    testModuleConst();
    testQualifiedConst();
    testLibSearch();
    testSubdirImport();
    testModuleAutodiscovery();
    testNativeTargetAliases();
    testKnownTargetsReachFrontEnd();
    testChecksFlag();
    testOomAndWriteFailure();
    testFailures();
    testVmTarget();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
