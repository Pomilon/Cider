// regalloc.h — the register-file vocabulary and allocation result.
//
// The allocator is a whole-function linear scan over the lowered, non-phi IR.
// It consumes `Liveness` (live-in/live-out sets, RPO positions, use counts and
// barrier crossings) and answers one home per value per block. Values it
// refuses, spills, or cannot keep across a barrier keep their frame slot, which
// remains the complete fallback for every value.
//
// Register pools. The SysV argument row is caller-saved:
//   rax, rcx, rdx, rsi, rdi, r8, r9, r10, r11
// The Microsoft x64 row keeps r8/r9/r10 first and omits rsi/rdi: those are
// nonvolatile there, and saving an arbitrary callee-saved GPR is Task 13's ABI
// slice. The callee pool is rbx, r12, r13, r14, r15. rsp and rbp are reserved.
//
// A value whose live range crosses a call, inline asm, trap, va_list access,
// or volatile access may only use a callee-saved register. The allocator also
// models the emitter's implicit scratch clobbers (the rax/rcx/rdx/r11 round
// trips, div/rem's rdx, shifts' rcx, call staging's r12-r15) so a home is
// never placed where the current emitter would overwrite it. Those models are
// in linear_scan.cpp; when an emitter path stops clobbering a register, its
// clobber entry can shrink in the same change.
//
// A value with more than one definition (a phi result after `opt::lowerPhis`),
// a float, Void, or a value touched by the fixed-scratch rules keeps its slot
// until a later slice handles it. A parameter keeps its slot on the legacy
// path because its prologue writes the slot; the fast path may home one, and
// the emitter then writes and reads that home (see `settleFastParameters`).

#ifndef CIDER_CODEGEN_REGALLOC_H
#define CIDER_CODEGEN_REGALLOC_H

#include <cstdint>
#include <vector>

#include "codegen/liveness.h"
#include "ir/ir.h"

namespace cider {
namespace codegen {

enum class Register : uint8_t {
    None = 0,
    Rax,
    Rcx,
    Rdx,
    Rbx,
    Rsi,
    Rdi,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
};

const char* registerName(Register reg);

// The float register file is a separate class: an XMM home never aliases a
// GPR home. `Xmm0` stays out of the pool because the current float selectors
// use it as their fixed scratch register; every other XMM register may carry a
// live float value.
enum class XmmRegister : uint8_t {
    None = 0,
    Xmm0,
    Xmm1,
    Xmm2,
    Xmm3,
    Xmm4,
    Xmm5,
    Xmm6,
    Xmm7,
    Xmm8,
    Xmm9,
    Xmm10,
    Xmm11,
    Xmm12,
    Xmm13,
    Xmm14,
    Xmm15,
};

const char* xmmRegisterName(XmmRegister reg);

// The home of every value in one block. `home[id] == Register::None` means the
// value keeps its stack slot; any other value names the register that carries
// it. A home appears in every block that defines or uses the value and in
// every block the value is live through.
struct BlockAllocation {
    std::vector<uint8_t> home;
};

// The parallel float allocation. `home[block][value]` is an XmmRegister when
// the value has an XMM home, otherwise None and the value keeps its frame
// slot. `calleeSaved` is empty on SysV, where every XMM register is
// caller-saved, and names the used xmm6-xmm15 on Microsoft x64.
struct FloatAllocation {
    std::vector<std::vector<uint8_t>> home;
    std::vector<XmmRegister> calleeSaved;
};

struct FunctionAllocation {
    std::vector<BlockAllocation> blocks;
    // Nonvolatile registers any home uses, unique and in enum order. The
    // emitter saves exactly these in the prologue and restores them before
    // every return. Task 9 uses rbx/r12-r15 on both targets; Microsoft x64's
    // rsi/rdi are nonvolatile but stay out of the pool until Task 13 owns
    // saving arbitrary callee-saved GPRs.
    std::vector<Register> calleeSaved;
    // Value id -> frame slot index. The current frame still gives every id its
    // own cell, so this maps id to id until Task 10's compact frame consumes
    // it. It is complete for every id the function mentions.
    std::vector<uint32_t> spillSlot;
    uint64_t valuesAllocated = 0;
    uint64_t valuesSpilled = 0;
    uint64_t barrierCount = 0;
    // Cross-block decisions, kept apart from the local counters so the Stage 1
    // gate can compare the whole-function scan with the legacy one.
    uint64_t globalValues = 0;
    uint64_t globalSpilled = 0;
    // The one liveness analysis the allocation ran. The fast emitter reuses
    // `live.liveOut` for its compare-fusion safety net instead of analysing
    // the function a second time.
    Liveness live;
    // Separate register class, filled once after the GPR allocation. Empty
    // when the caller asks for GPR-only allocation.
    FloatAllocation floats;
};

// A pure analysis: it reads the function and answers homes. It never changes
// the function, and it is deterministic in block and value-id order. The
// default allocates both register classes; the legacy emitter passes false so
// it pays for no float analysis. `fast` selects the fast emitter's exclusions
// and clobber model (parameter and call-word homes, no staged call clobber);
// its default keeps every existing caller on the legacy model.
FunctionAllocation allocateFunction(const ir::Function& fn,
                                    bool includeFloats = true,
                                    bool fast = false);

// The float half of the allocation. It uses the same linear-scan order and
// spill-cost rule as the GPR scan but a separate register class: SysV has no
// callee-saved XMM register, so a float live across a barrier keeps its slot;
// Microsoft x64 may use xmm6-xmm15 and reports the used ones in
// `calleeSaved`. Xmm0 is reserved as the emitter's float scratch register.
FloatAllocation allocateFloats(const ir::Function& fn, const Liveness& live);

}  // namespace codegen
}  // namespace cider

#endif  // CIDER_CODEGEN_REGALLOC_H
