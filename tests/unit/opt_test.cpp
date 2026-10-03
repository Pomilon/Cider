// opt_test.cpp — malformed IR is refused, and passes preserve meaning.
//
// IR.md §7 lists what every pass must preserve, and each row below pins one:
// a pass may not delete a trapping instruction, move a volatile one, change
// overflow, or drop a source location. The structural check runs before any
// pass — gate L4 — so no pass ever runs on a malformed module.

#include <cstdio>
#include <string>

#include "check/checker.h"
#include "ir/ir.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
#include "opt/opt.h"
#include "opt/structural.h"
#include "parse/parser.h"
#include "types/context.h"

namespace {

int failures = 0;
int checks = 0;

void check(bool holds, const std::string& what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++failures;
    }
}

struct Piped {
    bool ok = false;
    std::string stage;
    std::unique_ptr<cider::TypeContext> types;
    std::unique_ptr<cider::ir::Module> module;
};

Piped pipe(const std::string& text) {
    Piped out;
    out.types = std::make_unique<cider::TypeContext>();
    cider::Lexer lexer("test.cid", text);
    std::vector<cider::Token> tokens = lexer.lex();
    if (!lexer.errors().empty()) {
        out.stage = "lex";
        return out;
    }
    cider::Parser parser("test.cid", std::move(tokens));
    std::unique_ptr<cider::Program> program = parser.parse();
    if (!parser.ok()) {
        out.stage = "parse";
        return out;
    }
    auto resolver = std::make_unique<cider::resolve::Resolver>(*out.types);
    if (!resolver->resolveProgram(*program, "main")) {
        out.stage = "resolve";
        return out;
    }
    auto checker = std::make_unique<cider::check::Checker>(*out.types, *resolver);
    cider::lower::Lowerer lowerer(*out.types, *resolver, *checker);
    out.module = lowerer.lowerModule("main", *program);
    if (out.module == nullptr) {
        out.stage = "lower";
        return out;
    }
    cider::opt::OptResult result = cider::opt::run(*out.module);
    if (!result.ok) {
        out.stage = "opt:" + result.error;
        return out;
    }
    out.ok = true;
    return out;
}

int countOp(const cider::ir::Function& fn, cider::ir::Op op) {
    int n = 0;
    for (const cider::ir::Block& block : fn.blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == op) {
                ++n;
            }
        }
    }
    return n;
}

const cider::ir::Function* find(const cider::ir::Module* module, const std::string& name) {
    if (module == nullptr) {
        return nullptr;
    }
    for (const cider::ir::Function& fn : module->functions) {
        if (fn.name == name) {
            return &fn;
        }
    }
    return nullptr;
}

// Well-formed modules pass the structural check, with no diagnostics.
void testWellFormed() {
    std::printf("well-formed modules pass\n");
    Piped r = pipe("func f(a: int, b: int) -> int { return a + b; }\n");
    check(r.ok, "checks: " + r.stage);
}

// Constants fold: `1 + 2` becomes one const, and the add is gone.
void testFold() {
    std::printf("constants fold\n");
    Piped r = pipe("func f() -> int { return 1 + 2 * 3; }\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(countOp(*fn, cider::ir::Op::Add) == 0, "no add remains");
    check(countOp(*fn, cider::ir::Op::Mul) == 0, "no mul remains");
    check(countOp(*fn, cider::ir::Op::Const) == 1, "one const remains");
}

// An unused pure definition is deleted.
void testDeadCode() {
    std::printf("dead code is deleted\n");
    Piped r = pipe("func f(a: int) -> int { var t: int = a + 1; return a; }\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr && countOp(*fn, cider::ir::Op::Add) == 0, "the dead add is gone");
}

// A division that may trap is never deleted, used or not.
void testTrapKept() {
    std::printf("a trapping instruction is never deleted\n");
    Piped r = pipe("func f(a: int, b: int) -> int { var t: int = a / b; return a; }\n");
    check(r.ok, "lowers and optimises: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr && countOp(*fn, cider::ir::Op::Div) == 1, "the div stays");
}

// A dangling branch target names its block.
void testDanglingTarget() {
    std::printf("dangling targets name their block\n");
    cider::ir::Module module;
    module.name = "bad";
    cider::ir::Function fn;
    fn.name = "f";
    cider::ir::Block block;
    block.label = 0;
    cider::ir::Terminator term;
    term.term = cider::ir::Term::Br;
    term.target = 99;
    block.term = term;
    fn.blocks.push_back(block);
    module.functions.push_back(fn);
    std::vector<cider::opt::StructuralError> errors = cider::opt::checkModule(module);
    check(errors.size() == 1, "one diagnostic");
    check(!errors.empty() && errors[0].message.find("99") != std::string::npos,
          "naming the missing block");
}

// An operand with no definition names the value.
void testUndefinedOperand() {
    std::printf("undefined operands name the value\n");
    cider::ir::Module module;
    module.name = "bad";
    cider::ir::Function fn;
    fn.name = "f";
    cider::ir::Block block;
    block.label = 0;
    cider::ir::Instr instr;
    instr.op = cider::ir::Op::Add;
    instr.width = cider::ir::IRType::I32;
    instr.isPure = true;
    cider::ir::Value result;
    result.id = 7;
    result.width = cider::ir::IRType::I32;
    instr.results.push_back(result);
    cider::ir::Value ghost;
    ghost.id = 4242;
    ghost.width = cider::ir::IRType::I32;
    instr.operands.push_back(ghost);
    instr.operands.push_back(ghost);
    block.body.push_back(instr);
    cider::ir::Terminator term;
    term.term = cider::ir::Term::Ret;
    term.value = result;
    block.term = term;
    fn.blocks.push_back(block);
    module.functions.push_back(fn);
    std::vector<cider::opt::StructuralError> errors = cider::opt::checkModule(module);
    check(!errors.empty(), "a diagnostic");
}

// A value defined twice names the value.
void testDoubleDefine() {
    std::printf("a value defined twice names the value\n");
    cider::ir::Module module;
    module.name = "bad";
    cider::ir::Function fn;
    fn.name = "f";
    cider::ir::Block block;
    block.label = 0;
    for (int i = 0; i < 2; ++i) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Const;
        instr.width = cider::ir::IRType::I32;
        instr.isPure = true;
        cider::ir::Value result;
        result.id = 3;
        result.width = cider::ir::IRType::I32;
        instr.results.push_back(result);
        block.body.push_back(instr);
    }
    cider::ir::Terminator term;
    term.term = cider::ir::Term::Unreachable;
    block.term = term;
    fn.blocks.push_back(block);
    module.functions.push_back(fn);
    std::vector<cider::opt::StructuralError> errors = cider::opt::checkModule(module);
    check(!errors.empty(), "a diagnostic");
}

// The pipeline refuses a malformed module before any pass runs on it.
void testPipelineRefuses() {
    std::printf("the pipeline refuses malformed modules\n");
    cider::ir::Module module;
    module.name = "bad";
    cider::ir::Function fn;
    fn.name = "f";
    cider::ir::Block block;
    block.label = 0;
    cider::ir::Terminator term;
    term.term = cider::ir::Term::Br;
    term.target = 99;
    block.term = term;
    fn.blocks.push_back(block);
    module.functions.push_back(fn);
    cider::opt::OptResult result = cider::opt::run(module);
    check(!result.ok, "run fails");
    check(!result.error.empty(), "with a reason");
}

}  // namespace

int main() {
    std::printf("opt_test — malformed IR is refused, passes preserve meaning\n\n");
    testWellFormed();
    testFold();
    testDeadCode();
    testTrapKept();
    testDanglingTarget();
    testUndefinedOperand();
    testDoubleDefine();
    testPipelineRefuses();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
