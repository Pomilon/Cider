// dominance.cpp — dominators and dominance frontiers, once for all passes.

#include "opt/dominance.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cider {
namespace opt {

namespace {

std::vector<uint32_t> terminatorTargets(const ir::Terminator& term) {
    std::unordered_set<uint32_t> seen;
    std::vector<uint32_t> out;
    auto add = [&](uint32_t label) {
        if (seen.insert(label).second) {
            out.push_back(label);
        }
    };
    switch (term.term) {
        case ir::Term::Br:
            add(term.target);
            break;
        case ir::Term::CondBr:
            add(term.thenTarget);
            add(term.elseTarget);
            break;
        case ir::Term::Switch:
            for (const ir::Case& arm : term.cases) {
                add(arm.target);
            }
            add(term.defaultTarget);
            break;
        case ir::Term::Ret:
        case ir::Term::Unreachable:
        case ir::Term::Trap:
            break;
    }
    return out;
}

}  // namespace

bool DominatorInfo::dominates(uint32_t dom, uint32_t block) const {
    if (dom == block) {
        return true;
    }
    if (reachable.find(block) == reachable.end()) {
        return false;
    }
    uint32_t current = block;
    for (uint64_t step = 0; step <= idom.size(); ++step) {
        auto found = idom.find(current);
        if (found == idom.end()) {
            return false;
        }
        if (found->second == dom) {
            return true;
        }
        if (found->second == current) {
            return false;
        }
        current = found->second;
    }
    return false;
}

DominatorInfo computeDominators(const ir::Function& fn) {
    DominatorInfo out;
    if (fn.blocks.empty()) {
        return out;
    }
    std::unordered_map<uint32_t, uint64_t> order;
    for (uint64_t i = 0; i < fn.blocks.size(); ++i) {
        order[fn.blocks[i].label] = i;
    }
    for (const ir::Block& block : fn.blocks) {
        std::vector<uint32_t> targets = terminatorTargets(block.term);
        for (uint32_t target : targets) {
            if (order.find(target) == order.end()) {
                continue;
            }
            out.successors[block.label].push_back(target);
            out.predecessors[target].push_back(block.label);
        }
    }
    for (auto& entry : out.successors) {
        std::sort(entry.second.begin(), entry.second.end(),
                  [&](uint32_t a, uint32_t b) { return order[a] < order[b]; });
    }
    for (auto& entry : out.predecessors) {
        std::sort(entry.second.begin(), entry.second.end(),
                  [&](uint32_t a, uint32_t b) { return order[a] < order[b]; });
    }

    // Reverse postorder from the entry. The first block is the entry by the
    // same convention lowering and codegen use.
    const uint32_t entry = fn.blocks.front().label;
    struct Frame {
        uint32_t label = 0;
        uint64_t next = 0;
    };
    std::vector<Frame> stack;
    std::vector<uint32_t> postorder;
    std::unordered_set<uint32_t> seen;
    seen.insert(entry);
    stack.push_back(Frame{entry, 0});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        auto found = out.successors.find(frame.label);
        const std::vector<uint32_t> empty;
        const std::vector<uint32_t>& next =
            found == out.successors.end() ? empty : found->second;
        if (frame.next < next.size()) {
            uint32_t target = next[frame.next];
            ++frame.next;
            if (seen.insert(target).second) {
                stack.push_back(Frame{target, 0});
            }
            continue;
        }
        postorder.push_back(frame.label);
        stack.pop_back();
    }
    out.reachable.insert(seen.begin(), seen.end());
    out.rpo.assign(postorder.rbegin(), postorder.rend());
    for (uint64_t i = 0; i < out.rpo.size(); ++i) {
        out.rpoIndex[out.rpo[i]] = static_cast<uint32_t>(i);
    }

    auto intersect = [&](uint32_t a, uint32_t b) {
        while (a != b) {
            while (out.rpoIndex[a] > out.rpoIndex[b]) {
                a = out.idom[a];
            }
            while (out.rpoIndex[b] > out.rpoIndex[a]) {
                b = out.idom[b];
            }
        }
        return a;
    };

    out.idom[entry] = entry;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint64_t i = 1; i < out.rpo.size(); ++i) {
            const uint32_t block = out.rpo[i];
            uint32_t nextIdom = 0;
            bool have = false;
            auto preds = out.predecessors.find(block);
            if (preds != out.predecessors.end()) {
                for (uint32_t pred : preds->second) {
                    if (out.idom.find(pred) == out.idom.end()) {
                        continue;
                    }
                    if (!have) {
                        nextIdom = pred;
                        have = true;
                    } else {
                        nextIdom = intersect(pred, nextIdom);
                    }
                }
            }
            if (!have) {
                continue;
            }
            auto found = out.idom.find(block);
            if (found == out.idom.end() || found->second != nextIdom) {
                out.idom[block] = nextIdom;
                changed = true;
            }
        }
    }

    for (uint32_t block : out.rpo) {
        if (block == entry) {
            continue;
        }
        auto found = out.idom.find(block);
        if (found != out.idom.end()) {
            out.children[found->second].push_back(block);
        }
    }
    for (auto& entryChildren : out.children) {
        std::sort(entryChildren.second.begin(), entryChildren.second.end(),
                  [&](uint32_t a, uint32_t b) {
                      return out.rpoIndex[a] < out.rpoIndex[b];
                  });
    }
    return out;
}

std::unordered_map<uint32_t, std::vector<uint32_t>> dominanceFrontiers(
    const ir::Function& fn, const DominatorInfo& dominators) {
    (void)fn;
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> sets;
    for (uint32_t block : dominators.rpo) {
        auto idomFound = dominators.idom.find(block);
        if (idomFound == dominators.idom.end()) {
            continue;
        }
        auto preds = dominators.predecessors.find(block);
        if (preds == dominators.predecessors.end()) {
            continue;
        }
        for (uint32_t pred : preds->second) {
            if (dominators.reachable.find(pred) ==
                dominators.reachable.end()) {
                continue;
            }
            uint32_t runner = pred;
            while (runner != idomFound->second &&
                   dominators.idom.find(runner) != dominators.idom.end()) {
                sets[runner].insert(block);
                uint32_t next = dominators.idom.at(runner);
                if (next == runner) {
                    break;
                }
                runner = next;
            }
        }
    }
    std::unordered_map<uint32_t, std::vector<uint32_t>> out;
    for (auto& entry : sets) {
        out[entry.first].assign(entry.second.begin(), entry.second.end());
        std::sort(out[entry.first].begin(), out[entry.first].end(),
                  [&](uint32_t a, uint32_t b) {
                      return dominators.rpoIndex.at(a) < dominators.rpoIndex.at(b);
                  });
    }
    return out;
}

}  // namespace opt
}  // namespace cider
