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


def run_cider(args):
    r = subprocess.run([CIDER] + args, capture_output=True, text=True, timeout=60)
    return RunResult(r.returncode, r.stdout, r.stderr)


def run_native(ir_path):
    with tempfile.TemporaryDirectory() as tmp:
        asm = os.path.join(tmp, "prog.asm")
        obj = os.path.join(tmp, "prog.o")
        exe = os.path.join(tmp, "prog")
        subprocess.run([CIDER, "compile", ir_path, "-o", asm], check=True,
                       capture_output=True, text=True)
        subprocess.run(["nasm", "-O0", "-f", "elf64", asm, "-o", obj], check=True,
                       capture_output=True, text=True)
        subprocess.run(["ld", obj, "-o", exe], check=True, capture_output=True, text=True)
        r = subprocess.run([exe], capture_output=True, text=True, timeout=60)
        return RunResult(r.returncode, r.stdout, r.stderr)


def load_expected(name):
    with open(os.path.join(PROGRAMS, name, "expected.json")) as f:
        return json.load(f)


def make_test(name):
    def test(self):
        ir = os.path.join(PROGRAMS, name, "main.ir")
        exp = load_expected(name)

        r1 = run_cider(["run", "--no-opt", ir])
        r2 = run_cider(["run", ir])
        r3 = run_native(ir)

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
    # The triple-differential program corpus is being migrated from the
    # Phase-1 .ir scaffolding to .cdr sources (Phase 4 task 4). While the
    # VM and codegen are being rewritten (Phases 5/6), the programs dir is
    # empty and the matrix tests are disabled.
    if not os.path.isdir(PROGRAMS):
        return
    for name in sorted(os.listdir(PROGRAMS)):
        if os.path.isdir(os.path.join(PROGRAMS, name)):
            setattr(TestPrograms, f"test_{name}", make_test(name))


def test_fixtures(self):
    fixtures = os.path.join(REPO, "tests", "fixtures")
    for name in sorted(os.listdir(fixtures)):
        if not name.endswith(".cdr"):
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
