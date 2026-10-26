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
#include <vector>

#include "ir/ir.h"

namespace cider {
namespace vm {

struct VmResult {
    // ok is true once the interpreter reached a program end, whether the
    // program exited with a code or died by a signal; ok is false only when
    // the interpreter met IR it cannot run, which error names.
    bool ok = false;
    int exitCode = 0;
    // Non-zero when the program died by that signal: the emitter's traps
    // and divides fault on hardware, and a differential leg must show the
    // same death rather than a refusal.
    int signal = 0;
    std::string stdoutText;
    std::string stderrText;
    std::string error;
    Position pos{"", 1, 1};
};

// Interprets the source-defined `_start` of one lowered module, with the
// bytes a read from standard input will answer. v4 has no automatic main
// call or argument binding: `_start` takes no parameters, exactly as the
// native entry does.
VmResult run(const ir::Module& module, const std::string& pendingInput);

}  // namespace vm
}  // namespace cider

#endif  // CIDER_VM_VM_H
