// frame_test.cpp — the compact frame layout's contract.
//
// Task 10 replaces the per-id stack cell with a compact frame: only a value
// that has no register home in any block needs memory, and call words,
// alloca cells, callee-save cells and the SysV variadic area are laid out
// separately. This test builds the pressure shape the brief names — forty
// live values, one alloca and a call with eight outgoing words — and pins:
//
//   - the frame is a multiple of the call-alignment granule;
//   - the outgoing area covers the call words;
//   - no two spill slots overlap, and no spill overlaps an alloca cell;
//   - the frame extends past the highest slot;
//   - absent entries are the named sentinel, never offset zero;
//   - every value with no home has a slot and every homed value does not.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "codegen/frame.h"
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

bool present(uint32_t offset) {
    return offset != cider::codegen::kNoFrameSlot;
}

// Two present offsets overlap when they name different eight-byte cells.
bool overlap(uint32_t lhs, uint32_t rhs) {
    return present(lhs) && present(rhs) && lhs == rhs;
}

bool noOverlap(const std::vector<uint32_t>& offsets) {
    std::vector<uint32_t> seen;
    for (uint32_t offset : offsets) {
        if (!present(offset)) {
            continue;
        }
        if (std::find(seen.begin(), seen.end(), offset) != seen.end()) {
            return false;
        }
        seen.push_back(offset);
    }
    return true;
}

bool noOverlap(const std::vector<uint32_t>& lhs,
               const std::vector<uint32_t>& rhs) {
    for (uint32_t left : lhs) {
        if (!present(left)) {
            continue;
        }
        for (uint32_t right : rhs) {
            if (overlap(left, right)) {
                return false;
            }
        }
    }
    return true;
}

uint32_t highestOffset(const cider::codegen::FrameLayout& layout) {
    uint32_t highest = 0;
    bool any = false;
    for (const std::vector<uint32_t>* rows :
         {&layout.spillOffset, &layout.allocaOffset}) {
        for (uint32_t offset : *rows) {
            if (!present(offset)) {
                continue;
            }
            if (!any || offset > highest) {
                highest = offset;
                any = true;
            }
        }
    }
    return any ? highest : 0;
}

uint32_t presentCount(const std::vector<uint32_t>& offsets) {
    uint32_t count = 0;
    for (uint32_t offset : offsets) {
        if (present(offset)) {
            ++count;
        }
    }
    return count;
}

// Forty parameter values are never homed by the allocator: the prologue
// writes their slots. The alloca adds an address-taken cell and the call
// fills the six SysV integer registers and then asks for eight outgoing
// words, so the outgoing area must cover them.
cider::ir::Function pressureFunction() {
    constexpr uint32_t kLive = 40;
    constexpr uint32_t kCallWords = 14;
    cider::ir::Function fn;
    fn.name = "pressure";
    fn.signature = cider::Signature({}, nullptr, cider::Abi::SysV);
    for (uint32_t at = 0; at < kLive; ++at) {
        fn.params.push_back(value(at + 1));
        fn.paramClasses.push_back(cider::ir::IRType::I64);
    }
    cider::ir::Block block;
    block.label = 0;

    cider::ir::Instr alloca;
    alloca.op = cider::ir::Op::Alloca;
    alloca.width = cider::ir::IRType::Ptr;
    alloca.isPure = false;
    alloca.results.push_back(value(kLive + 1, cider::ir::IRType::Ptr));
    block.body.push_back(std::move(alloca));

    cider::ir::Instr call;
    call.op = cider::ir::Op::Call;
    call.callee = "callee";
    for (uint32_t at = 0; at < kCallWords; ++at) {
        cider::ir::Argument arg;
        arg.words.push_back(value(at + 1));
        call.arguments.push_back(std::move(arg));
    }
    block.body.push_back(std::move(call));

    block.term.term = cider::ir::Term::Ret;
    block.term.values.push_back(value(kLive));
    fn.blocks.push_back(std::move(block));
    return fn;
}

bool homeInSomeBlock(const cider::codegen::FunctionAllocation& alloc,
                     uint32_t id) {
    for (const cider::codegen::BlockAllocation& block : alloc.blocks) {
        if (id >= block.home.size()) {
            continue;
        }
        if (block.home[id] !=
            static_cast<uint8_t>(cider::codegen::Register::None)) {
            return true;
        }
    }
    return false;
}

void testCompactPressureFrame() {
    std::printf("compact pressure frame\n");
    const cider::ir::Function fn = pressureFunction();
    const auto alloc = cider::codegen::allocateFunction(fn);
    const cider::codegen::FrameLayout layout =
        cider::codegen::layoutFrame(fn, alloc);

    check(layout.frameBytes % 16 == 0, "frame is 16-byte aligned");
    check(layout.outgoingBytes >= 8 * 8,
          "outgoing area covers call words");
    check(noOverlap(layout.spillOffset), "spill slots do not overlap");
    check(noOverlap(layout.spillOffset, layout.allocaOffset),
          "spills do not overlap allocas");
    check(layout.frameBytes >= highestOffset(layout) + 8,
          "frame covers the highest slot");

    check(present(layout.allocaOffset[41]),
          "the alloca names its cell");
    check(!present(layout.spillOffset[41]),
          "the alloca is not also a spill slot");
    check(layout.allocaOffset[41] >= layout.outgoingBytes,
          "alloca cells sit above the outgoing area");

    bool disjointFromOutgoing = true;
    for (uint32_t offset : layout.spillOffset) {
        if (present(offset) && offset < layout.outgoingBytes) {
            disjointFromOutgoing = false;
        }
    }
    check(disjointFromOutgoing,
          "spill slots sit above the outgoing area");

    bool aligned = true;
    for (uint32_t offset : layout.spillOffset) {
        if (present(offset) && offset % 8 != 0) {
            aligned = false;
        }
    }
    for (uint32_t offset : layout.allocaOffset) {
        if (present(offset) && offset % 8 != 0) {
            aligned = false;
        }
    }
    check(aligned, "every slot is eight-byte aligned");

    check(presentCount(layout.spillOffset) <
              static_cast<uint32_t>(layout.spillOffset.size()),
          "the compact frame does not spill every id");

    uint32_t parameterSpills = 0;
    for (const cider::ir::Value& param : fn.params) {
        if (present(layout.spillOffset[param.id])) {
            ++parameterSpills;
        }
    }
    check(parameterSpills == fn.params.size(),
          "all unhomed parameters name their cells");

    bool homesMatch = true;
    for (uint32_t id = 0; id < layout.spillOffset.size(); ++id) {
        if (present(layout.allocaOffset[id])) {
            continue;
        }
        const bool homed = homeInSomeBlock(alloc, id);
        const bool spilled = present(layout.spillOffset[id]);
        if (homed && spilled) {
            homesMatch = false;
        }
    }
    check(homesMatch,
          "a value homed in any block never owns a spill slot");
}

// A tiny function with every value homed reserves no spill slot at all. This
// is the compactness the old per-id frame could not express.
void testHomedValuesKeepNoSlot() {
    std::printf("homed values keep no slot\n");
    cider::ir::Function fn;
    fn.name = "tiny";
    fn.signature = cider::Signature({}, nullptr, cider::Abi::SysV);
    cider::ir::Block block;
    block.label = 0;
    cider::ir::Instr first;
    first.op = cider::ir::Op::Const;
    first.width = cider::ir::IRType::I64;
    first.isPure = true;
    first.constantBits = 3;
    first.results.push_back(value(1));
    block.body.push_back(std::move(first));
    cider::ir::Instr second;
    second.op = cider::ir::Op::Add;
    second.width = cider::ir::IRType::I64;
    second.isPure = true;
    second.operands = {value(1), value(1)};
    second.results.push_back(value(2));
    block.body.push_back(std::move(second));
    block.term.term = cider::ir::Term::Ret;
    block.term.values.push_back(value(2));
    fn.blocks.push_back(std::move(block));

    // A synthetic allocation homes both values. The layout's contract is
    // about homes, not about whether this particular allocator chooses them.
    cider::codegen::FunctionAllocation alloc;
    alloc.blocks.resize(1);
    alloc.blocks[0].home.assign(3,
                                static_cast<uint8_t>(
                                    cider::codegen::Register::Rax));
    const cider::codegen::FrameLayout layout =
        cider::codegen::layoutFrame(fn, alloc);
    check(presentCount(layout.spillOffset) == 0,
          "a fully homed function reserves no spill slot");
    check(layout.outgoingBytes == 0, "a call-free function reserves no outgoing area");
    check(layout.frameBytes == 0,
          "a frame with no saved cells and no slots needs no bytes");
}

// A SysV variadic frame owns the 176-byte register-save area after the
// callee-save cells. One saved register is enough to prove the order: the
// save base comes first, the varargs base follows the saved cells, and the
// frame covers the whole save area.
void testVariadicFrameCoversSaves() {
    std::printf("variadic frame covers saves\n");
    cider::ir::Function fn;
    fn.name = "variadic";
    fn.signature =
        cider::Signature({}, nullptr, cider::Abi::SysV, true);
    cider::codegen::FunctionAllocation alloc;
    alloc.calleeSaved.push_back(cider::codegen::Register::Rbx);
    const cider::codegen::FrameLayout layout =
        cider::codegen::layoutFrame(fn, alloc);
    const uint32_t saveBase = cider::codegen::frameSaveBase(layout);
    const uint32_t varargsBase = cider::codegen::frameVarargsBase(
        layout, static_cast<uint32_t>(alloc.calleeSaved.size()));
    check(varargsBase ==
              saveBase +
                  static_cast<uint32_t>(alloc.calleeSaved.size()) *
                      cider::codegen::kFrameWordBytes,
          "the varargs base follows the callee-save cells");
    check(layout.frameBytes >=
              varargsBase + cider::codegen::kVarargsSaveBytes,
          "the frame covers the SysV variadic save area");
    check(layout.frameBytes % cider::codegen::kFrameQuantumBytes == 0,
          "the variadic frame keeps the 16-byte granule");
}

}  // namespace

int main() {
    testCompactPressureFrame();
    testHomedValuesKeepNoSlot();
    testVariadicFrameCoversSaves();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("%d checks, all passed\n", checks);
    return 0;
}
