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
    Pomeos,  // PomeOS userland: SysV C ABI plus PomeOS kernel syscalls
    Uefi,    // UEFI PE32+ application
    Vm,      // the Cider interpreter
};

// `PomeOS` is a target ABI tag, not a second C calling convention. The
// reference PomeOS tree compiles its C/C++ userland with the ordinary
// x86-64 SysV convention, and its Cider port spells the same convention on
// `_start(argc, argv)`. What differs from native is the kernel entry and the
// syscall numbers, not argument passing. Lowering therefore maps unspelled
// signatures on `pomeos` to SysV; an explicit `msabi` still means Microsoft
// x64.
enum class Abi {
    SysV,
    MsAbi,
    PomeOS,
};

// What the compiler writes. Native and PomeOS both write GNU `as` assembly;
// the PomeOS linker turns it into ELF64 and then PXE, so their direct output
// is still assembly.
enum class Output {
    Asm,
    Pe32Plus,
    Interpreted,
};

// The linked object an assembly output becomes. This is separate from Output:
// one assembly emitter can serve two link formats, and the file that names the
// link path must not be inferred from the emitter's spelling.
enum class Object {
    None,
    Elf64,
    Pe32Plus,
};

// The link step a target's output requires. PomeOS is not a host executable:
// `programs/link.ld` places the image at its per-program base and
// `scripts/pxe_packer.py` wraps the ELF in the PXE1 header the kernel loads.
enum class Link {
    None,
    HostElf,   // as + ld -nostdlib -e _start
    PomeosElf, // as + ld -nostdlib -T programs/link.ld, then pxe_packer.py
    UefiPe,    // scripts/link_uefi.sh
};

// The kernel or firmware service convention. Codegen never rewrites syscall
// numbers: source assembly and target libraries spell the numbers their
// target defines. PomeOS uses the Linux register row (rax number,
// rdi/rsi/rdx/r10/r8/r9 arguments, rax result) with PomeOS's own numbers:
// exit 2, write 13, read 12, open 10, close 11, sbrk 9, mmap 48.
enum class Syscall {
    None,
    Linux,
    PomeOS,
    Firmware,
};

struct Info {
    Kind kind;
    const char* name;
    Abi abi;
    const char* entry;
    Output output;
    Object object;
    Link link;
    Syscall syscall;
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
