#!/usr/bin/env python3
"""v4 freestanding symbol audit.

The first v4 milestone is a program that defines its own `_start`, uses no
prelude, and links with `ld -nostdlib`. This test compiles the smallest such
program and proves that the emitter did not pull in the v3 compiler runtime:
no print/panic helper, no allocator, no formatting helper, no fini stub, and
no exception state.
"""

import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

SOURCE = """func _start() -> void {
    asm {
        "mov rax, 60"
        "xor rdi, rdi"
        "syscall"
    }
}
"""

FORBIDDEN = (
    "cider_print",
    "cider_panic",
    "putchar",
    "cider_alloc",
    "cider_free",
    "cider_realloc",
    "cider_heap_fail",
    "cider_fl_head",
    "cider_heap_total",
    "cider_itoa_signed",
    "cider_itoa_unsigned",
    "cider_ftoa",
    "cider_fini_run",
    "cider_exc_flag",
    "cider_exc_type",
    "cider_exc_value",
    "cider_exc_raise",
)


def run(args, timeout=60):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix="cider-v4-") as tmp:
        source_path = os.path.join(tmp, "start.cid")
        asm_path = os.path.join(tmp, "start.s")
        obj_path = os.path.join(tmp, "start.o")
        bin_path = os.path.join(tmp, "start.bin")
        with open(source_path, "w") as handle:
            handle.write(SOURCE)

        compiled = run([CIDER, "compile", source_path, "-o", asm_path])
        if compiled.returncode != 0:
            print("FAIL: compile: %s" % describe(compiled), file=sys.stderr)
            return 1
        if compiled.stderr:
            failures.append("compiler wrote stderr: %r" % compiled.stderr)

        with open(asm_path) as handle:
            text = handle.read()

        if ".globl _start" not in text:
            failures.append("emitted assembly does not export _start")
        if text.count(".globl _start") != 1:
            failures.append("emitted assembly has more than one _start definition")

        for symbol in FORBIDDEN:
            if symbol in text:
                failures.append("compiler runtime symbol leaked: %s" % symbol)

        assembled = run(["as", "-o", obj_path, asm_path])
        if assembled.returncode != 0:
            failures.append("assembling failed: %s" % describe(assembled))
        else:
            linked = run([
                "ld", "-nostdlib", "-e", "_start", "-o", bin_path, obj_path,
            ])
            if linked.returncode != 0:
                failures.append("linking failed: %s" % describe(linked))
            else:
                executed = run([bin_path])
                if executed.returncode != 0:
                    failures.append("binary exited %r" % executed.returncode)

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 freestanding failure(s)" % len(failures), file=sys.stderr)
        return 1
    print("v4 freestanding: source-defined _start, no compiler runtime")
    return 0


if __name__ == "__main__":
    sys.exit(main())
