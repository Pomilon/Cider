#!/usr/bin/env python3
"""v4 struct-pointer delete.

`delete p` for `*Struct` (and `*Union`) is nil-safe and calls the global
`operator delete` once with the pointer value. Structs carry no destructor,
so no deinit chain is synthesized. Class pointers keep their current
destructor-before-release order, and pointers to scalars, pointers, function
pointers, interfaces and arrays stay rejected.

The positive program counts allocations and frees through a custom global
operator pair, verifies `delete nil` does not call the operator, and checks
that a class deinit still runs before its release. Native, vm-unopt and
vm-opt must agree; the negative programs pin the diagnostic.
"""

import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

POSITIVE = """var allocations: int = 0;
var frees: int = 0;
var order: int = 0;
var pool: [u8; 128] = [0, ...];

func operator new(size: i64, align: i64) -> *void {
    allocations = allocations + 1;
    return &pool[0] as *void;
}

func operator delete(ptr: *void) -> void {
    frees = frees + 1;
    order = order * 10 + 2;
}

struct S {
    var x: int;
    var y: int;
}

class C {
    var x: int;
    func init() { (*self).x = 4; }
    func deinit() { order = order * 10 + 1; }
}

func _start() -> void {
    var code: int = 0;
    var s: *S = new S { x: 3, y: 4 };
    var sx: int = s->x;
    var sy: int = s->y;
    delete nilPtr as *S;
    if (frees != 0) { code = 1; }
    delete s;
    if (frees != 1) { code = 2; }
    var c: *C = new C();
    delete c;
    if (allocations != 2) { code = 3; }
    if (frees != 2) { code = 4; }
    // struct delete emits 2; class deinit then release emits 1 then 2.
    if (order != 212) { code = 5; }
    if (sx != 3 || sy != 4) { code = 6; }
    asm {
        "mov rax, 60"
        "mov edi, {code:d}"
        "syscall"
    }
}
"""

SCALAR = """func operator delete(ptr: *void) -> void { }

func _start() -> void {
    var p: *int = nilPtr;
    delete p;
}
"""

POINTER = """func operator delete(ptr: *void) -> void { }

func _start() -> void {
    var p: **int = nilPtr;
    delete p;
}
"""

FUNCTION = """func f() -> void { }
func operator delete(ptr: *void) -> void { }

func _start() -> void {
    var p: *func() -> void = nilPtr;
    delete p;
}
"""

ARRAY = """func operator delete(ptr: *void) -> void { }

func _start() -> void {
    var p: *[int; 2] = nilPtr;
    delete p;
}
"""

INTERFACE = """interface I { func f() -> int; }
func operator delete(ptr: *void) -> void { }

func _start() -> void {
    var p: *I = nilPtr;
    delete p;
}
"""

LEGS = (
    ("native", []),
    ("vm-unopt", ["--target", "vm", "--no-opt"]),
    ("vm-opt", ["--target", "vm"]),
)


def run(args, timeout=60):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix="cider-v4-struct-delete-") as tmp:
        def source(name, text):
            path = os.path.join(tmp, name)
            with open(path, "w") as handle:
                handle.write(text)
            return path

        positive = source("positive.cid", POSITIVE)
        for leg, extra in LEGS:
            ran = run([CIDER, "run"] + extra + [positive])
            if (ran.returncode, ran.stdout, ran.stderr) != (0, "", ""):
                failures.append(
                    "struct delete %s run: %s (expected exit 0)" %
                    (leg, describe(ran)))

        negatives = (
            ("scalar", SCALAR),
            ("pointer", POINTER),
            ("function", FUNCTION),
            ("array", ARRAY),
            ("interface", INTERFACE),
        )
        for label, text in negatives:
            path = source("negative.cid", text)
            checked = run([CIDER, "check", path])
            if checked.returncode == 0:
                failures.append("%s pointer delete was accepted" % label)
            elif "delete needs a class, struct or union pointer" not in \
                    checked.stderr:
                failures.append(
                    "%s pointer diagnostic does not name the rule: %s" %
                    (label, describe(checked)))

    for failure in failures:
        print("FAIL: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("struct delete: native, vm-unopt and vm-opt agree; class deinit "
          "still precedes release; invalid pointees are rejected")
    return 0


if __name__ == "__main__":
    sys.exit(main())
