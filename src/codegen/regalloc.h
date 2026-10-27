// regalloc.h — a conservative local register allocator for codegen.
//
// The emitter is still stack-slot based: every value id keeps the frame slot
// the old strategy gave it, and that slot is the fallback/oracle for every
// value the allocator refuses, spills, or cannot keep across a barrier. This
// first slice only promotes values whose complete live range lies inside one
// basic block and between two barriers. The allocation is therefore local,
// but it is a real linear scan: intervals are built from defs and uses, the
// register pool is per ABI, and pressure spills the current interval back to
// its existing slot instead of inventing a second spill location.
//
// Why local: codegen runs after `opt::lowerPhis`, and the lowered form writes
// one phi result on several incoming edges. That is intentionally no longer
// single-definition SSA, so a function-wide allocator would need to reason
// about the lowered form's multiple definitions. A block-local range has one
// definition by construction, and the stack-slot form remains the correct
// answer for every cross-block value.
//
// Register pools are caller-saved only:
//   SysV (native/PomeOS): r8, r9, r10, rdi, rsi
//   Microsoft x64 (UEFI): r8, r9, r10
// No callee-saved register is used, so no prologue/epilogue or alignment
// change is needed. rdi/rsi are callee-saved under the Microsoft ABI, which
// is why the msabi pool leaves them alone. rax, rcx, rdx and r11 are fixed
// emitter scratch and never homes. A barrier (call, asm, trap, volatile
// access) cuts allocation; a value live across one stays in its slot.

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
};

const char* registerName(Register reg);

// The home of every value in one block. `home[id] == Register::None` means the
// value keeps its stack slot; any other value names the register that carries
// it from its definition to its last use.
struct BlockAllocation {
    std::vector<uint8_t> home;
};

struct FunctionAllocation {
    std::vector<BlockAllocation> blocks;
    uint64_t valuesAllocated = 0;
    uint64_t valuesSpilled = 0;
    uint64_t barrierCount = 0;
};

// A pure analysis: it reads the function and answers homes. It never changes
// the function, and it is deterministic in block and value-id order.
FunctionAllocation allocateFunction(const ir::Function& fn);

}  // namespace codegen
}  // namespace cider

#endif  // CIDER_CODEGEN_REGALLOC_H
