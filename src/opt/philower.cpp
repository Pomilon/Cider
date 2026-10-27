// philower.cpp — turn phi instructions into parallel edge copies for codegen.
//
// The optimized IR keeps phis so later passes can still see the SSA shape.
// Codegen consumes ordinary values and refuses a phi, so before it runs this
// pass removes every phi and emits ordinary Copy instructions on the incoming
// edge. Codegen's local register allocator works over this lowered form, not
// over the phi IR:
//
//   - a block with one reachable predecessor gets its copies at block entry;
//   - every other predecessor gets its copies at the end of that predecessor,
//     after first splitting each critical edge it owns.
//
// Critical-edge splitting inserts a small block between a multi-successor
// predecessor and a multi-predecessor phi block. That gives every copy a
// single execution point. Copies in one edge are parallel: all incoming
// values are read before any destination is written. A source that is also a
// destination is first copied to a fresh temporary, so swaps and loop-carried
// phis do not clobber each other.
//
// This is the SSA-to-stack boundary: one phi result is intentionally written
// on every incoming edge, so the output is no longer single-definition SSA.
// The structural checker validates the phi IR before this pass runs, and no
// pass or checker consumes the lowered form except codegen.

#include "opt/opt.h"

#include <algorithm>
#include <cstdint>
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
constexpr uint64_t kOne = 1;
constexpr uint64_t kOneResult = 1;

bool namesBase(ir::Op op) {
    return op == ir::Op::Load || op == ir::Op::Store ||
           op == ir::Op::Extract || op == ir::Op::Insert ||
           op == ir::Op::Gep;
}

struct CopyPair {
    ir::Value dest;
    ir::Value source;
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

void retarget(ir::Terminator& term, uint32_t from, uint32_t to) {
    switch (term.term) {
        case ir::Term::Br:
            if (term.target == from) {
                term.target = to;
            }
            break;
        case ir::Term::CondBr:
            if (term.thenTarget == from) {
                term.thenTarget = to;
            }
            if (term.elseTarget == from) {
                term.elseTarget = to;
            }
            break;
        case ir::Term::Switch:
            for (ir::Case& arm : term.cases) {
                if (arm.target == from) {
                    arm.target = to;
                }
            }
            if (term.defaultTarget == from) {
                term.defaultTarget = to;
            }
            break;
        case ir::Term::Ret:
        case ir::Term::Unreachable:
        case ir::Term::Trap:
            break;
    }
}

std::vector<uint32_t> reachablePreds(const ir::Function& fn,
                                     const DominatorInfo& dom,
                                     uint32_t label) {
    std::unordered_map<uint32_t, uint32_t> at;
    for (uint32_t i = 0; i < fn.blocks.size(); ++i) {
        at[fn.blocks[i].label] = i;
    }
    std::vector<uint32_t> out;
    auto found = dom.predecessors.find(label);
    if (found == dom.predecessors.end()) {
        return out;
    }
    for (uint32_t pred : found->second) {
        if (dom.reachable.find(pred) != dom.reachable.end() &&
            at.find(pred) != at.end()) {
            out.push_back(pred);
        }
    }
    return out;
}

bool hasPhi(const ir::Block& block) {
    for (const ir::Instr& instr : block.body) {
        if (instr.op == ir::Op::Phi) {
            return true;
        }
    }
    return false;
}

}  // namespace

PhiLowerResult lowerPhis(ir::Module& module) {
    PhiLowerResult result;
    result.ok = true;
    for (ir::Function& fn : module.functions) {
        bool anyPhi = false;
        uint64_t maxLabel = 0;
        for (const ir::Block& block : fn.blocks) {
            maxLabel = block.label > maxLabel ? block.label : maxLabel;
            anyPhi = anyPhi || hasPhi(block);
        }
        if (!anyPhi) {
            continue;
        }
        uint64_t nextId = static_cast<uint64_t>(maxValueId(fn)) + 1;

        // Split every critical edge that enters a multi-predecessor phi
        // block. Repeat because a split can expose the next edge on a chain.
        std::unordered_set<uint64_t> splitEdges;
        for (uint64_t guard = 0; guard <= fn.blocks.size(); ++guard) {
            const DominatorInfo dom = computeDominators(fn);
            struct Split {
                uint32_t pred = 0;
                uint32_t target = 0;
                uint32_t label = 0;
            };
            std::vector<Split> splits;
            for (const ir::Block& block : fn.blocks) {
                if (!hasPhi(block) ||
                    dom.reachable.find(block.label) ==
                        dom.reachable.end()) {
                    continue;
                }
                std::vector<uint32_t> preds =
                    reachablePreds(fn, dom, block.label);
                if (preds.size() <= kOne) {
                    continue;
                }
                for (uint32_t pred : preds) {
                    auto found = dom.successors.find(pred);
                    uint64_t succs =
                        found == dom.successors.end() ? 0 : found->second.size();
                    if (succs <= 1) {
                        continue;
                    }
                    uint64_t key = (static_cast<uint64_t>(pred) << 32) |
                                   block.label;
                    if (!splitEdges.insert(key).second) {
                        continue;
                    }
                    Split split;
                    split.pred = pred;
                    split.target = block.label;
                    split.label = static_cast<uint32_t>(++maxLabel);
                    splits.push_back(split);
                }
            }
            if (splits.empty()) {
                break;
            }
            std::unordered_map<uint32_t, uint32_t> at;
            for (uint32_t i = 0; i < fn.blocks.size(); ++i) {
                at[fn.blocks[i].label] = i;
            }
            for (const Split& split : splits) {
                auto predAt = at.find(split.pred);
                auto targetAt = at.find(split.target);
                if (predAt == at.end() || targetAt == at.end()) {
                    result.ok = false;
                    result.error = "a critical-edge split names a missing block";
                    return result;
                }
                retarget(fn.blocks[predAt->second].term, split.target,
                         split.label);
                ir::Block& target = fn.blocks[targetAt->second];
                for (ir::Instr& instr : target.body) {
                    if (instr.op != ir::Op::Phi) {
                        continue;
                    }
                    for (ir::PhiIncoming& incoming : instr.phiIncoming) {
                        if (incoming.block == split.pred) {
                            incoming.block = split.label;
                        }
                    }
                }
                ir::Block edge;
                edge.label = split.label;
                edge.loc = target.loc;
                ir::Terminator br;
                br.term = ir::Term::Br;
                br.target = split.target;
                br.loc = target.loc;
                edge.term = br;
                fn.blocks.push_back(std::move(edge));
                ++result.splitBlocks;
            }
        }

        const DominatorInfo dom = computeDominators(fn);
        std::unordered_map<uint32_t, std::vector<CopyPair>> startPairs;
        std::unordered_map<uint32_t, std::vector<CopyPair>> endPairs;
        for (const ir::Block& block : fn.blocks) {
            if (!hasPhi(block) ||
                dom.reachable.find(block.label) == dom.reachable.end()) {
                continue;
            }
            std::vector<uint32_t> preds =
                reachablePreds(fn, dom, block.label);
            if (preds.empty()) {
                result.ok = false;
                result.error = "a reachable phi block has no predecessor";
                return result;
            }
            if (preds.size() == kOne) {
                for (const ir::Instr& instr : block.body) {
                    if (instr.op != ir::Op::Phi || instr.results.size() != kOneResult) {
                        continue;
                    }
                    for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                        if (incoming.block != preds[0]) {
                            continue;
                        }
                        CopyPair pair;
                        pair.dest = instr.results[0];
                        pair.source = incoming.value;
                        startPairs[block.label].push_back(pair);
                    }
                }
                continue;
            }
            for (const ir::Instr& instr : block.body) {
                if (instr.op != ir::Op::Phi || instr.results.size() != kOneResult) {
                    continue;
                }
                for (const ir::PhiIncoming& incoming : instr.phiIncoming) {
                    auto succs = dom.successors.find(incoming.block);
                    if (succs == dom.successors.end() ||
                        succs->second.size() != kOne) {
                        result.ok = false;
                        result.error =
                            "a critical edge survived phi lowering";
                        return result;
                    }
                    CopyPair pair;
                    pair.dest = instr.results[0];
                    pair.source = incoming.value;
                    endPairs[incoming.block].push_back(pair);
                }
            }
        }

        // Build the copies per edge with the parallel-copy rule. Start and
        // end groups are kept apart: one block can be both a phi block with
        // entry copies and a predecessor with exit copies.
        auto buildCopies = [&](std::unordered_map<uint32_t,
                                                   std::vector<CopyPair>>& groups,
                               std::unordered_map<uint32_t,
                                                  std::vector<ir::Instr>>& out) {
            std::vector<uint32_t> keys;
            for (const ir::Block& block : fn.blocks) {
                if (groups.find(block.label) != groups.end()) {
                    keys.push_back(block.label);
                }
            }
            for (uint32_t key : keys) {
                std::vector<CopyPair> pairs = std::move(groups[key]);
                std::unordered_set<uint32_t> dests;
                for (const CopyPair& pair : pairs) {
                    dests.insert(pair.dest.id);
                }
                std::unordered_map<uint32_t, ir::Value> temps;
                for (const CopyPair& pair : pairs) {
                    if (pair.source.id == pair.dest.id) {
                        continue;
                    }
                    if (dests.find(pair.source.id) == dests.end() ||
                        temps.find(pair.source.id) != temps.end()) {
                        continue;
                    }
                    if (nextId > kLastFreshId) {
                        result.ok = false;
                        result.error =
                            "the phi-lowering value-id space is exhausted";
                        return false;
                    }
                    ir::Value temp;
                    temp.id = static_cast<uint32_t>(nextId++);
                    temp.width = pair.source.width;
                    temp.type = pair.source.type;
                    temps[pair.source.id] = temp;
                    ir::Instr copy;
                    copy.op = ir::Op::Copy;
                    copy.width = temp.width;
                    copy.type = temp.type;
                    copy.isPure = true;
                    copy.loc = fn.blocks.front().loc;
                    copy.results.push_back(temp);
                    copy.operands.push_back(pair.source);
                    out[key].push_back(std::move(copy));
                    ++result.copies;
                }
                for (const CopyPair& pair : pairs) {
                    if (pair.source.id == pair.dest.id) {
                        continue;
                    }
                    ir::Instr copy;
                    copy.op = ir::Op::Copy;
                    copy.width = pair.dest.width;
                    copy.type = pair.dest.type;
                    copy.isPure = true;
                    copy.loc = fn.blocks.front().loc;
                    copy.results.push_back(pair.dest);
                    auto temp = temps.find(pair.source.id);
                    copy.operands.push_back(
                        temp == temps.end() ? pair.source : temp->second);
                    out[key].push_back(std::move(copy));
                    ++result.copies;
                }
            }
            return true;
        };
        std::unordered_map<uint32_t, std::vector<ir::Instr>> startInstrs;
        std::unordered_map<uint32_t, std::vector<ir::Instr>> endInstrs;
        if (!buildCopies(startPairs, startInstrs) ||
            !buildCopies(endPairs, endInstrs)) {
            return result;
        }

        // Remove phis and place the copies.
        for (ir::Block& block : fn.blocks) {
            std::vector<ir::Instr> kept;
            for (ir::Instr& instr : block.body) {
                if (instr.op != ir::Op::Phi) {
                    kept.push_back(std::move(instr));
                } else {
                    ++result.phisLowered;
                }
            }
            block.body = std::move(kept);
        }
        std::unordered_map<uint32_t, uint32_t> at;
        for (uint32_t i = 0; i < fn.blocks.size(); ++i) {
            at[fn.blocks[i].label] = i;
        }
        for (const auto& entry : startInstrs) {
            auto found = at.find(entry.first);
            if (found == at.end()) {
                result.ok = false;
                result.error = "phi entry copies name a missing block";
                return result;
            }
            ir::Block& block = fn.blocks[found->second];
            std::vector<ir::Instr> body;
            body.reserve(entry.second.size() + block.body.size());
            for (const ir::Instr& copy : entry.second) {
                body.push_back(copy);
            }
            for (ir::Instr& instr : block.body) {
                body.push_back(std::move(instr));
            }
            block.body = std::move(body);
        }
        for (const auto& entry : endInstrs) {
            auto found = at.find(entry.first);
            if (found == at.end()) {
                result.ok = false;
                result.error = "phi exit copies name a missing block";
                return result;
            }
            ir::Block& block = fn.blocks[found->second];
            for (const ir::Instr& copy : entry.second) {
                block.body.push_back(copy);
            }
        }
    }
    return result;
}

}  // namespace opt
}  // namespace cider
