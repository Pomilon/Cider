#!/usr/bin/env python3
"""A loop-carried value lowered from a phi must stay in one register home."""
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

# The plan's literal program called `loop_sum` directly, so the inliner cloned
# it into `_start` and removed the standalone body: `function_body` was always
# None before the allocator ran. Calling through a function pointer keeps the
# optimised body in the assembly and makes the runtime leg execute that same
# homed body instead of a stack-slot clone.
PROGRAM = """
func loop_sum(n: i64) -> i64 {
    var i: i64 = 0;
    var s: i64 = 0;
    while (i < n) {
        s = s + i;
        i = i + 1;
    }
    return s;
}
func _start() -> void {
    var p: *func(i64) -> i64 = &loop_sum;
    var code: i32 = (p(5) as i32);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

def main():
    text = backend_shape.compile_asm(PROGRAM, ("--codegen=fast",))
    body = backend_shape.function_body(text, "loop_sum")
    if body is None:
        print("FAIL: loop_sum body missing", file=sys.stderr)
        return 1
    if "[rsp" in body:
        print("FAIL: loop_sum still uses frame cells:\n" + body[:1200],
              file=sys.stderr)
        return 1
    proc = backend_shape.run_source(PROGRAM, ("--codegen=fast",))
    if proc.returncode != 10:
        print("FAIL: loop_sum(5) exit %d, expected 10" % proc.returncode,
              file=sys.stderr)
        return 1
    print("fast loop: loop-carried values are homed")
    return 0

if __name__ == "__main__":
    sys.exit(main())
