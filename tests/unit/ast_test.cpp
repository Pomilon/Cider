#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "ast.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int run() {
    cider::AstProgram prog;
    std::vector<cider::FunctionDeclStmt::Parameter> params;
    params.emplace_back("a", std::make_unique<cider::TypeAST>("int", 0, 1, 1));
    auto fn = std::make_unique<cider::FunctionDeclStmt>(
        "main",
        std::move(params),
        [] {
            std::vector<std::unique_ptr<cider::Statement>> body;
            auto sum = std::make_unique<cider::BinaryExpr>(
                std::make_unique<cider::IdentifierExpr>("a", 3, 12), "+",
                std::make_unique<cider::NumberExpr>(2, 3, 16), 3, 12);
            body.push_back(std::make_unique<cider::ReturnStmt>(std::move(sum), 3, 5));
            return body;
        }(),
        std::make_unique<cider::TypeAST>("int", 0, 1, 1), 1, 1);
    prog.addStatement(std::move(fn));

    std::string d = cider::dump(prog);
    CHECK(d.find("(FunctionDecl main") != std::string::npos);
    CHECK(d.find("(params (a int))") != std::string::npos);
    CHECK(d.find("(returns int)") != std::string::npos);
    CHECK(d.find("(Return (Binary + (Ident a) (Number 2)))") != std::string::npos);

    // Type dumps: pointers and arrays.
    cider::TypeAST t1("char", 2, 1, 1);
    std::string td;
    t1.dump(td, 0);
    CHECK(td == "char**");

    auto arr = std::make_unique<cider::ArrayTypeAST>(
        std::make_unique<cider::TypeAST>("int", 0, 1, 1), 3, 1, 1);
    td.clear();
    arr->dump(td, 0);
    CHECK(td == "int[3]");

    // Statement forms dump inline.
    auto blk = std::make_unique<cider::BlockStmt>(
        [] {
            std::vector<std::unique_ptr<cider::Statement>> stmts;
            stmts.push_back(std::make_unique<cider::BreakStmt>(2, 3));
            return stmts;
        }(), 2, 1);
    std::string bd;
    blk->dump(bd, 0);
    CHECK(bd == "(Block (Break))");

    // String escapes round-trip through the dump.
    auto str = std::make_unique<cider::StringExpr>("a\nb\"c\\", 1, 1);
    std::string sd;
    str->dump(sd, 0);
    CHECK(sd == "(String \"a\\nb\\\"c\\\\\")");

    // CID-005: TypeAST carries an ABI, Default by default.
    cider::TypeAST abiType("fn", 0, 1, 1);
    CHECK(abiType.getAbi() == cider::Abi::Default);
    abiType.setAbi(cider::Abi::Ms);
    CHECK(abiType.getAbi() == cider::Abi::Ms);
    cider::TypeAST defaultType("int", 0, 1, 1);
    CHECK(defaultType.getAbi() == cider::Abi::Default);

    // CID-005: an explicit ABI renders in the fn dump.
    std::vector<std::unique_ptr<cider::TypeAST>> abiParams;
    abiParams.push_back(std::make_unique<cider::TypeAST>("int", 0, 1, 1));
    abiType.setFunctionSignature(std::move(abiParams),
                                 std::make_unique<cider::TypeAST>("u64", 0, 1, 1));
    std::string abiDump;
    abiType.dump(abiDump, 0);
    CHECK(abiDump.find("msabi fn(") != std::string::npos);
    return 0;
}

int main() { return run(); }
