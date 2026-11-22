#!/usr/bin/env python3
"""Fast dead-immediate-home fold into the first reader.

A homed integer constant whose home is dead after its first reader does not
need the ``mov D, imm`` that materialises it. The consumer can name the
immediate directly: ``add r, 48``, ``imul r, r, 10``, ``and r, 63``,
``cmp r, 64``, and a copy or cast that only renames the constant. A constant
with a second reader must keep its home, so the move stays.

The shape legs inspect fast ``--no-opt`` output for the integer forms and
optimised output for the copy/cast forms, where locals have been promoted out
of their frame cells. The runtime legs run the same programs on fast
(optimised and unoptimised) so a wrong width, immediate, or source/destination
alias shows up as a wrong exit code. The legacy leg pins the old materialise-
then-operate sequence, which must stay byte-identical.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

SHAPES = """
func add48(x: i64) -> i64 { return x + 48; }
func mul10(x: i64) -> i64 { return x * 10; }
func and63(x: i64) -> i64 { return x & 63; }
func eq0(x: i64) -> bool { return x == 0; }
func lt64(x: i64) -> bool { return x < 64; }
func multi_use(x: i64) -> i64 {
    var c: i64 = 7;
    var a: i64 = x + c;
    var b: i64 = x - c;
    return a + b;
}
func narrow_cast(x: i64) -> i64 {
    var c: i32 = 300;
    var b: u8 = c as u8;
    return x + (b as i64);
}
func probe() -> i32 {
    if (add48(2) != 50) { return 1; }
    if (mul10(3) != 30) { return 2; }
    if (and63(70) != 6) { return 3; }
    if (eq0(0) != true) { return 4; }
    if (eq0(1) != false) { return 5; }
    if (lt64(63) != true) { return 6; }
    if (lt64(64) != false) { return 7; }
    if (multi_use(5) != 10) { return 8; }
    if (narrow_cast(0) != 44) { return 9; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

# A mutable global keeps the optimiser from folding the call results, and the
# intermediate `probe` keeps the two bodies out of `_start` (whose asm block
# disables promotion). Locals are therefore promoted and the copy/cast shapes
# survive for inspection.
COPY_CAST = """
var g: i64 = 5;
func copy_const(x: i64) -> i64 {
    var c: i64 = 9;
    return x + c;
}
func cast_add(x: i64) -> i64 {
    var c: i32 = 300;
    var w: i64 = c as i64;
    return x + w;
}
func probe() -> i32 {
    var x: i64 = g;
    if (copy_const(x) != x + (g + 4)) { return 1; }
    if (cast_add(x) != x + (g + 295)) { return 2; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

MULTI = """
var g: i64 = 5;
func multi_use(x: i64) -> i64 {
    var c: i64 = 7;
    return (x + c) * (x - c);
}
func probe() -> i32 {
    var x: i64 = g;
    if (multi_use(x) != -24) { return 1; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""


def body(text, name):
    return backend_shape.function_body(text, name) or ""


def check_fast_shapes(text):
    failures = []

    add = body(text, "add48")
    if not re.search(r"add r\w+, 48\b", add):
        failures.append("add48 does not fold 48 into the add")
    if re.search(r"mov r\w+, 48\b", add):
        failures.append("add48 still materialises 48")

    mul = body(text, "mul10")
    if not re.search(r"imul r\w+, r\w+, 10\b", mul):
        failures.append("mul10 does not fold 10 into the three-operand imul")
    if re.search(r"mov r\w+, 10\b", mul):
        failures.append("mul10 still materialises 10")

    band = body(text, "and63")
    if not re.search(r"and r\w+, 63\b", band):
        failures.append("and63 does not fold 63 into the and")
    if re.search(r"mov r\w+, 63\b", band):
        failures.append("and63 still materialises 63")

    eq = body(text, "eq0")
    if not re.search(r"(?:test r\w+, r\w+|cmp r\w+, 0)\n", eq):
        failures.append("eq0 lost its zero compare")
    if re.search(r"mov r\w+, 0\n", eq):
        failures.append("eq0 materialises zero")

    lt = body(text, "lt64")
    if not re.search(r"cmp r\w+, 64\b", lt):
        failures.append("lt64 does not fold 64 into the compare")
    if re.search(r"mov r\w+, 64\b", lt):
        failures.append("lt64 still materialises 64")

    multi = body(text, "multi_use")
    if not re.search(r"mov r\w+, 7\b", multi):
        failures.append("multi_use lost the constant move it still needs")
    return failures


def check_copy_cast(text):
    failures = []

    copy = body(text, "copy_const")
    if not re.search(r"add r\w+, 9\b", copy):
        failures.append("copy_const does not fold the copied constant")
    if re.search(r"mov r\w+, 9\b", copy):
        failures.append("copy_const still materialises 9")

    cast = body(text, "cast_add")
    if not re.search(r"add r\w+, 300\b", cast):
        failures.append("cast_add does not fold the cast constant")
    if re.search(r"movsxd|movzx|movsx", cast):
        failures.append("cast_add still emits its cast instruction")
    return failures


def check_multi_use(text):
    """A two-reader constant must keep its home in optimised output too."""
    failures = []
    multi = body(text, "multi_use")
    if not multi:
        failures.append("multi_use was inlined away; shape is not inspectable")
    elif not re.search(r"mov r\w+, 7\b", multi):
        failures.append("multi_use lost the home for its two-reader constant")
    return failures


def check_legacy(text):
    """The default emitter keeps its constant move and register operation."""
    failures = []
    cases = (
        ("add48", r"mov r\w+, 48\b", r"add rax, r\w+"),
        ("mul10", r"mov r\w+, 10\b", r"imul rax, r\w+"),
        ("and63", r"mov r\w+, 63\b", r"and rax, r\w+"),
        ("lt64", r"mov r\w+, 64\b", r"cmp rax, r\w+"),
    )
    for name, materialise, operation in cases:
        function = body(text, name)
        if not re.search(materialise, function):
            failures.append("legacy %s lost its constant materialisation" %
                            name)
        if not re.search(operation, function):
            failures.append("legacy %s lost its register operation" % name)
    return failures


def run_leg(failures, label, source, args):
    proc = backend_shape.run_source(source, args)
    if proc.returncode != 0:
        failures.append("%s exited %d (stdout=%r stderr=%r)" %
                        (label, proc.returncode,
                         proc.stdout[-200:], proc.stderr[-200:]))


def main():
    failures = []
    fast = backend_shape.compile_asm(
        SHAPES, ("--codegen=fast", "--no-opt"))
    failures.extend(check_fast_shapes(fast))

    copy_cast = backend_shape.compile_asm(COPY_CAST, ("--codegen=fast",))
    failures.extend(check_copy_cast(copy_cast))

    multi = backend_shape.compile_asm(MULTI, ("--codegen=fast",))
    failures.extend(check_multi_use(multi))

    legacy = backend_shape.compile_asm(
        SHAPES, ("--codegen=legacy", "--no-opt"))
    failures.extend(check_legacy(legacy))

    run_leg(failures, "fast", SHAPES, ("--codegen=fast",))
    run_leg(failures, "fast --no-opt", SHAPES,
            ("--codegen=fast", "--no-opt"))
    run_leg(failures, "copy/cast fast", COPY_CAST, ("--codegen=fast",))
    run_leg(failures, "copy/cast fast --no-opt", COPY_CAST,
            ("--codegen=fast", "--no-opt"))
    run_leg(failures, "multi-use fast", MULTI, ("--codegen=fast",))
    run_leg(failures, "multi-use fast --no-opt", MULTI,
            ("--codegen=fast", "--no-opt"))
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("dead immediate homes fold into fast consumers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
