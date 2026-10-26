// target_test.cpp — the target descriptor is the one place target facts live.
//
// LAYERS.md and OUTPUT.md §2 make a target a description: ABI, entry symbol,
// output form, whether inline assembly is allowed, and whether a null access
// faults in hardware. This test pins those facts for every target the enum
// names. The descriptor must be total over Kind; -Werror=switch makes a
// missing enumerator a build failure.

#include "target/target.h"

#include <cstdio>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void check(bool holds, const std::string& what) {
    ++checks;
    if (holds) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++failures;
    }
}

using cider::target::Abi;
using cider::target::Kind;
using cider::target::Output;

void testNative() {
    std::printf("native\n");
    const auto& info = cider::target::info(Kind::Native);
    check(info.kind == Kind::Native, "kind is native");
    check(std::string(info.name) == "native", "name is native");
    check(info.abi == Abi::SysV, "ABI is SysV");
    check(std::string(info.entry) == "_start", "entry is _start");
    check(info.output == Output::Asm, "output is assembly");
    check(info.allowsAsm, "inline assembly is allowed");
    check(info.nullFaults, "address zero is unmapped, so a null access faults");
}

void testUefi() {
    std::printf("uefi\n");
    const auto& info = cider::target::info(Kind::Uefi);
    check(info.kind == Kind::Uefi, "kind is uefi");
    check(std::string(info.name) == "uefi", "name is uefi");
    check(info.abi == Abi::MsAbi, "ABI is Microsoft x64");
    check(std::string(info.entry) == "efi_main", "entry is efi_main");
    check(info.output == Output::Pe32Plus, "output is PE32+");
    check(!info.allowsAsm, "inline assembly is rejected");
    check(!info.nullFaults, "null accesses need emitted checks");
}

void testPomeos() {
    std::printf("pomeos\n");
    const auto& info = cider::target::info(Kind::Pomeos);
    check(info.kind == Kind::Pomeos, "kind is pomeos");
    check(std::string(info.name) == "pomeos", "name is pomeos");
    check(info.abi == Abi::PomeOS, "ABI is PomeOS");
    check(std::string(info.entry) == "_start", "entry is _start");
    check(info.output == Output::Asm, "output is assembly");
    check(info.allowsAsm, "inline assembly is allowed");
    check(!info.nullFaults, "null accesses need emitted checks on bare metal");
}

void testVm() {
    std::printf("vm\n");
    const auto& info = cider::target::info(Kind::Vm);
    check(info.kind == Kind::Vm, "kind is vm");
    check(std::string(info.name) == "vm", "name is vm");
    check(info.output == Output::Interpreted, "output is interpreted");
    check(std::string(info.entry) == "_start", "entry is _start");
    check(info.allowsAsm, "asm statements are interpreted where supported");
}

void testParse() {
    std::printf("parse\n");
    Kind kind = Kind::Vm;
    check(cider::target::parse("linux", kind) && kind == Kind::Native,
          "linux is a native alias");
    kind = Kind::Vm;
    check(cider::target::parse("x86_64-linux", kind) && kind == Kind::Native,
          "x86_64-linux is a native alias");
    kind = Kind::Vm;
    check(cider::target::parse("native", kind) && kind == Kind::Native,
          "native parses");
    kind = Kind::Native;
    check(cider::target::parse("uefi", kind) && kind == Kind::Uefi,
          "uefi parses");
    kind = Kind::Native;
    check(cider::target::parse("pomeos", kind) && kind == Kind::Pomeos,
          "pomeos parses");
    kind = Kind::Native;
    check(cider::target::parse("vm", kind) && kind == Kind::Vm, "vm parses");
    kind = Kind::Native;
    check(!cider::target::parse("mips", kind), "an unknown target fails");
}

}  // namespace

int main() {
    testNative();
    testUefi();
    testPomeos();
    testVm();
    testParse();
    std::printf("target_test: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
