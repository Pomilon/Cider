#!/usr/bin/env python3
"""Fast dead callee-save pairs are dropped after immediate folds.

A callee-saved register whose only remaining body mention is a dead
``mov reg, imm`` cannot be observed by its caller, so the prologue save, every
epilogue restore, and the dead write are all removable. A register that is
read in the body keeps its pair, and a function containing inline asm keeps
every pair because an implicit clobber (``cpuid``'s rbx, ``xlat``'s rbx base)
is invisible in the emitted text.

The shapes cover a single-return local epilogue, three per-return epilogues,
a shared four-return epilogue, a live home across a call, and an asm-bearing
function. The runtime legs execute the same program on fast (optimised and
unoptimised). Legacy keeps its original per-return pair shape.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
var g: i64 = 5;

func write_u16(buf: *u8, pos: int, val: u16) -> int {
    buf[pos]     = ((val >> 8) & 0xFF as u16) as u8;
    buf[pos + 1] = (val & 0xFF as u16) as u8;
    return pos + 2;
}

func pick3(buf: *u8, pos: int, val: u16, k: int) -> int {
    if (k == 0) { return 10; }
    if (k == 1) { return 20; }
    buf[pos]     = ((val >> 8) & 0xFF as u16) as u8;
    buf[pos + 1] = (val & 0xFF as u16) as u8;
    return pos + 2;
}

func multi(buf: *u8, pos: int, val: u16, k: int) -> int {
    if (k == 0) { return 10; }
    if (k == 1) { return 20; }
    if (k == 2) { return 30; }
    buf[pos]     = ((val >> 8) & 0xFF as u16) as u8;
    buf[pos + 1] = (val & 0xFF as u16) as u8;
    return pos + 2;
}

func asm_keep(buf: *u8, pos: int, val: u16, k: int) -> int {
    if (k == 0) { return 10; }
    if (k == 1) { return 20; }
    buf[pos]     = ((val >> 8) & 0xFF as u16) as u8;
    buf[pos + 1] = (val & 0xFF as u16) as u8;
    asm { "cpuid" }
    return pos + 2;
}

func one(x: i64) -> i64 { return x + 1; }
func two(x: i64) -> i64 { return x + 2; }

func probe() -> i32 {
    var b: [u8;8] = [0, 0, 0, 0, 0, 0, 0, 0];
    var p: *u8 = &b[0];
    if (write_u16(p, 0, 300) != 2) { return 1; }
    if (pick3(p, 0, 300, 5) != 2) { return 2; }
    if (pick3(p, 0, 300, 0) != 10) { return 3; }
    if (multi(p, 0, 300, 5) != 2) { return 4; }
    if (multi(p, 0, 300, 1) != 20) { return 5; }
    // Keep the asm body in the module without executing its implicit clobber
    // in this runtime leg; `g` is mutable, so the branch is not folded away.
    if (g == 123456789) { if (asm_keep(p, 0, 300, 5) != 2) { return 6; } }
    if (one(g) + two(0) != g + 3) { return 7; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

# The first call's result must survive the second call, so the allocator homes
# it in a callee-saved register and the emitter must keep that pair.
ACROSS = """
func one(x: i64) -> i64 { return x + 1; }
func two(x: i64) -> i64 { return x + 2; }
func _start() -> void {
    var code: i64 = one(40) + two(0);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""


def body(text, name):
    found = backend_shape.function_body(text, name)
    return found or ""


def save(body_text, reg):
    return re.search(
        r"(?m)^  mov qword ptr \[rsp \+ \d+\], %s$" % reg, body_text)


def restores(body_text, reg):
    return re.findall(
        r"(?m)^  mov %s, qword ptr \[rsp \+ \d+\]$" % reg, body_text)


def check_fast(text):
    failures = []

    # Single return, local epilogue: the null guard now tests the pristine
    # base pointer (buf), so the register holding it is genuinely LIVE across
    # the guard and its save/restore pair must stay. The live r12 home stays
    # too. dropDeadCalleeSaves is still pinned by asm_keep's dead immediate.
    write = body(text, "write_u16")
    if not save(write, "rbx"):
        failures.append("write_u16 dropped the live rbx save (base is guarded)")
    if not restores(write, "rbx"):
        failures.append("write_u16 dropped the live rbx restore (base is guarded)")
    if not save(write, "r12"):
        failures.append("write_u16 dropped the live r12 save")
    if not restores(write, "r12"):
        failures.append("write_u16 dropped the live r12 restore")

    # Three returns, per-return epilogues: the guarded base pointer makes r12
    # live, so its save and restores stay alongside the rbx/r13 pairs.
    pick = body(text, "pick3")
    if not save(pick, "r12"):
        failures.append("pick3 dropped the live r12 save (base is guarded)")
    if not restores(pick, "r12"):
        failures.append("pick3 dropped the live r12 restore (base is guarded)")
    if not save(pick, "rbx") or not save(pick, "r13"):
        failures.append("pick3 dropped a live callee-saved pair")
    if backend_shape.count(r"(?m)^  ret$", pick) != 3:
        failures.append("pick3 no longer has three local epilogues")

    # Four returns, shared epilogue: the guarded base pointer keeps r13 live,
    # so its pair survives in the one shared restore run.
    shared = body(text, "multi")
    if not save(shared, "r13"):
        failures.append("multi dropped the live r13 save (base is guarded)")
    if not restores(shared, "r13"):
        failures.append("multi dropped the live r13 restore (base is guarded)")
    if ".L_multi_epilogue:" not in shared:
        failures.append("multi lost its shared epilogue")
    if backend_shape.count(r"(?m)^  ret$", shared) != 1:
        failures.append("multi no longer has one shared epilogue")

    # Inline asm: even a dead-looking pair must survive because cpuid clobbers
    # rbx implicitly and the text cannot show it.
    asm = body(text, "asm_keep")
    if not save(asm, "r13"):
        failures.append("asm_keep dropped r13 despite inline asm")
    if not restores(asm, "r13"):
        failures.append("asm_keep dropped the r13 restore despite inline asm")
    if not re.search(r"(?m)^  mov r13, 0$", asm):
        failures.append("asm_keep lost its dead immediate write")
    if not re.search(r"(?m)^cpuid$", asm):
        failures.append("asm_keep lost its inline asm")

    return failures


def check_across(text):
    failures = []
    start = body(text, "_start")
    saved = re.search(
        r"(?m)^  mov qword ptr \[rsp \+ \d+\], (r(?:bx|12|13|14|15))$",
        start)
    if saved is None:
        failures.append("across_call no longer saves a callee-saved home")
    else:
        reg = saved.group(1)
        if not restores(start, reg):
            failures.append("across_call dropped the %s restore" % reg)
        if not re.search(r"(?m)^  (?:lea|add|mov) %s," % reg, start):
            failures.append("across_call lost the live %s body use" % reg)
    return failures


def check_legacy(text):
    failures = []
    pick = body(text, "pick3")
    if not save(pick, "rbx"):
        failures.append("legacy pick3 lost its rbx save")
    if len(restores(pick, "rbx")) != 3:
        failures.append("legacy pick3 changed its per-return rbx restores")
    if backend_shape.count(r"(?m)^  ret$", pick) != 3:
        failures.append("legacy pick3 changed its per-return epilogues")
    if ".L_pick3_epilogue:" in pick:
        failures.append("legacy pick3 gained a shared epilogue")
    return failures


def run_leg(failures, label, args):
    proc = backend_shape.run_source(PROGRAM, args)
    if proc.returncode != 0:
        failures.append("%s exited %d (stderr=%r)" %
                        (label, proc.returncode, proc.stderr[-300:]))


def main():
    failures = []
    fast = backend_shape.compile_asm(PROGRAM, ("--codegen=fast",))
    failures.extend(check_fast(fast))

    across = backend_shape.compile_asm(ACROSS, ("--codegen=fast",))
    failures.extend(check_across(across))

    legacy = backend_shape.compile_asm(
        PROGRAM, ("--codegen=legacy",))
    failures.extend(check_legacy(legacy))

    run_leg(failures, "fast", ("--codegen=fast",))
    run_leg(failures, "fast --no-opt", ("--codegen=fast", "--no-opt"))

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast dead callee-save pairs are removed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
