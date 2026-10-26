// codegen.h — checked IR to x86-64 assembly. LAYERS.md §3.7, OUTPUT.md §2.
//
// May read the IR and the target description. May never see the AST, a name, a
// declaration or a type — gate L2 exists to catch exactly that reach, because
// v2's lowering defined 600 symbols of its own, which is a second authority
// for declarations through a side door.
//
// This slice emits native Linux (SysV) and UEFI (Microsoft x64) assembly.
// Every other target is refused with a diagnostic naming it, never silently
// approximated.
//
// Registers are not allocated here: every SSA value lives in its own stack
// slot and the six scratch registers carry it between slot and operation.
// Correct by construction and slow by design — a register allocator replaces
// this file's strategy, not its interface, and nothing downstream may depend
// on which slot a value got.

#ifndef CIDER_CODEGEN_CODEGEN_H
#define CIDER_CODEGEN_CODEGEN_H

#include <cstdint>
#include <string>
#include <vector>

#include "ir/ir.h"
#include "target/target.h"

namespace cider {
namespace codegen {

// One target enum for the compiler. codegen names it Target because its call
// sites read that way, but the facts live in target::info.
using Target = target::Kind;

const char* targetName(Target kind);

struct EmitResult {
    bool ok = false;
    std::string text;
    std::string error;
    Position pos{"", 1, 1};
};

// Emits Intel-syntax assembly for one module on one target. The text is a pure
// function of the module: no timestamps, no working directories, no absolute
// paths — gate L5.
EmitResult emitNative(const ir::Module& module);

// The same, naming the target. Native is the implemented emitter; every other
// kind returns its own diagnostic, because a backend that does not exist must
// say which backend is missing rather than emit the wrong one.
EmitResult emit(const ir::Module& module, Target kind);

}  // namespace codegen
}  // namespace cider

#endif  // CIDER_CODEGEN_CODEGEN_H
