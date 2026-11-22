#!/usr/bin/env python3
"""Residual audit coverage: narrow callee-save mentions, shared-epilogue
fallthrough, and resident-home aliasing.

The dead-callee-save pass must see a live mention through any GP width.  The
`k_memcpy` function in the hash-map benchmark uses `r12d`/`r12b` while the
frame also saves and restores `r12`; if the scanner misses narrow spellings,
the pass treats the pair as dead and removes it.

A shared epilogue may only be entered by falling through from a `Ret`.  The
`spin` function has six returns, so fast codegen shares the epilogue, but its
last block is a loop back-edge and must end in an explicit jump.

Resident parameter loads may write the result back into the parameter's own
home.  Each `load_*` function below loads through a pointer whose home is
`rcx` and writes the loaded value into `rcx`/`ecx` directly.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

REPO = backend_shape.REPO
HASH_MAP = os.path.join(REPO, "bench", "compiler", "kernels", "hash_map",
                        "kernel.cid")

EPILOGUE = """
func spin(x: i32) -> i32 {
    if (x == 0) { return 1; }
    if (x == 1) { return 2; }
    if (x == 2) { return 3; }
    if (x == 3) { return 4; }
    while (x >= 0) {
        if (x == 10) { return 5; }
        x = x + 1;
    }
    return 6;
}
func probe() -> i32 {
    if (spin(0) != 1) { return 1; }
    if (spin(1) != 2) { return 2; }
    if (spin(2) != 3) { return 3; }
    if (spin(3) != 4) { return 4; }
    if (spin(10) != 5) { return 5; }
    return 0;
}
func _start() -> void { var c: i32 = probe();
    asm { "mov rax, 60" "mov edi, {c}" "syscall" } }
"""

ALIAS = """
func load_i8(p: *i8) -> i8 { return *p; }
func load_u8(p: *u8) -> u8 { return *p; }
func load_bool(p: *bool) -> bool { return *p; }
func load_i16(p: *i16) -> i16 { return *p; }
func load_u16(p: *u16) -> u16 { return *p; }
func load_i32(p: *i32) -> i32 { return *p; }
func load_u32(p: *u32) -> u32 { return *p; }
func load_i64(p: *i64) -> i64 { return *p; }
func load_ptr(p: **i64) -> *i64 { return *p; }

func probe() -> i32 {
    var a: i8 = -5;
    var b: u8 = 250;
    var c: bool = true;
    var d: i16 = -1234;
    var e: u16 = 60000;
    var f: i32 = -123456;
    var g: u32 = 4000000000;
    var h: i64 = -1234567890123;
    var q: i64 = 77;
    var qp: *i64 = &q;
    if (load_i8(&a) != -5) { return 1; }
    if (load_u8(&b) != 250) { return 2; }
    if (load_bool(&c) != true) { return 3; }
    if (load_i16(&d) != -1234) { return 4; }
    if (load_u16(&e) != 60000) { return 5; }
    if (load_i32(&f) != -123456) { return 6; }
    if (load_u32(&g) != 4000000000) { return 7; }
    if (load_i64(&h) != -1234567890123) { return 8; }
    if (load_ptr(&qp) != &q) { return 9; }
    return 0;
}
func _start() -> void { var c: i32 = probe();
    asm { "mov rax, 60" "mov edi, {c}" "syscall" } }
"""

VARIANTS = (
    ("fast", ("--codegen=fast",)),
    ("fast --no-opt", ("--codegen=fast", "--no-opt")),
    ("legacy", ("--codegen=legacy",)),
    ("legacy --no-opt", ("--codegen=legacy", "--no-opt")),
)


def fail(failures, message):
    failures.append(message)


def check_narrow_save(failures):
    if not os.path.isfile(HASH_MAP):
        fail(failures, "missing hash-map kernel: %s" % HASH_MAP)
        return
    with open(HASH_MAP, "r", encoding="utf-8") as handle:
        source = handle.read()
    asm = backend_shape.compile_asm(source, ("--codegen=fast",))
    body = backend_shape.function_body(asm, "k_memcpy")
    if body is None:
        fail(failures, "k_memcpy not found in optimized hash-map assembly")
        return
    if not re.search(r"mov qword ptr \[rsp \+ \d+\], r12", body):
        fail(failures, "k_memcpy lost its r12 save despite live narrow uses")
    if not re.search(r"mov r12, qword ptr \[rsp \+ \d+\]", body):
        fail(failures, "k_memcpy lost its r12 restore despite live narrow uses")
    if not re.search(r"\br12[bwd]\b", body):
        fail(failures, "k_memcpy no longer has the expected narrow r12 spelling")


def check_shared_epilogue(failures):
    asm = backend_shape.compile_asm(EPILOGUE, ("--codegen=fast",))
    body = backend_shape.function_body(asm, "spin")
    if body is None:
        fail(failures, "spin not found in fast assembly")
        return
    label = ".L_spin_epilogue:"
    if body.count(label) != 1:
        fail(failures, "spin should have exactly one shared epilogue label")
        return
    lines = body.splitlines()
    index = next(i for i, line in enumerate(lines) if line.strip() == label)
    previous = ""
    for line in reversed(lines[:index]):
        if line.strip():
            previous = line.strip()
            break
    if not re.match(r"jmp\b", previous):
        fail(failures,
             "last block before shared epilogue is not an explicit jump: %r" %
             previous)
    if len(re.findall(r"\bret\b", body)) != 1:
        fail(failures, "shared epilogue should contain exactly one ret")


def check_resident_alias(failures):
    asm = backend_shape.compile_asm(ALIAS, ("--codegen=fast", "--no-opt"))
    functions = (
        "load_i8", "load_u8", "load_bool", "load_i16", "load_u16",
        "load_i32", "load_u32", "load_i64", "load_ptr",
    )
    for name in functions:
        body = backend_shape.function_body(asm, name)
        if body is None:
            fail(failures, "%s not found in fast --no-opt assembly" % name)
            continue
        if not re.search(r"mov rcx, rdi", body):
            fail(failures, "%s does not keep the pointer in rcx" % name)
        if not re.search(
                r"mov(?:sx|zx|sxd)?\s+(?:rcx|ecx), (?:[A-Z]+ PTR )?\[rax\]",
                body):
            fail(failures,
                 "%s does not load directly into the parameter home" % name)
        if re.search(r"mov rcx, rax", body):
            fail(failures,
                 "%s copies the load result through rax" % name)


def run_program(failures, label, source, args, expected=0):
    proc = backend_shape.run_source(source, args)
    if proc.returncode != expected:
        fail(failures,
             "%s exited %d (expected %d; stderr=%r)" %
             (label, proc.returncode, expected, proc.stderr[-200:]))


def main():
    failures = []
    check_narrow_save(failures)
    check_shared_epilogue(failures)
    check_resident_alias(failures)
    for label, args in VARIANTS:
        run_program(failures, "%s alias" % label, ALIAS, args)
        run_program(failures, "%s epilogue" % label, EPILOGUE, args)
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("residual audit coverage passes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
