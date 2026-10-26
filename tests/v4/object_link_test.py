#!/usr/bin/env python3
"""v4 object emission and GNU ld linking.

Phase 3a proves the compiler can write a real ELF64 relocatable object and
that `cider link` drives GNU ld without a hidden runtime:

- native `compile -c` emits ET_REL / EM_X86_64 and the linked binary runs;
- PomeOS emits the same ELF64 object, links at 0x2000000 with the documented
  linker script and BASE_ADDR defsym, and defines `_start`;
- missing entry, undefined symbols, duplicate symbols and wrong targets fail
  loudly instead of falling back to another path.

Phase 3b (when the split-module path lands) compiles one entry module and one
library module separately. The library object defines only its own symbols;
the entry object carries undefined relocations, not duplicate definitions.
Compiler-minted generic specializations are weak, so two objects may use the
same specialization and the linker keeps one address for it.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

NATIVE_WHOLE = """func helper(x: int) -> int { return x + 1; }

func _start() -> void {
    var code: int = helper(41);
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}
"""

POMEOS_START = """func _start() -> void {
    asm {
        "mov rax, 2"
        "xor rdi, rdi"
        "syscall"
    }
}
"""

POMEOS_SCRIPT = """OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = BASE_ADDR;
    .text : { *(.text) }
    .data : { *(.data) }
    .rodata : { *(.rodata) }
    .bss : { *(.bss) }
}
"""

SPLIT_LIB = """export var shared: int = 35;

export func ident<T>(x: T) -> T { return x; }

export func add(a: int, b: int) -> int {
    return ident::<int>(a) + ident::<int>(b);
}
"""

VTABLE_LIB = """export interface Shape {
    func area() -> int;
}

export struct Sq {
    var s: int;
    func area() -> int { return (*self).s * (*self).s; }
}

export func areaOf(x: Shape) -> int { return x.area(); }

export func localArea(v: int) -> int {
    var q: Sq = Sq { s: v };
    var sh: Shape = q;
    return sh.area();
}
"""

VTABLE_MAIN = """import vlib;

func _start() -> void {
    var q: vlib.Sq = vlib.Sq { s: 6 };
    var sh: vlib.Shape = q;
    var code: int = vlib.areaOf(sh) + vlib.localArea(2);
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}
"""

SPLIT_MAIN = """import lib;
from lib import ident;

func _start() -> void {
    lib.shared = lib.shared + 1;
    var a: int = ident::<int>(20);
    var b: int = ident::<int>(21);
    var code: int = lib.add(a, b) + lib.add(0, 1);
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}
"""


def run(args, env=None, timeout=120):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def elf_header(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        raise RuntimeError("missing ELF magic")
    if data[4] != 2:
        raise RuntimeError("not ELF64")
    if data[5] != 1:
        raise RuntimeError("not little-endian")
    return {
        "type": int.from_bytes(data[16:18], "little"),
        "machine": int.from_bytes(data[18:20], "little"),
        "entry": int.from_bytes(data[24:32], "little"),
    }


def nm_symbols(path):
    proc = run(["nm", path])
    if proc.returncode != 0:
        raise RuntimeError("nm failed: " + describe(proc))
    defined = {}
    undefined = set()
    for line in proc.stdout.splitlines():
        fields = line.split()
        if not fields:
            continue
        if len(fields) == 2 and fields[0] == "U":
            undefined.add(fields[1])
        elif len(fields) >= 3:
            defined[fields[-1]] = fields[-2]
    return defined, undefined


def readelf_relocs(path):
    proc = run(["readelf", "-rW", path])
    if proc.returncode != 0:
        raise RuntimeError("readelf -r failed: " + describe(proc))
    return proc.stdout


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    for tool in ("as", "ld"):
        if shutil.which(tool) is None:
            print("SKIP: %s is unavailable" % tool)
            return 77
    have_nm = shutil.which("nm") is not None
    have_readelf = shutil.which("readelf") is not None

    failures = []
    notes = []

    def fail(text):
        failures.append(text)

    def source(tmp, name, text):
        path = os.path.join(tmp, name)
        with open(path, "w") as handle:
            handle.write(text)
        return path

    def expect_failure(label, args, needles):
        proc = run([CIDER] + args)
        if proc.returncode == 0:
            fail("%s: accepted (%s)" % (label, " ".join(args)))
            return
        for needle in needles:
            if needle not in proc.stderr:
                fail("%s: diagnostic does not name %r: %s" % (
                    label, needle, describe(proc)))

    with tempfile.TemporaryDirectory(prefix="cider-v4-object-") as tmp:
        # --- Phase 3a: one whole-program native object --------------------
        whole_src = source(tmp, "whole.cid", NATIVE_WHOLE)
        whole_obj = os.path.join(tmp, "whole.o")
        whole_bin = os.path.join(tmp, "whole.bin")
        compiled = run([CIDER, "compile", "-c", whole_src, "-o", whole_obj])
        if compiled.returncode != 0:
            fail("native object compile: " + describe(compiled))
        else:
            if compiled.stderr:
                fail("native object compile wrote stderr: %r" % compiled.stderr)
            header = elf_header(whole_obj)
            if header["type"] != 1:
                fail("native object is not ET_REL (%d)" % header["type"])
            if header["machine"] != 62:
                fail("native object is not x86-64 (%d)" % header["machine"])
            if have_nm:
                defined, undefined = nm_symbols(whole_obj)
                for symbol in ("_start", "helper"):
                    if symbol not in defined:
                        fail("native object does not define %s" % symbol)
                    elif defined[symbol].lower() != "t":
                        fail("native object symbol %s is not text (%s)" % (
                            symbol, defined[symbol]))
                if undefined:
                    fail("whole-program object has undefined symbols: %s" %
                         ", ".join(sorted(undefined)))
            linked = run([CIDER, "link", "-o", whole_bin, whole_obj])
            if linked.returncode != 0:
                fail("native link: " + describe(linked))
            else:
                if linked.stderr:
                    fail("native link wrote stderr: %r" % linked.stderr)
                executed = run([whole_bin])
                if executed.returncode != 42:
                    fail("native linked binary exited %r, not 42" % (
                        executed.returncode))

        # --- Phase 3a: PomeOS object and ELF64 link -----------------------
        pomeos_src = source(tmp, "start.cid", POMEOS_START)
        pomeos_obj = os.path.join(tmp, "start.o")
        pomeos_elf = os.path.join(tmp, "start.elf")
        pomeos_default = os.path.join(tmp, "start-default.elf")
        link_script = os.path.join(tmp, "link.ld")
        with open(link_script, "w") as handle:
            handle.write(POMEOS_SCRIPT)
        compiled = run([
            CIDER, "compile", "-c", "--target", "pomeos", pomeos_src,
            "-o", pomeos_obj,
        ])
        if compiled.returncode != 0:
            fail("PomeOS object compile: " + describe(compiled))
        else:
            header = elf_header(pomeos_obj)
            if header["type"] != 1:
                fail("PomeOS object is not ET_REL (%d)" % header["type"])
            if header["machine"] != 62:
                fail("PomeOS object is not x86-64 (%d)" % header["machine"])
            if have_nm:
                defined, _ = nm_symbols(pomeos_obj)
                if "_start" not in defined:
                    fail("PomeOS object does not define _start")
            linked = run([
                CIDER, "link", "--target", "pomeos", "--script", link_script,
                "--base-address", "0x2000000", "-o", pomeos_elf, pomeos_obj,
            ])
            if linked.returncode != 0:
                fail("PomeOS link: " + describe(linked))
            else:
                header = elf_header(pomeos_elf)
                if header["type"] != 2:
                    fail("PomeOS image is not ET_EXEC (%d)" % header["type"])
                if header["machine"] != 62:
                    fail("PomeOS image is not x86-64 (%d)" %
                         header["machine"])
                if header["entry"] != 0x2000000:
                    fail("PomeOS entry is 0x%x, not 0x2000000" %
                         header["entry"])
                if have_nm:
                    defined, undefined = nm_symbols(pomeos_elf)
                    if "_start" not in defined:
                        fail("PomeOS image does not define _start")
                    if undefined:
                        fail("PomeOS image has undefined symbols: %s" %
                             ", ".join(sorted(undefined)))
            # The default PomeOS script is the same contract; no caller
            # script is required to get the 0x2000000 base.
            defaulted = run([
                CIDER, "link", "--target", "pomeos", "-o", pomeos_default,
                pomeos_obj,
            ])
            if defaulted.returncode != 0:
                fail("PomeOS default-script link: " + describe(defaulted))
            else:
                header = elf_header(pomeos_default)
                if header["entry"] != 0x2000000:
                    fail("PomeOS default entry is 0x%x, not 0x2000000" %
                         header["entry"])

        # --- Phase 3b: entry and module objects linked together -----------
        lib_src = source(tmp, "lib.cid", SPLIT_LIB)
        main_src = source(tmp, "main.cid", SPLIT_MAIN)
        main_obj = os.path.join(tmp, "main.o")
        lib_obj = os.path.join(tmp, "lib.o")
        lib_no_entry_obj = os.path.join(tmp, "lib_no_entry.o")
        split_bin = os.path.join(tmp, "split.bin")
        split_bin2 = os.path.join(tmp, "split2.bin")

        compiled = run([
            CIDER, "compile", "-c", "--module", "main", main_src,
            "-o", main_obj,
        ])
        if compiled.returncode != 0:
            fail("entry-module object compile: " + describe(compiled))
        compiled_lib = run([
            CIDER, "compile", "-c", "--module", "lib", lib_src,
            "-o", lib_obj,
        ])
        if compiled_lib.returncode != 0:
            fail("module object compile: " + describe(compiled_lib))
        compiled_no_entry = run([
            CIDER, "compile", "-c", "--no-entry", lib_src,
            "-o", lib_no_entry_obj,
        ])
        if compiled_no_entry.returncode != 0:
            fail("--no-entry module object compile: " +
                 describe(compiled_no_entry))

        if all(os.path.exists(path) for path in
               (main_obj, lib_obj, lib_no_entry_obj)):
            if have_nm:
                main_defined, main_undefined = nm_symbols(main_obj)
                lib_defined, lib_undefined = nm_symbols(lib_obj)
                no_entry_defined, _ = nm_symbols(lib_no_entry_obj)
                if "_start" not in main_defined:
                    fail("entry module object does not define _start")
                if "lib__add" in main_defined:
                    fail("entry module object duplicates lib__add")
                if "lib__add" not in main_undefined:
                    fail("entry module object does not reference lib__add")
                if "g_lib__shared" not in main_undefined:
                    fail("entry module object does not reference g_lib__shared")
                if "_start" in lib_defined:
                    fail("library object duplicates the process entry")
                if "lib__add" not in lib_defined:
                    fail("library object does not define lib__add")
                if "g_lib__shared" not in lib_defined:
                    fail("library object does not define g_lib__shared")
                if "_start" in no_entry_defined:
                    fail("--no-entry object defines _start")
                if "lib__add" not in no_entry_defined:
                    fail("--no-entry object does not define lib__add")
                # Compiler-minted specializations are weak in every object:
                # both sides may use ident<int>, and the linker picks one
                # body so a function pointer keeps one address.
                main_weak = {name for name, kind in main_defined.items()
                             if "__ident__S" in name and kind == "W"}
                lib_weak = {name for name, kind in lib_defined.items()
                            if "__ident__S" in name and kind == "W"}
                if not lib_weak:
                    fail("library object did not mark ident<int> weak")
                if not main_weak:
                    fail("entry object did not mark ident<int> weak")
                if main_weak and lib_weak and main_weak != lib_weak:
                    fail("entry and library objects disagree on ident<int>")
            if have_readelf:
                relocs = readelf_relocs(main_obj)
                if "lib__add" not in relocs:
                    fail("entry module object has no relocation for lib__add")
                if "R_X86_64_PLT32" not in relocs:
                    fail("entry module object has no PLT32 call relocation")
            linked = run([CIDER, "link", "-o", split_bin, main_obj, lib_obj])
            if linked.returncode != 0:
                fail("split link: " + describe(linked))
            else:
                executed = run([split_bin])
                if executed.returncode != 42:
                    fail("split linked binary exited %r, not 42" % (
                        executed.returncode))
            linked = run([
                CIDER, "link", "-o", split_bin2, main_obj, lib_no_entry_obj,
            ])
            if linked.returncode != 0:
                fail("--no-entry split link: " + describe(linked))
            else:
                executed = run([split_bin2])
                if executed.returncode != 42:
                    fail("--no-entry linked binary exited %r, not 42" % (
                        executed.returncode))
        else:
            notes.append("split-module object files missing")

        # --- Phase 3b: an interface vtable is one weak symbol -------------
        vlib_src = source(tmp, "vlib.cid", VTABLE_LIB)
        vmain_src = source(tmp, "vmain.cid", VTABLE_MAIN)
        vmain_obj = os.path.join(tmp, "vmain.o")
        vlib_obj = os.path.join(tmp, "vlib.o")
        vtable_bin = os.path.join(tmp, "vtable.bin")
        vmain_build = run([
            CIDER, "compile", "-c", "--module", "main", vmain_src,
            "-o", vmain_obj,
        ])
        if vmain_build.returncode != 0:
            fail("vtable entry object compile: " + describe(vmain_build))
        vlib_build = run([
            CIDER, "compile", "-c", "--module", "vlib", vlib_src,
            "-o", vlib_obj,
        ])
        if vlib_build.returncode != 0:
            fail("vtable module object compile: " + describe(vlib_build))
        if os.path.exists(vmain_obj) and os.path.exists(vlib_obj):
            if have_nm:
                vmain_defined, _ = nm_symbols(vmain_obj)
                vlib_defined, _ = nm_symbols(vlib_obj)
                table = "vlib__Sq__vtable__vlib__Shape"
                if vmain_defined.get(table) != "W":
                    fail("entry object does not emit the interface vtable weak")
                if vlib_defined.get(table) != "W":
                    fail("module object does not emit the interface vtable weak")
            linked = run([
                CIDER, "link", "-o", vtable_bin, vmain_obj, vlib_obj,
            ])
            if linked.returncode != 0:
                fail("vtable split link: " + describe(linked))
            else:
                executed = run([vtable_bin])
                if executed.returncode != 40:
                    fail("vtable split binary exited %r, not 40" % (
                        executed.returncode))

        # --- Diagnostics --------------------------------------------------
        main_only = source(tmp, "main_only.cid",
                           "func main() -> int { return 0; }\n")
        main_only_obj = os.path.join(tmp, "main_only.o")
        expect_failure(
            "native object without _start",
            ["compile", "-c", main_only, "-o", main_only_obj],
            ["_start"])
        compiled = run([
            CIDER, "compile", "-c", "--no-entry", main_only,
            "-o", main_only_obj,
        ])
        if compiled.returncode != 0:
            fail("non-entry main-only object compile: " + describe(compiled))
        else:
            expect_failure(
                "link without an entry",
                ["link", "-o", os.path.join(tmp, "no_entry.bin"),
                 main_only_obj],
                ["_start", "entry"])

        undefined_src = source(tmp, "undefined.cid", """extern func missing(x: int) -> int;

func _start() -> void {
    var code: int = missing(1);
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}
""")
        undefined_obj = os.path.join(tmp, "undefined.o")
        compiled = run([
            CIDER, "compile", "-c", undefined_src, "-o", undefined_obj,
        ])
        if compiled.returncode != 0:
            fail("undefined-reference object compile: " + describe(compiled))
        else:
            expect_failure(
                "undefined symbol",
                ["link", "-o", os.path.join(tmp, "undefined.bin"),
                 undefined_obj],
                ["missing", "undefined reference"])

        duplicate_a = os.path.join(tmp, "duplicate_a.o")
        duplicate_b = os.path.join(tmp, "duplicate_b.o")
        for path in (duplicate_a, duplicate_b):
            built = run([CIDER, "compile", "-c", whole_src, "-o", path])
            if built.returncode != 0:
                fail("duplicate fixture compile: " + describe(built))
        expect_failure(
            "duplicate symbol",
            ["link", "-o", os.path.join(tmp, "duplicate.bin"),
             duplicate_a, duplicate_b],
            ["duplicate symbol"])

        expect_failure(
            "compile -c --target uefi",
            ["compile", "-c", "--target", "uefi", whole_src,
             "-o", os.path.join(tmp, "bad.o")],
            ["uefi", "ELF64"])
        expect_failure(
            "link --target uefi",
            ["link", "--target", "uefi", "-o", os.path.join(tmp, "bad.efi"),
             whole_obj],
            ["uefi"])
        expect_failure(
            "link --target vm",
            ["link", "--target", "vm", "-o", os.path.join(tmp, "bad.vm"),
             whole_obj],
            ["vm"])
        expect_failure(
            "compile -c --target vm",
            ["compile", "-c", "--target", "vm", whole_src,
             "-o", os.path.join(tmp, "bad-vm.o")],
            ["vm"])

    for note in notes:
        print("NOTE: " + note)
    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 object/link failure(s)" % len(failures), file=sys.stderr)
        return 1
    print("v4 object/link: ELF64 objects, GNU ld, split-module symbols, "
          "diagnostics")
    return 0


if __name__ == "__main__":
    sys.exit(main())
