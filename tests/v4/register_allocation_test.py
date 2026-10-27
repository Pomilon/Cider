#!/usr/bin/env python3
"""v4 local register-allocation end-to-end test.

The unit test pins the allocator's refusal scope. This test proves the real
pipeline result: native programs with straight-line pressure, calls, loops and
phi edge copies, inline asm, volatile accesses, multi-word returns and floats
still compute the same answers after codegen stops bouncing every value
through its frame slot. It also compiles an msabi function and proves the
UEFI pool never homes a value in rdi/rsi, which are callee-saved there.
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
            if name in ("straight", "pressure", "loops", "multiword_float"):
                probe = {"straight": "compute", "pressure": "pressure",
                         "loops": "loop_swap", "multiword_float": "pair"}[name]
                body = function_body(text, probe)
                if body is None:
                    failures.append("%s: function %s is missing" % (name, probe))
                elif not home_register(body):
                    failures.append(
                        "%s: %s uses no caller-saved home register" %
                        (name, probe))
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

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("register allocation: 6 native programs and the msabi pool passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
