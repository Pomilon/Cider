#include <cstdio>
#include <memory>
#include <vector>
#include "ast.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

struct CountVisitor : cider::ASTVisitor {
    int exprs = 0;
    int stmts = 0;
    void visit(cider::NumberExpr&) override { exprs++; }
    void visit(cider::IdentifierExpr&) override { exprs++; }
    void visit(cider::ReturnStmt& r) override {
        stmts++;
        if (r.getValue()) r.getValue()->accept(*this);
    }
    void visit(cider::FunctionDeclStmt& f) override {
        stmts++;
        for (auto& s : f.getBody()) s->accept(*this);
    }
    void visit(cider::BinaryExpr& e) override {
        exprs++;
        e.getLeft()->accept(*this);
        e.getRight()->accept(*this);
    }
    void visit(cider::AstProgram& p) override {
        for (auto& s : p.getStatements()) s->accept(*this);
    }
};

int run() {
    // func f() -> int { return 1 + x; }
    cider::AstProgram prog;
    std::vector<cider::FunctionDeclStmt::Parameter> params;
    std::vector<std::unique_ptr<cider::Statement>> body;
    body.push_back(std::make_unique<cider::ReturnStmt>(
        std::make_unique<cider::BinaryExpr>(
            std::make_unique<cider::NumberExpr>(1, 1, 1), "+",
            std::make_unique<cider::IdentifierExpr>("x", 1, 1), 1, 1), 1, 1));
    prog.addStatement(std::make_unique<cider::FunctionDeclStmt>(
        "f", std::move(params), std::move(body),
        std::make_unique<cider::TypeAST>("int", 0, 1, 1), 1, 1));

    CountVisitor v;
    prog.accept(v);
    CHECK(v.stmts == 2);
    CHECK(v.exprs == 3);   // Binary + Number + Ident
    return 0;
}
int main() { return run(); }
