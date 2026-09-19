// Tests for `cider bundle` (src/bundler.h): multi-file amalgamation that
// compiles identically. Self-contained: builds a temp project on disk and
// drives the Bundler API directly.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "bundler.h"
#include "lexer.h"
#include "parser.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

namespace {

std::string tmpDir() {
    char tmpl[] = "/tmp/cider_bundle_testXXXXXX";
    if (!mkdtemp(tmpl)) {
        std::fprintf(stderr, "mkdtemp failed\n");
        std::exit(2);
    }
    return tmpl;
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

int countOccurrences(const std::string& haystack, const std::string& needle) {
    int n = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

bool parsesCleanly(const std::string& text, const std::string& name) {
    cider::ErrorReporter rep;
    rep.setFile(name);
    cider::Lexer lexer(text, rep);
    cider::Parser parser(lexer, rep);
    auto prog = parser.parseProgram();
    return !rep.hasErrors() && prog != nullptr;
}

int run() {
    std::string dir = tmpDir();
    // Diamond: main -> {liba, libb} -> defs; main also from-imports.
    writeFile(dir + "/defs.cid", "struct Point {\n  var x: int;\n  var y: int;\n}\n");
    writeFile(dir + "/liba.cid",
              "import defs;\nfunc norm2(p: Point*) -> int {\n  return p.x + p.y;\n}\n");
    writeFile(dir + "/libb.cid",
              "import defs;\nfrom liba import norm2;\nfunc wrap(p: Point*) -> int {\n  return norm2(p);\n}\n");
    writeFile(dir + "/main.cid",
              "import liba;\nimport libb;\nfrom defs import Point;\n"
              "func main() -> int {\n  return 0;\n}\n");

    cider::Bundler bundler;
    cider::BundleResult r = bundler.bundle(dir + "/main.cid");
    CHECK(r.ok);
    // Every module exactly once (diamond dedup): 4 banners, 1 struct.
    CHECK(countOccurrences(r.output, "bundled from") == 4);
    CHECK(countOccurrences(r.output, "struct Point") == 1);
    CHECK(countOccurrences(r.output, "func norm2") == 1);
    CHECK(countOccurrences(r.output, "func wrap") == 1);
    CHECK(countOccurrences(r.output, "func main") == 1);
    // No import statements survive.
    CHECK(r.output.find("import defs") == std::string::npos);
    CHECK(r.output.find("import liba") == std::string::npos);
    CHECK(r.output.find("import libb") == std::string::npos);
    CHECK(r.output.find("from defs") == std::string::npos);
    CHECK(r.output.find("from liba") == std::string::npos);
    // Entry comes last (dependencies first).
    CHECK(r.output.rfind("main.cid") > r.output.find("defs.cid"));
    // The amalgamation itself parses cleanly.
    CHECK(parsesCleanly(r.output, "bundled.cid"));

    // Missing module.
    writeFile(dir + "/m1.cid", "import nosuchmod;\nfunc main() -> int {\n  return 0;\n}\n");
    cider::BundleResult bad = bundler.bundle(dir + "/m1.cid");
    CHECK(!bad.ok);
    CHECK(bad.error.find("Could not resolve module 'nosuchmod'") != std::string::npos);

    // Circular import.
    writeFile(dir + "/circ_a.cid", "import circ_b;\nfunc main() -> int {\n  return 0;\n}\n");
    writeFile(dir + "/circ_b.cid", "import circ_a;\nfunc fb() -> int {\n  return 1;\n}\n");
    cider::BundleResult circ = bundler.bundle(dir + "/circ_a.cid");
    CHECK(!circ.ok);
    CHECK(circ.error.find("Circular") != std::string::npos);

    // Parse error in entry.
    writeFile(dir + "/broken.cid", "func broken( -> int {\n  return 0;\n}\n");
    cider::BundleResult perr = bundler.bundle(dir + "/broken.cid");
    CHECK(!perr.ok);
    CHECK(perr.error.find("parse error") != std::string::npos);
    return 0;
}

}  // namespace

int main() { return run(); }
