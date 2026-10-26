// opt.h — the pass pipeline. IR.md §7.
//
// A pass may change the IR's shape and not its meaning. Each pass below is one
// row of that section made executable, and the flags in IR.md §3.1 exist so
// the first four rows are checkable by reading one field rather than by
// analysing a call graph:
//
//   - no pass deletes an instruction with mayTrap (that is deleting a check)
//   - no pass deletes, merges or moves one marked volatile
//   - no pass moves or duplicates an instruction across a mayTrap it depends on
//   - no pass changes an instruction's overflow
//
// run() checks the module structurally first, so no pass runs on a malformed
// module — gate L4.

#ifndef CIDER_OPT_OPT_H
#define CIDER_OPT_OPT_H

#include <cstdint>
#include <string>

#include "ir/ir.h"

namespace cider {
namespace opt {

struct OptResult {
    bool ok = false;
    std::string error;
    uint64_t folded = 0;
    uint64_t removed = 0;
    // Counters for the slot-forwarding slice. A forwarded load is a Load
    // replaced by an already-stored value; a removed store is one fully
    // overwritten before any read of its slot.
    uint64_t forwarded = 0;
    uint64_t storesRemoved = 0;
    // Mem2reg counters.
    uint64_t promoted = 0;
    uint64_t phis = 0;
    uint64_t loadsPromoted = 0;
    uint64_t storesPromoted = 0;
};

// What one slot-forwarding run did. Kept separate from OptResult so the
// pass is testable without the rest of the pipeline.
struct MemoryResult {
    uint64_t forwarded = 0;
    uint64_t storesRemoved = 0;
};

// What one mem2reg run did.
struct PromotionResult {
    uint64_t promotedSlots = 0;
    uint64_t phisInserted = 0;
    uint64_t loadsReplaced = 0;
    uint64_t storesRemoved = 0;
};

// What phi lowering did for codegen.
struct PhiLowerResult {
    bool ok = false;
    std::string error;
    uint64_t splitBlocks = 0;
    uint64_t phisLowered = 0;
    uint64_t copies = 0;
};

// Structural check, then mem2reg, then fold, then slot forwarding, then
// dead-code elimination. Mem2reg first exposes the SSA values that folding
// can use; forwarding still covers every slot mem2reg left in memory; the
// check first means none of the passes reads garbage.
OptResult run(ir::Module& module);

// Promote non-escaping single-word scalar Alloca slots to SSA values with
// phi nodes at dominance-frontier joins. This is the full Cytron algorithm:
// definition blocks, iterated dominance frontier, and a dominator-tree
// rename walk with one value stack per slot. Loads and stores to promoted
// slots disappear with the Alloca. Anything less certain — escapes,
// aggregates, volatile/partial-width accesses, asm functions, uninitialized
// loads — stays in memory for forwardMemory.
PromotionResult mem2reg(ir::Module& module);

// Replace phi nodes with parallel copies on the incoming edges, splitting
// critical edges first. Codegen is stack-slot based and consumes ordinary
// values, so the driver runs this after the optimizer and before generation;
// `cider ir` and the VM keep the phi IR. Returns an error rather than
// silently dropping a phi if an edge cannot be made single-entry.
PhiLowerResult lowerPhis(ir::Module& module);

// Constant folding. Folds only what is provably the same: no overflow, no
// division by zero, no out-of-range shift. Anything else stays for runtime,
// where the trap semantics live.
uint64_t foldConsts(ir::Module& module);

// Conservative load/store forwarding for non-escaping Alloca slots. After
// mem2reg this is the fallback for slots that stay in memory: no phi nodes and
// no SSA construction. It forwards a stored value to later loads of the same
// slot, width and type, and deletes a store fully overwritten before any read.
// A Gep or unexpected use of any slot disables the pass for the whole function:
// Alloca cells are adjacent, so one escaped address can name a neighbour by
// displacement. Volatile accesses and calls are barriers, and a function with
// asm is left alone. When in doubt, it does nothing.
MemoryResult forwardMemory(ir::Module& module);

// Dead-code elimination. Deletes a definition only when every result is
// unused, the instruction is pure, it cannot trap, and it is not volatile.
// Repeats to a fixpoint: deleting one definition can orphan another.
uint64_t deadCode(ir::Module& module);

}  // namespace opt
}  // namespace cider

#endif  // CIDER_OPT_OPT_H
