#!/usr/bin/env python3
"""Fast resident-parameter loads write the allocator's home directly.

A register-resident parameter used to widen into ``rax`` and then copy into
the result home (``mov H, rax``). The fast emitter now names the result home
in the extension itself, matching the destination-aware memory load. Every
width must extend exactly once, the source home may alias the result home,
and the legacy emitter must keep its slot-based two-step ``rax`` sequence.

The shape legs are the RED half: the old emitter writes ``eax``/``rax`` and
copies, the new one writes the home. The value legs run the same program on
fast (optimised and unoptimised) so a wrong extension or a clobbered source
shows up as a wrong exit code.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func read_i8(x: i8) -> i64 { return x as i64; }
func read_u8(x: u8) -> u64 { return x as u64; }
func read_bool(x: bool) -> u64 { return x as u64; }
func read_i16(x: i16) -> i64 { return x as i64; }
func read_u16(x: u16) -> u64 { return x as u64; }
func read_i32(x: i32) -> i64 { return x as i64; }
func read_u32(x: u32) -> u64 { return x as u64; }
func read_i64(x: i64) -> i64 { return x; }
func read_ptr(p: *u8) -> *u8 { return p; }

func arith_i8(x: i8) -> i64 {
    var r: i64 = x as i64;
    if (r < 0) { r = r + 100; }
    return r;
}
func arith_u8(x: u8) -> u64 {
    var r: u64 = x as u64;
    if (r > 7) { r = r + 3; }
    return r;
}
func arith_i16(x: i16) -> i64 {
    var r: i64 = x as i64;
    return r * 2;
}
func arith_u16(x: u16) -> u64 {
    var r: u64 = x as u64;
    return r + 3;
}
func arith_i32(x: i32) -> i64 {
    var r: i64 = x as i64;
    return r * 2;
}
func arith_u32(x: u32) -> u64 {
    var r: u64 = x as u64;
    return r + 1;
}
func arith_i64(x: i64, y: i64) -> i64 {
    var z: i64 = x;
    return z + y;
}
func ptr_eq(p: *u8, q: *u8) -> bool { return p == q; }
func cond_i64(x: i64) -> i64 {
    if (x > 0) { return x; }
    return 0 - x;
}
func use_ptr(p: *u8, n: i64) -> u8 { return p[n]; }

func probe() -> i32 {
    var b: [u8; 1] = [7];
    var p: *u8 = &b[0];
    if (read_i8((-5) as i8) != -5) { return 1; }
    if (read_u8(250 as u8) != 250) { return 2; }
    if (read_bool(true) != 1) { return 3; }
    if (read_i16((-30000) as i16) != -30000) { return 4; }
    if (read_u16(60000 as u16) != 60000) { return 5; }
    if (read_i32((-2000000000) as i32) != -2000000000) { return 6; }
    if (read_u32(4000000000 as u32) != 4000000000) { return 7; }
    if (read_i64((-9000000000000000000) as i64) != -9000000000000000000) { return 8; }
    if (read_ptr(p) != p) { return 9; }
    if (arith_i8((-5) as i8) != 95) { return 10; }
    if (arith_u8(9 as u8) != 12) { return 11; }
    if (arith_i16((-30000) as i16) != -60000) { return 12; }
    if (arith_u16(60000 as u16) != 60003) { return 13; }
    if (arith_i32((-7) as i32) != -14) { return 14; }
    if (arith_u32(4000000000 as u32) != 4000000001) { return 15; }
    if (arith_i64((-10) as i64, 3 as i64) != -7) { return 16; }
    if (!ptr_eq(p, p)) { return 17; }
    if (ptr_eq(p, &b[0]) != true) { return 18; }
    return 0;
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""


def body(text, name):
    return backend_shape.function_body(text, name) or ""


def expect(failures, segment, name, pattern, label):
    if not re.search(pattern, segment):
        failures.append("%s %s" % (name, label))


def forbid(failures, segment, name, pattern, label):
    if re.search(pattern, segment):
        failures.append("%s %s" % (name, label))


def no_rax_copy(failures, segment, name):
    """A homed resident load must not be followed by a copy out of rax."""
    forbid(failures, segment, name, r"(?m)^  mov r(?!ax\b)\w+, rax$",
           "still copies a resident load out of rax")


def check_fast(text):
    failures = []

    narrow = {
        "read_i8": (r"(?m)^  movsx (?!eax\b)r\w+, cl$",
                    r"movsx eax, cl|movsxd rax, eax",
                    "i8 load is not a direct 64-bit sign extension"),
        "read_u8": (r"(?m)^  movzx (?!eax\b)(?:e\w+|r\d+d), cl$",
                    r"movzx eax, cl|movsxd rax, eax",
                    "u8 load is not a direct zero extension"),
        "read_bool": (r"(?m)^  movzx (?!eax\b)(?:e\w+|r\d+d), cl$",
                      r"movzx eax, cl|movsxd rax, eax",
                      "bool load is not a direct zero extension"),
        "read_i16": (r"(?m)^  movsx (?!eax\b)r\w+, cx$",
                     r"movsx eax, cx|movsxd rax, eax",
                     "i16 load is not a direct 64-bit sign extension"),
        "read_u16": (r"(?m)^  movzx (?!eax\b)(?:e\w+|r\d+d), cx$",
                     r"movzx eax, cx|movsxd rax, eax",
                     "u16 load is not a direct zero extension"),
        "read_i32": (r"(?m)^  movsxd (?!rax\b)r\w+, ecx$",
                     r"mov r(?!ax\b)\w+, rax",
                     "i32 load is not a direct sign extension to its home"),
        "read_u32": (r"(?m)^  mov (?!eax\b)(?:e\w+|r\d+d), ecx$",
                     r"mov eax, ecx|mov r(?!ax\b)\w+, rax",
                     "u32 load is not a direct zero-extending home write"),
    }
    for name, (pattern, old, label) in narrow.items():
        segment = body(text, name)
        expect(failures, segment, name, pattern, label)
        forbid(failures, segment, name, old, "keeps the old rax widening")
        no_rax_copy(failures, segment, name)

    # The same widths used by compares and arithmetic.
    for name in ("arith_i8", "arith_u8", "arith_i16", "arith_u16",
                 "arith_i32", "arith_u32"):
        segment = body(text, name)
        no_rax_copy(failures, segment, name)
        if name == "arith_i8":
            expect(failures, segment, name,
                   r"(?m)^  movsx (?!eax\b)r\w+, cl$",
                   "i8 compare does not extend into a home")
        elif name == "arith_u8":
            expect(failures, segment, name,
                   r"(?m)^  movzx (?!eax\b)e\w+, cl$",
                   "u8 compare does not zero-extend into a home")
        elif name == "arith_i16":
            expect(failures, segment, name,
                   r"(?m)^  movsx (?!eax\b)r\w+, cx$",
                   "i16 arithmetic does not extend into a home")
        elif name == "arith_u16":
            expect(failures, segment, name,
                   r"(?m)^  movzx (?!eax\b)e\w+, cx$",
                   "u16 compare does not zero-extend into a home")
        elif name == "arith_i32":
            expect(failures, segment, name,
                   r"(?m)^  movsxd (?!rax\b)r\w+, ecx$",
                   "i32 arithmetic does not extend into a home")
        else:
            expect(failures, segment, name,
                   r"(?m)^  mov (?!eax\b)e\w+, ecx$",
                   "u32 arithmetic does not zero-extend into a home")

    # i64 and pointer loads are plain 64-bit moves. `cond_i64` and `use_ptr`
    # force a non-rax result home, so the old redundant rax preload is
    # visible as an extra move before the consuming instruction.
    arith_i64 = body(text, "arith_i64")
    expect(failures, arith_i64, "arith_i64",
           r"(?m)^  mov (?!rax\b)r\w+, rcx$",
           "i64 load does not write a non-rax home")
    no_rax_copy(failures, arith_i64, "arith_i64")

    ptr_eq = body(text, "ptr_eq")
    expect(failures, ptr_eq, "ptr_eq",
           r"(?m)^  mov (?!rax\b)r\w+, rcx$",
           "pointer load does not write a non-rax home")
    no_rax_copy(failures, ptr_eq, "ptr_eq")

    cond = body(text, "cond_i64")
    expect(failures, cond, "cond_i64", r"(?m)^  mov rdx, rsi$",
           "i64 compare does not keep the value in its result home")
    forbid(failures, cond, "cond_i64",
           r"(?m)^  mov rax, rsi\n  mov rdx, rsi$",
           "i64 compare still stages the value through rax")

    use = body(text, "use_ptr")
    expect(failures, use, "use_ptr", r"(?m)^  mov rsi, rdx$",
           "pointer arithmetic does not keep the base in its result home")
    forbid(failures, use, "use_ptr",
           r"(?m)^  mov rax, rcx\n  mov rsi, rdx$",
           "pointer arithmetic still stages the index through rax")

    return failures


def check_legacy(text):
    """The default emitter keeps its slot-based two-step rax sequence."""
    failures = []
    i8 = body(text, "read_i8")
    expect(failures, i8, "legacy read_i8",
           r"movsx eax, BYTE PTR \[rsp[^\n]*\n  movsxd rax, eax",
           "lost its two-step rax sequence")
    forbid(failures, i8, "legacy read_i8",
           r"movsx (?!eax\b)r\w+, cl",
           "writes a home instead of the parameter slot")

    u8 = body(text, "read_u8")
    expect(failures, u8, "legacy read_u8",
           r"movzx eax, BYTE PTR \[rsp[^\n]*\n  movsxd rax, eax",
           "lost its two-step rax sequence")
    forbid(failures, u8, "legacy read_u8",
           r"movzx (?!eax\b)e\w+, cl",
           "writes a home instead of the parameter slot")

    i32 = body(text, "read_i32")
    expect(failures, i32, "legacy read_i32",
           r"movsxd rax, DWORD PTR \[rsp", "lost its slot load")

    u32 = body(text, "read_u32")
    expect(failures, u32, "legacy read_u32",
           r"mov eax, DWORD PTR \[rsp", "lost its slot load")

    i64 = body(text, "read_i64")
    expect(failures, i64, "legacy read_i64",
           r"mov rax, \[rsp", "lost its slot load")

    ptr = body(text, "read_ptr")
    expect(failures, ptr, "legacy read_ptr",
           r"mov rax, \[rsp", "lost its slot load")
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
    legacy = backend_shape.compile_asm(PROGRAM,
                                       ("--codegen=legacy", "--no-opt"))
    failures.extend(check_legacy(legacy))
    run_leg(failures, "fast", ("--codegen=fast",))
    run_leg(failures, "fast --no-opt", ("--codegen=fast", "--no-opt"))
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast resident loads: home destinations, single extension, "
          "legacy unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
