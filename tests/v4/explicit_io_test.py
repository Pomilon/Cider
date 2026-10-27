#!/usr/bin/env python3
"""v4 explicit I/O test.

`putchar` is not a compiler-provided helper in v4. A bare extern declaration
gets an ordinary undefined symbol, and only an explicit library import can
define it. The library implementation lives in `std.io` and is reached over
the same syscall/asm surface as the rest of that test-only module.

The test also pins the positive side: a source-defined `_start` that imports
`std.io` can print on native and on both interpreter legs without any
compiler runtime, prelude, or hidden host call.
"""

import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))
LIB = os.path.join(REPO, "modules")

EXTERN = """extern func putchar(c: int) -> int;

func _start() -> void {
    putchar(65);
}
"""

UNDECLARED = """func _start() -> void {
    putchar(65);
}
"""

EXPLICIT = """from std.io import print, putchar, exit;

var keep_putchar: *void = &putchar as *void;

func _start() -> void {
    if (keep_putchar == nilPtr) {
        exit(7);
    }
    var wrote: int = putchar(67);
    if (wrote != 67) {
        exit(1);
    }
    print(" explicit\\n");
    exit(0);
}
"""

UEFI_EXTERN = """extern func putchar(c: int) -> int;

msabi func efi_main(image_handle: *void, system_table: *void) -> int {
    putchar(65);
    return 0;
}
"""

FORBIDDEN = (
    "cider_print",
    "cider_panic",
    "cider_alloc",
    "cider_free",
    "cider_realloc",
    "cider_itoa_signed",
    "cider_itoa_unsigned",
    "cider_ftoa",
    "cider_fini_run",
)

LEGS = (
    ("native", []),
    ("vm-unopt", ["--target", "vm", "--no-opt"]),
    ("vm-opt", ["--target", "vm"]),
)


def run(args, env=None, timeout=60):
    return subprocess.run(
        args, capture_output=True, text=True, env=env, timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def library_env():
    env = dict(os.environ)
    env["CIDER_LIB"] = LIB
    return env


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix="cider-v4-io-") as tmp:
        def source(name, text):
            path = os.path.join(tmp, name)
            with open(path, "w") as handle:
                handle.write(text)
            return path

        # A bare extern is only a declaration: the compiler defines no
        # helper, so the ordinary freestanding link must fail loudly.
        extern_src = source("extern.cid", EXTERN)
        extern_asm = os.path.join(tmp, "extern.s")
        extern_obj = os.path.join(tmp, "extern.o")
        extern_bin = os.path.join(tmp, "extern.bin")
        compiled = run([CIDER, "compile", extern_src, "-o", extern_asm])
        if compiled.returncode != 0:
            failures.append("extern putchar compile: " + describe(compiled))
        else:
            text = open(extern_asm).read()
            if "\nputchar:" in text:
                failures.append("codegen defined a putchar helper")
            if ".globl putchar" in text:
                failures.append("codegen exported a putchar helper")
            for symbol in FORBIDDEN:
                if symbol in text:
                    failures.append(
                        "compiler runtime symbol leaked: %s" % symbol)

            assembled = run(["as", "-o", extern_obj, extern_asm])
            if assembled.returncode != 0:
                failures.append("extern putchar assembly: " + describe(assembled))
            else:
                linked = run([
                    "ld", "-nostdlib", "-e", "_start", "-o", extern_bin,
                    extern_obj,
                ])
                if linked.returncode == 0:
                    failures.append(
                        "extern putchar linked without a library definition")
                elif "putchar" not in linked.stderr:
                    failures.append(
                        "undefined-symbol diagnostic does not name putchar: " +
                        describe(linked))

        # Without an import the name is not even bound: there is no prelude.
        undeclared_src = source("undeclared.cid", UNDECLARED)
        checked = run([CIDER, "check", undeclared_src])
        if checked.returncode == 0:
            failures.append("bare putchar was recognized without a declaration")
        elif "putchar" not in checked.stderr:
            failures.append(
                "undeclared putchar diagnostic does not name it: " +
                describe(checked))

        # The UEFI special case is gone too: the extern is an ordinary
        # unresolved linker symbol, never a compiler-emitted write path.
        uefi_src = source("uefi.cid", UEFI_EXTERN)
        uefi_asm = os.path.join(tmp, "uefi.s")
        compiled = run([CIDER, "compile", "--target", "uefi", uefi_src,
                        "-o", uefi_asm])
        if compiled.returncode != 0:
            failures.append("uefi extern putchar compile: " + describe(compiled))
        else:
            text = open(uefi_asm).read()
            if "\nputchar:" in text or ".globl putchar" in text:
                failures.append("uefi codegen defined a putchar helper")
            if "syscall" in text:
                failures.append("uefi codegen emitted a Linux write path")

        # Explicit std.io is ordinary library code: import it, link it with
        # `ld -nostdlib`, and print through it on native.
        io_src = source("io.cid", EXPLICIT)
        io_asm = os.path.join(tmp, "io.s")
        io_obj = os.path.join(tmp, "io.o")
        io_bin = os.path.join(tmp, "io.bin")
        compiled = run([CIDER, "compile", io_src, "-o", io_asm],
                       env=library_env())
        if compiled.returncode != 0:
            failures.append("explicit std.io compile: " + describe(compiled))
        else:
            text = open(io_asm).read()
            if "std_io__putchar:" not in text:
                failures.append("std.io putchar was not emitted as a library function")
            if "\nputchar:" in text or ".globl putchar" in text:
                failures.append("std.io putchar fell back to a compiler helper")
            for symbol in FORBIDDEN:
                if symbol in text:
                    failures.append(
                        "compiler runtime symbol leaked: %s" % symbol)

            assembled = run(["as", "-o", io_obj, io_asm])
            if assembled.returncode != 0:
                failures.append("explicit std.io assembly: " + describe(assembled))
            else:
                linked = run([
                    "ld", "-nostdlib", "-e", "_start", "-o", io_bin, io_obj,
                ])
                if linked.returncode != 0:
                    failures.append("explicit std.io link: " + describe(linked))
                else:
                    executed = run([io_bin])
                    if (executed.returncode, executed.stdout,
                            executed.stderr) != (0, "C explicit\n", ""):
                        failures.append(
                            "explicit std.io native run: " + describe(executed))

        # The same explicit library works through the interpreter, which no
        # longer hosts a synthetic putchar call.
        for leg, leg_args in LEGS:
            proc = run([CIDER, "run"] + leg_args + [io_src], env=library_env())
            if (proc.returncode, proc.stdout, proc.stderr) != (
                    0, "C explicit\n", ""):
                failures.append(
                    "explicit std.io [%s]: %s" % (leg, describe(proc)))

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 explicit I/O failure(s)" % len(failures), file=sys.stderr)
        return 1
    print("v4 explicit io: no compiler putchar; std.io is explicit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
