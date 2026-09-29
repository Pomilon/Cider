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

int testVolatileGap101IrFlags() {
    // CIDER-BUG-101: three emission points missed the CIDER-BUG-061/079
    // volatile model — (1) parameter materialisation wrote the local copy
    // through plain stores (scalar and interface), (2) copying FROM a
    // volatile interface source read the two fat-pointer words through
    // plain loads, and (3) interface dispatch reads of a volatile
    // object/pointee were plain. Every case is pinned with an exact count
    // against a non-volatile control, so non-volatile IR keeps its shape.
    struct Tally {
        int volatileLoads = 0;
        int volatileStores = 0;
    };
    auto tally = [](const ModuleResult& m) {
        Tally t;
        for (const auto& func : m.module.getFunctions()) {
            for (const auto& b : func->getBlocks()) {
                for (const auto& in : b->getInstructions()) {
                    if (!in.isVolatile) continue;
                    if (in.op == cider::IROp::LOAD) t.volatileLoads++;
                    if (in.op == cider::IROp::STORE) t.volatileStores++;
                }
            }
        }
        return t;
    };

    // (1) Parameter materialisation: the scalar copy is one volatile
    // store; the interface copy reads both fat-pointer words through
    // volatile loads and writes them through volatile stores.
    auto p1 = tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(x: volatile int, s: volatile Shape) -> void { }\n"));
    CHECK(p1.volatileLoads == 2);
    CHECK(p1.volatileStores == 3);
    auto p0 = tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(x: int, s: Shape) -> void { }\n"));
    CHECK(p0.volatileLoads == 0 && p0.volatileStores == 0);

    // (2) Copying from a volatile interface source: `var y: Shape = x`
    // adds two volatile source loads while the volatile target's
    // declaration and reassignment keep their CIDER-BUG-079 stores (2+2).
    auto c1 = tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(c: Shape) -> void {\n"
        "  var x: volatile Shape = c;\n"
        "  var y: Shape = x;\n"
        "  x = y;\n"
        "}\n"));
    CHECK(c1.volatileLoads == 2);
    CHECK(c1.volatileStores == 4);

    // The same holds for `return <volatile interface>`: the volatile
    // parameter materialises through 2+2 volatile accesses and the return
    // adds two source loads; the sret target stays a plain store.
    auto r1 = tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(c: Shape) -> Shape { return c; }\n"
        "func g(x: volatile Shape) -> Shape { return x; }\n"));
    CHECK(r1.volatileLoads == 4);
    CHECK(r1.volatileStores == 2);

    // (3) Dispatch through a volatile pointee: both fat-pointer words are
    // volatile loads; the parameter store is volatile too and the
    // method-address load stays plain (the vtable is static data).
    auto d1 = tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(p: volatile *Shape) -> int { return (*p).area(); }\n"));
    CHECK(d1.volatileLoads == 2);
    CHECK(d1.volatileStores == 1);
    auto d0 = tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(p: *Shape) -> int { return (*p).area(); }\n"));
    CHECK(d0.volatileLoads == 0 && d0.volatileStores == 0);

    // Negative control: the combined shapes without `volatile` carry no
    // volatile instruction at all.
    auto n = tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "func f(c: Shape) -> Shape {\n"
        "  var x: Shape = c;\n"
        "  var y: Shape = x;\n"
        "  x = y;\n"
        "  return y;\n"
        "}\n"
        "func d(p: *Shape) -> int { return (*p).area(); }\n"));
    CHECK(n.volatileLoads == 0 && n.volatileStores == 0);
    return 0;
}

int testVolatileGap133IrFlags() {
    // CIDER-BUG-133: round 2 of the volatile sweep. Five emission points
    // still bypassed the CIDER-BUG-061/079/101 model — (1) aggregate copies
    // consulted only the TARGET flag, so a volatile SOURCE copied through a
    // plain MEMCPY; (2) emitClassVptrStore always stamped plainly, so a
    // volatile class value's re-stamp after a copy was unmarked; (3) a
    // member access dropped the enclosing object's volatile flag from the
    // expression type, so dispatch through an interface field of a volatile
    // object (and nested member targets) were classified non-volatile; (5)
    // the declaration paths used the raw spelling flag, missing
    // `type V = volatile T` aliases; (6) the foreach element copy (array or
    // slice) read volatile elements plainly. Every case is pinned with
    // exact counts against a non-volatile control, so non-volatile IR keeps
    // its existing shape.
    //
    // The documented non-fixes: by-value call arguments materialise inside
    // the callee from the CALLEE parameter's flag (no per-argument
    // volatility in IR CALL; see convertCallArgs), and the foreach
    // slice-limit load reads a compiler-generated descriptor, not a
    // user-qualified object, so it stays plain.
    struct Tally {
        int volatileLoads = 0;
        int volatileStores = 0;
        int volatileMemcpy = 0;
        int volatileTotal = 0;
    };
    auto tally = [](const ModuleResult& m) {
        Tally t;
        for (const auto& func : m.module.getFunctions()) {
            for (const auto& b : func->getBlocks()) {
                for (const auto& in : b->getInstructions()) {
                    if (!in.isVolatile) continue;
                    t.volatileTotal++;
                    if (in.op == cider::IROp::LOAD) t.volatileLoads++;
                    if (in.op == cider::IROp::STORE) t.volatileStores++;
                    if (in.op == cider::IROp::MEMCPY) t.volatileMemcpy++;
                }
            }
        }
        return t;
    };

    // (1)+(3) Source-side aggregate copies. `h.inner` type-checks as Inner
    // (the declared field type); its volatility is inherited from the
    // volatile enclosing Holder, so only the source flag can mark the copy.
    // Each function also materialises its volatile parameter (CIDER-BUG-101),
    // so declaration + sret return + assignment contribute three source
    // copies on top of three parameter copies: six volatile MEMCPYs.
    auto g1 = tally(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "struct Holder { var inner: Inner; }\n"
        "func decl(h: volatile Holder) -> void { var y: Inner = h.inner; }\n"
        "func ret(h: volatile Holder) -> Inner { return h.inner; }\n"
        "func assign(h: volatile Holder) -> void { var y: Inner; y = h.inner; }\n"));
    CHECK(g1.volatileMemcpy == 6);
    CHECK(g1.volatileLoads == 0 && g1.volatileStores == 0);
    CHECK(tally(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "struct Holder { var inner: Inner; }\n"
        "func decl(h: Holder) -> void { var y: Inner = h.inner; }\n"
        "func ret(h: Holder) -> Inner { return h.inner; }\n"
        "func assign(h: Holder) -> void { var y: Inner; y = h.inner; }\n")).volatileTotal == 0);

    // The declared-return copy: `return gv` of a volatile global into a
    // volatile return type is one source+target volatile sret MEMCPY (it
    // carried no flag at all before).
    auto g2 = tally(genModule(
        "struct S { var a: int; var b: int; }\n"
        "var gv: volatile S;\n"
        "func ret() -> volatile S { return gv; }\n"));
    CHECK(g2.volatileMemcpy == 1);
    CHECK(g2.volatileTotal == 1);
    CHECK(tally(genModule(
        "struct S { var a: int; var b: int; }\n"
        "var gv: S;\n"
        "func ret() -> S { return gv; }\n")).volatileTotal == 0);

    // CIDER-BUG-077 boxing: the heap box copy reads the volatile struct
    // value (the parameter materialisation adds the second volatile MEMCPY;
    // the interface stores target a plain Shape return).
    auto g3 = tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "class C { var x: int; func area() -> int { return this.x; } }\n"
        "func box(v: volatile C) -> Shape { return v; }\n"));
    CHECK(g3.volatileMemcpy == 2);
    CHECK(g3.volatileTotal == 2);
    CHECK(tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "class C { var x: int; func area() -> int { return this.x; } }\n"
        "func box(v: C) -> Shape { return v; }\n")).volatileTotal == 0);

    // (2) Vptr stamps when the destination is volatile: the parameter, the
    // deref copy and the volatile return each re-stamp `vtable_V` through a
    // volatile STORE; the two pointer parameters store their own slots
    // volatile too (five stores). The three materialisation copies are the
    // three volatile MEMCPYs.
    auto g4 = tally(genModule(
        "class V { virtual func f() -> int { return 1; } }\n"
        "func param(v: volatile V) -> void { }\n"
        "func copy(p: volatile *V) -> void { var x: volatile V = *p; }\n"
        "func ret(p: volatile *V) -> volatile V { return *p; }\n"));
    CHECK(g4.volatileStores == 5);
    CHECK(g4.volatileMemcpy == 3);
    CHECK(g4.volatileLoads == 0);
    CHECK(tally(genModule(
        "class V { virtual func f() -> int { return 1; } }\n"
        "func param(v: V) -> void { }\n"
        "func copy(p: *V) -> void { var x: V = *p; }\n"
        "func ret(p: *V) -> V { return *p; }\n")).volatileTotal == 0);

    // (3) Dispatch and store through an interface field of a volatile
    // enclosing object: both fat-pointer words are volatile loads, and the
    // NESTED member store (`o.inner.shape = c`) marks its two target words.
    // The three volatile parameters contribute three materialisation
    // MEMCPYs.
    auto g5 = tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "struct Holder { var shape: Shape; }\n"
        "struct Inner { var shape: Shape; }\n"
        "struct Outer { var inner: Inner; }\n"
        "func f(h: volatile Holder) -> int { return h.shape.area(); }\n"
        "func g(o: volatile Outer) -> int { return o.inner.shape.area(); }\n"
        "func s(o: volatile Outer, c: Shape) -> void { o.inner.shape = c; }\n"));
    CHECK(g5.volatileLoads == 4);
    CHECK(g5.volatileStores == 2);
    CHECK(g5.volatileMemcpy == 3);
    CHECK(tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "struct Holder { var shape: Shape; }\n"
        "struct Inner { var shape: Shape; }\n"
        "struct Outer { var inner: Inner; }\n"
        "func f(h: Holder) -> int { return h.shape.area(); }\n"
        "func g(o: Outer) -> int { return o.inner.shape.area(); }\n"
        "func s(o: Outer, c: Shape) -> void { o.inner.shape = c; }\n")).volatileTotal == 0);

    // (5) Alias spellings: `type VS = volatile Shape` marks the
    // declaration's two fat-pointer stores, and `type VI = volatile int`
    // prevents the const read from folding and marks the load. The plain
    // aliases carry no volatile instruction.
    auto g6 = tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "type VS = volatile Shape;\n"
        "type VI = volatile int;\n"
        "const K: VI = 7;\n"
        "func f(c: Shape) -> void { var x: VS = c; }\n"
        "func g() -> int { return K; }\n"));
    CHECK(g6.volatileLoads == 1);
    CHECK(g6.volatileStores == 2);
    CHECK(g6.volatileMemcpy == 0);
    CHECK(tally(genModule(
        "interface Shape { func area() -> int; }\n"
        "type VS = Shape;\n"
        "type VI = int;\n"
        "const K: VI = 7;\n"
        "func f(c: Shape) -> void { var x: VS = c; }\n"
        "func g() -> int { return K; }\n")).volatileTotal == 0);

    // (6) Foreach element reads: the element copy of a `[volatile T; N]`
    // array (directly, or through a slice expression over one) is a volatile
    // load; the loop-variable temp reads are volatile too (its declared type
    // is the volatile element). The array element-init stores were already
    // marked by the wrapper flag. The slice descriptor's length/data loads
    // stay plain (there is no volatile slice spelling).
    auto g7 = tally(genModule(
        "func f() -> int {\n"
        "  var arr: [volatile int; 3] = [1, 2, 3];\n"
        "  var s: int = 0;\n"
        "  for (var x in arr) { s = s + x; }\n"
        "  return s;\n"
        "}\n"
        "func g() -> int {\n"
        "  var arr: [volatile int; 3] = [1, 2, 3];\n"
        "  var s: int = 0;\n"
        "  for (var x in arr[0:2]) { s = s + x; }\n"
        "  return s;\n"
        "}\n"));
    CHECK(g7.volatileLoads == 4);
    CHECK(g7.volatileStores == 6);
    CHECK(g7.volatileMemcpy == 0);
    CHECK(tally(genModule(
        "func f() -> int {\n"
        "  var arr: [int; 3] = [1, 2, 3];\n"
        "  var s: int = 0;\n"
        "  for (var x in arr) { s = s + x; }\n"
        "  return s;\n"
        "}\n"
        "func g() -> int {\n"
        "  var arr: [int; 3] = [1, 2, 3];\n"
        "  var s: int = 0;\n"
        "  for (var x in arr[0:2]) { s = s + x; }\n"
        "  return s;\n"
        "}\n")).volatileTotal == 0);
    return 0;
}

int testVolatileFormation144IrFlags() {
    // CIDER-BUG-144: formation-time stores into volatile aggregates stayed
    // plain. emitZeroInstance, emitClassFieldInitializersImpl,
    // emitEmbeddedInitializers (and the struct twin) took no destination
    // flag, so `var x: volatile V;` zeroed, stamped its vptr and ran field
    // initializers through plain instructions — the same gap at globals,
    // `new` instances and embedded fields. The flag now comes from the same
    // alias-resolved sources the copy sites use (CIDER-BUG-061/079/133) and
    // is pinned here with exact counts against non-volatile controls, so
    // non-volatile IR keeps its shape.
    struct Tally {
        int volatileLoads = 0;
        int volatileStores = 0;
        int volatileMemcpy = 0;
        int volatileTotal = 0;
    };
    auto tally = [](const ModuleResult& m) {
        Tally t;
        for (const auto& func : m.module.getFunctions()) {
            for (const auto& b : func->getBlocks()) {
                for (const auto& in : b->getInstructions()) {
                    if (!in.isVolatile) continue;
                    t.volatileTotal++;
                    if (in.op == cider::IROp::LOAD) t.volatileLoads++;
                    if (in.op == cider::IROp::STORE) t.volatileStores++;
                    if (in.op == cider::IROp::MEMCPY) t.volatileMemcpy++;
                }
            }
        }
        return t;
    };

    // V: vptr stamp, an embedded class value with its own field initializer,
    // and a scalar field initializer — three stores per instance.
    const std::string shape =
        "class Inner { var a: int = 7; }\n"
        "class V { virtual func f() -> int { return 1; } var inner: Inner; var x: int = 42; }\n";

    // (1) A volatile value local: the whole-instance zero MEMCPY, the vptr
    // stamp, the embedded Inner's initializer and V's own field
    // initializer are all volatile (one memcpy + three stores). The alias
    // spelling `type VV = volatile V` marks the same formation stores.
    auto l1 = tally(genModule(shape + "func f() -> void { var x: volatile V; }\n"));
    CHECK(l1.volatileMemcpy == 1);
    CHECK(l1.volatileStores == 3);
    CHECK(l1.volatileLoads == 0);
    auto l2 = tally(genModule(shape +
        "type VV = volatile V;\n"
        "func f() -> void { var x: VV; }\n"));
    CHECK(l2.volatileMemcpy == 1);
    CHECK(l2.volatileStores == 3);
    CHECK(tally(genModule(shape + "func f() -> void { var x: V; }\n")).volatileTotal == 0);

    // (2) A volatile global: .bss starts zero, so the synthesized startup
    // pass has no zero MEMCPY; its vptr and field-initializer stores are
    // volatile. The same global without `volatile` stays wholly plain.
    auto g1 = tally(genModule(shape +
        "var gv: volatile V;\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(g1.volatileMemcpy == 0);
    CHECK(g1.volatileStores == 3);
    CHECK(tally(genModule(shape +
        "var gp: V;\n"
        "func main() -> i32 { return 0; }\n")).volatileTotal == 0);

    // (3) `new volatile V`: the zero, vptr and field stores are marked; the
    // volatile pointer local's own initialization adds the fourth store
    // (CIDER-BUG-061). `new VV` through the alias marks the three formation
    // stores while its plain pointer local adds none; `new V` stays plain.
    auto n1 = tally(genModule(shape +
        "func f() -> void { var p: volatile V* = new volatile V; }\n"));
    CHECK(n1.volatileMemcpy == 1);
    CHECK(n1.volatileStores == 4);
    auto n2 = tally(genModule(shape +
        "type VV = volatile V;\n"
        "func f() -> void { var p: VV* = new VV; }\n"));
    CHECK(n2.volatileMemcpy == 1);
    CHECK(n2.volatileStores == 3);
    CHECK(tally(genModule(shape +
        "func f() -> void { var p: V* = new V; }\n")).volatileTotal == 0);

    // (4) Embedded recursion: a volatile Holder zeroes and forms its
    // embedded V volatile (one memcpy + three stores); a volatile-qualified
    // FIELD marks its own formation stores even under a plain destination
    // (its enclosing zero stays plain). The all-plain shapes carry nothing.
    auto e1 = tally(genModule(shape +
        "class Holder { var v: V; }\n"
        "func f() -> void { var h: volatile Holder; }\n"));
    CHECK(e1.volatileMemcpy == 1);
    CHECK(e1.volatileStores == 3);
    auto e2 = tally(genModule(shape +
        "class VolField { var v: volatile V; }\n"
        "func f() -> void { var h: VolField; }\n"));
    CHECK(e2.volatileMemcpy == 0);
    CHECK(e2.volatileStores == 3);

    // (5) A fixed array of volatile class values: the zero covers the whole
    // array and every element's formation stores are marked (2 x 3 stores).
    auto a1 = tally(genModule(shape + "func f() -> void { var a: [volatile V; 2]; }\n"));
    CHECK(a1.volatileMemcpy == 1);
    CHECK(a1.volatileStores == 6);
    CHECK(a1.volatileLoads == 0);

    // Negative control: every shape above without `volatile` (locals,
    // embedded fields, array, `new`, global, alias) carries no volatile
    // instruction at all.
    auto n = tally(genModule(shape +
        "class Holder { var v: V; }\n"
        "class PlainField { var v: V; }\n"
        "type VV = V;\n"
        "var gp: V;\n"
        "func f() -> void { var x: V; var h: Holder; var hf: PlainField; var a: [V; 2]; }\n"
        "func nf() -> void { var p: V* = new V; }\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(n.volatileLoads == 0 && n.volatileStores == 0 && n.volatileMemcpy == 0);
    return 0;
}

int testVolatileArraySpelling145IrFlags() {
    // CIDER-BUG-145: parseType consumed a leading `volatile` and the array
    // wrapper discarded it, so `volatile [T; N]` compiled to a plain array
    // while `[volatile T; N]` marked the element. The wrapper qualifier now
    // reaches TypeInfo::fromAST, and both spellings describe the same
    // volatile aggregate: every element access (at any nesting depth) is
    // observable. Pinned with exact counts, spelling-pair equality and
    // non-volatile controls.
    struct Tally {
        int volatileLoads = 0;
        int volatileStores = 0;
        int volatileMemcpy = 0;
        int volatileTotal = 0;
    };
    auto tally = [](const ModuleResult& m) {
        Tally t;
        for (const auto& func : m.module.getFunctions()) {
            for (const auto& b : func->getBlocks()) {
                for (const auto& in : b->getInstructions()) {
                    if (!in.isVolatile) continue;
                    t.volatileTotal++;
                    if (in.op == cider::IROp::LOAD) t.volatileLoads++;
                    if (in.op == cider::IROp::STORE) t.volatileStores++;
                    if (in.op == cider::IROp::MEMCPY) t.volatileMemcpy++;
                }
            }
        }
        return t;
    };

    // (1) One dimension: one volatile store and one volatile load for the
    // wrapper spelling, exactly the counts the element spelling already
    // produced; the plain array stays at zero.
    auto oneD = [](const std::string& ty) {
        return "func f() -> int {\n"
               "  var a: " + ty + ";\n"
               "  a[0] = 1;\n"
               "  return a[1];\n"
               "}\n"
               "func main() -> int { return f(); }\n";
    };
    auto d1w = tally(genModule(oneD("volatile [int; 3]")));
    CHECK(d1w.volatileStores == 1);
    CHECK(d1w.volatileLoads == 1);
    auto d1e = tally(genModule(oneD("[volatile int; 3]")));
    CHECK(d1e.volatileStores == 1);
    CHECK(d1e.volatileLoads == 1);
    CHECK(tally(genModule(oneD("[int; 3]"))).volatileTotal == 0);

    // (2) Nested dimensions: the wrapper qualifier reaches the innermost
    // element, so 2-D and 3-D wrapper spellings match their element twins.
    auto twoD = [](const std::string& ty) {
        return "func f() -> int {\n"
               "  var a: " + ty + ";\n"
               "  a[0][1] = 1;\n"
               "  return a[1][0];\n"
               "}\n"
               "func main() -> int { return f(); }\n";
    };
    auto d2w = tally(genModule(twoD("volatile [[int; 2]; 3]")));
    CHECK(d2w.volatileStores == 1);
    CHECK(d2w.volatileLoads == 1);
    auto d2e = tally(genModule(twoD("[[volatile int; 2]; 3]")));
    CHECK(d2e.volatileStores == 1);
    CHECK(d2e.volatileLoads == 1);
    CHECK(tally(genModule(twoD("[[int; 2]; 3]"))).volatileTotal == 0);

    auto threeD = [](const std::string& ty) {
        return "func f() -> int {\n"
               "  var a: " + ty + ";\n"
               "  a[0][1][0] = 1;\n"
               "  return a[1][0][1];\n"
               "}\n"
               "func main() -> int { return f(); }\n";
    };
    auto d3w = tally(genModule(threeD("volatile [[[int; 2]; 3]; 4]")));
    CHECK(d3w.volatileStores == 1);
    CHECK(d3w.volatileLoads == 1);
    auto d3e = tally(genModule(threeD("[[[volatile int; 2]; 3]; 4]")));
    CHECK(d3e.volatileStores == 1);
    CHECK(d3e.volatileLoads == 1);
    CHECK(tally(genModule(threeD("[[[int; 2]; 3]; 4]"))).volatileTotal == 0);

    // (3) Formation of a volatile class-value array: the whole-array zero
    // MEMCPY plus each element's vptr stamp and field initializer are
    // volatile (1 memcpy + 2 x 2 stores), identically for both spellings.
    const std::string shape =
        "class V { virtual func f() -> int { return 1; } var x: int = 42; }\n";
    auto form = [&](const std::string& ty) {
        return shape + "func g() -> void { var a: " + ty + "; }\n";
    };
    auto f1 = tally(genModule(form("volatile [V; 2]")));
    CHECK(f1.volatileMemcpy == 1);
    CHECK(f1.volatileStores == 4);
    auto f2 = tally(genModule(form("[volatile V; 2]")));
    CHECK(f2.volatileMemcpy == 1);
    CHECK(f2.volatileStores == 4);
    CHECK(tally(genModule(form("[V; 2]"))).volatileTotal == 0);

    // (4) Scalar `volatile T` is untouched: one init store, one assignment
    // store and one read.
    auto s = tally(genModule(
        "func f() -> int {\n"
        "  var v: volatile int = 1;\n"
        "  v = 2;\n"
        "  return v;\n"
        "}\n"
        "func main() -> int { return f(); }\n"));
    CHECK(s.volatileStores == 2);
    CHECK(s.volatileLoads == 1);
    return 0;
}

int testVolatileLiteral146IrFlags() {
    // CIDER-BUG-146: an explicit field store in a struct literal into a
    // volatile-qualified field (`H{ f: 5 }`) stayed a plain STORE while the
    // direct member store (`h.f = 5`) marked through memberAccessIsVolatile.
    // The literal's field-store paths now mirror CIDER-BUG-144's field rule:
    // the field's own alias-resolved flag marks the scalar STORE, joins
    // CIDER-BUG-133's source flag on the aggregate MEMCPY, and marks the
    // class-value vptr re-stamp. Pinned with exact counts against
    // non-volatile controls, so non-volatile IR keeps its shape.
    //
    // The documented non-fix (investigated, no ABI change): `new volatile
    // C(args)` marks every caller-side formation store (CIDER-BUG-144) but
    // the constructor BODY writes `this` fields plainly — one shared callee
    // with a plain `C*` hidden parameter and no per-argument volatility in
    // IR CALL (the same architecture limit CIDER-BUG-133 records for
    // by-value arguments; see emitConstructorCall). The case below pins the
    // reviewed behaviour so a future mechanism is a deliberate change.
    struct Tally {
        int volatileLoads = 0;
        int volatileStores = 0;
        int volatileMemcpy = 0;
        int volatileTotal = 0;
    };
    auto tallyFn = [](const ModuleResult& m, const std::string& name) {
        Tally t;
        for (const auto& func : m.module.getFunctions()) {
            if (func->getName() != name) continue;
            for (const auto& b : func->getBlocks()) {
                for (const auto& in : b->getInstructions()) {
                    if (!in.isVolatile) continue;
                    t.volatileTotal++;
                    if (in.op == cider::IROp::LOAD) t.volatileLoads++;
                    if (in.op == cider::IROp::STORE) t.volatileStores++;
                    if (in.op == cider::IROp::MEMCPY) t.volatileMemcpy++;
                }
            }
        }
        return t;
    };
    // Whole-module tally: range-for over every function.
    auto tallyAll = [](const ModuleResult& m) {
        Tally t;
        for (const auto& func : m.module.getFunctions()) {
            for (const auto& b : func->getBlocks()) {
                for (const auto& in : b->getInstructions()) {
                    if (!in.isVolatile) continue;
                    t.volatileTotal++;
                    if (in.op == cider::IROp::LOAD) t.volatileLoads++;
                    if (in.op == cider::IROp::STORE) t.volatileStores++;
                    if (in.op == cider::IROp::MEMCPY) t.volatileMemcpy++;
                }
            }
        }
        return t;
    };

    // (1) Scalar volatile field: the literal's `f` store and the direct
    // assignment are the two volatile stores; the `g` store, the literal
    // temp's zero/copy and the return local's copy stay plain. The alias
    // spelling `type VI = volatile int` marks identically. The all-plain
    // shape carries no volatile instruction.
    auto s1 = tallyAll(genModule("struct H { var f: volatile int; var g: int; }\n"
        "func f() -> int { var h: H = H{ f: 5, g: 6 }; h.f = 7; return h.f; }\n"
        "func main() -> i32 { return f(); }\n"));
    CHECK(s1.volatileStores == 2);
    CHECK(s1.volatileLoads == 1);
    CHECK(s1.volatileMemcpy == 0);
    auto s2 = tallyAll(genModule(
        "type VI = volatile int;\n"
        "struct H { var f: VI; var g: int; }\n"
        "func f() -> int { var h: H = H{ f: 5, g: 6 }; h.f = 7; return h.f; }\n"
        "func main() -> i32 { return f(); }\n"));
    CHECK(s2.volatileStores == 2);
    CHECK(s2.volatileLoads == 1);
    CHECK(tallyAll(genModule(
        "struct H { var f: int; var g: int; }\n"
        "func f() -> int { var h: H = H{ f: 5, g: 6 }; h.f = 7; return h.f; }\n"
        "func main() -> i32 { return f(); }\n")).volatileTotal == 0);

    // (2) Aggregate struct field: the literal's copy into the volatile field
    // is marked (joins the volatile source's CIDER-BUG-133 flag; the
    // materialised volatile parameter is the second MEMCPY). The same copy
    // through a plain field with a volatile enclosing source is marked by the
    // source flag alone; both all-plain controls stay at zero.
    auto a1 = tallyAll(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "struct HIn { var inner: volatile Inner; var g: int; }\n"
        "func f(src: volatile Inner) -> void { var h: HIn = HIn{ inner: src, g: 1 }; }\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(a1.volatileMemcpy == 2);
    CHECK(a1.volatileLoads == 0 && a1.volatileStores == 0);
    CHECK(tallyAll(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "struct PIn { var inner: Inner; var g: int; }\n"
        "func f(src: Inner) -> void { var h: PIn = PIn{ inner: src, g: 1 }; }\n"
        "func main() -> i32 { return 0; }\n")).volatileTotal == 0);
    auto a2 = tallyAll(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "struct Holder { var inner: Inner; }\n"
        "struct PIn { var inner: Inner; var g: int; }\n"
        "func f(h: volatile Holder) -> void { var x: PIn = PIn{ inner: h.inner, g: 1 }; }\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(a2.volatileMemcpy == 2);
    CHECK(a2.volatileTotal == 2);
    CHECK(tallyAll(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "struct Holder { var inner: Inner; }\n"
        "struct PIn { var inner: Inner; var g: int; }\n"
        "func f(h: Holder) -> void { var x: PIn = PIn{ inner: h.inner, g: 1 }; }\n"
        "func main() -> i32 { return 0; }\n")).volatileTotal == 0);

    // (3) Class-valued volatile field: the literal's copy AND its vptr
    // re-stamp are volatile; the volatile parameter's materialisation copy and
    // re-stamp are the other two (CIDER-BUG-101/133). The plain field/source
    // control carries nothing. Before the fix only the copy was marked, so
    // the two stores pin the vptr half of the rule.
    const std::string vclass = "class V { virtual func f() -> int { return 1; } }\n";
    auto v1 = tallyAll(genModule(vclass +
        "struct HV { var v: volatile V; var g: int; }\n"
        "func f(src: volatile V) -> void { var h: HV = HV{ v: src, g: 1 }; }\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(v1.volatileMemcpy == 2);
    CHECK(v1.volatileStores == 2);
    CHECK(v1.volatileLoads == 0);
    CHECK(tallyAll(genModule(vclass +
        "struct PV { var v: V; var g: int; }\n"
        "func f(src: V) -> void { var h: PV = PV{ v: src, g: 1 }; }\n"
        "func main() -> i32 { return 0; }\n")).volatileTotal == 0);

    // (4) Interface-typed volatile field: the literal's two fat-pointer
    // words are volatile (the path already carried the field flag); the plain
    // control carries nothing.
    const std::string iface =
        "interface Shape { func area() -> int; }\n"
        "class C { var x: int; func area() -> int { return this.x; } }\n";
    auto i1 = tallyAll(genModule(iface +
        "struct HI { var shape: volatile Shape; var g: int; }\n"
        "func f(c: Shape) -> void { var h: HI = HI{ shape: c, g: 1 }; }\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(i1.volatileStores == 2);
    CHECK(i1.volatileTotal == 2);
    CHECK(tallyAll(genModule(iface +
        "struct PI { var shape: Shape; var g: int; }\n"
        "func f(c: Shape) -> void { var h: PI = PI{ shape: c, g: 1 }; }\n"
        "func main() -> i32 { return 0; }\n")).volatileTotal == 0);

    // (5) Documented non-fix: `new volatile K(5)` marks the caller-side zero
    // (MEMCPY) and the two caller stores (field initializer + volatile
    // pointer local), but the shared constructor's own `this` stores are
    // plain — there is no per-call-site volatility to thread. The same shape
    // without `volatile` carries nothing.
    const std::string kclass =
        "class K { var x: int = 42; func init(v: int) { this.x = v; } }\n";
    auto m1 = genModule(kclass +
        "func f() -> void { var p: volatile K* = new volatile K(5); }\n"
        "func main() -> i32 { return 0; }\n");
    auto c1 = tallyAll(m1);
    CHECK(c1.volatileMemcpy == 1);
    CHECK(c1.volatileStores == 2);
    CHECK(c1.volatileLoads == 0);
    auto k1 = tallyFn(m1, "K_init");
    CHECK(k1.volatileTotal == 0);
    CHECK(tallyAll(genModule(kclass +
        "func f() -> void { var p: K* = new K(5); }\n"
        "func main() -> i32 { return 0; }\n")).volatileTotal == 0);
    return 0;
}

int testVolatileAggregateConvert167IrFlags() {
    // CIDER-BUG-167: the checker now accepts a plain aggregate value for a
    // volatile-qualified destination (and a volatile source read into a
    // plain destination), the qualifier being a property of the access, not
    // of the value. The IR side already marks the resulting aggregate
    // copies through CIDER-BUG-133/144's destination||source rule; this
    // test pins that every newly accepted path marks its MEMCPY (and the
    // class vptr re-stamp) when either end is volatile, with all-plain
    // controls carrying nothing.
    struct Tally {
        int volatileLoads = 0;
        int volatileStores = 0;
        int volatileMemcpy = 0;
        int volatileTotal = 0;
    };
    auto tallyAll = [](const ModuleResult& m) {
        Tally t;
        for (const auto& func : m.module.getFunctions()) {
            for (const auto& b : func->getBlocks()) {
                for (const auto& in : b->getInstructions()) {
                    if (!in.isVolatile) continue;
                    t.volatileTotal++;
                    if (in.op == cider::IROp::LOAD) t.volatileLoads++;
                    if (in.op == cider::IROp::STORE) t.volatileStores++;
                    if (in.op == cider::IROp::MEMCPY) t.volatileMemcpy++;
                }
            }
        }
        return t;
    };

    // (1) Plain -> volatile destination: local declaration, field
    // assignment, struct-literal field, alias-spelled local and a struct
    // literal as the source value each lower to exactly one volatile
    // MEMCPY. The copies into plain destinations in the same functions
    // stay plain.
    auto a1 = tallyAll(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "struct H { var f: volatile Inner; var g: int; }\n"
        "type VI = volatile Inner;\n"
        "func aa() -> void { var src: Inner; var dst: volatile Inner = src; }\n"
        "func bb() -> void { var h: H; var src: Inner; h.f = src; }\n"
        "func cc() -> void { var src: Inner; var h: H = H{ f: src, g: 1 }; }\n"
        "func dd() -> void { var src: Inner; var dst: VI = src; }\n"
        "func ee() -> void { var dst: volatile Inner = Inner{ a: 7, b: 8 }; }\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(a1.volatileMemcpy == 5);
    CHECK(a1.volatileTotal == 5);
    // All-plain controls for the same shapes carry no volatile instruction.
    CHECK(tallyAll(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "struct H { var f: Inner; var g: int; }\n"
        "type VI = Inner;\n"
        "func aa() -> void { var src: Inner; var dst: VI = src; }\n"
        "func bb() -> void { var h: H; var src: Inner; h.f = src; }\n"
        "func cc() -> void { var src: Inner; var h: H = H{ f: src, g: 1 }; }\n"
        "func dd() -> void { var dst: VI = Inner{ a: 7, b: 8 }; }\n"
        "func main() -> i32 { return 0; }\n")).volatileTotal == 0);

    // (2) Volatile source -> plain destination: both the declaration and
    // the reassignment mark their copy through CIDER-BUG-133's source flag.
    auto b1 = tallyAll(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "func f() -> void {\n"
        "  var src: volatile Inner;\n"
        "  var dst: Inner = src;\n"
        "  var dst2: Inner;\n"
        "  dst2 = src;\n"
        "  var plain: Inner;\n"
        "  var control: Inner = plain;\n"
        "}\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(b1.volatileMemcpy == 2);
    CHECK(b1.volatileTotal == 2);

    // (3) Parameter materialisation and the sret return copy: a plain
    // argument into a volatile parameter marks the callee's copy, a plain
    // value returned from a volatile-returning function marks the sret
    // copy, and storing that result into a volatile local marks the local
    // copy too.
    auto c1 = tallyAll(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "func take(x: volatile Inner) -> void { }\n"
        "func give() -> volatile Inner { var v: Inner; return v; }\n"
        "func f() -> void { var src: Inner; take(src); var r: volatile Inner = give(); }\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(c1.volatileMemcpy == 3);
    CHECK(c1.volatileTotal == 3);
    // Non-volatile control: no volatile instruction.
    CHECK(tallyAll(genModule(
        "struct Inner { var a: int; var b: int; }\n"
        "func take(x: Inner) -> void { }\n"
        "func give() -> Inner { var v: Inner; return v; }\n"
        "func f() -> void { var src: Inner; take(src); var r: Inner = give(); }\n"
        "func main() -> i32 { return 0; }\n")).volatileTotal == 0);

    // (4) Nested array: the deep destination marks (volatile [[int;2];3])
    // reach the whole-object MEMCPY; the same shape with only the element
    // spelling also marks.
    auto d1 = tallyAll(genModule(
        "func f() -> void {\n"
        "  var src: [[int; 2]; 3];\n"
        "  var dst: volatile [[int; 2]; 3] = src;\n"
        "  var dst2: [[volatile int; 2]; 3] = src;\n"
        "}\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(d1.volatileMemcpy == 2);
    CHECK(d1.volatileTotal == 2);

    // (5) Class value: the copy and the vptr re-stamp both follow the
    // volatile destination, exactly like CIDER-BUG-144/146's other paths.
    auto e1 = tallyAll(genModule(
        "class V { var x: int; virtual func f() -> int { return 1; } }\n"
        "func f() -> void { var src: V; var dst: volatile V = src; }\n"
        "func main() -> i32 { return 0; }\n"));
    CHECK(e1.volatileMemcpy == 1);
    CHECK(e1.volatileStores == 1);
    CHECK(e1.volatileTotal == 2);
    CHECK(tallyAll(genModule(
        "class V { var x: int; virtual func f() -> int { return 1; } }\n"
        "func f() -> void { var src: V; var dst: V = src; }\n"
        "func main() -> i32 { return 0; }\n")).volatileTotal == 0);
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

int testDerivedStoreLicm() {
    // CIDER-BUG-091: an in-loop STORE through a derived pointer
    // (`%seed = add %slot, %off`, carried through a PHI and a MOV) writes the
    // base alloca's storage, so a direct LOAD of `%slot` must not hoist. A
    // LOAD of an alloca the loop never writes (`%other`) still must — the
    // loop-store set records base allocas, not raw address registers.
    // Today's ir_gen only stores aggregates through `add` temps, so this
    // latent case is built as IR by hand.
    auto build = []() {
        cider::ModuleIR mod;
        auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::VOID);

        auto entry = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction slot;
        slot.op = cider::IROp::ALLOCA;
        slot.dest = cider::IRValue::Register("slot", cider::IRType::PTR);
        slot.args = {cider::IRValue::Constant(64, cider::IRType::I32)};
        entry->addInstruction(slot);
        cider::IRInstruction other;
        other.op = cider::IROp::ALLOCA;
        other.dest = cider::IRValue::Register("other", cider::IRType::PTR);
        other.args = {cider::IRValue::Constant(8, cider::IRType::I32)};
        entry->addInstruction(other);
        cider::IRInstruction initOther;
        initOther.op = cider::IROp::STORE;
        initOther.args = {cider::IRValue::Register("other", cider::IRType::PTR),
                          cider::IRValue::Constant(7, cider::IRType::I32),
                          cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(initOther);
        cider::IRInstruction off;
        off.op = cider::IROp::MOV;
        off.dest = cider::IRValue::Register("off", cider::IRType::I64);
        off.args = {cider::IRValue::Constant(0, cider::IRType::I64)};
        entry->addInstruction(off);
        cider::IRInstruction seed;
        seed.op = cider::IROp::ADD;
        seed.dest = cider::IRValue::Register("seed", cider::IRType::PTR);
        seed.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                     cider::IRValue::Register("off", cider::IRType::I64)};
        entry->addInstruction(seed);
        cider::IRInstruction i0;
        i0.op = cider::IROp::MOV;
        i0.dest = cider::IRValue::Register("i0", cider::IRType::I32);
        i0.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(i0);
        cider::IRInstruction one;
        one.op = cider::IROp::MOV;
        one.dest = cider::IRValue::Register("one", cider::IRType::I32);
        one.args = {cider::IRValue::Constant(1, cider::IRType::I32)};
        entry->addInstruction(one);
        cider::IRInstruction enter;
        enter.op = cider::IROp::BR;
        enter.args = {cider::IRValue::Label("header")};
        entry->addInstruction(enter);

        auto header = std::make_unique<cider::BasicBlock>("header");
        cider::IRInstruction i;
        i.op = cider::IROp::PHI;
        i.dest = cider::IRValue::Register("i", cider::IRType::I32);
        i.phiArgs = {{"entry", cider::IRValue::Register("i0", cider::IRType::I32)},
                     {"body", cider::IRValue::Register("iNext", cider::IRType::I32)}};
        header->addInstruction(i);
        cider::IRInstruction p;
        p.op = cider::IROp::PHI;
        p.dest = cider::IRValue::Register("p", cider::IRType::PTR);
        p.phiArgs = {{"entry", cider::IRValue::Register("seed", cider::IRType::PTR)},
                     {"body", cider::IRValue::Register("pNext", cider::IRType::PTR)}};
        header->addInstruction(p);
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.args = {cider::IRValue::Register("p", cider::IRType::PTR),
                   cider::IRValue::Register("i", cider::IRType::I32),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(st);
        cider::IRInstruction ldSlot;
        ldSlot.op = cider::IROp::LOAD;
        ldSlot.dest = cider::IRValue::Register("v", cider::IRType::I32);
        ldSlot.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                       cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldSlot);
        cider::IRInstruction ldOther;
        ldOther.op = cider::IROp::LOAD;
        ldOther.dest = cider::IRValue::Register("w", cider::IRType::I32);
        ldOther.args = {cider::IRValue::Register("other", cider::IRType::PTR),
                        cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldOther);
        cider::IRInstruction sum;
        sum.op = cider::IROp::ADD;
        sum.dest = cider::IRValue::Register("s", cider::IRType::I32);
        sum.args = {cider::IRValue::Register("v", cider::IRType::I32),
                    cider::IRValue::Register("w", cider::IRType::I32)};
        header->addInstruction(sum);
        cider::IRInstruction inc;
        inc.op = cider::IROp::ADD;
        inc.dest = cider::IRValue::Register("iNext", cider::IRType::I32);
        inc.args = {cider::IRValue::Register("i", cider::IRType::I32),
                    cider::IRValue::Register("one", cider::IRType::I32)};
        header->addInstruction(inc);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_LT;
        cmp.dest = cider::IRValue::Register("cond", cider::IRType::BOOL);
        cmp.args = {cider::IRValue::Register("iNext", cider::IRType::I32),
                    cider::IRValue::Constant(10, cider::IRType::I32)};
        header->addInstruction(cmp);
        cider::IRInstruction cbr;
        cbr.op = cider::IROp::COND_BR;
        cbr.args = {cider::IRValue::Register("cond", cider::IRType::BOOL),
                    cider::IRValue::Label("body"), cider::IRValue::Label("exit")};
        header->addInstruction(cbr);

        auto body = std::make_unique<cider::BasicBlock>("body");
        cider::IRInstruction pNext;
        pNext.op = cider::IROp::MOV;
        pNext.dest = cider::IRValue::Register("pNext", cider::IRType::PTR);
        pNext.args = {cider::IRValue::Register("p", cider::IRType::PTR)};
        body->addInstruction(pNext);
        cider::IRInstruction back;
        back.op = cider::IROp::BR;
        back.args = {cider::IRValue::Label("header")};
        body->addInstruction(back);

        auto exit = std::make_unique<cider::BasicBlock>("exit");
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        exit->addInstruction(ret);

        fn->addBlock(std::move(entry));
        fn->addBlock(std::move(header));
        fn->addBlock(std::move(body));
        fn->addBlock(std::move(exit));
        mod.addFunction(std::move(fn));
        return mod;
    };

    auto checkHoist = [](cider::ModuleIR& mod) {
        int preLoads = 0, preOther = 0, loopLoads = 0;
        for (const auto& b : mod.getFunctions()[0]->getBlocks()) {
            const bool preheader = b->getName().find("preheader") != std::string::npos;
            for (const auto& in : b->getInstructions()) {
                if (in.op != cider::IROp::LOAD) continue;
                if (preheader) {
                    preLoads++;
                    if (in.args[0].kind == cider::IRValue::REGISTER &&
                        in.args[0].name == "other") preOther++;
                } else if (b->getName() == "header") {
                    loopLoads++;
                }
            }
        }
        CHECK(preLoads == 1);   // only the unwritten `other` load hoists
        CHECK(preOther == 1);
        CHECK(loopLoads == 1);  // `slot` load blocked by the derived store
        return 0;
    };

    auto ssaMod = build();
    cider::SSALicmPass licm;
    CHECK(licm.run(ssaMod));
    CHECK(checkHoist(ssaMod) == 0);

    auto postMod = build();
    cider::PostLicmPass postLicm;
    CHECK(postLicm.run(postMod));
    CHECK(checkHoist(postMod) == 0);
    return 0;
}

int testMultiDefStoreLicm() {
    // CIDER-BUG-143: an in-loop STORE whose address register is
    // multi-defined (the inliner writes one `MOV dest, ret` per cloned
    // return) can carry any of several pointers, but defMap keeps only the
    // first def. A base alloca reachable solely through a later def was
    // missed by loopStores, so its in-loop LOAD hoisted to the preheader
    // and froze the first iteration's bytes. The fallback must treat every
    // alloca as written. Control: with a single-defined store address the
    // precise behavior stays — unwritten allocas still hoist.
    auto build = [](bool multiDef) {
        cider::ModuleIR mod;
        auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::VOID);

        auto entry = std::make_unique<cider::BasicBlock>("entry");
        cider::IRInstruction slot;
        slot.op = cider::IROp::ALLOCA;
        slot.dest = cider::IRValue::Register("slot", cider::IRType::PTR);
        slot.args = {cider::IRValue::Constant(64, cider::IRType::I32)};
        entry->addInstruction(slot);
        cider::IRInstruction victim;
        victim.op = cider::IROp::ALLOCA;
        victim.dest = cider::IRValue::Register("victim", cider::IRType::PTR);
        victim.args = {cider::IRValue::Constant(8, cider::IRType::I32)};
        entry->addInstruction(victim);
        cider::IRInstruction other;
        other.op = cider::IROp::ALLOCA;
        other.dest = cider::IRValue::Register("other", cider::IRType::PTR);
        other.args = {cider::IRValue::Constant(8, cider::IRType::I32)};
        entry->addInstruction(other);
        cider::IRInstruction initOther;
        initOther.op = cider::IROp::STORE;
        initOther.args = {cider::IRValue::Register("other", cider::IRType::PTR),
                          cider::IRValue::Constant(7, cider::IRType::I32),
                          cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(initOther);
        cider::IRInstruction off;
        off.op = cider::IROp::MOV;
        off.dest = cider::IRValue::Register("off", cider::IRType::I64);
        off.args = {cider::IRValue::Constant(0, cider::IRType::I64)};
        entry->addInstruction(off);
        cider::IRInstruction pSlot;
        pSlot.op = cider::IROp::ADD;
        pSlot.dest = cider::IRValue::Register("p", cider::IRType::PTR);
        pSlot.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                      cider::IRValue::Register("off", cider::IRType::I64)};
        entry->addInstruction(pSlot);
        if (multiDef) {
            // The inliner's per-clone `MOV dest, ret` shape: defMap keeps
            // only pSlot, so the store below is actually writing `victim`.
            cider::IRInstruction pVictim;
            pVictim.op = cider::IROp::MOV;
            pVictim.dest = cider::IRValue::Register("p", cider::IRType::PTR);
            pVictim.args = {cider::IRValue::Register("victim", cider::IRType::PTR)};
            entry->addInstruction(pVictim);
        }
        cider::IRInstruction i0;
        i0.op = cider::IROp::MOV;
        i0.dest = cider::IRValue::Register("i0", cider::IRType::I32);
        i0.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(i0);
        cider::IRInstruction one;
        one.op = cider::IROp::MOV;
        one.dest = cider::IRValue::Register("one", cider::IRType::I32);
        one.args = {cider::IRValue::Constant(1, cider::IRType::I32)};
        entry->addInstruction(one);
        cider::IRInstruction enter;
        enter.op = cider::IROp::BR;
        enter.args = {cider::IRValue::Label("header")};
        entry->addInstruction(enter);

        auto header = std::make_unique<cider::BasicBlock>("header");
        cider::IRInstruction i;
        i.op = cider::IROp::PHI;
        i.dest = cider::IRValue::Register("i", cider::IRType::I32);
        i.phiArgs = {{"entry", cider::IRValue::Register("i0", cider::IRType::I32)},
                     {"body", cider::IRValue::Register("iNext", cider::IRType::I32)}};
        header->addInstruction(i);
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.args = {cider::IRValue::Register("p", cider::IRType::PTR),
                   cider::IRValue::Register("i", cider::IRType::I32),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(st);
        cider::IRInstruction ldVictim;
        ldVictim.op = cider::IROp::LOAD;
        ldVictim.dest = cider::IRValue::Register("v", cider::IRType::I32);
        ldVictim.args = {cider::IRValue::Register("victim", cider::IRType::PTR),
                         cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldVictim);
        cider::IRInstruction ldOther;
        ldOther.op = cider::IROp::LOAD;
        ldOther.dest = cider::IRValue::Register("w", cider::IRType::I32);
        ldOther.args = {cider::IRValue::Register("other", cider::IRType::PTR),
                        cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldOther);
        cider::IRInstruction sum;
        sum.op = cider::IROp::ADD;
        sum.dest = cider::IRValue::Register("s", cider::IRType::I32);
        sum.args = {cider::IRValue::Register("v", cider::IRType::I32),
                    cider::IRValue::Register("w", cider::IRType::I32)};
        header->addInstruction(sum);
        cider::IRInstruction inc;
        inc.op = cider::IROp::ADD;
        inc.dest = cider::IRValue::Register("iNext", cider::IRType::I32);
        inc.args = {cider::IRValue::Register("i", cider::IRType::I32),
                    cider::IRValue::Register("one", cider::IRType::I32)};
        header->addInstruction(inc);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_LT;
        cmp.dest = cider::IRValue::Register("cond", cider::IRType::BOOL);
        cmp.args = {cider::IRValue::Register("iNext", cider::IRType::I32),
                    cider::IRValue::Constant(10, cider::IRType::I32)};
        header->addInstruction(cmp);
        cider::IRInstruction cbr;
        cbr.op = cider::IROp::COND_BR;
        cbr.args = {cider::IRValue::Register("cond", cider::IRType::BOOL),
                    cider::IRValue::Label("body"), cider::IRValue::Label("exit")};
        header->addInstruction(cbr);

        auto body = std::make_unique<cider::BasicBlock>("body");
        cider::IRInstruction back;
        back.op = cider::IROp::BR;
        back.args = {cider::IRValue::Label("header")};
        body->addInstruction(back);

        auto exit = std::make_unique<cider::BasicBlock>("exit");
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        exit->addInstruction(ret);

        fn->addBlock(std::move(entry));
        fn->addBlock(std::move(header));
        fn->addBlock(std::move(body));
        fn->addBlock(std::move(exit));
        mod.addFunction(std::move(fn));
        return mod;
    };

    auto countLoads = [](cider::ModuleIR& mod, int& preLoads, int& loopLoads) {
        preLoads = 0;
        loopLoads = 0;
        for (const auto& b : mod.getFunctions()[0]->getBlocks()) {
            const bool preheader = b->getName().find("preheader") != std::string::npos;
            for (const auto& in : b->getInstructions()) {
                if (in.op != cider::IROp::LOAD) continue;
                if (preheader) preLoads++;
                else if (b->getName() == "header") loopLoads++;
            }
        }
    };

    // Multi-defined store address: conservative fallback, no load hoists.
    // Before the fix both loads (including `victim`, reachable only through
    // the second def of `p`) reached the preheader.
    auto ssaMod = build(true);
    cider::SSALicmPass licm;
    licm.run(ssaMod);
    int ssaPre = 0, ssaLoop = 0;
    countLoads(ssaMod, ssaPre, ssaLoop);
    CHECK(ssaPre == 0);
    CHECK(ssaLoop == 2);

    auto postMod = build(true);
    cider::PostLicmPass postLicm;
    postLicm.run(postMod);
    int postPre = 0, postLoop = 0;
    countLoads(postMod, postPre, postLoop);
    CHECK(postPre == 0);
    CHECK(postLoop == 2);

    // Control: a single-defined store address keeps the precise behavior —
    // allocas the loop never writes still hoist.
    auto controlMod = build(false);
    cider::SSALicmPass controlLicm;
    CHECK(controlLicm.run(controlMod));
    int ctrlPre = 0, ctrlLoop = 0;
    countLoads(controlMod, ctrlPre, ctrlLoop);
    CHECK(ctrlPre == 2);
    CHECK(ctrlLoop == 0);
    return 0;
}

int testUnknownStoreAddressLicm() {
    // CIDER-BUG-161: an in-loop STORE/MEMCPY through an address of unknown
    // provenance recorded nothing in loopStores, and escapedAllocas was only
    // consulted when the loop had a clobber. In a clobber-free loop such a
    // store may write an escaped alloca (an external pointer can only name
    // an address that left the function), yet the LOAD of that alloca still
    // hoisted to the preheader and froze the first iteration's bytes.
    // Chosen rule: an unresolved store address is treated like a clobber for
    // ESCAPED allocas only. A LOAD of the non-escaping `other` alloca still
    // hoists (control), while `slot` stays in the loop.
    enum Mode { ParamAddr, LoadAddr, MemcpyDest };
    auto build = [](Mode mode) {
        cider::ModuleIR mod;
        auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::VOID);
        fn->addParam("p", cider::IRType::PTR);

        auto entry = std::make_unique<cider::BasicBlock>("entry");
        auto addAlloca = [&](const char* name, int64_t size) {
            cider::IRInstruction a;
            a.op = cider::IROp::ALLOCA;
            a.dest = cider::IRValue::Register(name, cider::IRType::PTR);
            a.args = {cider::IRValue::Constant(size, cider::IRType::I32)};
            entry->addInstruction(a);
        };
        addAlloca("slot", 64);   // escapes through the call below
        addAlloca("other", 8);   // never escapes: control alloca
        addAlloca("pp", 8);      // holds the pointer parameter (LoadAddr mode)
        cider::IRInstruction initOther;
        initOther.op = cider::IROp::STORE;
        initOther.args = {cider::IRValue::Register("other", cider::IRType::PTR),
                          cider::IRValue::Constant(7, cider::IRType::I32),
                          cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(initOther);
        cider::IRInstruction initPp;
        initPp.op = cider::IROp::STORE;
        initPp.args = {cider::IRValue::Register("pp", cider::IRType::PTR),
                       cider::IRValue::Register("p", cider::IRType::PTR),
                       cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(initPp);
        // The alloca's address leaves the function, so an external pointer
        // can legitimately name it. The call is OUTSIDE the loop, so it is
        // not a clobber and escapedAllocas used to stay unconsulted.
        cider::IRInstruction esc;
        esc.op = cider::IROp::CALL;
        esc.args = {cider::IRValue::Label("sink"),
                    cider::IRValue::Register("slot", cider::IRType::PTR)};
        entry->addInstruction(esc);
        if (mode == LoadAddr) {
            // Unknown provenance through a LOAD: `pp` holds `p`, and
            // `pload` is a copy of that pointer from memory.
            cider::IRInstruction ld;
            ld.op = cider::IROp::LOAD;
            ld.dest = cider::IRValue::Register("pload", cider::IRType::PTR);
            ld.args = {cider::IRValue::Register("pp", cider::IRType::PTR),
                       cider::IRValue::Constant(0, cider::IRType::I32)};
            entry->addInstruction(ld);
        }
        cider::IRInstruction i0;
        i0.op = cider::IROp::MOV;
        i0.dest = cider::IRValue::Register("i0", cider::IRType::I32);
        i0.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(i0);
        cider::IRInstruction one;
        one.op = cider::IROp::MOV;
        one.dest = cider::IRValue::Register("one", cider::IRType::I32);
        one.args = {cider::IRValue::Constant(1, cider::IRType::I32)};
        entry->addInstruction(one);
        cider::IRInstruction enter;
        enter.op = cider::IROp::BR;
        enter.args = {cider::IRValue::Label("header")};
        entry->addInstruction(enter);

        auto header = std::make_unique<cider::BasicBlock>("header");
        cider::IRInstruction i;
        i.op = cider::IROp::PHI;
        i.dest = cider::IRValue::Register("i", cider::IRType::I32);
        i.phiArgs = {{"entry", cider::IRValue::Register("i0", cider::IRType::I32)},
                     {"body", cider::IRValue::Register("iNext", cider::IRType::I32)}};
        header->addInstruction(i);
        const char* addr = (mode == LoadAddr) ? "pload" : "p";
        if (mode == MemcpyDest) {
            cider::IRInstruction mc;
            mc.op = cider::IROp::MEMCPY;
            mc.args = {cider::IRValue::Register(addr, cider::IRType::PTR),
                       cider::IRValue::Register("other", cider::IRType::PTR),
                       cider::IRValue::Constant(8, cider::IRType::I32)};
            header->addInstruction(mc);
        } else {
            cider::IRInstruction st;
            st.op = cider::IROp::STORE;
            st.args = {cider::IRValue::Register(addr, cider::IRType::PTR),
                       cider::IRValue::Register("i", cider::IRType::I32),
                       cider::IRValue::Constant(0, cider::IRType::I32)};
            header->addInstruction(st);
        }
        cider::IRInstruction ldSlot;
        ldSlot.op = cider::IROp::LOAD;
        ldSlot.dest = cider::IRValue::Register("v", cider::IRType::I32);
        ldSlot.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                       cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldSlot);
        cider::IRInstruction ldOther;
        ldOther.op = cider::IROp::LOAD;
        ldOther.dest = cider::IRValue::Register("w", cider::IRType::I32);
        ldOther.args = {cider::IRValue::Register("other", cider::IRType::PTR),
                        cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldOther);
        cider::IRInstruction sum;
        sum.op = cider::IROp::ADD;
        sum.dest = cider::IRValue::Register("s", cider::IRType::I32);
        sum.args = {cider::IRValue::Register("v", cider::IRType::I32),
                    cider::IRValue::Register("w", cider::IRType::I32)};
        header->addInstruction(sum);
        cider::IRInstruction inc;
        inc.op = cider::IROp::ADD;
        inc.dest = cider::IRValue::Register("iNext", cider::IRType::I32);
        inc.args = {cider::IRValue::Register("i", cider::IRType::I32),
                    cider::IRValue::Register("one", cider::IRType::I32)};
        header->addInstruction(inc);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_LT;
        cmp.dest = cider::IRValue::Register("cond", cider::IRType::BOOL);
        cmp.args = {cider::IRValue::Register("iNext", cider::IRType::I32),
                    cider::IRValue::Constant(10, cider::IRType::I32)};
        header->addInstruction(cmp);
        cider::IRInstruction cbr;
        cbr.op = cider::IROp::COND_BR;
        cbr.args = {cider::IRValue::Register("cond", cider::IRType::BOOL),
                    cider::IRValue::Label("body"), cider::IRValue::Label("exit")};
        header->addInstruction(cbr);

        auto body = std::make_unique<cider::BasicBlock>("body");
        cider::IRInstruction back;
        back.op = cider::IROp::BR;
        back.args = {cider::IRValue::Label("header")};
        body->addInstruction(back);

        auto exit = std::make_unique<cider::BasicBlock>("exit");
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        exit->addInstruction(ret);

        fn->addBlock(std::move(entry));
        fn->addBlock(std::move(header));
        fn->addBlock(std::move(body));
        fn->addBlock(std::move(exit));
        mod.addFunction(std::move(fn));
        return mod;
    };

    auto checkHoist = [](cider::ModuleIR& mod) {
        int preSlot = 0, loopSlot = 0, preOther = 0, loopOther = 0;
        for (const auto& b : mod.getFunctions()[0]->getBlocks()) {
            const bool preheader = b->getName().find("preheader") != std::string::npos;
            for (const auto& in : b->getInstructions()) {
                if (in.op != cider::IROp::LOAD || in.args.empty() ||
                    in.args[0].kind != cider::IRValue::REGISTER) continue;
                const std::string& a = in.args[0].name;
                const bool inLoop = b->getName() == "header";
                if (a == "slot") { if (preheader) preSlot++; else if (inLoop) loopSlot++; }
                if (a == "other") { if (preheader) preOther++; else if (inLoop) loopOther++; }
            }
        }
        CHECK(preSlot == 0);   // escaped alloca load stays in the loop
        CHECK(loopSlot == 1);
        CHECK(preOther == 1);  // control: non-escaping load still hoists
        CHECK(loopOther == 0);
        return 0;
    };

    const Mode modes[] = {ParamAddr, LoadAddr, MemcpyDest};
    for (Mode mode : modes) {
        auto ssaMod = build(mode);
        cider::SSALicmPass licm;
        licm.run(ssaMod);
        CHECK(checkHoist(ssaMod) == 0);

        auto postMod = build(mode);
        cider::PostLicmPass postLicm;
        postLicm.run(postMod);
        CHECK(checkHoist(postMod) == 0);
    }
    return 0;
}

int testDeepMultiDefStoreLicm() {
    // CIDER-BUG-160: the CIDER-BUG-143 fallback checked only the raw store
    // address for multi-defness. A multi-defined register DEEPER in the
    // chain (`%p = add %q, off`) still resolved through defMap's first def
    // of `%q`, so a base carried solely by a later def was missing from
    // loopStores and its in-loop LOAD hoisted to the preheader, freezing
    // the first iteration's bytes. The chain walk now reports the
    // intermediate and the store takes the loopStoresUnknown fallback.
    // Control: a single-defined ADD/MOV/PHI chain keeps resolving, so loads
    // of allocas the loop never writes still hoist.
    enum Mode { MultiDefMov, MultiDefPhi, SingleDefChain };
    auto build = [](Mode mode) {
        cider::ModuleIR mod;
        auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::VOID);

        auto entry = std::make_unique<cider::BasicBlock>("entry");
        auto addAlloca = [&](const char* name, int64_t size) {
            cider::IRInstruction a;
            a.op = cider::IROp::ALLOCA;
            a.dest = cider::IRValue::Register(name, cider::IRType::PTR);
            a.args = {cider::IRValue::Constant(size, cider::IRType::I32)};
            entry->addInstruction(a);
        };
        addAlloca("slot", 64);   // written through the derived address
        addAlloca("victim", 8);  // reachable only through the later `q` def
        addAlloca("other", 8);   // never written: control alloca
        cider::IRInstruction initOther;
        initOther.op = cider::IROp::STORE;
        initOther.args = {cider::IRValue::Register("other", cider::IRType::PTR),
                          cider::IRValue::Constant(7, cider::IRType::I32),
                          cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(initOther);
        cider::IRInstruction off;
        off.op = cider::IROp::MOV;
        off.dest = cider::IRValue::Register("off", cider::IRType::I64);
        off.args = {cider::IRValue::Constant(0, cider::IRType::I64)};
        entry->addInstruction(off);
        // `q` is the address intermediate; defMap keeps only its first def.
        cider::IRInstruction qSlot;
        qSlot.op = cider::IROp::MOV;
        qSlot.dest = cider::IRValue::Register("q", cider::IRType::PTR);
        qSlot.args = {cider::IRValue::Register("slot", cider::IRType::PTR)};
        entry->addInstruction(qSlot);
        if (mode == MultiDefMov) {
            // A later def of `q` carries another base, mirroring the
            // inliner's per-clone `MOV dest, ret`.
            cider::IRInstruction qVictim;
            qVictim.op = cider::IROp::MOV;
            qVictim.dest = cider::IRValue::Register("q", cider::IRType::PTR);
            qVictim.args = {cider::IRValue::Register("victim", cider::IRType::PTR)};
            entry->addInstruction(qVictim);
        } else if (mode == MultiDefPhi) {
            cider::IRInstruction qEntry;
            qEntry.op = cider::IROp::MOV;
            qEntry.dest = cider::IRValue::Register("qEntry", cider::IRType::PTR);
            qEntry.args = {cider::IRValue::Register("victim", cider::IRType::PTR)};
            entry->addInstruction(qEntry);
        }
        cider::IRInstruction i0;
        i0.op = cider::IROp::MOV;
        i0.dest = cider::IRValue::Register("i0", cider::IRType::I32);
        i0.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(i0);
        cider::IRInstruction one;
        one.op = cider::IROp::MOV;
        one.dest = cider::IRValue::Register("one", cider::IRType::I32);
        one.args = {cider::IRValue::Constant(1, cider::IRType::I32)};
        entry->addInstruction(one);
        cider::IRInstruction enter;
        enter.op = cider::IROp::BR;
        enter.args = {cider::IRValue::Label("header")};
        entry->addInstruction(enter);

        auto header = std::make_unique<cider::BasicBlock>("header");
        cider::IRInstruction i;
        i.op = cider::IROp::PHI;
        i.dest = cider::IRValue::Register("i", cider::IRType::I32);
        i.phiArgs = {{"entry", cider::IRValue::Register("i0", cider::IRType::I32)},
                     {"body", cider::IRValue::Register("iNext", cider::IRType::I32)}};
        header->addInstruction(i);
        if (mode == MultiDefPhi) {
            // The second def of `q` is a PHI whose arms carry the hidden
            // base; the walk must not resolve `q` through its first def only.
            cider::IRInstruction qPhi;
            qPhi.op = cider::IROp::PHI;
            qPhi.dest = cider::IRValue::Register("q", cider::IRType::PTR);
            qPhi.phiArgs = {{"entry", cider::IRValue::Register("qEntry", cider::IRType::PTR)},
                            {"body", cider::IRValue::Register("qBack", cider::IRType::PTR)}};
            header->addInstruction(qPhi);
        }
        cider::IRInstruction p;
        p.op = cider::IROp::ADD;
        p.dest = cider::IRValue::Register("p", cider::IRType::PTR);
        p.args = {cider::IRValue::Register("q", cider::IRType::PTR),
                  cider::IRValue::Register("off", cider::IRType::I64)};
        header->addInstruction(p);
        cider::IRInstruction st;
        st.op = cider::IROp::STORE;
        st.args = {cider::IRValue::Register("p", cider::IRType::PTR),
                   cider::IRValue::Register("i", cider::IRType::I32),
                   cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(st);
        cider::IRInstruction ldSlot;
        ldSlot.op = cider::IROp::LOAD;
        ldSlot.dest = cider::IRValue::Register("v", cider::IRType::I32);
        ldSlot.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                       cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldSlot);
        cider::IRInstruction ldVictim;
        ldVictim.op = cider::IROp::LOAD;
        ldVictim.dest = cider::IRValue::Register("w", cider::IRType::I32);
        ldVictim.args = {cider::IRValue::Register("victim", cider::IRType::PTR),
                         cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldVictim);
        cider::IRInstruction ldOther;
        ldOther.op = cider::IROp::LOAD;
        ldOther.dest = cider::IRValue::Register("u", cider::IRType::I32);
        ldOther.args = {cider::IRValue::Register("other", cider::IRType::PTR),
                        cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldOther);
        cider::IRInstruction sum;
        sum.op = cider::IROp::ADD;
        sum.dest = cider::IRValue::Register("s", cider::IRType::I32);
        sum.args = {cider::IRValue::Register("v", cider::IRType::I32),
                    cider::IRValue::Register("w", cider::IRType::I32)};
        header->addInstruction(sum);
        cider::IRInstruction sum2;
        sum2.op = cider::IROp::ADD;
        sum2.dest = cider::IRValue::Register("s2", cider::IRType::I32);
        sum2.args = {cider::IRValue::Register("s", cider::IRType::I32),
                     cider::IRValue::Register("u", cider::IRType::I32)};
        header->addInstruction(sum2);
        cider::IRInstruction inc;
        inc.op = cider::IROp::ADD;
        inc.dest = cider::IRValue::Register("iNext", cider::IRType::I32);
        inc.args = {cider::IRValue::Register("i", cider::IRType::I32),
                    cider::IRValue::Register("one", cider::IRType::I32)};
        header->addInstruction(inc);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_LT;
        cmp.dest = cider::IRValue::Register("cond", cider::IRType::BOOL);
        cmp.args = {cider::IRValue::Register("iNext", cider::IRType::I32),
                    cider::IRValue::Constant(10, cider::IRType::I32)};
        header->addInstruction(cmp);
        cider::IRInstruction cbr;
        cbr.op = cider::IROp::COND_BR;
        cbr.args = {cider::IRValue::Register("cond", cider::IRType::BOOL),
                    cider::IRValue::Label("body"), cider::IRValue::Label("exit")};
        header->addInstruction(cbr);

        auto body = std::make_unique<cider::BasicBlock>("body");
        if (mode == MultiDefPhi) {
            cider::IRInstruction qBack;
            qBack.op = cider::IROp::MOV;
            qBack.dest = cider::IRValue::Register("qBack", cider::IRType::PTR);
            qBack.args = {cider::IRValue::Register("victim", cider::IRType::PTR)};
            body->addInstruction(qBack);
        }
        cider::IRInstruction back;
        back.op = cider::IROp::BR;
        back.args = {cider::IRValue::Label("header")};
        body->addInstruction(back);

        auto exit = std::make_unique<cider::BasicBlock>("exit");
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        exit->addInstruction(ret);

        fn->addBlock(std::move(entry));
        fn->addBlock(std::move(header));
        fn->addBlock(std::move(body));
        fn->addBlock(std::move(exit));
        mod.addFunction(std::move(fn));
        return mod;
    };

    struct Counts { int preSlot, loopSlot, preVictim, loopVictim, preOther, loopOther; };
    auto count = [](cider::ModuleIR& mod) {
        Counts n{0, 0, 0, 0, 0, 0};
        for (const auto& b : mod.getFunctions()[0]->getBlocks()) {
            const bool pre = b->getName().find("preheader") != std::string::npos;
            const bool inLoop = b->getName() == "header";
            for (const auto& in : b->getInstructions()) {
                if (in.op != cider::IROp::LOAD || in.args.empty() ||
                    in.args[0].kind != cider::IRValue::REGISTER) continue;
                const std::string& a = in.args[0].name;
                if (a == "slot") { if (pre) n.preSlot++; else if (inLoop) n.loopSlot++; }
                if (a == "victim") { if (pre) n.preVictim++; else if (inLoop) n.loopVictim++; }
                if (a == "other") { if (pre) n.preOther++; else if (inLoop) n.loopOther++; }
            }
        }
        return n;
    };
    auto checkFallback = [&](cider::ModuleIR& mod) {
        Counts n = count(mod);
        CHECK(n.preSlot == 0);     // written through the chain: stays
        CHECK(n.loopSlot == 1);
        CHECK(n.preVictim == 0);   // hidden base: must stay too
        CHECK(n.loopVictim == 1);
        CHECK(n.preOther == 0);    // conservative fallback: nothing hoists
        CHECK(n.loopOther == 1);
        return 0;
    };
    auto checkPrecise = [&](cider::ModuleIR& mod) {
        Counts n = count(mod);
        CHECK(n.preSlot == 0);     // written through the chain: stays
        CHECK(n.loopSlot == 1);
        CHECK(n.preVictim == 1);   // control: unwritten allocas still hoist
        CHECK(n.loopVictim == 0);
        CHECK(n.preOther == 1);
        CHECK(n.loopOther == 0);
        return 0;
    };

    const Mode modes[] = {MultiDefMov, MultiDefPhi, SingleDefChain};
    for (Mode mode : modes) {
        auto ssaMod = build(mode);
        cider::SSALicmPass licm;
        licm.run(ssaMod);
        CHECK((mode == SingleDefChain ? checkPrecise(ssaMod) : checkFallback(ssaMod)) == 0);

        auto postMod = build(mode);
        cider::PostLicmPass postLicm;
        postLicm.run(postMod);
        CHECK((mode == SingleDefChain ? checkPrecise(postMod) : checkFallback(postMod)) == 0);
    }
    return 0;
}

int testDeepMultiDefEscapeLicm() {
    // CIDER-BUG-160 / CIDER-BUG-086: markEscapedValue resolves an operand
    // that reaches a callee through the same chain walk, and checked only
    // the raw register for multi-defness. With a multi-defined intermediate
    // (`%p = add %q, off`, `%q` defined twice), defMap's first def marked
    // the wrong alloca as escaped, so `slot` — reachable solely through the
    // later def — was not, and its in-loop LOAD hoisted across the in-loop
    // clobber, freezing the first iteration's bytes. The walk now reports
    // the intermediate and escapedUnknown takes over. Control: a
    // single-defined chain still marks precisely, so only the escaping
    // alloca is blocked and the non-escaping one keeps hoisting.
    enum Mode { MultiDef, SingleDef };
    auto build = [](Mode mode) {
        cider::ModuleIR mod;
        auto fn = std::make_unique<cider::FunctionIR>("f", cider::IRType::VOID);

        auto entry = std::make_unique<cider::BasicBlock>("entry");
        auto addAlloca = [&](const char* name, int64_t size) {
            cider::IRInstruction a;
            a.op = cider::IROp::ALLOCA;
            a.dest = cider::IRValue::Register(name, cider::IRType::PTR);
            a.args = {cider::IRValue::Constant(size, cider::IRType::I32)};
            entry->addInstruction(a);
        };
        addAlloca("slot", 64);   // really reaches the callee through `q`
        addAlloca("other", 8);   // never escapes: control alloca
        cider::IRInstruction initOther;
        initOther.op = cider::IROp::STORE;
        initOther.args = {cider::IRValue::Register("other", cider::IRType::PTR),
                          cider::IRValue::Constant(7, cider::IRType::I32),
                          cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(initOther);
        cider::IRInstruction off;
        off.op = cider::IROp::MOV;
        off.dest = cider::IRValue::Register("off", cider::IRType::I64);
        off.args = {cider::IRValue::Constant(0, cider::IRType::I64)};
        entry->addInstruction(off);
        // `q` is the escape-operand intermediate; defMap keeps only its
        // first def, so a base carried by a later def is hidden.
        if (mode == MultiDef) {
            cider::IRInstruction qOther;
            qOther.op = cider::IROp::MOV;
            qOther.dest = cider::IRValue::Register("q", cider::IRType::PTR);
            qOther.args = {cider::IRValue::Register("other", cider::IRType::PTR)};
            entry->addInstruction(qOther);
        }
        cider::IRInstruction qSlot;
        qSlot.op = cider::IROp::MOV;
        qSlot.dest = cider::IRValue::Register("q", cider::IRType::PTR);
        qSlot.args = {cider::IRValue::Register("slot", cider::IRType::PTR)};
        entry->addInstruction(qSlot);
        cider::IRInstruction p;
        p.op = cider::IROp::ADD;
        p.dest = cider::IRValue::Register("p", cider::IRType::PTR);
        p.args = {cider::IRValue::Register("q", cider::IRType::PTR),
                  cider::IRValue::Register("off", cider::IRType::I64)};
        entry->addInstruction(p);
        // Escape site outside the loop: the callee can keep the pointer and
        // later write `slot` through it. `q`'s later def is the real one.
        cider::IRInstruction esc;
        esc.op = cider::IROp::CALL;
        esc.args = {cider::IRValue::Label("sink"),
                    cider::IRValue::Register("p", cider::IRType::PTR)};
        entry->addInstruction(esc);
        cider::IRInstruction i0;
        i0.op = cider::IROp::MOV;
        i0.dest = cider::IRValue::Register("i0", cider::IRType::I32);
        i0.args = {cider::IRValue::Constant(0, cider::IRType::I32)};
        entry->addInstruction(i0);
        cider::IRInstruction one;
        one.op = cider::IROp::MOV;
        one.dest = cider::IRValue::Register("one", cider::IRType::I32);
        one.args = {cider::IRValue::Constant(1, cider::IRType::I32)};
        entry->addInstruction(one);
        cider::IRInstruction enter;
        enter.op = cider::IROp::BR;
        enter.args = {cider::IRValue::Label("header")};
        entry->addInstruction(enter);

        auto header = std::make_unique<cider::BasicBlock>("header");
        cider::IRInstruction i;
        i.op = cider::IROp::PHI;
        i.dest = cider::IRValue::Register("i", cider::IRType::I32);
        i.phiArgs = {{"entry", cider::IRValue::Register("i0", cider::IRType::I32)},
                     {"body", cider::IRValue::Register("iNext", cider::IRType::I32)}};
        header->addInstruction(i);
        cider::IRInstruction clobber;
        clobber.op = cider::IROp::CALL;
        clobber.args = {cider::IRValue::Label("touch")};
        header->addInstruction(clobber);
        cider::IRInstruction ldSlot;
        ldSlot.op = cider::IROp::LOAD;
        ldSlot.dest = cider::IRValue::Register("v", cider::IRType::I32);
        ldSlot.args = {cider::IRValue::Register("slot", cider::IRType::PTR),
                       cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldSlot);
        cider::IRInstruction ldOther;
        ldOther.op = cider::IROp::LOAD;
        ldOther.dest = cider::IRValue::Register("w", cider::IRType::I32);
        ldOther.args = {cider::IRValue::Register("other", cider::IRType::PTR),
                        cider::IRValue::Constant(0, cider::IRType::I32)};
        header->addInstruction(ldOther);
        cider::IRInstruction sum;
        sum.op = cider::IROp::ADD;
        sum.dest = cider::IRValue::Register("s", cider::IRType::I32);
        sum.args = {cider::IRValue::Register("v", cider::IRType::I32),
                    cider::IRValue::Register("w", cider::IRType::I32)};
        header->addInstruction(sum);
        cider::IRInstruction inc;
        inc.op = cider::IROp::ADD;
        inc.dest = cider::IRValue::Register("iNext", cider::IRType::I32);
        inc.args = {cider::IRValue::Register("i", cider::IRType::I32),
                    cider::IRValue::Register("one", cider::IRType::I32)};
        header->addInstruction(inc);
        cider::IRInstruction cmp;
        cmp.op = cider::IROp::CMP_LT;
        cmp.dest = cider::IRValue::Register("cond", cider::IRType::BOOL);
        cmp.args = {cider::IRValue::Register("iNext", cider::IRType::I32),
                    cider::IRValue::Constant(10, cider::IRType::I32)};
        header->addInstruction(cmp);
        cider::IRInstruction cbr;
        cbr.op = cider::IROp::COND_BR;
        cbr.args = {cider::IRValue::Register("cond", cider::IRType::BOOL),
                    cider::IRValue::Label("body"), cider::IRValue::Label("exit")};
        header->addInstruction(cbr);

        auto body = std::make_unique<cider::BasicBlock>("body");
        cider::IRInstruction back;
        back.op = cider::IROp::BR;
        back.args = {cider::IRValue::Label("header")};
        body->addInstruction(back);

        auto exit = std::make_unique<cider::BasicBlock>("exit");
        cider::IRInstruction ret;
        ret.op = cider::IROp::RET;
        exit->addInstruction(ret);

        fn->addBlock(std::move(entry));
        fn->addBlock(std::move(header));
        fn->addBlock(std::move(body));
        fn->addBlock(std::move(exit));
        mod.addFunction(std::move(fn));
        return mod;
    };

    struct Counts { int preSlot, loopSlot, preOther, loopOther; };
    auto count = [](cider::ModuleIR& mod) {
        Counts n{0, 0, 0, 0};
        for (const auto& b : mod.getFunctions()[0]->getBlocks()) {
            const bool pre = b->getName().find("preheader") != std::string::npos;
            const bool inLoop = b->getName() == "header";
            for (const auto& in : b->getInstructions()) {
                if (in.op != cider::IROp::LOAD || in.args.empty() ||
                    in.args[0].kind != cider::IRValue::REGISTER) continue;
                const std::string& a = in.args[0].name;
                if (a == "slot") { if (pre) n.preSlot++; else if (inLoop) n.loopSlot++; }
                if (a == "other") { if (pre) n.preOther++; else if (inLoop) n.loopOther++; }
            }
        }
        return n;
    };
    auto checkEscape = [&](cider::ModuleIR& mod) {
        Counts n = count(mod);
        CHECK(n.preSlot == 0);     // escaped alloca: no hoist across the clobber
        CHECK(n.loopSlot == 1);
        CHECK(n.preOther == 0);    // conservative fallback: nothing hoists
        CHECK(n.loopOther == 1);
        return 0;
    };
    auto checkPrecise = [&](cider::ModuleIR& mod) {
        Counts n = count(mod);
        CHECK(n.preSlot == 0);     // only the escaping alloca is blocked
        CHECK(n.loopSlot == 1);
        CHECK(n.preOther == 1);    // non-escaping control still hoists
        CHECK(n.loopOther == 0);
        return 0;
    };

    const Mode modes[] = {MultiDef, SingleDef};
    for (Mode mode : modes) {
        auto ssaMod = build(mode);
        cider::SSALicmPass licm;
        licm.run(ssaMod);
        CHECK((mode == SingleDef ? checkPrecise(ssaMod) : checkEscape(ssaMod)) == 0);

        auto postMod = build(mode);
        cider::PostLicmPass postLicm;
        postLicm.run(postMod);
        CHECK((mode == SingleDef ? checkPrecise(postMod) : checkEscape(postMod)) == 0);
    }
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

int testInlinerSiteBudget() {
    // CIDER-BUG-162: cloned call sites must not feed back into the inliner's
    // gain term. `mk` is called at `sites` places; its body destroys a local
    // whose destructor calls each level below twice, so inlining clones both
    // the destructor call and (in later fixpoint rounds) the destructor body
    // itself. If every round re-counted those compiler-generated sites, the
    // site term would double per round and the inliner would duplicate the IR
    // exponentially. The budget is captured once and only consumed, so the
    // body stays linear in the source call sites.
    constexpr int kSites = 10;
    constexpr int kLevels = 6;
    std::string src = "var drops: int = 0;\n";
    src += "func p0() -> int { return 1; }\n";
    for (int i = 1; i <= kLevels; ++i) {
        const std::string prev = "p" + std::to_string(i - 1);
        src += "func p" + std::to_string(i) + "() -> int { return " + prev +
               "() + " + prev + "(); }\n";
    }
    const std::string top = "p" + std::to_string(kLevels);
    src += "class D {\n  var label: int;\n  func ~D() {\n";
    src += "    drops = drops + " + top + "();\n";
    src += "    drops = drops + " + top + "();\n";
    src += "  }\n}\n";
    src += "func mk(n: int) -> D {\n  var d: D;\n  d.label = n;\n  return d;\n}\n";
    src += "func main() -> int {\n  var total: int = 0;\n";
    for (int i = 0; i < kSites; ++i) {
        src += "  total = total + mk(" + std::to_string(i) + ").label;\n";
    }
    src += "  return total;\n}\n";

    auto r = genModule(src);
    auto countInstrs = [&]() {
        size_t n = 0;
        for (const auto& f : r.module.getFunctions()) {
            for (const auto& b : f->getBlocks()) n += b->getInstructions().size();
        }
        return n;
    };
    const size_t before = countInstrs();
    cider::Inliner<> inliner;
    for (int pass = 0; pass < 10; ++pass) {
        if (!inliner.run(r.module)) break;
    }
    const size_t after = countInstrs();
    // Pre-fix this reaches ~8.7k instructions (doubling each round); the
    // bounded budget keeps the 10-site shape around 300.
    CHECK(after < 1000);
    CHECK(after >= before);  // inlining only substitutes bodies
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
    CHECK(testVolatileGap101IrFlags() == 0);
    CHECK(testVolatileGap133IrFlags() == 0);
    CHECK(testVolatileFormation144IrFlags() == 0);
    CHECK(testVolatileArraySpelling145IrFlags() == 0);
    CHECK(testVolatileLiteral146IrFlags() == 0);
    CHECK(testVolatileAggregateConvert167IrFlags() == 0);
    CHECK(testVolatileGvn() == 0);
    CHECK(testVolatileLicm() == 0);
    CHECK(testCallClobberLicm() == 0);
    CHECK(testDerivedStoreLicm() == 0);
    CHECK(testMultiDefStoreLicm() == 0);
    CHECK(testUnknownStoreAddressLicm() == 0);
    CHECK(testDeepMultiDefStoreLicm() == 0);
    CHECK(testDeepMultiDefEscapeLicm() == 0);
    CHECK(testInliner() == 0);
    CHECK(testInlinerSiteBudget() == 0);
    return 0;
}

int main() { return run(); }
