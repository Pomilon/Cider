// target.cpp — the target table. OUTPUT.md §2.1 and §2.2.

#include "target/target.h"

#include <cstdlib>

namespace cider {
namespace target {

namespace {

// nullFaults is a fact about the memory map, not a preference: address zero is
// unmapped on Linux, while UEFI and bare-metal PomeOS do not promise that, so
// those targets need an emitted check. The VM detects the condition itself.
const Info kNative{Kind::Native, "native", Abi::SysV, "_start", Output::Asm,
                   true, true};
const Info kPomeos{Kind::Pomeos, "pomeos", Abi::PomeOS, "_start", Output::Asm,
                   true, false};
const Info kUefi{Kind::Uefi, "uefi", Abi::MsAbi, "efi_main", Output::Pe32Plus,
                 true, false};
const Info kVm{Kind::Vm, "vm", Abi::SysV, "_start", Output::Interpreted, true,
               false};

}  // namespace

const Info& info(Kind kind) {
    switch (kind) {
        case Kind::Native:
            return kNative;
        case Kind::Pomeos:
            return kPomeos;
        case Kind::Uefi:
            return kUefi;
        case Kind::Vm:
            return kVm;
    }
    std::abort();
}

bool parse(const std::string& name, Kind& out) {
    if (name == "native" || name == "linux" || name == "x86_64-linux") {
        out = Kind::Native;
        return true;
    }
    if (name == "pomeos") {
        out = Kind::Pomeos;
        return true;
    }
    if (name == "uefi") {
        out = Kind::Uefi;
        return true;
    }
    if (name == "vm") {
        out = Kind::Vm;
        return true;
    }
    return false;
}

}  // namespace target
}  // namespace cider
