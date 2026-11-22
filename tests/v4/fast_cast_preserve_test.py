#!/usr/bin/env python3
"""Immediate folds must not use a cast's source value when the cast changes it.

Immediate folds read the constant behind a homed ``Cast`` through
``safeConstantValue``. That is only valid when the cast is value-preserving.
A narrowing cast (``300 as u8`` -> 44) and a signed-to-unsigned widening cast
(``-56 as u32`` -> 4294967240) are not, and must stay on the register path.
Otherwise a consumer can receive the source immediate and silently compute
the wrong value.

The runtime legs compare fast, fast --no-opt, legacy, and legacy --no-opt
against the expected exit status. The matrix covers the signed-negative
widening combinations that the old predicate accepted.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

C1 = """
func f(i: i64) -> i64 { var a: i64 = i * 2; return ((300 as u8) as i64) * a; }
func probe() -> i32 { if (f(1) != 88) { return 4; } return 0; }
func _start() -> void { var c: i32 = probe();
    asm { "mov rax, 60" "mov edi, {c}" "syscall" } }
"""

C1_FUZZ = """
func probe(x: u64, y: u64) -> int {
    var a: i32 = 300;
    var b: u8 = a as u8;
    var c: u64 = b as u64;
    var p: u64 = y + 1;
    var r: u64 = c + p;
    if (r + y + x == 64) { return 7; }
    return 9;
}
func _start() -> void {
    var fp: *func(u64, u64) -> int = &probe;
    var code: int = (*fp)(5, 7);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

C2 = """
func f(i: i64) -> i64 { var x: i8 = -56; return ((x as u32) as i64) + i; }
func probe() -> i32 { if (f(0) != 4294967240) { return 1; } return 0; }
func _start() -> void { var c: i32 = probe();
    asm { "mov rax, 60" "mov edi, {c}" "syscall" } }
"""

C2_FUZZ = """
func probe(x: int) -> int {
    var a: i8 = -1;
    var b: u16 = a as u16;
    if (b == 65535) { return 7 + x; }
    return 9 + x;
}
func _start() -> void {
    var fp: *func(int) -> int = &probe;
    var code: int = (*fp)(0);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

MATRIX = (
    ("i8", -56, "u16", 65480),
    ("i8", -56, "u32", 4294967240),
    ("i8", -128, "u16", 65408),
    ("i8", -1, "u16", 65535),
    ("i8", -1, "u32", 4294967295),
    ("i16", -30000, "u16", 35536),
    ("i16", -30000, "u32", 4294937296),
    ("i16", -1, "u16", 65535),
    ("i16", -1, "u32", 4294967295),
)

VARIANTS = (
    ("fast", ("--codegen=fast",)),
    ("fast --no-opt", ("--codegen=fast", "--no-opt")),
    ("legacy", ("--codegen=legacy",)),
    ("legacy --no-opt", ("--codegen=legacy", "--no-opt")),
)


def matrix_source(from_type, value, to_type, expected):
    return (
        "func f(i: i64) -> i64 { var x: %s = %s; "
        "return ((x as %s) as i64) + i; }\n"
        "func probe() -> i32 { if (f(0) != %s) { return 1; } return 0; }\n"
        "func _start() -> void { var c: i32 = probe();\n"
        "    asm { \"mov rax, 60\" \"mov edi, {c}\" \"syscall\" } }\n"
    ) % (from_type, value, to_type, expected)


def run_case(failures, label, source, expected, args):
    proc = backend_shape.run_source(source, args)
    if proc.returncode != expected:
        failures.append(
            "%s exited %d (expected %d; stdout=%r stderr=%r)" %
            (label, proc.returncode, expected,
             proc.stdout[-200:], proc.stderr[-200:]))


def main():
    failures = []
    cases = [
        ("C1 narrowing-cast multiply", C1, 0),
        ("C1 fuzz narrowing-cast reuse", C1_FUZZ, 7),
        ("C2 signed-to-unsigned widening", C2, 0),
        ("C2 fuzz signed-to-unsigned compare", C2_FUZZ, 7),
    ]
    for from_type, value, to_type, expected in MATRIX:
        cases.append((
            "matrix %s %d -> %s" % (from_type, value, to_type),
            matrix_source(from_type, value, to_type, expected), 0))

    for label, args in VARIANTS:
        for name, source, expected in cases:
            run_case(failures, "%s %s" % (label, name), source, expected, args)

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("cast immediate folds preserve narrowing and signedness")
    return 0


if __name__ == "__main__":
    sys.exit(main())
