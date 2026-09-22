#include <cstdio>
#include <memory>
#include <string>
#include "module_scope.h"
#include "symbols.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    using namespace cider;

    // qualifierForModule: last dot-segment.
    CHECK(qualifierForModule("a.util") == "util");
    CHECK(qualifierForModule("solo") == "solo");

    // Spec section 5 example: dots become double underscores.
    CHECK(mangleQualifiedName("a.util", "foo") == "mod_a__util__foo");

    // Escape invariant: escaped segments never contain "__", so "__"
    // uniquely marks separators in mangled names.
    CHECK(escapeModuleSegment("plain").find("__") == std::string::npos);
    CHECK(escapeModuleSegment("_").find("__") == std::string::npos);
    CHECK(escapeModuleSegment("__").find("__") == std::string::npos);
    CHECK(escapeModuleSegment("a_b__c").find("__") == std::string::npos);
    CHECK(escapeModuleSegment("_") == "_u");
    CHECK(escapeModuleSegment("a_b") == "a_ub");

    // Boundary underscores: pairs that collided under the old '___' escape
    // must mangle distinctly.
    CHECK(mangleQualifiedName("net_", "parse") == "mod_net_u__parse");
    CHECK(mangleQualifiedName("net", "_parse") == "mod_net___uparse");
    CHECK(mangleQualifiedName("net_", "parse") != mangleQualifiedName("net", "_parse"));
    CHECK(mangleQualifiedName("a_.b", "c") != mangleQualifiedName("a._b", "c"));
    CHECK(mangleQualifiedName("a_b", "c") != mangleQualifiedName("a", "b_c"));

    // canonicalTypeName: full logical path, never the bare qualifier.
    CHECK(canonicalTypeName("Foo", "a.util") == "a.util::Foo");
    CHECK(canonicalTypeName("a.util::Foo", "b.util") == "a.util::Foo");
    CHECK(canonicalTypeName("Foo", "") == "Foo");

    // ModuleSymbol carries its logical name and scope.
    auto m = std::make_shared<ModuleSymbol>();
    m->kind = SymbolKind::MODULE;
    m->name = "util";
    m->logicalName = "a.util";
    CHECK(m->scope == nullptr);
    return 0;
}

int main() { return run(); }
