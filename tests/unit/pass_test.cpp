#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "constant_folder.h"
#include "dominators.h"
#include "gvn.h"
#include "inliner.h"
#include "ir_gen.h"
#include "lexer.h"
#include "licm.h"
#include "module_resolver.h"
#include "optimizer.h"
#include "parser.h"
#include "symbol_collector.h"
#include "symbols.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

struct ModuleResult {
    cider::ModuleIR module;
    std::string print() {
        std::ostringstream ss;
        std::streambuf* old = std::cout.rdbuf(ss.rdbuf());
        module.print();
        std::cout.rdbuf(old);
        return ss.str();
    }
};

ModuleResult genModule(const std::string& src) {
    cider::ErrorReporter rep;
    cider::Lexer lex(src, rep);
    cider::Parser parser(lex, rep);
    auto prog = parser.parseProgram();
    cider::SymbolTable table;
    cider::ModuleResolver resolver;
    cider::SymbolCollector collector(table, rep, resolver);
    collector.collect(*prog);
    cider::ConstantFolder folder;
    folder.fold(*prog);
    cider::IRGenerator gen(table);
    ModuleResult r;
    r.module = gen.generate(*prog);
    return r;
}

int testDominators() {
    // entry -> A/B (cond) -> merge -> exit; plus a back edge merge -> A.
    auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::VOID);
    auto entry = std::make_unique<cider::BasicBlock>("entry");
    cider::IRInstruction br1;
    br1.op = cider::IROp::COND_BR;
    br1.args = {cider::IRValue::Constant(1, cider::IRType::I32),
                cider::IRValue::Label("A"), cider::IRValue::Label("B")};
    entry->addInstruction(br1);
    auto a = std::make_unique<cider::BasicBlock>("A");
    cider::IRInstruction br2;
    br2.op = cider::IROp::BR;
    br2.args = {cider::IRValue::Label("merge")};
    a->addInstruction(br2);
    auto b = std::make_unique<cider::BasicBlock>("B");
    cider::IRInstruction brToA;
    brToA.op = cider::IROp::BR;
    brToA.args = {cider::IRValue::Label("A")};
    b->addInstruction(brToA);
    auto merge = std::make_unique<cider::BasicBlock>("merge");
    cider::IRInstruction br3;
    br3.op = cider::IROp::COND_BR;
    br3.args = {cider::IRValue::Constant(0, cider::IRType::I32),
                cider::IRValue::Label("A"), cider::IRValue::Label("exit")};
    merge->addInstruction(br3);
    auto exit = std::make_unique<cider::BasicBlock>("exit");
    fn->addBlock(std::move(entry));
    fn->addBlock(std::move(a));
    fn->addBlock(std::move(b));
    fn->addBlock(std::move(merge));
    fn->addBlock(std::move(exit));

    cider::DominatorTree dt;
    dt.build(*fn);
    CHECK(dt.dominates("entry", "A"));
    CHECK(dt.dominates("entry", "merge"));
    CHECK(!dt.dominates("A", "B"));
    CHECK(!dt.dominates("B", "A"));
    CHECK(dt.dominates("merge", "exit"));
    CHECK(!dt.dominates("exit", "merge"));

    // Loop detection on merge -> A -> merge.
    cider::NaturalLoopFinder loops;
    loops.build(*fn, dt);
    const auto& ls = loops.getLoops();
    CHECK(!ls.empty());
    bool found = false;
    for (const auto& l : ls) {
        if (l.header == "A") {
            found = true;
            CHECK(l.blocks.count("merge") == 1);
        }
    }
    CHECK(found);
    return 0;
}

int testDce() {
    auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::VOID);
    auto block = std::make_unique<cider::BasicBlock>("entry");
    cider::IRInstruction dead;
    dead.op = cider::IROp::ADD;
    dead.dest = cider::IRValue::Register("d", cider::IRType::I32);
    dead.args = {cider::IRValue::Constant(1, cider::IRType::I32),
                 cider::IRValue::Constant(2, cider::IRType::I32)};
    block->addInstruction(dead);
    cider::IRInstruction live;
    live.op = cider::IROp::MOV;
    live.dest = cider::IRValue::Register("l", cider::IRType::I32);
    live.args = {cider::IRValue::Constant(5, cider::IRType::I32)};
    block->addInstruction(live);
    cider::IRInstruction ret;
    ret.op = cider::IROp::RET;
    ret.args = {cider::IRValue::Register("l", cider::IRType::I32)};
    block->addInstruction(ret);
    fn->addBlock(std::move(block));
    cider::Optimizer::dce(*fn);
    const auto& instrs = fn->getBlocks()[0]->getInstructions();
    CHECK(instrs.size() == 2);  // dead add removed
    CHECK(instrs[0].dest->name == "l");
    return 0;
}

int testConstFold() {
    // var x: int = 2 + 3 * 4;  -> folded at the AST level before ir-gen.
    auto r = genModule("func main() -> void {\n  var x: int = 2 + 3 * 4;\n}\n");
    std::string d = r.print();
    CHECK(d.find("store %v0, 14") != std::string::npos);
    return 0;
}

int testGvn() {
    auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::I32);
    auto block = std::make_unique<cider::BasicBlock>("entry");
    auto addInstr = [](const std::string& dest) {
        cider::IRInstruction in;
        in.op = cider::IROp::ADD;
        in.dest = cider::IRValue::Register(dest, cider::IRType::I32);
        in.args = {cider::IRValue::Register("x", cider::IRType::I32),
                   cider::IRValue::Constant(1, cider::IRType::I32)};
        return in;
    };
    block->addInstruction(addInstr("a"));
    block->addInstruction(addInstr("b"));
    cider::IRInstruction ret;
    ret.op = cider::IROp::RET;
    ret.args = {cider::IRValue::Register("b", cider::IRType::I32)};
    block->addInstruction(ret);
    fn->addBlock(std::move(block));
    cider::ModuleIR mod;
    mod.addFunction(std::move(fn));
    cider::GvnPass gvn;
    CHECK(gvn.run(mod));
    const auto& instrs = mod.getFunctions()[0]->getBlocks()[0]->getInstructions();
    CHECK(instrs[0].op == cider::IROp::ADD);  // first kept
    CHECK(instrs[1].op == cider::IROp::MOV);  // second deduped to a mov
    CHECK(instrs[1].args[0].name == "a");
    return 0;
}

int testGvnWideStrings() {
    // CID-005/T5 fix round 1 (Q5/GVN): wide literals cross IR as PTR
    // constants named `L"..."`; they must be numbered by content like the
    // narrow `"..."` constants. The generic PTR key (`const_PTR_0`) made
    // every wide literal share a value number, so under optimization an
    // earlier `&u"X"[0]` replaced a later `&u"QR"[0]` with a MOV and the
    // QR data was never emitted (silent wrong code on native + VM opt).
    auto r = genModule(
        "func main() -> int {\n"
        "  var a: *u16 = &u\"X\"[0];\n"
        "  var b: *u16 = &u\"QR\"[0];\n"
        "  return a[0] + b[0];\n"
        "}\n");
    cider::GvnPass gvn;
    gvn.run(r.module);
    std::string d = r.print();
    CHECK(d.find("L\"X") != std::string::npos);
    // The wide name carries embedded NUL units, so only the prefix is
    // contiguous: `L"Q` pins the QR literal's survival.
    CHECK(d.find("L\"Q") != std::string::npos);  // not collapsed to a MOV
    return 0;
}

int testGvnTypes() {
    // CID-003 follow-up (W5): value numbers must include the result type.
    // `x & 255` as i8 (raw 0x9C, widened with movsx to -100) and as u8
    // (156) share operands but are different values; deduplicating them
    // also collapsed their CVT_ITOF consumers, whose result depends on the
    // source type, so the optimized VM/native disagreed with --no-opt.
    auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::I32);
    auto block = std::make_unique<cider::BasicBlock>("entry");
    auto andInstr = [](const std::string& dest, cider::IRType t) {
        cider::IRInstruction in;
        in.op = cider::IROp::BIT_AND;
        in.dest = cider::IRValue::Register(dest, t);
        in.args = {cider::IRValue::Register("x", cider::IRType::I32),
                   cider::IRValue::Constant(255, cider::IRType::I32)};
        return in;
    };
    auto cvtInstr = [](const std::string& dest, const std::string& src, cider::IRType srcT) {
        cider::IRInstruction in;
        in.op = cider::IROp::CVT_ITOF;
        in.dest = cider::IRValue::Register(dest, cider::IRType::F64);
        in.args = {cider::IRValue::Register(src, srcT)};
        return in;
    };
    block->addInstruction(andInstr("a8", cider::IRType::I8));
    block->addInstruction(cvtInstr("c1", "a8", cider::IRType::I8));
    block->addInstruction(andInstr("b8", cider::IRType::U8));
    block->addInstruction(cvtInstr("c2", "b8", cider::IRType::U8));
    cider::IRInstruction ret;
    ret.op = cider::IROp::RET;
    ret.args = {cider::IRValue::Register("c2", cider::IRType::F64)};
    block->addInstruction(ret);
    fn->addBlock(std::move(block));
    cider::ModuleIR mod;
    mod.addFunction(std::move(fn));
    cider::GvnPass gvn;
    gvn.run(mod);
    const auto& instrs = mod.getFunctions()[0]->getBlocks()[0]->getInstructions();
    CHECK(instrs[0].op == cider::IROp::BIT_AND);  // i8 AND kept
    CHECK(instrs[1].op == cider::IROp::CVT_ITOF); // i8 conversion kept
    CHECK(instrs[2].op == cider::IROp::BIT_AND);  // u8 AND not merged
    CHECK(instrs[3].op == cider::IROp::CVT_ITOF); // u8 conversion not merged
    return 0;
}

int testLicm() {
    // while (i < 10) { j = 2 * 3; }  -> the mul hoists to the preheader.
    auto r = genModule(
        "func main() -> void {\n"
        "  var i: int = 0;\n"
        "  var j: int = 0;\n"
        "  while (i < 10) {\n"
        "    j = 2 * 3;\n"
        "    i = i + 1;\n"
        "  }\n"
        "}\n");
    std::string before = r.print();
    cider::SSALicmPass licm;
    CHECK(licm.run(r.module));
    std::string after = r.print();
    CHECK(after.find("mul") != std::string::npos);            // still present
    CHECK(after.find("_preheader") != std::string::npos);     // preheader created
    return 0;
}

int testVolatileIrFlags() {
    // CIDER-BUG-061: ir_gen marks every access that touches a volatile
    // object — a volatile local, a volatile global, and a store through a
    // pointer to volatile — so the optimizer passes can see it.
    auto r = genModule(
        "var g: volatile int = 0;\n"
        "func f() -> int {\n"
        "  var x: volatile int = 3;\n"
        "  var p: volatile int* = &x;\n"
        "  *p = 5;\n"
        "  g = x + *p;\n"
        "  return g;\n"
        "}\n");
    int volatileLoads = 0, volatileStores = 0;
    for (const auto& b : r.module.getFunctions()[0]->getBlocks()) {
        for (const auto& in : b->getInstructions()) {
            if (in.op == cider::IROp::LOAD && in.isVolatile) volatileLoads++;
            if (in.op == cider::IROp::STORE && in.isVolatile) volatileStores++;
        }
    }
    // x init, *p = 5, and the g store are all volatile writes.
    CHECK(volatileStores >= 3);
    // x, *p and g are all volatile reads.
    CHECK(volatileLoads >= 3);

    // Negative control: the same function without `volatile` carries no
    // volatile instruction, so non-volatile IR keeps its existing shape.
    auto r2 = genModule(
        "var g: int = 0;\n"
        "func f() -> int {\n"
        "  var x: int = 3;\n"
        "  var p: int* = &x;\n"
        "  *p = 5;\n"
        "  g = x + *p;\n"
        "  return g;\n"
        "}\n");
    for (const auto& b : r2.module.getFunctions()[0]->getBlocks()) {
        for (const auto& in : b->getInstructions()) {
            CHECK(!in.isVolatile);
        }
    }
    return 0;
}

int testVolatileInterfaceIrFlags() {
    // CIDER-BUG-079: an interface (16-byte fat pointer) store must mark BOTH
    // words volatile when the target is volatile — the same guarantee
    // CIDER-BUG-061 gives scalar/pointer/aggregate stores. Covers a volatile
    // local declaration, its reassignment, and a volatile interface field.
    auto r = genModule(
        "interface Shape { func area() -> int; }\n"
        "struct Holder { var shape: volatile Shape; }\n"
        "func f(c: Shape) -> void {\n"
        "  var x: volatile Shape = c;\n"
        "  x = c;\n"
        "  var h: Holder;\n"
        "  h.shape = c;\n"
        "}\n");
    int volatileStores = 0;
    for (const auto& func : r.module.getFunctions()) {
        for (const auto& b : func->getBlocks()) {
            for (const auto& in : b->getInstructions()) {
                if (in.op == cider::IROp::STORE && in.isVolatile) volatileStores++;
            }
        }
    }
    // x init, x reassignment, field store: data + vtable words each.
    CHECK(volatileStores == 6);

    // Negative control: the same function without `volatile` carries no
    // volatile instruction at all, so non-volatile IR keeps its shape.
    auto r2 = genModule(
        "interface Shape { func area() -> int; }\n"
        "struct Holder { var shape: Shape; }\n"
        "func f(c: Shape) -> void {\n"
        "  var x: Shape = c;\n"
        "  x = c;\n"
        "  var h: Holder;\n"
        "  h.shape = c;\n"
        "}\n");
    for (const auto& func : r2.module.getFunctions()) {
        for (const auto& b : func->getBlocks()) {
            for (const auto& in : b->getInstructions()) {
                CHECK(!in.isVolatile);
            }
        }
    }
    return 0;
}

int testPointerInterfaceStoreIrFlags() {
    // CIDER-BUG-100: `*p = v` through a pointer-to-interface takes the same
    // 16-byte fat-pointer store as `p[i] = v` (two STOREs, data at +0 and
    // vtable at +8) and carries the pointee's volatile flag on both words
    // (CIDER-BUG-079). The lowering this bug produced was a 16-byte MEMCPY
    // of the pointer value (data word + stale neighbour), corrupting
    // dispatch.
    struct StoreTally {
        int stores = 0;
        int volatileStores = 0;
        int memcpyCount = 0;
    };
    auto tally = [](const ModuleResult& m) {
        StoreTally t;
        for (const auto& func : m.module.getFunctions()) {
            for (const auto& b : func->getBlocks()) {
                for (const auto& in : b->getInstructions()) {
                    if (in.op == cider::IROp::MEMCPY) t.memcpyCount++;
                    if (in.op == cider::IROp::STORE) {
                        t.stores++;
                        if (in.isVolatile) t.volatileStores++;
                    }
                }
            }
        }
        return t;
    };

    // Control without the deref store: declaration and pointer locals only.
    auto control = genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(c: Shape) -> void {\n"
        "  var x: Shape = c;\n"
        "  var p: *Shape = &x;\n"
        "}\n");
    auto t0 = tally(control);

    // The deref store adds exactly the two fat-pointer words, volatile-
    // free, and never a MEMCPY of the source address.
    auto r = genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(c: Shape) -> void {\n"
        "  var x: Shape = c;\n"
        "  var p: *Shape = &x;\n"
        "  *p = c;\n"
        "}\n");
    auto t1 = tally(r);
    CHECK(t1.stores == t0.stores + 2);
    CHECK(t1.volatileStores == t0.volatileStores);
    CHECK(t1.memcpyCount == t0.memcpyCount);

    // Volatile pointee: the two deref words are volatile (the volatile
    // declaration/pointer stores every access, so the deref adds exactly
    // two more volatile stores), matching the volatile interface
    // variable/field stores.
    auto controlV = genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(c: Shape) -> void {\n"
        "  var x: volatile Shape = c;\n"
        "  var p: volatile *Shape = &x;\n"
        "}\n");
    auto t0v = tally(controlV);
    auto r2 = genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(c: Shape) -> void {\n"
        "  var x: volatile Shape = c;\n"
        "  var p: volatile *Shape = &x;\n"
        "  *p = c;\n"
        "}\n");
    auto t2 = tally(r2);
    CHECK(t2.stores == t0.stores + 2);
    CHECK(t2.volatileStores == t0v.volatileStores + 2);
    CHECK(t2.memcpyCount == t0.memcpyCount);

    // The pre-existing `p[0] = c` spelling keeps the same two-word shape.
    auto r3 = genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(c: Shape) -> void {\n"
        "  var x: Shape = c;\n"
        "  var p: *Shape = &x;\n"
        "  p[0] = c;\n"
        "}\n");
    auto t3 = tally(r3);
    CHECK(t3.stores == t0.stores + 2);
    CHECK(t3.volatileStores == t0.volatileStores);
    CHECK(t3.memcpyCount == t0.memcpyCount);
    return 0;
}

int testVolatileGvn() {
    // CIDER-BUG-061: GVN must never replace a volatile access with a MOV of
    // an earlier occurrence — even for an opcode that is normally deduped.
    // The straight-line loads pin the LOAD case; the artificially volatile
    // ADD pins the value-number guard itself.
    auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::I32);
    auto block = std::make_unique<cider::BasicBlock>("entry");
    cider::IRInstruction load1;
    load1.op = cider::IROp::LOAD;
    load1.dest = cider::IRValue::Register("v1", cider::IRType::I32);
    load1.args = {cider::IRValue::Register("p", cider::IRType::PTR),
                  cider::IRValue::Constant(0, cider::IRType::I32)};
    load1.isVolatile = true;
    cider::IRInstruction load2 = load1;
    load2.dest = cider::IRValue::Register("v2", cider::IRType::I32);
    cider::IRInstruction add1;
    add1.op = cider::IROp::ADD;
    add1.dest = cider::IRValue::Register("a", cider::IRType::I32);
    add1.args = {cider::IRValue::Register("x", cider::IRType::I32),
                 cider::IRValue::Constant(1, cider::IRType::I32)};
    cider::IRInstruction add2 = add1;
    add2.dest = cider::IRValue::Register("b", cider::IRType::I32);
    add2.isVolatile = true;
    block->addInstruction(load1);
    block->addInstruction(load2);
    block->addInstruction(add1);
    block->addInstruction(add2);
    cider::IRInstruction ret;
    ret.op = cider::IROp::RET;
    ret.args = {cider::IRValue::Register("b", cider::IRType::I32)};
    block->addInstruction(ret);
    fn->addBlock(std::move(block));
    cider::ModuleIR mod;
    mod.addFunction(std::move(fn));
    cider::GvnPass gvn;
    gvn.run(mod);
    const auto& instrs = mod.getFunctions()[0]->getBlocks()[0]->getInstructions();
    CHECK(instrs[0].op == cider::IROp::LOAD && instrs[0].isVolatile);
    CHECK(instrs[1].op == cider::IROp::LOAD && instrs[1].isVolatile);  // kept, not MOV v1
    CHECK(instrs[2].op == cider::IROp::ADD && !instrs[2].isVolatile);
    CHECK(instrs[3].op == cider::IROp::ADD && instrs[3].isVolatile);   // not deduped
    return 0;
}

int testVolatileLicm() {
    // CIDER-BUG-061: LICM hoists a non-volatile alloca load out of the loop
    // (control), but must leave the volatile load in place.
    auto r = genModule(
        "func f(n: int) -> int {\n"
        "  var x: volatile int = 0;\n"
        "  var y: int = 0;\n"
        "  var i: int = 0;\n"
        "  var s: int = 0;\n"
        "  while (i < n) {\n"
        "    s = s + x;\n"
        "    s = s + y;\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return s;\n"
        "}\n");
    cider::SSALicmPass licm;
    CHECK(licm.run(r.module));
    int volLoadsStayed = 0, volLoadsInPreheader = 0, plainLoadsInPreheader = 0;
    for (const auto& b : r.module.getFunctions()[0]->getBlocks()) {
        bool preheader = b->getName().find("preheader") != std::string::npos;
        for (const auto& in : b->getInstructions()) {
            if (in.op != cider::IROp::LOAD) continue;
            if (in.isVolatile) {
                if (preheader) volLoadsInPreheader++;
                else volLoadsStayed++;
            } else if (preheader) {
                plainLoadsInPreheader++;
            }
        }
    }
    CHECK(volLoadsStayed == 1);         // volatile load stayed in the loop
    CHECK(volLoadsInPreheader == 0);    // and was not hoisted
    CHECK(plainLoadsInPreheader >= 1);  // control: a plain load did hoist
    return 0;
}

int testCallClobberLicm() {
    // CIDER-BUG-086: a call is a memory clobber. LICM must not hoist the
    // caller's MEMCPY of an sret aggregate result, nor a load of an alloca
    // whose address was passed to a callee (both would freeze the first
    // iteration's bytes). The alloca holding `i` is loop-written and the
    // sret slot and `x` escape, so after the fix no load may reach the
    // preheader at all.
    auto r = genModule(
        "struct Pair { var a: int; var b: int; }\n"
        "func make(n: int) -> Pair {\n"
        "  var p: Pair;\n"
        "  p.a = n;\n"
        "  p.b = n;\n"
        "  return p;\n"
        "}\n"
        "func store(p: *int, v: int) -> void { *p = v; }\n"
        "func main() -> void {\n"
        "  var x: int = 0;\n"
        "  var i: int = 1;\n"
        "  while (i <= 3) {\n"
        "    var p: Pair = make(i);\n"
        "    store(&x, i);\n"
        "    p.b = x;\n"
        "    i = i + 1;\n"
        "  }\n"
        "}\n");
    cider::SSALicmPass licm;
    CHECK(licm.run(r.module));
    int preMemcpy = 0, loopMemcpy = 0, preLoads = 0, loopLoads = 0;
    for (const auto& func : r.module.getFunctions()) {
        if (func->getName() != "main") continue;
        for (const auto& b : func->getBlocks()) {
            bool preheader = b->getName().find("preheader") != std::string::npos;
            for (const auto& in : b->getInstructions()) {
                if (in.op == cider::IROp::MEMCPY) {
                    if (preheader) preMemcpy++; else loopMemcpy++;
                }
                if (in.op == cider::IROp::LOAD) {
                    if (preheader) preLoads++; else loopLoads++;
                }
            }
        }
    }
    CHECK(preMemcpy == 0);    // sret copy not hoisted
    CHECK(loopMemcpy >= 1);   // it stays in the loop
    CHECK(preLoads == 0);     // escaped x / loop-written i not hoisted
    CHECK(loopLoads >= 1);    // x is re-read every iteration
    return 0;
}

int testInliner() {
    // A small helper gets inlined; recursion does not.
    auto r = genModule(
        "func helper(a: int) -> int {\n  return a + 1;\n}\n"
        "func main() -> int {\n  return helper(41);\n}\n");
    std::string before = r.print();
    cider::Inliner<> inliner;
    CHECK(inliner.run(r.module));
    std::string after = r.print();
    CHECK(after.find("call helper") == std::string::npos);  // call gone
    CHECK(after.find("helper_") != std::string::npos);      // cloned blocks present
    return 0;
}

int run() {
    CHECK(testDominators() == 0);
    CHECK(testDce() == 0);
    CHECK(testConstFold() == 0);
    CHECK(testGvn() == 0);
    CHECK(testGvnWideStrings() == 0);
    CHECK(testGvnTypes() == 0);
    CHECK(testLicm() == 0);
    CHECK(testVolatileIrFlags() == 0);
    CHECK(testVolatileInterfaceIrFlags() == 0);
    CHECK(testPointerInterfaceStoreIrFlags() == 0);
    CHECK(testVolatileGvn() == 0);
    CHECK(testVolatileLicm() == 0);
    CHECK(testCallClobberLicm() == 0);
    CHECK(testInliner() == 0);
    return 0;
}

int main() { return run(); }
