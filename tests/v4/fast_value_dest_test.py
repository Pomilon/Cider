#!/usr/bin/env python3
"""Fast integer constants, casts, and compares write their homes directly.

A fast integer cast used to materialise its source in ``rax``, extend there,
and copy the result into the allocator's home (``mov rax, H`` ...
``mov H, rax``). The emitter now names the destination home in the extension
itself. Constants and integer compares already wrote their homes directly, so
their legs are regression guards: a later change must not reintroduce the rax
round trip.

The value legs run the same program on fast (optimised and unoptimised) so a
wrong width, signedness, or a source/destination alias shows up as a wrong exit
code. The legacy leg pins the old two-step sequence, which must stay
byte-identical.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func const_value() -> i64 { return 123456789; }
func cast_u8(x: u64) -> u64 { return (x as u8) as u64; }
func cast_i16(x: i64) -> i64 { return (x as i16) as i64; }
func cmp_lt(x: i64) -> bool { return x < 10; }
func probe() -> i32 {
    if (const_value() != 123456789) { return 1; }
    if (cast_u8(0x1ff as u64) != 255) { return 2; }
    if (cast_i16(-2) != -2) { return 3; }
    if (cmp_lt(9) != true) { return 4; }
    if (cmp_lt(10) != false) { return 5; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

# The old two-step result: extend in rax, then copy rax into a home.
CAST_RESULT_COPY = re.compile(
    r"mov(?:zx|sx) rax, (?:al|ax)\n\s+mov r\w+, rax")
# A direct extension whose destination is not rax/eax.
DIRECT_EXTEND = re.compile(
    r"mov(?:zx|sx) (?P<dest>e?\w+), [a-z][a-z0-9]*")
# A compare's setcc followed by a zero-extension of that same byte.
SETCC_EXTEND = re.compile(
    r"set(?P<cc>\w+) (?P<byte>[a-z][a-z0-9]*)\n"
    r"\s+movzx (?P<wide>e?\w+), (?P=byte)")


def body(text, name):
    return backend_shape.function_body(text, name) or ""


def check_fast(text):
    failures = []
    const = body(text, "const_value")
    if not re.search(r"mov r\w+, 123456789", const):
        failures.append("const_value does not write 123456789 to a home")
    if "mov rax, 123456789" in const:
        failures.append("const_value still materialises 123456789 in rax")

    cast_u8 = body(text, "cast_u8")
    if CAST_RESULT_COPY.search(cast_u8):
        failures.append("cast_u8 still copies its result out of rax")
    match = DIRECT_EXTEND.search(cast_u8)
    if not match or match.group("dest") in ("rax", "eax"):
        failures.append("cast_u8 does not zero-extend into a home")

    cast_i16 = body(text, "cast_i16")
    if CAST_RESULT_COPY.search(cast_i16):
        failures.append("cast_i16 still copies its result out of rax")
    match = DIRECT_EXTEND.search(cast_i16)
    if not match or match.group("dest") in ("rax", "eax"):
        failures.append("cast_i16 does not sign-extend into a home")

    cmp_lt = body(text, "cmp_lt")
    if not SETCC_EXTEND.search(cmp_lt):
        failures.append("cmp_lt does not zero-extend its home in place")
    if re.search(r"movzx rax, al\n\s+mov r\w+, rax", cmp_lt):
        failures.append("cmp_lt still copies its result out of rax")
    return failures


def check_legacy(text):
    """The default emitter keeps its two-step rax sequence."""
    failures = []
    cast = body(text, "cast_u8")
    if not CAST_RESULT_COPY.search(cast):
        failures.append("legacy cast_u8 lost its rax result copy")
    cmp = body(text, "cmp_lt")
    if not re.search(r"movzx rax, al\n\s+mov r\w+, rax", cmp):
        failures.append("legacy cmp_lt lost its rax result copy")
    const = body(text, "const_value")
    if "mov rax, 123456789" in const:
        failures.append("legacy const_value materialised its constant in rax")
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
    print("fast values: constants, casts, and compares write homes directly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
