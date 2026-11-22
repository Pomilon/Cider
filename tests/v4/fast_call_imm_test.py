#!/usr/bin/env python3
"""Fast dead-immediate-home fold into a call argument word.

A homed integer constant whose first reader is a call argument word does not
need the ``mov D, imm`` that materialises it. The argument staging move names
the immediate directly (``mov rsi, 7``) instead of copying the constant's
home, as long as the home is dead at the call, no later read occurs, and the
word stages through a GPR. A multi-use constant, a constant that is also an
address base, and a word past the register row keep their materialisation.

The shape legs inspect fast ``--no-opt`` output for the folded argument moves
and the two negative cases. The runtime legs run the same program on fast
(optimised and unoptimised) so a wrong immediate, width, or staging order
shows up as a wrong exit code. The legacy leg pins the old materialise-then-
copy sequence, which must stay byte-identical.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func opaque(a: i64, b: i64, c: i64) -> i64 { return a + b + c; }
func opaque_ptr(p: *u64, n: i64) -> i64 { return n; }
func sum8(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64,
          g: i64, h: i64) -> i64 {
    return a + b + c + d + e + f + g + h;
}

func call_int(x: i64) -> i64 {
    return opaque(x, 7, 9);
}
func stack_args(x: i64) -> i64 {
    return sum8(x, 7, 9, 11, 13, 15, 17, 19);
}
func call_ptr(x: i64) -> i64 {
    return opaque_ptr(4096 as *u64, x);
}
func multi_use(x: i64) -> i64 {
    var c: i64 = 5;
    return opaque(x, c, c);
}
func addr_base(x: i64) -> i64 {
    var p: *u64 = 0 as *u64;
    if (p != nilPtr) {
        return opaque_ptr(p, x) + (*p as i64);
    }
    return x;
}
func probe() -> i32 {
    if (call_int(1) != 17) { return 1; }
    if (call_ptr(3) != 3) { return 2; }
    if (multi_use(1) != 11) { return 3; }
    if (addr_base(4) != 4) { return 4; }
    if (stack_args(1) != 92) { return 5; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

# A register that is not one of the SysV argument registers. The allocator
# uses these as constant homes in the unoptimised shape.
HOME = r"r(?:bx|12|13|14|15)"


def body(text, name):
    return backend_shape.function_body(text, name) or ""


def check_fast(text):
    failures = []

    call_int = body(text, "call_int")
    if not re.search(r"mov rsi, 7\b", call_int):
        failures.append("call_int does not stage 7 as an immediate argument")
    if not re.search(r"mov rdx, 9\b", call_int):
        failures.append("call_int does not stage 9 as an immediate argument")
    if re.search(r"mov %s, 7\b" % HOME, call_int):
        failures.append("call_int still materialises 7")
    if re.search(r"mov %s, 9\b" % HOME, call_int):
        failures.append("call_int still materialises 9")
    if re.search(r"mov r(?:si|dx), %s\b" % HOME, call_int):
        failures.append("call_int still copies a constant home")

    call_ptr = body(text, "call_ptr")
    if not re.search(r"mov rdi, 4096\b", call_ptr):
        failures.append("call_ptr does not stage the pointer constant")
    if re.search(r"mov %s, 4096\b" % HOME, call_ptr):
        failures.append("call_ptr still materialises 4096")

    stack = body(text, "stack_args")
    for reg, value in (("rsi", "7"), ("rdx", "9"), ("rcx", "11"),
                       ("r8", "13")):
        if not re.search(r"mov %s, %s\b" % (reg, value), stack):
            failures.append(
                "stack_args does not stage %s as an immediate argument" %
                value)
    for value in ("17", "19"):
        if not re.search(r"mov r\w+, %s\b" % value, stack):
            failures.append(
                "stack_args lost the materialisation of stack word %s" %
                value)

    multi = body(text, "multi_use")
    if not re.search(r"mov r\w+, 5\b", multi):
        failures.append("multi_use lost the constant move it still needs")
    if not re.search(r"mov r(?:si|dx), r\w+\b", multi):
        failures.append("multi_use staged an immediate for a two-use constant")

    addr = body(text, "addr_base")
    if not re.search(r"mov r\w+, 0\b", addr):
        failures.append("addr_base lost the constant it uses as an address")
    if not re.search(r"mov rdi, r\w+\b", addr):
        failures.append("addr_base staged an immediate for an address base")
    if re.search(r"mov rdi, 0\b", addr):
        failures.append("addr_base folded a constant that is an address base")
    if not re.search(r"mov r\w+, \[r\w+\]", addr):
        failures.append("addr_base lost its address-base load")
    return failures


def check_legacy(text):
    """The default emitter keeps its materialisation and slot copy."""
    failures = []

    call_int = body(text, "call_int")
    if not re.search(r"mov rax, 7\b", call_int):
        failures.append("legacy call_int lost its 7 materialisation")
    if not re.search(r"mov rax, 9\b", call_int):
        failures.append("legacy call_int lost its 9 materialisation")
    if not re.search(r"mov rsi, \[rsp \+ \d+\]", call_int):
        failures.append("legacy call_int lost its slot copy for 7")
    if not re.search(r"mov rdx, \[rsp \+ \d+\]", call_int):
        failures.append("legacy call_int lost its slot copy for 9")
    if re.search(r"mov rsi, 7\b", call_int):
        failures.append("legacy call_int gained an immediate argument")
    if re.search(r"mov rdx, 9\b", call_int):
        failures.append("legacy call_int gained an immediate argument")

    call_ptr = body(text, "call_ptr")
    if not re.search(r"mov r(?:ax|cx), 4096\b", call_ptr):
        failures.append("legacy call_ptr lost its 4096 materialisation")
    if re.search(r"mov rdi, 4096\b", call_ptr):
        failures.append("legacy call_ptr gained an immediate argument")
    return failures


def run_leg(failures, label, args):
    proc = backend_shape.run_source(PROGRAM, args)
    if proc.returncode != 0:
        failures.append("%s exited %d (stdout=%r stderr=%r)" %
                        (label, proc.returncode,
                         proc.stdout[-200:], proc.stderr[-200:]))


def main():
    failures = []
    fast = backend_shape.compile_asm(
        PROGRAM, ("--codegen=fast", "--no-opt"))
    failures.extend(check_fast(fast))

    legacy = backend_shape.compile_asm(
        PROGRAM, ("--codegen=legacy", "--no-opt"))
    failures.extend(check_legacy(legacy))

    run_leg(failures, "fast", ("--codegen=fast",))
    run_leg(failures, "fast --no-opt", ("--codegen=fast", "--no-opt"))
    run_leg(failures, "legacy --no-opt",
            ("--codegen=legacy", "--no-opt"))
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("dead immediate homes fold into fast call arguments")
    return 0


if __name__ == "__main__":
    sys.exit(main())
