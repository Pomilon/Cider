#include <cstdio>
#include <memory>
#include <string>
#include "symbols.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    // TypeInfo::fromAST
    auto t1 = std::make_unique<cider::TypeAST>("int", 0, 1, 1);
    cider::TypeInfo i1 = cider::TypeInfo::fromAST(t1.get());
    CHECK(i1.name == "int" && i1.pointerLevel == 0 && !i1.isArray);
    CHECK(i1.toString() == "int");

    auto t2 = std::make_unique<cider::TypeAST>("char", 2, 1, 1);
    cider::TypeInfo i2 = cider::TypeInfo::fromAST(t2.get());
    CHECK(i2.pointerLevel == 2 && i2.toString() == "**char");

    auto t3 = std::make_unique<cider::ArrayTypeAST>(
        std::make_unique<cider::TypeAST>("int", 0, 1, 1), 3, 1, 1);
    cider::TypeInfo i3 = cider::TypeInfo::fromAST(t3.get());
    CHECK(i3.isArray && i3.arraySize == 3 && i3.toString() == "[int; 3]");

    // Scopes: local resolution shadows, parent chain resolves.
    cider::SymbolTable table;
    auto g = std::make_shared<cider::Symbol>(cider::Symbol{cider::SymbolKind::VARIABLE, "g", cider::TypeInfo{"int", 0}, 1, 1});
    table.define(g);
    CHECK(table.resolve("g") == g);

    table.enterScope();
    auto x = std::make_shared<cider::Symbol>(cider::Symbol{cider::SymbolKind::VARIABLE, "x", cider::TypeInfo{"char*", 1}, 2, 2});
    table.define(x);
    CHECK(table.resolve("x") == x);
    CHECK(table.resolve("g") == g);        // parent chain
    CHECK(table.resolve("missing") == nullptr);

    // Shadowing: redefine g in the inner scope.
    auto g2 = std::make_shared<cider::Symbol>(cider::Symbol{cider::SymbolKind::VARIABLE, "g", cider::TypeInfo{"float", 0}, 2, 3});
    table.define(g2);
    CHECK(table.resolve("g") == g2);
    table.exitScope();
    CHECK(table.resolve("g") == g);        // outer g again

    // TypeInfo equality.
    cider::TypeInfo a{"int", 0};
    cider::TypeInfo b{"int", 0};
    cider::TypeInfo c{"int", 1};
    CHECK(a == b && a != c);
    return 0;
}

int main() { return run(); }
