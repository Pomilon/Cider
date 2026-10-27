// regalloc_test.cpp — the local linear scan's contract, without an emitter.
//
// These tests build the lowered IR shape codegen receives and inspect the
// homes the allocator answers. They pin the safe scope: one block, one
// definition, no floats, no fixed-scratch instructions, no values that cross
// a call/asm/trap/volatile barrier, and per-ABI caller-saved register pools.

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "codegen/regalloc.h"
#include "types/signature.h"

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

cider::ir::Value value(uint32_t id,
                       cider::ir::IRType width = cider::ir::IRType::I64) {
    cider::ir::Value out;
    out.id = id;
    out.width = width;
    return out;
}

cider::ir::Instr define(cider::ir::Op op, uint32_t id,
                        cider::ir::IRType width = cider::ir::IRType::I64) {
    cider::ir::Instr out;
    out.op = op;
    out.width = width;
    out.isPure = true;
    out.results.push_back(value(id, width));
    return out;
}

cider::ir::Instr binary(cider::ir::Op op, uint32_t result,
                        std::initializer_list<cider::ir::Value> operands,
                        cider::ir::IRType width = cider::ir::IRType::I64) {
    cider::ir::Instr out;
    out.op = op;
    out.width = width;
    out.isPure = true;
    out.results.push_back(value(result, width));
    for (const cider::ir::Value& operand : operands) {
        out.operands.push_back(operand);
    }
    return out;
}

struct Fn {
    cider::ir::Function fn;

    explicit Fn(cider::Abi abi = cider::Abi::SysV) {
        fn.name = "f";
        fn.signature = cider::Signature({}, nullptr, abi);
        cider::ir::Block block;
        block.label = 0;
        fn.blocks.push_back(std::move(block));
    }

    void add(uint32_t block, cider::ir::Instr instr) {
        fn.blocks[block].body.push_back(std::move(instr));
    }

    void term(uint32_t block, uint32_t target) {
        fn.blocks[block].term.term = cider::ir::Term::Br;
        fn.blocks[block].term.target = target;
    }
};

bool isReg(const cider::codegen::FunctionAllocation& alloc, uint32_t block,
           uint32_t id, cider::codegen::Register reg) {
    const auto& home = alloc.blocks[block].home;
    return id < home.size() &&
           home[id] == static_cast<uint8_t>(reg);
}

bool hasHome(const cider::codegen::FunctionAllocation& alloc, uint32_t block,
             uint32_t id) {
    const auto& home = alloc.blocks[block].home;
    return id < home.size() &&
           home[id] != static_cast<uint8_t>(cider::codegen::Register::None);
}

cider::codegen::Register homeOf(const cider::codegen::FunctionAllocation& alloc,
                                uint32_t block, uint32_t id) {
    const auto& home = alloc.blocks[block].home;
    if (id >= home.size()) {
        return cider::codegen::Register::None;
    }
    return static_cast<cider::codegen::Register>(home[id]);
}

bool hasCalleeSaved(const cider::codegen::FunctionAllocation& alloc,
                    cider::codegen::Register reg) {
    for (cider::codegen::Register used : alloc.calleeSaved) {
        if (used == reg) {
            return true;
        }
    }
    return false;
}

bool allHomesInPool(const cider::codegen::FunctionAllocation& alloc,
                    uint32_t block, uint32_t top,
                    const std::vector<cider::codegen::Register>& pool) {
    for (uint32_t id = 0; id <= top; ++id) {
        const auto& home = alloc.blocks[block].home;
        if (id >= home.size() ||
            home[id] == static_cast<uint8_t>(cider::codegen::Register::None)) {
            continue;
        }
        bool found = false;
        for (cider::codegen::Register reg : pool) {
            if (home[id] == static_cast<uint8_t>(reg)) {
                found = true;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

void testReuse() {
    std::printf("local reuse\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
    f.add(0, binary(cider::ir::Op::Add, 2, {value(1), value(1)}));
    f.add(0, binary(cider::ir::Op::Add, 3, {value(2), value(2)}));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(3));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(isReg(alloc, 0, 0, cider::codegen::Register::R8),
          "the first interval takes r8");
    check(isReg(alloc, 0, 1, cider::codegen::Register::R9),
          "a value overlapping the previous definition gets the next register");
    check(isReg(alloc, 0, 2, cider::codegen::Register::R8),
          "a register is reused after its interval ends");
    check(alloc.valuesAllocated == 4 && alloc.valuesSpilled == 0,
          "four short intervals, no spills");
}

void testPressure() {
    std::printf("spill-on-pressure\n");
    Fn f;
    for (uint32_t id = 0; id < 6; ++id) {
        f.add(0, define(cider::ir::Op::Const, id));
    }
    cider::ir::Instr joined;
    joined.op = cider::ir::Op::Add;
    joined.width = cider::ir::IRType::I64;
    for (uint32_t id = 0; id < 6; ++id) {
        joined.operands.push_back(value(id));
    }
    joined.results.push_back(value(6));
    f.add(0, std::move(joined));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(alloc.valuesSpilled == 1,
          "a sixth simultaneous SysV interval spills");
    check(alloc.valuesAllocated == 5,
          "the five caller-saved SysV registers carry the rest");
    std::vector<cider::codegen::Register> pool = {
        cider::codegen::Register::R8, cider::codegen::Register::R9,
        cider::codegen::Register::R10, cider::codegen::Register::Rdi,
        cider::codegen::Register::Rsi};
    check(allHomesInPool(alloc, 0, 5, pool),
          "every spill leaves a value in the SysV pool");
    check(isReg(alloc, 0, 5, cider::codegen::Register::None),
          "the last interval is the one that keeps its stack slot");
}

void testMsAbiPool() {
    std::printf("MS ABI caller-saved pool\n");
    Fn f(cider::Abi::MsAbi);
    for (uint32_t id = 0; id < 4; ++id) {
        f.add(0, define(cider::ir::Op::Const, id));
    }
    cider::ir::Instr joined;
    joined.op = cider::ir::Op::Add;
    joined.width = cider::ir::IRType::I64;
    for (uint32_t id = 0; id < 4; ++id) {
        joined.operands.push_back(value(id));
    }
    f.add(0, std::move(joined));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    std::vector<cider::codegen::Register> pool = {
        cider::codegen::Register::R8, cider::codegen::Register::R9,
        cider::codegen::Register::R10};
    check(allHomesInPool(alloc, 0, 3, pool),
          "msabi never homes a value in nonvolatile rdi/rsi");
    check(alloc.valuesAllocated == 3 && alloc.valuesSpilled == 1,
          "the msabi row holds three, then spills");
}

void testBarriers() {
    std::printf("barriers\n");
    {
        Fn f;
        f.add(0, define(cider::ir::Op::Const, 0));
        cider::ir::Instr call;
        call.op = cider::ir::Op::Call;
        call.callee = "g";
        f.add(0, std::move(call));
        f.add(0, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
        f.add(0, binary(cider::ir::Op::Add, 2, {value(1), value(1)}));
        f.fn.blocks[0].term.term = cider::ir::Term::Ret;
        f.fn.blocks[0].term.values.push_back(value(2));
        const auto alloc = cider::codegen::allocateFunction(f.fn);
        check(!hasHome(alloc, 0, 0), "a value live across a call keeps its slot");
        check(hasHome(alloc, 0, 1), "a value born after the call may use a register");
        check(hasHome(alloc, 0, 2), "the post-call range continues in a register");
    }
    {
        Fn f;
        f.add(0, define(cider::ir::Op::Const, 0));
        cider::ir::Instr asmInstr;
        asmInstr.op = cider::ir::Op::Asm;
        asmInstr.asmText = "nop";
        f.add(0, std::move(asmInstr));
        f.add(0, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
        const auto alloc = cider::codegen::allocateFunction(f.fn);
        check(!hasHome(alloc, 0, 0), "asm is a barrier");
    }
    {
        Fn f;
        f.add(0, define(cider::ir::Op::Const, 0));
        cider::ir::Instr trap;
        trap.op = cider::ir::Op::Trap;
        f.add(0, std::move(trap));
        f.add(0, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
        const auto alloc = cider::codegen::allocateFunction(f.fn);
        check(!hasHome(alloc, 0, 0), "an explicit trap is a barrier");
    }
    {
        Fn f;
        f.add(0, define(cider::ir::Op::Const, 0));
        cider::ir::Instr store;
        store.op = cider::ir::Op::Store;
        store.isVolatile = true;
        store.addressBase = value(9);
        store.storedValue = value(0);
        f.add(0, std::move(store));
        f.add(0, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
        const auto alloc = cider::codegen::allocateFunction(f.fn);
        check(!hasHome(alloc, 0, 0),
              "a value read after a volatile access keeps its slot");
    }
}

void testFixedScratch() {
    std::printf("fixed scratch instructions\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, define(cider::ir::Op::Const, 1));
    f.add(0, binary(cider::ir::Op::Div, 2, {value(0), value(1)}));
    f.add(0, binary(cider::ir::Op::Shl, 3, {value(2), value(1)}));
    f.add(0, binary(cider::ir::Op::Mul, 4, {value(0), value(1)},
                     cider::ir::IRType::I8));
    cider::ir::Instr alloca;
    alloca.op = cider::ir::Op::Alloca;
    alloca.width = cider::ir::IRType::Ptr;
    alloca.results.push_back(value(5, cider::ir::IRType::Ptr));
    f.add(0, std::move(alloca));
    f.add(0, binary(cider::ir::Op::Add, 6, {value(3), value(4)}));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(!hasHome(alloc, 0, 0) && !hasHome(alloc, 0, 1),
          "division operands stay in their slots");
    check(!hasHome(alloc, 0, 2), "a division result stays in its slot");
    check(!hasHome(alloc, 0, 3), "a variable-shift result stays in its slot");
    check(!hasHome(alloc, 0, 4), "an eight-bit multiply stays in its slot");
    check(!hasHome(alloc, 0, 5), "an Alloca address is never a register value");
}

void testCopies() {
    std::printf("copy chains\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    cider::ir::Instr copy;
    copy.op = cider::ir::Op::Copy;
    copy.width = cider::ir::IRType::I64;
    copy.isPure = true;
    copy.operands.push_back(value(0));
    copy.results.push_back(value(1));
    f.add(0, std::move(copy));
    f.add(0, binary(cider::ir::Op::Add, 2, {value(1), value(1)}));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(2));
    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(hasHome(alloc, 0, 0) && hasHome(alloc, 0, 1) &&
              hasHome(alloc, 0, 2),
          "a lowered phi-style Copy chain can be homed end to end");
}

void testCrossBlock() {
    std::printf("cross-block values\n");
    Fn f;
    cider::ir::Block second;
    second.label = 1;
    f.fn.blocks.push_back(std::move(second));
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, define(cider::ir::Op::Const, 1));
    f.add(0, binary(cider::ir::Op::Add, 2, {value(1), value(1)}));
    f.add(0, binary(cider::ir::Op::Add, 4, {value(2), value(2)}));
    f.term(0, 1);
    f.add(1, binary(cider::ir::Op::Add, 3, {value(0), value(0)}));
    f.fn.blocks[1].term.term = cider::ir::Term::Ret;
    f.fn.blocks[1].term.values.push_back(value(3));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(hasHome(alloc, 0, 0) && hasHome(alloc, 1, 0),
          "a value used in another block is homed on both sides");
    check(homeOf(alloc, 0, 0) == homeOf(alloc, 1, 0),
          "one register carries the cross-block value");
    check(homeOf(alloc, 0, 0) == cider::codegen::Register::R8,
          "the only global interval takes r8");
    check(hasHome(alloc, 0, 1), "a value contained in one block is homed");
    check(hasHome(alloc, 0, 2), "the block-local result is homed too");
    check(alloc.globalValues == 1 && alloc.globalSpilled == 0,
          "one global interval, no global spill");
}

void testGlobalBranchMerge() {
    std::printf("global branch merge\n");
    Fn f;
    cider::ir::Block one;
    one.label = 1;
    cider::ir::Block two;
    two.label = 2;
    cider::ir::Block three;
    three.label = 3;
    f.fn.blocks.push_back(std::move(one));
    f.fn.blocks.push_back(std::move(two));
    f.fn.blocks.push_back(std::move(three));
    f.add(0, define(cider::ir::Op::Const, 0));
    f.term(0, 1);
    f.term(1, 3);
    f.term(2, 3);
    f.add(3, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
    f.fn.blocks[3].term.term = cider::ir::Term::Ret;
    f.fn.blocks[3].term.values.push_back(value(1));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(hasHome(alloc, 0, 0) && hasHome(alloc, 1, 0) &&
              hasHome(alloc, 2, 0) && hasHome(alloc, 3, 0),
          "a merge keeps the value in one home through both arms");
    check(homeOf(alloc, 0, 0) == homeOf(alloc, 3, 0),
          "the branch merge reads the same register the entry wrote");
    check(homeOf(alloc, 0, 0) == cider::codegen::Register::R8,
          "an unbarriered global interval uses r8");
    check(alloc.calleeSaved.empty(),
          "a merge with no barrier needs no callee-saved register");
    check(homeOf(alloc, 3, 1) != homeOf(alloc, 3, 0),
          "the merge's local result does not reuse the global register");
}

void testGlobalLoopCarried() {
    std::printf("global loop-carried value\n");
    Fn f;
    cider::ir::Block header;
    header.label = 1;
    cider::ir::Block body;
    body.label = 2;
    cider::ir::Block exit;
    exit.label = 3;
    f.fn.blocks.push_back(std::move(header));
    f.fn.blocks.push_back(std::move(body));
    f.fn.blocks.push_back(std::move(exit));
    f.add(0, define(cider::ir::Op::Const, 0));
    f.term(0, 1);
    f.add(1, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
    f.fn.blocks[1].term.term = cider::ir::Term::CondBr;
    f.fn.blocks[1].term.cond = value(1);
    f.fn.blocks[1].term.thenTarget = 2;
    f.fn.blocks[1].term.elseTarget = 3;
    f.add(2, binary(cider::ir::Op::Add, 2, {value(0), value(0)}));
    f.term(2, 1);
    f.add(3, binary(cider::ir::Op::Add, 3, {value(0), value(0)}));
    f.fn.blocks[3].term.term = cider::ir::Term::Ret;
    f.fn.blocks[3].term.values.push_back(value(3));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(hasHome(alloc, 0, 0) && hasHome(alloc, 1, 0) &&
              hasHome(alloc, 2, 0) && hasHome(alloc, 3, 0),
          "a preheader value stays homed around the back edge");
    check(homeOf(alloc, 0, 0) == homeOf(alloc, 2, 0) &&
              homeOf(alloc, 0, 0) == homeOf(alloc, 3, 0),
          "the loop and its exit read the same register");
    check(homeOf(alloc, 0, 0) == cider::codegen::Register::R8,
          "a loop with no barrier keeps the caller-saved home");
}

void testGlobalAcrossCall() {
    std::printf("global values across a call\n");
    Fn f;
    cider::ir::Block callBlock;
    callBlock.label = 1;
    cider::ir::Block after;
    after.label = 2;
    f.fn.blocks.push_back(std::move(callBlock));
    f.fn.blocks.push_back(std::move(after));
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, define(cider::ir::Op::Const, 1));
    f.term(0, 1);
    cider::ir::Instr call;
    call.op = cider::ir::Op::Call;
    call.callee = "g";
    f.add(1, std::move(call));
    f.term(1, 2);
    f.add(2, binary(cider::ir::Op::Add, 2, {value(0), value(1)}));
    f.fn.blocks[2].term.term = cider::ir::Term::Ret;
    f.fn.blocks[2].term.values.push_back(value(2));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(homeOf(alloc, 0, 0) == cider::codegen::Register::Rbx &&
              homeOf(alloc, 2, 0) == cider::codegen::Register::Rbx,
          "the first cross-call value takes the callee-saved rbx");
    check(!hasHome(alloc, 0, 1) && !hasHome(alloc, 2, 1),
          "the second cross-call value spills instead of sharing rbx");
    check(hasCalleeSaved(alloc, cider::codegen::Register::Rbx),
          "the allocation asks the emitter to save rbx");
    check(alloc.globalValues == 1 && alloc.globalSpilled == 1,
          "one callee-saved home and one global spill");
}

void testGlobalAsmAndVolatileBarriers() {
    std::printf("global asm and volatile barriers\n");
    {
        Fn f;
        cider::ir::Block marker;
        marker.label = 1;
        cider::ir::Block after;
        after.label = 2;
        f.fn.blocks.push_back(std::move(marker));
        f.fn.blocks.push_back(std::move(after));
        f.add(0, define(cider::ir::Op::Const, 0));
        f.add(0, define(cider::ir::Op::Const, 1));
        f.term(0, 1);
        cider::ir::Instr asmInstr;
        asmInstr.op = cider::ir::Op::Asm;
        asmInstr.asmText = "nop";
        f.add(1, std::move(asmInstr));
        f.term(1, 2);
        f.add(2, binary(cider::ir::Op::Add, 2, {value(0), value(1)}));
        f.fn.blocks[2].term.term = cider::ir::Term::Ret;
        f.fn.blocks[2].term.values.push_back(value(2));

        const auto alloc = cider::codegen::allocateFunction(f.fn);
        check(homeOf(alloc, 0, 0) == cider::codegen::Register::Rbx,
              "asm leaves at most the callee-saved rbx for a crosser");
        check(!hasHome(alloc, 0, 1),
              "asm pressure spills the second crosser");
    }
    {
        Fn f;
        cider::ir::Block marker;
        marker.label = 1;
        cider::ir::Block after;
        after.label = 2;
        f.fn.blocks.push_back(std::move(marker));
        f.fn.blocks.push_back(std::move(after));
        f.add(0, define(cider::ir::Op::Const, 0));
        f.term(0, 1);
        cider::ir::Instr load;
        load.op = cider::ir::Op::Load;
        load.width = cider::ir::IRType::I64;
        load.isVolatile = true;
        load.addressBase = value(9, cider::ir::IRType::Ptr);
        load.results.push_back(value(8));
        f.add(1, std::move(load));
        f.term(1, 2);
        f.add(2, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
        f.fn.blocks[2].term.term = cider::ir::Term::Ret;
        f.fn.blocks[2].term.values.push_back(value(1));

        const auto alloc = cider::codegen::allocateFunction(f.fn);
        check(homeOf(alloc, 0, 0) == cider::codegen::Register::Rbx,
              "a volatile access also forces the callee-saved home");
        check(!hasHome(alloc, 1, 8),
              "the volatile load result keeps its slot");
    }
}

void testGlobalMultipleDefinitions() {
    std::printf("global multiple definitions\n");
    Fn f;
    cider::ir::Block one;
    one.label = 1;
    cider::ir::Block two;
    two.label = 2;
    cider::ir::Block merge;
    merge.label = 3;
    f.fn.blocks.push_back(std::move(one));
    f.fn.blocks.push_back(std::move(two));
    f.fn.blocks.push_back(std::move(merge));
    f.add(0, define(cider::ir::Op::Const, 0));
    f.fn.blocks[0].term.term = cider::ir::Term::CondBr;
    f.fn.blocks[0].term.cond = value(0);
    f.fn.blocks[0].term.thenTarget = 1;
    f.fn.blocks[0].term.elseTarget = 2;

    auto copy = [](uint32_t dest, uint32_t source) {
        cider::ir::Instr out;
        out.op = cider::ir::Op::Copy;
        out.width = cider::ir::IRType::I64;
        out.isPure = true;
        out.operands.push_back(value(source));
        out.results.push_back(value(dest));
        return out;
    };
    f.add(1, copy(5, 0));
    f.term(1, 3);
    f.add(2, define(cider::ir::Op::Const, 1));
    f.add(2, copy(5, 1));
    f.term(2, 3);
    f.add(3, binary(cider::ir::Op::Add, 2, {value(5), value(5)}));
    f.fn.blocks[3].term.term = cider::ir::Term::Ret;
    f.fn.blocks[3].term.values.push_back(value(2));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(!hasHome(alloc, 0, 5) && !hasHome(alloc, 1, 5) &&
              !hasHome(alloc, 2, 5) && !hasHome(alloc, 3, 5),
          "a phi-lowered result written on two edges is never globally homed");
    check(alloc.globalSpilled == 0,
          "a multi-def id is not counted as a global spill");
}

void testGlobalRegisterReuse() {
    std::printf("global register reuse\n");
    Fn f;
    cider::ir::Block one;
    one.label = 1;
    cider::ir::Block two;
    two.label = 2;
    cider::ir::Block three;
    three.label = 3;
    f.fn.blocks.push_back(std::move(one));
    f.fn.blocks.push_back(std::move(two));
    f.fn.blocks.push_back(std::move(three));
    f.add(0, define(cider::ir::Op::Const, 0));
    f.term(0, 1);
    f.add(1, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
    f.term(1, 2);
    f.add(2, define(cider::ir::Op::Const, 2));
    f.term(2, 3);
    f.add(3, binary(cider::ir::Op::Add, 3, {value(2), value(2)}));
    f.fn.blocks[3].term.term = cider::ir::Term::Ret;
    f.fn.blocks[3].term.values.push_back(value(3));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(homeOf(alloc, 0, 0) == cider::codegen::Register::R8 &&
              homeOf(alloc, 1, 0) == cider::codegen::Register::R8,
          "the first global interval takes r8");
    check(homeOf(alloc, 2, 2) == cider::codegen::Register::R8 &&
              homeOf(alloc, 3, 2) == cider::codegen::Register::R8,
          "a later, non-overlapping global interval reuses r8");
    check(alloc.globalValues == 2 && alloc.globalSpilled == 0,
          "both global intervals are homed without pressure");
}

void testWideLiveSetFallsBack() {
    std::printf("wide live set\n");
    // An array copy can keep thousands of values live at one point. The
    // allocator must not build that clique; it keeps every cross-block value
    // in its slot instead, and still homes a local value normally.
    constexpr uint32_t kWide = 80;
    Fn f;
    cider::ir::Block second;
    second.label = 1;
    f.fn.blocks.push_back(std::move(second));
    for (uint32_t id = 0; id < kWide; ++id) {
        f.add(0, define(cider::ir::Op::Const, id));
    }
    f.term(0, 1);
    cider::ir::Instr join;
    join.op = cider::ir::Op::Add;
    join.width = cider::ir::IRType::I64;
    for (uint32_t id = 0; id < kWide; ++id) {
        join.operands.push_back(value(id));
    }
    join.results.push_back(value(kWide));
    f.add(1, std::move(join));
    f.fn.blocks[1].term.term = cider::ir::Term::Ret;
    f.fn.blocks[1].term.values.push_back(value(kWide));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    bool allSpilled = true;
    for (uint32_t id = 0; id < kWide; ++id) {
        if (hasHome(alloc, 0, id) || hasHome(alloc, 1, id)) {
            allSpilled = false;
        }
    }
    check(allSpilled, "a live set over the clique limit keeps every slot");
    check(alloc.globalSpilled == kWide,
          "every wide live-set value is counted as a global spill");
    check(hasHome(alloc, 1, kWide),
          "a value local to the wide block still gets a register");
}

void testUnsuitableValues() {
    std::printf("unsuitable values\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0, cider::ir::IRType::F64));
    f.add(0, define(cider::ir::Op::Const, 1));
    cider::ir::Instr call;
    call.op = cider::ir::Op::Call;
    call.callee = "g";
    cider::ir::Argument arg;
    arg.words.push_back(value(1));
    call.arguments.push_back(arg);
    call.results.push_back(value(2));
    f.add(0, std::move(call));
    f.add(0, binary(cider::ir::Op::Add, 3, {value(2), value(2)}));
    f.add(0, binary(cider::ir::Op::Add, 4, {value(3), value(3)}));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(!hasHome(alloc, 0, 0), "a float value is never homed");
    check(!hasHome(alloc, 0, 1), "a call argument keeps its slot");
    check(hasHome(alloc, 0, 2), "a call result after the barrier may be homed");
    check(hasHome(alloc, 0, 3), "the result's local chain stays in registers");
}

void testMultipleDefinitions() {
    std::printf("multiple definitions\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(1));
    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(!hasHome(alloc, 0, 0),
          "two definitions of one id in a block keep the slot fallback");
    check(hasHome(alloc, 0, 1), "the consuming value is still homed");
}

void testPhiRefused() {
    std::printf("phi\n");
    Fn f;
    cider::ir::Instr phi;
    phi.op = cider::ir::Op::Phi;
    phi.width = cider::ir::IRType::I64;
    phi.results.push_back(value(0));
    phi.phiIncoming.push_back(cider::ir::PhiIncoming{value(1), 0});
    f.add(0, std::move(phi));
    f.add(0, define(cider::ir::Op::Const, 1));
    f.add(0, binary(cider::ir::Op::Add, 2, {value(0), value(1)}));
    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(!hasHome(alloc, 0, 0), "a phi result is left for lowering, never homed");
    check(!hasHome(alloc, 0, 1), "a phi incoming value is excluded conservatively");
}

void testNames() {
    std::printf("names\n");
    check(std::string(cider::codegen::registerName(cider::codegen::Register::R8)) == "r8",
          "r8 names itself");
    check(std::string(cider::codegen::registerName(cider::codegen::Register::Rsi)) == "rsi",
          "rsi names itself");
    check(std::string(cider::codegen::registerName(cider::codegen::Register::Rbx)) == "rbx",
          "rbx names itself");
}

}  // namespace

int main() {
    testReuse();
    testPressure();
    testMsAbiPool();
    testBarriers();
    testFixedScratch();
    testCopies();
    testCrossBlock();
    testGlobalBranchMerge();
    testGlobalLoopCarried();
    testGlobalAcrossCall();
    testGlobalAsmAndVolatileBarriers();
    testGlobalMultipleDefinitions();
    testGlobalRegisterReuse();
    testWideLiveSetFallsBack();
    testUnsuitableValues();
    testMultipleDefinitions();
    testPhiRefused();
    testNames();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("%d checks, all passed\n", checks);
    return 0;
}
