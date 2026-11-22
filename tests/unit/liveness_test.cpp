// liveness_test.cpp — the whole-function liveness analysis, without an
// emitter.
//
// These tests build the lowered, non-phi IR codegen receives and pin the
// contracts Task 9's linear-scan allocator relies on: may-liveness reaches
// across branch merges, loops and back edges; live ranges run from the first
// definition to the last use in deterministic RPO positions; and a call (or
// another barrier) only marks a value as crossing when it sits strictly
// between that definition and a later use.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "codegen/liveness.h"
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

    uint32_t block(uint32_t label) {
        cider::ir::Block block;
        block.label = label;
        fn.blocks.push_back(std::move(block));
        return static_cast<uint32_t>(fn.blocks.size()) - 1;
    }

    void add(uint32_t blockIndex, cider::ir::Instr instr) {
        fn.blocks[blockIndex].body.push_back(std::move(instr));
    }

    void term(uint32_t blockIndex, uint32_t target) {
        fn.blocks[blockIndex].term.term = cider::ir::Term::Br;
        fn.blocks[blockIndex].term.target = target;
    }
};

bool has(const std::vector<uint32_t>& ids, uint32_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

const cider::codegen::LiveRange* findRange(
    const cider::codegen::Liveness& live, uint32_t id) {
    for (const cider::codegen::LiveRange& range : live.ranges) {
        if (range.value == id) {
            return &range;
        }
    }
    return nullptr;
}

// The entry block is always block index 0. The helper exists so the diamond
// test reads as "entry", not as a bare zero.
uint32_t entry(const cider::codegen::Liveness& live) {
    return live.blockOrder.empty() ? 0 : live.blockOrder.front();
}

// The linear position of a block's first instruction. Positions are
// blockPosition * 1'000'000 + instructionIndex.
uint32_t blockStart(const cider::codegen::Liveness& live, uint32_t block) {
    for (uint32_t position = 0; position < live.blockOrder.size(); ++position) {
        if (live.blockOrder[position] == block) {
            return position * 1000000u;
        }
    }
    return 0xFFFFFFFFu;
}

void testDiamond() {
    std::printf("diamond\n");
    Fn f;
    const uint32_t left = f.block(10);
    const uint32_t right = f.block(20);
    const uint32_t join = f.block(30);

    const uint32_t c = 0;
    const uint32_t a = 1;
    const uint32_t b = 2;
    const uint32_t sum = 3;
    f.add(0, define(cider::ir::Op::Const, c));
    f.fn.blocks[0].term.term = cider::ir::Term::CondBr;
    f.fn.blocks[0].term.cond = value(c);
    f.fn.blocks[0].term.thenTarget = 10;
    f.fn.blocks[0].term.elseTarget = 20;
    // Lowered phi form writes each merged id on every incoming edge. Both ids
    // are therefore defined on both arms, which is also what keeps the entry
    // (which has no predecessor) from acquiring a live-in.
    f.add(left, define(cider::ir::Op::Const, a));
    f.add(left, define(cider::ir::Op::Const, b));
    f.term(left, 30);
    f.add(right, define(cider::ir::Op::Const, a));
    f.add(right, define(cider::ir::Op::Const, b));
    f.term(right, 30);
    f.add(join, binary(cider::ir::Op::Add, sum, {value(a), value(b)}));
    f.fn.blocks[join].term.term = cider::ir::Term::Ret;
    f.fn.blocks[join].term.values.push_back(value(sum));

    const cider::codegen::Liveness live = cider::codegen::analyzeLiveness(f.fn);

    check(has(live.liveOut[left], a), "left defines a live at join");
    check(has(live.liveIn[join], a), "join sees a");
    check(has(live.liveOut[right], b), "right defines b live at join");
    check(live.liveIn[entry(live)].empty(), "entry has no live-in");

    const auto* aRange = findRange(live, a);
    check(aRange != nullptr &&
              aRange->start ==
                  std::min(blockStart(live, left), blockStart(live, right)) &&
              aRange->end == blockStart(live, join) && aRange->uses == 1,
          "a's range runs from its first definition to the join's use");
    check(std::is_sorted(live.liveIn[join].begin(), live.liveIn[join].end()) &&
              std::is_sorted(live.liveOut[left].begin(),
                             live.liveOut[left].end()),
          "live sets are sorted by value id");
}

void testLoop() {
    std::printf("loop and back edge\n");
    Fn f;
    const uint32_t header = f.block(1);
    const uint32_t body = f.block(2);
    const uint32_t exit = f.block(3);

    const uint32_t pre = 0;    // defined before the loop
    const uint32_t cond = 4;   // the loop's condition
    const uint32_t h = 1;      // defined in the header, used in exit
    const uint32_t b = 2;      // the body's own value
    const uint32_t out = 3;

    f.add(0, define(cider::ir::Op::Const, pre));
    f.add(0, define(cider::ir::Op::Const, cond));
    f.term(0, 1);
    f.add(header, binary(cider::ir::Op::Add, h, {value(pre), value(pre)}));
    f.fn.blocks[header].term.term = cider::ir::Term::CondBr;
    f.fn.blocks[header].term.cond = value(cond);
    f.fn.blocks[header].term.thenTarget = 2;
    f.fn.blocks[header].term.elseTarget = 3;
    f.add(body, binary(cider::ir::Op::Add, b, {value(pre), value(pre)}));
    f.term(body, 1);
    f.add(exit, binary(cider::ir::Op::Add, out, {value(pre), value(h)}));
    f.fn.blocks[exit].term.term = cider::ir::Term::Ret;
    f.fn.blocks[exit].term.values.push_back(value(out));

    const cider::codegen::Liveness live = cider::codegen::analyzeLiveness(f.fn);

    const auto* hRange = findRange(live, h);
    check(hRange != nullptr && hRange->start == blockStart(live, header) &&
              hRange->end == blockStart(live, exit),
          "a header value's range ends at its use after the loop");
    check(has(live.liveOut[body], pre),
          "a preheader value is live out of the body across the back edge");
    const auto* preRange = findRange(live, pre);
    check(preRange != nullptr && preRange->start == blockStart(live, 0) &&
              preRange->end == blockStart(live, body),
          "the preheader value's range reaches the loop body");
}

void testCallBarrier() {
    std::printf("call barrier\n");
    Fn f;
    const uint32_t before = 0;
    const uint32_t callResult = 1;
    const uint32_t after = 2;
    const uint32_t chain = 3;
    const uint32_t onlyArgument = 4;

    f.add(0, define(cider::ir::Op::Const, before));
    f.add(0, define(cider::ir::Op::Const, onlyArgument));
    cider::ir::Instr call;
    call.op = cider::ir::Op::Call;
    call.callee = "g";
    cider::ir::Argument arg;
    arg.words.push_back(value(onlyArgument));
    call.arguments.push_back(arg);
    call.results.push_back(value(callResult));
    f.add(0, std::move(call));
    f.add(0, binary(cider::ir::Op::Add, after,
                     {value(before), value(callResult)}));
    f.add(0, binary(cider::ir::Op::Add, chain, {value(after), value(after)}));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(chain));

    const cider::codegen::Liveness live = cider::codegen::analyzeLiveness(f.fn);

    const auto* beforeRange = findRange(live, before);
    check(beforeRange != nullptr && beforeRange->crossesCall,
          "a value defined before and used after a call crosses it");
    const auto* resultRange = findRange(live, callResult);
    check(resultRange != nullptr &&
              resultRange->start == blockStart(live, 0) + 2 &&
              !resultRange->crossesCall,
          "a call result starts after its call and does not cross it");
    const auto* chainRange = findRange(live, chain);
    check(chainRange != nullptr && !chainRange->crossesCall,
          "a value born after the call does not cross it");
    const auto* argumentRange = findRange(live, onlyArgument);
    check(argumentRange != nullptr && !argumentRange->crossesCall,
          "a value used only by the call itself does not cross it");
}

void testFloatFlag() {
    std::printf("float intervals\n");
    Fn f;
    f.add(0, define(cider::ir::Op::Const, 0, cider::ir::IRType::F64));
    f.add(0, binary(cider::ir::Op::FAdd, 1, {value(0), value(0)},
                     cider::ir::IRType::F64));
    f.fn.blocks[0].term.term = cider::ir::Term::Ret;
    f.fn.blocks[0].term.values.push_back(value(1));

    const cider::codegen::Liveness live = cider::codegen::analyzeLiveness(f.fn);
    const auto* floatRange = findRange(live, 0);
    const auto* sumRange = findRange(live, 1);
    check(floatRange != nullptr && floatRange->isFloat,
          "an F64 value's range is marked float");
    check(sumRange != nullptr && sumRange->isFloat,
          "the float result's range is marked float");
}

void testDeterministicOrder() {
    std::printf("determinism\n");
    Fn f;
    const uint32_t second = f.block(1);
    f.add(0, define(cider::ir::Op::Const, 3));
    f.add(0, define(cider::ir::Op::Const, 1));
    f.add(0, define(cider::ir::Op::Const, 2));
    f.term(0, 1);
    f.add(second, binary(cider::ir::Op::Add, 4,
                         {value(1), value(2)}));
    f.add(second, binary(cider::ir::Op::Add, 5,
                         {value(3), value(4)}));
    f.fn.blocks[second].term.term = cider::ir::Term::Ret;
    f.fn.blocks[second].term.values.push_back(value(5));

    const cider::codegen::Liveness first = cider::codegen::analyzeLiveness(f.fn);
    const cider::codegen::Liveness again = cider::codegen::analyzeLiveness(f.fn);

    bool sorted = true;
    for (uint32_t previous = 1; previous < first.ranges.size(); ++previous) {
        if (first.ranges[previous - 1].value >= first.ranges[previous].value) {
            sorted = false;
        }
    }
    check(sorted, "ranges are unique and sorted by value id");
    check(first.liveIn == again.liveIn && first.liveOut == again.liveOut &&
              first.blockOrder == again.blockOrder,
          "two analyses of one function agree on every live set");
    check(first.ranges.size() == again.ranges.size(),
          "two analyses of one function agree on the range count");
    bool sameRanges = first.ranges.size() == again.ranges.size();
    for (size_t i = 0; sameRanges && i < first.ranges.size(); ++i) {
        const cider::codegen::LiveRange& a = first.ranges[i];
        const cider::codegen::LiveRange& b = again.ranges[i];
        sameRanges = a.value == b.value && a.start == b.start &&
                     a.end == b.end && a.crossesCall == b.crossesCall &&
                     a.isFloat == b.isFloat && a.uses == b.uses;
    }
    check(sameRanges, "two analyses of one function agree on every range");
}

}  // namespace

int main() {
    testDiamond();
    testLoop();
    testCallBarrier();
    testFloatFlag();
    testDeterministicOrder();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("%d checks, all passed\n", checks);
    return 0;
}
