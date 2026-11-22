#!/usr/bin/env python3
"""Fast integer stores write the source home directly.

A fast Store used to materialise its value in ``rax`` (``mov rax, H``) and then
store ``al``/``ax``/``eax``/``rax``. The emitter now names the source home in
the store itself, so every width must still write exactly its own bytes and the
legacy emitter must keep its two-step ``rax`` sequence byte-for-byte.

The shape legs are the RED half: before this task a narrow store emitted the
rax preload followed by the store. The value legs run the same program on fast
(optimised and unoptimised) so a wrong width or an address/source register
conflict shows up as a wrong exit code.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func store_u8(x: u8) -> u8 {
    var a: [u8; 1] = [0];
    var y: u8 = (x + 1) as u8;
    a[0] = y;
    return a[0];
}
func store_i16(x: i16) -> i16 {
    var a: [i16; 1] = [0];
    var y: i16 = (x - 1) as i16;
    a[0] = y;
    return a[0];
}
func store_i32(x: i32) -> i32 {
    var a: [i32; 1] = [0];
    var y: i32 = x + 1;
    a[0] = y;
    return a[0];
}
func store_i64(x: i64) -> i64 {
    var a: [i64; 1] = [0];
    var y: i64 = x + 1;
    a[0] = y;
    return a[0];
}
func probe() -> i32 {
    if (store_u8(41) != 42) { return 1; }
    if (store_i16(42) != 41) { return 2; }
    if (store_i32(43) != 44) { return 3; }
    if (store_i64(44) != 45) { return 4; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

STORES = ("store_u8", "store_i16", "store_i32", "store_i64")

# The preload this task removes: `mov rax, rX` immediately followed by a store
# of rax's low byte/word/dword/qword.
PRELOAD_STORE = re.compile(
    r"mov rax, r\w+\n\s+mov \[[^\]]+\], (?:al|ax|eax|rax)")
# A store whose value is any register, not a specific home or address.
DIRECT_STORE = re.compile(r"mov \[[^\]]+\], [a-z][a-z0-9]*")


def body(text, name):
    return backend_shape.function_body(text, name) or ""


def check_fast(text):
    failures = []
    for name in STORES:
        segment = body(text, name)
        if not segment:
            failures.append("%s has no function body" % name)
            continue
        if PRELOAD_STORE.search(segment):
            failures.append("%s still preloads its store value into rax" % name)
        if not DIRECT_STORE.search(segment):
            failures.append("%s does not store from a direct register" % name)
    return failures


def check_legacy(text):
    """The default emitter keeps its two-step rax sequence."""
    failures = []
    segment = body(text, "store_i64")
    if not re.search(
            r"mov rax, r\w+\n\s+mov \[[^\]]+\], rax", segment):
        failures.append("legacy store_i64 lost its rax preload")
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
    # `--no-opt` keeps the small store functions from being inlined into
    # `probe`, so the legacy sequence can be inspected where it is emitted.
    legacy = backend_shape.compile_asm(PROGRAM, ("--codegen=legacy", "--no-opt"))
    failures.extend(check_legacy(legacy))
    run_leg(failures, "fast", ("--codegen=fast",))
    run_leg(failures, "fast --no-opt", ("--codegen=fast", "--no-opt"))
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast stores: direct source homes, exact widths, legacy unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
