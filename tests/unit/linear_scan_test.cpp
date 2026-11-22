// linear_scan_test.cpp — the whole-function allocator's contract.
//
// The pressure case has more independent live values than the register file,
// with a call between every definition and every use. It pins the three
// properties that make the allocator safe to run over the whole function:
//
//   - overlapping values never share a register;
//   - a caller-saved home never crosses a call;
//   - pressure uses the callee-saved row;
//   - two allocations of the same function are byte-for-byte identical.
//
// The allocator is deterministic in block and value-id order, so the tests
// compare two runs rather than pinning one particular register assignment.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "codegen/liveness.h"
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

cider::ir::Instr call(uint32_t result = 0xFFFFFFFFu) {
    cider::ir::Instr out;
    out.op = cider::ir::Op::Call;
    out.callee = "g";
    if (result != 0xFFFFFFFFu) {
        out.results.push_back(value(result));
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
};

cider::codegen::Register homeOf(const cider::codegen::FunctionAllocation& alloc,
                                uint32_t block, uint32_t id) {
    const auto& home = alloc.blocks[block].home;
    if (id >= home.size()) {
        return cider::codegen::Register::None;
    }
    return static_cast<cider::codegen::Register>(home[id]);
}

bool sameAllocation(const cider::codegen::FunctionAllocation& lhs,
                    const cider::codegen::FunctionAllocation& rhs) {
    if (lhs.blocks.size() != rhs.blocks.size() ||
        lhs.calleeSaved != rhs.calleeSaved ||
        lhs.spillSlot != rhs.spillSlot) {
        return false;
    }
    for (size_t block = 0; block < lhs.blocks.size(); ++block) {
        if (lhs.blocks[block].home != rhs.blocks[block].home) {
            return false;
        }
    }
    return true;
}

bool isCallerSavedPooled(cider::codegen::Register reg) {
    using cider::codegen::Register;
    switch (reg) {
        case Register::Rax:
        case Register::Rcx:
        case Register::Rdx:
        case Register::Rsi:
        case Register::Rdi:
        case Register::R8:
        case Register::R9:
        case Register::R10:
        case Register::R11:
            return true;
        case Register::None:
        case Register::Rbx:
        case Register::R12:
        case Register::R13:
        case Register::R14:
        case Register::R15:
            return false;
    }
    return false;
}

bool isCalleeSavedPooled(cider::codegen::Register reg) {
    using cider::codegen::Register;
    return reg == Register::Rbx || reg == Register::R12 ||
           reg == Register::R13 || reg == Register::R14 ||
           reg == Register::R15;
}

bool callerSavedLiveAcrossCall(const cider::codegen::FunctionAllocation& alloc,
                               uint32_t block,
                               const std::vector<uint32_t>& crossing) {
    for (uint32_t id : crossing) {
        const cider::codegen::Register reg = homeOf(alloc, block, id);
        if (reg != cider::codegen::Register::None && isCallerSavedPooled(reg)) {
            return true;
        }
    }
    return false;
}

bool usesCalleeSaved(const cider::codegen::FunctionAllocation& alloc) {
    if (alloc.calleeSaved.empty()) {
        return false;
    }
    for (uint32_t id = 0; id < alloc.blocks[0].home.size(); ++id) {
        const cider::codegen::Register reg = homeOf(alloc, 0, id);
        if (isCalleeSavedPooled(reg)) {
            return true;
        }
    }
    return false;
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

bool overlap(const cider::codegen::LiveRange& lhs,
             const cider::codegen::LiveRange& rhs) {
    // Two values are simultaneously live only when each starts strictly
    // before the other ends. A use at p and a definition at p are ordered by
    // the instruction, not simultaneous.
    return lhs.start < rhs.end && rhs.start < lhs.end;
}

// 24 independent values, every one alive across a call. The register file is
// smaller, so some values spill and the crossing ones must use callee-saved
// homes.
void testPressureAcrossCall() {
    std::printf("pressure across a call\n");
    constexpr uint32_t kLive = 24;
    Fn f;
    for (uint32_t id = 0; id < kLive; ++id) {
        f.add(0, define(cider::ir::Op::Const, id));
    }
    f.add(0, call());
    cider::ir::Instr join;
    join.op = cider::ir::Op::Add;
    join.width = cider::ir::IRType::I64;
    for (uint32_t id = 0; id < kLive; ++id) {
        join.operands.push_back(value(id));
    }
    join.results.push_back(value(kLive));
    f.add(0, std::move(join));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(kLive));

    const auto live = cider::codegen::analyzeLiveness(f.fn);
    std::vector<cider::codegen::LiveRange> ranges;
    for (const auto& range : live.ranges) {
        if (range.value < kLive) {
            ranges.push_back(range);
        }
    }
    check(ranges.size() == kLive, "liveness sees all 24 values");

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    bool distinct = true;
    for (uint32_t a = 0; a < ranges.size(); ++a) {
        for (uint32_t b = a + 1; b < ranges.size(); ++b) {
            if (!overlap(ranges[a], ranges[b])) {
                continue;
            }
            const auto homeA = homeOf(alloc, 0, ranges[a].value);
            const auto homeB = homeOf(alloc, 0, ranges[b].value);
            if (homeA != cider::codegen::Register::None && homeA == homeB) {
                distinct = false;
            }
        }
    }
    check(distinct, "overlapping values never share a register");

    std::vector<uint32_t> crossing;
    for (const auto& range : ranges) {
        if (range.crossesCall) {
            crossing.push_back(range.value);
        }
    }
    check(crossing.size() == kLive, "every value crosses the call");
    check(!callerSavedLiveAcrossCall(alloc, 0, crossing),
          "no caller-saved home crosses a call");
    check(usesCalleeSaved(alloc), "pressure forces a callee-saved home");
    check(hasCalleeSaved(alloc, cider::codegen::Register::Rbx) &&
              hasCalleeSaved(alloc, cider::codegen::Register::R12) &&
              hasCalleeSaved(alloc, cider::codegen::Register::R13),
          "rbx and the new r12/r13 carry the pressure");
    check(std::is_sorted(alloc.calleeSaved.begin(), alloc.calleeSaved.end()),
          "the callee-saved list is sorted by enum value");
    check(sameAllocation(cider::codegen::allocateFunction(f.fn),
                         cider::codegen::allocateFunction(f.fn)),
          "allocation is deterministic");
    check(alloc.spillSlot.size() >= kLive + 1,
          "the allocation names a slot for every value");
}

// A Copy's destination shares its source's register when the two do not
// interfere; the emitter then emits no move at all.
void testCopyCoalescing() {
    std::printf("copy coalescing\n");
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
    check(homeOf(alloc, 0, 0) != cider::codegen::Register::None &&
              homeOf(alloc, 0, 0) == homeOf(alloc, 0, 1),
          "a non-interfering Copy is coalesced");
}

// The emitter stages the words past the argument registers in rax/r10/r11 and
// then r12-r15 without restoring them. A value live across such a call cannot
// be homed in a staged register.
void testCallStagingClobbers() {
    std::printf("call staging clobbers\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, define(cider::ir::Op::Const, 1));
    cider::ir::Instr staged = call();
    for (uint32_t word = 0; word < 10; ++word) {
        cider::ir::Argument arg;
        arg.words.push_back(value(100 + word));
        staged.arguments.push_back(std::move(arg));
    }
    f.add(0, std::move(staged));
    f.add(0, binary(cider::ir::Op::Add, 2, {value(0), value(1)}));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(2));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(!hasCalleeSaved(alloc, cider::codegen::Register::R12),
          "a value crossing a stack-spilling call avoids the staged r12");
    check(hasCalleeSaved(alloc, cider::codegen::Register::Rbx),
          "the unstaged callee-saved rbx still carries a crossing value");
    check(hasCalleeSaved(alloc, cider::codegen::Register::R13),
          "an unstaged callee-saved register carries the second crosser");
}

// A value live through a block without a definition or use there still needs
// its home in that block: the emitter reads the home from the block map.
void testLiveThroughBlockHome() {
    std::printf("live-through block home\n");
    Fn f;
    cider::ir::Block one;
    one.label = 1;
    cider::ir::Block two;
    two.label = 2;
    f.fn.blocks.push_back(std::move(one));
    f.fn.blocks.push_back(std::move(two));
    f.add(0, define(cider::ir::Op::Const, 0));
    f.fn.blocks[0].term.term = cider::ir::Term::Br;
    f.fn.blocks[0].term.target = 1;
    f.fn.blocks[1].term.term = cider::ir::Term::Br;
    f.fn.blocks[1].term.target = 2;
    f.add(2, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
    f.fn.blocks[2].term.term = cider::ir::Term::Ret;
    f.fn.blocks[2].term.values.push_back(value(1));

    const auto alloc = cider::codegen::allocateFunction(f.fn);
    check(homeOf(alloc, 0, 0) != cider::codegen::Register::None,
          "the defining block homes the value");
    check(homeOf(alloc, 0, 0) == homeOf(alloc, 1, 0),
          "the live-through block names the same home");
    check(homeOf(alloc, 0, 0) == homeOf(alloc, 2, 0),
          "the using block names the same home");
}

// A call result used only as a later call's argument must be able to keep a
// caller-saved home: the argument is read before the later call clobbers it.
void testCallArgumentChainKeepsCallerHome() {
    std::printf("call argument chain home\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    cider::ir::Instr first = call(1);
    cider::ir::Argument a0;
    a0.words.push_back(value(0));
    first.arguments.push_back(std::move(a0));
    f.add(0, std::move(first));
    cider::ir::Instr second = call(2);
    cider::ir::Argument a1;
    a1.words.push_back(value(1));
    second.arguments.push_back(std::move(a1));
    f.add(0, std::move(second));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(2));

    const auto alloc = cider::codegen::allocateFunction(f.fn, true, true);
    const auto reg = homeOf(alloc, 0, 1);
    check(reg != cider::codegen::Register::None,
          "a call result used only as the next call's argument keeps a home");
    check(isCallerSavedPooled(reg),
          "that home is caller-saved, not a scarce callee register");
}

// The relaxation must not apply when the chosen register is an earlier
// argument's destination: staging could write that home before the value's
// own word is read. The emitter's dependency-aware staging also makes a
// positional "staged before" model unsound, so all other argument
// destinations are excluded from the relaxation.
void testArgumentStagingDoesNotClobberEarlierWord() {
    std::printf("argument staging order\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));  // feeds arg 0 (rdi)
    f.add(0, define(cider::ir::Op::Const, 1));  // feeds arg 1 (rsi)
    cider::ir::Instr first = call(2);
    cider::ir::Argument f0;
    f0.words.push_back(value(0));
    first.arguments.push_back(std::move(f0));
    f.add(0, std::move(first));
    cider::ir::Instr second = call(3);
    cider::ir::Argument s0;
    s0.words.push_back(value(0));
    second.arguments.push_back(std::move(s0));
    cider::ir::Argument s1;
    s1.words.push_back(value(2));
    second.arguments.push_back(std::move(s1));
    f.add(0, std::move(second));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(3));

    const auto alloc = cider::codegen::allocateFunction(f.fn, true, true);
    const auto reg = homeOf(alloc, 0, 2);
    check(reg != cider::codegen::Register::Rdi,
          "a value staged as argument 1 is not homed in argument 0's rdi");
}

// If the value feeds two argument words of the same call, an intermediate
// staging could overwrite its home before the last read; keep the slot.
void testRepeatedArgumentUseKeepsSlot() {
    std::printf("repeated argument use\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, call(1));                       // result feeds two words of call 2
    cider::ir::Instr second = call(2);
    for (int k = 0; k < 2; ++k) {
        cider::ir::Argument a;
        a.words.push_back(value(1));
        second.arguments.push_back(std::move(a));
    }
    f.add(0, std::move(second));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(2));

    const auto alloc = cider::codegen::allocateFunction(f.fn, true, true);
    const auto reg = homeOf(alloc, 0, 1);
    check(reg == cider::codegen::Register::None ||
          isCalleeSavedPooled(reg),
          "a value used by two argument words keeps the conservative rule");
}

// A value still live after the call must keep the conservative rule: the
// call's clobber really does destroy a caller-saved home.
void testLiveAfterCallStillSpills() {
    std::printf("live after call spills\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, call());                       // clobbers caller row, no result
    f.add(0, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(1));

    const auto alloc = cider::codegen::allocateFunction(f.fn, true, true);
    const auto reg = homeOf(alloc, 0, 0);
    check(reg == cider::codegen::Register::None ||
          isCalleeSavedPooled(reg),
          "a caller-saved home never crosses a call, even with the relaxation");
}

// A non-call clobber at the last-use position (Div writes rax/rdx) is not an
// argument read and must still forbid those homes.
void testNonCallClobberStillForbids() {
    std::printf("non-call clobber forbids\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    cider::ir::Instr div = binary(cider::ir::Op::Div, 1,
                                  {value(0), value(0)});
    f.add(0, std::move(div));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(1));

    const auto alloc = cider::codegen::allocateFunction(f.fn, true, true);
    const auto reg = homeOf(alloc, 0, 0);
    check(reg != cider::codegen::Register::Rax &&
          reg != cider::codegen::Register::Rdx,
          "a Div's fixed rax/rdx clobber still forbids those homes");
}

// A value whose only post-barrier use is a final store, and whose slot is
// prologue-written and unmodified, need not consume a scarce callee home.
void testRank2ReloadsPrologueWrittenSlot() {
    std::printf("rank2 prologue slot reload\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, call());                       // barrier with no result
    cider::ir::Instr store = define(cider::ir::Op::Store, 0xFFFFFFFFu);
    store.storedValue = value(0);
    store.addressBase = value(100);         // a known frame address
    f.add(0, std::move(store));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;

    // The relaxation is guarded by fast; the brief's allocateFunction(f.fn)
    // defaults to fast=false and could never exercise it.
    const auto alloc = cider::codegen::allocateFunction(f.fn, true, true);
    check(alloc.calleeSaved.empty(),
          "a final-store-only value does not spend a callee-saved register");
}

// A genuine crosser (used in arithmetic after the barrier) must keep its
// callee home; Rank 2 must not become a blanket slot demotion.
void testRank2KeepsRealCrossers() {
    std::printf("rank2 keeps real crossers\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0));
    f.add(0, call());
    f.add(0, binary(cider::ir::Op::Add, 1, {value(0), value(0)}));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(1));

    const auto alloc = cider::codegen::allocateFunction(f.fn, true, true);
    const auto reg = homeOf(alloc, 0, 0);
    check(reg != cider::codegen::Register::None,
          "a value used in arithmetic after a call still has a home");
    check(isCalleeSavedPooled(reg),
          "and that home is callee-saved because it crosses the barrier");
}

// The fast prologue writes a parameter's home from its incoming ABI register.
// If that home equals a LATER parameter's incoming register,
// settleFastParameters drops the home and the value is reloaded from its slot
// on every use. The allocator must never propose such a home: for a two-param
// SysV function (p arrives in rdi, n in rsi), p must not be homed in rsi.
void testFastParamAvoidsLaterIncomingRegister() {
    std::printf("fast parameter home avoids later incoming register\n");
    Fn f;
    f.fn.params.push_back(value(0));  // p: incoming rdi
    f.fn.params.push_back(value(1));  // n: incoming rsi
    f.fn.paramClasses.push_back(cider::ir::IRType::I64);
    f.fn.paramClasses.push_back(cider::ir::IRType::I64);
    // Clobber rax/rcx/rdx/r11 across p's live range (as fnv1a's hash loop
    // does) so the scan's only free caller register for p is rsi.
    f.add(0, binary(cider::ir::Op::Add, 2, {value(0), value(1)}));
    f.add(0, binary(cider::ir::Op::Cmp, 3, {value(2), value(0)}));
    f.add(0, binary(cider::ir::Op::Cast, 4, {value(3), value(0)}));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(0));

    const auto alloc = cider::codegen::allocateFunction(f.fn, true, true);
    const auto p = homeOf(alloc, 0, 0);
    check(p != cider::codegen::Register::None,
          "the first fast parameter keeps a register home");
    check(p != cider::codegen::Register::Rsi,
          "the first fast parameter avoids the later parameter's rsi");
}

}  // namespace

int main() {
    testFastParamAvoidsLaterIncomingRegister();
    testPressureAcrossCall();
    testCopyCoalescing();
    testCallStagingClobbers();
    testLiveThroughBlockHome();
    testCallArgumentChainKeepsCallerHome();
    testArgumentStagingDoesNotClobberEarlierWord();
    testRepeatedArgumentUseKeepsSlot();
    testLiveAfterCallStillSpills();
    testNonCallClobberStillForbids();
    testRank2ReloadsPrologueWrittenSlot();
    testRank2KeepsRealCrossers();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("%d checks, all passed\n", checks);
    return 0;
}
