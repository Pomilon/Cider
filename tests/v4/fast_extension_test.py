#!/usr/bin/env python3
"""Narrow loads and fast binary results must extend once, not twice."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func widen_u8(p: *u8) -> i64 {
    var x: u8 = p[0];
    return x as i64;
}
func widen_i8(p: *i8) -> i64 {
    var x: i8 = p[0];
    return x as i64;
}
func widen_u16(p: *u16) -> i64 {
    var x: u16 = p[0];
    return x as i64;
}
func widen_i16(p: *i16) -> i64 {
    var x: i16 = p[0];
    return x as i64;
}
func add_i8(a: i8, b: i8) -> i8 { return a + b; }
func add_u8(a: u8, b: u8) -> u8 { return a + b; }
func add_i16(a: i16, b: i16) -> i16 { return a + b; }
func add_u16(a: u16, b: u16) -> u16 { return a + b; }
func _start() -> void {
    asm { "mov rax, 60" "mov edi, 0" "syscall" }
}
"""

def check_narrow_result(failures, text, name, form):
    """The arithmetic result is canonicalized once, right after the add.

    Resident narrow parameters still take the two-step register load (out of
    this task's scope), so only the instruction sequence after the add is
    inspected.
    """
    body = backend_shape.function_body(text, name) or ""
    match = re.search(r"add rcx, rdx\n((?:  [^\n]+\n)*)", body)
    if match is None:
        failures.append("%s has no `add rcx, rdx` result" % name)
        return
    tail = match.group(1)
    if not re.search(r"^  %s$" % re.escape(form), tail, re.M):
        failures.append("%s result is not canonicalized with `%s`" % (name, form))
    if re.search(r"^  movsxd ", tail, re.M):
        failures.append("%s result is extended twice" % name)
    if form.startswith("movsx") and re.search(r"^  movsx e(?:ax|cx),", tail, re.M):
        failures.append("%s result uses a 32-bit signed destination" % name)

def check(text):
    failures = []
    u = backend_shape.function_body(text, "widen_u8") or ""
    if re.search(r"movzx e\w+, BYTE PTR[^\n]*\n  movsxd", u):
        failures.append("u8 load is extended twice")
    if not re.search(r"movzx e\w+, BYTE PTR", u):
        failures.append("u8 load is not a single zero-extending byte load")
    if re.search(r"movsx|movsxd", u):
        failures.append("u8 load uses a sign extension")
    i = backend_shape.function_body(text, "widen_i8") or ""
    if re.search(r"movsx r\w+, BYTE PTR[^\n]*\n  movsxd", i):
        failures.append("i8 load is extended twice")
    if not re.search(r"movsx r\w+, BYTE PTR", i):
        failures.append("signed i8 load is not a direct 64-bit extension")
    if re.search(r"movsx e\w+, BYTE PTR", i):
        failures.append("i8 load uses a 32-bit signed destination")
    u16 = backend_shape.function_body(text, "widen_u16") or ""
    if re.search(r"movzx e\w+, WORD PTR[^\n]*\n  movsxd", u16):
        failures.append("u16 load is extended twice")
    if not re.search(r"movzx e\w+, WORD PTR", u16):
        failures.append("u16 load is not a single zero-extending word load")
    if re.search(r"movsx|movsxd", u16):
        failures.append("u16 load uses a sign extension")
    i16 = backend_shape.function_body(text, "widen_i16") or ""
    if re.search(r"movsx r\w+, WORD PTR[^\n]*\n  movsxd", i16):
        failures.append("i16 load is extended twice")
    if not re.search(r"movsx r\w+, WORD PTR", i16):
        failures.append("signed i16 load is not a direct 64-bit extension")
    if re.search(r"movsx e\w+, WORD PTR", i16):
        failures.append("i16 load uses a 32-bit signed destination")
    check_narrow_result(failures, text, "add_i8", "movsx rcx, cl")
    check_narrow_result(failures, text, "add_u8", "movzx ecx, cl")
    check_narrow_result(failures, text, "add_i16", "movsx rcx, cx")
    check_narrow_result(failures, text, "add_u16", "movzx rcx, cx")
    return failures

def main():
    text = backend_shape.compile_asm(PROGRAM, ("--codegen=fast", "--no-opt"))
    failures = check(text)
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast extensions: narrow loads and results extend once")
    return 0

if __name__ == "__main__":
    sys.exit(main())
