// regalloc.h — a conservative global register allocator for codegen.
//
// The emitter is still stack-slot based: every value id keeps the frame slot
// the old strategy gave it, and that slot is the fallback/oracle for every
// value the allocator refuses, spills, or cannot keep across a barrier.
//
// Two allocations cooperate:
//
//   - A function-wide liveness pass promotes values whose live range crosses
//     a block boundary. Interference comes from live-set cliques over the
//     lowered CFG, so branch merges, loops and back edges are modelled rather
//     than approximated by block order. A value with more than one definition
//     (a phi result after `opt::lowerPhis`) is excluded from global homes:
//     the lowered form intentionally writes those ids on several edges.
//   - The original block-local linear scan still homes values wholly inside
//     one block. It keeps its old register choices and refuses anything that
//     crosses a barrier or leaves the block. Global homes reserve their
//     register for the whole block they are live in, so the local scan never
//     clobbers a cross-block value.
//
// Register pools. Caller-saved registers are the same rows as before:
//   SysV (native/PomeOS): r8, r9, r10, rdi, rsi
//   Microsoft x64 (UEFI): r8, r9, r10
// No caller-saved value may stay in a register across a call, asm, trap or
// volatile barrier on any path from definition to use.
//
// SysV rdi/rsi are callee-saved under the Microsoft ABI, which is why the
// msabi caller-saved row leaves them alone. The callee-saved pool is rbx on
// both ABIs: the emitter saves and restores it in the frame, and no call
// staging path uses it. r12-r15 stay out of the pool because call argument
// spill staging borrows them without restoring; using them as homes would
// require the allocator to prove a value never crosses such a call.

#ifndef CIDER_CODEGEN_REGALLOC_H
#define CIDER_CODEGEN_REGALLOC_H

#include <cstdint>
#include <vector>

#include "ir/ir.h"

namespace cider {
namespace codegen {

enum class Register : uint8_t {
    None = 0,
    R8,
    R9,
    R10,
    Rdi,
    Rsi,
    Rbx,
};

const char* registerName(Register reg);

// The home of every value in one block. `home[id] == Register::None` means the
// value keeps its stack slot; any other value names the register that carries
// it. A global home appears in every block that defines or uses the value and
// in every block the value is live through.
struct BlockAllocation {
    std::vector<uint8_t> home;
};

struct FunctionAllocation {
    std::vector<BlockAllocation> blocks;
    // Callee-saved registers any home uses, in enum order. The emitter saves
    // exactly these in the prologue and restores them before every return.
    std::vector<Register> calleeSaved;
    uint64_t valuesAllocated = 0;
    uint64_t valuesSpilled = 0;
    uint64_t barrierCount = 0;
    // Global (cross-block) decisions, kept apart from the local counters so
    // the block-local oracle's numbers stay comparable across slices.
    uint64_t globalValues = 0;
    uint64_t globalSpilled = 0;
};

// A pure analysis: it reads the function and answers homes. It never changes
// the function, and it is deterministic in block and value-id order.
FunctionAllocation allocateFunction(const ir::Function& fn);

}  // namespace codegen
}  // namespace cider

#endif  // CIDER_CODEGEN_REGALLOC_H
