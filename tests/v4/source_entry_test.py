#!/usr/bin/env python3
"""v4 source-defined entry test.

Native programs define `func _start() -> void`; UEFI programs define
`msabi func efi_main(image_handle: *void, system_table: *void) -> int`.
The compiler emits no startup shim, no `main` trampoline, no automatic
exit, and no firmware handle globals. Missing or wrong entries fail at
`check` and `compile` with a diagnostic naming the symbol.

The UEFI link smoke is skipped with a clear message when clang or GNU ld
with the i386pep emulation is unavailable.
"""

import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))
LINK_UEFI = os.path.join(REPO, "scripts", "link_uefi.sh")

NATIVE = """func _start() -> void {
    asm {
        "mov rax, 60"
        "xor rdi, rdi"
        "syscall"
    }
}
"""

UEFI = """msabi func efi_main(image_handle: *void, system_table: *void) -> int {
    asm { "nop" }
    return 0;
}
"""


def run(args, timeout=60):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def ld_has_i386pep():
    if shutil.which("ld") is None:
        return False
    proc = run(["ld", "-V"])
    return proc.returncode == 0 and "i386pep" in (proc.stdout + proc.stderr)


def validate_pe32plus(path):
    data = open(path, "rb").read()
    if len(data) < 0x100 or data[:2] != b"MZ":
        return "missing DOS/PE header"
    pe = int.from_bytes(data[0x3C:0x40], "little")
    if pe + 0x60 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        return "missing PE signature"
    machine = int.from_bytes(data[pe + 4:pe + 6], "little")
    if machine != 0x8664:
        return "machine is 0x%04x, not x86-64" % machine
    optional = pe + 24
    magic = int.from_bytes(data[optional:optional + 2], "little")
    if magic != 0x20B:
        return "optional-header magic is 0x%04x, not PE32+" % magic
    subsystem = int.from_bytes(data[optional + 68:optional + 70], "little")
    if subsystem != 10:
        return "subsystem is %d, not EFI application (10)" % subsystem
    return None


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []
    skipped = []

    def source(tmp, name, text):
        path = os.path.join(tmp, name)
        with open(path, "w") as handle:
            handle.write(text)
        return path

    def expect_failure(label, path, needle, command="check"):
        proc = run([CIDER, command, path])
        if proc.returncode == 0:
            failures.append("%s: %s accepted" % (label, command))
            return
        if needle not in proc.stderr:
            failures.append(
                "%s: diagnostic does not name %r: %s" % (
                    label, needle, describe(proc)))

    with tempfile.TemporaryDirectory(prefix="cider-v4-entry-") as tmp:
        native_src = source(tmp, "start.cid", NATIVE)
        native_asm = os.path.join(tmp, "start.s")
        native_obj = os.path.join(tmp, "start.o")
        native_bin = os.path.join(tmp, "start.bin")
        compiled = run([CIDER, "compile", native_src, "-o", native_asm])
        if compiled.returncode != 0:
            failures.append("native compile: " + describe(compiled))
        else:
            text = open(native_asm).read()
            if text.count(".globl _start") != 1:
                failures.append("native assembly does not export _start once")
            if ".globl main" in text or "call main" in text:
                failures.append("compiler synthesized a main entry call")
            if "mov eax, 60" in text:
                failures.append("compiler injected an automatic exit syscall")
            assembled = run(["as", "-o", native_obj, native_asm])
            if assembled.returncode != 0:
                failures.append("native assembly: " + describe(assembled))
            else:
                linked = run([
                    "ld", "-nostdlib", "-e", "_start", "-o", native_bin,
                    native_obj,
                ])
                if linked.returncode != 0:
                    failures.append("native link: " + describe(linked))
                else:
                    executed = run([native_bin])
                    if executed.returncode != 0:
                        failures.append(
                            "native binary exited %r" % executed.returncode)

        main_only = source(tmp, "main_only.cid",
                           "func main() -> int { return 0; }\n")
        expect_failure("missing native entry", main_only, "_start")
        expect_failure("missing native entry", main_only, "_start",
                       command="compile")

        wrong_params = source(
            tmp, "wrong_params.cid", "func _start(x: int) -> void { }\n")
        expect_failure("native _start parameters", wrong_params,
                       "takes no parameters")
        wrong_return = source(
            tmp, "wrong_return.cid", "func _start() -> int { return 0; }\n")
        expect_failure("native _start return", wrong_return,
                       "must return void")

        uefi_src = source(tmp, "efi.cid", UEFI)
        uefi_asm = os.path.join(tmp, "efi.s")
        compiled = run([
            CIDER, "compile", "--target", "uefi", uefi_src, "-o", uefi_asm,
        ])
        if compiled.returncode != 0:
            failures.append("uefi compile: " + describe(compiled))
        else:
            text = open(uefi_asm).read()
            if text.count(".globl efi_main") != 1:
                failures.append("uefi assembly does not export efi_main once")
            for forbidden in (
                    "cider_efi_main_user", "call efi_main",
                    "image_handle:", "system_table:",
                    "[rip + image_handle]", "[rip + system_table]",
                    "_start:", "syscall"):
                if forbidden in text:
                    failures.append(
                        "compiler injected uefi shim state: %s" % forbidden)
            if shutil.which("clang") is None or not ld_has_i386pep():
                skipped.append(
                    "UEFI PE32+ link smoke: clang or ld -m i386pep unavailable")
            else:
                efi = os.path.join(tmp, "boot.efi")
                linked = run([LINK_UEFI, uefi_asm, "-o", efi])
                if linked.returncode != 0:
                    failures.append("uefi link: " + describe(linked))
                else:
                    problem = validate_pe32plus(efi)
                    if problem is not None:
                        failures.append("uefi image: " + problem)

        uefi_main = source(tmp, "uefi_main.cid",
                           "func main() -> int { return 0; }\n")
        expect_failure("missing uefi entry", uefi_main, "efi_main",
                       command="check")
        expect_failure("missing uefi entry", uefi_main, "efi_main",
                       command="compile")

        no_abi = source(
            tmp, "no_abi.cid",
            "func efi_main(image_handle: *void, system_table: *void) -> int "
            "{ return 0; }\n")
        proc = run([CIDER, "check", "--target", "uefi", no_abi])
        if proc.returncode == 0 or "msabi" not in proc.stderr:
            failures.append("uefi entry without msabi was not rejected: " +
                            describe(proc))

        one_param = source(
            tmp, "one_param.cid",
            "msabi func efi_main(image_handle: *void) -> int { return 0; }\n")
        proc = run([CIDER, "check", "--target", "uefi", one_param])
        if proc.returncode == 0 or "two parameters" not in proc.stderr:
            failures.append("uefi entry arity was not rejected: " +
                            describe(proc))

        bad_param = source(
            tmp, "bad_param.cid",
            "msabi func efi_main(image_handle: *void, system_table: *int) "
            "-> int { return 0; }\n")
        proc = run([CIDER, "check", "--target", "uefi", bad_param])
        if proc.returncode == 0 or "must be *void" not in proc.stderr:
            failures.append("uefi entry parameter type was not rejected: " +
                            describe(proc))

        bad_return = source(
            tmp, "bad_return.cid",
            "msabi func efi_main(image_handle: *void, system_table: *void) "
            "-> void { }\n")
        proc = run([CIDER, "check", "--target", "uefi", bad_return])
        if proc.returncode == 0 or "must return int" not in proc.stderr:
            failures.append("uefi entry return type was not rejected: " +
                            describe(proc))

        bad_asm = source(
            tmp, "bad_asm.cid",
            "msabi func efi_main(image_handle: *void, system_table: *void) "
            "-> int { asm { \"mov edi, {nope}\" } return 0; }\n")
        proc = run([CIDER, "check", "--target", "uefi", bad_asm])
        if proc.returncode == 0 or "nope" not in proc.stderr:
            failures.append("uefi inline asm placeholder was not validated: " +
                            describe(proc))

    for message in skipped:
        print("SKIP: " + message)
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 entry failure(s)" % len(failures), file=sys.stderr)
        return 1
    if skipped:
        return 77
    print("v4 entry: source-defined _start and efi_main, no compiler shim")
    return 0


if __name__ == "__main__":
    sys.exit(main())
