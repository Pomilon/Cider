// liveness.cpp — whole-function may-liveness and live ranges.
//
// A standard live-in/live-out fixpoint over the CFG successors, iterated in
// reverse postorder so a loop settles in one or two passes. Use and def sets
// include every operand form the emitter already reads; the two helpers that
// enumerate them are duplicated verbatim from regalloc.cpp (see the comment on
// appendInstrUses) and Task 9 may consolidate them. Keeping this file a pure
// analysis is what lets it land without changing codegen output.

#include "codegen/liveness.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cider {
namespace codegen {

namespace {

constexpr uint32_t kInvalidValue = 0xFFFFFFFFu;

bool isFloatWidth(ir::IRType width) {
    return width == ir::IRType::F32 || width == ir::IRType::F64;
}

// The barrier set is regalloc.cpp's: a call, inline asm, trap, va_list
// access, or a volatile load/store ends a caller-saved live range.
bool isBarrier(const ir::Instr& instr) {
    if (instr.op == ir::Op::Call || instr.op == ir::Op::Asm ||
        instr.op == ir::Op::Trap || instr.op == ir::Op::VaStart ||
        instr.op == ir::Op::VaArg) {
        return true;
    }
    return (instr.op == ir::Op::Load || instr.op == ir::Op::Store) &&
           instr.isVolatile;
}

// Every value an instruction reads. Duplicated verbatim from regalloc.cpp on
// purpose: the emitter, the allocator and this analysis must agree on what a
// use is, and a shared header would have widened a task that must not change
// codegen. If one copy changes, the other must change in the same commit.
void appendInstrUses(const ir::Instr& instr, std::vector<uint32_t>& out) {
    auto add = [&](const ir::Value& value) {
        if (value.id != kInvalidValue) {
            out.push_back(value.id);
        }
    };
    for (const ir::Value& operand : instr.operands) {
        add(operand);
    }
    for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
        add(incoming.value);
    }
    if (instr.op == ir::Op::Load || instr.op == ir::Op::Store ||
        instr.op == ir::Op::Extract || instr.op == ir::Op::Insert ||
        instr.op == ir::Op::Gep) {
        add(instr.addressBase);
    }
    if (instr.op == ir::Op::Store || instr.op == ir::Op::Insert) {
        add(instr.storedValue);
    }
    if (instr.op == ir::Op::Call) {
        for (const ir::Argument& arg : instr.arguments) {
            for (const ir::Value& word : arg.words) {
                add(word);
            }
        }
        if (instr.call.indirect) {
            add(instr.calleeValue);
        }
    }
}

void appendTermUses(const ir::Terminator& term, std::vector<uint32_t>& out) {
    auto add = [&](const ir::Value& value) {
        if (value.id != kInvalidValue) {
            out.push_back(value.id);
        }
    };
    for (const ir::Value& value : term.values) {
        add(value);
    }
    if (term.cond.has_value()) {
        add(*term.cond);
    }
    if (term.term == ir::Term::Switch) {
        add(term.switchValue);
        for (const ir::Case& arm : term.cases) {
            add(arm.value);
        }
    }
}

// Reverse postorder from the entry, whose label is the first block. DFS
// successors in terminator order, so the order is a function of the IR
// alone. Unreachable blocks are appended in index order: they have no RPO
// position, but they still need one for the position mapping and the
// deterministic tail keeps every def/use representable.
std::vector<uint32_t> reversePostorder(
    const std::vector<std::vector<uint32_t>>& successors) {
    const uint32_t count = static_cast<uint32_t>(successors.size());
    std::vector<uint32_t> order;
    if (count == 0) {
        return order;
    }
    order.reserve(count);
    std::vector<uint8_t> seen(count, 0);
    std::vector<std::pair<uint32_t, size_t>> stack;
    std::vector<uint32_t> post;
    post.reserve(count);
    seen[0] = 1;
    stack.emplace_back(0, 0);
    while (!stack.empty()) {
        auto& top = stack.back();
        if (top.second < successors[top.first].size()) {
            const uint32_t child = successors[top.first][top.second++];
            if (seen[child] == 0) {
                seen[child] = 1;
                stack.emplace_back(child, 0);
            }
        } else {
            post.push_back(top.first);
            stack.pop_back();
        }
    }
    order.assign(post.rbegin(), post.rend());
    for (uint32_t block = 0; block < count; ++block) {
        if (seen[block] == 0) {
            order.push_back(block);
        }
    }
    return order;
}

}  // namespace

Liveness analyzeLiveness(const ir::Function& fn) {
    Liveness live;
    const size_t blockCount = fn.blocks.size();
    live.liveIn.resize(blockCount);
    live.liveOut.resize(blockCount);
    if (blockCount == 0) {
        return live;
    }

    std::unordered_map<uint32_t, uint32_t> blockAt;
    for (uint32_t block = 0; block < blockCount; ++block) {
        blockAt[fn.blocks[block].label] = block;
    }

    std::vector<std::vector<uint32_t>> successors(blockCount);
    auto addTarget = [&](std::vector<uint32_t>& out, uint32_t label) {
        auto found = blockAt.find(label);
        if (found == blockAt.end()) {
            return;
        }
        const uint32_t target = found->second;
        if (std::find(out.begin(), out.end(), target) == out.end()) {
            out.push_back(target);
        }
    };
    for (uint32_t block = 0; block < blockCount; ++block) {
        const ir::Terminator& term = fn.blocks[block].term;
        switch (term.term) {
            case ir::Term::Br:
                addTarget(successors[block], term.target);
                break;
            case ir::Term::CondBr:
                addTarget(successors[block], term.thenTarget);
                addTarget(successors[block], term.elseTarget);
                break;
            case ir::Term::Switch:
                for (const ir::Case& arm : term.cases) {
                    addTarget(successors[block], arm.target);
                }
                addTarget(successors[block], term.defaultTarget);
                break;
            case ir::Term::Ret:
            case ir::Term::Unreachable:
            case ir::Term::Trap:
                break;
        }
    }

    live.blockOrder = reversePostorder(successors);
    std::vector<uint32_t> blockPosition(blockCount, 0);
    for (uint32_t position = 0; position < live.blockOrder.size(); ++position) {
        blockPosition[live.blockOrder[position]] = position;
    }

    // Parameters are SSA definitions at function entry. Seeding the entry
    // block's def set with them keeps a parameter use from becoming a live-in
    // of the entry, which has no predecessor to carry it.
    std::unordered_set<uint32_t> paramIds;
    for (const ir::Value& param : fn.params) {
        if (param.id != kInvalidValue) {
            paramIds.insert(param.id);
        }
    }

    std::vector<std::unordered_set<uint32_t>> blockDefs(blockCount);
    std::vector<std::unordered_set<uint32_t>> upUses(blockCount);
    std::vector<uint32_t> scratch;
    for (uint32_t block = 0; block < blockCount; ++block) {
        std::unordered_set<uint32_t>& defined = blockDefs[block];
        if (block == 0) {
            defined = paramIds;
        }
        const ir::Block& body = fn.blocks[block];
        for (const ir::Instr& instr : body.body) {
            scratch.clear();
            appendInstrUses(instr, scratch);
            for (uint32_t id : scratch) {
                if (defined.find(id) == defined.end()) {
                    upUses[block].insert(id);
                }
            }
            for (const ir::Value& result : instr.results) {
                if (result.id != kInvalidValue) {
                    defined.insert(result.id);
                }
            }
        }
        scratch.clear();
        appendTermUses(body.term, scratch);
        for (uint32_t id : scratch) {
            if (defined.find(id) == defined.end()) {
                upUses[block].insert(id);
            }
        }
    }

    std::vector<std::unordered_set<uint32_t>> in(blockCount);
    std::vector<std::unordered_set<uint32_t>> out(blockCount);
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto at = live.blockOrder.rbegin(); at != live.blockOrder.rend();
             ++at) {
            const uint32_t block = *at;
            std::unordered_set<uint32_t> nextOut;
            for (uint32_t successor : successors[block]) {
                nextOut.insert(in[successor].begin(), in[successor].end());
            }
            std::unordered_set<uint32_t> nextIn = upUses[block];
            for (uint32_t id : nextOut) {
                if (blockDefs[block].find(id) == blockDefs[block].end()) {
                    nextIn.insert(id);
                }
            }
            if (nextOut != out[block] || nextIn != in[block]) {
                out[block] = std::move(nextOut);
                in[block] = std::move(nextIn);
                changed = true;
            }
        }
    }

    for (uint32_t block = 0; block < blockCount; ++block) {
        live.liveIn[block].assign(in[block].begin(), in[block].end());
        live.liveOut[block].assign(out[block].begin(), out[block].end());
        std::sort(live.liveIn[block].begin(), live.liveIn[block].end());
        std::sort(live.liveOut[block].begin(), live.liveOut[block].end());
    }

    // A barrier is crossed only by a value that is still needed after it. The
    // backward walk therefore checks the live set before adding the barrier's
    // own operands, and removes its result first: a call result begins after
    // its call, while a call argument used nowhere else ends at the call.
    std::unordered_set<uint32_t> crossing;
    for (uint32_t block = 0; block < blockCount; ++block) {
        const ir::Block& body = fn.blocks[block];
        std::unordered_set<uint32_t> current = out[block];
        scratch.clear();
        appendTermUses(body.term, scratch);
        for (uint32_t id : scratch) {
            current.insert(id);
        }
        size_t index = body.body.size();
        while (index > 0) {
            --index;
            const ir::Instr& instr = body.body[index];
            // Instructions after a trap never execute; nothing before it is
            // live across it.
            if (instr.op == ir::Op::Trap) {
                current.clear();
                continue;
            }
            if (isBarrier(instr)) {
                std::unordered_set<uint32_t> definedHere;
                for (const ir::Value& result : instr.results) {
                    if (result.id != kInvalidValue) {
                        definedHere.insert(result.id);
                    }
                }
                for (uint32_t id : current) {
                    if (definedHere.find(id) == definedHere.end()) {
                        crossing.insert(id);
                    }
                }
            }
            for (const ir::Value& result : instr.results) {
                current.erase(result.id);
            }
            scratch.clear();
            appendInstrUses(instr, scratch);
            for (uint32_t id : scratch) {
                current.insert(id);
            }
        }
    }

    std::unordered_map<uint32_t, std::vector<uint32_t>> defsAt;
    std::unordered_map<uint32_t, std::vector<uint32_t>> usesAt;
    std::unordered_map<uint32_t, ir::IRType> widthAt;
    for (const ir::Value& param : fn.params) {
        if (param.id != kInvalidValue) {
            defsAt[param.id].push_back(0);
            widthAt.emplace(param.id, param.width);
        }
    }
    for (uint32_t block = 0; block < blockCount; ++block) {
        const uint32_t base = blockPosition[block] * kBlockPositionScale;
        const ir::Block& body = fn.blocks[block];
        for (uint32_t index = 0; index < body.body.size(); ++index) {
            const ir::Instr& instr = body.body[index];
            const uint32_t position = base + index;
            for (const ir::Value& result : instr.results) {
                if (result.id != kInvalidValue) {
                    defsAt[result.id].push_back(position);
                    widthAt.emplace(result.id, result.width);
                }
            }
            scratch.clear();
            appendInstrUses(instr, scratch);
            for (uint32_t id : scratch) {
                usesAt[id].push_back(position);
            }
        }
        scratch.clear();
        appendTermUses(body.term, scratch);
        const uint32_t terminator = base + static_cast<uint32_t>(body.body.size());
        for (uint32_t id : scratch) {
            usesAt[id].push_back(terminator);
        }
    }

    std::vector<uint32_t> values;
    values.reserve(defsAt.size());
    for (const auto& entry : defsAt) {
        values.push_back(entry.first);
    }
    std::sort(values.begin(), values.end());
    live.ranges.reserve(values.size());
    for (uint32_t id : values) {
        const std::vector<uint32_t>& definitions = defsAt[id];
        uint32_t start = definitions.front();
        for (uint32_t position : definitions) {
            start = std::min(start, position);
        }
        // A definition with no use keeps a one-point range so the range list
        // still covers every defined value; the allocator can ignore it.
        uint32_t end = start;
        uint32_t uses = 0;
        auto found = usesAt.find(id);
        if (found != usesAt.end()) {
            uses = static_cast<uint32_t>(found->second.size());
            for (uint32_t position : found->second) {
                end = std::max(end, position);
            }
        }
        LiveRange range;
        range.value = id;
        range.start = start;
        range.end = end;
        range.crossesCall = crossing.find(id) != crossing.end();
        auto width = widthAt.find(id);
        range.isFloat = width != widthAt.end() && isFloatWidth(width->second);
        range.uses = uses;
        live.ranges.push_back(range);
    }
    return live;
}

}  // namespace codegen
}  // namespace cider
