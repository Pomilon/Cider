#!/usr/bin/env python3
"""Fast left-immediate add and multiply fold into lea / three-operand imul.

When a fast integer constant is the left operand and the allocator gives the
constant's home to the result (``mov D, imm; add D, R``), the constant move is
dead: the operation can name the immediate directly. ``add`` becomes
``lea D, [R + imm]`` and ``imul`` becomes ``imul D, R, imm``. The allocator
reuses the constant's home only when its live interval ends at the operation,
so eliding the move is safe.

The shape legs inspect fast ``--no-opt`` output; the runtime legs run the same
program on fast (optimised and unoptimised) so a wrong displacement, width, or
source/destination alias shows up as a wrong exit code. The legacy leg pins the
old ``mov`` + ``add``/``imul`` sequence, which must stay byte-identical.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func add_one(i: i64) -> i64 {
    var x: i64 = 1 + i;
    return x;
}
func mul48(i: i64) -> i64 {
    var x: i64 = 48 * i;
    return x;
}
func branch_after(i: i64) -> i32 {
    var x: i64 = 1 + i;
    if (x == 10) { return 7; }
    return 0;
}
func probe() -> i32 {
    if (add_one(41) != 42) { return 1; }
    if (mul48(3) != 144) { return 2; }
    if (branch_after(9) != 7) { return 3; }
    if (branch_after(10) != 0) { return 4; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

LEA_ONE = re.compile(r"lea r\w+, \[r\w+ \+ 1\]")
IMUL_IMM = re.compile(r"imul r\w+, r\w+, 48\b")
MOV_ONE = re.compile(r"mov r\w+, 1\b")
MOV_48 = re.compile(r"mov r\w+, 48\b")
COMPARE_BRANCH = re.compile(
    r"cmp r\w+, (?:r\w+|-?\d+)\n\s+j(?:ne|e) \.L_")


def body(text, name):
    return backend_shape.function_body(text, name) or ""


def check_fast(text):
    failures = []
    add_one = body(text, "add_one")
    if not LEA_ONE.search(add_one):
        failures.append("add_one does not fold 1 + i into lea")
    if MOV_ONE.search(add_one):
        failures.append("add_one still materialises 1 before the add")

    mul48 = body(text, "mul48")
    if not IMUL_IMM.search(mul48):
        failures.append("mul48 does not fold 48 * i into three-operand imul")
    if MOV_48.search(mul48):
        failures.append("mul48 still materialises 48 before the multiply")

    branch = body(text, "branch_after")
    if not LEA_ONE.search(branch):
        failures.append("branch_after does not fold 1 + i into lea")
    if not COMPARE_BRANCH.search(branch):
        failures.append("branch_after lost its own compare/branch after the fold")
    return failures


def check_legacy(text):
    """The default emitter keeps its rax materialisation and two-operand op."""
    failures = []
    add_one = body(text, "add_one")
    if re.search(r"\blea\s", add_one):
        failures.append("legacy add_one gained a lea")
    if not re.search(r"mov rcx, 1\n", add_one):
        failures.append("legacy add_one lost its constant materialisation")
    if not re.search(r"add rax,", add_one):
        failures.append("legacy add_one lost its rax add")

    mul48 = body(text, "mul48")
    if not re.search(r"mov rcx, 48\n", mul48):
        failures.append("legacy mul48 lost its constant materialisation")
    if not re.search(r"imul rax,", mul48):
        failures.append("legacy mul48 lost its two-operand imul")
    return failures


def run_leg(failures, label, args):
    proc = backend_shape.run_source(PROGRAM, args)
    if proc.returncode != 0:
        failures.append("%s exited %d (stdout=%r stderr=%r)" %
                        (label, proc.returncode,
                         proc.stdout[-200:], proc.stderr[-200:]))


def main():
    failures = []
    fast = backend_shape.compile_asm(PROGRAM, ("--codegen=fast", "--no-opt"))
    failures.extend(check_fast(fast))
    # `--no-opt` keeps the small functions from being inlined into `probe`, so
    # the legacy sequence can be inspected where it is emitted.
    legacy = backend_shape.compile_asm(PROGRAM, ("--codegen=legacy", "--no-opt"))
    failures.extend(check_legacy(legacy))
    run_leg(failures, "fast", ("--codegen=fast",))
    run_leg(failures, "fast --no-opt", ("--codegen=fast", "--no-opt"))
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast left immediates fold into lea and three-operand imul")
    return 0


if __name__ == "__main__":
    sys.exit(main())
