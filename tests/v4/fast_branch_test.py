#!/usr/bin/env python3
"""A branch to the next block must not emit a redundant jump.

Three conditional shapes are covered: the then arm is the next block
(`choose`), the else arm is the next block (`empty_then`), and both arms are
the next block (`both`, where no branch is needed at all).
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func choose(x: i64) -> i64 {
    if (x > 0) {
        return 1;
    } else {
        return 2;
    }
}
func empty_then(x: i64) -> i64 {
    var r: i64 = 0;
    if (x > 0) { } else { r = 2; }
    return r;
}
func both(x: i64) -> i64 {
    var r: i64 = 3;
    if (x > 0) { } else { }
    return r;
}
func _start() -> void {
    var a: *func(i64) -> i64 = &choose;
    var b: *func(i64) -> i64 = &empty_then;
    var c: *func(i64) -> i64 = &both;
    var code: i32 = (a(1) + b(0) + c(1)) as i32;
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

# Function name and whether a conditional branch must survive.
SHAPES = (
    ("choose", True),
    ("empty_then", True),
    ("both", False),
)

BRANCH = re.compile(r"(?m)^  (j[a-z]+) (\.L_[A-Za-z0-9_]+)$")
LABEL = re.compile(r"(?m)^(\.L_[A-Za-z0-9_]+):")


def check_body(text, name, expect_conditional):
    body = backend_shape.function_body(text, name)
    if body is None:
        print("FAIL: %s body missing" % name, file=sys.stderr)
        return False
    conditional = 0
    for match in BRANCH.finditer(body):
        target = match.group(2)
        following = LABEL.search(body, match.end())
        if following is not None and following.group(1) == target:
            print("FAIL: %s branches to the next block: %s" %
                  (name, target), file=sys.stderr)
            return False
        if match.group(1) != "jmp":
            conditional += 1
    if expect_conditional and conditional == 0:
        print("FAIL: %s has no conditional branch" % name, file=sys.stderr)
        return False
    if not expect_conditional and conditional != 0:
        print("FAIL: %s still has a conditional branch" % name,
              file=sys.stderr)
        return False
    return True


def main():
    text = backend_shape.compile_asm(PROGRAM, ("--codegen=fast",))
    for name, expect_conditional in SHAPES:
        if not check_body(text, name, expect_conditional):
            return 1
    proc = backend_shape.run_source(PROGRAM, ("--codegen=fast",))
    if proc.returncode != 6:
        print("FAIL: program exited %r, expected 6 (stderr: %r)" %
              (proc.returncode, proc.stderr[:400]), file=sys.stderr)
        return 1
    print("fast branches: no branch to the next block")
    return 0


if __name__ == "__main__":
    sys.exit(main())
