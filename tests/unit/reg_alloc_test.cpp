// Register allocator unit tests (CID-019: spill-slot reuse via CFG
// liveness). The allocator keeps its register decisions unchanged and
// only recycles 8-byte spill slots between values whose CFG-liveness
// block ranges are disjoint.
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "reg_alloc.h"

#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

using cider::BasicBlock;
using cider::FunctionIR;
using cider::IRInstruction;
using cider::IROp;
using cider::IRType;
using cider::IRValue;

// %dst = 0 + 1 — a pure definition (no register reads).
static IRInstruction makeDef(const std::string& dst) {
    IRInstruction in;
    in.op = IROp::ADD;
    in.dest = IRValue::Register(dst, IRType::I32);
    in.args = {IRValue::Constant(0, IRType::I32),
               IRValue::Constant(1, IRType::I32)};
    return in;
}

// push %src — a pure use.
static IRInstruction makeUse(const std::string& src) {
    IRInstruction in;
    in.op = IROp::PUSH;
    in.args = {IRValue::Register(src, IRType::I32)};
    return in;
}

static IRInstruction makeBr(const std::string& target) {
    IRInstruction in;
    in.op = IROp::BR;
    in.args = {IRValue::Label(target)};
    return in;
}

static IRInstruction makeCondBr(const std::string& cond, const std::string& t,
                                const std::string& f) {
    IRInstruction in;
    in.op = IROp::COND_BR;
    in.args = {IRValue::Register(cond, IRType::I32), IRValue::Label(t),
               IRValue::Label(f)};
    return in;
}

static IRInstruction makeRet() {
    IRInstruction in;
    in.op = IROp::RET;
    return in;
}

// Two values with disjoint live ranges share a slot when both spill.
struct SpillSummary {
    int count = 0;
    std::set<int> offsets;
    std::map<std::string, int> byName;
};

static SpillSummary summarize(
    const std::map<std::string, cider::AllocationResult>& result) {
    SpillSummary s;
    for (const auto& [name, res] : result) {
        if (res.isSpilled) {
            s.count++;
            s.offsets.insert(res.stackOffset);
            s.byName[name] = res.stackOffset;
        }
    }
    return s;
}

int run() {
    // ---- Registers only: no spills, no stack. ----
    auto fn = std::make_unique<FunctionIR>("f", IRType::VOID);
    auto b0 = std::make_unique<BasicBlock>("b0");
    b0->addInstruction(makeUse("a"));
    b0->addInstruction(makeBr("b3"));
    auto b1 = std::make_unique<BasicBlock>("b1");
    b1->addInstruction(makeBr("b3"));
    auto b2 = std::make_unique<BasicBlock>("b2");
    b2->addInstruction(makeBr("b3"));
    auto b3 = std::make_unique<BasicBlock>("b3");
    b3->addInstruction(makeUse("b"));
    b3->addInstruction(makeRet());
    fn->addBlock(std::move(b0));
    fn->addBlock(std::move(b1));
    fn->addBlock(std::move(b2));
    fn->addBlock(std::move(b3));

    cider::RegisterAllocator alloc;
    int stackSize = 0;
    auto result = alloc.allocate(*fn, stackSize);
    CHECK(stackSize == 0);  // a and b both in registers
    CHECK(!result.at("a").isSpilled);
    CHECK(!result.at("b").isSpilled);

    // ---- Disjoint live ranges share one slot (CID-019). ----
    // Wave 1 (v0..v5) is live only in b0; wave 2 (u0..u5) only in b1.
    // With 5 int registers each wave spills one value, and the two
    // spilled values have disjoint block ranges, so they share a slot:
    // stack size is 8, not 16.
    auto fn2 = std::make_unique<FunctionIR>("disjoint", IRType::VOID);
    auto d0 = std::make_unique<BasicBlock>("d0");
    for (int i = 0; i < 6; ++i) d0->addInstruction(makeDef("v" + std::to_string(i)));
    d0->addInstruction(makeBr("d1"));
    auto d1 = std::make_unique<BasicBlock>("d1");
    for (int i = 0; i < 6; ++i) d1->addInstruction(makeDef("u" + std::to_string(i)));
    d1->addInstruction(makeRet());
    fn2->addBlock(std::move(d0));
    fn2->addBlock(std::move(d1));

    cider::RegisterAllocator alloc2;
    int stackSize2 = 0;
    auto result2 = alloc2.allocate(*fn2, stackSize2);
    SpillSummary sum2 = summarize(result2);
    CHECK(sum2.count == 2);          // one spill per wave
    CHECK(sum2.offsets.size() == 1); // ... sharing a single slot
    CHECK(stackSize2 == 8);
    {
        // The two spilled values are one from each wave and share the slot.
        const int* wave1 = nullptr;
        const int* wave2 = nullptr;
        for (int i = 0; i < 6; ++i) {
            auto a = result2.find("v" + std::to_string(i));
            if (a != result2.end() && a->second.isSpilled) wave1 = &a->second.stackOffset;
            auto b = result2.find("u" + std::to_string(i));
            if (b != result2.end() && b->second.isSpilled) wave2 = &b->second.stackOffset;
        }
        CHECK(wave1 && wave2 && *wave1 == *wave2);
    }

    // ---- Overlapping live ranges must NOT share a slot. ----
    // Wave 1 is defined in o0 and used in o2; wave 2 is defined in o1 and
    // used in o2. Both waves are live together in o1/o2, so no reuse.
    auto fn3 = std::make_unique<FunctionIR>("overlap", IRType::VOID);
    auto o0 = std::make_unique<BasicBlock>("o0");
    for (int i = 0; i < 6; ++i) o0->addInstruction(makeDef("x" + std::to_string(i)));
    o0->addInstruction(makeBr("o1"));
    auto o1 = std::make_unique<BasicBlock>("o1");
    for (int i = 0; i < 6; ++i) o1->addInstruction(makeDef("y" + std::to_string(i)));
    o1->addInstruction(makeBr("o2"));
    auto o2 = std::make_unique<BasicBlock>("o2");
    for (int i = 0; i < 6; ++i) o2->addInstruction(makeUse("x" + std::to_string(i)));
    for (int i = 0; i < 6; ++i) o2->addInstruction(makeUse("y" + std::to_string(i)));
    o2->addInstruction(makeRet());
    fn3->addBlock(std::move(o0));
    fn3->addBlock(std::move(o1));
    fn3->addBlock(std::move(o2));

    cider::RegisterAllocator alloc3;
    int stackSize3 = 0;
    auto result3 = alloc3.allocate(*fn3, stackSize3);
    SpillSummary sum3 = summarize(result3);
    CHECK(sum3.count == 7);            // 1 from x, 6 from y
    CHECK(sum3.offsets.size() == 7);   // every spill keeps its own slot
    CHECK(stackSize3 == 7 * 8);

    // ---- Loop-carried value keeps its slot across the back edge. ----
    // f0..f4 hold all five registers from l0 to l3; p (a loop PHI dest)
    // and q (a body temp) both spill. q is defined in the latch l2 and
    // flows back into the header's PHI, so p/q's ranges overlap at l2
    // and they must not share a slot.
    auto fn4 = std::make_unique<FunctionIR>("loop", IRType::VOID);
    auto l0 = std::make_unique<BasicBlock>("l0");
    for (int i = 0; i < 5; ++i) l0->addInstruction(makeDef("f" + std::to_string(i)));
    l0->addInstruction(makeBr("l1"));
    auto l1 = std::make_unique<BasicBlock>("l1");
    {
        IRInstruction phi;
        phi.op = IROp::PHI;
        phi.dest = IRValue::Register("p", IRType::I32);
        phi.phiArgs["l0"] = IRValue::Register("init", IRType::I32);
        phi.phiArgs["l2"] = IRValue::Register("q", IRType::I32);
        l1->addInstruction(phi);
    }
    l1->addInstruction(makeUse("p"));
    l1->addInstruction(makeCondBr("c", "l2", "l3"));
    auto l2 = std::make_unique<BasicBlock>("l2");
    l2->addInstruction(makeDef("q"));
    l2->addInstruction(makeBr("l1"));
    auto l3 = std::make_unique<BasicBlock>("l3");
    for (int i = 0; i < 5; ++i) l3->addInstruction(makeUse("f" + std::to_string(i)));
    l3->addInstruction(makeUse("p"));
    l3->addInstruction(makeRet());
    fn4->addBlock(std::move(l0));
    fn4->addBlock(std::move(l1));
    fn4->addBlock(std::move(l2));
    fn4->addBlock(std::move(l3));

    cider::RegisterAllocator alloc4;
    int stackSize4 = 0;
    auto result4 = alloc4.allocate(*fn4, stackSize4);
    SpillSummary sum4 = summarize(result4);
    CHECK(sum4.byName.count("p") == 1);  // p spills (registers held by f0..f4)
    CHECK(sum4.byName.count("q") == 1);  // q spills too
    CHECK(sum4.byName["p"] != sum4.byName["q"]);  // overlapping at l2 -> separate slots
    CHECK(stackSize4 == (int)(sum4.offsets.size() * 8));

    // ---- Issue #10 shape: value live across an inlined region. ----
    // e0/i0 model the caller split by inlining: e0 holds all five
    // registers, i0 defines `across` before the call site, i1/i2 are the
    // inlined callee's loop, i3 is the inlined exit and a4 the caller's
    // "after" block. `across` runs from i0 to a4; the inlined temp t is
    // defined in i1 and used in a4. Both are live in i3 (i1 branches
    // around i2 to i3) and a4, so they must not share a slot.
    auto fn5 = std::make_unique<FunctionIR>("inline_shape", IRType::VOID);
    auto e0 = std::make_unique<BasicBlock>("e0");
    for (int i = 0; i < 5; ++i) e0->addInstruction(makeDef("g" + std::to_string(i)));
    e0->addInstruction(makeBr("i0"));
    auto i0 = std::make_unique<BasicBlock>("i0");
    i0->addInstruction(makeDef("across"));
    i0->addInstruction(makeBr("i1"));
    auto i1 = std::make_unique<BasicBlock>("i1");
    i1->addInstruction(makeDef("t"));
    i1->addInstruction(makeCondBr("c", "i3", "i2"));  // exit or body
    auto i2 = std::make_unique<BasicBlock>("i2");
    i2->addInstruction(makeDef("h"));
    i2->addInstruction(makeBr("i1"));  // inlined loop back edge
    auto i3 = std::make_unique<BasicBlock>("i3");
    for (int i = 0; i < 5; ++i) i3->addInstruction(makeUse("g" + std::to_string(i)));
    i3->addInstruction(makeBr("a4"));
    auto a4 = std::make_unique<BasicBlock>("a4");
    a4->addInstruction(makeUse("across"));
    a4->addInstruction(makeUse("t"));
    a4->addInstruction(makeRet());
    fn5->addBlock(std::move(e0));
    fn5->addBlock(std::move(i0));
    fn5->addBlock(std::move(i1));
    fn5->addBlock(std::move(i2));
    fn5->addBlock(std::move(i3));
    fn5->addBlock(std::move(a4));

    cider::RegisterAllocator alloc5;
    int stackSize5 = 0;
    auto result5 = alloc5.allocate(*fn5, stackSize5);
    SpillSummary sum5 = summarize(result5);
    CHECK(sum5.byName.count("across") == 1);
    CHECK(sum5.byName.count("t") == 1);
    CHECK(sum5.byName["across"] != sum5.byName["t"]);
    CHECK(stackSize5 == (int)(sum5.offsets.size() * 8));

    // ---- Conservative fallback: unreachable block values keep fresh slots. ----
    // Reachable s/r waves reuse one slot as in the disjoint case, but the
    // unreachable block q's spill gets a fresh slot of its own: 16 bytes.
    auto fn6 = std::make_unique<FunctionIR>("unreachable", IRType::VOID);
    auto m0 = std::make_unique<BasicBlock>("m0");
    for (int i = 0; i < 6; ++i) m0->addInstruction(makeDef("s" + std::to_string(i)));
    m0->addInstruction(makeBr("m1"));
    auto m1 = std::make_unique<BasicBlock>("m1");
    for (int i = 0; i < 6; ++i) m1->addInstruction(makeDef("r" + std::to_string(i)));
    m1->addInstruction(makeRet());
    auto m2 = std::make_unique<BasicBlock>("m2");  // unreachable, well-formed
    for (int i = 0; i < 6; ++i) m2->addInstruction(makeDef("q" + std::to_string(i)));
    m2->addInstruction(makeRet());
    fn6->addBlock(std::move(m0));
    fn6->addBlock(std::move(m1));
    fn6->addBlock(std::move(m2));

    cider::RegisterAllocator alloc6;
    int stackSize6 = 0;
    auto result6 = alloc6.allocate(*fn6, stackSize6);
    SpillSummary sum6 = summarize(result6);
    CHECK(sum6.count == 3);            // one spill per wave/block
    CHECK(sum6.offsets.size() == 2);   // reachable pair shares; q gets a fresh slot
    CHECK(stackSize6 == 16);

    // ---- Conservative fallback: unreachable malformed block. ----
    // n2 has no terminator but nothing branches to it (dead code): its
    // values keep fresh slots while the reachable waves still share one.
    auto fn7 = std::make_unique<FunctionIR>("unreachable_malformed", IRType::VOID);
    auto n0 = std::make_unique<BasicBlock>("n0");
    for (int i = 0; i < 6; ++i) n0->addInstruction(makeDef("s" + std::to_string(i)));
    n0->addInstruction(makeBr("n1"));
    auto n1 = std::make_unique<BasicBlock>("n1");
    for (int i = 0; i < 6; ++i) n1->addInstruction(makeDef("r" + std::to_string(i)));
    n1->addInstruction(makeRet());
    auto n2 = std::make_unique<BasicBlock>("n2");  // no terminator at all
    for (int i = 0; i < 6; ++i) n2->addInstruction(makeDef("bogus" + std::to_string(i)));
    fn7->addBlock(std::move(n0));
    fn7->addBlock(std::move(n1));
    fn7->addBlock(std::move(n2));

    cider::RegisterAllocator alloc7;
    int stackSize7 = 0;
    auto result7 = alloc7.allocate(*fn7, stackSize7);
    SpillSummary sum7 = summarize(result7);
    CHECK(sum7.count == 3);
    CHECK(sum7.offsets.size() == 2);  // s/r share; bogus is fresh
    CHECK(stackSize7 == 16);

    // ---- Conservative fallback: reachable malformed block disables reuse. ----
    // n1 branches to n2, which has no terminator (e.g. an IR call that
    // never returns): its successors are unknown, so the whole function
    // falls back to fresh slots — today's behavior, 24 bytes.
    auto fn8 = std::make_unique<FunctionIR>("reachable_malformed", IRType::VOID);
    auto p0 = std::make_unique<BasicBlock>("p0");
    for (int i = 0; i < 6; ++i) p0->addInstruction(makeDef("s" + std::to_string(i)));
    p0->addInstruction(makeBr("p1"));
    auto p1 = std::make_unique<BasicBlock>("p1");
    for (int i = 0; i < 6; ++i) p1->addInstruction(makeDef("r" + std::to_string(i)));
    p1->addInstruction(makeBr("p2"));
    auto p2 = std::make_unique<BasicBlock>("p2");  // reachable, no terminator
    for (int i = 0; i < 6; ++i) p2->addInstruction(makeDef("bogus" + std::to_string(i)));
    fn8->addBlock(std::move(p0));
    fn8->addBlock(std::move(p1));
    fn8->addBlock(std::move(p2));

    cider::RegisterAllocator alloc8;
    int stackSize8 = 0;
    auto result8 = alloc8.allocate(*fn8, stackSize8);
    SpillSummary sum8 = summarize(result8);
    CHECK(sum8.count == 3);
    CHECK(sum8.offsets.size() == 3);  // no reuse anywhere
    CHECK(stackSize8 == 24);

    // ---- PHI edge use from the PHI's own block (CID-019). ----
    // x is defined earlier in h1, the block that holds the PHI, so it is
    // not an upward-exposed use of h1 (USE[h1] does not see it). The PHI
    // reads x only when control enters h1 from the latch d2: the operand
    // lives on edge d2 -> h1 and loops back into its own defining block.
    // Only the per-edge PHI use keeps x live-out of d2/live-in d2, i.e.
    // live from h1 through d2. w is defined in d2 and would otherwise
    // recycle x's slot: with the edge use, x is live through d2 and w
    // needs its own slot — 4 slots, 32 bytes, x and w at distinct
    // offsets. Without the edge use (or with the edge keyed to the wrong
    // block), x dies in h1 and w takes its slot — 3 slots, 24 bytes, the
    // same offset. c plus f0..f4 exhaust the five int registers in h0,
    // so x, p and w always spill; which one of c/f0..f4 spills depends
    // on tie order in the linear scan and does not affect either check.
    auto fn9 = std::make_unique<FunctionIR>("phi_edge_use", IRType::VOID);
    auto h0 = std::make_unique<BasicBlock>("h0");
    for (int i = 0; i < 5; ++i) h0->addInstruction(makeDef("f" + std::to_string(i)));
    h0->addInstruction(makeDef("c"));  // loop condition
    h0->addInstruction(makeBr("h1"));
    auto h1 = std::make_unique<BasicBlock>("h1");
    h1->addInstruction(makeDef("x"));  // defined before the PHI, same block
    {
        IRInstruction phi;
        phi.op = IROp::PHI;
        phi.dest = IRValue::Register("p", IRType::I32);
        phi.phiArgs["h0"] = IRValue::Constant(0, IRType::I32);
        phi.phiArgs["d2"] = IRValue::Register("x", IRType::I32);
        h1->addInstruction(phi);
    }
    h1->addInstruction(makeUse("p"));
    h1->addInstruction(makeBr("d2"));
    auto d2 = std::make_unique<BasicBlock>("d2");
    d2->addInstruction(makeDef("w"));
    d2->addInstruction(makeCondBr("c", "h1", "end"));  // back edge d2 -> h1
    auto end = std::make_unique<BasicBlock>("end");
    for (int i = 0; i < 5; ++i) end->addInstruction(makeUse("f" + std::to_string(i)));
    end->addInstruction(makeUse("p"));
    end->addInstruction(makeRet());
    fn9->addBlock(std::move(h0));
    fn9->addBlock(std::move(h1));
    fn9->addBlock(std::move(d2));
    fn9->addBlock(std::move(end));

    cider::RegisterAllocator alloc9;
    int stackSize9 = 0;
    auto result9 = alloc9.allocate(*fn9, stackSize9);
    SpillSummary sum9 = summarize(result9);
    CHECK(sum9.byName.count("x") == 1);  // x spills (f0..f4 hold the registers)
    CHECK(sum9.byName.count("w") == 1);  // w spills too
    CHECK(sum9.byName["x"] != sum9.byName["w"]);  // edge keeps x live -> no sharing
    CHECK(stackSize9 == 32);  // x stays live through d2; four distinct slots

    return 0;
}

int main() { return run(); }
