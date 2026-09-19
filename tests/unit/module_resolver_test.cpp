#include <cstdio>
#include <string>
#include <vector>

#include "module_resolver.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    cider::ModuleResolver resolver;

    // Not found.
    auto nf = resolver.resolve("no_such_module", "/nonexistent/x.cid");
    CHECK(nf.type == cider::ModuleType::NOT_FOUND);

    // Relative import next to the origin file (design §5: file-relative).
    auto rel = resolver.resolve("mod_a", "<repo>/tests/fixtures/mod_b.cid");
    CHECK(rel.type == cider::ModuleType::CIDER_SOURCE_FILE);
    CHECK(rel.path.find("tests/fixtures/mod_a.cid") != std::string::npos);
    CHECK(rel.moduleName == "mod_a");

    // Dotted module names become paths.
    resolver.addSearchPath("<repo>/tests/fixtures");
    auto dotted = resolver.resolve("pkg.sub", "");
    CHECK(dotted.type == cider::ModuleType::NOT_FOUND);  // no pkg/sub.cid there
    return 0;
}

int main() { return run(); }
