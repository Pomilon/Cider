// CIDER-FEAT-006: `.cider-meta` module snapshot helpers unit test.
//
// Covers the pure pieces of src/module_meta.h: SHA-256, the `module`/`dep`
// header parser, `template<...>` line translation, cache file naming,
// declaration span extraction (body stripping, strings/comments), and
// logical module-name derivation from a `modules/` root.
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "module_meta.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

static bool writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << text;
    return true;
}

int run() {
    // --- SHA-256 (FIPS 180-4 vectors) ------------------------------------
    CHECK(cider::sha256Hex("") ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(cider::sha256Hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(cider::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // --- header parsing ---------------------------------------------------
    cider::MetaFile meta;
    const std::string metaText =
        "module std.io\n"
        "dep std.string\n"
        "dep std.alloc\n"
        "\n"
        "export func print(msg: string) -> void { }\n";
    CHECK(cider::parseMetaText(metaText, meta));
    CHECK(meta.moduleName == "std.io");
    CHECK(meta.deps.size() == 2);
    CHECK(meta.deps[0] == "std.string");
    CHECK(meta.deps[1] == "std.alloc");
    CHECK(meta.declarations.find("export func print") != std::string::npos);

    cider::MetaFile missing;
    CHECK(!cider::parseMetaText("dep x\n", missing));
    CHECK(!cider::parseMetaText("", missing));
    // A declaration line before `module` is not a valid snapshot.
    CHECK(!cider::parseMetaText("export func f() -> void;\nmodule m\n", missing));

    // --- template line translation ---------------------------------------
    CHECK(cider::translateMetaDeclarations(
              "template<T> export func f<T>(x: T) -> T { return x; }\n") ==
          "export func f<T>(x: T) -> T { return x; }\n");
    CHECK(cider::translateMetaDeclarations(
              "template<T, U> export struct Pair<T, U> { var a: T; var b: U; }\n") ==
          "export struct Pair<T, U> { var a: T; var b: U; }\n");
    CHECK(cider::translateMetaDeclarations("export func g() -> int  { }\n") ==
          "export func g() -> int  { }\n");

    // --- cache file names -------------------------------------------------
    CHECK(cider::metaFileName("std.io", ".cider-meta") == "std.io.cider-meta");
    CHECK(cider::metaFileName("a/b c", ".hash") == "a_b_c.hash");

    // --- declaration span extraction -------------------------------------
    const std::string src =
        "// leading comment\n"
        "export func add(a: int, b: int) -> int {\n"
        "  var s: char* = \"}\";\n"
        "  return a + b;\n"
        "}\n"
        "var g: int = 7;\n";
    std::string fnText = cider::extractDeclSource(src, 2, 1, false);
    CHECK(fnText.find("export func add") == 0);
    CHECK(fnText.find("return a + b;") != std::string::npos);
    std::string stripped = cider::extractDeclSource(src, 2, 1, true);
    CHECK(stripped.find("export func add(a: int, b: int) -> int") == 0);
    CHECK(stripped.find("{ }") != std::string::npos);
    CHECK(stripped.find("return a + b;") == std::string::npos);
    // The `}` inside the string literal must not end the body early.
    CHECK(stripped.find("var s: char*") == std::string::npos);
    std::string varText = cider::extractDeclSource(src, 6, 1, false);
    CHECK(varText == "var g: int = 7;");

    // --- logical module names --------------------------------------------
    std::string tmp = "/tmp/cider-meta-test-XXXXXX";
    std::vector<char> buf(tmp.begin(), tmp.end());
    buf.push_back('\0');
    CHECK(mkdtemp(buf.data()) != nullptr);
    std::string root(buf.data());
    CHECK(mkdir((root + "/proj").c_str(), 0755) == 0 ||
          errno == EEXIST);
    CHECK(mkdir((root + "/proj/modules").c_str(), 0755) == 0 ||
          errno == EEXIST);
    CHECK(mkdir((root + "/proj/modules/pkg").c_str(), 0755) == 0 ||
          errno == EEXIST);
    CHECK(writeFile(root + "/proj/modules/pkg/mod.cid", "export func f() -> int { return 1; }\n"));
    CHECK(chdir((root + "/proj").c_str()) == 0);
    CHECK(cider::deriveModuleName("modules/pkg/mod.cid") == "pkg.mod");
    CHECK(cider::deriveModuleName(root + "/proj/modules/pkg/mod.cid") == "pkg.mod");
    // Package `__init__.cid` drops the file segment.
    CHECK(writeFile(root + "/proj/modules/pkg/__init__.cid", "export func g() -> int { return 2; }\n"));
    CHECK(cider::deriveModuleName("modules/pkg/__init__.cid") == "pkg");

    std::printf("module_meta_test: OK\n");
    return 0;
}

int main() {
    if (run() != 0) return 1;
    return 0;
}
