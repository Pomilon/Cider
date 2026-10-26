// memory.cpp — conservative load/store forwarding for non-escaping slots.
//
// After mem2reg, this is the fallback for every slot that stays in memory:
// there are no phi nodes and no SSA construction here. It removes two kinds of
// work that do not need either:
//
//   - a load that reads a value the same slot already holds, replaced by that
//     stored value;
//   - a store whose bytes are fully overwritten by a later store before any
//     load of the slot.
//
// The entry condition is deliberately narrow. A slot is eligible only when its
// Alloca pointer is used exclusively as the base of direct Load/Store
// instructions with displacement zero and no global label. A Gep, a call
// argument, an asm placeholder, a cast, an Extract/Insert, a store of the
// pointer itself, a terminator value, or any other use marks the slot as
// escaping. Escaping slots are left completely alone. Because Alloca cells are
// adjacent eight-byte frame slots, one escaped address can reach a neighbour
// by displacement; one escaping Alloca therefore disables the pass for the
// entire function. A per-slot analysis alone cannot prove otherwise: an
// interface is two Allocas, and a Gep to its first word can write the second
// at +8.
//
// Within a block the only instruction that can change an eligible slot is a
// direct Load/Store through that same Alloca: the pointer never leaves the
// function, so no other address can name its cell. Volatile accesses and calls
// clear the whole forwarding state even when they do not name the slot: a call
// is the barrier this slice promises rather than a considered alias analysis.
// A function containing asm is not optimised at all: asm can name frame
// storage by register without an operand, and removing the only IR load of a
// store would let dead-code elimination delete a value the asm still reads.
//
// State also crosses an edge only when the destination has exactly one
// distinct predecessor and that predecessor has already been processed. A
// value stored before the edge is then live on the only path into the block.
// A dead store crosses that edge only when every block since the store has a
// single successor: with a second successor another path could still read the
// value, so the store is not dead there.

#include "opt/opt.h"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cider {
namespace opt {

namespace {

// One result is the shape every forwarded load has.
constexpr uint64_t kOneResult = 1;

// True for the four instructions whose addressBase names a pointer-like base.
bool namesBase(ir::Op op) {
    return op == ir::Op::Load || op == ir::Op::Store ||
           op == ir::Op::Extract || op == ir::Op::Insert ||
           op == ir::Op::Gep;
}

// Bytes touched by a value of this width. Void is the full eight-byte cell
// here: a Store that leaves `width` unset is emitted as a 64-bit move (see
// codegen's emitStoreMem), so it writes the whole slot even when its stored
// value is narrower.
uint8_t widthBytes(ir::IRType width) {
    switch (width) {
        case ir::IRType::I8:
        case ir::IRType::U8:
        case ir::IRType::Bool:
            return 1;
        case ir::IRType::I16:
        case ir::IRType::U16:
            return 2;
        case ir::IRType::I32:
        case ir::IRType::U32:
        case ir::IRType::F32:
            return 4;
        case ir::IRType::I64:
        case ir::IRType::U64:
        case ir::IRType::F64:
        case ir::IRType::Ptr:
            return 8;
        case ir::IRType::Void:
            return 8;
    }
    return 0;
}

// What a Store puts in the slot, when that is safe to reason about. The value
// carries the width and language type that were canonicalised into its slot;
// `bytes` is the store's actual memory extent.
struct StoreInfo {
    bool ok = false;
    ir::Value value;
    ir::IRType width = ir::IRType::Void;
    TypeRef type;
    uint8_t bytes = 0;
};

StoreInfo storeInfo(const ir::Instr& instr) {
    StoreInfo out;
    if (instr.op != ir::Op::Store || instr.isVolatile || instr.mayTrap) {
        return out;
    }
    const ir::Value& value = instr.storedValue;
    if (value.width == ir::IRType::Void || value.type.isError()) {
        return out;
    }
    const uint8_t valueBytes = widthBytes(value.width);
    const uint8_t bytes = widthBytes(instr.width);
    // A value whose width does not fit the bytes the store writes cannot be
    // forwarded: the load would read bytes the store never produced.
    if (valueBytes == 0 || bytes < valueBytes) {
        return out;
    }
    out.ok = true;
    out.value = value;
    out.width = value.width;
    out.type = value.type;
    out.bytes = bytes;
    return out;
}

// The state for one eligible slot. `known` means a store wrote a value whose
// width/type can answer a later load. `last` means that store has not been
// read since and may still be eliminated by a covering overwrite.
struct SlotState {
    bool known = false;
    ir::Value value;
    ir::IRType width = ir::IRType::Void;
    TypeRef type;
    uint8_t bytes = 0;

    bool last = false;
    uint64_t lastBlock = 0;
    uint64_t lastIndex = 0;
    bool lastSafe = false;
};

using SlotStates = std::unordered_map<uint32_t, SlotState>;

// Collect the Alloca results that never escape. Any use this function does not
// recognise marks the slot escaping, which is the direction that cannot lose
// semantics.
//
// Alloca cells are adjacent eight-byte frame slots. An address that escapes
// for one cell can name a neighbour by displacement, so one escaping Alloca
// makes every cell in the function potentially reachable through that pointer.
// The whole function therefore keeps the old shape unless every Alloca is
// direct-only. This is the neighbour hazard that a per-slot analysis cannot
// see: an interface is two Allocas, and a Gep to its first word can write the
// second at +8.
std::unordered_set<uint32_t> eligibleSlots(const ir::Function& fn) {
    std::unordered_set<uint32_t> allocas;
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op == ir::Op::Alloca) {
                for (const ir::Value& result : instr.results) {
                    allocas.insert(result.id);
                }
            }
        }
    }
    std::unordered_set<uint32_t> escaped;
    std::unordered_set<uint32_t> unsafe;
    bool hasAsm = false;
    bool hasSwitch = false;
    auto note = [&](uint32_t id, std::unordered_set<uint32_t>& into) {
        if (allocas.find(id) != allocas.end()) {
            into.insert(id);
        }
    };
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            if (instr.op == ir::Op::Alloca) {
                continue;
            }
            if (instr.op == ir::Op::Asm) {
                // Asm can name frame storage by register without an operand.
                // A later dead-code pass would then see no IR reader for a
                // store whose only load this pass forwarded. Keep the whole
                // function's memory shape.
                hasAsm = true;
            }
            for (const ir::Value& operand : instr.operands) {
                note(operand.id, escaped);
            }
            for (const ir::Argument& arg : instr.arguments) {
                for (const ir::Value& word : arg.words) {
                    note(word.id, escaped);
                }
            }
            for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                note(incoming.value.id, escaped);
            }
            if (namesBase(instr.op)) {
                // A load or store through a link label names the label, not
                // its default zero addressBase. Reading that default as an
                // Alloca id would taint the whole function for an access the
                // instruction does not make.
                const bool labelBase =
                    (instr.op == ir::Op::Load || instr.op == ir::Op::Store) &&
                    !instr.globalLabel.empty();
                if (!labelBase && allocas.find(instr.addressBase.id) !=
                                     allocas.end()) {
                    if (instr.op == ir::Op::Load || instr.op == ir::Op::Store) {
                        if (instr.displacement != 0) {
                            note(instr.addressBase.id, escaped);
                        } else if (instr.isVolatile) {
                            note(instr.addressBase.id, unsafe);
                        }
                    } else {
                        // Gep, Extract and Insert take a pointer that may be
                        // offset or widened; the slot has escaped our reach.
                        note(instr.addressBase.id, escaped);
                    }
                }
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                note(instr.storedValue.id, escaped);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                note(instr.calleeValue.id, escaped);
            }
        }
        const ir::Terminator& term = block.term;
        for (const ir::Value& word : term.values) {
            note(word.id, escaped);
        }
        if (term.cond.has_value()) {
            note(term.cond->id, escaped);
        }
        if (term.term == ir::Term::Switch) {
            // Lowering emits branches, not Switch, today. DCE's use set does
            // not yet count case values, so leaving such a function alone
            // avoids exposing that gap through a forwarded case value.
            hasSwitch = true;
            note(term.switchValue.id, escaped);
            for (const ir::Case& arm : term.cases) {
                note(arm.value.id, escaped);
            }
        }
    }
    if (!escaped.empty() || hasAsm || hasSwitch) {
        return {};
    }
    std::unordered_set<uint32_t> eligible;
    for (uint32_t id : allocas) {
        if (unsafe.find(id) == unsafe.end()) {
            eligible.insert(id);
        }
    }
    return eligible;
}

// Distinct successor targets of a terminator. Repeated targets (CondBr with
// both arms equal, a Switch repeating an arm) are one successor.
std::unordered_set<uint32_t> successors(const ir::Terminator& term) {
    std::unordered_set<uint32_t> out;
    switch (term.term) {
        case ir::Term::Br:
            out.insert(term.target);
            break;
        case ir::Term::CondBr:
            out.insert(term.thenTarget);
            out.insert(term.elseTarget);
            break;
        case ir::Term::Switch:
            for (const ir::Case& arm : term.cases) {
                out.insert(arm.target);
            }
            out.insert(term.defaultTarget);
            break;
        case ir::Term::Ret:
        case ir::Term::Unreachable:
        case ir::Term::Trap:
            break;
    }
    return out;
}

// Rewrite every semantic use of a value id. Definitions are never touched:
// the map only names results of loads this pass deletes.
void replaceUses(ir::Function& fn,
                 const std::unordered_map<uint32_t, ir::Value>& subst) {
    auto remap = [&](ir::Value& value) {
        auto found = subst.find(value.id);
        if (found != subst.end()) {
            value = found->second;
        }
    };
    for (ir::Block& block : fn.blocks) {
        for (ir::Instr& instr : block.body) {
            for (ir::Value& operand : instr.operands) {
                remap(operand);
            }
            for (ir::Argument& arg : instr.arguments) {
                for (ir::Value& word : arg.words) {
                    remap(word);
                }
            }
            for (ir::PhiIncoming& incoming : instr.phiIncoming) {
                remap(incoming.value);
            }
            if (namesBase(instr.op) && instr.globalLabel.empty()) {
                remap(instr.addressBase);
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                remap(instr.storedValue);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                remap(instr.calleeValue);
            }
        }
        ir::Terminator& term = block.term;
        for (ir::Value& word : term.values) {
            remap(word);
        }
        if (term.cond.has_value()) {
            remap(*term.cond);
        }
        if (term.term == ir::Term::Switch) {
            remap(term.switchValue);
            for (ir::Case& arm : term.cases) {
                remap(arm.value);
            }
        }
    }
}

}  // namespace

MemoryResult forwardMemory(ir::Module& module) {
    MemoryResult result;
    for (ir::Function& fn : module.functions) {
        std::unordered_set<uint32_t> eligible = eligibleSlots(fn);
        if (eligible.empty()) {
            continue;
        }
        const uint64_t blockCount = fn.blocks.size();
        std::vector<std::vector<uint8_t>> drop(blockCount);
        for (uint64_t i = 0; i < blockCount; ++i) {
            const uint64_t slots = fn.blocks[i].body.size();
            drop[i].assign(slots, 0);
        }
        // Distinct predecessor count per target block, capped at two: one
        // means the state may cross, two or more means it may not.
        struct Pred {
            uint64_t first = 0;
            uint64_t count = 0;
        };
        std::unordered_map<uint32_t, Pred> preds;
        std::vector<uint64_t> succCount(blockCount, 0);
        for (uint64_t i = 0; i < blockCount; ++i) {
            std::unordered_set<uint32_t> targets =
                successors(fn.blocks[i].term);
            succCount[i] = targets.size();
            for (uint32_t target : targets) {
                Pred& pred = preds[target];
                if (pred.count == 0) {
                    pred.first = i;
                    pred.count = 1;
                } else if (pred.first != i) {
                    pred.count = 2;
                }
            }
        }
        std::unordered_map<uint32_t, SlotStates> outStates;
        std::unordered_map<uint32_t, ir::Value> subst;
        for (uint64_t i = 0; i < blockCount; ++i) {
            ir::Block& block = fn.blocks[i];
            SlotStates state;
            // The first block is the function entry; an edge into it is a
            // back edge, not a definition of its entry state.
            if (i != 0) {
                auto found = preds.find(block.label);
                if (found != preds.end() && found->second.count == 1 &&
                    found->second.first < i) {
                    auto out = outStates.find(fn.blocks[found->second.first].label);
                    if (out != outStates.end()) {
                        state = out->second;
                    }
                }
            }
            for (uint64_t j = 0; j < block.body.size(); ++j) {
                ir::Instr& instr = block.body[j];
                if (instr.isVolatile) {
                    // A volatile access is a barrier even for other slots:
                    // this slice never reasons across one.
                    state.clear();
                    continue;
                }
                if (instr.op == ir::Op::Call || instr.op == ir::Op::Asm) {
                    state.clear();
                    continue;
                }
                if (instr.op == ir::Op::Store) {
                    if (eligible.find(instr.addressBase.id) == eligible.end() ||
                        !instr.globalLabel.empty() || instr.displacement != 0) {
                        continue;
                    }
                    StoreInfo info = storeInfo(instr);
                    if (!info.ok) {
                        state.erase(instr.addressBase.id);
                        continue;
                    }
                    SlotState& slot = state[instr.addressBase.id];
                    if (slot.known && slot.last && slot.lastSafe &&
                        slot.width == info.width && slot.type == info.type &&
                        info.bytes >= slot.bytes) {
                        drop[slot.lastBlock][slot.lastIndex] = 1;
                        ++result.storesRemoved;
                    }
                    // A forwarded load earlier may have named a value that
                    // is itself a deleted load; canonicalise the store now
                    // so later forwards never resurrect that result.
                    auto mapped = subst.find(info.value.id);
                    slot.known = true;
                    slot.value = mapped != subst.end() ? mapped->second : info.value;
                    slot.width = info.width;
                    slot.type = info.type;
                    slot.bytes = info.bytes;
                    slot.last = true;
                    slot.lastBlock = i;
                    slot.lastIndex = j;
                    slot.lastSafe = true;
                    continue;
                }
                if (instr.op == ir::Op::Load) {
                    if (eligible.find(instr.addressBase.id) == eligible.end() ||
                        !instr.globalLabel.empty() || instr.displacement != 0) {
                        continue;
                    }
                    auto found = state.find(instr.addressBase.id);
                    if (found != state.end() && found->second.known &&
                        instr.results.size() == kOneResult && instr.isPure &&
                        !instr.mayTrap && instr.width == found->second.width &&
                        instr.type == found->second.type &&
                        widthBytes(instr.width) <= found->second.bytes) {
                        subst[instr.results[0].id] = found->second.value;
                        drop[i][j] = 1;
                        ++result.forwarded;
                        // The delete removes the read but does not change the
                        // cell; the store is still not removable as "never
                        // read" because a later overwrite is only safe if
                        // this access is gone. Keeping `last` truthful is
                        // cheaper than reasoning about a forwarded read.
                        found->second.last = false;
                    } else {
                        // A mismatched read, or a read of a slot whose value
                        // is unknown, makes every later answer uncertain.
                        state.erase(instr.addressBase.id);
                    }
                    continue;
                }
            }
            if (succCount[i] != 1) {
                for (auto& entry : state) {
                    entry.second.lastSafe = false;
                }
            }
            outStates[block.label] = std::move(state);
        }
        replaceUses(fn, subst);
        for (uint64_t i = 0; i < blockCount; ++i) {
            std::vector<ir::Instr> kept;
            for (uint64_t j = 0; j < fn.blocks[i].body.size(); ++j) {
                if (!drop[i][j]) {
                    kept.push_back(std::move(fn.blocks[i].body[j]));
                }
            }
            fn.blocks[i].body = std::move(kept);
        }
    }
    return result;
}

}  // namespace opt
}  // namespace cider
