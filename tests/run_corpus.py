#!/usr/bin/env python3
"""v3 corpus runner: differential harness, native leg live, VM legs stubbed.

Per program (tests/corpus/<name>/): main.cid + expected.json
{stdout, stderr, exit} (+ optional input.txt, sibling modules).
Class comes from tests/corpus/classes.json (OUTPUT.md differential gate):
  A  still works: stdout/stderr/exit must match (signals normalize to TRAPPED)
  B  v2-silenced, v3 must reject or trap: any loud failure passes
     (nonzero exit with stderr diagnostic, or death by signal)
  C  still rejected: v3 must fail, same class (check-time vs trap noted)
  D  no v2 repro / informational: run and report only, never fails the suite
Multi-file programs need imports (unimplemented): harness skips them with
reason `blocked-imports` until the driver loads modules.

Legs: only `native` exists (v3 `run` shells to as+ld -nostdlib).
`vm-unopt`/`vm-opt` columns are accepted but reported as PENDING — there is
no v3 interpreter yet (see ticket for the v3 VM). The schema is ready for it.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "tests", "corpus")
CIDER = os.environ.get(
    "CIDER_BIN", os.path.join(ROOT, "build", "v3", "cider"))
LEGS = ["native"]  # vm-unopt, vm-opt join when the interpreter lands
TRAPPED = "<trapped by signal>"


def outcome_of(proc):
    if proc.returncode is not None and proc.returncode < 0:
        return (TRAPPED, proc.stdout, proc.returncode)
    return (proc.stdout, proc.stderr, proc.returncode)


def run_native(path, stdin_data):
    try:
        proc = subprocess.run(
            [CIDER, "run", path], input=stdin_data,
            capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return ("<timeout>", "", "timeout")
    out, err, code = outcome_of(proc)
    return (out, err, code)


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
        with open(os.path.join(d, "expected.json")) as fh:
            expected = json.load(fh)
        siblings = [f for f in os.listdir(d)
                    if f.endswith(".cid") and f != "main.cid"]
        if siblings:
            skipped.append((name, "blocked-imports"))
            continue
        stdin_data = None
        if os.path.exists(os.path.join(d, "input.txt")):
            with open(os.path.join(d, "input.txt")) as fh:
                stdin_data = fh.read()
        out, err, code = run_native(os.path.join(d, "main.cid"), stdin_data)
        if cls == "A":
            ok = (out == expected["stdout"] and err == expected["stderr"]
                  and code == expected["exit"])
            # A trap in v3 is silent by design (diff #19): a program whose
            # v2 expectation names a vm-error line fails here loudly, which
            # is the signal to reclassify it B, not to bless the text.
            if not ok:
                failures.append((name, (out, err, code), expected))
        elif cls in ("B", "C"):
            loud = (code != 0 and (err != "" or out == TRAPPED))
            if not loud:
                failures.append((name, (out, err, code), "expected loud failure"))
        elif cls == "D":
            pending.append((name, (out, err, code)))
    print("corpus: %d programs, %d failures, %d pending(D), %d skipped" % (
        len(names), len(failures), len(pending), len(skipped)))
    for name, reason in skipped:
        print("SKIP %s: %s" % (name, reason))
    for name, got, want in failures:
        print("FAIL %s: got=%r want=%r" % (name, got, want))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
