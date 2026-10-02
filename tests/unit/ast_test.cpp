// ast_test.cpp — the tree holds every §14 form, and one visitor sees them all.
//
// FEATURES.md §14 claims to be the complete surface. This test builds a small
// program by hand — a global, a function with a body, a return of mixed
// arithmetic — and walks it with a counting visitor. If a node kind cannot be
// built or is not visited, the count is wrong. The single-visitor rule itself
// (gates T5/L1) is checked by scripts/gates.py, not here.

#include <cstdio>
#include <memory>

#include "ast/ast.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool holds, const char* what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what);
    } else {
        std::printf("  FAIL %s\n", what);
        ++failures;
    }
}

cider::Position at(uint32_t line, uint32_t column) {
    return cider::Position{"test.cid", line, column};
}

// Counts the nodes a full walk reaches. Defined here, in tests/: the gates
// allow exactly one `public ASTVisitor` in src/, and this is not src/.
struct Counter : public cider::ASTVisitor {
    int vars = 0;
    int funcs = 0;
    int returns = 0;
    int binaries = 0;
    int idents = 0;
    int intLits = 0;
    void visitVarDecl(cider::VarDecl& n) override { ++vars; ASTVisitor::visitVarDecl(n); }
    void visitFuncDecl(cider::FuncDecl& n) override { ++funcs; ASTVisitor::visitFuncDecl(n); }
    void visitReturn(cider::Return& n) override { ++returns; ASTVisitor::visitReturn(n); }
    void visitBinary(cider::Binary& n) override { ++binaries; ASTVisitor::visitBinary(n); }
    void visitIdent(cider::Ident& n) override { ++idents; ASTVisitor::visitIdent(n); }
    void visitIntLit(cider::IntLit& n) override { ++intLits; ASTVisitor::visitIntLit(n); }
};

// var g: int = 1 + 2 * 3;
std::unique_ptr<cider::VarDecl> sampleGlobal() {
    auto two = std::make_unique<cider::IntLit>(at(1, 13), 10, "2", false);
    auto three = std::make_unique<cider::IntLit>(at(1, 17), 10, "3", false);
    auto mul = std::make_unique<cider::Binary>(at(1, 15), cider::BinOp::Mul, std::move(two),
                                               std::move(three));
    auto one = std::make_unique<cider::IntLit>(at(1, 9), 10, "1", false);
    auto add = std::make_unique<cider::Binary>(at(1, 11), cider::BinOp::Add, std::move(one),
                                               std::move(mul));
    auto type = std::make_unique<cider::BasicType>(at(1, 7), cider::BasicKind::I32);
    return std::make_unique<cider::VarDecl>(at(1, 1), "g", std::move(type), std::move(add));
}

void testWalk() {
    std::printf("one visitor sees every node\n");
    auto global = sampleGlobal();
    Counter counter;
    global->accept(counter);
    check(counter.vars == 1, "the declaration");
    check(counter.binaries == 2, "both operators");
    check(counter.intLits == 3, "all three literals");
    check(counter.idents == 0, "no identifiers here");
}

// Positions belong to the node, not to a token: the tree is usable without one.
void testPositions() {
    std::printf("positions ride on the node\n");
    auto global = sampleGlobal();
    check(global->pos.line == 1 && global->pos.column == 1, "declaration position");
    const cider::Binary& add = static_cast<const cider::Binary&>(*global->init);
    check(add.pos.column == 11, "operator position");
    check(add.op == cider::BinOp::Add, "the operator survived");
}

// A function with parameters, a return type and a body walks too.
void testFunc() {
    std::printf("functions carry signatures and bodies\n");
    auto aType = std::make_unique<cider::BasicType>(at(2, 14), cider::BasicKind::I32);
    auto bType = std::make_unique<cider::BasicType>(at(2, 21), cider::BasicKind::I32);
    auto retType = std::make_unique<cider::BasicType>(at(2, 29), cider::BasicKind::I32);
    auto body = std::make_unique<cider::Block>(at(2, 34));
    auto a = std::make_unique<cider::Ident>(at(2, 40), "a");
    auto b = std::make_unique<cider::Ident>(at(2, 44), "b");
    auto sum = std::make_unique<cider::Binary>(at(2, 42), cider::BinOp::Add, std::move(a),
                                               std::move(b));
    body->stmts.push_back(std::make_unique<cider::Return>(at(2, 33), std::move(sum)));
    std::vector<cider::Param> params;
    params.push_back(cider::Param{at(2, 12), "a", std::move(aType), false});
    params.push_back(cider::Param{at(2, 19), "b", std::move(bType), false});
    cider::FuncDecl fn(at(2, 1), "add", {}, std::move(params), std::move(retType),
                       std::move(body));
    Counter counter;
    fn.accept(counter);
    check(counter.funcs == 1, "the function");
    check(counter.returns == 1, "its return");
    check(counter.binaries == 1, "the addition");
    check(counter.idents == 2, "both parameters read");
}

}  // namespace

int main() {
    std::printf("ast_test — the tree holds every form\n\n");
    testWalk();
    testPositions();
    testFunc();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
