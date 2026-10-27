#!/usr/bin/env python3
"""v4 whole-program global and rodata pruning.

Task 8's second slice extends dead-definition elimination past functions. This
test pins the driver contract end to end:

- optimized whole-program native assembly omits unreferenced globals, the
  rodata of an unused string global, and rodata used only by a dead function;
- a used global's initializer keeps its rodata alive through the global, and a
  dead function's reference does not keep its global alive (the old
  function-only pass would have kept that global forever);
- used global, string, and rodata symbols remain, the assembly links without
  undefined symbols, and the binary runs;
- `compile -c` and `compile --no-opt` keep every definition, because a split
  object may be referenced elsewhere and an unoptimized build promises no
  pruning;
- two optimized compiles are byte-identical.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

SOURCE = """var used_counter: int = 40;
var unused_counter: int = 99;
var used_text: string = "used-text";
var unused_text: string = "unused-text";
var used_global_chain: string = "kept-by-global";

func unused_function() -> void {
    var s: string = "dead-function-rodata";
    unused_counter = unused_counter + s.len;
}

func _start() -> void {
    used_counter = used_counter + 2;
    var code: int = 0;
    if (used_counter != 42) { code = 1; }
    if (used_text.len != 9) { code = 2; }
    if (used_global_chain.len != 14) { code = 3; }
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}
"""

USED_GLOBALS = ("g_used_counter", "g_used_text", "g_used_global_chain")
DEAD_GLOBALS = ("g_unused_counter", "g_unused_text")


def run(args, env=None, timeout=120):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def symbol_labels(text):
    """Every top-level label in emitted assembly, without the colon."""
    return set(re.findall(r"^([A-Za-z_][A-Za-z0-9_]*):$", text, re.MULTILINE))


def rodata_labels(text):
    return set(re.findall(r"\b(s_\d+)\b", text))


def label_block(text, label):
    match = re.search(
        r"^%s:\n(.*?)(?=^[A-Za-z_][A-Za-z0-9_]*:\n|\Z)" % re.escape(label),
        text, re.MULTILINE | re.DOTALL)
    return match.group(1) if match else ""


def global_init_label(text, label):
    match = re.search(r"\.quad (s_\d+)", label_block(text, label))
    return match.group(1) if match else None


def nm_defined(path):
    proc = run(["nm", path])
    if proc.returncode != 0:
        raise RuntimeError("nm failed: " + describe(proc))
    defined = set()
    for line in proc.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 3:
            defined.add(fields[-1])
    return defined


def nm_undefined(path):
    proc = run(["nm", "-u", path])
    if proc.returncode != 0:
        raise RuntimeError("nm -u failed: " + describe(proc))
    return set(line.split()[-1] for line in proc.stdout.splitlines()
               if line.split())


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    with tempfile.TemporaryDirectory(prefix="cider-v4-global-prune-") as tmp:
        source = os.path.join(tmp, "globals.cid")
        opt_asm = os.path.join(tmp, "opt.s")
        opt_asm2 = os.path.join(tmp, "opt2.s")
        noopt_asm = os.path.join(tmp, "noopt.s")
        opt_obj = os.path.join(tmp, "opt.o")
        object_obj = os.path.join(tmp, "whole.o")
        module_obj = os.path.join(tmp, "module.o")
        opt_bin = os.path.join(tmp, "opt.bin")
        with open(source, "w") as handle:
            handle.write(SOURCE)

        compiled = run([CIDER, "compile", source, "-o", opt_asm], env)
        if compiled.returncode != 0:
            print("FAIL: optimized compile: " + describe(compiled),
                  file=sys.stderr)
            return 1
        if compiled.stderr:
            failures.append("optimized compile wrote stderr: %r" %
                            compiled.stderr)
        compiled2 = run([CIDER, "compile", source, "-o", opt_asm2], env)
        if compiled2.returncode != 0:
            failures.append("second optimized compile: " + describe(compiled2))
        elif open(opt_asm, "rb").read() != open(opt_asm2, "rb").read():
            failures.append("two optimized compiles are not byte-identical")

        text = open(opt_asm).read()
        labels = symbol_labels(text)
        for name in USED_GLOBALS:
            if name not in labels:
                failures.append("optimized assembly lost used global %s" % name)
        for name in DEAD_GLOBALS:
            if name in labels:
                failures.append(
                    "optimized assembly kept unused global %s" % name)
        if "unused_function" in labels:
            failures.append("optimized assembly kept the dead function")
        if "_start" not in labels:
            failures.append("optimized assembly lost _start")

        used_rodata = rodata_labels(text)
        if len(used_rodata) != 2:
            failures.append(
                "optimized assembly has %d rodata blobs, expected 2" %
                len(used_rodata))
        for name in ("g_used_text", "g_used_global_chain"):
            target = global_init_label(text, name)
            if target is None:
                failures.append("%s has no rodata initLabel" % name)
            elif target not in used_rodata:
                failures.append(
                    "%s points at missing rodata %s" % (name, target))

        # `ir` is not an executable build, so the dump still carries every
        # definition even though `compile` pruned the same source.
        dumped = run([CIDER, "ir", source], env)
        if dumped.returncode != 0:
            failures.append("ir dump: " + describe(dumped))
        else:
            for name in ("g_unused_counter", "g_unused_text",
                         "unused_function"):
                if name not in dumped.stdout:
                    failures.append("ir dump was pruned: lost %s" % name)

        noopt = run([CIDER, "compile", "--no-opt", source,
                     "-o", noopt_asm], env)
        if noopt.returncode != 0:
            failures.append("--no-opt compile: " + describe(noopt))
        else:
            if noopt.stderr:
                failures.append("--no-opt compile wrote stderr: %r" %
                                noopt.stderr)
            noopt_text = open(noopt_asm).read()
            noopt_labels = symbol_labels(noopt_text)
            for name in USED_GLOBALS + DEAD_GLOBALS + ("unused_function",):
                if name not in noopt_labels:
                    failures.append(
                        "--no-opt assembly lost %s" % name)
            noopt_rodata = rodata_labels(noopt_text)
            if len(noopt_rodata) != 4:
                failures.append(
                    "--no-opt assembly has %d rodata blobs, expected 4" %
                    len(noopt_rodata))
            if not used_rodata < noopt_rodata:
                failures.append(
                    "optimized rodata is not a subset of the unoptimized set")

        whole = run([CIDER, "compile", "-c", source, "-o", object_obj], env)
        if whole.returncode != 0:
            failures.append("object compile: " + describe(whole))
        else:
            if whole.stderr:
                failures.append("object compile wrote stderr: %r" %
                                whole.stderr)
            defined = nm_defined(object_obj)
            for name in USED_GLOBALS + DEAD_GLOBALS + ("unused_function",):
                if name not in defined:
                    failures.append("object lost %s" % name)
            undefined = nm_undefined(object_obj)
            if undefined:
                failures.append(
                    "whole-program object has undefined symbols: %s" %
                    sorted(undefined))

        split = run([CIDER, "compile", "-c", "--module", "main", source,
                     "-o", module_obj], env)
        if split.returncode != 0:
            failures.append("split module compile: " + describe(split))
        else:
            defined = nm_defined(module_obj)
            for name in USED_GLOBALS + DEAD_GLOBALS + ("unused_function",):
                if name not in defined:
                    failures.append("split module object lost %s" % name)

        # A program that runs proves the kept global/rodata image is intact:
        # the string lengths are read through the globals, not folded.
        optimized_run = run([CIDER, "run", source], env)
        if (optimized_run.returncode, optimized_run.stdout,
                optimized_run.stderr) != (0, "", ""):
            failures.append("optimized native run: " +
                            describe(optimized_run))
        vm_run = run([CIDER, "run", "--target", "vm", source], env)
        if (vm_run.returncode, vm_run.stdout, vm_run.stderr) != (0, "", ""):
            failures.append("optimized vm run: " + describe(vm_run))

        assembled = run(["as", "-o", opt_obj, opt_asm])
        if assembled.returncode != 0:
            failures.append("assembling optimized output: " +
                            describe(assembled))
        else:
            undefined = nm_undefined(opt_obj)
            if undefined:
                failures.append(
                    "optimized object has undefined symbols: %s" %
                    sorted(undefined))
            linked = run(["ld", "-nostdlib", "-e", "_start",
                          "-o", opt_bin, opt_obj])
            if linked.returncode != 0:
                failures.append("linking optimized output: " +
                                describe(linked))
            else:
                executed = run([opt_bin])
                if (executed.returncode, executed.stdout,
                        executed.stderr) != (0, "", ""):
                    failures.append("linked optimized binary: " +
                                    describe(executed))

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 global-pruning failure(s)" % len(failures),
              file=sys.stderr)
        return 1
    print("v4 global pruning: unused globals/rodata omitted; used image runs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
