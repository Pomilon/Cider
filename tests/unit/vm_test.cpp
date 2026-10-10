// vm_test.cpp — the interpreter answers without a toolchain.
//
// The first VM slice: source text lowers to an IR module, the interpreter
// runs main, and the exit code is the program's answer. No assembler, no
// linker, no fork. Both legs are reachable here: the unoptimised module the
// lowerer produced, and the same module through the optimiser.

#include <cstdio>
#include <string>

#include "check/checker.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
#include "opt/opt.h"
#include "parse/parser.h"
#include "types/context.h"
#include "vm/vm.h"

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

struct Lowered {
    bool ok = false;
    std::string stage;
    std::unique_ptr<cider::TypeContext> types;
    std::unique_ptr<cider::ir::Module> module;
};

Lowered lower(const std::string& text) {
    Lowered out;
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
    if (!lowerer.lowerModule("main", *program)) {
        out.stage = "lower";
        return out;
    }
    out.module = lowerer.takeModule();
    out.ok = true;
    return out;
}

// Straight-line integer code answers its exit code on both legs.
void testExit() {
    std::printf("the interpreter answers\n");
    Lowered unopt = lower("func main() -> int { return 40 + 2; }\n");
    check(unopt.ok, "lowers: " + unopt.stage);
    cider::vm::VmResult unr = cider::vm::run(*unopt.module);
    check(unr.ok, "runs unoptimised: " + unr.error);
    check(unr.exitCode == 42, "the unoptimised answer is 42");
    cider::opt::OptResult ores = cider::opt::run(*unopt.module);
    check(ores.ok, "optimises: " + ores.error);
    cider::vm::VmResult opr = cider::vm::run(*unopt.module);
    check(opr.ok, "runs optimised: " + opr.error);
    check(opr.exitCode == 42, "the optimised answer is 42");
}

}  // namespace

int main() {
    std::printf("vm_test — the interpreter answers\n\n");
    testExit();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
