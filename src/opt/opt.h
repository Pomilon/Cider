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
    // Inlining counters. `clonedInstructions` counts every instruction the
    // inline added to a caller: cloned body instructions, the fresh parameter
    // Alloca/Store pairs, and return phis. `inliningSkipped` counts calls the
    // pass considered in a reachable caller block and refused: indirect,
    // missing/extern, direct-recursive, asm, over the cap, a shape it cannot
    // map, or a per-caller budget. Calls in unreachable blocks and inside
    // cloned bodies are not scanned, so they are not counted.
    uint64_t inlined = 0;
    uint64_t clonedInstructions = 0;
    uint64_t inliningSkipped = 0;
    // Value-numbering counters. `expressionsRemoved` counts every duplicate
    // pure instruction deleted, including collapsed Copy moves;
    // `copiesCollapsed` names the Copy subset. `usesRedirected` counts every
    // operand, call word, phi incoming value, address base, stored value,
    // indirect callee and terminator value rewritten to a dominating leader.
    uint64_t expressionsRemoved = 0;
    uint64_t usesRedirected = 0;
    uint64_t copiesCollapsed = 0;
    // Loop-invariant code motion counters. `licmLoops` counts natural loops
    // detected, `licmHoisted` counts instruction moves (a nested invariant
    // moved out of two loops counts twice), and `licmSkipped` counts loops
    // left alone because they have no existing preheader.
    uint64_t licmLoops = 0;
    uint64_t licmHoisted = 0;
    uint64_t licmSkipped = 0;
};

// What one inlining run did. Kept separate from OptResult so the pass is
// testable without the rest of the pipeline.
struct InlineResult {
    uint64_t inlined = 0;
    uint64_t clonedInstructions = 0;
    uint64_t skipped = 0;
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

// What one value-numbering run did. Kept separate from OptResult so the pass
// is testable without the rest of the pipeline.
struct GvnResult {
    uint64_t expressionsRemoved = 0;
    uint64_t usesRedirected = 0;
    uint64_t copiesCollapsed = 0;
};

// What one loop-invariant code motion run did. `loops` counts natural loops
// detected (one per header), `hoisted` counts instruction moves, and `skipped`
// counts loops with no existing preheader.
struct LicmResult {
    uint64_t loops = 0;
    uint64_t hoisted = 0;
    uint64_t skipped = 0;
};

// Whole-program function pruning. This is deliberately separate from the
// instruction-level `deadCode` pass: it removes unreachable definitions, not
// unused values, and it must only run when every source module is present.
struct FunctionPruneResult {
    uint64_t functionsRemoved = 0;
    uint64_t instructionsRemoved = 0;
    bool skipped = false;
};

// Whole-program pruning over all three definition kinds. Functions, globals,
// and rodata are resolved together: a live global can keep a function or
// another global alive, and a dead cycle stays dead. `rodataBytesRemoved`
// counts the bytes of removed blobs, including their terminator.
struct WholeProgramPruneResult {
    uint64_t functionsRemoved = 0;
    uint64_t instructionsRemoved = 0;
    uint64_t globalsRemoved = 0;
    uint64_t rodataRemoved = 0;
    uint64_t rodataBytesRemoved = 0;
    bool skipped = false;
};

// What phi lowering did for codegen.
struct PhiLowerResult {
    bool ok = false;
    std::string error;
    uint64_t splitBlocks = 0;
    uint64_t phisLowered = 0;
    uint64_t copies = 0;
};

// Structural check, then inlining, then a second structural check, then
// mem2reg, fold, dominance-scoped value numbering, loop-invariant code motion,
// slot forwarding, and dead-code elimination. Inlining first gives mem2reg the
// cloned parameter slots to promote; the second check refuses a malformed
// clone instead of letting a later pass read it; the third check after value
// numbering catches a malformed use rewrite, and the fourth after LICM catches
// a malformed move, before any later pass or codegen sees it.
OptResult run(ir::Module& module);

// Clone small direct, same-module callees at their call sites, one level per
// call site. Parameters become fresh Alloca slots plus explicit Stores of the
// flattened call-argument words, so the following mem2reg can promote them.
// Direct self-recursive callees, callees containing asm, extern/missing
// callees, indirect calls, callees over 80 instructions or 16 blocks, and
// calls whose ABI disagrees with the caller are refused. Returns counters for
// inlined sites and added instructions; a refused call is counted as skipped
// and its IR is untouched.
InlineResult inlineCalls(ir::Module& module);

// Promote non-escaping single-word scalar Alloca slots to SSA values with
// phi nodes at dominance-frontier joins. This is the full Cytron algorithm:
// definition blocks, iterated dominance frontier, and a dominator-tree
// rename walk with one value stack per slot. Loads and stores to promoted
// slots disappear with the Alloca. Anything less certain — escapes,
// aggregates, volatile/partial-width accesses, asm functions, uninitialized
// loads — stays in memory for forwardMemory.
PromotionResult mem2reg(ir::Module& module);

// Replace phi nodes with parallel copies on the incoming edges, splitting
// critical edges first. Codegen consumes ordinary values and refuses a phi,
// so the driver runs this after the optimizer and before generation; the
// local register allocator then works over that lowered form. `cider ir` and
// the VM keep the phi IR. Returns an error rather than silently dropping a
// phi if an edge cannot be made single-entry.
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

// Dominance-scoped global value numbering. An expression is available in a
// block only when its leader dominates that block, so sibling branches never
// share one and a join sees only what every path computed. Commoned ops are
// the explicit pure, non-trapping allowlist in gvn.cpp: scalar integer and
// pointer arithmetic/bitwise/shift, Neg/Not/BitNot, Cmp, Const, Copy and Gep.
// Floats, loads, stores, calls, phis, extract/insert, casts, asm, trapping
// ops and volatile accesses are never commoned. Commutative operands are
// ordered by value id. Every use of a duplicate is redirected to its leader,
// and Copy chains collapse when the copied value has the same width and type.
GvnResult valueNumber(ir::Module& module);

// Loop-invariant code motion over the phi IR. A natural loop is a back edge
// `latch -> header` where the header dominates the latch. A loop is rewritten
// only when it already has a preheader: exactly one predecessor outside the
// loop whose only successor is the header. Pure, non-volatile, non-trapping
// instructions with one non-float result and the GVN allowlist move to the end
// of that preheader when every operand is outside the loop or was hoisted
// earlier. Loops are processed innermost first, so an invariant can leave both
// an inner and an outer loop. Speculation is allowed because an allowed
// instruction cannot fault or observe memory; traps, loads, stores, calls,
// asm, phis, casts and float operations never move. No block is created,
// split, reordered or relabelled.
LicmResult loopInvariantCodeMotion(ir::Module& module);

// Remove function definitions that the entry cannot reach. Globals and
// rodata are kept; this is the function-only form. Callers that own the whole
// program should use pruneWholeProgram, which also collects unreachable data
// and resolves the mutual references between the three kinds. Either form
// must be restricted to whole-program builds, where every source module is
// present and no external object can import a symbol.
FunctionPruneResult pruneFunctions(ir::Module& module, const std::string& entry);

// Remove unreachable functions, globals, and rodata in one reachability
// pass. Roots are the target entry, extern declarations, module-level
// assembly, symbols named by reachable inline assembly, direct call callees,
// FnAddr targets, live globals' initializer labels, and the same properties
// reached recursively through live definitions. Ordering within each list is
// preserved and removal depends only on the module contents.
WholeProgramPruneResult pruneWholeProgram(ir::Module& module,
                                          const std::string& entry);

// Dead-code elimination. Deletes a definition only when every result is
// unused, the instruction is pure, it cannot trap, and it is not volatile.
// Repeats to a fixpoint: deleting one definition can orphan another.
uint64_t deadCode(ir::Module& module);

}  // namespace opt
}  // namespace cider

#endif  // CIDER_OPT_OPT_H
