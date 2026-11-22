// frame.h — the compact fast-backend frame layout.
//
// The legacy emitter gives every value id its own eight-byte cell and starts
// calls past the last id. The fast backend only gives memory to a value that
// has no register home in any block, and reserves the other frame regions
// explicitly: the outgoing call area, alloca cells, callee-save cells and the
// SysV variadic register-save area.
//
// Layout order, from the lowest address (rsp) upward:
//
//   [rsp + 0, rsp + outgoingBytes)   outgoing call words / MS shadow space
//   spill slots                      one eight-byte cell per unhomed value
//   alloca cells                     one eight-byte cell per Alloca result
//   callee-save cells                one eight-byte cell per saved register
//   SysV variadic save area          only for a SysV variadic function
//
// `frameBytes` rounds the whole span up to the call-alignment granule.
// `outgoingBytes` is at the bottom so a fast call stages its stack words there
// without moving rsp. Every present offset names a distinct eight-byte cell.
//
// Absence is `kNoFrameSlot`, never offset zero: offset zero is a real cell
// (the first outgoing word, or the first spill when no call words are needed).
//
// `spillOffset` and `allocaOffset` are indexed by value id. An id that is not
// mentioned by the function, a value homed in every block that defines or uses
// it, and an Alloca id all answer `kNoFrameSlot` in `spillOffset`; an Alloca
// id answers its cell in `allocaOffset`. The emitter reads an alloca's address
// through `allocaOffset` and never treats the cell's contents as the pointer.

#ifndef CIDER_CODEGEN_FRAME_H
#define CIDER_CODEGEN_FRAME_H

#include <cstdint>
#include <vector>

#include "codegen/regalloc.h"
#include "ir/ir.h"

namespace cider {
namespace codegen {

// The absence sentinel. A present offset is always a multiple of eight and
// names a real cell; zero is therefore unambiguous.
inline constexpr uint32_t kNoFrameSlot = 0xFFFFFFFFu;

// The SysV and Microsoft x64 argument rows. The frame layout and the fast
// emitter read these one shared copy: a drift between the capacity they
// reserve and the words the emitter stages would silently overwrite a spill
// or alloca cell.
inline constexpr uint32_t kFrameWordBytes = 8;
inline constexpr uint32_t kFrameQuantumBytes = 16;
inline constexpr uint32_t kSysVIntegerArgumentRegs = 6;
inline constexpr uint32_t kSysVSseArgumentRegs = 8;
inline constexpr uint32_t kMsPositionalArgumentHomes = 4;
inline constexpr uint32_t kMsShadowSpaceBytes = 32;
inline constexpr uint32_t kSingleFloatWord = 1;

// The SysV variadic register-save area: six integer homes then eight 16-byte
// XMM homes, 176 bytes in all (VARIADICS.md §6).
inline constexpr uint32_t kVarargsGpSlots = 6;
inline constexpr uint32_t kVarargsXmmSlots = 8;
inline constexpr uint32_t kVarargsXmmSlotBytes = 16;
inline constexpr uint32_t kVarargsSaveBytes =
    kVarargsGpSlots * kFrameWordBytes +
    kVarargsXmmSlots * kVarargsXmmSlotBytes;

struct FrameLayout {
    uint32_t frameBytes = 0;
    uint32_t outgoingBytes = 0;
    std::vector<uint32_t> spillOffset;  // value id -> [rsp+offset]
    std::vector<uint32_t> allocaOffset;
};

// One float argument owns one positional slot only when its IR carries one
// word. A float-shaped argument with more words is classified by its words,
// exactly as the emitter's argument walk does.
inline bool isSingleFloatWordArgument(const ir::Argument& arg) {
    return (arg.width == ir::IRType::F32 ||
            arg.width == ir::IRType::F64) &&
           arg.words.size() == kSingleFloatWord;
}

// The call shape the fast emitter stages. `outgoingBase` is zero for SysV and
// the shadow extent for Microsoft x64; `outgoingBytes` is the unrounded span
// from rsp through the last stack word. The layout reserves the maximum of
// these spans, and the emitter checks each call against that reservation.
struct CallFrameShape {
    uint32_t stackWords = 0;
    uint32_t outgoingBase = 0;
    uint32_t outgoingBytes = 0;
    uint32_t vectorRegs = 0;
};

CallFrameShape callFrameShape(const ir::Instr& call);

// The first byte after the outgoing area, spill slots and alloca cells. The
// callee-save cells start here. Both the layout and the emitter use this one
// answer, so the regions cannot be accumulated two different ways.
uint32_t frameSaveBase(const FrameLayout& layout);

// The first byte after the callee-save cells: the SysV variadic area's base.
// `calleeSavedCount` is `alloc.calleeSaved.size()`.
uint32_t frameVarargsBase(const FrameLayout& layout, uint32_t calleeSavedCount);

// Pure and deterministic: reads the function and the allocation, answers the
// compact frame, and changes neither. The function must be lowered (no phis
// reaching codegen) but the layout itself only reads value ids and instruction
// shapes.
FrameLayout layoutFrame(const ir::Function& fn, const FunctionAllocation& alloc);

}  // namespace codegen
}  // namespace cider

#endif  // CIDER_CODEGEN_FRAME_H
