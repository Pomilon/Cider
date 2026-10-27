#!/usr/bin/env python3
"""v4 assignment through a pointer-returning operator[].

A struct/class member `operator[]` that answers `*T` may be assigned through:
the receiver and index are evaluated once, the operator is called once, and
the value is stored through the returned pointer. Every built-in compound
form loads the pointee, combines, and stores once. A value-returning operator
is never treated as built-in indexing.

The positive program exercises `=` and every compound form on native,
vm-unopt and vm-opt; the negative programs pin the exact-value, exact-index
and pointer-result diagnostics.
"""

import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

POSITIVE = """struct Vec {
    var data: [int; 4];
    func operator[](i: int) -> *int {
        return &(*self).data[i];
    }
}

func _start() -> void {
    var v: Vec = Vec { data: [0, ...] };
    v[0] = 8;
    v[0] += 4;
    v[0] -= 2;
    v[0] *= 3;
    v[0] /= 2;
    v[0] %= 4;
    v[0] |= 8;
    v[0] &= 10;
    v[0] ^= 3;
    v[0] <<= 2;
    v[0] >>= 1;
    v[1] = 7;
    var code: int = *v[0] + *v[1];
    asm {
        "mov rax, 60"
        "mov edi, {code:d}"
        "syscall"
    }
}
"""

VALUE_MISMATCH = """struct Vec {
    var data: [int; 4];
    func operator[](i: int) -> *int {
        return &(*self).data[i];
    }
}

func _start() -> void {
    var v: Vec = Vec { data: [0, ...] };
    var x: u8 = 7;
    v[0] = x;
}
"""

INDEX_MISMATCH = """struct Vec {
    var data: [int; 4];
    func operator[](i: int) -> *int {
        return &(*self).data[i];
    }
}

func _start() -> void {
    var v: Vec = Vec { data: [0, ...] };
    v[true] = 7;
}
"""

VALUE_RETURN = """struct Vec {
    var data: [int; 4];
    func operator[](i: int) -> int {
        return (*self).data[i];
    }
}

func _start() -> void {
    var v: Vec = Vec { data: [0, ...] };
    v[0] = 7;
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
    with tempfile.TemporaryDirectory(prefix="cider-v4-index-assign-") as tmp:
        def source(name, text):
            path = os.path.join(tmp, name)
            with open(path, "w") as handle:
                handle.write(text)
            return path

        positive = source("positive.cid", POSITIVE)
        # 18 after the full compound chain plus 7 from the plain store.
        for leg, extra in LEGS:
            ran = run([CIDER, "run"] + extra + [positive])
            if (ran.returncode, ran.stdout, ran.stderr) != (25, "", ""):
                failures.append(
                    "index assignment %s run: %s (expected exit 25)" %
                    (leg, describe(ran)))

        negatives = (
            ("value type", VALUE_MISMATCH,
             "assignment through operator '[]' needs i32"),
            ("index type", INDEX_MISMATCH, "no matching operator '[]'"),
            ("pointer result", VALUE_RETURN,
             "assignment through operator '[]' needs a pointer result"),
        )
        for label, text, expected in negatives:
            path = source("negative.cid", text)
            checked = run([CIDER, "check", path])
            if checked.returncode == 0:
                failures.append(
                    "%s mismatch was accepted" % label)
            elif expected not in checked.stderr:
                failures.append(
                    "%s mismatch diagnostic does not name the rule: %s" %
                    (label, describe(checked)))

    for failure in failures:
        print("FAIL: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("operator[] assignment: native, vm-unopt and vm-opt agree; "
          "diagnostics name the rules")
    return 0


if __name__ == "__main__":
    sys.exit(main())
