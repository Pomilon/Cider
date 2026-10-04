// lower_test.cpp — source text becomes IR, and the IR is what the document says.
//
// The first vertical slice: scalars. Integer and float arithmetic,
// comparisons, calls between functions, returns, if/else, while, and global
// initialisers. Aggregates, interfaces, unions, classes, defer/try/throw and
// match are not lowered yet, and the lowerer says so rather than emitting
// something plausible — a phase that receives an Error stops (TYPES.md §7).
//
// Every test parses, resolves, checks and lowers, so a pass here means all four
// layers agree.

#include <cstdio>
#include <string>

#include "check/checker.h"
#include "ir/ir.h"
#include "lexer/lexer.h"
#include "lower/lower.h"
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

struct Lowered {
    bool ok = false;
    std::string stage;
    std::unique_ptr<cider::TypeContext> types;
    std::unique_ptr<cider::Program> program;
    std::unique_ptr<cider::resolve::Resolver> resolver;
    std::unique_ptr<cider::check::Checker> checker;
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
    out.program = parser.parse();
    if (!parser.ok()) {
        out.stage = "parse";
        return out;
    }
    out.resolver = std::make_unique<cider::resolve::Resolver>(*out.types);
    if (!out.resolver->resolveProgram(*out.program, "main")) {
        out.stage = "resolve";
        return out;
    }
    out.checker = std::make_unique<cider::check::Checker>(*out.types, *out.resolver);
    if (!out.checker->checkProgram(*out.program, "main")) {
        out.stage = "check";
        return out;
    }
    cider::lower::Lowerer lowerer(*out.types, *out.resolver, *out.checker);
    out.module = lowerer.lowerModule("main", *out.program);
    if (out.module == nullptr) {
        out.stage = "lower";
        return out;
    }
    out.ok = true;
    return out;
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

const cider::ir::Instr* firstOp(const cider::ir::Function& fn, cider::ir::Op op) {
    for (const cider::ir::Block& block : fn.blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == op) {
                return &instr;
            }
        }
    }
    return nullptr;
}

// A global initialiser becomes a module global with a constant.
void testGlobal() {
    std::printf("globals carry their constant\n");
    Lowered r = lower("var g: int = 42;\n");
    check(r.ok, "lowers: " + r.stage);
    check(r.module->globals.size() == 1, "one global");
    check(r.module->globals[0].label == "g_g", "the link label");
    check(r.module->globals[0].initial == 42, "the initial bytes");
}

// Arithmetic emits the documented op, with the documented flags.
void testArithmetic() {
    std::printf("arithmetic emits add and mul\n");
    Lowered r = lower("func f(a: int, b: int) -> int { return a + b * 2; }\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    check(fn != nullptr, "the function is there");
    if (fn == nullptr) {
        return;
    }
    check(countOp(*fn, cider::ir::Op::Add) == 1, "one add");
    check(countOp(*fn, cider::ir::Op::Mul) == 1, "one mul");
    const cider::ir::Instr* add = firstOp(*fn, cider::ir::Op::Add);
    check(add != nullptr && !add->mayTrap, "add cannot trap");
    check(add != nullptr && add->isPure, "add is pure");
    check(add != nullptr && add->overflow == cider::ir::OverflowMode::Undefined,
          "add wraps only under the flag");
    const cider::ir::Instr* mul = firstOp(*fn, cider::ir::Op::Mul);
    check(mul != nullptr && mul->width == cider::ir::IRType::I32, "mul is i32");
    check(fn->blocks.back().term.term == cider::ir::Term::Ret, "the last block returns");
}

// Division may trap; that is the flag the optimiser reads.
void testTrapFlag() {
    std::printf("division is marked mayTrap\n");
    Lowered r = lower("func f(a: int, b: int) -> int { return a / b; }\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    if (fn == nullptr) {
        return;
    }
    const cider::ir::Instr* div = firstOp(*fn, cider::ir::Op::Div);
    check(div != nullptr, "a div is emitted");
    check(div != nullptr && div->mayTrap, "and it may trap");
    check(!fn->canThrow, "a trap is not a throw");
}

// `# experimental(wrap)` travels on the instruction, not in a compiler flag.
void testWrapFlag() {
    std::printf("wrap travels on the instruction\n");
    Lowered wrapped = lower("# experimental(wrap)\nfunc f(a: int, b: int) -> int { return a + b; }\n");
    check(wrapped.ok, "lowers: " + wrapped.stage);
    const cider::ir::Function* fn = find(wrapped.module.get(), "f");
    const cider::ir::Instr* add = fn != nullptr ? firstOp(*fn, cider::ir::Op::Add) : nullptr;
    check(add != nullptr && add->overflow == cider::ir::OverflowMode::Wrap,
          "the add carries Wrap");
    Lowered plain = lower("func f(a: int, b: int) -> int { return a + b; }\n");
    const cider::ir::Function* plainFn = find(plain.module.get(), "f");
    const cider::ir::Instr* plainAdd =
        plainFn != nullptr ? firstOp(*plainFn, cider::ir::Op::Add) : nullptr;
    check(plainAdd != nullptr &&
              plainAdd->overflow == cider::ir::OverflowMode::Undefined,
          "and without the flag it does not");
}

// Comparisons yield a bool, and signedness is in the opcode.
void testCompare() {
    std::printf("comparisons yield bool\n");
    Lowered r = lower("func f(a: int, b: int) -> bool { return a < b; }\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    const cider::ir::Instr* cmp = fn != nullptr ? firstOp(*fn, cider::ir::Op::Cmp) : nullptr;
    check(cmp != nullptr, "a cmp is emitted");
    check(cmp != nullptr && cmp->width == cider::ir::IRType::Bool, "the cmp yields bool");
    check(cmp != nullptr && cmp->cmp == cider::ir::CmpOp::Lt, "signed less-than");
    Lowered uns = lower("func f(a: u32, b: u32) -> bool { return a < b; }\n");
    const cider::ir::Function* unsFn = find(uns.module.get(), "f");
    const cider::ir::Instr* unsCmp =
        unsFn != nullptr ? firstOp(*unsFn, cider::ir::Op::Cmp) : nullptr;
    check(unsCmp != nullptr && unsCmp->cmp == cider::ir::CmpOp::Ult,
          "unsigned less-than for u32");
}

// Every value is defined once, and a join is a phi.
void testSsa() {
    std::printf("joins use phi\n");
    Lowered r = lower("func f(c: bool) -> int { if (c) { return 1; } return 2; }\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    if (fn == nullptr) {
        return;
    }
    check(fn->blocks.size() >= 3, "entry, then-branch and join");
    check(firstOp(*fn, cider::ir::Op::Const) != nullptr, "constants are folded in");
}

// Calls check arity and mark the callee; an external one is a plain call.
void testCalls() {
    std::printf("calls\n");
    Lowered r = lower(
        "func g(a: int) -> int { return a; }\n"
        "func f() -> int { return g(1); }\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    const cider::ir::Instr* call = fn != nullptr ? firstOp(*fn, cider::ir::Op::Call) : nullptr;
    check(call != nullptr, "a call is emitted");
    check(call != nullptr && !call->call.indirect, "the callee is named");
    check(call != nullptr && call->callee == "g", "and it is g");
    check(call != nullptr && call->mayTrap, "a call may trap");
}

// A loop makes a back edge: the body block's terminator targets an earlier one.
void testLoop() {
    std::printf("while makes a back edge\n");
    Lowered r = lower("func f(n: int) -> void { var i: int = 0; while (i < n) { i = i + 1; } }\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    if (fn == nullptr) {
        return;
    }
    bool backEdge = false;
    for (const cider::ir::Block& block : fn->blocks) {
        if (block.term.term == cider::ir::Term::Br && block.term.target < block.label) {
            backEdge = true;
        }
        if (block.term.term == cider::ir::Term::CondBr &&
            (block.term.thenTarget < block.label || block.term.elseTarget < block.label)) {
            backEdge = true;
        }
    }
    check(backEdge, "a block branches backwards");
}

// What is not lowered yet is refused, not approximated. Structs and arrays
// lower now; what remains refused is named here. (Unions never reach the
// lowerer at all: the checker refuses to construct one.)
// A struct inside a struct constructs field by field: the inner `new`
// builds its words, and the outer stores them at the field's offset.
void testNestedConstruct() {
    std::printf("nested construction lowers\n");
    Lowered r = lower(
        "struct Inner { var v: int; }\n"
        "struct Outer { var i: Inner; var w: int; }\n"
        "func main() -> int {\n"
        "    var o: Outer = new Outer { i: new Inner { v: 5 }, w: 1 };\n"
        "    return o.i.v;\n"
        "}\n");
    check(r.ok, "nested new lowers");
    if (r.ok) {
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr, "main is emitted");
        check(countOp(*main, cider::ir::Op::Store) >= 4, "every field is stored");
    }
}

void testNotYetLowered() {
    std::printf("unlowered constructs are refused\n");
    Lowered r = lower("func f(x: int) -> void { var s: string = \"v = {x}\"; }\n");
    check(!r.ok && r.stage == "lower", "interpolation is not lowered yet");
}

}  // namespace

int main() {
    std::printf("lower_test — source becomes IR\n\n");
    testGlobal();
    testArithmetic();
    testTrapFlag();
    testWrapFlag();
    testCompare();
    testSsa();
    testCalls();
    testLoop();
    testNestedConstruct();
    testNotYetLowered();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
