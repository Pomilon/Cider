#!/usr/bin/env python3
"""Stage 1 acceptance test: fast backend versus GCC -O1, plus correctness.

What the default run does
-------------------------
* Compiles all ten benchmark kernels with ``--codegen=fast`` through
  ``bench/compiler/run.py``'s ``run_suite`` API.
* Runs the suite for 3 samples x 2 sessions (6 pooled samples per variant)
  pinned to CPU 2.  This is deliberately smaller than the Stage 1 gate's
  15 x 3 run so the test is practical in a normal development loop, but it is
  still an honest interleaved measurement: every variant is executed once per
  round and the starting variant rotates each round.
* Asserts every kernel checksum is ``ok`` and that the normalised Cider/GCC
  ``-O1`` geomean is ``<= 1.0`` (the Stage 1 gate threshold).
* Runs ``ctest --test-dir build/v3 --output-on-failure``,
  ``python3 tests/run_corpus.py`` and ``python3 scripts/gates.py`` as
  subprocesses and fails on any non-zero exit status.
* Runs every class-A corpus program under ``--codegen=fast`` and compares its
  stdout, stderr and exit status with the recorded expectation.  The legacy
  corpus runner alone cannot catch a fast-only miscompile; this leg covers
  ``gvn_float_mov`` and the rest of the 258-program class-A set.
* Runs a dedicated fast/legacy differential with an int-to-float and a
  float-to-i64 conversion, so a wrong GP effect model fails even outside the
  corpus.
* Checks that ``--codegen=fast`` actually selects a different native emitter
  from ``--codegen=legacy`` on a representative program; a silent fallback to
  legacy would otherwise satisfy the timing assertion by measuring the old
  backend.

What the full gate mode does
----------------------------
``--full`` uses the exact Stage 1 gate configuration: 15 samples x 3
sessions, pinned to CPU 2 (45 pooled samples per variant), with the same
checksum, geomean and correctness assertions.  The separate Stage 1 gate run
in ``bench/compiler/results/stage1.json`` uses this configuration.

This script never changes the harness, thresholds, kernel set, sample counts,
or checksums.  ``--no-correctness`` exists only for a development loop that
already ran the three correctness suites; the gate run must not use it.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))
sys.path.insert(0, str(REPO / "tests"))

from bench.compiler.run import run_suite  # noqa: E402
from source_entry import with_main_entry, wrap_file  # noqa: E402

CORPUS = REPO / "tests" / "corpus"

DEFAULT_SAMPLES = 3
DEFAULT_SESSIONS = 2
GATE_SAMPLES = 15
GATE_SESSIONS = 3
GATE_CPU = 2
O1_THRESHOLD = 1.0

ROUTE_SOURCE = """\
func add(a: i32, b: i32) -> i32 {
    return a + b;
}

func probe() -> i32 {
    var total: i32 = 0;
    var i: i32 = 0;
    while (i < 4) {
        total = add(total, i);
        i = i + 1;
    }
    return total;
}

func _start() -> void {
    asm {
        "mov rax, 60"
        "mov edi, 6"
        "syscall"
    }
}
"""


# Conversion cases that exercise both directions of the GP <-> SSE effect
# model.  The float-to-i64 program reproduces the stale-alias miscompile (`k`
# must become 9, not the integer 5) and the int-to-float program covers the
# feeding-GP-read direction that dead-move removal previously broke.
FAST_DIFFERENTIAL_CASES = [
    (
        "int_to_float",
        """\
func _start() {
    var i: i64 = 5;
    var f1: float = i as float;
    var f2: float = i as float;
    var k: i64 = f1 as i64;
    if (f1 == 5.0 && f2 == 5.0 && k == 5) {
        asm {
            "mov rax, 60"
            "mov rdi, 42"
            "syscall"
        }
    }
    asm {
        "mov rax, 60"
        "mov rdi, 1"
        "syscall"
    }
}
""",
        42,
    ),
    (
        "float_to_i64",
        """\
var GF: f64 = 9.9;

func main() -> int {
    var a: int = 42;
    var f: f64 = a as f64;
    var d: f64 = 49.9;
    var i: i64 = d as i64;
    if (f != 42.0) { return 10; }
    if (i != 49) { return 11; }
    var g: f64 = 7.9;
    var j: i64 = g as i64;
    if (j != 7) { return 12; }
    var c: int = 5;
    var h: f64 = c as f64;
    var e: f64 = GF;
    var k: i64 = e as i64;
    if (k != 5) { return 13; }
    if (h != 5.0) { return 14; }
    return 0;
}
""",
        13,
    ),
]


def run_command(label, command, env=None):
    print("== %s ==" % label, flush=True)
    print("$ " + " ".join(map(str, command)), flush=True)
    proc = subprocess.run(command, cwd=str(REPO), env=env, text=True)
    if proc.returncode != 0:
        print("%s exited %d" % (label, proc.returncode), file=sys.stderr)
        return False
    return True


def _run_program(cider, entry, codegen, stdin_data=None, limit_kb=None):
    """Run one native program with the selected emitter and return its tuple."""
    command = [str(cider), "run", "--codegen=" + codegen, str(entry)]
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", str(REPO / "modules"))
    preexec = None
    if limit_kb is not None:
        import resource
        cap = int(limit_kb) * 1024

        def preexec():
            resource.setrlimit(resource.RLIMIT_AS, (cap, cap))

    try:
        proc = subprocess.run(
            command, input=stdin_data, env=env, capture_output=True,
            text=True, timeout=120, preexec_fn=preexec)
    except subprocess.TimeoutExpired:
        return ("<timeout>", "", "timeout")
    return (proc.stdout, proc.stderr, proc.returncode)


def check_fast_differential(cider, workdir):
    """Run the conversion cases through both emitters and compare results."""
    failures = []
    for name, source, expected_exit in FAST_DIFFERENTIAL_CASES:
        path = workdir / (name + ".cid")
        # Mirror the corpus harness: fixtures that define `main` get a real
        # source `_start` wrapper, so the test does not depend on a hidden
        # entry trampoline.
        path.write_text(with_main_entry(source))
        want = ("", "", expected_exit)
        for kind in ("legacy", "fast"):
            got = _run_program(cider, path, kind)
            if got != want:
                failures.append("%s/%s got=%r want=%r" %
                                (name, kind, got, want))
    if failures:
        for failure in failures:
            print("fast differential: " + failure, file=sys.stderr)
        return False
    print("fast differential: %d conversion cases passed on both emitters" %
          len(FAST_DIFFERENTIAL_CASES))
    return True


def check_fast_corpus(cider):
    """Run every class-A corpus program through the fast emitter."""
    with (CORPUS / "classes.json").open() as handle:
        classes = json.load(handle)
    names = sorted(name for name, entry in classes.items()
                   if entry["class"] == "A")
    failures = []
    for name in names:
        directory = CORPUS / name
        if not directory.is_dir():
            directory = CORPUS / "legacy" / name
        with (directory / "expected.json").open() as handle:
            expected = json.load(handle)
        stdin_data = None
        input_path = directory / "input.txt"
        if input_path.exists():
            stdin_data = input_path.read_text()
        entry, scratch = wrap_file(str(directory / "main.cid"))
        try:
            got = _run_program(cider, entry, "fast", stdin_data,
                               expected.get("limit_kb"))
        finally:
            if scratch is not None:
                shutil.rmtree(scratch, ignore_errors=True)
        want = (expected["stdout"], expected["stderr"], expected["exit"])
        if got != want:
            failures.append((name, got, want))
    print("fast corpus: %d programs, %d failures" %
          (len(names), len(failures)))
    for name, got, want in failures:
        print("FAST FAIL %s: got=%r want=%r" % (name, got, want),
              file=sys.stderr)
    return not failures


def check_fast_route(cider, workdir):
    """Prove ``--codegen=fast`` is not a silent alias for legacy."""
    source = workdir / "route.cid"
    source.write_text(ROUTE_SOURCE)
    env = dict(os.environ)
    env["CIDER_LIB"] = str(REPO / "modules")
    outputs = {}
    for kind in ("legacy", "fast"):
        target = workdir / ("route." + kind + ".asm")
        proc = subprocess.run(
            [str(cider), "compile", "--codegen=" + kind, "-o", str(target),
             str(source)],
            capture_output=True, text=True, env=env, timeout=120)
        if proc.returncode != 0:
            print("route check: %s compile failed: %s" %
                  (kind, proc.stderr), file=sys.stderr)
            return False
        outputs[kind] = target.read_text()
    if outputs["fast"] == outputs["legacy"]:
        print("route check: --codegen=fast produced legacy assembly; the fast "
              "backend was not selected", file=sys.stderr)
        return False
    print("route check: fast and legacy assembly differ (fast selected)")
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--full", action="store_true",
                        help="use the exact Stage 1 gate configuration "
                             "(15 samples x 3 sessions, CPU 2)")
    parser.add_argument("--samples", type=int, default=None,
                        help="override samples per session (default %d)"
                             % DEFAULT_SAMPLES)
    parser.add_argument("--sessions", type=int, default=None,
                        help="override sessions (default %d)"
                             % DEFAULT_SESSIONS)
    parser.add_argument("--cpu", type=int, default=None,
                        help="pin every run to this CPU (gate uses 2)")
    parser.add_argument("--cider", type=Path,
                        default=REPO / "build" / "v3" / "cider")
    parser.add_argument("--out", type=Path, default=None,
                        help="keep the raw benchmark JSON here instead of a "
                             "temporary file")
    parser.add_argument("--no-correctness", action="store_true",
                        help="skip ctest/corpus/gates (development only; "
                             "never valid for the gate)")
    parser.add_argument("--skip-route", action="store_true",
                        help="skip the fast-emitter route assertion")
    args = parser.parse_args(argv)

    if args.samples is not None and args.samples <= 0:
        parser.error("--samples must be positive")
    if args.sessions is not None and args.sessions <= 0:
        parser.error("--sessions must be positive")
    if args.full and (args.samples is not None or args.sessions is not None):
        parser.error("--full cannot be combined with --samples/--sessions")

    samples = GATE_SAMPLES if args.full else (
        args.samples if args.samples is not None else DEFAULT_SAMPLES)
    sessions = GATE_SESSIONS if args.full else (
        args.sessions if args.sessions is not None else DEFAULT_SESSIONS)
    cpu = args.cpu if args.cpu is not None else GATE_CPU

    cider = args.cider.resolve()
    if not cider.is_file():
        print("missing compiler: %s (build with cmake --build build/v3)" %
              cider, file=sys.stderr)
        return 1

    print("Stage 1 test: %d samples x %d sessions, CPU %s, --codegen=fast" %
          (samples, sessions, "unpinned" if cpu is None else cpu), flush=True)

    if not args.no_correctness:
        env = dict(os.environ, CIDER_BIN=str(cider))
        checks = [
            ("ctest", ["ctest", "--test-dir", "build/v3",
                       "--output-on-failure"]),
            ("corpus", [sys.executable, "tests/run_corpus.py"]),
            ("gates", [sys.executable, "scripts/gates.py"]),
        ]
        for label, command in checks:
            if not run_command(label, command, env):
                return 1
    else:
        print("correctness suites skipped by --no-correctness")

    workdir_ctx = tempfile.TemporaryDirectory(prefix="cider-stage1-test-")
    workdir = Path(workdir_ctx.name)
    if not args.no_correctness:
        if not check_fast_differential(cider, workdir):
            return 1
        if not check_fast_corpus(cider):
            return 1
    if not args.skip_route and not check_fast_route(cider, workdir):
        return 1

    out = args.out.resolve() if args.out is not None else (
        workdir / "stage1-test.json")
    if args.out is not None:
        out.parent.mkdir(parents=True, exist_ok=True)

    report = run_suite(
        REPO / "bench" / "compiler" / "kernels",
        out,
        cider,
        "gcc",
        samples,
        sessions,
        cpu,
        ("--codegen=fast",),
    )

    print("\n%-26s %10s %10s %10s %8s" %
          ("kernel", "Cider ms", "-O1 ms", "Cider/-O1", "checksum"))
    for kernel in report["kernels"]:
        cider_ms = kernel["cider"]["median_ns"] / 1e6
        o1_ms = kernel["c"]["O1"]["median_ns"] / 1e6
        ratio = kernel["c"]["ratios"]["O1"]
        print("%-26s %10.3f %10.3f %10.3f %8s" %
              (kernel["name"], cider_ms, o1_ms, ratio,
               kernel["checksum"]))
    geomean = report["geomean"]["O1"]
    print("\ngeomean Cider / GCC -O1: %.4f (threshold <= %.1f)" %
          (geomean, O1_THRESHOLD))
    print("raw report: %s" % out)

    failures = []
    bad = [k["name"] for k in report["kernels"]
           if k["checksum"] != "ok"]
    if bad:
        failures.append("non-ok checksums: %s" % ", ".join(bad))
    if geomean > O1_THRESHOLD:
        failures.append("O1 geomean %.4f exceeds %.1f" %
                        (geomean, O1_THRESHOLD))
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("PASS: all checksums ok and Cider geomean <= GCC -O1")
    return 0


if __name__ == "__main__":
    sys.exit(main())
