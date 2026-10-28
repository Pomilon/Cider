#!/usr/bin/env python3
"""v4 native C interop for C variadics (VARIADICS.md §11).

Two directions are proved against real gcc-compiled objects:

- a Cider caller calls the C helpers in tests/v4/c/varargs_helper.c with
  integers, pointers and doubles, directly and through a variadic function
  pointer;
- a gcc-compiled C caller declares a Cider-defined `sum(int, ...)` and the
  other variadic exports, and receives exact answers.

The Cider-defined `sum` uses `va_copy` and `va_end`, so a broken copy or a
missing end shows up as a wrong sum. The UEFI fixture is compiled as an
msabi variadic function and its assembly is checked for the Microsoft homes
and floating-point duplication, then linked as a PE32+ image when the COFF
toolchain is available.

gcc, clang and GNU ld's i386pep emulation are the UEFI toolchain as well as
the native one, so a missing tool skips the gate with status 77 rather than
pretending the interop was checked.
"""

import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))
LIB = os.path.join(REPO, "modules")
LINK_UEFI = os.path.join(REPO, "scripts", "link_uefi.sh")
HELPER = os.path.join(REPO, "tests", "v4", "c", "varargs_helper.c")

# The Cider caller direction: every check folds into one exit code, so a
# failure names the subtest that failed. The expected answer is 0.
CALLER = """extern func c_sum(n: i32, ...) -> i64;
extern func c_sum_f64(n: i32, ...) -> f64;
extern func c_sum_ptr(n: i32, ...) -> i64;

var first: i64 = 20;
var second: i64 = 22;

func _start() -> void {
    var direct: i64 = c_sum(4, 10, 12, 15, 5);
    var spilled: i64 = c_sum(8, 1, 2, 3, 4, 5, 6, 7, 14);
    var doubles: i64 = c_sum_f64(3, 1.5, 2.25, 3.25) as i64;
    var many: i64 = c_sum_f64(8, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
                              14.0) as i64;
    var pointers: i64 = c_sum_ptr(2, &first, &second);
    var p: *func(i32, ...) -> i64 = &c_sum;
    var indirect: i64 = p(3, 10, 20, 12);
    var pf: *func(i32, ...) -> f64 = &c_sum_f64;
    var indirect_f: i64 = pf(3, 1.5, 2.25, 3.25) as i64;

    var bad: i32 = 0;
    if (direct != 42) { bad = 1; }
    if (spilled != 42) { bad = 2; }
    if (doubles != 7) { bad = 3; }
    if (many != 42) { bad = 4; }
    if (pointers != 42) { bad = 5; }
    if (indirect != 42) { bad = 6; }
    if (indirect_f != 7) { bad = 7; }
    asm {
        "mov rax, 60"
        "mov edi, {bad}"
        "syscall"
    }
}
"""

CALLER_FAILURES = {
    1: "direct integer variadic call",
    2: "integer variadic arguments beyond the register row",
    3: "f64 variadic call",
    4: "eight f64 variadic registers (SysV al count)",
    5: "pointer variadic arguments",
    6: "variadic call through a function pointer",
    7: "f64 variadic call through a function pointer",
}

# The C-to-Cider direction: `sum` reads every value twice through va_copy,
# so both cursors must yield the same total. `sum_f64` and `sum_ptr` cover
# the other two scalar classes the ABI carries.
SUM = """from std.varargs import VaList, va_start, va_copy, va_end, va_arg_i64,
    va_arg_f64, va_arg_ptr;

export func sum(n: i32, ...) -> i64 {
    var ap: VaList = uninit;
    var copy: VaList = uninit;
    va_start(&ap);
    va_copy(&copy, &ap);
    var primary: i64 = 0;
    var duplicate: i64 = 0;
    var i: i32 = 0;
    while (i < n) {
        primary = primary + va_arg_i64(&ap);
        i = i + 1;
    }
    i = 0;
    while (i < n) {
        duplicate = duplicate + va_arg_i64(&copy);
        i = i + 1;
    }
    va_end(&ap);
    va_end(&copy);
    if (primary != duplicate) { return 100; }
    return primary;
}

export func sum_f64(n: i32, ...) -> f64 {
    var ap: VaList = uninit;
    va_start(&ap);
    var total: f64 = 0.0;
    var i: i32 = 0;
    while (i < n) {
        total = total + va_arg_f64(&ap);
        i = i + 1;
    }
    va_end(&ap);
    return total;
}

export func sum_ptr(n: i32, ...) -> i64 {
    var ap: VaList = uninit;
    va_start(&ap);
    var total: i64 = 0;
    var i: i32 = 0;
    while (i < n) {
        var p: *i64 = va_arg_ptr(&ap) as *i64;
        total = total + *p;
        i = i + 1;
    }
    va_end(&ap);
    return total;
}

extern func c_call_sum() -> i64;
extern func c_call_f64() -> i64;
extern func c_call_ptr() -> i64;

func _start() -> void {
    var bad: i32 = 0;
    if (c_call_sum() != 42) { bad = 1; }
    if (c_call_f64() != 7) { bad = 2; }
    if (c_call_ptr() != 42) { bad = 3; }
    asm {
        "mov rax, 60"
        "mov edi, {bad}"
        "syscall"
    }
}
"""

SUM_FAILURES = {
    1: "C caller to Cider va_copy/va_end integer sum",
    2: "C caller to Cider f64 sum",
    3: "C caller to Cider pointer sum",
}

# gcc has no libc here: the caller is an ordinary function the Cider `_start`
# calls, and the variadic call itself uses the normal SysV register/stack row.
C_CALLER = """#include <stdint.h>

extern int64_t sum(int n, ...);
extern double sum_f64(int n, ...);
extern int64_t sum_ptr(int n, ...);

int64_t c_call_sum(void) {
    /* Eight variadic words: five past the named register and three on the
       stack, so both callee paths are exercised. */
    return sum(8, 1, 2, 3, 4, 5, 6, 7, 14);
}

int64_t c_call_f64(void) {
    return (int64_t)sum_f64(3, 1.5, 2.25, 3.25);
}

int64_t c_call_ptr(void) {
    int64_t a = 20;
    int64_t b = 22;
    return sum_ptr(2, &a, &b);
}
"""

UEFI = """from std.varargs import VaList, va_start, va_arg_f64;

msabi func vsum(n: i32, ...) -> f64 {
    var ap: VaList = uninit;
    va_start(&ap);
    return va_arg_f64(&ap);
}

msabi func efi_main(image_handle: *void, system_table: *void) -> int {
    var p: *msabi func(i32, ...) -> f64 = &vsum;
    return p(1, 2.5) as int;
}
"""


def run(args, env=None, timeout=120):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=timeout)


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
    if shutil.which("gcc") is None:
        print("SKIP: gcc is unavailable")
        return 77
    if shutil.which("clang") is None:
        print("SKIP: clang is unavailable")
        return 77
    if not ld_has_i386pep():
        print("SKIP: ld -m i386pep is unavailable")
        return 77
    if shutil.which("as") is None:
        print("FAIL: as is unavailable", file=sys.stderr)
        return 1

    failures = []
    env = dict(os.environ)
    env["CIDER_LIB"] = LIB

    def fail(text):
        failures.append(text)

    def source(tmp, name, text):
        path = os.path.join(tmp, name)
        with open(path, "w") as handle:
            handle.write(text)
        return path

    def compile_c(path, obj):
        return run([
            "gcc", "-c", "-fno-stack-protector", "-o", obj, path,
        ])

    def run_program(path, labels):
        executed = run([path])
        if executed.returncode == 0:
            return
        if executed.returncode in labels:
            fail("native binary exited %r: %s" % (
                executed.returncode, labels[executed.returncode]))
        else:
            fail("native binary exited %r, not 0 (%s)" % (
                executed.returncode, describe(executed)))

    with tempfile.TemporaryDirectory(prefix="cider-v4-varargs-") as tmp:
        # --- Cider calls gcc ---------------------------------------------
        helper_obj = os.path.join(tmp, "varargs_helper.o")
        compiled = compile_c(HELPER, helper_obj)
        if compiled.returncode != 0:
            fail("gcc helper compile: " + describe(compiled))
        else:
            caller_src = source(tmp, "caller.cid", CALLER)
            caller_asm = os.path.join(tmp, "caller.s")
            caller_obj = os.path.join(tmp, "caller.o")
            caller_bin = os.path.join(tmp, "caller.bin")
            built = run([CIDER, "compile", caller_src, "-o", caller_asm],
                        env=env)
            if built.returncode != 0:
                fail("Cider caller compile: " + describe(built))
            else:
                text = open(caller_asm).read()
                if "mov al," not in text:
                    fail("Cider caller emitted no SysV al count")
                assembled = run(["as", "-o", caller_obj, caller_asm])
                if assembled.returncode != 0:
                    fail("Cider caller assemble: " + describe(assembled))
                else:
                    linked = run([
                        "ld", "-nostdlib", "-e", "_start", "-o", caller_bin,
                        caller_obj, helper_obj,
                    ])
                    if linked.returncode != 0:
                        fail("Cider-to-C link: " + describe(linked))
                    else:
                        run_program(caller_bin, CALLER_FAILURES)

        # --- gcc calls Cider ---------------------------------------------
        sum_src = source(tmp, "sum.cid", SUM)
        c_caller = source(tmp, "c_caller.c", C_CALLER)
        sum_obj = os.path.join(tmp, "sum.o")
        c_caller_obj = os.path.join(tmp, "c_caller.o")
        sum_bin = os.path.join(tmp, "sum.bin")
        # A split entry object keeps the exported definitions, while a plain
        # whole-program executable would prune them as unreachable: only C
        # can see these callers.
        built = run([
            CIDER, "compile", "-c", "--module", "main", sum_src,
            "-o", sum_obj,
        ], env=env)
        if built.returncode != 0:
            fail("Cider variadic definition compile: " + describe(built))
        compiled = compile_c(c_caller, c_caller_obj)
        if compiled.returncode != 0:
            fail("gcc caller compile: " + describe(compiled))
        if os.path.exists(sum_obj) and os.path.exists(c_caller_obj):
            linked = run([
                "ld", "-nostdlib", "-e", "_start", "-o", sum_bin,
                sum_obj, c_caller_obj,
            ])
            if linked.returncode != 0:
                fail("C-to-Cider link: " + describe(linked))
            else:
                run_program(sum_bin, SUM_FAILURES)

        # --- UEFI msabi variadic shape -----------------------------------
        uefi_src = source(tmp, "uefi.cid", UEFI)
        uefi_asm = os.path.join(tmp, "uefi.s")
        compiled = run([
            CIDER, "compile", "--target", "uefi", uefi_src, "-o", uefi_asm,
        ], env=env)
        if compiled.returncode != 0:
            fail("uefi variadic compile: " + describe(compiled))
        else:
            text = open(uefi_asm).read()
            for needle, what in (
                    ("mov qword ptr [rbp + 16], rcx",
                     "the shadow-space home for rcx"),
                    ("mov qword ptr [rbp + 24], rdx",
                     "the home for the first variadic slot"),
                    ("movsd xmm1,", "the variadic float in xmm1"),
                    ("movq rdx, xmm1",
                     "the variadic float duplicated into its GPR"),
                    ("lea rcx, [rbp + 24]",
                     "the cursor past the one named slot"),
                    ("call rax",
                     "the call through the variadic pointer")):
                if needle not in text:
                    fail("uefi variadic assembly lacks %s: %s" % (
                        what, needle))
            if "mov al," in text:
                fail("uefi variadic call emitted an al count")
            efi = os.path.join(tmp, "varargs.efi")
            linked = run([LINK_UEFI, uefi_asm, "-o", efi])
            if linked.returncode != 0:
                fail("uefi variadic link: " + describe(linked))
            else:
                problem = validate_pe32plus(efi)
                if problem is not None:
                    fail("uefi variadic image: " + problem)

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 varargs failure(s)" % len(failures), file=sys.stderr)
        return 1
    print("v4 varargs: native C interop both directions, UEFI msabi shape")
    return 0


if __name__ == "__main__":
    sys.exit(main())
