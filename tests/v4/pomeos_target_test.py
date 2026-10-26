#!/usr/bin/env python3
"""v4 PomeOS target test.

The reference PomeOS userland is an x86-64 SysV program entered at `_start`
with argc in rdi and argv in rsi. Cider emits GNU `as` assembly; the PomeOS
link script places the image at 0x2000000 and `pxe_packer.py` wraps the ELF in
the PXE1 header the kernel loads.

This test pins the Cider-side contract only:

- the target descriptor names the entry, ELF64 object, PomeOS link path, and
  PomeOS syscall convention;
- a source-defined `_start` compiles with its syscall numbers untouched;
- `_start(argc: int, argv: **char)` receives the reference arguments;
- the assembly links with the reference link script into an ELF64 ET_EXEC
  image at the reference base address and defines `_start`;
- missing and wrong entries fail with diagnostics naming the rule;
- `run --target pomeos` refuses instead of executing a PomeOS syscall as a
  Linux process.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

TRIVIAL = """func _start() -> void {
    asm {
        "mov rax, 2"
        "xor rdi, rdi"
        "syscall"
    }
}
"""

ARGS = """func _start(argc: int, argv: **char) -> void { }
"""

# programs/link.ld in the reference PomeOS tree. The link step is external to
# the compiler, exactly as it is for UEFI.
LINK_SCRIPT = """OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x2000000;
    .text : { *(.text) }
    .data : { *(.data) }
    .rodata : { *(.rodata) }
    .bss : { *(.bss) }
}
"""

BASE_ADDRESS = 0x2000000

FORBIDDEN = (
    "_setjmp",
    "longjmp",
    "cider_print",
    "cider_panic",
    "cider_alloc",
    "cider_free",
    "cider_realloc",
    "cider_itoa_",
    "cider_ftoa",
    "cider_fini_run",
)


def run(args, timeout=60):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def elf_header(data):
    if data[:4] != b"\x7fELF":
        return None, "missing ELF magic"
    if data[4] != 2:
        return None, "ELF class is not 64-bit"
    if data[5] != 1:
        return None, "ELF data is not little-endian"
    e_type = int.from_bytes(data[16:18], "little")
    e_machine = int.from_bytes(data[18:20], "little")
    e_entry = int.from_bytes(data[24:32], "little")
    return (e_type, e_machine, e_entry), None


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []

    with tempfile.TemporaryDirectory(prefix="cider-v4-pomeos-") as tmp:
        def source(name, text):
            path = os.path.join(tmp, name)
            with open(path, "w") as handle:
                handle.write(text)
            return path

        def expect_failure(label, path, needle, command="check"):
            proc = run([CIDER, command, "--target", "pomeos", path])
            if proc.returncode == 0:
                failures.append("%s: %s accepted" % (label, command))
            elif needle not in proc.stderr:
                failures.append(
                    "%s: diagnostic does not name %r: %s" % (
                        label, needle, describe(proc)))

        trivial = source("start.cid", TRIVIAL)
        asm_path = os.path.join(tmp, "start.s")
        obj_path = os.path.join(tmp, "start.o")
        elf_path = os.path.join(tmp, "start.elf")
        compiled = run([CIDER, "compile", "--target", "pomeos", trivial,
                        "-o", asm_path])
        if compiled.returncode != 0:
            failures.append("pomeos compile: " + describe(compiled))
        else:
            text = open(asm_path).read()
            if ".intel_syntax noprefix" not in text:
                failures.append("assembly is not GNU as Intel syntax")
            if text.count(".globl _start") != 1:
                failures.append("assembly does not export _start exactly once")
            if text.count("\n_start:") != 1:
                failures.append("assembly does not define _start exactly once")
            if "call main" in text:
                failures.append("compiler synthesized a main call")
            if "mov rax, 2" not in text:
                failures.append("source PomeOS syscall number was not emitted")
            for linux in ("mov rax, 60", "mov eax, 60"):
                if linux in text:
                    failures.append(
                        "emitter injected a Linux exit syscall: %s" % linux)
            for symbol in FORBIDDEN:
                if symbol in text:
                    failures.append(
                        "compiler runtime symbol leaked: %s" % symbol)
            if text.count("syscall") != 1:
                failures.append(
                    "emitter injected a syscall beside the source's one")

            assembled = run(["as", "-o", obj_path, asm_path])
            if assembled.returncode != 0:
                failures.append("assembling failed: " + describe(assembled))
            else:
                linker = os.path.join(tmp, "link.ld")
                with open(linker, "w") as handle:
                    handle.write(LINK_SCRIPT)
                linked = run([
                    "ld", "-nostdlib", "-static", "-T", linker,
                    "-o", elf_path, obj_path,
                ])
                if linked.returncode != 0:
                    failures.append("PomeOS link failed: " + describe(linked))
                else:
                    data = open(elf_path, "rb").read()
                    header, problem = elf_header(data)
                    if problem is not None:
                        failures.append("PomeOS image: " + problem)
                    else:
                        e_type, e_machine, e_entry = header
                        if e_type != 2:
                            failures.append(
                                "PomeOS image is not ET_EXEC (%d)" % e_type)
                        if e_machine != 62:
                            failures.append(
                                "PomeOS image is not x86-64 (%d)" % e_machine)
                        if e_entry != BASE_ADDRESS:
                            failures.append(
                                "PomeOS entry is 0x%x, not 0x%x" % (
                                    e_entry, BASE_ADDRESS))
                    symbols = run(["nm", elf_path])
                    if symbols.returncode != 0:
                        failures.append(
                            "nm failed: " + describe(symbols))
                    elif not re.search(
                            r"(?m)^[0-9a-fA-F]+\s+T\s+_start$",
                            symbols.stdout):
                        failures.append("_start is not a defined text symbol")
                    undefined = run(["nm", "-u", elf_path])
                    if undefined.returncode == 0 and undefined.stdout.strip():
                        failures.append(
                            "PomeOS image has undefined symbols: %s" %
                            undefined.stdout.strip())

        args_src = source("args.cid", ARGS)
        args_asm = os.path.join(tmp, "args.s")
        compiled = run([CIDER, "compile", "--target", "pomeos", args_src,
                        "-o", args_asm])
        if compiled.returncode != 0:
            failures.append(
                "PomeOS argc/argv entry compile: " + describe(compiled))
        else:
            text = open(args_asm).read()
            if not re.search(r"mov \[rsp \+ \d+\], rdi", text):
                failures.append("PomeOS entry does not receive argc in rdi")
            if not re.search(r"mov \[rsp \+ \d+\], rsi", text):
                failures.append("PomeOS entry does not receive argv in rsi")

        main_only = source("main_only.cid",
                           "func main() -> int { return 0; }\n")
        expect_failure("missing entry", main_only, "_start")
        expect_failure("missing entry", main_only, "_start",
                       command="compile")

        one_arg = source("one_arg.cid",
                         "func _start(argc: int) -> void { }\n")
        expect_failure("one-argument entry", one_arg,
                       "argc: int, argv: **char")

        bad_argv = source("bad_argv.cid",
                          "func _start(argc: int, argv: **u8) -> void { }\n")
        expect_failure("non-char argv", bad_argv, "**char")

        bad_return = source("bad_return.cid",
                            "func _start() -> int { return 0; }\n")
        expect_failure("non-void entry", bad_return, "must return void")

        msabi = source("msabi.cid", "msabi func _start() -> void { }\n")
        expect_failure("msabi entry", msabi, "sysv")

        refused = run([CIDER, "run", "--target", "pomeos", trivial])
        if refused.returncode == 0:
            failures.append("run --target pomeos executed a host binary")
        elif "not a host executable" not in refused.stderr:
            failures.append(
                "run refusal does not explain the PomeOS path: " +
                describe(refused))

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 PomeOS failure(s)" % len(failures), file=sys.stderr)
        return 1
    print("v4 pomeos: target descriptor, source entry, ELF64 link, diagnostics")
    return 0


if __name__ == "__main__":
    sys.exit(main())
