#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "module_resolver.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

namespace {

// CIDER-BUG-222: a checkout-shaped tree to resolve against. The stdlib
// lives in ONE place (`<root>/modules`); the build directory is nested
// deeper than the old two-level `../`, `../../` probe could reach.
struct Fixture {
    std::string root;
    char cwd[4096];

    Fixture() {
        char tmpl[] = "/tmp/cider_resolver_testXXXXXX";
        const char* dir = mkdtemp(tmpl);
        if (!dir) {
            std::fprintf(stderr, "mkdtemp failed\n");
            std::exit(2);
        }
        root = dir;
        for (const char* d : {"modules", "modules/std", "modules/pack", "modules2",
                              "elsewhere", "elsewhere/x", "build", "build/a", "build/a/b"}) {
            if (mkdir((root + "/" + d).c_str(), 0755) != 0) {
                std::fprintf(stderr, "mkdir %s failed\n", d);
                std::exit(2);
            }
        }
        write("modules/std/demo.cid", "export func demo() -> int { return 1; }\n");
        write("modules/pack/__init__.cid", "export func p() -> int { return 2; }\n");
        write("modules/plain.cid", "export func q() -> int { return 3; }\n");
        write("modules2/only_here.cid", "export func r() -> int { return 4; }\n");
        if (!getcwd(cwd, sizeof(cwd))) {
            std::fprintf(stderr, "getcwd failed\n");
            std::exit(2);
        }
    }

    ~Fixture() {
        if (chdir(cwd) != 0) { /* best effort: the test may have failed early */ }
        for (const char* f : {"modules/std/demo.cid", "modules/pack/__init__.cid",
                              "modules/plain.cid", "modules2/only_here.cid", "empty.cid"}) {
            std::remove((root + "/" + f).c_str());
        }
        for (const char* d : {"modules/std", "modules/pack", "modules", "modules2",
                              "elsewhere/x", "elsewhere", "build/a/b", "build/a",
                              "build", "empty"}) {
            rmdir((root + "/" + d).c_str());
        }
        rmdir(root.c_str());
    }

    void write(const std::string& rel, const std::string& text) {
        std::ofstream out(root + "/" + rel, std::ios::binary | std::ios::trunc);
        out << text;
    }
};

}  // namespace

int run() {
    cider::ModuleResolver resolver;

    // Not found.
    auto nf = resolver.resolve("no_such_module", "/nonexistent/x.cid");
    CHECK(nf.type == cider::ModuleType::NOT_FOUND);
    CHECK(nf.path.empty());

    // Relative import next to the origin file (design §5: file-relative).
    auto rel = resolver.resolve("mod_a", "<repo>/tests/fixtures/mod_b.cid");
    CHECK(rel.type == cider::ModuleType::CIDER_SOURCE_FILE);
    CHECK(rel.path.find("tests/fixtures/mod_a.cid") != std::string::npos);
    CHECK(rel.moduleName == "mod_a");

    // Dotted module names become paths.
    resolver.addSearchPath("<repo>/tests/fixtures");
    auto dotted = resolver.resolve("pkg.sub", "");
    CHECK(dotted.type == cider::ModuleType::NOT_FOUND);  // no pkg/sub.cid there

    // CIDER-BUG-222: std resolution must not depend on where the binary
    // lives. The rule is anchored at the WORKING DIRECTORY, so a module in
    // an ancestor's `modules/` is found from anywhere below it -- at any
    // depth, whatever the build directory looks like -- and it resolves to
    // one spelling that mentions neither the binary nor the search root
    // that found it.
    Fixture fx;
    fx.write("empty.cid", "");
    CHECK(chdir((fx.root + "/build/a/b").c_str()) == 0);

    auto fromDeep = cider::ModuleResolver().resolve("std.demo", "main.cid");
    CHECK(fromDeep.type == cider::ModuleType::CIDER_SOURCE_FILE);
    CHECK(fromDeep.moduleName == "demo");
    // The exact string, and nothing else: it is a function of the tree and
    // the working directory only. Before the fix this lookup existed solely
    // as the binary's `../modules` and `../../modules`, so a binary one
    // level deeper found nothing at all and a shallower one spelled the
    // resolved path differently -- which changed the bytes `cider bundle`
    // and `cider compile -g` emitted for one and the same program.
    CHECK(fromDeep.path == "../../../modules/std/demo.cid");
    CHECK(fromDeep.path.find(fx.root) == std::string::npos);
    CHECK(fromDeep.path.find("build") == std::string::npos);

    // A package directory resolves through the same walk, normalized too.
    auto pack = cider::ModuleResolver().resolve("pack", "");
    CHECK(pack.type == cider::ModuleType::CIDER_PACKAGE_DIR);
    CHECK(pack.path == "../../../modules/pack");
    CHECK(pack.moduleName == "pack");

    // Same module, same string, with and without an origin file.
    CHECK(cider::ModuleResolver().resolve("std.demo", "").path == fromDeep.path);

    // The same source from the project root: a different working directory,
    // so a different -- equally deterministic -- spelling.
    CHECK(chdir(fx.root.c_str()) == 0);
    auto fromRoot = cider::ModuleResolver().resolve("std.demo", "main.cid");
    CHECK(fromRoot.path == "modules/std/demo.cid");

    // One module, one string: a resolver that also searches `./modules/`
    // explicitly resolves it to the same path as the default resolver, so
    // an extra search path cannot change the compiler's output.
    cider::ModuleResolver withExtra;
    withExtra.addSearchPath("./modules/");
    CHECK(withExtra.resolve("std.demo", "main.cid").path == fromRoot.path);

    // A relative override still wins over every discovery rule, and it is
    // normalized lexically instead of being passed through with its `..`
    // segments -- `elsewhere/../modules2` names the same file as
    // `modules2`, and must print as the latter.
    cider::ModuleResolver overridden;
    overridden.addSearchPath("elsewhere/../modules2");
    auto over = overridden.resolve("only_here", "main.cid");
    CHECK(over.type == cider::ModuleType::CIDER_SOURCE_FILE);
    CHECK(over.path == "modules2/only_here.cid");
    // Only the override can find it: no discovery rule knows about
    // `modules2`, and a relative override is honored from the working
    // directory.
    CHECK(cider::ModuleResolver().resolve("only_here", "main.cid").type ==
          cider::ModuleType::NOT_FOUND);

    // The ancestor walk must not invent a root for a module that is not
    // there.
    CHECK(cider::ModuleResolver().resolve("no_such_module", "main.cid").type ==
          cider::ModuleType::NOT_FOUND);
    CHECK(chdir(fx.cwd) == 0);
    return 0;
}

int main() { return run(); }
