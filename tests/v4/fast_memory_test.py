#!/usr/bin/env python3
"""Task 12's memory-operand, width and float-home end-to-end test.

The fast backend now gives floats their own XMM home table and folds a
single-use GEP address into the access that consumes it. The value legs run
every program on fast (optimised and unoptimised) and legacy, so a wrong
extension, a widened i32, a bad float spill or a removed volatile access
shows up as a wrong exit code.

The assembly-shape legs are the RED half of the test. Before Task 12, fast
float arithmetic only ever wrote xmm0 and then a frame slot, and a GEP was
always materialised as a separate address. The checks below therefore fail
on the old emitter even though its values happened to be correct.
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


def start(probe, call="probe()"):
    return probe + """
func _start() -> void {
    var code: i32 = %s;
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""" % call


# Every width is written, read back, and narrowed at its own width. The return
# is zero only when all checks hold; a silent widening or truncation of a
# narrow load/store changes a code.
WIDTHS = start("""func probe() -> i32 {
    var u8v: u8 = 200;
    if (u8v != 200) { return 1; }
    var i8v: i8 = -100;
    if (i8v != -100) { return 2; }
    var u16v: u16 = 60000;
    if (u16v != 60000) { return 3; }
    var i16v: i16 = -30000;
    if (i16v != -30000) { return 4; }
    var u32v: u32 = 4000000000;
    if (u32v != 4000000000) { return 5; }
    var i32v: i32 = -2000000000;
    if (i32v != -2000000000) { return 6; }
    var u64v: u64 = 9000000000000000000;
    if (u64v != 9000000000000000000) { return 7; }
    var i64v: i64 = -9000000000000000000;
    if (i64v != -9000000000000000000) { return 8; }
    var w8: u8 = (u8v + 100) as u8;
    if (w8 != 44) { return 9; }
    var s8: i8 = (i8v + 10) as i8;
    if (s8 != -90) { return 10; }
    var w16: u16 = (u16v + 10000) as u16;
    if (w16 != 4464) { return 11; }
    var s16: i16 = (i16v - 3000) as i16;
    if (s16 != 32536) { return 12; }
    var w32: u32 = (u32v + 1000000000) as u32;
    if (w32 != 705032704) { return 13; }
    return 0;
}
""")

# A dynamic array index turns the array base and the scaled index into a Gep.
# 1 + 2 + 3 + 4 + 5, with arr[2] replaced by 30 = 42.
ARRAY = start("""func probe() -> i32 {
    var arr: [i32; 5] = [1, 2, 3, 4, 5];
    arr[2] = 30;
    var sum: i32 = 0;
    var i: i32 = 0;
    while (i < 5) {
        sum = sum + arr[i];
        i = i + 1;
    }
    return sum;
}
""")

# Field addresses are Gep candidates; (7 + 5) + 11 = 23.
STRUCT = start("""struct Pair {
    var x: i32;
    var y: i32;
}
func probe() -> i32 {
    var p: Pair = Pair{x: 7, y: 11};
    p.x = p.x + 5;
    return p.x + p.y;
}
""")

# Pointer arithmetic produces a Gep with a scaled index: 30 + 40 = 70.
POINTER = start("""func probe() -> i32 {
    var arr: [i32; 4] = [10, 20, 30, 40];
    var p: *i32 = &arr[0];
    var q: *i32 = p + 2;
    return *q + *(p + 3);
}
""")

# f32 and f64 values in registers and in arrays:
# (1.5 + 2.5 + 3.5) + (2.0 * 3.5) = 7.5 + 7.0 = 14.5; 14.5 * 4 = 58.
FLOATS = start("""func probe() -> i32 {
    var fa: [f32; 3] = [1.5, 2.5, 3.5];
    var fs: f32 = fa[0] + fa[1] + fa[2];
    var a: f64 = 2.0;
    var b: f64 = 3.5;
    var c: f64 = a * b;
    var arr: [f64; 2] = [fs as f64, c];
    var sum: f64 = arr[0] + arr[1];
    return (sum * 4.0) as i32;
}
""")

# Eighteen loop-carried f64 values, more than the fifteen XMM home registers
# the allocator can use (xmm0 is the float scratch). Four increments move each
# value from i+1 to i+5; sum(5..22) = 243. The separate FLOAT_PRESSURE shape
# leg pins the home/spill behaviour; this leg checks every value survives.
FLOAT_LOOP = start("""func probe() -> i32 {
    var a0: f64 = 1.0;
    var a1: f64 = 2.0;
    var a2: f64 = 3.0;
    var a3: f64 = 4.0;
    var a4: f64 = 5.0;
    var a5: f64 = 6.0;
    var a6: f64 = 7.0;
    var a7: f64 = 8.0;
    var a8: f64 = 9.0;
    var a9: f64 = 10.0;
    var a10: f64 = 11.0;
    var a11: f64 = 12.0;
    var a12: f64 = 13.0;
    var a13: f64 = 14.0;
    var a14: f64 = 15.0;
    var a15: f64 = 16.0;
    var a16: f64 = 17.0;
    var a17: f64 = 18.0;
    var i: i32 = 0;
    while (i < 4) {
        a0 = a0 + 1.0;
        a1 = a1 + 1.0;
        a2 = a2 + 1.0;
        a3 = a3 + 1.0;
        a4 = a4 + 1.0;
        a5 = a5 + 1.0;
        a6 = a6 + 1.0;
        a7 = a7 + 1.0;
        a8 = a8 + 1.0;
        a9 = a9 + 1.0;
        a10 = a10 + 1.0;
        a11 = a11 + 1.0;
        a12 = a12 + 1.0;
        a13 = a13 + 1.0;
        a14 = a14 + 1.0;
        a15 = a15 + 1.0;
        a16 = a16 + 1.0;
        a17 = a17 + 1.0;
        i = i + 1;
    }
    var total: f64 = a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8
        + a9 + a10 + a11 + a12 + a13 + a14 + a15 + a16 + a17;
    return total as i32;
}
""")

# A local index keeps the Gep's scaled operand a home, so the address is one
# memory operand after folding. arr[2]=3, arr[3]=4, arr[4]=7 -> 14.
GEP = start("""func probe() -> i32 {
    var arr: [i32; 8] = [1, 2, 3, 4, 5, 6, 7, 8];
    var i: i32 = 2;
    var s: i32 = arr[i];
    s = s + arr[i + 1];
    arr[i + 2] = s;
    return arr[i] + arr[i + 1] + arr[i + 2];
}
""")

# Eighteen simultaneously live computed f64 values fill the fifteen XMM homes
# and force three to frame cells. With x=1 the values are 2..19, sum 189.
def pressure_source():
    lines = ["func probe(x: i32) -> i32 {"]
    for i in range(18):
        lines.append("    var a%d: f64 = (x as f64) + %d.0;" % (i, i + 1))
    lines.append("    var total: f64 = " +
                 " + ".join("a%d" % i for i in range(18)) + ";")
    lines.append("    return total as i32;")
    lines.append("}")
    return start("\n".join(lines), "probe(1)")


FLOAT_PRESSURE = pressure_source()

# The value f is defined in the entry block and used by the compare in the
# then-block, so it is a single-definition float live across a block boundary.
# The thirty trailing adds keep the function over the inliner's 24-instruction
# limit, so probe stays a real function and the boundary remains visible.
def cross_block_source():
    lines = [
        "func probe(x: i32) -> i32 {",
        "    var f: f64 = (x as f64) + 0.5;",
        "    var r: i32 = 0;",
        "    if (x > 0) {",
        "        if (f > 0.0) { r = 42; } else { r = 7; }",
        "    }",
    ]
    for i in range(30):
        lines.append("    var v%d: i32 = x + %d;" % (i, i + 1))
    lines.append("    var sum: i32 = " +
                 " + ".join("v%d" % i for i in range(30)) + ";")
    lines.append("    if (sum == 0) { return 123; }")
    lines.append("    return r;")
    lines.append("}")
    return start("\n".join(lines), "probe(3)")


CROSS_BLOCK = cross_block_source()

# A volatile f64 global is loaded, updated and loaded again. (1.5 + 2.5) * 2 =
# 8; neither access may become an XMM home.
VOLATILE_FLOAT = start("""var gv: volatile f64 = 1.5;
func probe() -> i32 {
    var a: f64 = gv;
    gv = a + 2.5;
    var b: f64 = gv;
    return (b * 2.0) as i32;
}
""")

CASES = [
    ("widths", WIDTHS, 0),
    ("array", ARRAY, 42),
    ("struct", STRUCT, 23),
    ("pointer", POINTER, 70),
    ("floats", FLOATS, 58),
    ("float_loop", FLOAT_LOOP, 243),
    ("float_pressure", FLOAT_PRESSURE, 189),
    ("cross_block_float", CROSS_BLOCK, 42),
    ("volatile_float", VOLATILE_FLOAT, 8),
    ("gep", GEP, 14),
]

UEFI_SOURCE = """msabi func mix(a: f64, b: f64) -> f64 {
    var x: f32 = a as f32;
    var y: f32 = b as f32;
    return (x + y) as f64;
}

msabi func efi_main(image_handle: *void, system_table: *void) -> int {
    var v: f64 = mix(1.5, 2.5);
    return v as int;
}
"""


def run(args, env, timeout=180):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=timeout)


def function_body(text, name):
    start_at = re.search(r"(?m)^%s:\n" % re.escape(name), text)
    if start_at is None:
        return None
    rest = text[start_at.end():]
    end = re.search(r"(?m)^(?:\.globl |\.weak |(?![.]L_)[A-Za-z_.][^\s:]*:\n)",
                    rest)
    return rest[:end.start()] if end else rest


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout[:400], proc.stderr[:400])


def has_i386pep():
    if shutil.which("ld") is None:
        return False
    proc = subprocess.run(["ld", "-V"], capture_output=True, text=True)
    return proc.returncode == 0 and "i386pep" in (proc.stdout + proc.stderr)


def compile_fast(tmp, name, text, no_opt=False):
    source = os.path.join(tmp, name + ".cid")
    asm = os.path.join(tmp, name + ".s")
    with open(source, "w") as handle:
        handle.write(text)
    command = [str(CIDER), "compile", "--codegen=fast"]
    if no_opt:
        command.append("--no-opt")
    command.extend(["-o", asm, source])
    proc = run(command, dict(os.environ))
    return proc, asm


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    failures = []
    with tempfile.TemporaryDirectory(prefix="cider-fast-memory-") as tmp:
        for name, source_text, expected in CASES:
            source = os.path.join(tmp, name + ".cid")
            with open(source, "w") as handle:
                handle.write(source_text)
            for label, flags in (
                    ("fast", ["--codegen=fast"]),
                    ("fast-no-opt", ["--codegen=fast", "--no-opt"]),
                    ("legacy", ["--codegen=legacy"])):
                proc = run([str(CIDER), "run"] + flags + [source], env)
                if proc.returncode != expected or proc.stdout != "" or \
                        proc.stderr != "":
                    failures.append(
                        "%s %s: %s; expected exit %d with no output" %
                        (name, label, describe(proc), expected))

        # RED shape: eighteen live computed floats fill the fifteen XMM homes,
        # and the three over-pressure values become genuine frame spill/reload
        # pairs. Before Task 12 every float operation wrote xmm0 and then a
        # slot, so no xmm1-xmm15 destination existed and the home count fails.
        proc, asm = compile_fast(tmp, "float_pressure_shape", FLOAT_PRESSURE)
        if proc.returncode != 0:
            failures.append("float_pressure shape compile: " + describe(proc))
        else:
            body = function_body(open(asm, encoding="utf-8").read(), "probe")
            if body is None:
                failures.append("float_pressure shape: probe is missing")
            else:
                homes = set(re.findall(
                    r"(?m)^  (?:movss|movsd) xmm(1[0-5]|[1-9]), ", body))
                if len(homes) < 15:
                    failures.append(
                        "float_pressure shape: only %d XMM homes used, not the "
                        "full xmm1-xmm15 row (body: %r)" %
                        (len(homes), body))
                # A value computed in xmm0 is written to a frame cell and
                # later read back from that same cell. This is a concrete
                # eviction/reload, not any incidental float memory access.
                spilled = set(re.findall(
                    r"(?m)^  movsd \[rsp \+ (\d+)\], xmm0$", body))
                reloaded = set(m.group(1) for m in re.finditer(
                    r"(?m)^  (?:movsd|addsd) (?:xmm\d+, )?\[rsp \+ "
                    r"(\d+)\]$", body))
                overlap = spilled & reloaded
                if not overlap:
                    failures.append(
                        "float_pressure shape: no value was defined into a "
                        "frame slot and reloaded from that same slot "
                        "(spilled=%r reloaded=%r body=%r)" %
                        (spilled, reloaded, body))

        # A float defined in one block and used in a successor must arrive in
        # its XMM home, not through a reload of its old frame cell.
        proc, asm = compile_fast(tmp, "cross_block_shape", CROSS_BLOCK)
        if proc.returncode != 0:
            failures.append("cross_block shape compile: " + describe(proc))
        else:
            body = function_body(open(asm, encoding="utf-8").read(), "probe")
            if body is None:
                failures.append("cross_block shape: probe is missing")
            else:
                crossing = False
                for label, chunk in re.findall(
                        r"(?ms)^(\.L_probe_\d+):\n(.*?)"
                        r"(?=^\.L_probe_\d+:|\Z)", body):
                    if label == ".L_probe_0":
                        continue
                    home = re.search(
                        r"(?m)^  movsd xmm0, xmm(1[0-5]|[1-9])$", chunk)
                    if home is None:
                        continue
                    reg = home.group(1)
                    prefix = chunk[:home.start()]
                    if re.search(r"\bxmm%s\b" % reg, prefix):
                        continue
                    if "ucomisd" in chunk[home.end():]:
                        crossing = True
                        break
                if not crossing:
                    failures.append(
                        "cross_block shape: the value live across a block "
                        "boundary was not read from an XMM home in its "
                        "successor (body: %r)" % body)

        # A volatile f64 global must stay a memory load/store even though the
        # surrounding float values use XMM homes.
        proc, asm = compile_fast(tmp, "volatile_shape", VOLATILE_FLOAT,
                                 no_opt=True)
        if proc.returncode != 0:
            failures.append("volatile shape compile: " + describe(proc))
        else:
            body = function_body(open(asm, encoding="utf-8").read(), "probe")
            if body is None:
                failures.append("volatile shape: probe is missing")
            else:
                if not re.search(
                        r"(?m)^  movsd xmm0, \[rip \+ g_gv\]$", body):
                    failures.append(
                        "volatile shape: the volatile load is not a memory "
                        "operation (body: %r)" % body)
                if not re.search(
                        r"(?m)^  mov (?:qword ptr )?\[rip \+ g_gv\], ", body):
                    failures.append(
                        "volatile shape: the volatile store is not a memory "
                        "operation (body: %r)" % body)

        # RED shape: a single-use Gep address folds into the access's memory
        # operand. Before Task 12 the emitter materialised the address in rax
        # and addressed [rax] (or [rcx]).
        proc, asm = compile_fast(tmp, "gep_shape", GEP, no_opt=True)
        if proc.returncode != 0:
            failures.append("gep shape compile: " + describe(proc))
        else:
            body = function_body(open(asm, encoding="utf-8").read(), "probe")
            if body is None:
                failures.append("gep shape: probe is missing")
            elif not re.search(
                    r"(?m)^  [^\n]*\[rsp \+ \d+ \+ r[a-z0-9]+\]$", body):
                failures.append(
                    "gep shape: the Gep address was not folded into a memory "
                    "operand (body: %r)" % body)

        # The UEFI target still falls back to legacy during bring-up; the
        # fast flag must at least compile the same text and not corrupt the
        # Microsoft x64 path. QEMU is not required for this compile check.
        if shutil.which("clang") is None or not has_i386pep():
            print("SKIP: UEFI toolchain unavailable")
        else:
            uefi_source = os.path.join(tmp, "uefi.cid")
            with open(uefi_source, "w") as handle:
                handle.write(UEFI_SOURCE)
            legacy_asm = os.path.join(tmp, "uefi-legacy.s")
            fast_asm = os.path.join(tmp, "uefi-fast.s")
            legacy = run([str(CIDER), "compile", "--target", "uefi",
                          "--codegen=legacy", "-o", legacy_asm, uefi_source],
                         env)
            fast = run([str(CIDER), "compile", "--target", "uefi",
                        "--codegen=fast", "-o", fast_asm, uefi_source], env)
            if legacy.returncode != 0:
                failures.append("uefi legacy compile: " + describe(legacy))
            if fast.returncode != 0:
                failures.append("uefi fast compile: " + describe(fast))
            if legacy.returncode == 0 and fast.returncode == 0:
                with open(legacy_asm, encoding="utf-8") as handle:
                    legacy_text = handle.read()
                with open(fast_asm, encoding="utf-8") as handle:
                    fast_text = handle.read()
                if legacy_text != fast_text:
                    failures.append(
                        "uefi fast did not fall back to legacy byte-for-byte")
                obj = os.path.join(tmp, "uefi.o")
                assembled = run(["clang", "--target=x86_64-pc-win32-coff",
                                 "-c", fast_asm, "-o", obj], env)
                if assembled.returncode != 0:
                    failures.append("uefi assemble: " + describe(assembled))

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast memory: width-correct loads/stores, GEP folding, float homes, "
          "cross-block homes, volatile memory and float pressure passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
