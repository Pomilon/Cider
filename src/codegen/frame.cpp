// frame.cpp — compact frame arithmetic for the fast backend.
//
// The input is the lowered function and the whole-function allocation. The
// output is one offset per memory cell, with the outgoing call area at the
// bottom of the frame. The layout is a pure function of its inputs: it reads
// no target state, writes nothing, and is stable in value-id order.
//
// The outgoing area must cover the widest call shape the function contains.
// That is the argument words past the register row for SysV, and the 32-byte
// Microsoft shadow space plus the words past its four positional homes. The
// helper below follows the emitter's call path; when the ABI row changes both
// sites change together.

#include "codegen/frame.h"

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "layout.h"

namespace cider {
namespace codegen {

namespace {

constexpr uint32_t kSlotBytes = kFrameWordBytes;
constexpr uint32_t kFrameQuantum = kFrameQuantumBytes;
constexpr uint32_t kInvalidValue = 0xFFFFFFFFu;

bool namesAddressBase(ir::Op op) {
    return op == ir::Op::Load || op == ir::Op::Store ||
           op == ir::Op::Extract || op == ir::Op::Insert ||
           op == ir::Op::Gep;
}

bool namesStoredValue(ir::Op op) {
    return op == ir::Op::Store || op == ir::Op::Insert;
}

void noteValue(uint32_t id, uint32_t& top, bool& any) {
    if (id == kInvalidValue) {
        return;
    }
    if (!any || id > top) {
        top = id;
        any = true;
    }
}

void noteInstr(const ir::Instr& instr, uint32_t& top, bool& any) {
    for (const ir::Value& result : instr.results) {
        noteValue(result.id, top, any);
    }
    for (const ir::Value& operand : instr.operands) {
        noteValue(operand.id, top, any);
    }
    if (namesAddressBase(instr.op)) {
        noteValue(instr.addressBase.id, top, any);
    }
    if (namesStoredValue(instr.op)) {
        noteValue(instr.storedValue.id, top, any);
    }
    if (instr.op == ir::Op::Call && instr.call.indirect) {
        noteValue(instr.calleeValue.id, top, any);
    }
    for (const ir::Argument& arg : instr.arguments) {
        for (const ir::Value& word : arg.words) {
            noteValue(word.id, top, any);
        }
    }
    for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
        noteValue(incoming.value.id, top, any);
    }
}

void noteTerm(const ir::Terminator& term, uint32_t& top, bool& any) {
    for (const ir::Value& word : term.values) {
        noteValue(word.id, top, any);
    }
    if (term.cond.has_value()) {
        noteValue(term.cond->id, top, any);
    }
    if (term.term == ir::Term::Switch) {
        noteValue(term.switchValue.id, top, any);
        for (const ir::Case& arm : term.cases) {
            noteValue(arm.value.id, top, any);
        }
    }
}

uint32_t valueTop(const ir::Function& fn) {
    uint32_t top = 0;
    bool any = false;
    for (const ir::Value& param : fn.params) {
        noteValue(param.id, top, any);
    }
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            noteInstr(instr, top, any);
        }
        noteTerm(block.term, top, any);
    }
    return any ? top : 0;
}

}  // namespace

CallFrameShape callFrameShape(const ir::Instr& call) {
    CallFrameShape shape;
    if (call.call.msabi) {
        uint32_t slots = 0;
        for (const ir::Argument& arg : call.arguments) {
            if (isSingleFloatWordArgument(arg)) {
                slots += kSingleFloatWord;
            } else {
                slots += static_cast<uint32_t>(arg.words.size());
            }
        }
        shape.stackWords = slots > kMsPositionalArgumentHomes
                               ? slots - kMsPositionalArgumentHomes
                               : 0;
        shape.outgoingBase = kMsShadowSpaceBytes;
        shape.outgoingBytes =
            shape.outgoingBase + shape.stackWords * kFrameWordBytes;
        shape.vectorRegs = 0;
        return shape;
    }
    uint32_t gprWords = 0;
    uint32_t sseWords = 0;
    for (const ir::Argument& arg : call.arguments) {
        if (isSingleFloatWordArgument(arg)) {
            ++sseWords;
            continue;
        }
        gprWords += static_cast<uint32_t>(arg.words.size());
    }
    shape.stackWords =
        gprWords > kSysVIntegerArgumentRegs
            ? gprWords - kSysVIntegerArgumentRegs
            : 0;
    shape.outgoingBase = 0;
    shape.outgoingBytes = shape.stackWords * kFrameWordBytes;
    shape.vectorRegs = sseWords;
    return shape;
}

uint32_t frameSaveBase(const FrameLayout& layout) {
    uint32_t base = layout.outgoingBytes;
    for (uint32_t offset : layout.spillOffset) {
        if (offset == kNoFrameSlot) {
            continue;
        }
        const uint32_t candidate = offset + kFrameWordBytes;
        if (candidate > base) {
            base = candidate;
        }
    }
    for (uint32_t offset : layout.allocaOffset) {
        if (offset == kNoFrameSlot) {
            continue;
        }
        const uint32_t candidate = offset + kFrameWordBytes;
        if (candidate > base) {
            base = candidate;
        }
    }
    return base;
}

uint32_t frameVarargsBase(const FrameLayout& layout,
                          uint32_t calleeSavedCount) {
    return frameSaveBase(layout) + calleeSavedCount * kFrameWordBytes;
}

FrameLayout layoutFrame(const ir::Function& fn,
                        const FunctionAllocation& alloc) {
    FrameLayout frame;
    const uint32_t top = valueTop(fn);
    const uint32_t valueCount = top + 1;
    frame.spillOffset.assign(valueCount, kNoFrameSlot);
    frame.allocaOffset.assign(valueCount, kNoFrameSlot);

    // The widest call decides how far the outgoing area reaches. Rounding it
    // to the frame granule also gives an odd count of stack words its pad.
    uint32_t outgoing = 0;
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op != ir::Op::Call) {
                continue;
            }
            const CallFrameShape shape = callFrameShape(instr);
            if (shape.outgoingBytes > outgoing) {
                outgoing = shape.outgoingBytes;
            }
        }
    }
    frame.outgoingBytes = static_cast<uint32_t>(
        layout::roundUp(outgoing, kFrameQuantum));

    std::vector<uint8_t> mentioned(valueCount, 0);
    std::unordered_set<uint32_t> allocaIds;
    auto mention = [&](uint32_t id) {
        if (id != kInvalidValue && id < mentioned.size()) {
            mentioned[id] = 1;
        }
    };
    for (const ir::Value& param : fn.params) {
        mention(param.id);
    }
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            for (const ir::Value& result : instr.results) {
                mention(result.id);
            }
            for (const ir::Value& operand : instr.operands) {
                mention(operand.id);
            }
            if (namesAddressBase(instr.op)) {
                mention(instr.addressBase.id);
            }
            if (namesStoredValue(instr.op)) {
                mention(instr.storedValue.id);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                mention(instr.calleeValue.id);
            }
            for (const ir::Argument& arg : instr.arguments) {
                for (const ir::Value& word : arg.words) {
                    mention(word.id);
                }
            }
            for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                mention(incoming.value.id);
            }
            if (instr.op == ir::Op::Alloca && !instr.results.empty()) {
                allocaIds.insert(instr.results[0].id);
            }
        }
        for (const ir::Value& word : block.term.values) {
            mention(word.id);
        }
        if (block.term.cond.has_value()) {
            mention(block.term.cond->id);
        }
        if (block.term.term == ir::Term::Switch) {
            mention(block.term.switchValue.id);
            for (const ir::Case& arm : block.term.cases) {
                mention(arm.value.id);
            }
        }
    }

    // A value needs a spill slot exactly when no block homes it. Parameters
    // are never homed (the prologue writes their slots), and the allocator
    // already excludes floats, Void, multi-definition ids, alloca results and
    // the fixed-scratch shapes.
    std::vector<uint8_t> homed(valueCount, 0);
    for (const BlockAllocation& block : alloc.blocks) {
        const uint32_t limit =
            static_cast<uint32_t>(block.home.size() < homed.size()
                                      ? block.home.size()
                                      : homed.size());
        for (uint32_t id = 0; id < limit; ++id) {
            if (block.home[id] !=
                static_cast<uint8_t>(Register::None)) {
                homed[id] = 1;
            }
        }
    }

    uint32_t at = frame.outgoingBytes;
    for (uint32_t id = 0; id < valueCount; ++id) {
        if (mentioned[id] == 0 || homed[id] != 0 ||
            allocaIds.count(id) != 0) {
            continue;
        }
        frame.spillOffset[id] = at;
        at += kSlotBytes;
    }
    // Alloca cells keep value-id order, so the consecutive words a lowering
    // emits for one aggregate stay consecutive and a displacement reaches
    // the same neighbour it reached in the legacy frame.
    for (uint32_t id = 0; id < valueCount; ++id) {
        if (mentioned[id] == 0 || allocaIds.count(id) == 0) {
            continue;
        }
        frame.allocaOffset[id] = at;
        at += kSlotBytes;
    }
    const bool sysVVarargs =
        fn.signature.abi() != Abi::MsAbi && fn.signature.variadic();
    uint32_t end = frameVarargsBase(
        frame, static_cast<uint32_t>(alloc.calleeSaved.size()));
    if (sysVVarargs) {
        end += kVarargsSaveBytes;
    }
    frame.frameBytes = static_cast<uint32_t>(
        layout::roundUp(end, kFrameQuantum));
    return frame;
}

}  // namespace codegen
}  // namespace cider
