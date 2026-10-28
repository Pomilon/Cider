// mem2reg.cpp — promote non-escaping scalar slots to SSA with real phis.
//
// The pass is Cytron-style: find the blocks that define each promotable slot,
// insert one phi at the iterated dominance frontier, then rename with a
// dominator-tree walk and a value stack per slot. Loads become the reaching
// value; stores disappear; the Alloca instruction disappears once every
// access to its cell is gone.
//
// The input to this pass is the lowerer's stack-heavy IR. The output keeps
// phi instructions in the optimized IR: `cider ir` shows them, the VM runs
// them directly, and codegen consumes them through `lowerPhis`, which turns
// each phi into parallel edge copies after splitting critical edges.
//
// Safety is deliberately narrower than "the pointer did not escape":
//   - Alloca cells are adjacent eight-byte frame slots. One escaped Alloca
//     address can name a neighbour by displacement, so any escaping Alloca
//     disables promotion for the whole function. The same is true of a
//     non-zero displacement access: lowerer offsets within an aggregate word
//     are not tracked as ranges here.
//   - A function with asm is skipped: asm can name frame storage without an
//     IR operand.
//   - A slot must be a single-word scalar (basic, pointer, function pointer or
//     enum), used only by direct zero-displacement Load/Store with the exact
//     same IRType and language TypeRef. Volatile, trapping, partial-width and
//     mismatched accesses disqualify the slot.
//   - A load is promoted only when the slot is definitely assigned on every
//     path to it. `= uninit` deliberately stores nothing, so such a slot keeps
//     its memory load; this pass never invents a zero.
//   - A phi block must be definitely assigned on entry, so no incoming edge
//     needs an invented value. Phis in the function's entry block are not
//     built; such a slot stays in memory for `forwardMemory`.

#include "opt/opt.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "opt/dominance.h"

namespace cider {
namespace opt {

namespace {

constexpr uint32_t kInvalidValue = 0xFFFFFFFFu;
constexpr uint32_t kLastFreshId = 0xFFFFFFFEu;
constexpr uint64_t kOneResult = 1;

bool namesBase(ir::Op op) {
    return op == ir::Op::Load || op == ir::Op::Store ||
           op == ir::Op::Extract || op == ir::Op::Insert ||
           op == ir::Op::Gep;
}

bool scalarSlot(const TypeRef& type) {
    if (type.isError() || type.type == nullptr) {
        return false;
    }
    const int64_t bytes = typeSize(type.type);
    if (bytes < 0 || bytes > static_cast<int64_t>(layout::kSizePointer)) {
        return false;
    }
    switch (type.type->kind()) {
        case Kind::Basic:
        case Kind::Pointer:
        case Kind::FunctionPtr:
        case Kind::Enum:
            return true;
        case Kind::Error:
        case Kind::Void:
        case Kind::String:
        case Kind::Array:
        case Kind::Slice:
        case Kind::Struct:
        case Kind::Class:
        case Kind::Interface:
        case Kind::Union:
        case Kind::VaList:
        case Kind::Closure:
        case Kind::FunctionItem:
        case Kind::TypeParam:
        case Kind::Alias:
        case Kind::Wildcard:
            return false;
    }
    return false;
}

struct Access {
    uint64_t block = 0;
    uint64_t index = 0;
    bool load = false;
};

struct SlotInfo {
    uint32_t id = 0;
    TypeRef type;
    ir::IRType width = ir::IRType::Void;
    bool param = false;
    bool hasLoad = false;
    bool volatileAccess = false;
    std::vector<Access> accesses;
    std::unordered_set<uint32_t> storeBlocks;

};

struct PhiRecord {
    uint32_t slot = 0;
    uint32_t block = 0;
    ir::Value result;
    std::vector<uint32_t> preds;
    std::unordered_map<uint32_t, ir::Value> incoming;
};

struct BlockAccess {
    uint32_t slot = 0;
    uint64_t index = 0;
    bool load = false;
};

uint32_t maxValueId(const ir::Function& fn) {
    uint32_t top = 0;
    auto note = [&](uint32_t id) {
        if (id > top && id != kInvalidValue) {
            top = id;
        }
    };
    for (const ir::Value& param : fn.params) {
        note(param.id);
    }
    for (const ir::Block& block : fn.blocks) {
        for (const ir::Instr& instr : block.body) {
            for (const ir::Value& result : instr.results) {
                note(result.id);
            }
            for (const ir::Value& operand : instr.operands) {
                note(operand.id);
            }
            for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                note(incoming.value.id);
            }
            if (namesBase(instr.op)) {
                note(instr.addressBase.id);
            }
            if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                note(instr.storedValue.id);
            }
            if (instr.op == ir::Op::Call && instr.call.indirect) {
                note(instr.calleeValue.id);
            }
        }
        const ir::Terminator& term = block.term;
        for (const ir::Value& value : term.values) {
            note(value.id);
        }
        if (term.cond.has_value()) {
            note(term.cond->id);
        }
        if (term.term == ir::Term::Switch) {
            note(term.switchValue.id);
            for (const ir::Case& arm : term.cases) {
                note(arm.value.id);
            }
        }
    }
    return top;
}

// Every semantic use of a value id, with phi incoming values included. The
// remap is the same field set the structural checker reads.
void replaceUses(ir::Function& fn,
                 const std::unordered_map<uint32_t, ir::Value>& subst) {
    if (subst.empty()) {
        return;
    }
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
        for (ir::Value& value : term.values) {
            remap(value);
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

PromotionResult mem2reg(ir::Module& module) {
    PromotionResult result;
    for (ir::Function& fn : module.functions) {
        if (fn.blocks.empty()) {
            continue;
        }
        bool hasAsm = false;
        std::unordered_set<uint32_t> allocas;
        std::unordered_map<uint32_t, SlotInfo> slots;
        std::unordered_map<uint32_t, uint64_t> blockAt;
        for (uint64_t i = 0; i < fn.blocks.size(); ++i) {
            blockAt[fn.blocks[i].label] = i;
        }
        for (const ir::Block& block : fn.blocks) {
            for (const ir::Instr& instr : block.body) {
                if (instr.op == ir::Op::Asm) {
                    hasAsm = true;
                }
                if (instr.op != ir::Op::Alloca || instr.results.empty()) {
                    continue;
                }
                uint32_t id = instr.results[0].id;
                allocas.insert(id);
                SlotInfo info;
                info.id = id;
                if (instr.type.type != nullptr) {
                    info.type = TypeRef{instr.type.type->pointee()};
                }
                slots[id] = info;
            }
        }
        if (hasAsm || allocas.empty()) {
            continue;
        }
        if (blockAt.size() != fn.blocks.size()) {
            // Duplicate block labels are a structural error; leave the
            // function alone rather than guess which block was meant.
            continue;
        }

        // First sweep: classify every use and record direct accesses.
        bool tainted = false;
        auto escape = [&](uint32_t id) {
            if (allocas.find(id) != allocas.end()) {
                tainted = true;
            }
        };
        for (uint64_t bi = 0; bi < fn.blocks.size(); ++bi) {
            const ir::Block& block = fn.blocks[bi];
            for (uint64_t ii = 0; ii < block.body.size(); ++ii) {
                const ir::Instr& instr = block.body[ii];
                if (instr.op == ir::Op::Alloca) {
                    continue;
                }
                for (const ir::Value& operand : instr.operands) {
                    escape(operand.id);
                }
                for (const ir::Argument& arg : instr.arguments) {
                    for (const ir::Value& word : arg.words) {
                        escape(word.id);
                    }
                }
                for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                    escape(incoming.value.id);
                }
                if (namesBase(instr.op)) {
                    const bool labelBase =
                        (instr.op == ir::Op::Load ||
                         instr.op == ir::Op::Store) &&
                        !instr.globalLabel.empty();
                    auto found = allocas.find(instr.addressBase.id);
                    if (!labelBase && found != allocas.end()) {
                        if (instr.op == ir::Op::Load ||
                            instr.op == ir::Op::Store) {
                            SlotInfo& slot = slots[instr.addressBase.id];
                            if (instr.displacement != 0) {
                                // A displacement can cross into the next
                                // frame cell, and this slice has no range
                                // proof; leave the function in memory.
                                tainted = true;
                            } else if (instr.isVolatile) {
                                slot.volatileAccess = true;
                            } else {
                                Access access;
                                access.block = bi;
                                access.index = ii;
                                access.load = instr.op == ir::Op::Load;
                                slot.accesses.push_back(access);
                                if (access.load) {
                                    slot.hasLoad = true;
                                } else {
                                    slot.storeBlocks.insert(block.label);
                                }
                            }
                        } else {
                            tainted = true;
                        }
                    }
                }
                if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
                    escape(instr.storedValue.id);
                }
                if (instr.op == ir::Op::Call && instr.call.indirect) {
                    escape(instr.calleeValue.id);
                }
            }
            const ir::Terminator& term = block.term;
            for (const ir::Value& value : term.values) {
                escape(value.id);
            }
            if (term.cond.has_value()) {
                escape(term.cond->id);
            }
            if (term.term == ir::Term::Switch) {
                escape(term.switchValue.id);
                for (const ir::Case& arm : term.cases) {
                    escape(arm.value.id);
                }
            }
        }
        if (tainted) {
            continue;
        }

        std::unordered_map<uint32_t, uint64_t> paramIndex;
        for (uint64_t i = 0; i < fn.params.size(); ++i) {
            paramIndex[fn.params[i].id] = i;
        }
        const DominatorInfo dominators = computeDominators(fn);
        const uint64_t blockCount = fn.blocks.size();
        std::vector<uint8_t> reachable(blockCount, 0);
        for (uint64_t i = 0; i < blockCount; ++i) {
            reachable[i] =
                dominators.reachable.find(fn.blocks[i].label) !=
                dominators.reachable.end();
        }
        auto reachablePreds = [&](uint32_t label) {
            std::vector<uint64_t> out;
            auto found = dominators.predecessors.find(label);
            if (found == dominators.predecessors.end()) {
                return out;
            }
            for (uint32_t pred : found->second) {
                auto at = blockAt.find(pred);
                if (at != blockAt.end() && reachable[at->second]) {
                    out.push_back(at->second);
                }
            }
            return out;
        };
        auto frontier = dominanceFrontiers(fn, dominators);

        // Validate each candidate and compute definite assignment.
        std::vector<uint32_t> order;
        order.reserve(slots.size());
        for (const auto& entry : slots) {
            order.push_back(entry.first);
        }
        std::sort(order.begin(), order.end());
        std::vector<uint32_t> selected;
        std::unordered_map<uint32_t, std::vector<uint32_t>> phiBlocksBySlot;
        for (uint32_t id : order) {
            SlotInfo& slot = slots[id];
            if (!scalarSlot(slot.type) || slot.volatileAccess ||
                !slot.hasLoad || slot.accesses.empty()) {
                continue;
            }
            bool ok = true;
            bool widthKnown = false;
            for (const Access& access : slot.accesses) {
                if (access.block >= blockCount) {
                    ok = false;
                    break;
                }
                if (!reachable[access.block]) {
                    // Promotion is defined over the reachable CFG. A slot
                    // touched in an unreachable block stays in memory.
                    ok = false;
                    break;
                }
                const ir::Instr& instr = fn.blocks[access.block].body[access.index];
                if (access.load) {
                    if (!instr.isPure || instr.mayTrap || instr.isVolatile ||
                        instr.type != slot.type ||
                        instr.width == ir::IRType::Void) {
                        ok = false;
                        break;
                    }
                    if (!widthKnown) {
                        slot.width = instr.width;
                        widthKnown = true;
                    } else if (slot.width != instr.width) {
                        ok = false;
                        break;
                    }
                } else {
                    if (instr.mayTrap || instr.isVolatile ||
                        instr.type != slot.type ||
                        instr.storedValue.type != slot.type ||
                        instr.storedValue.width == ir::IRType::Void) {
                        ok = false;
                        break;
                    }
                    if (instr.width != ir::IRType::Void &&
                        instr.width != instr.storedValue.width) {
                        ok = false;
                        break;
                    }
                    if (!widthKnown) {
                        slot.width = instr.storedValue.width;
                        widthKnown = true;
                    } else if (slot.width != instr.storedValue.width) {
                        ok = false;
                        break;
                    }
                }
            }
            if (!ok || !widthKnown) {
                continue;
            }
            auto param = paramIndex.find(id);
            slot.param = param != paramIndex.end();
            if (!scalarSlot(slot.type)) {
                continue;
            }

            // Definite assignment over reachable blocks. A parameter is
            // assigned at entry; an ordinary slot becomes assigned at each
            // store. A load is promoted only where every predecessor path
            // already assigned the slot.
            const uint32_t entry = fn.blocks.front().label;
            // Definite assignment is a must-analysis: start optimistic
            // (every reachable block assigned) and lower a block only when a
            // predecessor path proves it is not. Starting at zero would
            // mis-handle a parameter or entry store carried around a loop:
            // the latch's value is assigned even though the latch has no
            // store of its own.
            std::vector<uint8_t> in(blockCount, 1);
            std::vector<uint8_t> out(blockCount, 1);
            std::vector<uint8_t> hasStore(blockCount, 0);
            for (uint32_t label : slot.storeBlocks) {
                auto at = blockAt.find(label);
                if (at != blockAt.end()) {
                    hasStore[at->second] = 1;
                }
            }
            for (uint64_t round = 0; round <= blockCount; ++round) {
                bool changed = false;
                for (uint64_t bi = 0; bi < blockCount; ++bi) {
                    if (!reachable[bi]) {
                        continue;
                    }
                    uint8_t nextIn = 0;
                    if (fn.blocks[bi].label == entry) {
                        nextIn = slot.param ? 1 : 0;
                    } else {
                        std::vector<uint64_t> preds =
                            reachablePreds(fn.blocks[bi].label);
                        bool any = !preds.empty();
                        bool all = true;
                        for (uint64_t pred : preds) {
                            if (!out[pred]) {
                                all = false;
                                break;
                            }
                        }
                        nextIn = any && all ? 1 : 0;
                    }
                    uint8_t nextOut =
                        static_cast<uint8_t>(nextIn || hasStore[bi]);
                    if (in[bi] != nextIn || out[bi] != nextOut) {
                        in[bi] = nextIn;
                        out[bi] = nextOut;
                        changed = true;
                    }
                }
                if (!changed) {
                    break;
                }
            }
            uint64_t currentBlock = blockCount;
            uint8_t have = 0;
            for (const Access& access : slot.accesses) {
                if (access.block != currentBlock) {
                    currentBlock = access.block;
                    have = in[currentBlock];
                }
                if (access.load) {
                    if (!have) {
                        ok = false;
                        break;
                    }
                } else {
                    have = 1;
                }
            }
            if (!ok) {
                continue;
            }

            std::unordered_set<uint32_t> defBlocks = slot.storeBlocks;
            if (slot.param) {
                defBlocks.insert(entry);
            }
            std::unordered_set<uint32_t> phiBlocks;
            std::vector<uint32_t> work(defBlocks.begin(), defBlocks.end());
            while (!work.empty()) {
                uint32_t block = work.back();
                work.pop_back();
                auto found = frontier.find(block);
                if (found == frontier.end()) {
                    continue;
                }
                for (uint32_t next : found->second) {
                    if (phiBlocks.insert(next).second) {
                        work.push_back(next);
                    }
                }
            }
            std::vector<uint32_t> phis(phiBlocks.begin(), phiBlocks.end());
            std::sort(phis.begin(), phis.end(), [&](uint32_t a, uint32_t b) {
                return blockAt[a] < blockAt[b];
            });
            bool phiOk = true;
            for (uint32_t label : phis) {
                auto at = blockAt.find(label);
                if (at == blockAt.end() || label == entry ||
                    !in[at->second]) {
                    phiOk = false;
                    break;
                }
            }
            if (!phiOk) {
                continue;
            }
            selected.push_back(id);
            phiBlocksBySlot[id] = std::move(phis);
        }
        if (selected.empty()) {
            continue;
        }

        // Create phi records. Result ids are fresh and deterministic: slots
        // in ascending id order, phis in block order.
        uint64_t nextId = static_cast<uint64_t>(maxValueId(fn)) + 1;
        std::vector<PhiRecord> phis;
        std::vector<std::vector<uint64_t>> phisByBlock(blockCount);
        bool idSpaceExhausted = false;
        for (uint32_t id : selected) {
            SlotInfo& slot = slots[id];
            for (uint32_t label : phiBlocksBySlot[id]) {
                if (nextId > kLastFreshId) {
                    idSpaceExhausted = true;
                    break;
                }
                PhiRecord record;
                record.slot = id;
                record.block = label;
                record.result.id = static_cast<uint32_t>(nextId++);
                record.result.width = slot.width;
                record.result.type = slot.type;
                for (uint64_t pred : reachablePreds(label)) {
                    record.preds.push_back(fn.blocks[pred].label);
                }
                uint64_t index = phis.size();
                phis.push_back(std::move(record));
                phisByBlock[blockAt[label]].push_back(index);
            }
            if (idSpaceExhausted) {
                break;
            }
        }
        if (idSpaceExhausted) {
            // Leave the module untouched rather than promote half a function.
            continue;
        }

        // Per-block access lists, already in scan order; sorting by
        // instruction index recovers the execution order across slots.
        std::vector<std::vector<BlockAccess>> accesses(blockCount);
        std::unordered_set<uint32_t> promoted(selected.begin(), selected.end());
        for (uint32_t id : selected) {
            for (const Access& access : slots[id].accesses) {
                BlockAccess ref;
                ref.slot = id;
                ref.index = access.index;
                ref.load = access.load;
                accesses[access.block].push_back(ref);
            }
        }
        for (auto& list : accesses) {
            std::sort(list.begin(), list.end(),
                      [](const BlockAccess& a, const BlockAccess& b) {
                          if (a.index != b.index) {
                              return a.index < b.index;
                          }
                          return a.slot < b.slot;
                      });
        }

        std::vector<std::vector<uint8_t>> drop(blockCount);
        for (uint64_t bi = 0; bi < blockCount; ++bi) {
            const uint64_t slots = fn.blocks[bi].body.size();
            drop[bi].assign(slots, 0);
        }
        std::unordered_map<uint32_t, ir::Value> subst;
        std::unordered_map<uint32_t, std::vector<ir::Value>> stacks;
        for (uint32_t id : selected) {
            if (slots[id].param) {
                ir::Value seed;
                seed.id = id;
                seed.width = slots[id].width;
                seed.type = slots[id].type;
                stacks[id].push_back(seed);
            }
        }
        bool failed = false;
        uint64_t loadsReplaced = 0;
        uint64_t storesRemoved = 0;

        std::function<void(uint32_t)> rename = [&](uint32_t label) {
            if (failed) {
                return;
            }
            auto at = blockAt.find(label);
            if (at == blockAt.end()) {
                failed = true;
                return;
            }
            const uint64_t bi = at->second;
            std::unordered_map<uint32_t, uint64_t> base;
            auto push = [&](uint32_t id, ir::Value value) {
                std::vector<ir::Value>& stack = stacks[id];
                if (base.find(id) == base.end()) {
                    base[id] = stack.size();
                }
                stack.push_back(value);
            };
            for (uint64_t index : phisByBlock[bi]) {
                PhiRecord& record = phis[index];
                push(record.slot, record.result);
            }
            for (const BlockAccess& access : accesses[bi]) {
                std::vector<ir::Value>& stack = stacks[access.slot];
                if (access.load) {
                    if (stack.empty()) {
                        failed = true;
                        return;
                    }
                    const ir::Instr& instr = fn.blocks[bi].body[access.index];
                    if (instr.results.size() != kOneResult) {
                        failed = true;
                        return;
                    }
                    subst[instr.results[0].id] = stack.back();
                    drop[bi][access.index] = 1;
                    ++loadsReplaced;
                } else {
                    const ir::Instr& instr = fn.blocks[bi].body[access.index];
                    ir::Value value = instr.storedValue;
                    auto found = subst.find(value.id);
                    if (found != subst.end()) {
                        value = found->second;
                    }
                    push(access.slot, value);
                    drop[bi][access.index] = 1;
                    ++storesRemoved;
                }
            }
            auto succs = dominators.successors.find(label);
            if (succs != dominators.successors.end()) {
                for (uint32_t succ : succs->second) {
                    auto succAt = blockAt.find(succ);
                    if (succAt == blockAt.end() ||
                        !reachable[succAt->second]) {
                        continue;
                    }
                    for (uint64_t index : phisByBlock[succAt->second]) {
                        PhiRecord& record = phis[index];
                        std::vector<ir::Value>& stack = stacks[record.slot];
                        if (stack.empty()) {
                            failed = true;
                            return;
                        }
                        record.incoming[label] = stack.back();
                    }
                }
            }
            auto children = dominators.children.find(label);
            if (children != dominators.children.end()) {
                for (uint32_t child : children->second) {
                    rename(child);
                    if (failed) {
                        return;
                    }
                }
            }
            for (const auto& entry : base) {
                stacks[entry.first].resize(entry.second);
            }
        };
        rename(fn.blocks.front().label);
        if (failed) {
            continue;
        }

        // Every phi must have a value for every reachable predecessor.
        std::unordered_map<uint32_t, std::vector<ir::Instr>> phiInstrs;
        for (PhiRecord& record : phis) {
            ir::Instr instr;
            instr.op = ir::Op::Phi;
            instr.width = record.result.width;
            instr.type = record.result.type;
            instr.isPure = true;
            instr.results.push_back(record.result);
            for (uint32_t pred : record.preds) {
                auto found = record.incoming.find(pred);
                if (found == record.incoming.end()) {
                    failed = true;
                    break;
                }
                ir::PhiIncoming incoming;
                incoming.value = found->second;
                incoming.block = pred;
                instr.phiIncoming.push_back(incoming);
            }
            if (failed) {
                break;
            }
            phiInstrs[record.block].push_back(std::move(instr));
        }
        if (failed) {
            continue;
        }

        replaceUses(fn, subst);

        // Remove promoted loads, stores and the Alloca instructions.
        for (uint64_t bi = 0; bi < blockCount; ++bi) {
            std::vector<ir::Instr> kept;
            for (uint64_t ii = 0; ii < fn.blocks[bi].body.size(); ++ii) {
                const ir::Instr& instr = fn.blocks[bi].body[ii];
                if (drop[bi][ii]) {
                    continue;
                }
                if (instr.op == ir::Op::Alloca && !instr.results.empty() &&
                    promoted.find(instr.results[0].id) != promoted.end()) {
                    continue;
                }
                kept.push_back(std::move(fn.blocks[bi].body[ii]));
            }
            fn.blocks[bi].body = std::move(kept);
        }

        // A promoted parameter now names the value itself, not its old
        // pointer-shaped slot. Codegen's prologue and the VM's argument bind
        // still write the id's cell, so only the metadata changes.
        for (uint32_t id : selected) {
            if (!slots[id].param) {
                continue;
            }
            for (ir::Value& param : fn.params) {
                if (param.id == id) {
                    param.width = slots[id].width;
                    param.type = slots[id].type;
                }
            }
        }

        // Phis lead their blocks.
        for (const auto& entry : phiInstrs) {
            auto at = blockAt.find(entry.first);
            if (at == blockAt.end()) {
                continue;
            }
            std::vector<ir::Instr> body;
            body.reserve(entry.second.size() + fn.blocks[at->second].body.size());
            for (const ir::Instr& phi : entry.second) {
                body.push_back(phi);
            }
            for (ir::Instr& instr : fn.blocks[at->second].body) {
                body.push_back(std::move(instr));
            }
            fn.blocks[at->second].body = std::move(body);
        }

        result.promotedSlots += selected.size();
        result.phisInserted += phis.size();
        result.loadsReplaced += loadsReplaced;
        result.storesRemoved += storesRemoved;
    }
    return result;
}

}  // namespace opt
}  // namespace cider
