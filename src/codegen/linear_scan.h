// linear_scan.h — the whole-function linear-scan register allocator.
//
// This is the Stage 1 replacement for the conservative block-local scan. It
// consumes `analyzeLiveness`'s whole-function live sets and ranges, and answers
// one home per value per block, the spills that keep their stack slot, and the
// nonvolatile registers the emitter must save in the prologue.
//
// The public type is `FunctionAllocation` in regalloc.h: the emitter already
// reads that type, and the legacy allocator is kept behind a compile-time
// switch for bring-up and for the Stage 1 gate's comparison. There is no
// runtime flag.

#ifndef CIDER_CODEGEN_LINEAR_SCAN_H
#define CIDER_CODEGEN_LINEAR_SCAN_H

#include "codegen/regalloc.h"
#include "ir/ir.h"

namespace cider {
namespace codegen {

// Pure and deterministic: reads the function, answers homes, and never changes
// the function. The wrapper in regalloc.cpp selects this implementation.
FunctionAllocation allocateLinearScan(const ir::Function& fn);

// The same scan over an already-computed liveness result. `allocateFunction`
// uses this overload so the GPR scan and the float scan share one analysis;
// the one-argument form above stays as the test-facing convenience wrapper.
// `fast` selects the fast emitter's exclusions and clobber model: it homes
// parameters and call argument words, and it does not bar the callee-saved
// registers the legacy call staging borrows. Its default keeps every legacy
// caller on the old model.
FunctionAllocation allocateLinearScan(const ir::Function& fn,
                                      const Liveness& live,
                                      bool fast = false);

}  // namespace codegen
}  // namespace cider

#endif  // CIDER_CODEGEN_LINEAR_SCAN_H
