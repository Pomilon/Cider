// vm.h — the v3 interpreter: an IR module in, an exit code out.
//
// Reads the IR only, under the same layer discipline as codegen: never the
// AST, a name, a declaration or a type beyond what the IR carries. No
// toolchain, no fork: execution is a function call, which is what makes the
// vm-unopt/vm-opt harness legs toolchain-free. An instruction the interpreter
// does not implement yet fails loudly naming the opcode, never silently
// approximated.

#ifndef CIDER_VM_VM_H
#define CIDER_VM_VM_H

#include <cstdint>
#include <string>

#include "ir/ir.h"

namespace cider {
namespace vm {

struct VmResult {
    bool ok = false;
    int exitCode = 0;
    std::string stdoutText;
    std::string stderrText;
    std::string error;
    Position pos{"", 1, 1};
};

// Interprets main (or _start) of one lowered module with no arguments.
VmResult run(const ir::Module& module);

}  // namespace vm
}  // namespace cider

#endif  // CIDER_VM_VM_H
