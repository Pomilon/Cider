// liveness.h — whole-function may-liveness and live ranges over lowered IR.
//
// The input is the lowered, non-phi form codegen receives: one definition per
// value except for the copies a phi lowering leaves on incoming edges. This is
// a pure analysis. It reads the function, answers live-in/live-out sets and
// intervals, and changes neither the function nor codegen behaviour — Task 9's
// whole-function linear scan consumes it.
//
// Positions are deterministic. Blocks are numbered in reverse postorder with
// the entry first, and an instruction's position is
//
//     blockPosition * 1'000'000 + instructionIndex
//
// where the terminator sits at `body.size()`. A live range starts at the first
// definition and ends at the last use, inclusive. Live sets and ranges are
// sorted by value id, so two analyses of the same function produce the same
// answer in the same order.

#ifndef CIDER_CODEGEN_LIVENESS_H
#define CIDER_CODEGEN_LIVENESS_H

#include <cstdint>
#include <vector>

#include "ir/ir.h"

namespace cider {
namespace codegen {

// Reverse-postorder positions scale by this constant so an instruction index
// fits below the next block's base. Liveness, the GPR scan and the float scan
// all order ranges with the same numbers.
constexpr uint32_t kBlockPositionScale = 1000000u;
// Above this many blocks the scaled position wraps in uint32. The allocators
// keep every candidate in its slot instead of ordering wrapped intervals.
constexpr uint32_t kPositionedBlockLimit = 4000u;

// A site in the function: block index and instruction index. Kept as a named
// pair because positions are derived from it and later analyses need both
// halves.
struct Point {
    uint32_t block = 0;
    uint32_t instr = 0;
};

struct LiveRange {
    uint32_t value = 0;
    uint32_t start = 0;       // RPO linear position
    uint32_t end = 0;         // inclusive
    // True when a call/asm/trap/volatile barrier sits strictly between the
    // definition and a later use of this value. A barrier at the definition
    // (a call result) or at the last use (a call argument) is not a crossing.
    bool crossesCall = false;
    bool isFloat = false;
    uint32_t uses = 0;
};

struct Liveness {
    std::vector<std::vector<uint32_t>> liveIn;
    std::vector<std::vector<uint32_t>> liveOut;
    std::vector<LiveRange> ranges;
    std::vector<uint32_t> blockOrder;   // RPO block indices, entry first
};

Liveness analyzeLiveness(const ir::Function& fn);

}  // namespace codegen
}  // namespace cider

#endif  // CIDER_CODEGEN_LIVENESS_H
