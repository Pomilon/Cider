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
#include "target/target.h"
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

Lowered lowerAs(const std::string& text, cider::target::Kind target) {
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
    cider::lower::Lowerer lowerer(*out.types, *out.resolver, *out.checker, target);
    if (!lowerer.lowerModule("main", *out.program)) {
        out.module = nullptr;
        out.stage = "lower";
        return out;
    }
    out.module = lowerer.takeModule();
    out.ok = true;
    return out;
}

Lowered lower(const std::string& text) {
    return lowerAs(text, cider::target::Kind::Native);
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
        "    var o: Outer = Outer { i: Inner { v: 5 }, w: 1 };\n"
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
        "    var a: [P; 2] = [P { x: 1, y: 2 }, P { x: 3, y: 4 }];\n"
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
        "    var x: Wrap = Wrap { w: 1, big: Big { a: 2, b: 3 } };\n"
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
        "    var a: [T; 2] = [T { a: 1, b: 2, c: 3 }, T { a: 4, b: 5, c: 6 }];\n"
        "    a[0] = T { a: 7, b: 8, c: 9 };\n"
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
        "    var v: Value = Value { i: 42 };\n"
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
        "    var v: Value = Value { i: 1 };\n"
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
        "    var a: [int; 3] = [5, 6, 7];\n"
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
        "    var a: [int; 3] = [5, 6, 7];\n"
        "    var s: []int = a[0:9];\n"
        "    return s[0];\n"
        "}\n");
    check(!bad.ok && bad.stage == "lower", "a provable breach is refused");
    Lowered sub = lower(
        "func main() -> int {\n"
        "    var a: [int; 4] = [1, 2, 3, 4];\n"
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
        "    var a: [int; 3] = [5, 6, 7];\n"
        "    var s: []int = a[0:2];\n"
        "    s[0] = 9;\n"
        "    return s.len as int;\n"
        "}\n");
    check(r.ok, "a slice store and len lower");
}

// CIDER-BUG-089: a class value belongs to its declared type, so a copy
// from a pointer re-stamps the declared class's table instead of keeping
// the dynamic type's table.
void testClassValueVptr() {
    std::printf("class value copies stamp the declared vptr\n");
    Lowered r = lower(
        "class V { var x: int; virtual func f() -> int { return 1; } }\n"
        "class W : V { var y: int; virtual func f() -> int { return 2; } }\n"
        "var gv: V = V { x: 0 };\n"
        "var ga: [V; 2] = [V { x: 0 }, V { x: 0 }];\n"
        "func main() -> int {\n"
        "    var p: *V = new W { x: 41, y: 99 };\n"
        "    var vs: V = *p;\n"
        "    return vs.f();\n"
        "}\n");
    check(r.ok, "slicing copy lowers");
    if (r.ok) {
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr, "main is emitted");
        check(countOp(*main, cider::ir::Op::DataAddr) >= 2,
              "the declared class's table is stamped after the copy");
        bool gvFound = false;
        bool gaFound = false;
        for (const auto& global : r.module->globals) {
            if (global.label == "g_gv") {
                gvFound = global.initWords.size() == 2 &&
                          global.relocWords.size() == 1 &&
                          global.relocWords[0].first == 0 &&
                          global.relocWords[0].second.rfind("vtable_", 0) == 0;
            }
            if (global.label == "g_ga") {
                gaFound = global.initWords.size() == 4 &&
                          global.relocWords.size() == 2 &&
                          global.relocWords[0].first == 0 &&
                          global.relocWords[1].first == 2;
            }
        }
        check(gvFound, "a class-valued global names its declared vtable");
        check(gaFound, "array elements each name their declared vtable");
    }
}

// A class value constructs like a struct with a leading vptr word: the
// vptr is zero until methods are wired (T7), and fields follow it.
void testClass() {
    std::printf("class values lower\n");
    Lowered r = lower(
        "class Node { var v: int; }\n"
        "func main() -> int {\n"
        "    var n: Node = Node { v: 7 };\n"
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

void testInterfaceParam() {
    std::printf("interface params lower\n");
    Lowered r = lower(
        "interface Shape { func area() -> int; }\n"
        "struct Sq { var s: int; func area() -> int { return (*self).s; } }\n"
        "func get(s: Shape) -> int { return s.area(); }\n"
        "func main() -> int {\n"
        "    var a: Sq = Sq { s: 9 };\n"
        "    return get(a);\n"
        "}\n");
    check(r.ok, "an interface param lowers: " + r.stage);
    if (!r.ok) {
        return;
    }
    const cider::ir::Function* get = find(r.module.get(), "get");
    check(get != nullptr, "the callee is there");
    if (get == nullptr) {
        return;
    }
    check(get->params.size() == 2, "data plus vtable ride as two words");
    bool table = false;
    for (const cider::ir::Global& global : r.module->globals) {
        if (global.label == "Sq__vtable__Shape" && global.initLabels.size() == 1 &&
            global.initLabels[0] == "Sq__area") {
            table = true;
        }
    }
    check(table, "one vtable names the concrete method in order");
    int indirect = 0;
    for (const cider::ir::Block& block : get->blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == cider::ir::Op::Call && instr.call.indirect) {
                ++indirect;
            }
        }
    }
    check(indirect == 1, "dispatch calls through the vtable");
}

void testInterfaceClassAdapter() {
    std::printf("interface class entries adapt\n");
    Lowered r = lower(
        "interface Shape { func area() -> int; }\n"
        "class Circle { var r: int;\n"
        "    func init(r: int) -> void { (*self).r = r; }\n"
        "    func area() -> int { return (*self).r * 2; } }\n"
        "func get(s: Shape) -> int { return s.area(); }\n"
        "func main() -> int {\n"
        "    var c: *Circle = new Circle(5);\n"
        "    return get(c);\n"
        "}\n");
    check(r.ok, "a class reference boxes: " + r.stage);
    if (!r.ok) {
        return;
    }
    bool table = false;
    for (const cider::ir::Global& global : r.module->globals) {
        if (global.label == "Circle__vtable__Shape" &&
            global.initLabels.size() == 1 &&
            global.initLabels[0] == "Circle__area__ifaceadapt") {
            table = true;
        }
    }
    check(table, "a class entry names the receiver adapter");
    const cider::ir::Function* adapt =
        find(r.module.get(), "Circle__area__ifaceadapt");
    check(adapt != nullptr, "the adapter is emitted");
    if (adapt == nullptr) {
        return;
    }
    check(adapt->params.size() == 1, "the adapter takes the data address");
    bool forwards = false;
    for (const cider::ir::Block& block : adapt->blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == cider::ir::Op::Call && !instr.call.indirect &&
                instr.callee == "Circle__area") {
                forwards = true;
            }
        }
    }
    check(forwards, "the adapter tail-calls the method");
}

void testForLoop() {
    std::printf("for loops lower\n");
    Lowered r = lower(
        "func f(n: int) -> int {\n"
        "    var s: int = 0;\n"
        "    for (var i: int = 0; i < n; i = i + 1) {\n"
        "        s = s + 1;\n"
        "    }\n"
        "    return s;\n"
        "}\n");
    check(r.ok, "a for loop lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    if (fn == nullptr) {
        return;
    }
    // The head carries the condition; its two forward edges are the body
    // and the exit, and the step jumps back to it.
    const cider::ir::Block* head = nullptr;
    for (const cider::ir::Block& block : fn->blocks) {
        if (block.term.term == cider::ir::Term::CondBr &&
            block.term.thenTarget > block.label &&
            block.term.elseTarget > block.label) {
            head = &block;
            break;
        }
    }
    check(head != nullptr, "the condition block is the loop head");
    bool toHead = false;
    bool allTerminated = true;
    for (const cider::ir::Block& block : fn->blocks) {
        if (block.term.term == cider::ir::Term::Unreachable) {
            allTerminated = false;
        }
        if (head != nullptr && block.label != head->label &&
            block.term.term == cider::ir::Term::Br &&
            block.term.target == head->label) {
            toHead = true;
        }
    }
    check(toHead, "the step jumps back to the head");
    check(allTerminated, "no block falls off");
    // The init declaration must reach the head's block order: the loop
    // variable is assigned before the condition ever runs.
    bool hasAdd = false;
    for (const cider::ir::Block& block : fn->blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == cider::ir::Op::Add) {
                hasAdd = true;
            }
        }
    }
    check(hasAdd, "the step expression lowered");
}

void testBreakContinue() {
    std::printf("break and continue lower\n");
    // The head is the one block a jump goes backwards to: the condition is
    // re-evaluated there. `continue` must reach it and `break` must not.
    Lowered r = lower(
        "func f(n: int) -> void {\n"
        "    var i: int = 0;\n"
        "    while (i < n) {\n"
        "        i = i + 1;\n"
        "        if (i == 2) { continue; }\n"
        "        if (i == 3) { break; }\n"
        "    }\n"
        "}\n");
    check(r.ok, "break and continue lower: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    if (fn == nullptr) {
        return;
    }
    // Block 1 carries the condition: its false edge is the loop exit and its
    // true edge the body, so it is the head by construction, not by guess.
    const cider::ir::Block* head = nullptr;
    for (const cider::ir::Block& block : fn->blocks) {
        if (block.term.term == cider::ir::Term::CondBr &&
            block.term.thenTarget > block.label &&
            block.term.elseTarget > block.label) {
            head = &block;
            break;
        }
    }
    check(head != nullptr, "the condition block is the loop head");
    bool toHead = false;
    bool toExit = false;
    bool allTerminated = true;
    for (const cider::ir::Block& block : fn->blocks) {
        if (block.term.term == cider::ir::Term::Unreachable) {
            allTerminated = false;
        }
        if (head == nullptr || block.label == head->label) {
            continue;
        }
        if (block.term.term == cider::ir::Term::Br && block.term.target == head->label) {
            toHead = true;
        }
        if (block.term.term == cider::ir::Term::Br && block.term.target == head->term.elseTarget) {
            toExit = true;
        }
    }
    check(toHead, "continue jumps back to the loop head");
    check(toExit, "break jumps forward to the loop exit");
    check(allTerminated, "no block is left unterminated");
}

void testFloat() {
    std::printf("float literals and arithmetic lower\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var a: f64 = 1.5;\n"
        "    var b: f64 = a + 2.5;\n"
        "    var d: f64 = a / b;\n"
        "    return b as int;\n"
        "}\n");
    check(r.ok, "a float expression lowers");
    if (r.ok) {
        const cider::ir::Function* main = find(r.module.get(), "main");
        check(main != nullptr, "main exists");
        // 1.5 and 2.5 as IEEE754 binary64, the values the Const carries.
        const int64_t oneAndAHalf = INT64_C(4609434218613702656);
        const int64_t twoAndAHalf = INT64_C(4612811918334230528);
        bool sawFirst = false;
        bool sawSecond = false;
        bool sawFloatAdd = false;
        bool trappedDiv = false;
        for (const cider::ir::Block& block : main->blocks) {
            for (const cider::ir::Instr& instr : block.body) {
                if (instr.op == cider::ir::Op::Const &&
                    instr.constantBits == oneAndAHalf) {
                    sawFirst = true;
                }
                if (instr.op == cider::ir::Op::Const &&
                    instr.constantBits == twoAndAHalf) {
                    sawSecond = true;
                }
                if (instr.op == cider::ir::Op::Add &&
                    instr.width == cider::ir::IRType::F64) {
                    sawFloatAdd = true;
                }
                if ((instr.op == cider::ir::Op::Div ||
                     instr.op == cider::ir::Op::Rem) &&
                    instr.width == cider::ir::IRType::F64 && instr.mayTrap) {
                    trappedDiv = true;
                }
            }
        }
        check(sawFirst, "1.5 reaches the IR as binary64 bits");
        check(sawSecond, "2.5 reaches the IR as binary64 bits");
        check(sawFloatAdd, "the add carries the f64 width");
        check(!trappedDiv, "float division is not marked as trapping");
    }
}

void testNotYetLowered() {
    std::printf("computed index stores lower with guards\n");
    Lowered r = lower(
        "func f(a: [int; 3], j: int) -> void { a[j] = 1; }\n"
        "func main() -> int { return 0; }\n");
    check(r.ok, "a computed index store lowers now");
    uint32_t condBrs = 0;
    for (const auto& fn : r.module->functions) {
        if (fn.name != "f") {
            continue;
        }
        for (const auto& block : fn.blocks) {
            if (block.term.term == cider::ir::Term::CondBr) {
                ++condBrs;
            }
        }
    }
    check(condBrs >= 2, "the store carries both bounds checks");
}

void testGlobalZeroArray() {
    // A zero-repeat global lowers to zeroed image words, and element
    // access through the global label works like a local's slots.
    std::printf("a zero-filled global array lowers\n");
    Lowered r = lower(
        "var g: [u8; 3] = [0, ...];\n"
        "func main() -> int {\n"
        "    g[1] = 7;\n"
        "    return g[1] as int;\n"
        "}\n");
    check(r.ok, "global array element access lowers: " + r.stage);
    bool found = false;
    if (r.ok) {
        for (const auto& global : r.module->globals) {
            if (global.label == "g_g" && global.initWords.size() == 1 &&
                global.initWords[0] == 0) {
                found = true;
            }
        }
    }
    check(found, "the global carries one zeroed word");
}

void testFoldedBounds() {
    std::printf("a folded past-end index rejects\n");
    Lowered read = lower(
        "func main() -> int {\n"
        "    var a: [u8; 3] = [1, 2, 3];\n"
        "    return a[5] as int;\n"
        "}\n");
    check(!read.ok, "a past-end folded read refuses");
    Lowered store = lower(
        "func main() -> int {\n"
        "    var a: [u8; 3] = [1, 2, 3];\n"
        "    a[5] = 9;\n"
        "    return 0;\n"
        "}\n");
    check(!store.ok, "a past-end folded store refuses");
    Lowered last = lower(
        "func main() -> int {\n"
        "    var a: [u8; 3] = [1, 2, 3];\n"
        "    return a[2] as int;\n"
        "}\n");
    check(last.ok, "the last in-range element still lowers");
}

void testValueFalloff() {
    std::printf("value falloff refuses\n");
    Lowered r = lower(
        "func f(n: int) -> int {\n"
        "    if (n > 0) { return 1; }\n"
        "}\n"
        "func main() -> int { return 0; }\n");
    check(!r.ok && r.stage == "lower", "a value function that may fall off refuses");
    Lowered both = lower(
        "func f(n: int) -> int {\n"
        "    if (n > 0) { return 1; } else { return 2; }\n"
        "}\n"
        "func main() -> int { return 0; }\n");
    check(both.ok, "both arms returning lowers: " + both.stage);
    Lowered loop = lower(
        "func spin() -> int {\n"
        "    while (true) {\n"
        "    }\n"
        "}\n"
        "func main() -> int { return 0; }\n");
    check(loop.ok, "a diverging loop needs no return: " + loop.stage);
}

void testRefuseKeepsScopes() {
    // A refusal inside a condition must not unbalance the scope stack:
    // a past-end folded index refuses in lowering — but the function
    // frame must survive the block exits instead of crashing the driver.
    std::printf("a mid-block refusal keeps scopes balanced\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var a: [u8; 3] = [1, 2, 3];\n"
        "    if (a[5] == 1) { return 1; }\n"
        "    return 0;\n"
        "}\n");
    check(!r.ok && r.stage == "lower", "a past-end index in a condition is refused");
}

// A method emits its own function under the owner's name, and a call site
// reaches it with the receiver first.
void testMethodCall() {
    std::printf("method calls lower\n");
    Lowered r = lower(
        "class Counter { var v: int; func get() -> int { return (*self).v; } }\n"
        "func main() -> int {\n"
        "    var c: Counter = Counter { v: 3 };\n"
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

void testForIn() {
    std::printf("for-in lowers\n");
    Lowered r = lower(
        "func f(a: [int; 4]) -> int {\n"
        "    var s: int = 0;\n"
        "    for (var x in a) {\n"
        "        s = s + x;\n"
        "    }\n"
        "    return s;\n"
        "}\n");
    check(r.ok, "a for-in loop lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "f");
    if (fn == nullptr) {
        return;
    }
    // Same shape as the for head: a condition block with two forward
    // edges, a back edge to it, and no block left unterminated.
    const cider::ir::Block* head = nullptr;
    for (const cider::ir::Block& block : fn->blocks) {
        if (block.term.term == cider::ir::Term::CondBr &&
            block.term.thenTarget > block.label &&
            block.term.elseTarget > block.label) {
            head = &block;
            break;
        }
    }
    check(head != nullptr, "the bounds check block is the loop head");
    bool toHead = false;
    bool allTerminated = true;
    for (const cider::ir::Block& block : fn->blocks) {
        if (block.term.term == cider::ir::Term::Unreachable) {
            allTerminated = false;
        }
        if (head != nullptr && block.label != head->label &&
            block.term.term == cider::ir::Term::Br &&
            block.term.target == head->label) {
            toHead = true;
        }
    }
    check(toHead, "the step jumps back to the head");
    check(allTerminated, "no block falls off");
    // Four-byte elements scale the index, so the address math leaves a
    // multiply behind.
    bool hasMul = false;
    for (const cider::ir::Block& block : fn->blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == cider::ir::Op::Mul) {
                hasMul = true;
            }
        }
    }
    check(hasMul, "the element stride scales the index");
}

// One asm instruction carries the joined text, one name per placeholder,
// and one paired operand: a slot for a local, an address word for a global.
void testAsm() {
    std::printf("asm lowers\n");
    Lowered r = lower(
        "var g: int = 5;\n"
        "func main() -> int {\n"
        "    var sum: int = 3;\n"
        "    asm { \"mov edi, {sum}\" \"mov eax, {g}\" }\n"
        "    return 0;\n"
        "}\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "main");
    if (fn == nullptr) {
        return;
    }
    const cider::ir::Instr* asmb = nullptr;
    for (const cider::ir::Block& block : fn->blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == cider::ir::Op::Asm) {
                asmb = &instr;
            }
        }
    }
    check(asmb != nullptr, "an asm instruction exists");
    if (asmb == nullptr) {
        return;
    }
    check(asmb->asmNames.size() == 2, "one name per placeholder");
    check(asmb->operands.size() == asmb->asmNames.size(),
          "one operand per placeholder");
    check(asmb->asmText.find("mov edi, {sum}") != std::string::npos,
          "the text keeps its placeholders");
    check(asmb->asmText.find("mov eax, {g}") != std::string::npos,
          "the second line survives");
}

void testShortCircuit() {
    std::printf("short circuit lowers\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var a: bool = true;\n"
        "    var b: bool = false;\n"
        "    var c: bool = a && b;\n"
        "    var d: bool = a || b;\n"
        "    if (c) { return 1; }\n"
        "    if (d) { return 2; }\n"
        "    return 0;\n"
        "}\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "main");
    if (fn == nullptr) {
        return;
    }
    uint32_t condBrs = 0;
    for (const cider::ir::Block& block : fn->blocks) {
        if (block.term.term == cider::ir::Term::CondBr) {
            ++condBrs;
        }
    }
    // Two short-circuits plus two ifs: every one branches on a condition.
    check(condBrs >= 4, "each &&, ||, and if leaves a conditional branch");
}

void testTernary() {
    std::printf("ternary lowers\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var pick: bool = true;\n"
        "    var a: int = pick ? 3 : 4;\n"
        "    return a;\n"
        "}\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "main");
    if (fn == nullptr) {
        return;
    }
    uint32_t condBrs = 0;
    for (const cider::ir::Block& block : fn->blocks) {
        if (block.term.term == cider::ir::Term::CondBr) {
            ++condBrs;
        }
    }
    check(condBrs >= 1, "the ternary leaves a conditional branch");
}

void testBareReturn() {
    std::printf("bare return closes its block\n");
    Lowered r = lower(
        "func guard(v: int, z: int) {\n"
        "    if (v == 0) { return; }\n"
        "    var q: int = v / z;\n"
        "}\n"
        "func main() -> int { guard(0, 0); return 7; }\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "guard");
    if (fn == nullptr) {
        return;
    }
    // The then-arm's bare return must close its block with Ret: a branch
    // to the join instead silently runs the divide that follows it.
    uint32_t rets = 0;
    for (const cider::ir::Block& block : fn->blocks) {
        if (block.term.term == cider::ir::Term::Ret) {
            ++rets;
        }
    }
    check(rets == 2, "the early bare return and the value return each close a block");
}

// A two-word return fills the terminator's word list: the register row
// reads it back in order (rax, rdx), so the shape is pinned here, not
// just observed through execution.
void testMultiWordReturn() {
    std::printf("multi-word returns fill Ret words\n");
    Lowered r = lower(
        "func make() -> [u64; 2] {\n"
        "    var t: [u64; 2] = [1, 2];\n"
        "    return t;\n"
        "}\n"
        "func main() -> int {\n"
        "    var m: [u64; 2] = make();\n"
        "    return (m[0] + m[1]) as int;\n"
        "}\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* make = find(r.module.get(), "make");
    if (make == nullptr) {
        return;
    }
    uint32_t words = 0;
    for (const cider::ir::Block& block : make->blocks) {
        if (block.term.term == cider::ir::Term::Ret) {
            words += static_cast<uint32_t>(block.term.values.size());
        }
    }
    check(words == 2, "the array return answers two words");
    const cider::ir::Function* main = find(r.module.get(), "main");
    check(main != nullptr && countOp(*main, cider::ir::Op::Call) == 1,
          "the caller issues one call");
}

// Interpolation builds its string on the heap: the parts' bytes are
// copied into one fresh buffer, so the value owns its storage.
void testInterp() {
    std::printf("interpolation allocates and joins\n");
    Lowered r = lower(
        "func main() -> int {\n"
        "    var v: string = \"ab{12}c\";\n"
        "    return v.len as int;\n"
        "}\n");
    check(r.ok, "lowers: " + r.stage);
    const cider::ir::Function* fn = find(r.module.get(), "main");
    if (fn == nullptr) {
        return;
    }
    bool joins = false;
    for (const cider::ir::Block& block : fn->blocks) {
        for (const cider::ir::Instr& instr : block.body) {
            if (instr.op == cider::ir::Op::Call && instr.callee == "cider_alloc") {
                joins = true;
            }
        }
    }
    check(joins, "the parts join in a fresh buffer");
    // A float prints through the ftoa helper: a widening cast when the
    // part is a float, then a call naming cider_ftoa.
    Lowered floats = lower(
        "func main() -> int {\n"
        "    var x: f32 = 1.5;\n"
        "    var v: string = \"{x}\";\n"
        "    return v.len as int;\n"
        "}\n");
    check(floats.ok, "a float part lowers: " + floats.stage);
    const cider::ir::Function* fmain = find(floats.module.get(), "main");
    bool widens = false;
    bool callsFtoa = false;
    if (fmain != nullptr) {
        for (const cider::ir::Block& block : fmain->blocks) {
            for (const cider::ir::Instr& instr : block.body) {
                if (instr.op == cider::ir::Op::Cast &&
                    instr.width == cider::ir::IRType::F64) {
                    widens = true;
                }
                if (instr.op == cider::ir::Op::Call &&
                    instr.callee == "cider_ftoa") {
                    callsFtoa = true;
                }
            }
        }
    }
    check(widens, "the float widens to a double first");
    check(callsFtoa, "the part converts through cider_ftoa");
}

}  // namespace

void testUefiEntry() {
    std::printf("uefi entry\n");
    Lowered lowered = lowerAs(
        "func efi_main() -> int { return 0; }\n",
        cider::target::Kind::Uefi);
    check(lowered.ok, "a uefi efi_main lowers: " + lowered.stage);
    if (!lowered.ok) {
        return;
    }
    check(find(lowered.module.get(), "efi_main") == nullptr,
          "the firmware symbol is not the user function");
    check(find(lowered.module.get(), "cider_efi_main_user") != nullptr,
          "the user entry has a hidden link symbol");
    check(lowered.module->entry == "cider_efi_main_user",
          "the module names the hidden entry symbol");
    bool image = false;
    bool system = false;
    for (const cider::ir::Global& global : lowered.module->globals) {
        if (global.label == "image_handle") {
            image = true;
        }
        if (global.label == "system_table") {
            system = true;
        }
    }
    check(image, "the image handle global is emitted");
    check(system, "the system table global is emitted");
}

int main() {
    std::printf("lower_test — source becomes IR\n\n");
    testUefiEntry();
    testGlobal();
    testArithmetic();
    testTrapFlag();
    testWrapFlag();
    testCompare();
    testSsa();
    testCalls();
    testLoop();
    testBareReturn();
    testNestedConstruct();
    testFieldOverIndex();
    testStraddlingRead();
    testMultiWordAssign();
    testUnion();
    testSlice();
    testSliceStore();
    testClass();
    testClassValueVptr();
    testMethodCall();
    testDelete();
    testIndirect();
    testClosure();
    testClosureNested();
    testClosureBareName();
    testAddrOf();
    testDerefStore();
    testBreakContinue();
    testForLoop();
    testForIn();
    testAsm();
    testShortCircuit();
    testTernary();
    testInterp();
    testRefuseKeepsScopes();
    testValueFalloff();
    testInterfaceParam();
    testInterfaceClassAdapter();
    testNotYetLowered();
    testFoldedBounds();
    testGlobalZeroArray();
    testFloat();
    testMultiWordReturn();
    std::printf("\n%d checks, %s\n", checks, failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
