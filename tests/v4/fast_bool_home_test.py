#!/usr/bin/env python3
"""Fast boolean results materialise in their GPR homes.

A boolean result used to be built in ``al``, zero-extended through ``rax``,
and then copied into the allocator's home (``movzx rax, al`` ...
``mov H, rax``). The fast emitter now names the home in both steps, and a
``Not`` whose source already has a home tests that home directly instead of
staging it through ``rax``. Integer compares already wrote their homes
directly, so their leg is a regression guard.

The value legs run the same program on fast (optimised and unoptimised) so a
wrong source/destination alias or a stale flag shows up as a wrong exit code.
The legacy leg pins the old two-step rax sequence, which must stay
byte-identical.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func not_bool(x: bool) -> bool { return !x; }
func cmp_lt(x: i64) -> bool { return x < 10; }
func cmp_eq(x: i64, y: i64) -> bool { return x == y; }
func probe() -> i32 {
    if (not_bool(true) != false) { return 1; }
    if (not_bool(false) != true) { return 2; }
    if (cmp_lt(9) != true) { return 3; }
    if (cmp_lt(10) != false) { return 4; }
    if (cmp_eq(3, 3) != true) { return 5; }
    if (cmp_eq(3, 4) != false) { return 6; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

# The old two-step result: zero-extend in rax, then copy rax into a home.
RESULT_COPY = re.compile(r"(?m)^  mov r(?!ax\b)[a-z0-9]+, rax$")
# The old staging: the result is built in al and widened through rax.
RAX_STAGING = re.compile(r"(?m)^  movzx rax, al$")
# A flag byte in a home followed by that same home's zero extension.
SETCC_HOME = re.compile(
    r"(?m)^  set(?P<cc>\w+) (?P<byte>[a-z][a-z0-9]*)\n"
    r"\s+movzx (?P<wide>e?[a-z0-9]+), (?P=byte)$")
# `!x` tests a homed source in place and materialises the flag in its home.
NOT_HOME = re.compile(
    r"(?m)^  test (?P<src>r(?:ax|cx|dx|bx|si|di|8|9|1[0-5])), (?P=src)\n"
    r"\s+setz (?P<byte>[a-z][a-z0-9]*)\n"
    r"\s+movzx (?P<wide>e?[a-z0-9]+), (?P=byte)$")


def body(text, name):
    return backend_shape.function_body(text, name) or ""


def check_home(failures, segment, name, pattern, label):
    match = pattern.search(segment)
    if not match:
        failures.append("%s %s" % (name, label))
        return
    wide = match.group("wide")
    if wide in ("rax", "eax"):
        failures.append("%s still materialises its result in rax" % name)


def no_result_copy(failures, segment, name):
    if RESULT_COPY.search(segment):
        failures.append("%s still copies its result out of rax" % name)


def check_fast(text):
    failures = []

    not_bool = body(text, "not_bool")
    check_home(failures, not_bool, "not_bool", NOT_HOME,
               "does not test its home and extend the flag in place")
    if RAX_STAGING.search(not_bool):
        failures.append("not_bool still stages its flag through rax")
    no_result_copy(failures, not_bool, "not_bool")

    cmp_lt = body(text, "cmp_lt")
    check_home(failures, cmp_lt, "cmp_lt", SETCC_HOME,
               "does not zero-extend its home in place")
    no_result_copy(failures, cmp_lt, "cmp_lt")

    cmp_eq = body(text, "cmp_eq")
    check_home(failures, cmp_eq, "cmp_eq", SETCC_HOME,
               "does not zero-extend its home in place")
    no_result_copy(failures, cmp_eq, "cmp_eq")
    return failures


def check_legacy(text):
    """The default emitter keeps its two-step rax sequence."""
    failures = []
    not_bool = body(text, "not_bool")
    if not re.search(
            r"(?m)^  mov rax, r\w+\n"
            r"\s+test rax, rax\n"
            r"\s+setz al\n"
            r"\s+movzx rax, al\n"
            r"\s+mov r(?!ax\b)\w+, rax$", not_bool):
        failures.append("legacy not_bool lost its rax staging and result copy")

    cmp_lt = body(text, "cmp_lt")
    if not re.search(r"(?m)^  movzx rax, al\n"
                     r"\s+mov r(?!ax\b)\w+, rax$", cmp_lt):
        failures.append("legacy cmp_lt lost its rax result copy")
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
    run_leg(failures, "legacy", ("--codegen=legacy",))
    run_leg(failures, "legacy --no-opt", ("--codegen=legacy", "--no-opt"))
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast booleans: Not and compares write their homes directly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
