#!/usr/bin/env python3
"""WS3 runtime-diagnostics gate.

Every diagnosed fault must make the same promise on the native backend and
both interpreter legs: one exact stderr line and exit status 2.  The test is
deliberately strict about the status: a signal death is not a diagnosis.
"""

import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from source_entry import with_main_entry  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

NULL_DEREF = "vm error: null pointer dereference\n"
NIL_IFACE = "vm error: nil interface dispatch\n"
NIL_FNPTR = "vm error: call: nil function pointer\n"
OOB = "vm error: index out of bounds\n"
COMPILER_OOM = "compiler: out of memory\n"
VM_OOM = "vm error: out of memory\n"

LEGS = [
    ("native", []),
    ("vm-unopt", ["--target", "vm", "--no-opt"]),
    ("vm-opt", ["--target", "vm"]),
]


def env_with(**values):
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    for key, value in values.items():
        env[key] = value
    return env


def invoke(args, env=None, timeout=60):
    return subprocess.run(
        [CIDER] + args,
        capture_output=True,
        text=True,
        env=env,
        timeout=timeout,
    )


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


class Harness:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="cider-ws3-")
        self.failures = []

    def close(self):
        self.tmp.cleanup()

    def source(self, name, text):
        path = os.path.join(self.tmp.name, name)
        with open(path, "w") as handle:
            handle.write(with_main_entry(text))
        return path

    def expect(self, label, proc, stderr, code=2, stdout=""):
        if (proc.returncode, proc.stdout, proc.stderr) != (code, stdout, stderr):
            self.failures.append(
                "%s: expected exit %r, stdout %r, stderr %r; got %s" % (
                    label, code, stdout, stderr, describe(proc)))

    def run_legs(self, label, path, stderr, extra=()):
        for leg, leg_args in LEGS:
            proc = invoke(["run"] + leg_args + list(extra) + [path], env=env_with())
            self.expect("%s [%s]" % (label, leg), proc, stderr)

    def run_live(self, label, path, extra=()):
        for leg, leg_args in LEGS:
            proc = invoke(["run"] + leg_args + list(extra) + [path], env=env_with())
            self.expect("%s [%s]" % (label, leg), proc, "", code=0)


def nil_pointer_read():
    return """func main() -> int {
  var p: *int = nilPtr;
  return *p;
}
"""


def nil_pointer_store():
    return """func main() -> int {
  var p: *int = nilPtr;
  *p = 7;
  return 0;
}
"""


def nil_interface():
    return """interface Shape {
  func kind() -> int;
}

func main() -> int {
  var s: Shape = uninit;
  return s.kind();
}
"""


def nil_fnptr():
    return """func main() -> int {
  var g: *func(int) -> int = nilPtr;
  return g(3);
}
"""


def nil_closure():
    return """func main() -> int {
  var c: func(int) -> int = uninit;
  return c(3);
}
"""


def oob_read(with_checks):
    return """var idx: int = 0;

func main() -> int {
  var a: [int; 3] = [10, 20, 30];
  idx = 5;
  return a[idx];
}
"""


def oob_store(with_checks):
    return """var idx: int = 0;

func main() -> int {
  var a: [int; 3] = [10, 20, 30];
  idx = 5;
  a[idx] = 7;
  return 0;
}
"""


def in_range():
    return """var idx: int = 0;

func main() -> int {
  var a: [int; 3] = [10, 20, 30];
  idx = 1;
  return a[idx] - 20;
}
"""


def live_pointer():
    return """func main() -> int {
  var x: int = 41;
  var p: *int = &x;
  return *p - 41;
}
"""


def live_interface():
    return """interface Shape {
  func kind() -> int;
}

class Box {
  var v: int;
  func kind() -> int { return this->v; }
}

func main() -> int {
  var b: Box = Box { v: 7 };
  var s: Shape = b;
  return s.kind() - 7;
}
"""


def live_fnptr():
    return """func inc(x: int) -> int { return x + 1; }

func main() -> int {
  var g: *func(int) -> int = &inc;
  return g(3) - 4;
}
"""


def live_closure():
    return """func main() -> int {
  var c: func(int) -> int = func(x: int) -> int { return x + 1; };
  return c(3) - 4;
}
"""


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    h = Harness()
    try:
        h.run_legs("nil pointer read", h.source("nil_ptr_read.cid", nil_pointer_read()), NULL_DEREF)
        h.run_legs("nil pointer store", h.source("nil_ptr_store.cid", nil_pointer_store()), NULL_DEREF)
        h.run_legs("nil interface dispatch", h.source("nil_iface.cid", nil_interface()), NIL_IFACE)
        h.run_legs("nil function pointer", h.source("nil_fnptr.cid", nil_fnptr()), NIL_FNPTR)
        h.run_legs("nil closure", h.source("nil_closure.cid", nil_closure()), NULL_DEREF)

        h.run_legs("checked OOB read", h.source("oob_read.cid", oob_read(True)), OOB,
                   extra=("--checks",))
        h.run_legs("checked OOB store", h.source("oob_store.cid", oob_store(True)), OOB,
                   extra=("--checks",))
        h.run_live("in-range checked index", h.source("in_range.cid", in_range()),
                   extra=("--checks",))

        unchecked = h.source("oob_unchecked.cid", oob_store(False))
        for leg, leg_args in LEGS:
            proc = invoke(["run"] + leg_args + [unchecked], env=env_with())
            if proc.returncode == 2 or proc.stderr == OOB:
                h.failures.append(
                    "unchecked OOB [%s] must not diagnose: %s" % (
                        leg, describe(proc)))

        h.run_live("live pointer", h.source("live_ptr.cid", live_pointer()))
        h.run_live("live interface", h.source("live_iface.cid", live_interface()))
        h.run_live("live fnptr", h.source("live_fnptr.cid", live_fnptr()))
        h.run_live("live closure", h.source("live_closure.cid", live_closure()))

        check_src = h.source("check_oom.cid", live_pointer())
        for spec in ("check", "check:len"):
            proc = invoke(["check", check_src], env=env_with(CIDER_FAULT_ALLOC=spec))
            h.expect("compiler OOM %s" % spec, proc, COMPILER_OOM)

        asm = os.path.join(h.tmp.name, "oom.s")
        proc = invoke(["compile", check_src, "-o", asm],
                      env=env_with(CIDER_FAULT_ALLOC="optimize"))
        h.expect("compiler OOM optimize", proc, COMPILER_OOM)

        codegen_asm = os.path.join(h.tmp.name, "oom_codegen.s")
        proc = invoke(["compile", check_src, "-o", codegen_asm],
                      env=env_with(CIDER_FAULT_ALLOC="codegen"))
        h.expect("compiler OOM codegen", proc, COMPILER_OOM)

        proc = invoke(["run", "--target", "vm", check_src],
                      env=env_with(CIDER_FAULT_VM_ALLOC="1"))
        h.expect("VM OOM", proc, VM_OOM)

        outdir = os.path.join(h.tmp.name, "outdir")
        os.makedirs(outdir)
        proc = invoke(["compile", check_src, "-o", outdir], env=env_with())
        h.expect("write failure", proc,
                 "compiler: cannot write '%s'\n" % outdir)

        uefi_src = h.source("uefi_nil.cid", """func efi_main() -> int {
  var p: *int = nilPtr;
  return *p;
}
""")
        uefi_asm = os.path.join(h.tmp.name, "uefi_nil.s")
        proc = invoke(["compile", "--target", "uefi", uefi_src, "-o", uefi_asm],
                      env=env_with())
        if proc.returncode != 0:
            h.failures.append("UEFI policy compile failed: %s" % describe(proc))
        else:
            with open(uefi_asm) as handle:
                text = handle.read()
            if "ud2" not in text:
                h.failures.append("UEFI policy: assembly has no ud2")
            for message in (NULL_DEREF, NIL_IFACE, NIL_FNPTR, OOB):
                if message in text:
                    h.failures.append(
                        "UEFI policy: assembly contains Linux diagnostic %r" % message)
    finally:
        h.close()

    if h.failures:
        for failure in h.failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d runtime diagnostic failure(s)" % len(h.failures), file=sys.stderr)
        return 1
    print("runtime diagnostics: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
