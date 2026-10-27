// inline_test.cpp — direct-call inlining before mem2reg.
//
// The positive cases lower real source so the parameter-slot and return shapes
// are the ones the lowerer actually emits. The refusal cases build a module by
// hand where a source spelling cannot state the exact defect (an extern body,
// a return arity mismatch, a callee with no Ret). Every case checks the module
// structurally after the pass: a refusal must leave the IR untouched, and an
// inline must be a valid replacement.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

const cider::ir::Function* find(const cider::ir::Module* module,
                                const std::string& name) {
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

bool valid(const cider::ir::Module& module) {
    return cider::opt::checkModule(module).empty();
}

// --- A small hand-built module, for refusals source cannot spell ------------

struct Builder {
    cider::TypeContext types;
    cider::ir::Module module;

    Builder() { module.functions.reserve(8); }

    cider::TypeRef i32() {
        return cider::TypeRef{types.basic(cider::BasicKind::I32)};
    }
    cider::TypeRef ptr(const cider::TypeRef& inner) {
        return cider::TypeRef{types.pointer(inner.type)};
    }
    cider::ir::Value value(uint32_t id, cider::ir::IRType width,
                           cider::TypeRef type) {
        cider::ir::Value out;
        out.id = id;
        out.width = width;
        out.type = type;
        return out;
    }
    cider::ir::Function& addFunction(const std::string& name) {
        cider::ir::Function fn;
        fn.name = name;
        module.functions.push_back(std::move(fn));
        return module.functions.back();
    }
    void addBlock(cider::ir::Function& fn, uint32_t label) {
        cider::ir::Block block;
        block.label = label;
        fn.blocks.push_back(std::move(block));
    }
    void emit(cider::ir::Function& fn, cider::ir::Instr instr) {
        fn.blocks.back().body.push_back(std::move(instr));
    }
    cider::ir::Instr constant(uint32_t id, int64_t bits,
                              cider::ir::IRType width, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Const;
        instr.width = width;
        instr.type = type;
        instr.constantBits = bits;
        instr.constantText = std::to_string(bits);
        instr.isPure = true;
        instr.results.push_back(value(id, width, type));
        return instr;
    }
    cider::ir::Instr allocaParam(uint32_t id, cider::TypeRef pointee) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Alloca;
        instr.width = cider::ir::IRType::Ptr;
        instr.type = ptr(pointee);
        instr.isPure = false;
        instr.results.push_back(value(id, instr.width, instr.type));
        return instr;
    }
    cider::ir::Instr load(uint32_t id, uint32_t base, cider::TypeRef type) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Load;
        instr.width = cider::ir::IRType::I32;
        instr.type = type;
        instr.addressBase = value(base, cider::ir::IRType::Ptr, ptr(type));
        instr.isPure = true;
        instr.results.push_back(value(id, instr.width, type));
        return instr;
    }
    cider::ir::Instr call(const std::string& callee,
                          const std::vector<cider::ir::Value>& words,
                          const std::vector<cider::ir::Value>& results) {
        cider::ir::Instr instr;
        instr.op = cider::ir::Op::Call;
        instr.callee = callee;
        instr.mayTrap = true;
        for (const cider::ir::Value& word : words) {
            cider::ir::Argument arg;
            arg.words.push_back(word);
            arg.width = word.width;
            instr.arguments.push_back(std::move(arg));
        }
        instr.results = results;
        instr.width = results.empty() ? cider::ir::IRType::Void
                                      : results[0].width;
        if (!results.empty()) {
            instr.type = results[0].type;
        }
        return instr;
    }
    void ret(cider::ir::Function& fn, size_t block,
             const std::vector<cider::ir::Value>& words) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Ret;
        term.values = words;
        fn.blocks[block].term = term;
    }
    void br(cider::ir::Function& fn, size_t block, uint32_t target) {
        cider::ir::Terminator term;
        term.term = cider::ir::Term::Br;
        term.target = target;
        fn.blocks[block].term = term;
    }
};

// A direct call to a hand-built callee that returns one i32 word.
struct SimpleModule {
    Builder b;
    cider::ir::Function* callee = nullptr;
    cider::ir::Function* caller = nullptr;

    SimpleModule() {
        moduleName();
        cider::ir::Function& c = b.addFunction("g");
        cider::ir::Function& f = b.addFunction("f");
        callee = &c;
        caller = &f;
        build();
    }

    void moduleName() { b.module.name = "inline-test"; }

    void build() {
        cider::TypeRef word = b.i32();
        callee->params.push_back(
            b.value(0, cider::ir::IRType::Ptr, b.ptr(word)));
        b.addBlock(*callee, 0);
        b.emit(*callee, b.allocaParam(0, word));
        b.emit(*callee, b.load(1, 0, word));
        b.ret(*callee, 0, {b.value(1, cider::ir::IRType::I32, word)});

        b.addBlock(*caller, 0);
        b.emit(*caller, b.constant(0, 41, cider::ir::IRType::I32, word));
        b.emit(*caller,
               b.call("g", {b.value(0, cider::ir::IRType::I32, word)},
                      {b.value(1, cider::ir::IRType::I32, word)}));
        b.ret(*caller, 0, {b.value(1, cider::ir::IRType::I32, word)});
    }
};

// --- Positive source cases ---------------------------------------------------

void testSimpleLeaf() {
    std::printf("a leaf call is inlined with a fresh parameter slot\n");
    Lowered r = lower(
        "func leaf(a: int) -> int { return a + 1; }\n"
        "func f(a: int) -> int { return leaf(a); }\n");
    check(r.ok, "lowers: " + r.stage);
    if (!r.ok) {
        return;
    }
    check(valid(*r.module), "the lowered module is well formed");
    cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
    check(inlined.inlined == 1, "one call was inlined");
    check(inlined.skipped == 0, "nothing was skipped");
    check(inlined.clonedInstructions > 0, "cloned instructions are counted");
    check(valid(*r.module), "the inlined module is well formed");
    const cider::ir::Function* f = find(r.module.get(), "f");
    check(f != nullptr, "the caller is there");
    if (f == nullptr) {
        return;
    }
    check(countOp(*f, cider::ir::Op::Call) == 0, "the call is gone");
    check(countOp(*f, cider::ir::Op::Alloca) >= 1,
          "a fresh parameter slot was created");
    check(countOp(*f, cider::ir::Op::Store) >= 1,
          "the argument was stored into the slot");
}

void testMultipleSites() {
    std::printf("several sites inline, including the original suffix\n");
    Lowered r = lower(
        "func bump(x: int) -> int { return x + 1; }\n"
        "func f() -> int {\n"
        "    var a: int = bump(1);\n"
        "    var b: int = bump(a);\n"
        "    return a + b;\n"
        "}\n");
    check(r.ok, "lowers: " + r.stage);
    if (!r.ok) {
        return;
    }
    cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
    check(inlined.inlined == 2, "both calls were inlined");
    check(inlined.skipped == 0, "nothing was skipped");
    check(valid(*r.module), "the inlined module is well formed");
    const cider::ir::Function* f = find(r.module.get(), "f");
    check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 0,
          "no call remains in the caller");
}

void testOneLevel() {
    std::printf("calls inside cloned bodies are not inlined again\n");
    // f is lowered first, so its clone of mid is built before mid itself is
    // visited as a caller. The cloned call to leaf must stay a call; mid's
    // own original call is still eligible when its turn comes.
    Lowered r = lower(
        "func f() -> int { return mid(); }\n"
        "func mid() -> int { return leaf(); }\n"
        "func leaf() -> int { return 7; }\n");
    check(r.ok, "lowers: " + r.stage);
    if (!r.ok) {
        return;
    }
    cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
    check(inlined.inlined == 2, "one call per caller was inlined");
    check(valid(*r.module), "the inlined module is well formed");
    const cider::ir::Function* f = find(r.module.get(), "f");
    check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 1,
          "the cloned body keeps its call");
}

void testSingleReturnLaterBlock() {
    std::printf("a single return substitutes into a later block\n");
    Lowered r = lower(
        "func g(a: int) -> int { return a + 1; }\n"
        "func f(c: bool, a: int) -> int {\n"
        "    var x: int = g(a);\n"
        "    if (c) { return x; }\n"
        "    return x + 1;\n"
        "}\n");
    check(r.ok, "lowers: " + r.stage);
    if (!r.ok) {
        return;
    }
    cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
    check(inlined.inlined == 1, "the call was inlined");
    check(valid(*r.module), "the result use in a later block is valid");
    const cider::ir::Function* f = find(r.module.get(), "f");
    check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 0,
          "the call is gone");
}

void testMultipleReturnPhi() {
    std::printf("several returns build one phi per result word\n");
    Lowered r = lower(
        "func pick(c: bool) -> int {\n"
        "    if (c) { return 7; }\n"
        "    return 9;\n"
        "}\n"
        "func f(c: bool) -> int {\n"
        "    var x: int = pick(c);\n"
        "    if (x > 0) { return x; }\n"
        "    return 0;\n"
        "}\n");
    check(r.ok, "lowers: " + r.stage);
    if (!r.ok) {
        return;
    }
    cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
    check(inlined.inlined == 1, "the call was inlined");
    check(valid(*r.module), "the return phi is structurally valid");
    const cider::ir::Function* f = find(r.module.get(), "f");
    check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 0,
          "the call is gone");
    check(f != nullptr && countOp(*f, cider::ir::Op::Phi) >= 1,
          "the join has a phi");
    if (f != nullptr) {
        for (const cider::ir::Block& block : f->blocks) {
            for (const cider::ir::Instr& instr : block.body) {
                if (instr.op == cider::ir::Op::Phi &&
                    instr.phiIncoming.size() == 2) {
                    return;
                }
            }
        }
        check(false, "the return phi has two incoming edges");
    }
}

void testVoidAndSret() {
    std::printf("void and sret calls inline without result words\n");
    {
        Lowered r = lower(
            "var g: int = 0;\n"
            "func touch(a: int) -> void { g = a; }\n"
            "func f(a: int) -> int { touch(a); return g; }\n");
        check(r.ok, "void program lowers: " + r.stage);
        if (r.ok) {
            cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
            check(inlined.inlined == 1, "the void call was inlined");
            check(valid(*r.module), "the void clone is well formed");
            const cider::ir::Function* f = find(r.module.get(), "f");
            check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 0,
                  "the void call is gone");
        }
    }
    {
        Lowered r = lower(
            "struct Wide { var a: i64; var b: i64; var c: i64; }\n"
            "func make(x: i64) -> Wide {\n"
            "    return Wide { a: x, b: x + 1, c: x + 2 };\n"
            "}\n"
            "func f(x: i64) -> i64 {\n"
            "    var w: Wide = make(x);\n"
            "    return w.a + w.b + w.c;\n"
            "}\n");
        check(r.ok, "sret program lowers: " + r.stage);
        if (r.ok) {
            cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
            check(inlined.inlined == 1, "the sret call was inlined");
            check(valid(*r.module), "the sret clone is well formed");
            const cider::ir::Function* f = find(r.module.get(), "f");
            check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 0,
                  "the sret call is gone");
        }
    }
}

void testMultiwordAndFloat() {
    std::printf("multiword and float returns keep their words\n");
    {
        Lowered r = lower(
            "func pair() -> [u64; 2] {\n"
            "    var r: [u64; 2] = [11, 22];\n"
            "    return r;\n"
            "}\n"
            "func f() -> u64 {\n"
            "    var p: [u64; 2] = pair();\n"
            "    return p[0] + p[1];\n"
            "}\n");
        check(r.ok, "multiword program lowers: " + r.stage);
        if (r.ok) {
            cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
            check(inlined.inlined == 1, "the multiword call was inlined");
            check(valid(*r.module), "the multiword clone is well formed");
            const cider::ir::Function* f = find(r.module.get(), "f");
            check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 0,
                  "the multiword call is gone");
        }
    }
    {
        Lowered r = lower(
            "func half(x: f64) -> f64 { return x / 2.0; }\n"
            "func f(x: f64) -> f64 { return half(x); }\n");
        check(r.ok, "float program lowers: " + r.stage);
        if (r.ok) {
            cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
            check(inlined.inlined == 1, "the float call was inlined");
            check(valid(*r.module), "the float clone is well formed");
            const cider::ir::Function* f = find(r.module.get(), "f");
            check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 0,
                  "the float call is gone");
        }
    }
}

void testRecursionSkipped() {
    std::printf("a self-recursive callee stays a call\n");
    Lowered r = lower(
        "func rec(n: int) -> int {\n"
        "    if (n <= 0) { return 0; }\n"
        "    return rec(n - 1);\n"
        "}\n"
        "func f() -> int { return rec(3); }\n");
    check(r.ok, "lowers: " + r.stage);
    if (!r.ok) {
        return;
    }
    cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
    check(inlined.inlined == 0, "nothing was inlined");
    check(inlined.skipped >= 1, "the recursive call was counted as skipped");
    check(valid(*r.module), "the module is still well formed");
    const cider::ir::Function* f = find(r.module.get(), "f");
    check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 1,
          "the recursive call remains");
}

void testMissingSkipped() {
    std::printf("a missing or extern callee stays a call\n");
    {
        Lowered r = lower(
            "extern func ext(x: int) -> int;\n"
            "func f(x: int) -> int { return ext(x); }\n");
        check(r.ok, "missing-callee program lowers: " + r.stage);
        if (r.ok) {
            cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
            check(inlined.inlined == 0, "the missing callee was not inlined");
            check(inlined.skipped == 1, "the call was counted as skipped");
            check(valid(*r.module), "the module is still well formed");
            const cider::ir::Function* f = find(r.module.get(), "f");
            check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 1,
                  "the extern call remains");
        }
    }
    {
        SimpleModule m;
        m.b.module.functions[0].isExtern = true;
        check(valid(m.b.module), "the hand-built extern module is well formed");
        cider::opt::InlineResult inlined = cider::opt::inlineCalls(m.b.module);
        check(inlined.inlined == 0, "an extern body is not inlined");
        check(inlined.skipped == 1, "the extern call was counted as skipped");
        check(countOp(*m.caller, cider::ir::Op::Call) == 1,
              "the extern call remains");
    }
}

void testAsmSkipped() {
    std::printf("a callee containing asm stays a call\n");
    Lowered r = lower(
        "func g(a: int) -> int {\n"
        "    asm { \"nop\" }\n"
        "    return a + 1;\n"
        "}\n"
        "func f(a: int) -> int { return g(a); }\n");
    check(r.ok, "lowers: " + r.stage);
    if (!r.ok) {
        return;
    }
    cider::opt::InlineResult inlined = cider::opt::inlineCalls(*r.module);
    check(inlined.inlined == 0, "the asm callee was not inlined");
    check(inlined.skipped == 1, "the call was counted as skipped");
    check(valid(*r.module), "the module is still well formed");
    const cider::ir::Function* f = find(r.module.get(), "f");
    check(f != nullptr && countOp(*f, cider::ir::Op::Call) == 1,
          "the asm callee call remains");
}

void testOversizedSkipped() {
    std::printf("callees over the instruction or block cap stay calls\n");
    {
        SimpleModule m;
        cider::ir::Function& callee = m.b.module.functions[0];
        callee.blocks.clear();
        callee.params.clear();
        m.b.addBlock(callee, 0);
        cider::TypeRef word = m.b.i32();
        for (uint32_t i = 0; i < 81; ++i) {
            m.b.emit(callee,
                     m.b.constant(i, static_cast<int64_t>(i),
                                  cider::ir::IRType::I32, word));
        }
        m.b.ret(callee, 0,
                {m.b.value(80, cider::ir::IRType::I32, word)});
        m.caller->params.clear();
        m.caller->blocks.clear();
        m.b.addBlock(*m.caller, 0);
        m.b.emit(*m.caller,
                 m.b.call("g", {},
                          {m.b.value(0, cider::ir::IRType::I32, word)}));
        m.b.ret(*m.caller, 0,
                {m.b.value(0, cider::ir::IRType::I32, word)});
        check(valid(m.b.module), "the oversized module is well formed");
        cider::opt::InlineResult inlined = cider::opt::inlineCalls(m.b.module);
        check(inlined.inlined == 0, "the oversized callee was not inlined");
        check(inlined.skipped == 1, "the oversized call was skipped");
        check(countOp(*m.caller, cider::ir::Op::Call) == 1,
              "the oversized call remains");
    }
    {
        SimpleModule m;
        cider::ir::Function& callee = m.b.module.functions[0];
        callee.blocks.clear();
        callee.params.clear();
        cider::TypeRef word = m.b.i32();
        // A chain of 17 distinct blocks, each branching to the next, ending
        // in a Ret: over the block cap but under the instruction cap.
        for (uint32_t i = 0; i < 17; ++i) {
            m.b.addBlock(callee, i);
        }
        for (uint32_t i = 0; i < 16; ++i) {
            m.b.br(callee, i, i + 1);
        }
        m.b.emit(callee, m.b.constant(0, 0, cider::ir::IRType::I32, word));
        m.b.ret(callee, 16,
                {m.b.value(0, cider::ir::IRType::I32, word)});
        m.caller->params.clear();
        m.caller->blocks.clear();
        m.b.addBlock(*m.caller, 0);
        m.b.emit(*m.caller,
                 m.b.call("g", {},
                          {m.b.value(0, cider::ir::IRType::I32, word)}));
        m.b.ret(*m.caller, 0,
                {m.b.value(0, cider::ir::IRType::I32, word)});
        check(valid(m.b.module), "the over-block module is well formed");
        cider::opt::InlineResult inlined = cider::opt::inlineCalls(m.b.module);
        check(inlined.inlined == 0, "the over-block callee was not inlined");
        check(inlined.skipped == 1, "the over-block call was skipped");
        check(countOp(*m.caller, cider::ir::Op::Call) == 1,
              "the over-block call remains");
    }
}

void testCostModel() {
    std::printf("the cost model limits duplicated medium callees\n");
    auto build = [](uint32_t bodyInstrs, uint32_t sites) {
        auto m = std::make_unique<SimpleModule>();
        cider::ir::Function& callee = *m->callee;
        callee.blocks.clear();
        callee.params.clear();
        cider::TypeRef word = m->b.i32();
        m->b.addBlock(callee, 0);
        for (uint32_t i = 0; i < bodyInstrs; ++i) {
            m->b.emit(callee, m->b.constant(i, static_cast<int64_t>(i),
                                          cider::ir::IRType::I32, word));
        }
        m->b.ret(callee, 0,
                 {m->b.value(bodyInstrs - 1, cider::ir::IRType::I32, word)});

        cider::ir::Function& caller = *m->caller;
        caller.blocks.clear();
        caller.params.clear();
        m->b.addBlock(caller, 0);
        for (uint32_t i = 0; i < sites; ++i) {
            m->b.emit(caller,
                      m->b.call("g", {},
                                {m->b.value(1000 + i, cider::ir::IRType::I32,
                                            word)}));
        }
        m->b.ret(caller, 0,
                 {m->b.value(1000, cider::ir::IRType::I32, word)});
        return m;
    };

    {
        auto m = build(12, 5);
        check(valid(m->b.module), "the five-site module is well formed");
        cider::opt::InlineResult r = cider::opt::inlineCalls(m->b.module);
        check(r.inlined == 0, "a medium callee at five sites is not inlined");
        check(r.skipped == 5, "all five sites are skipped");
        check(countOp(*m->caller, cider::ir::Op::Call) == 5,
              "the five calls remain");
    }
    {
        auto m = build(12, 4);
        check(valid(m->b.module), "the four-site module is well formed");
        cider::opt::InlineResult r = cider::opt::inlineCalls(m->b.module);
        check(r.inlined == 4, "a medium callee at four sites is inlined");
        check(r.skipped == 0, "no site is skipped at four sites");
        check(countOp(*m->caller, cider::ir::Op::Call) == 0,
              "the four calls are gone");
    }
    {
        auto m = build(5, 8);
        check(valid(m->b.module), "the tiny-callee module is well formed");
        cider::opt::InlineResult r = cider::opt::inlineCalls(m->b.module);
        check(r.inlined == 8, "a tiny callee is inlined at every site");
        check(r.skipped == 0, "no tiny site is skipped");
    }
    {
        auto m = build(25, 1);
        check(valid(m->b.module), "the over-cap module is well formed");
        cider::opt::InlineResult r = cider::opt::inlineCalls(m->b.module);
        check(r.inlined == 0, "a callee over 24 instructions is not inlined");
        check(r.skipped == 1, "the over-cap site is skipped");
    }
}

void testMismatchLeavesIrValid() {
    std::printf("return-shape and no-Ret refusals leave the IR untouched\n");
    {
        SimpleModule m;
        cider::ir::Function& callee = m.b.module.functions[0];
        callee.params.clear();
        callee.blocks.clear();
        cider::TypeRef word = m.b.i32();
        m.b.addBlock(callee, 0);
        m.b.emit(callee, m.b.constant(0, 1, cider::ir::IRType::I32, word));
        m.b.ret(callee, 0, {m.b.value(0, cider::ir::IRType::I32, word)});
        m.caller->params.clear();
        m.caller->blocks.clear();
        m.b.addBlock(*m.caller, 0);
        m.b.emit(*m.caller,
                 m.b.call("g", {},
                          {m.b.value(0, cider::ir::IRType::I32, word),
                           m.b.value(1, cider::ir::IRType::I32, word)}));
        m.b.ret(*m.caller, 0,
                {m.b.value(0, cider::ir::IRType::I32, word)});
        check(valid(m.b.module), "the mismatched module is well formed");
        const std::string before = cider::ir::dumpModule(m.b.module);
        cider::opt::InlineResult inlined = cider::opt::inlineCalls(m.b.module);
        check(inlined.inlined == 0, "the return mismatch was refused");
        check(inlined.skipped == 1, "the mismatch was counted as skipped");
        check(valid(m.b.module), "the refused module is still well formed");
        check(cider::ir::dumpModule(m.b.module) == before,
              "the refusal changed no instruction");
    }
    {
        SimpleModule m;
        cider::ir::Function& callee = m.b.module.functions[0];
        callee.params.clear();
        callee.blocks.clear();
        cider::TypeRef word = m.b.i32();
        m.b.addBlock(callee, 0);
        m.b.br(callee, 0, 0);  // a reachable infinite loop with no Ret
        m.caller->params.clear();
        m.caller->blocks.clear();
        m.b.addBlock(*m.caller, 0);
        m.b.emit(*m.caller,
                 m.b.call("g", {},
                          {m.b.value(0, cider::ir::IRType::I32, word)}));
        m.b.ret(*m.caller, 0,
                {m.b.value(0, cider::ir::IRType::I32, word)});
        check(valid(m.b.module), "the no-Ret module is well formed");
        const std::string before = cider::ir::dumpModule(m.b.module);
        cider::opt::InlineResult inlined = cider::opt::inlineCalls(m.b.module);
        check(inlined.inlined == 0, "the no-Ret callee was refused");
        check(inlined.skipped == 1, "the no-Ret call was counted as skipped");
        check(valid(m.b.module), "the refused module is still well formed");
        check(cider::ir::dumpModule(m.b.module) == before,
              "the no-Ret refusal changed no instruction");
    }
    {
        SimpleModule m;
        m.caller->params.clear();
        m.caller->blocks.clear();
        cider::TypeRef word = m.b.i32();
        m.b.addBlock(*m.caller, 0);
        m.b.emit(*m.caller,
                 m.b.call("g", {},
                          {m.b.value(0, cider::ir::IRType::I32, word)}));
        m.b.ret(*m.caller, 0,
                {m.b.value(0, cider::ir::IRType::I32, word)});
        check(valid(m.b.module), "the short-argument module is well formed");
        const std::string before = cider::ir::dumpModule(m.b.module);
        cider::opt::InlineResult inlined = cider::opt::inlineCalls(m.b.module);
        check(inlined.inlined == 0, "the argument shortfall was refused");
        check(inlined.skipped == 1, "the shortfall was counted as skipped");
        check(valid(m.b.module), "the shortfall module is well formed");
        check(cider::ir::dumpModule(m.b.module) == before,
              "the shortfall refusal changed no instruction");
    }
}

void testCountersAndDeterminism() {
    std::printf("counters are stable and two runs agree byte for byte\n");
    const std::string source =
        "func bump(x: int) -> int { return x + 1; }\n"
        "func pick(c: bool) -> int {\n"
        "    if (c) { return 3; }\n"
        "    return 4;\n"
        "}\n"
        "func f(c: bool) -> int {\n"
        "    var a: int = bump(1);\n"
        "    var b: int = pick(c);\n"
        "    return a + b;\n"
        "}\n";
    Lowered one = lower(source);
    Lowered two = lower(source);
    check(one.ok && two.ok, "both copies lower");
    if (!one.ok || !two.ok) {
        return;
    }
    cider::opt::InlineResult first = cider::opt::inlineCalls(*one.module);
    cider::opt::InlineResult second = cider::opt::inlineCalls(*two.module);
    check(first.inlined == 2 && second.inlined == 2, "both calls inline");
    check(first.clonedInstructions == second.clonedInstructions,
          "the cloned-instruction count is deterministic");
    check(first.skipped == second.skipped, "the skip count is deterministic");
    check(cider::ir::dumpModule(*one.module) ==
              cider::ir::dumpModule(*two.module),
          "two compiles produce the same IR");
    check(valid(*one.module) && valid(*two.module),
          "both inlined modules are well formed");

    Lowered pipelineRun = lower(source);
    check(pipelineRun.ok, "a fresh copy lowers for the pipeline");
    if (!pipelineRun.ok) {
        return;
    }
    cider::opt::OptResult pipeline = cider::opt::run(*pipelineRun.module);
    check(pipeline.ok, "the full pipeline accepts the inlined module: " +
                           pipeline.error);
    check(pipeline.inlined == 2, "the pipeline reports the inline count");
    check(pipeline.clonedInstructions == first.clonedInstructions,
          "the pipeline reports the cloned-instruction count");
    check(valid(*pipelineRun.module),
          "the pipeline leaves a well-formed module");
}

}  // namespace

int main() {
    std::printf("inline_test — direct-call inlining before mem2reg\n\n");
    testSimpleLeaf();
    testMultipleSites();
    testOneLevel();
    testSingleReturnLaterBlock();
    testMultipleReturnPhi();
    testVoidAndSret();
    testMultiwordAndFloat();
    testRecursionSkipped();
    testMissingSkipped();
    testAsmSkipped();
    testOversizedSkipped();
    testCostModel();
    testMismatchLeavesIrValid();
    testCountersAndDeterminism();
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
