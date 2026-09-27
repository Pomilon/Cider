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

    // CID-006: fn(...) types carry their signature; equality is exact.
    auto fnAst = std::make_unique<cider::TypeAST>("fn", 0, 1, 1);
    std::vector<std::unique_ptr<cider::TypeAST>> fnParams;
    fnParams.push_back(std::make_unique<cider::TypeAST>("int", 0, 1, 1));
    fnParams.push_back(std::make_unique<cider::TypeAST>("int", 0, 1, 1));
    fnAst->setFunctionSignature(std::move(fnParams),
                                std::make_unique<cider::TypeAST>("int", 0, 1, 1));
    CHECK(fnAst->isFunctionType());
    CHECK(fnAst->getFnParamTypes().size() == 2);
    CHECK(fnAst->getFnReturnType() != nullptr);

    cider::TypeInfo f1 = cider::TypeInfo::fromAST(fnAst.get());
    CHECK(f1.isFunctionPointer);
    CHECK(f1.fnParamTypes && f1.fnParamTypes->size() == 2);
    CHECK(f1.toString() == "fn(int, int) -> int");
    CHECK((f1 != cider::TypeInfo{"int", 0}));
    CHECK((f1 != cider::TypeInfo{"void", 1}));

    cider::TypeInfo f1copy = f1;
    CHECK(f1 == f1copy);

    auto fnAst2 = std::make_unique<cider::TypeAST>("fn", 0, 1, 1);
    std::vector<std::unique_ptr<cider::TypeAST>> fnParams2;
    fnParams2.push_back(std::make_unique<cider::TypeAST>("int", 0, 1, 1));
    fnAst2->setFunctionSignature(std::move(fnParams2),
                                 std::make_unique<cider::TypeAST>("int", 0, 1, 1));
    cider::TypeInfo f2 = cider::TypeInfo::fromAST(fnAst2.get());
    CHECK(f1 != f2);
    CHECK(f2.toString() == "fn(int) -> int");

    // Array of function pointers keeps the element signature and the size.
    auto fnBase = std::make_unique<cider::TypeAST>("fn", 0, 1, 1);
    std::vector<std::unique_ptr<cider::TypeAST>> fnBaseParams;
    fnBaseParams.push_back(std::make_unique<cider::TypeAST>("int", 0, 1, 1));
    fnBase->setFunctionSignature(std::move(fnBaseParams),
                                 std::make_unique<cider::TypeAST>("int", 0, 1, 1));
    auto fnArr = std::make_unique<cider::ArrayTypeAST>(std::move(fnBase), 2, 1, 1);
    cider::TypeInfo fa = cider::TypeInfo::fromAST(fnArr.get());
    CHECK(fa.isArray && fa.isFunctionPointer && fa.arraySize == 2);
    CHECK(fa.toString() == "[fn(int) -> int; 2]");

    cider::TypeInfo fa3 = fa;
    fa3.arraySize = 3;
    CHECK(fa != fa3);

    // CID-005: ABI is part of fn type identity; Default == SysV.
    cider::TypeInfo abl;
    abl.name = "fn"; abl.isFunctionPointer = true;
    abl.fnParamTypes = std::make_shared<std::vector<cider::TypeInfo>>();
    abl.fnReturnType = std::make_shared<cider::TypeInfo>(cider::TypeInfo{"u64", 0});
    cider::TypeInfo ablMs = abl; ablMs.isMsAbi = true;
    CHECK(abl != ablMs);
    CHECK(abl.toString() == "fn() -> u64");
    CHECK(ablMs.toString() == "msabi fn() -> u64");

    // CIDER-BUG-044: symbols carry stable unique ids; the defining-module
    // provenance maps key by id, so a freed symbol's address reuse cannot
    // leak a stale module into a fresh symbol.
    {
        auto s1 = std::make_shared<cider::Symbol>();
        auto s2 = std::make_shared<cider::Symbol>();
        CHECK(s1->id != 0 && s2->id != 0 && s1->id != s2->id);
        auto s3 = std::make_shared<cider::Symbol>();
        CHECK(s3->id != s1->id && s3->id != s2->id);
    }
    return 0;
}

int main() { return run(); }
