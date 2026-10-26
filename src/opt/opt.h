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
};

// What one slot-forwarding run did. Kept separate from OptResult so the
// pass is testable without the rest of the pipeline.
struct MemoryResult {
    uint64_t forwarded = 0;
    uint64_t storesRemoved = 0;
};

// Structural check, then fold, then slot forwarding, then dead-code
// elimination. Folding first exposes more dead code; forwarding before DCE
// lets the values it orphans be deleted in the same run; the check first means
// none of the passes reads garbage.
OptResult run(ir::Module& module);

// Constant folding. Folds only what is provably the same: no overflow, no
// division by zero, no out-of-range shift. Anything else stays for runtime,
// where the trap semantics live.
uint64_t foldConsts(ir::Module& module);

// Conservative load/store forwarding for non-escaping Alloca slots. This is
// the local foundation of mem2reg, not mem2reg: no phi nodes and no SSA
// construction. It forwards a stored value to later loads of the same slot,
// width and type, and deletes a store fully overwritten before any read. A Gep
// or unexpected use of any slot disables the pass for the whole function:
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
