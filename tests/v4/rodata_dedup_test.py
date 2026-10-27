#!/usr/bin/env python3
"""v4 exact-byte rodata deduplication.

The optimizer keeps the first blob for each exact byte string, rewrites every
reference to the later copies, and removes the duplicates. This test pins the
end-to-end contract:

- optimized whole-program assembly emits one blob per unique string, and every
  global initializer points at the canonical label;
- every rodata label spelled in the assembly is defined, the output assembles
  and links, and the binary runs with the expected answer;
- both VM legs see the same bytes and answer the same way;
- `--no-opt` keeps the per-occurrence blobs, because no optimization is
  promised there;
- two optimized compiles are byte-identical;
- the UEFI assembly path emits and assembles the same single copy, when
  clang's COFF assembler is available.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))
LINK_UEFI = os.path.join(REPO, "scripts", "link_uefi.sh")

SOURCE = """var first: string = "dedup-same";
var second: string = "dedup-same";
var third: string = "dedup-other";

func _start() -> void {
    var local: string = "dedup-same";
    var other: string = "dedup-other";
    var code: int = 0;
    if (first != "dedup-same") { code = 1; }
    if (second != "dedup-same") { code = 2; }
    if (third != "dedup-other") { code = 3; }
    if (local != "dedup-same") { code = 4; }
    if (other != "dedup-other") { code = 5; }
    if (first[0] != 'd' as char) { code = 6; }
    if (first[9] != 'e' as char) { code = 7; }
    if (first != second) { code = 8; }
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}
"""

UEFI_SOURCE = """var ua: string = "uefi-dedup";
var ub: string = "uefi-dedup";

msabi func efi_main(image_handle: *void, system_table: *void) -> int {
    var uc: string = "uefi-dedup";
    if (ua != ub) { return 1; }
    if (uc != ua) { return 2; }
    return 0;
}
"""


def run(args, env=None, timeout=120):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def defined_rodata_labels(text):
    return set(re.findall(r"^(s_\d+):$", text, re.MULTILINE))


def referenced_rodata_labels(text):
    return set(re.findall(r"\b(s_\d+)\b", text))


def label_block(text, label):
    match = re.search(
        r"^%s:\n(.*?)(?=^[A-Za-z_][A-Za-z0-9_]*:\n|\Z)" % re.escape(label),
        text, re.MULTILINE | re.DOTALL)
    return match.group(1) if match else ""


def global_init_label(text, label):
    match = re.search(r"\.quad (s_\d+)", label_block(text, label))
    return match.group(1) if match else None


def nm_undefined(path):
    proc = run(["nm", "-u", path])
    if proc.returncode != 0:
        raise RuntimeError("nm -u failed: " + describe(proc))
    return set(line.split()[-1] for line in proc.stdout.splitlines()
               if line.split())


def ld_has_i386pep():
    if shutil.which("ld") is None:
        return False
    proc = run(["ld", "-V"])
    return proc.returncode == 0 and "i386pep" in (proc.stdout + proc.stderr)


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []
    skipped = []
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    with tempfile.TemporaryDirectory(prefix="cider-v4-rodata-dedup-") as tmp:
        source = os.path.join(tmp, "dedup.cid")
        opt_asm = os.path.join(tmp, "opt.s")
        opt_asm2 = os.path.join(tmp, "opt2.s")
        noopt_asm = os.path.join(tmp, "noopt.s")
        opt_obj = os.path.join(tmp, "opt.o")
        opt_bin = os.path.join(tmp, "opt.bin")
        uefi_src = os.path.join(tmp, "dedup_efi.cid")
        uefi_asm = os.path.join(tmp, "dedup_efi.s")
        with open(source, "w") as handle:
            handle.write(SOURCE)
        with open(uefi_src, "w") as handle:
            handle.write(UEFI_SOURCE)

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
            failures.append("second optimized compile: " +
                            describe(compiled2))
        elif open(opt_asm, "rb").read() != open(opt_asm2, "rb").read():
            failures.append("two optimized compiles are not byte-identical")

        text = open(opt_asm).read()
        defined = defined_rodata_labels(text)
        referenced = referenced_rodata_labels(text)
        if len(defined) != 2:
            failures.append(
                "optimized assembly has %d rodata blobs, expected 2: %s" %
                (len(defined), sorted(defined)))
        missing = referenced - defined
        if missing:
            failures.append(
                "assembly names undefined rodata labels: %s" %
                sorted(missing))

        first_label = global_init_label(text, "g_first")
        second_label = global_init_label(text, "g_second")
        third_label = global_init_label(text, "g_third")
        if first_label is None or second_label is None:
            failures.append("optimized string globals lost their init labels")
        elif first_label != second_label:
            failures.append(
                "identical globals point at different blobs: %s vs %s" %
                (first_label, second_label))
        elif first_label not in defined:
            failures.append("g_first points at missing rodata %s" %
                            first_label)
        if third_label is None:
            failures.append("g_third lost its rodata init label")
        elif third_label == first_label:
            failures.append("distinct string bytes were merged")

        noopt = run([CIDER, "compile", "--no-opt", source,
                     "-o", noopt_asm], env)
        if noopt.returncode != 0:
            failures.append("--no-opt compile: " + describe(noopt))
        else:
            # Five declarations plus five comparison literals, one blob each.
            noopt_defined = defined_rodata_labels(open(noopt_asm).read())
            if len(noopt_defined) != 10:
                failures.append(
                    "--no-opt assembly has %d rodata blobs, expected 10" %
                    len(noopt_defined))

        # Behavior: optimized native, optimized VM, and unoptimized VM must
        # agree. String equality and byte indexing read the blob contents.
        for leg, extra in (("native", []),
                           ("vm-opt", ["--target", "vm"]),
                           ("vm-unopt", ["--target", "vm", "--no-opt"])):
            got = run([CIDER, "run"] + extra + [source], env)
            if (got.returncode, got.stdout, got.stderr) != (0, "", ""):
                failures.append(
                    "%s: exit %r, stdout %r, stderr %r; expected 0, '', ''" %
                    (leg, got.returncode, got.stdout, got.stderr))

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

        # UEFI uses clang's COFF assembler rather than GNU as. The same IR
        # dedup runs before both emitters, so one copy must appear and every
        # reference must resolve through the PE32+ link path.
        uefi = run([CIDER, "compile", "--target", "uefi", uefi_src,
                    "-o", uefi_asm], env)
        if uefi.returncode != 0:
            failures.append("uefi compile: " + describe(uefi))
        else:
            uefi_text = open(uefi_asm).read()
            uefi_defined = defined_rodata_labels(uefi_text)
            if len(uefi_defined) != 1:
                failures.append(
                    "uefi assembly has %d rodata blobs, expected 1" %
                    len(uefi_defined))
            uefi_missing = (referenced_rodata_labels(uefi_text) -
                            uefi_defined)
            if uefi_missing:
                failures.append(
                    "uefi assembly names undefined rodata labels: %s" %
                    sorted(uefi_missing))
            if shutil.which("clang") is None or not ld_has_i386pep():
                skipped.append(
                    "UEFI COFF assemble/link: clang or ld -m i386pep "
                    "unavailable")
            else:
                efi = os.path.join(tmp, "dedup.efi")
                linked = run([LINK_UEFI, uefi_asm, "-o", efi])
                if linked.returncode != 0:
                    failures.append("uefi COFF assemble/link: " +
                                    describe(linked))

    for message in skipped:
        print("SKIP: " + message)
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 rodata-dedup failure(s)" % len(failures),
              file=sys.stderr)
        return 1
    print("v4 rodata dedup: one blob per byte string; labels resolve and run")
    return 0


if __name__ == "__main__":
    sys.exit(main())
