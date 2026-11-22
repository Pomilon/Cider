#!/usr/bin/env python3
"""Fast selection must use memory destinations and immediate forms."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

# The plan's literal program leaves every function unreferenced, so whole-
# program pruning removes their bodies before the allocator runs. `_start`
# therefore calls each function through a pointer, the same technique
# `fast_loop_test` uses to keep an optimised standalone body in the assembly.
#
# The allocator also homes an arithmetic result, a constant and a loop-carried
# value whenever a register is free, which hides the memory-destination and
# immediate forms. `add_one` therefore feeds the add result and its literal
# count to a shift: shift operands are fixed-scratch values, so neither the
# result nor the shared constant receives a home while the loop still
# increments `x` by one. The store in `set_value` writes through a local
# pointer, whose destination operand is the Store scratch register rather than
# an `[rsp + N]` frame cell, so its assertion accepts either spelling.
PROGRAM = """
func add_one(n: i64) -> i64 {
    var x: i64 = 0;
    var sink: i64 = 0;
    while (x < n) {
        var t: i64 = x + 1;
        x = t;
        var keep: i64 = t << 1;
        sink = sink + keep;
    }
    return x + sink;
}
func set_value(n: i64) -> i64 {
    var x: i64 = n;
    var p: *i64 = &x;
    *p = 7;
    return x + n;
}
func shift(p: *i64) -> i64 {
    var x: i64 = p[0];
    return x << 8;
}
func _start() -> void {
    var a: *func(i64) -> i64 = &add_one;
    var s: *func(i64) -> i64 = &set_value;
    var h: *func(*i64) -> i64 = &shift;
    var x: i64 = 3;
    var code: i32 = (a(5) + s(3) + h(&x)) as i32;
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

def body(name):
    text = backend_shape.compile_asm(PROGRAM, ("--codegen=fast",))
    return backend_shape.function_body(text, name)

def main():
    failures = []
    add = body("add_one") or ""
    if not re.search(r"add qword ptr \[rsp[^\n]*, 1", add):
        failures.append("add_one has no immediate memory add")
    seven = body("set_value") or ""
    if not re.search(r"mov qword ptr \[(?:rsp|rcx)[^\n]*, 7", seven):
        failures.append("set_value has no immediate-to-memory store")
    sh = body("shift") or ""
    if not re.search(r"shl (?:qword ptr \[rsp[^\n]*, |r\w+, )8", sh):
        failures.append("shift has no immediate shift")
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    # The shape assertions can pass while the selected form computes the
    # wrong value, so pin the runtime result in both fast modes. The program's
    # `_start` exits with the sum of the three calls as i32.
    for name, args in (("fast", ("--codegen=fast",)),
                       ("fast-no-opt", ("--codegen=fast", "--no-opt"))):
        proc = backend_shape.run_source(PROGRAM, args)
        if proc.returncode != 45:
            print("FAIL: %s runtime exit %r; expected 45 (stderr %r)" %
                  (name, proc.returncode, proc.stderr[:300]), file=sys.stderr)
            return 1
    print("fast selection: memory/immediate forms present")
    return 0

if __name__ == "__main__":
    sys.exit(main())
