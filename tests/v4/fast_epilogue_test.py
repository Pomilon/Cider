#!/usr/bin/env python3
"""Only fast functions with many returns share one epilogue.

A four-return function (`pick4`) must stage each return value, jump to one
epilogue label, and restore/leave/ret once. A three-return function (`pick3`)
keeps its local per-return epilogues: the threshold is four. Legacy keeps its
per-return epilogue at every return count.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func pick4(x: i32) -> i32 {
    if (x == 0) { return 10; }
    if (x == 1) { return 20; }
    if (x == 2) { return 30; }
    return 40;
}
func pick3(x: i32) -> i32 {
    if (x == 0) { return 10; }
    if (x == 1) { return 20; }
    return 30;
}
func _start() -> void {
    // Indirect calls keep both callees in the module: a direct call to a body
    // this small is inlined by the optimiser, and the emitter shape would
    // disappear before codegen.
    var f4: *func(i32) -> i32 = &pick4;
    var f3: *func(i32) -> i32 = &pick3;
    var code: i32 = 0;
    if (f4(0) != 10) { code = 1; }
    if (f4(1) != 20) { code = 2; }
    if (f4(2) != 30) { code = 3; }
    if (f4(3) != 40) { code = 4; }
    if (f3(0) != 10) { code = 5; }
    if (f3(1) != 20) { code = 6; }
    if (f3(2) != 30) { code = 7; }
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

PICK4_EPILOGUE = ".L_pick4_epilogue"
PICK3_EPILOGUE = ".L_pick3_epilogue"


def body_of(text, name):
    body = backend_shape.function_body(text, name)
    if body is None:
        print("FAIL: %s body missing" % name, file=sys.stderr)
    return body


def check_fast(args, label):
    text = backend_shape.compile_asm(PROGRAM, args)
    pick4 = body_of(text, "pick4")
    if pick4 is None:
        return False
    rets = backend_shape.count(r"(?m)^  ret$", pick4)
    if rets != 1:
        print("FAIL: %s pick4 has %d rets, expected 1" % (label, rets),
              file=sys.stderr)
        return False
    if not re.search(r"(?m)^%s:$" % re.escape(PICK4_EPILOGUE), pick4):
        print("FAIL: %s pick4 shared epilogue label is missing" % label,
              file=sys.stderr)
        return False
    # Four returns: the last emitted block falls through to the epilogue, so
    # exactly the other three jump.
    jumps = backend_shape.count(
        r"(?m)^  jmp %s$" % re.escape(PICK4_EPILOGUE), pick4)
    if jumps != 3:
        print("FAIL: %s pick4 has %d shared jumps, expected 3" % (label, jumps),
              file=sys.stderr)
        return False
    epilogue = pick4[pick4.index(PICK4_EPILOGUE):]
    if backend_shape.count(r"(?m)^  ret$", epilogue) != 1:
        print("FAIL: %s pick4 shared epilogue does not end in one ret" % label,
              file=sys.stderr)
        return False

    pick3 = body_of(text, "pick3")
    if pick3 is None:
        return False
    rets = backend_shape.count(r"(?m)^  ret$", pick3)
    if rets != 3:
        print("FAIL: %s pick3 has %d rets, expected 3 local epilogues" %
              (label, rets), file=sys.stderr)
        return False
    if PICK3_EPILOGUE in pick3:
        print("FAIL: %s pick3 gained a shared epilogue below the threshold" %
              label, file=sys.stderr)
        return False
    return True


def check_legacy():
    text = backend_shape.compile_asm(
        PROGRAM, ("--codegen=legacy", "--no-opt"))
    pick4 = body_of(text, "pick4")
    if pick4 is None:
        return False
    rets = backend_shape.count(r"(?m)^  ret$", pick4)
    if rets != 4:
        print("FAIL: legacy pick4 has %d rets, expected 4" % rets,
              file=sys.stderr)
        return False
    if PICK4_EPILOGUE in pick4:
        print("FAIL: legacy pick4 gained a shared epilogue", file=sys.stderr)
        return False
    return True


def check_runtime(args, label):
    proc = backend_shape.run_source(PROGRAM, args)
    if proc.returncode != 0:
        print("FAIL: %s exited %r (stderr: %r)" %
              (label, proc.returncode, proc.stderr[:400]), file=sys.stderr)
        return False
    return True


def main():
    for args, label in ((("--codegen=fast",), "fast"),
                        (("--codegen=fast", "--no-opt"), "fast no-opt")):
        if not check_fast(args, label):
            return 1
        if not check_runtime(args, label):
            return 1
    if not check_legacy():
        return 1
    if not check_runtime(("--codegen=legacy", "--no-opt"), "legacy no-opt"):
        return 1
    print("fast epilogue: four-return functions share one epilogue")
    return 0


if __name__ == "__main__":
    sys.exit(main())
