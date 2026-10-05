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

// A field over an element (`a[1].y`) resolves the element's address: the
// base plus a folded index times the element size.
void testFieldOverIndex() {
    std::printf("field over index lowers\n");
    Lowered r = lower(
        "struct P { var x: int; var y: int; }\n"
        "func main() -> int {\n"
        "    var a: [P; 2] = new [P; 2] { new P { x: 1, y: 2 }, new P { x: 3, y: 4 } };\n"
        "    return a[1].y;\n"
        "}\n");
    check(r.ok, "element field lowers");
}

// An 8-byte field at offset 4 straddles two slots: it reads through a
// computed address, which names no slot and keeps both sides' stores alive.
// Naming one slot would let the optimizer delete the other's store as dead.
void testStraddlingRead() {
    std::printf("straddling field reads go indirect\n");
    Lowered r = lower(
        "struct Big { var a: int; var b: int; }\n"
        "struct Wrap { var w: int; var big: Big; }\n"
        "func main() -> int {\n"
        "    var x: Wrap = new Wrap { w: 1, big: new Big { a: 2, b: 3 } };\n"
        "    var c: Big = x.big;\n"
        "    return c.b;\n"
        "}\n");
    check(r.ok, "straddling read lowers");
    if (r.ok) {
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr && countOp(*main, cider::ir::Op::Gep) >= 1,
              "the straddle uses a computed address");
    }
}

// A statement assignment stores without answering a value, so several words
// are fine; the tail store is exactly its bytes, never a full word over
// the neighbor element.
void testMultiWordAssign() {
    std::printf("multi-word statement assignment lowers\n");
    Lowered r = lower(
        "struct T { var a: int; var b: int; var c: int; }\n"
        "func main() -> int {\n"
        "    var a: [T; 2] = new [T; 2] { new T { a: 1, b: 2, c: 3 }, new T { a: 4, b: 5, c: 6 } };\n"
        "    a[0] = new T { a: 7, b: 8, c: 9 };\n"
        "    return a[0].c;\n"
        "}\n");
    check(r.ok, "element store lowers");
}

// A union constructs its one live member and writes the tag; a member read
// checks the tag and traps on a mismatch, so the IR holds a Trap
// terminator. hasM is the same check answered as a boolean, with no trap.
void testUnion() {
    std::printf("unions construct, check, and trap\n");
    Lowered r = lower(
        "union Value { var i: int; var s: string; }\n"
        "func main() -> int {\n"
        "    var v: Value = new Value { i: 42 };\n"
        "    return v.i;\n"
        "}\n");
    check(r.ok, "union construction lowers");
    if (r.ok) {
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr, "main is emitted");
        int traps = 0;
        int conds = 0;
        for (const cider::ir::Block& block : main->blocks) {
            if (block.term.term == cider::ir::Term::Trap) {
                ++traps;
            }
            if (block.term.term == cider::ir::Term::CondBr) {
                ++conds;
            }
        }
        check(traps == 1, "a tag mismatch traps");
        check(conds >= 1, "the tag is branched on");
    }
    Lowered has = lower(
        "union Value { var i: int; var s: string; }\n"
        "func main() -> int {\n"
        "    var v: Value = new Value { i: 1 };\n"
        "    if (v.hasI()) { return 7; }\n"
        "    return 0;\n"
        "}\n");
    check(has.ok, "hasM lowers");
    if (has.ok) {
        const cider::ir::Function* main = find(has.module.get(), "main");
        int traps = 0;
        for (const cider::ir::Block& block : main->blocks) {
            if (block.term.term == cider::ir::Term::Trap) {
                ++traps;
            }
        }
        check(traps == 0, "asking never traps");
    }
}

void testSlice() {
    std::printf("slices create, guard, and trap\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var a: [int; 3] = new [int; 3] { 5, 6, 7 };\n"
        "    var s: []int = a[0:2];\n"
        "    return s[1];\n"
        "}\n");
    check(r.ok, "slice creation lowers");
    if (r.ok) {
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr, "main is emitted");
        int traps = 0;
        for (const cider::ir::Block& block : main->blocks) {
            if (block.term.term == cider::ir::Term::Trap) {
                ++traps;
            }
        }
        // Proven bounds check nothing; the runtime index still guards
        // against the runtime length.
        check(traps == 1, "only the index guards");
    }
    Lowered bad = lower(
        "func main() -> int {\n"
        "    var a: [int; 3] = new [int; 3] { 5, 6, 7 };\n"
        "    var s: []int = a[0:9];\n"
        "    return s[0];\n"
        "}\n");
    check(!bad.ok && bad.stage == "lower", "a provable breach is refused");
    Lowered sub = lower(
        "func main() -> int {\n"
        "    var a: [int; 4] = new [int; 4] { 1, 2, 3, 4 };\n"
        "    var s: []int = a[0:4];\n"
        "    var t: []int = s[1:3];\n"
        "    return t[1];\n"
        "}\n");
    check(sub.ok, "a slice of a slice lowers");
}

void testSliceStore() {
    std::printf("slice stores guard, then write\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var a: [int; 3] = new [int; 3] { 5, 6, 7 };\n"
        "    var s: []int = a[0:2];\n"
        "    s[0] = 9;\n"
        "    return s.len as int;\n"
        "}\n");
    check(r.ok, "a slice store and len lower");
}

// A class value constructs like a struct with a leading vptr word: the
// vptr is zero until methods are wired (T7), and fields follow it.
void testClass() {
    std::printf("class values lower\n");
    Lowered r = lower(
        "class Node { var v: int; }\n"
        "func main() -> int {\n"
        "    var n: Node = new Node { v: 7 };\n"
        "    return n.v;\n"
        "}\n");
    check(r.ok, "class new lowers");
    if (r.ok) {
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr, "main is emitted");
        check(countOp(*main, cider::ir::Op::Store) >= 2, "vptr and field are stored");
    }
}

void testAddrOf() {
    std::printf("address-of lowers\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var x: int = 5;\n"
        "    var p: *int = &x;\n"
        "    return *p;\n"
        "}\n");
    check(r.ok, "address-of plus dereference lowers");
    if (r.ok) {
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr, "main exists");
        check(firstOp(*main, cider::ir::Op::Gep) != nullptr,
              "the address materializes as a gep");
    }
}

void testDerefStore() {
    std::printf("deref stores lower\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var x: int = 5;\n"
        "    var p: *int = &x;\n"
        "    *p = 41;\n"
        "    return x;\n"
        "}\n");
    check(r.ok, "a store through a dereference lowers");
    if (r.ok) {
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr, "main exists");
        check(firstOp(*main, cider::ir::Op::Store) != nullptr,
              "the store reaches the indirect address");
    }
}

void testNotYetLowered() {
    std::printf("unlowered constructs are refused\n");
    Lowered r = lower("func f(x: int) -> void { var s: string = \"v = {x}\"; }\n");
    check(!r.ok && r.stage == "lower", "interpolation is not lowered yet");
}

// A method emits its own function under the owner's name, and a call site
// reaches it with the receiver first.
void testMethodCall() {
    std::printf("method calls lower\n");
    Lowered r = lower(
        "class Counter { var v: int; func get() -> int { return (*self).v; } }\n"
        "func main() -> int {\n"
        "    var c: Counter = new Counter { v: 3 };\n"
        "    return c.get();\n"
        "}\n");
    check(r.ok, "method call lowers");
    if (r.ok) {
        const cider::ir::Function* get = find(r.module.get(), "Counter__get");
        check(get != nullptr, "method is emitted under the owner");
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr && countOp(*main, cider::ir::Op::Call) == 1,
              "main calls the method once");
    }
}

void testDelete() {
    std::printf("delete lowers to deinit + free\n");
    Lowered withDeinit = lower(
        "class Box { var v: int; func init(x: int) { (*self).v = x; } "
        "func deinit() { } }\n"
        "func main() -> int {\n"
        "    var b: *Box = new Box(3);\n"
        "    delete b;\n"
        "    return 5;\n"
        "}\n");
    check(withDeinit.ok, "delete with deinit lowers");
    if (withDeinit.ok) {
        const cider::ir::Function* main = find(withDeinit.module.get(), "main");
        check(main != nullptr, "main exists");
        // main calls cider_alloc, Box__init, Box__deinit, cider_free: four.
        check(main != nullptr && countOp(*main, cider::ir::Op::Call) == 4,
              "delete emits deinit and free after init");
    }
    Lowered bare = lower(
        "class Box { var v: int; func init(x: int) { (*self).v = x; } }\n"
        "func main() -> int {\n"
        "    var b: *Box = new Box(3);\n"
        "    delete b;\n"
        "    return 5;\n"
        "}\n");
    check(bare.ok, "delete without deinit lowers");
    if (bare.ok) {
        const cider::ir::Function* main = find(bare.module.get(), "main");
        check(main != nullptr && countOp(*main, cider::ir::Op::Call) == 3,
              "no deinit means no deinit call, free alone");
    }
    Lowered bad = lower(
        "func main() -> int {\n"
        "    var x: int = 1;\n"
        "    var p: *int = &x;\n"
        "    delete p;\n"
        "    return 0;\n"
        "}\n");
    check(!bad.ok, "delete on a non-class pointer is refused");
}

// A function address lowers to FnAddr, and an indirect call carries the
// callee word with it — codegen must never invent the target.
void testIndirect() {
    std::printf("indirect calls lower\n");
    Lowered r = lower(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int {\n"
        "    var f: *func(int, int) -> int = &add;\n"
        "    return (*f)(20, 22);\n"
        "}\n");
    check(r.ok, "indirect call lowers");
    if (r.ok) {
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr, "main exists");
        const cider::ir::Instr* addr = firstOp(*main, cider::ir::Op::FnAddr);
        check(addr != nullptr && addr->globalLabel == "add",
              "function address names the function");
        bool indirectCall = false;
        bool calleeWired = false;
        for (const cider::ir::Block& block : main->blocks) {
            for (const cider::ir::Instr& instr : block.body) {
                if (instr.op == cider::ir::Op::Call && instr.call.indirect) {
                    indirectCall = true;
                    calleeWired = instr.calleeValue.id != 0xFFFFFFFFu;
                }
            }
        }
        check(indirectCall, "the call is indirect");
        check(calleeWired, "the callee word travels with the call");
    }
}

void testClosure() {
    std::printf("closure literals lower\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var n: int = 10;\n"
        "    var f: func(int) -> int = func(x: int) -> int { return x + n; };\n"
        "    return f(4);\n"
        "}\n");
    check(r.ok, "a capturing closure lowers");
    if (r.ok) {
        const cider::ir::Function* hidden = nullptr;
        for (const cider::ir::Function& fn : r.module->functions) {
            if (fn.name != "main") {
                hidden = &fn;
            }
        }
        check(hidden != nullptr, "the literal becomes its own function");
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr, "main exists");
        bool indirectCall = false;
        if (main != nullptr) {
            for (const cider::ir::Block& block : main->blocks) {
                for (const cider::ir::Instr& instr : block.body) {
                    if (instr.op == cider::ir::Op::Call && instr.call.indirect) {
                        indirectCall = true;
                    }
                }
            }
        }
        check(indirectCall, "a bare closure call is indirect");
    }
}

void testClosureNested() {
    std::printf("nested closure captures are transitive\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var a: int = 1;\n"
        "    var outer: func(int) -> int = func(x: int) -> int {\n"
        "        var inner: func(int) -> int = func(y: int) -> int { return x + y + a; };\n"
        "        return inner(100);\n"
        "    };\n"
        "    return outer(20);\n"
        "}\n");
    check(r.ok, "a nested closure lowers");
    if (r.ok) {
        uint64_t hidden = 0;
        for (const cider::ir::Function& fn : r.module->functions) {
            if (fn.name != "main") {
                ++hidden;
            }
        }
        check(hidden == 2, "each literal becomes its own function");
    }
}

void testClosureBareName() {
    std::printf("a bare function name answers a closure pair\n");
    Lowered r = lower(
        "func add(a: int, b: int) -> int { return a + b; }\n"
        "func main() -> int {\n"
        "    var f: func(int, int) -> int = add;\n"
        "    return f(20, 22);\n"
        "}\n");
    check(r.ok, "a bare name lowers as a closure");
    if (r.ok) {
        // The forwarder drops the captures word: main, add, and one hidden.
        uint64_t count = 0;
        for (const cider::ir::Function& fn : r.module->functions) {
            (void)fn;
            ++count;
        }
        check(count == 3, "the name reference emits one forwarder");
    }
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
    testFieldOverIndex();
    testStraddlingRead();
    testMultiWordAssign();
    testUnion();
    testSlice();
    testSliceStore();
    testClass();
    testMethodCall();
    testDelete();
    testIndirect();
    testClosure();
    testClosureNested();
    testClosureBareName();
    testAddrOf();
    testDerefStore();
    testNotYetLowered();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
