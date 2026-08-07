#!/usr/bin/env python3
"""Run the main-branch legacy test corpus through the v2 compiler.

For each tests/legacy/<name>.cdr:
  - VM:        cider run (optimized) and cider run --no-opt
  - Native:    cider compile -> nasm -> ld (or gcc for main-less binaries)
Every path must exit with the code in tests/legacy/<name>.exit.
Also checks vm-opt == vm-unopt == native exit agreement.
"""
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "cider"))
LEGACY = os.path.join(REPO, "tests", "legacy")


def run_cider(args, timeout=180):
    r = subprocess.run([CIDER] + args, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout, r.stderr


def run_native(cdr):
    with tempfile.TemporaryDirectory() as tmp:
        asm = os.path.join(tmp, "prog.asm")
        obj = os.path.join(tmp, "prog.o")
        exe = os.path.join(tmp, "prog")
        c = subprocess.run([CIDER, "compile", cdr, "-o", asm], capture_output=True, text=True)
        if c.returncode != 0:
            return ("err", "compile", c.stderr.strip()[:200])
        a = subprocess.run(["nasm", "-O0", "-f", "elf64", asm, "-o", obj], capture_output=True, text=True)
        if a.returncode != 0:
            return ("err", "nasm", a.stderr.strip()[:200])
        nm = subprocess.run(["nm", obj], capture_output=True, text=True).stdout
        if "_start" in nm:
            # -nostartfiles: the program's own _start is the entry, but libc
            # is still linked so extern free/malloc/putchar resolve.
            l = subprocess.run(["gcc", "-nostartfiles", obj, "-o", exe, "-no-pie"],
                               capture_output=True, text=True)
        else:
            l = subprocess.run(["gcc", obj, "-o", exe, "-no-pie"], capture_output=True, text=True)
        if l.returncode != 0:
            return ("err", "link", l.stderr.strip()[:200])
        r = subprocess.run([exe], capture_output=True, text=True, timeout=180)
        return r.returncode, r.stdout[:200], r.stderr[:200]


def main():
    names = sorted(f[:-4] for f in os.listdir(LEGACY) if f.endswith(".cdr"))
    results = []
    for name in names:
        cdr = os.path.join(LEGACY, name + ".cdr")
        exp_path = os.path.join(LEGACY, name + ".exit")
        expected = int(open(exp_path).read().strip()) if os.path.exists(exp_path) else None

        vm_opt = run_cider(["run", cdr])
        vm_noopt = run_cider(["run", "--no-opt", cdr])
        native = run_native(cdr)

        statuses = []
        if vm_opt[0] == 124 or vm_noopt[0] == 124:
            statuses.append("VM-TIMEOUT")
        if expected is not None:
            if vm_opt[0] != expected:
                statuses.append(f"vm-opt={vm_opt[0]}(exp {expected})")
            if vm_noopt[0] != expected:
                statuses.append(f"vm-noopt={vm_noopt[0]}(exp {expected})")
            if native[0] != expected:
                statuses.append(f"native={native[0]}(exp {expected})")
        if native[0] == "err":
            statuses.append(f"native-{native[1]}-error: {native[2][:120]}")
        if not statuses:
            # matrix agreement
            if vm_opt[0] != vm_noopt[0] or vm_opt[0] != native[0]:
                statuses.append("MATRIX-MISMATCH")
        results.append((name, statuses))
        status = "OK " if not statuses else "FAIL"
        print(f"{status} {name}: {', '.join(statuses) if statuses else 'exit ' + str(vm_opt[0])}")

    ok = sum(1 for _, s in results if not s)
    print(f"\n{ok}/{len(results)} pass")

main()
