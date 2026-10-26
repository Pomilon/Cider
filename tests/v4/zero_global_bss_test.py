#!/usr/bin/env python3
"""v4 zero-aggregate global storage test.

A global whose image is entirely zero bytes is storage, not data. The emitter
must reserve it in `.bss` with one `.zero` of the declared extent, aligned by
the type's natural alignment, instead of writing one `.quad 0` per eight
bytes. This is the Cid-MC shape that produced 19.4M lines of assembly: two
1.1 MB frame buffers and several large zeroed tables.

The test also pins the other side: a non-zero aggregate and a string global
stay in `.data`, their symbols still land where the linker expects, and the
program can write and read the zero aggregate back. ELF symbol size and
address prove the extent and alignment, so a `.zero` that silently rounds or
packs cannot pass.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

COUNT = 65536
ZERO_BYTES = COUNT * 8

SOURCE = """var pad: [u8; 1] = [0, ...];
var zeros: [u64; %d] = [0, ...];
var nz: [u64; 2] = [7, 9];
var text: string = "zero-bss";

func _start() -> void {
    zeros[0] = 11;
    zeros[%d] = 22;
    var code: int = 0;
    if (zeros[0] != 11) { code = 1; }
    if (zeros[%d] != 22) { code = 2; }
    if (nz[0] != 7) { code = 3; }
    if (nz[1] != 9) { code = 4; }
    if (text.len != 8) { code = 5; }
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}
""" % (COUNT, COUNT - 1, COUNT - 1)


def run(args, env=None, timeout=120):
    return subprocess.run(
        args, capture_output=True, text=True, env=env, timeout=timeout, cwd=None)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def elf_sections(path):
    proc = run(["readelf", "-SW", path])
    if proc.returncode != 0:
        raise RuntimeError("readelf -SW failed: " + describe(proc))
    sections = {}
    for line in proc.stdout.splitlines():
        match = re.match(r"\s*\[\s*(\d+)\]", line)
        if not match:
            continue
        fields = line.split()
        sections[fields[2] if len(fields) > 2 else ""] = {
            "index": int(match.group(1)),
            "align": int(fields[-1], 16),
        }
    return sections


def elf_symbols(path):
    proc = run(["readelf", "-sW", path])
    if proc.returncode != 0:
        raise RuntimeError("readelf -sW failed: " + describe(proc))
    symbols = {}
    for line in proc.stdout.splitlines():
        match = re.match(
            r"\s*(\d+):\s+([0-9A-Fa-f]+)\s+"
            r"(\d+|0x[0-9A-Fa-f]+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s*$",
            line)
        if not match:
            continue
        size = match.group(3)
        symbols[match.group(8)] = {
            "value": int(match.group(2), 16),
            "size": int(size, 16) if size.startswith("0x") else int(size),
            "section": int(match.group(7)),
        }
    return symbols


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix="cider-v4-zero-bss-") as tmp:
        source_path = os.path.join(tmp, "zero.cid")
        asm_path = os.path.join(tmp, "zero.s")
        obj_path = os.path.join(tmp, "zero.o")
        bin_path = os.path.join(tmp, "zero.bin")
        with open(source_path, "w") as handle:
            handle.write(SOURCE)

        compiled = run([CIDER, "compile", source_path, "-o", asm_path])
        if compiled.returncode != 0:
            print("FAIL: compile: " + describe(compiled), file=sys.stderr)
            return 1
        if compiled.stderr:
            failures.append("compiler wrote stderr: %r" % compiled.stderr)

        text = open(asm_path).read()
        if ".section .bss" not in text and "\n.bss\n" not in text:
            failures.append("zero aggregate did not land in .bss")
        if ".zero %d" % ZERO_BYTES not in text:
            failures.append(
                "compact zero storage for %d bytes is missing" % ZERO_BYTES)
        # One `.quad 0` per eight bytes was the old shape. A couple could be
        # legitimate scalar zeros in another program; this source has none.
        quad_zeros = text.count(".quad 0")
        if quad_zeros:
            failures.append(
                "zero aggregate still emits %d `.quad 0` line(s)" % quad_zeros)
        if text.count(".quad") > 16:
            failures.append(
                "assembly still carries a mass of .quad image words (%d)" %
                text.count(".quad"))
        if len(text) > 10000:
            failures.append("assembly is not compact: %d bytes" % len(text))
        if ".quad 7" not in text or ".quad 9" not in text:
            failures.append("non-zero aggregate left the .data image path")
        if ".section .rodata" not in text:
            failures.append("string bytes left .rodata")

        assembled = run(["as", "-o", obj_path, asm_path])
        if assembled.returncode != 0:
            failures.append("assembling failed: " + describe(assembled))
        else:
            sections = elf_sections(obj_path)
            symbols = elf_symbols(obj_path)
            bss = sections.get(".bss")
            data = sections.get(".data")
            if bss is None:
                failures.append("object has no .bss section")
            for name, size in (("g_pad", 1), ("g_zeros", ZERO_BYTES)):
                symbol = symbols.get(name)
                if symbol is None:
                    failures.append("object has no %s symbol" % name)
                    continue
                if symbol["size"] != size:
                    failures.append(
                        "%s size is %d, not %d" %
                        (name, symbol["size"], size))
                if bss is not None and symbol["section"] != bss["index"]:
                    failures.append("%s is not in .bss" % name)
            zeros = symbols.get("g_zeros")
            if zeros is not None and zeros["value"] % 8:
                failures.append(
                    "g_zeros address 0x%x is not 8-byte aligned" %
                    zeros["value"])
            if bss is not None and bss["align"] < 8:
                failures.append(
                    ".bss alignment is %d, not 8" % bss["align"])
            for name in ("g_nz", "g_text"):
                symbol = symbols.get(name)
                if symbol is None:
                    failures.append("object has no %s symbol" % name)
                elif data is not None and symbol["section"] != data["index"]:
                    failures.append("%s left its .data cell" % name)

            linked = run([
                "ld", "-nostdlib", "-e", "_start", "-o", bin_path, obj_path,
            ])
            if linked.returncode != 0:
                failures.append("linking failed: " + describe(linked))
            else:
                executed = run([bin_path])
                if executed.returncode != 0:
                    failures.append(
                        "zero aggregate did not read/write back: exit %r" %
                        executed.returncode)

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 zero-global failure(s)" % len(failures), file=sys.stderr)
        return 1
    print("v4 zero global: .bss/.zero storage, exact size, aligned, writable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
