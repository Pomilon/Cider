#!/usr/bin/env python3
"""Fast integer loads write the allocator's home directly.

A fast Load used to materialise in ``rax`` and then copy to the value's home
(``mov H, rax``). The emitter now names the home in the load itself, so every
width must still extend exactly once and the legacy emitter must keep its
two-step ``rax`` sequence byte-for-byte.

The shape legs are the RED half: before this task every load wrote ``eax`` or
``rax`` and the following ``storeResult`` emitted the copy. The value legs run
the same program on fast (optimised and unoptimised) so a wrong extension or a
destination that aliases the address register shows up as a wrong exit code.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func sum_u8(p: *u8, n: i64) -> i64 {
    var sum: i64 = 0;
    var i: i64 = 0;
    while (i < n) {
        sum = sum + (p[i] as i64);
        i = i + 1;
    }
    return sum;
}
func load_u8(p: *u8) -> u64 { return p[0] as u64; }
func load_i8(p: *i8) -> i64 { return p[0] as i64; }
func load_u16(p: *u16) -> u64 { return p[0] as u64; }
func load_i16(p: *i16) -> i64 { return p[0] as i64; }
func load_u32(p: *u32) -> u64 { return p[0] as u64; }
func load_i32(p: *i32) -> i64 { return p[0] as i64; }
func load_i64(p: *i64) -> i64 { return p[0]; }
func probe() -> i32 {
    var b: [u8; 4] = [1, 2, 3, 4];
    var i8s: [i8; 1] = [-1];
    var u16s: [u16; 1] = [2];
    var i16s: [i16; 2] = [-3, 4];
    var u: [u32; 2] = [4000000000, 1];
    var d: [i32; 2] = [-2000000000, 7];
    var q: [i64; 2] = [-9000000000000000000, 5];
    if (sum_u8(&b[0], 4) != 10) { return 1; }
    if (load_u8(&b[3]) != 4) { return 2; }
    if (load_i8(&i8s[0]) != -1) { return 3; }
    if (load_u16(&u16s[0]) != 2) { return 4; }
    if (load_i16(&i16s[0]) != -3) { return 5; }
    if (load_u32(&u[0]) != 4000000000) { return 6; }
    if (load_i32(&d[0]) != -2000000000) { return 7; }
    if (load_i64(&q[0]) != -9000000000000000000) { return 8; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""


def body(text, name):
    return backend_shape.function_body(text, name) or ""


def no_copy(failures, text, name):
    """A homed load must not be followed by a copy out of rax."""
    segment = body(text, name)
    if re.search(r"mov r\w+, rax", segment):
        failures.append("%s still copies its load out of rax" % name)


def check_fast(text):
    failures = []
    # The loop's byte load must name a home other than rax and must not be
    # followed by the old copy sequence.
    loop = body(text, "sum_u8")
    if not re.search(r"movzx (?!eax)[er]\w+, BYTE PTR", loop):
        failures.append("sum_u8 byte load does not write a non-rax home")
    if re.search(r"movzx eax, BYTE PTR[^\n]*\n  mov r\w+, rax", loop):
        failures.append("sum_u8 byte load is copied out of rax")

    u8 = body(text, "load_u8")
    if not re.search(r"movzx e\w+, BYTE PTR", u8):
        failures.append("u8 load is not a single zero-extending byte load")
    if re.search(r"movzx eax, BYTE PTR[^\n]*\n  mov r\w+, rax", u8):
        failures.append("u8 load is copied out of rax")
    if re.search(r"movsx|movsxd", u8):
        failures.append("u8 load uses a sign extension")
    no_copy(failures, text, "load_u8")

    i8 = body(text, "load_i8")
    if not re.search(r"movsx r\w+, BYTE PTR", i8):
        failures.append("signed i8 load is not a direct 64-bit extension")
    if re.search(r"movsxd", i8):
        failures.append("i8 load is extended twice")
    if re.search(r"movsx e\w+, BYTE PTR", i8):
        failures.append("i8 load uses a 32-bit signed destination")
    no_copy(failures, text, "load_i8")

    u16 = body(text, "load_u16")
    if not re.search(r"movzx e\w+, WORD PTR", u16):
        failures.append("u16 load is not a single zero-extending word load")
    if re.search(r"movsx|movsxd", u16):
        failures.append("u16 load uses a sign extension")
    no_copy(failures, text, "load_u16")

    i16 = body(text, "load_i16")
    if not re.search(r"movsx r\w+, WORD PTR", i16):
        failures.append("signed i16 load is not a direct 64-bit extension")
    if re.search(r"movsxd", i16):
        failures.append("i16 load is extended twice")
    if re.search(r"movsx e\w+, WORD PTR", i16):
        failures.append("i16 load uses a 32-bit signed destination")
    no_copy(failures, text, "load_i16")

    u32 = body(text, "load_u32")
    if not re.search(r"mov e\w+, DWORD PTR", u32):
        failures.append("u32 load is not a single zero-extending dword load")
    if re.search(r"movsxd", u32):
        failures.append("u32 load uses a sign extension")
    no_copy(failures, text, "load_u32")

    i32 = body(text, "load_i32")
    if not re.search(r"movsxd r\w+, DWORD PTR", i32):
        failures.append("i32 load is not a single sign-extending dword load")
    no_copy(failures, text, "load_i32")

    i64 = body(text, "load_i64")
    if not re.search(r"mov r\w+, (?:QWORD PTR )?\[", i64):
        failures.append("i64 load is not a 64-bit load")
    no_copy(failures, text, "load_i64")
    return failures


def check_legacy(text):
    """The default emitter keeps its two-step rax sequence."""
    failures = []
    u8 = body(text, "load_u8")
    if not re.search(r"movzx eax, BYTE PTR[^\n]*\n  movsxd rax, eax", u8):
        failures.append("legacy u8 load lost its two-step rax sequence")
    if re.search(r"movzx (?!eax)[er]\w+, BYTE PTR", u8):
        failures.append("legacy u8 load writes a home destination")
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
    # `--no-opt` keeps the small load functions from being inlined into
    # `probe`, so the legacy sequence can be inspected where it is emitted.
    legacy = backend_shape.compile_asm(PROGRAM, ("--codegen=legacy", "--no-opt"))
    failures.extend(check_legacy(legacy))
    run_leg(failures, "fast", ("--codegen=fast",))
    run_leg(failures, "fast --no-opt", ("--codegen=fast", "--no-opt"))
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast loads: home destinations, single extension, legacy unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
