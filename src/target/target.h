// target.h — the one description of a compilation target.
//
// OUTPUT.md §2: a target is a description, not a layer. This header owns the
// facts every later stage asks about: its ABI, entry symbol, output form, and
// the two policies a target can differ on (inline assembly and whether the
// hardware faults on a null access). Codegen, the driver and the checker may
// read it; none of them may grow a second copy.

#ifndef CIDER_TARGET_TARGET_H
#define CIDER_TARGET_TARGET_H

#include <string>

namespace cider {
namespace target {

enum class Kind {
    Native,  // Linux x86-64, SysV
    Pomeos,  // bare-metal PomeOS
    Uefi,    // UEFI PE32+ application
    Vm,      // the Cider interpreter
};

enum class Abi {
    SysV,
    MsAbi,
    PomeOS,
};

enum class Output {
    Asm,
    Pe32Plus,
    Interpreted,
};

struct Info {
    Kind kind;
    const char* name;
    Abi abi;
    const char* entry;
    Output output;
    bool allowsAsm;
    bool nullFaults;
};

// Total over Kind. The switch is exhaustive; -Werror=switch catches a target
// added without a description.
const Info& info(Kind kind);

// Canonical name or alias -> Kind. Aliases `linux` and `x86_64-linux` mean
// native. On failure `out` is left unchanged.
bool parse(const std::string& name, Kind& out);

}  // namespace target
}  // namespace cider

#endif  // CIDER_TARGET_TARGET_H
