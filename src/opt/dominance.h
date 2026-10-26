// dominance.h — the CFG facts mem2reg and the phi validator share.
//
// One computation answers the immediate dominator tree, the reverse postorder
// numbering that makes intersection cheap, and each block's dominance
// frontier. Both users need the same answers, so there is one implementation
// rather than a second copy inside mem2reg.

#ifndef CIDER_OPT_DOMINANCE_H
#define CIDER_OPT_DOMINANCE_H

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/ir.h"

namespace cider {
namespace opt {

struct DominatorInfo {
    // Block label -> position in reverse postorder. Reachable blocks only.
    std::unordered_map<uint32_t, uint32_t> rpoIndex;
    // Reverse postorder, entry first.
    std::vector<uint32_t> rpo;
    // Block label -> immediate dominator label; the entry dominates itself.
    std::unordered_map<uint32_t, uint32_t> idom;
    // Immediate-dominator children of each reachable block, ordered by RPO.
    std::unordered_map<uint32_t, std::vector<uint32_t>> children;
    // Distinct predecessor/successor labels, reachable or not.
    std::unordered_map<uint32_t, std::vector<uint32_t>> predecessors;
    std::unordered_map<uint32_t, std::vector<uint32_t>> successors;
    std::unordered_set<uint32_t> reachable;

    // True when every path from the entry to `block` passes through `dom`.
    bool dominates(uint32_t dom, uint32_t block) const;
};

// Build the CFG from terminators and compute dominators by the iterative
// Cooper-Harvey-Kennedy intersection. The first block is the entry, the same
// convention lowering and codegen use. Dangling targets are ignored here; the
// structural checker reports them separately.
DominatorInfo computeDominators(const ir::Function& fn);

// Standard Cytron dominance frontier: the blocks where a definition from one
// path and a definition from another path meet. Each vector is ordered by RPO
// so phi insertion is deterministic.
std::unordered_map<uint32_t, std::vector<uint32_t>> dominanceFrontiers(
    const ir::Function& fn, const DominatorInfo& dominators);

}  // namespace opt
}  // namespace cider

#endif  // CIDER_OPT_DOMINANCE_H
