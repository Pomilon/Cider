#!/usr/bin/env python3
"""v4 register-allocation end-to-end test.

The unit test pins the allocator's refusal scope. This test proves the real
pipeline result: native programs with straight-line pressure, calls, loops,
phi edge copies, inline asm, volatile accesses, multi-word returns and floats
still compute the same answers after codegen stops bouncing every value
through its frame slot. It also exercises cross-block live ranges: branch
merges, loop-carried values, values live across calls (which must use the
callee-saved rbx), pressure beyond that one callee-saved home, and a value
across a volatile global access. Finally it compiles an msabi function and
proves the UEFI pool never homes a value in rdi/rsi, which are callee-saved
there, while rbx is saved/restored and the Microsoft argument row is
respected.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

CASES = {
    "straight": ("""func compute(x: i32) -> i32 {
    var a: i32 = x + 3;
    var b: i32 = a * 5;
    var c: i32 = b - 7;
    var d: i32 = c + a;
    var e: i32 = d ^ b;
    return e + c;
}

func _start() -> void {
    var code: i32 = compute(4);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 28),

    "pressure": ("""func pressure(x: i32) -> i32 {
    var a: i32 = x + 1;
    var b: i32 = x + 2;
    var c: i32 = x + 3;
    var d: i32 = x + 4;
    var e: i32 = x + 5;
    var f: i32 = x + 6;
    var g: i32 = x + 7;
    var h: i32 = x + 8;
    return a + b + c + d + e + f + g + h;
}

func _start() -> void {
    var code: i32 = pressure(2);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 52),

    "calls": ("""func bump(x: i32) -> i32 {
    return x + 1;
}

func mixed(x: i32) -> i32 {
    var a: i32 = x + 10;
    var b: i32 = bump(x);
    var c: i32 = a + b;
    var d: i32 = bump(c);
    return c * 2 + d;
}

func _start() -> void {
    var code: i32 = mixed(5);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 64),

    "loops": ("""func loop_swap(n: i32) -> i32 {
    var x: i32 = 1;
    var y: i32 = 2;
    var i: i32 = 0;
    while (i < n) {
        var t: i32 = x;
        x = y;
        y = t;
        i = i + 1;
    }
    return x * 10 + y;
}

func sum(n: i32) -> i32 {
    var s: i32 = 0;
    var i: i32 = 0;
    while (i < n) {
        s = s + i;
        i = i + 1;
    }
    return s;
}

func _start() -> void {
    var code: i32 = loop_swap(3) + sum(10);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 66),

    "asm_volatile": ("""func asm_barrier(x: i32) -> i32 {
    var a: i32 = x + 5;
    var addend: i32 = 0;
    asm { "mov {addend:q}, 7" }
    return a + addend;
}

func volatile_barrier(x: i32) -> i32 {
    var v: volatile i32 = x;
    var a: i32 = v + 1;
    v = a;
    return v + a;
}

func _start() -> void {
    var code: i32 = asm_barrier(30) + volatile_barrier(20);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 84),

    "multiword_float": ("""func pair() -> [u64; 2] {
    var r: [u64; 2] = [11, 22];
    return r;
}

func float_mix(x: f64) -> f64 {
    var a: f64 = x + 1.5;
    var b: f64 = a * 2.0;
    return b - 0.5;
}

func fadd(a: f64, b: f64) -> f64 {
    return a + b;
}

func _start() -> void {
    var p: [u64; 2] = pair();
    var s: string = "hi";
    var f: i32 = float_mix(2.0) as i32;
    var g: i32 = fadd(1.5, 2.5) as i32;
    var code: i32 = (p[0] + p[1]) as i32 + s.len as i32 + f + g;
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 45),

    "global_branch": ("""func branchy(x: i32) -> i32 {
    var base: i32 = x * 3;
    var r: i32 = 0;
    if (x > 0) { r = base + 1; } else { r = base + 2; }
    return r + base;
}

func _start() -> void {
    var code: i32 = branchy(7);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 43),

    "global_loop": ("""func loop_global(n: i32) -> i32 {
    var step: i32 = 3;
    var i: i32 = 0;
    var s: i32 = 0;
    while (i < n) {
        s = s + step;
        i = i + 1;
    }
    return s + step;
}

func _start() -> void {
    var code: i32 = loop_global(10);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 33),

    "global_call": ("""func bump2(x: i32) -> i32 {
    asm { "nop" }
    return x + 1;
}

func bump(x: i32) -> i32 {
    var t: i32 = x + 1;
    if (x == 2147483647) { return bump(x); }
    if (x > 0) { return t + bump2(x); } else { return t + 2; }
}

func keep(x: i32) -> i32 {
    var a: i32 = x + 10;
    var r: i32 = 0;
    if (x > 0) { r = a + bump(x); } else { r = a - bump(x); }
    return r + a;
}

func _start() -> void {
    var code: i32 = keep(5) + keep(-5);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 54),

    "global_pressure": ("""func callee(x: i32) -> i32 {
    asm { "nop" }
    return x + 1;
}

func pressure_global(x: i32) -> i32 {
    var a: i32 = x + 1;
    var b: i32 = x + 2;
    var r: i32 = 0;
    if (x > 0) {
        r = callee(x) + a + b;
    } else {
        r = callee(x) - a - b;
    }
    return r + a + b;
}

func _start() -> void {
    var code: i32 = pressure_global(3);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 22),

    "global_loop_call": ("""func bump(x: i32) -> i32 {
    asm { "nop" }
    return x + 1;
}

func loop_call(n: i32) -> i32 {
    var step: i32 = n + 1;
    var i: i32 = 0;
    var s: i32 = 0;
    while (i < n) {
        var t: i32 = bump(i);
        s = s + step + t;
        i = i + 1;
    }
    return s + step;
}

func _start() -> void {
    var code: i32 = loop_call(4);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 35),

    "global_volatile": ("""var gv: volatile i32 = 0;

func volatile_global(x: i32) -> i32 {
    var a: i32 = x + 4;
    var r: i32 = 0;
    if (x > 0) {
        gv = x;
        r = a + gv;
    } else {
        gv = 1;
        r = a - gv;
    }
    return r + a;
}

func _start() -> void {
    var code: i32 = volatile_global(3) + volatile_global(-3);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 18),
}

UEFI_SOURCE = """msabi func helper(x: i32) -> i32 {
    var a: i32 = x + 1;
    var b: i32 = a + 2;
    var c: i32 = b + 3;
    var d: i32 = c + 4;
    var e: i32 = d + 5;
    return e;
}

msabi func efi_main(image_handle: *void, system_table: *void) -> int {
    return helper(1);
}
"""

UEFI_GLOBAL_SOURCE = """msabi func helper(x: i32) -> i32 {
    asm { "nop" }
    return x + 1;
}

msabi func keep(x: i32) -> i32 {
    var a: i32 = x + 10;
    var r: i32 = 0;
    if (x > 0) {
        r = a + helper(x);
    } else {
        r = a - helper(x);
    }
    return r + a;
}

msabi func efi_main(image_handle: *void, system_table: *void) -> int {
    return keep(5);
}
"""

POMEOS_GLOBAL_SOURCE = """func probe() -> int {
    asm { "nop" }
    return 1;
}

func _start(argc: int, argv: **char) -> void {
    var a: int = argc + 3;
    var r: int = 0;
    if (argc > 0) {
        r = probe();
    } else {
        r = 2;
    }
    var code: int = r + a;
    if (code == 1234567) { return; }
}
"""


def run(args, timeout=180):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout[:500], proc.stderr[:500])


def function_body(text, name):
    start = re.search(r"(?m)^%s:\n" % re.escape(name), text)
    if start is None:
        return None
    rest = text[start.end():]
    # A function ends at the next module-level label or .globl. Local block
    # labels begin with ".L_" and belong to the body.
    end = re.search(r"(?m)^(?:\.globl |(?![.]L_)[A-Za-z_.][^\s:]*:\n)", rest)
    return rest[:end.start()] if end else rest


def home_register(body):
    return re.search(r"(?m)^  mov r(?:8|9|10|di|si), ", body) is not None


def saves_register(body, reg):
    return re.search(
        r"(?m)^  mov qword ptr \[rsp \+ \d+\], %s$" % reg, body) is not None


def restores_register(body, reg):
    return re.search(
        r"(?m)^  mov %s, qword ptr \[rsp \+ \d+\]$" % reg, body) is not None


def prologue_span(text, name):
    match = re.search(
        r"(?m)^" + re.escape(name) +
        r":\n  push rbp\n  mov rbp, rsp\n  sub rsp, (\d+)\n", text)
    return int(match.group(1)) if match else None


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix="cider-regalloc-") as tmp:
        for name, (source_text, expected) in CASES.items():
            source = os.path.join(tmp, name + ".cid")
            asm = os.path.join(tmp, name + ".s")
            obj = os.path.join(tmp, name + ".o")
            binary = os.path.join(tmp, name + ".bin")
            with open(source, "w") as handle:
                handle.write(source_text)

            compiled = run([CIDER, "compile", source, "-o", asm])
            if compiled.returncode != 0:
                failures.append("%s compile: %s" % (name, describe(compiled)))
                continue
            assembled = run(["as", "-o", obj, asm])
            if assembled.returncode != 0:
                failures.append("%s assemble: %s" % (name, describe(assembled)))
                continue
            linked = run(["ld", "-nostdlib", "-e", "_start", "-o", binary, obj])
            if linked.returncode != 0:
                failures.append("%s link: %s" % (name, describe(linked)))
                continue
            executed = run([binary])
            if executed.returncode != expected or executed.stderr != "":
                failures.append(
                    "%s run: exit %r, stderr %r; expected %d" %
                    (name, executed.returncode, executed.stderr, expected))

            text = open(asm).read()
            if name in ("straight", "pressure", "loops", "multiword_float",
                        "global_branch", "global_loop"):
                probe = {"straight": "compute", "pressure": "pressure",
                         "loops": "loop_swap", "multiword_float": "pair",
                         "global_branch": "branchy",
                         "global_loop": "loop_global"}[name]
                body = function_body(text, probe)
                if body is None:
                    failures.append("%s: function %s is missing" % (name, probe))
                elif not home_register(body):
                    failures.append(
                        "%s: %s uses no caller-saved home register" %
                        (name, probe))
            if name == "global_branch":
                body = function_body(text, "branchy") or ""
                if not home_register(body):
                    failures.append(
                        "global_branch: the pre-branch value has no home")
            if name == "global_loop":
                body = function_body(text, "loop_global") or ""
                if not home_register(body):
                    failures.append(
                        "global_loop: the preheader value has no home")
            if name == "global_call":
                for probe in ("keep", "bump"):
                    body = function_body(text, probe) or ""
                    if not re.search(r"(?m)^  mov rbx, ", body):
                        failures.append(
                            "global_call: %s never homes a value in rbx" %
                            probe)
                    if not saves_register(body, "rbx"):
                        failures.append(
                            "global_call: %s does not save rbx" % probe)
                    if not restores_register(body, "rbx"):
                        failures.append(
                            "global_call: %s does not restore rbx" % probe)
            if name == "global_pressure":
                body = function_body(text, "pressure_global") or ""
                if not re.search(r"(?m)^  mov rbx, ", body):
                    failures.append(
                        "global_pressure: no cross-call value got rbx")
                if "[rsp + " not in body:
                    failures.append(
                        "global_pressure: the second cross-call value "
                        "did not fall back to its slot")
                if not saves_register(body, "rbx") or \
                        not restores_register(body, "rbx"):
                    failures.append(
                        "global_pressure: rbx is not saved and restored")
            if name == "global_volatile":
                body = function_body(text, "volatile_global") or ""
                if not re.search(r"(?m)^  mov rbx, ", body):
                    failures.append(
                        "global_volatile: the value across the volatile "
                        "access is not in rbx")
                if not re.search(r"\[rip \+ [^\]]*gv\]", body):
                    failures.append(
                        "global_volatile: the volatile global access is gone")
                if not saves_register(body, "rbx") or \
                        not restores_register(body, "rbx"):
                    failures.append(
                        "global_volatile: rbx is not saved and restored")
            if name == "global_loop_call":
                body = function_body(text, "loop_call") or ""
                if not re.search(r"(?m)^  mov rbx, ", body):
                    failures.append(
                        "global_loop_call: the loop value across the call "
                        "is not in rbx")
                if "call bump" not in body:
                    failures.append(
                        "global_loop_call: the loop call is gone")
                if not saves_register(body, "rbx") or \
                        not restores_register(body, "rbx"):
                    failures.append(
                        "global_loop_call: rbx is not saved and restored")
            if name == "pressure":
                body = function_body(text, "pressure") or ""
                if "[rsp + " not in body:
                    failures.append(
                        "pressure: the over-pressure case has no stack spill")
            if name == "asm_volatile":
                body = function_body(text, "asm_barrier") or ""
                if "mov {addend:q}" in body:
                    failures.append("asm_volatile: the placeholder was not substituted")

        # The msabi pool excludes rdi/rsi. A function that is not the entry
        # must show only r8/r9/r10 when it uses a home at all.
        uefi_source = os.path.join(tmp, "uefi.cid")
        uefi_asm = os.path.join(tmp, "uefi.s")
        with open(uefi_source, "w") as handle:
            handle.write(UEFI_SOURCE)
        compiled = run([CIDER, "compile", "--target", "uefi",
                        uefi_source, "-o", uefi_asm])
        if compiled.returncode != 0:
            failures.append("uefi compile: " + describe(compiled))
        else:
            text = open(uefi_asm).read()
            body = function_body(text, "helper")
            if body is None:
                failures.append("uefi: helper is missing")
            else:
                if not home_register(body):
                    failures.append("uefi: helper uses no home register")
                if re.search(r"\brdi\b|\brsi\b", body):
                    failures.append(
                        "uefi: an msabi function named the callee-saved rdi/rsi")

        # A cross-block msabi value live across an msabi call must use the
        # callee-saved rbx, save/restore it, and keep the Microsoft argument
        # row (rcx/rdx/r8/r9). It must still never home rdi/rsi.
        uefi_global_source = os.path.join(tmp, "uefi_global.cid")
        uefi_global_asm = os.path.join(tmp, "uefi_global.s")
        with open(uefi_global_source, "w") as handle:
            handle.write(UEFI_GLOBAL_SOURCE)
        compiled = run([CIDER, "compile", "--target", "uefi",
                        uefi_global_source, "-o", uefi_global_asm])
        if compiled.returncode != 0:
            failures.append("uefi global compile: " + describe(compiled))
        else:
            asm_run = run(["as", "-o", uefi_global_asm + ".o",
                           uefi_global_asm])
            if asm_run.returncode != 0:
                failures.append("uefi global assemble: " + describe(asm_run))
            text = open(uefi_global_asm).read()
            body = function_body(text, "keep")
            if body is None:
                failures.append("uefi global: keep is missing")
            else:
                if not re.search(r"(?m)^  mov rbx, ", body):
                    failures.append(
                        "uefi global: the value across the msabi call "
                        "is not in rbx")
                if not saves_register(body, "rbx"):
                    failures.append("uefi global: keep does not save rbx")
                if not restores_register(body, "rbx"):
                    failures.append("uefi global: keep does not restore rbx")
                if re.search(r"\brdi\b|\brsi\b", body):
                    failures.append(
                        "uefi global: an msabi home used the callee-saved "
                        "rdi/rsi instead of rbx")
                if not re.search(r"(?m)^  mov rcx, \[rsp \+ ", body):
                    failures.append(
                        "uefi global: the msabi call did not use rcx")
                if "call helper" not in body:
                    failures.append("uefi global: the msabi call vanished")

        # PomeOS enters _start call-like (16-byte aligned), so a frame that
        # grows a callee-save cell must still be a 16-multiple. The entry
        # must keep receiving argc in rdi and argv in rsi.
        pomeos_source = os.path.join(tmp, "pomeos_global.cid")
        pomeos_asm = os.path.join(tmp, "pomeos_global.s")
        with open(pomeos_source, "w") as handle:
            handle.write(POMEOS_GLOBAL_SOURCE)
        compiled = run([CIDER, "compile", "--target", "pomeos",
                        pomeos_source, "-o", pomeos_asm])
        if compiled.returncode != 0:
            failures.append("pomeos global compile: " + describe(compiled))
        else:
            text = open(pomeos_asm).read()
            span = prologue_span(text, "_start")
            if span is None:
                failures.append("pomeos global: _start prologue is missing")
            elif span % 16 != 0:
                failures.append(
                    "pomeos global: _start frame is %d bytes; expected a "
                    "16-byte multiple" % span)
            body = function_body(text, "_start") or ""
            if not saves_register(body, "rbx") or \
                    not restores_register(body, "rbx"):
                failures.append(
                    "pomeos global: the entry does not save and restore rbx")
            if not re.search(r"mov \[rsp \+ \d+\], rdi", body):
                failures.append(
                    "pomeos global: the entry no longer receives argc in rdi")
            if not re.search(r"mov \[rsp \+ \d+\], rsi", body):
                failures.append(
                    "pomeos global: the entry no longer receives argv in rsi")

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("register allocation: 12 native programs, global live ranges, "
          "and the msabi pool passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
