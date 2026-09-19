#!/usr/bin/env python3
"""Triple-differential test harness.

Runs every tests/programs/<name>/main.ir three ways and requires all three
to agree with each other and with expected.json (stdout/stderr/exit):
  1. VM on unoptimized IR   : cider run --no-opt
  2. VM on optimized IR     : cider run            (optimizer arrives in Phase 7)
  3. Native                 : cider compile -> nasm -> ld -> run
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from collections import namedtuple

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "cider"))
PROGRAMS = os.path.join(REPO, "tests", "programs")

RunResult = namedtuple("RunResult", "returncode stdout stderr")


def run_cider(args, stdin_data=None):
    r = subprocess.run([CIDER] + args, input=stdin_data, capture_output=True,
                       text=True, timeout=180)
    return RunResult(r.returncode, r.stdout, r.stderr)


def run_native(ir_path, stdin_data=None):
    with tempfile.TemporaryDirectory() as tmp:
        asm = os.path.join(tmp, "prog.asm")
        obj = os.path.join(tmp, "prog.o")
        exe = os.path.join(tmp, "prog")
        subprocess.run([CIDER, "compile", ir_path, "-o", asm], check=True,
                       capture_output=True, text=True)
        subprocess.run(["nasm", "-O0", "-f", "elf64", asm, "-o", obj], check=True,
                       capture_output=True, text=True)
        # Link with gcc: the crt0 provides _start, which calls main and
        # exits with its return value (a bare ld -e main crashes when main
        # returns, since the kernel stack has no return address). The
        # stdlib's malloc/free definitions in the object shadow libc's.
        nm = subprocess.run(["nm", obj], capture_output=True, text=True).stdout
        if "_start" in nm:
            # -nostartfiles: the program's own _start is the entry, but libc
            # is linked so extern malloc/putchar resolve.
            subprocess.run(["gcc", "-nostartfiles", obj, "-o", exe, "-no-pie"],
                           check=True, capture_output=True, text=True)
        else:
            subprocess.run(["gcc", obj, "-o", exe, "-no-pie"], check=True,
                           capture_output=True, text=True)
        r = subprocess.run([exe], input=stdin_data, capture_output=True,
                           text=True, timeout=180)
        return RunResult(r.returncode, r.stdout, r.stderr)


def load_expected(name):
    with open(os.path.join(PROGRAMS, name, "expected.json")) as f:
        return json.load(f)


def make_test(name):
    def test(self):
        src = os.path.join(PROGRAMS, name, "main.cid")
        exp = load_expected(name)
        inp = None
        in_path = os.path.join(PROGRAMS, name, "input.txt")
        if os.path.exists(in_path):
            with open(in_path) as f:
                inp = f.read()

        r1 = run_cider(["run", "--no-opt", src], inp)
        r2 = run_cider(["run", src], inp)
        r3 = run_native(src, inp)

        paths = [("vm-unopt", r1), ("vm-opt", r2), ("native", r3)]
        for label, r in paths:
            self.assertEqual(r.returncode, exp["exit"], f"{label}: exit")
            self.assertEqual(r.stdout, exp["stdout"], f"{label}: stdout")
            self.assertEqual(r.stderr, exp["stderr"], f"{label}: stderr")

        # Matrix agreement: all three paths identical.
        for a, b in [("vm-unopt", "vm-opt"), ("vm-unopt", "native"), ("vm-opt", "native")]:
            ra = dict(paths)[a]
            rb = dict(paths)[b]
            self.assertEqual((ra.returncode, ra.stdout, ra.stderr),
                             (rb.returncode, rb.stdout, rb.stderr),
                             f"{a} != {b}")

    return test


def discover():
    # The native path (path 3 of the triple-differential matrix) re-enables
    # in Phase 6 when the codegen is rewritten; paths 1-2 (VM) are live now.
    if not os.path.isdir(PROGRAMS):
        return
    for name in sorted(os.listdir(PROGRAMS)):
        if os.path.isdir(os.path.join(PROGRAMS, name)):
            setattr(TestPrograms, f"test_{name}", make_test(name))


def test_fixtures(self):
    fixtures = os.path.join(REPO, "tests", "fixtures")
    for name in sorted(os.listdir(fixtures)):
        if not name.endswith(".cid"):
            continue
        base = name[:-4]
        src = os.path.join(fixtures, name)
        if os.path.exists(os.path.join(fixtures, base + ".expected")):
            with open(os.path.join(fixtures, base + ".expected")) as f:
                expected = f.read()
            cmd = "build" if base.startswith("ir_") else "parse"
            r = run_cider([cmd, src])
            self.assertEqual(r.returncode, 0, f"{name}: {cmd} exit")
            self.assertEqual(r.stdout, expected, f"{name}: dump mismatch")
        elif os.path.exists(os.path.join(fixtures, base + ".errors")):
            with open(os.path.join(fixtures, base + ".errors")) as f:
                expected_errors = [l.strip() for l in f if l.strip()]
            cmd = "check" if base.startswith("check_") else "parse"
            r = run_cider([cmd, src])
            self.assertEqual(r.returncode, 1, f"{name}: expected failure")
            for sub in expected_errors:
                self.assertIn(sub, r.stderr, f"{name}: missing diagnostic {sub}")


class TestPrograms(unittest.TestCase):
    pass


setattr(TestPrograms, "test_fixtures", test_fixtures)
discover()

if __name__ == "__main__":
    unittest.main()
