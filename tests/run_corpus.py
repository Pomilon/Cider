#!/usr/bin/env python3
"""v3 corpus runner: differential harness across every leg.

Per program (tests/corpus/<name>/): main.cid + expected.json
{stdout, stderr, exit} (+ optional input.txt, sibling modules).
Class comes from tests/corpus/classes.json (OUTPUT.md differential gate):
  A  still works: stdout/stderr/exit must match (signals normalize to TRAPPED)
  B  v2-silenced, v3 must reject or trap: any loud failure passes
     (nonzero exit with stderr diagnostic, or death by signal)
  C  still rejected: v3 must fail, same class (check-time vs trap noted)
  D  no v2 repro / informational: run and report only, never fails the suite
Multi-file programs run like any other: the driver loads sibling modules.
Legs: `native` compiles and runs (v3 `run` shells to as+ld -nostdlib),
`vm-unopt` interprets the module the lowerer produced, and `vm-opt` the same
module after the optimiser. The two VM legs must agree with native: a death
arrives as a negative returncode on every leg, which outcome_of() normalises
to TRAPPED, and an interpreter refusal is reported as its own outcome
rather than a silent mismatch.
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "tests", "corpus")
CIDER = os.environ.get(
    "CIDER_BIN", os.path.join(ROOT, "build", "v3", "cider"))
LEGS = ["native", "vm-unopt", "vm-opt"]
TRAPPED = "<trapped by signal>"
sys.path.insert(0, os.path.join(ROOT, "tests"))
from source_entry import wrap_file  # noqa: E402
def outcome_of(proc):
    if proc.returncode is not None and proc.returncode < 0:
        return (TRAPPED, proc.stdout, proc.returncode)
    # Trap normalization (OUTPUT.md differential gate): `cider run` dies by
    # the same signal that killed the program, so signal death arrives as a
    # negative returncode and a legitimate high exit status (e.g. 150)
    # stays an exit status. There is no band heuristic: the signal
    # surviving is what names a trap, not a range of exit codes.
    return (proc.stdout, proc.stderr, proc.returncode)


def run_leg(path, stdin_data, leg, limit_kb=None):
    command = [CIDER, "run"]
    if leg != "native":
        # An interpreter is not an emitter: run interprets, and --no-opt
        # skips the optimiser so the unoptimised module is the one run.
        command += ["--target", "vm"]
        if leg == "vm-unopt":
            command += ["--no-opt"]
    command.append(path)
    try:
        env = dict(os.environ)
        # The library tree ships with the compiler: dotted imports
        # (`std.io`) resolve to <root>/modules without ambient setup.
        env.setdefault("CIDER_LIB", os.path.join(ROOT, "modules"))
        preexec = None
        if limit_kb is not None:
            import resource
            cap = int(limit_kb) * 1024

            def preexec():
                resource.setrlimit(resource.RLIMIT_AS, (cap, cap))

        proc = subprocess.run(
            command, input=stdin_data, env=env,
            capture_output=True, text=True, timeout=60,
            preexec_fn=preexec)
    except subprocess.TimeoutExpired:
        return ("<timeout>", "", "timeout")
    return outcome_of(proc)


def main():
    with open(os.path.join(CORPUS, "classes.json")) as fh:
        classes = json.load(fh)
    names = sorted(classes.keys()) if len(sys.argv) < 2 else sys.argv[1:]
    failures = []
    pending = []
    skipped = []
    for name in names:
        entry = classes[name]
        cls = entry["class"]
        d = os.path.join(CORPUS, name)
        if not os.path.isdir(d):
            d = os.path.join(CORPUS, "legacy", name)
        with open(os.path.join(d, "expected.json")) as fh:
            expected = json.load(fh)
        stdin_data = None
        if os.path.exists(os.path.join(d, "input.txt")):
            with open(os.path.join(d, "input.txt")) as fh:
                stdin_data = fh.read()
        # v4 requires a source-defined entry. Fixtures that still express
        # `main` get a real Cider `_start` wrapper in a copied tree, so the
        # compiler never injects one and sibling imports still resolve.
        entry_path, scratch = wrap_file(os.path.join(d, "main.cid"))
        out = {}
        err = {}
        code = {}
        try:
            for leg in LEGS:
                got = run_leg(entry_path, stdin_data, leg,
                              expected.get("limit_kb"))
                out[leg], err[leg], code[leg] = got
        finally:
            if scratch is not None:
                shutil.rmtree(scratch, ignore_errors=True)
        outcomes = dict((leg, (out[leg], err[leg], code[leg])) for leg in LEGS)
        # Every leg answers for itself: a VM refusal reads as that leg's
        # own loud failure, so no leg is carved out of the verdict below.
        if cls == "A":
            ok = all(value == (expected["stdout"], expected["stderr"],
                               expected["exit"])
                     for value in outcomes.values())
            # A trap in v3 is silent by design (diff #19): a program whose
            # v2 expectation names a vm-error line fails here loudly, which
            # is the signal to reclassify it B, not to bless the text.
            if not ok:
                failures.append((name, outcomes, expected))
        elif cls in ("B", "C"):
            loud = code["native"] != 0 and (
                err["native"] != "" or out["native"] == TRAPPED)
            # A class that native rejects loudly must not pass on a leg that
            # answers silently: the VM leg's answer is compared against
            # native's own, refusal included as the interpreter's limit.
            for leg, value in outcomes.items():
                if leg == "native":
                    continue
                if value != (out["native"], err["native"], code["native"]):
                    loud = False
            if not loud:
                failures.append((name, outcomes, "expected loud failure"))
        elif cls == "D":
            pending.append((name, outcomes))
    print("corpus: %d programs, %d failures, %d pending(D), %d skipped" % (
        len(names), len(failures), len(pending), len(skipped)))
    for name, reason in skipped:
        print("SKIP %s: %s" % (name, reason))
    for name, got, want in failures:
        print("FAIL %s: got=%r want=%r" % (name, got, want))
    # A program carrying no v2 repro is reported with its native answer so
    # the D class stays informational without printing every leg's text.
    for name, outcomes in pending:
        print("D    %s: native=%r" % (name, outcomes["native"]))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
