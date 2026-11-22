#!/usr/bin/env python3
"""Task 13's fast-backend ABI end-to-end test.

The fast emitter owns the SysV call row, the Microsoft x64 positional row,
multi-word returns, fixed-register sequences (div/rem and variable shifts),
indirect calls, and the nonvolatile register set.  Every value case compiles
with ``--codegen=fast`` (optimised and unoptimised), assembles, links and is
run for an exact exit status; the legacy emitter is run beside it so a fast
regression cannot hide behind the old path.

The assembly-shape legs pin the parts of the ABI that a wrong exit status can
miss: the seventh integer word going to the outgoing area, a two-word return
arriving in rax/rdx and a float return in xmm0, a cross-call value using a
saved callee-saved GPR, ``al`` carrying the SysV SSE count, an indirect call
through a staged pointer, a frame-sourced float move that must wait for a
homed argument (the parallel-move deordering regression), a native msabi
variadic duplicate, and a full 128-bit Microsoft x64 xmm6-xmm15 save.

The div/rem and shift cases check the allocator's current safe shape and the
exact results.  The whole-function allocator refuses to home rax/rdx/rcx
across those instructions, so the emitter's fixed-register guard cannot be
reached today; it is future-proofing, not claimed coverage.  See the report.

The Microsoft x64 XMM leg is the RED case for the old emitter: it saved the
scalar low 64 bits with ``movsd``, which clears the upper half of a nonvolatile
XMM register.  The test seeds xmm6 with a 128-bit pattern, calls an msabi
function that uses xmm6 as a live float home, and checks both halves afterwards.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))
LIB = os.path.join(REPO, "modules")
HELPER = os.path.join(REPO, "tests", "v4", "c", "varargs_helper.c")


def start(body, call):
    return body + """
func _start() -> void {
    var code: i32 = %s;
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""" % call


# Seven integer arguments: the first six take rdi/rsi/rdx/rcx/r8/r9 and the
# seventh is staged in the caller's outgoing word at [rsp + 0].
INT7 = start("""func add7(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64,
                g: i64) -> i64 {
    return a + b + c + d + e + f + g;
}
""", "add7(1, 2, 3, 4, 5, 6, 7) as i32")

# SysV uses separate integer and SSE counters: four integer words take
# rdi/rsi/rdx/rcx and four floats take xmm0-xmm3.  The double answer travels
# home in xmm0 and is converted for the exit status.
FLOAT_MIX = start("""func mix(a: i64, b: f64, c: i64, d: f64, e: i64, f: f64,
                g: i64, h: f64) -> f64 {
    return (a as f64) + b * 2.0 + (c as f64) + d * 2.0
         + (e as f64) + f * 2.0 + (g as f64) + h * 2.0;
}
""", "mix(1, 1.5, 2, 2.5, 3, 3.5, 4, 4.5) as i32")

# A 16-byte aggregate returns word zero in rax and word one in rdx; a float
# return arrives in xmm0.  (11 + 22) + (9.0 * 0.5 truncated to 4) = 37.
RETURNS = start("""func pair() -> [u64; 2] {
    var r: [u64; 2] = [11, 22];
    return r;
}

func half(x: f64) -> f64 { return x * 0.5; }
""", "((pair()[0] + pair()[1]) as i32) + (half(9.0) as i32)")

# `keep` is produced by one call and consumed after a second call, so a
# correct caller must carry it in a callee-saved register across the second
# call.  41 + 2 = 43.
ACROSS_CALL = start("""func one(x: i64) -> i64 { return x + 1; }
func two(x: i64) -> i64 { return x + 2; }
""", "(one(40) + two(0)) as i32")

# Division and remainder both use rax as the dividend and rdx as the high
# half.  `live` is defined before them and consumed afterwards: (58 + 3) * 100
# + 2 = 6102 -> exit 214.
DIV_REM = start("""func divide(x: i64, y: i64) -> i64 {
    var live: i64 = x * 3 + 7;
    var q: i64 = x / y;
    var r: i64 = x % y;
    return (live + q) * 100 + r;
}
""", "divide(17, 5) as i32")

# A variable shift takes its count through cl while rax carries the value.
# (3 * 2 + 1) * 100 + (3 << 4) = 748 -> exit 236.
SHIFT = start("""func shift(x: i64, n: i64) -> i64 {
    var live: i64 = x * 2 + 1;
    var s: i64 = x << n;
    return live * 100 + s;
}
""", "shift(3, 4) as i32")

# The call target lives in a pointer value, so the emitter stages it in rax
# (or r11 for a variadic indirect call) and calls through that register.
INDIRECT = start("""func add(a: i64, b: i64) -> i64 { return a + b; }

func via(x: i64, y: i64) -> i64 {
    var p: *func(i64, i64) -> i64 = &add;
    return p(x, y);
}
""", "via(20, 22) as i32")

# Argument staging regression: q is homed in xmm1 and is the third argument,
# while p1 is a parameter (frame-sourced) and owns the second XMM destination,
# also xmm1.  The p1 move must wait until q has been read from xmm1, otherwise
# the call sees p1 twice.  p0=1, p1=2, p2=3, q=x*2=8:
# 1 + 2*10 + 8*100 + 3*1000 = 3821 -> exit 237.
FLOAT_STAGE_CLOBBER = """func fp(a: f64, b: f64, c: f64, d: f64) -> f64 {
    asm { "nop" }
    return a + b * 10.0 + c * 100.0 + d * 1000.0;
}

func probe(p0: f64, p1: f64, p2: f64, x: f64) -> i32 {
    var q: f64 = x * 2.0;
    var r: f64 = fp(p0, p1, q, p2);
    return r as i32;
}

func _start() -> void {
    var p: *func(f64, f64, f64, f64) -> i32 = &probe;
    var code: i32 = p(1.0, 2.0, 3.0, 4.0);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

# An msabi argument cycle: q is homed in xmm1 (third argument, destination
# xmm2) and w in xmm2 (second argument, destination xmm1), while keep is live
# across the call in nonvolatile xmm6.  The cycle must break through a
# caller-saved scratch and must not disturb xmm6.  keep=104, fp=1+120+800+3000
# = 3921, total 4025 -> exit 185.
MS_CYCLE = """msabi func fp(a: f64, b: f64, c: f64, d: f64) -> f64 {
    asm { "nop" }
    return a + b * 10.0 + c * 100.0 + d * 1000.0;
}

msabi func probe(p0: f64, p1: f64, p2: f64, x: f64) -> i32 {
    var keep: f64 = x + 100.0;
    var q: f64 = x * 2.0;
    var w: f64 = x * 3.0;
    var r: f64 = fp(p0, w, q, p2);
    return (keep + r) as i32;
}

func _start() -> void {
    var p: *msabi func(f64, f64, f64, f64) -> i32 = &probe;
    var code: i32 = p(1.0, 2.0, 3.0, 4.0);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

# A native msabi variadic called from another msabi function.  The variadic
# float belongs in positional slot one, so it travels in xmm1 and is duplicated
# into rdx; the following variadic int takes r8.  The callee reads both back
# through va_arg: 2.5 * 10 + 40 = 65.
MS_VARIADIC = """from std.varargs import VaList, va_start, va_end, va_arg_i32,
    va_arg_f64;

msabi func mixed(n: i32, ...) -> f64 {
    var ap: VaList = uninit;
    va_start(&ap);
    var d: f64 = va_arg_f64(&ap);
    var i: i32 = va_arg_i32(&ap);
    va_end(&ap);
    return d * 10.0 + (i as f64);
}

msabi func call_it() -> i32 {
    return mixed(2, 2.5, 40) as i32;
}

func _start() -> void {
    var code: i32 = call_it();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

# A Microsoft x64 function keeps xmm6-xmm15 across calls.  Ten computed floats
# are live across the helper call, so the fast float allocator homes them in
# xmm6-xmm15 and the prologue/epilogue must preserve all 128 bits.  The caller
# seeds xmm6 with a non-zero upper half and checks both halves after the call.
# 2+3+...+11 plus helper(1.0)=1.5 is 66.5 -> 66 when the register survives.
MSXMM = """msabi func helper(x: f64) -> f64 { return x + 0.5; }

msabi func work(x: f64) -> f64 {
    var a0: f64 = x + 1.0;
    var a1: f64 = x + 2.0;
    var a2: f64 = x + 3.0;
    var a3: f64 = x + 4.0;
    var a4: f64 = x + 5.0;
    var a5: f64 = x + 6.0;
    var a6: f64 = x + 7.0;
    var a7: f64 = x + 8.0;
    var a8: f64 = x + 9.0;
    var a9: f64 = x + 10.0;
    var sink: f64 = helper(x);
    return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7 + a8 + a9 + sink;
}

func _start() -> void {
    var bad: i32 = 0;
    asm {
        "mov rax, 0x1122334455667788"
        "movq xmm6, rax"
        "mov rax, 0x99aabbccddeeff00"
        "pinsrq xmm6, rax, 1"
    }
    var y: f64 = work(1.0);
    asm {
        "movq rax, xmm6"
        "mov rcx, 0x1122334455667788"
        "cmp rax, rcx"
        "setne {bad:b}"
        "pextrq rax, xmm6, 1"
        "mov rcx, 0x99aabbccddeeff00"
        "cmp rax, rcx"
        "setne cl"
        "or {bad:b}, cl"
    }
    var code: i32 = (y as i32) + bad * 100;
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

# A Cider caller into gcc's C variadic helpers: direct and spilled integer
# words, eight f64 registers (the al count), pointer words and the indirect
# paths.  The program exits with the first failing subtest number, or 0.
C_VARIADIC = """extern func c_sum(n: i32, ...) -> i64;
extern func c_sum_f64(n: i32, ...) -> f64;
extern func c_sum_ptr(n: i32, ...) -> i64;

var first: i64 = 20;
var second: i64 = 22;

func _start() -> void {
    var direct: i64 = c_sum(4, 10, 12, 15, 5);
    var spilled: i64 = c_sum(8, 1, 2, 3, 4, 5, 6, 7, 14);
    var doubles: i64 = c_sum_f64(3, 1.5, 2.25, 3.25) as i64;
    var many: i64 = c_sum_f64(8, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0,
                              14.0) as i64;
    var pointers: i64 = c_sum_ptr(2, &first, &second);
    var p: *func(i32, ...) -> i64 = &c_sum;
    var indirect: i64 = p(3, 10, 20, 12);
    var bad: i32 = 0;
    if (direct != 42) { bad = 1; }
    if (spilled != 42) { bad = 2; }
    if (doubles != 7) { bad = 3; }
    if (many != 42) { bad = 4; }
    if (pointers != 42) { bad = 5; }
    if (indirect != 42) { bad = 6; }
    asm { "mov rax, 60" "mov edi, {bad}" "syscall" }
}
"""

# The reverse direction: gcc calls a Cider-defined variadic.  `_start` remains
# the Cider entry, and the C caller only supplies the missing call.
CIDER_VARIADIC = """from std.varargs import VaList, va_start, va_end, va_arg_i64;

export func sum(n: i32, ...) -> i64 {
    var ap: VaList = uninit;
    va_start(&ap);
    var total: i64 = 0;
    var i: i32 = 0;
    while (i < n) {
        total = total + va_arg_i64(&ap);
        i = i + 1;
    }
    va_end(&ap);
    return total;
}

extern func c_call_sum() -> i64;

func _start() -> void {
    var bad: i32 = 0;
    if (c_call_sum() != 42) { bad = 1; }
    asm { "mov rax, 60" "mov edi, {bad}" "syscall" }
}
"""

C_CALLER = """#include <stdint.h>
extern int64_t sum(int n, ...);
int64_t c_call_sum(void) {
    return sum(8, 1, 2, 3, 4, 5, 6, 7, 14);
}
"""

# A naked C callee writes every caller-saved register and returns 7.  The
# Cider caller keeps eight values live across it; 10+...+80+7 = 367 -> 111.
CLOBBER_C = """#include <stdint.h>
__attribute__((naked)) int64_t clobber_all(void) {
    __asm__(
        "mov $0x1111111111111111, %rax\\n"
        "mov $0x2222222222222222, %rcx\\n"
        "mov $0x3333333333333333, %rdx\\n"
        "mov $0x4444444444444444, %rsi\\n"
        "mov $0x5555555555555555, %rdi\\n"
        "mov $0x6666666666666666, %r8\\n"
        "mov $0x7777777777777777, %r9\\n"
        "mov $0x8888888888888888, %r10\\n"
        "mov $0x9999999999999999, %r11\\n"
        "mov $7, %rax\\n"
        "ret\\n");
}
"""

CLOBBER = start("""extern func clobber_all() -> i64;

func live_sum() -> i64 {
    var a: i64 = 10;
    var b: i64 = 20;
    var c: i64 = 30;
    var d: i64 = 40;
    var e: i64 = 50;
    var f: i64 = 60;
    var g: i64 = 70;
    var h: i64 = 80;
    return a + b + c + d + e + f + g + h + clobber_all();
}
""", "live_sum() as i32")

# The callee observes the SysV entry alignment: after its call pushed the
# return address, rsp is 8 mod 16.  A misaligned caller is rejected.
ALIGN_C = """#include <stdint.h>
__attribute__((naked)) int64_t check_align(void) {
    __asm__(
        "mov %rsp, %rax\\n"
        "and $15, %eax\\n"
        "cmp $8, %eax\\n"
        "jne bad\\n"
        "mov $1, %rax\\n"
        "ret\\n"
        "bad:\\n"
        "xor %eax, %eax\\n"
        "ret\\n");
}
"""

ALIGN = """extern func check_align() -> i64;

func _start() -> void {
    var ok: i64 = check_align();
    var code: i32 = 0;
    if (ok != 1) { code = 9; }
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

VALUE_CASES = [
    ("int7_stack", INT7, 28),
    ("float_mix", FLOAT_MIX, 34),
    ("returns", RETURNS, 37),
    ("across_call", ACROSS_CALL, 43),
    ("div_rem", DIV_REM, 214),
    ("shift", SHIFT, 236),
    ("indirect", INDIRECT, 42),
    ("float_stage_clobber", FLOAT_STAGE_CLOBBER, 237),
    ("msabi_cycle", MS_CYCLE, 185),
    ("msabi_variadic", MS_VARIADIC, 65),
    ("msabi_xmm", MSXMM, 66),
]


def run(args, env=None, timeout=180):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=timeout)


def read_text(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout[:400], proc.stderr[:400])


def function_body(text, name):
    start_at = re.search(r"(?m)^%s:\n" % re.escape(name), text)
    if start_at is None:
        return None
    rest = text[start_at.end():]
    end = re.search(r"(?m)^(?:\.globl |\.weak |(?![.]L_)[A-Za-z_.][^\s:]*:\n)",
                    rest)
    return rest[:end.start()] if end else rest


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    for tool in ("gcc", "as", "ld"):
        if shutil.which(tool) is None:
            print("SKIP: %s is unavailable" % tool)
            return 77

    env = dict(os.environ)
    env.setdefault("CIDER_LIB", LIB)
    failures = []

    def fail(text):
        failures.append(text)

    with tempfile.TemporaryDirectory(prefix="cider-fast-abi-") as tmp:
        def source(name, text):
            path = os.path.join(tmp, name + ".cid")
            with open(path, "w") as handle:
                handle.write(text)
            return path

        def compile_fast(path, asm, no_opt=False):
            command = [CIDER, "compile", "--codegen=fast"]
            if no_opt:
                command.append("--no-opt")
            command.extend(["-o", asm, path])
            return run(command, env)

        # --- Value legs ---------------------------------------------------
        for name, text, expected in VALUE_CASES:
            path = source(name, text)
            for label, flags in (
                    ("fast", ["--codegen=fast"]),
                    ("fast-no-opt", ["--codegen=fast", "--no-opt"]),
                    ("legacy", ["--codegen=legacy"])):
                got = run([CIDER, "run"] + flags + [path], env)
                if (got.returncode, got.stdout, got.stderr) != (expected, "",
                                                               ""):
                    fail("%s %s: %s; expected exit %d with no output" %
                         (name, label, describe(got), expected))

        # --- Seven arguments: the seventh is in the outgoing area ---------
        path = source("int7_shape", INT7)
        asm = os.path.join(tmp, "int7_shape.s")
        built = compile_fast(path, asm, no_opt=True)
        if built.returncode != 0:
            fail("int7 shape compile: " + describe(built))
        else:
            body = function_body(read_text(asm), "_start")
            if body is None:
                fail("int7 shape: _start is missing")
            else:
                if "mov qword ptr [rsp + 0], " not in body:
                    fail("int7 shape: the seventh word is not staged in the "
                         "outgoing area")
                if "call add7" not in body:
                    fail("int7 shape: the seven-argument call is missing")

        # --- Two-word and float returns -----------------------------------
        path = source("returns_shape", RETURNS)
        asm = os.path.join(tmp, "returns_shape.s")
        built = compile_fast(path, asm, no_opt=True)
        if built.returncode != 0:
            fail("returns shape compile: " + describe(built))
        else:
            text = read_text(asm)
            if "mov rdx, rsi" not in text:
                fail("returns shape: a two-word return did not come home "
                     "through rdx")
            if not re.search(r"(?m)^  movsd xmm0, ", text):
                fail("returns shape: the float return did not use xmm0")

        # --- Callee-saved use across a call --------------------------------
        path = source("across_shape", ACROSS_CALL)
        asm = os.path.join(tmp, "across_shape.s")
        built = compile_fast(path, asm)
        if built.returncode != 0:
            fail("across_call shape compile: " + describe(built))
        else:
            body = function_body(read_text(asm), "_start")
            if body is None:
                fail("across_call shape: _start is missing")
            else:
                saved = re.search(
                    r"(?m)^  mov qword ptr \[rsp \+ \d+\], (r(?:bx|12|13|14|15))$",
                    body)
                if saved is None:
                    fail("across_call shape: no callee-saved home was saved "
                         "(body: %r)" % body)
                elif not re.search(
                        r"(?m)^  mov %s, qword ptr \[rsp \+ \d+\]$" %
                        saved.group(1), body):
                    fail("across_call shape: %s is not restored" %
                         saved.group(1))

        # --- Frame-sourced move waits for a homed argument -----------------
        # q lives in xmm1; the second argument's destination is also xmm1.
        # The q move (xmm2 <- xmm1) must precede the p1 move (xmm1 <- slot).
        path = source("float_stage_shape", FLOAT_STAGE_CLOBBER)
        asm = os.path.join(tmp, "float_stage_shape.s")
        built = compile_fast(path, asm)
        if built.returncode != 0:
            fail("float staging shape compile: " + describe(built))
        else:
            body = function_body(read_text(asm), "probe")
            if body is None:
                fail("float staging shape: probe is missing")
            else:
                read = re.search(r"(?m)^  movsd xmm2, xmm1$", body)
                clobber = re.search(
                    r"(?m)^  movsd xmm1, \[rsp \+ \d+\]$", body)
                if read is None or clobber is None:
                    fail("float staging shape: expected the homed q read and "
                         "the frame-sourced p1 move (body: %r)" % body)
                elif read.start() > clobber.start():
                    fail("float staging shape: the frame-sourced p1 move "
                         "clobbered xmm1 before q was read")

        # --- Native msabi variadic float duplication -----------------------
        path = source("msabi_variadic_shape", MS_VARIADIC)
        asm = os.path.join(tmp, "msabi_variadic_shape.s")
        built = compile_fast(path, asm, no_opt=True)
        if built.returncode != 0:
            fail("msabi variadic shape compile: " + describe(built))
        else:
            body = function_body(read_text(asm), "call_it")
            if body is None:
                fail("msabi variadic shape: call_it is missing")
            else:
                if "movq rdx, xmm1" not in body:
                    fail("msabi variadic shape: the float was not duplicated "
                         "into its positional GPR (body: %r)" % body)
                if not re.search(r"(?m)^  call mixed$", body):
                    fail("msabi variadic shape: call_it does not call mixed")

        # --- msabi argument cycle keeps nonvolatile xmm6 -------------------
        path = source("msabi_cycle_shape", MS_CYCLE)
        asm = os.path.join(tmp, "msabi_cycle_shape.s")
        built = compile_fast(path, asm)
        if built.returncode != 0:
            fail("msabi cycle shape compile: " + describe(built))
        else:
            body = function_body(open(asm, encoding="utf-8").read(), "probe")
            if body is None:
                fail("msabi cycle shape: probe is missing")
            else:
                bridge = re.search(r"(?m)^  movsd (xmm\d+), xmm2$", body)
                if bridge is None:
                    fail("msabi cycle shape: the xmm1/xmm2 cycle was not "
                         "bridged (body: %r)" % body)
                else:
                    scratch = bridge.group(1)
                    if scratch in ("xmm6", "xmm7", "xmm8", "xmm9", "xmm10",
                                   "xmm11", "xmm12", "xmm13", "xmm14",
                                   "xmm15"):
                        fail("msabi cycle shape: scratch %s is a nonvolatile "
                             "home" % scratch)
                    if not re.search(r"(?m)^  movsd xmm1, %s$" % scratch,
                                     body):
                        fail("msabi cycle shape: the cycle did not read "
                             "back through its scratch")
                if "movdqu xmmword ptr [rsp + " not in body:
                    fail("msabi cycle shape: xmm6 was not saved at full width")
                if not re.search(r"(?m)^  movdqu xmm6, xmmword ptr \[rsp \+ ",
                                 body):
                    fail("msabi cycle shape: xmm6 was not restored")

        # --- Fixed-register sequences --------------------------------------
        for name, text, opcode in (
                ("div_shape", DIV_REM, "idiv"),
                ("shift_shape", SHIFT, "shl")):
            path = source(name, text)
            asm = os.path.join(tmp, name + ".s")
            built = compile_fast(path, asm, no_opt=True)
            if built.returncode != 0:
                fail("%s compile: %s" % (name, describe(built)))
                continue
            body = function_body(read_text(asm),
                                 "divide" if opcode == "idiv" else "shift")
            if body is None:
                fail("%s: the probe is missing" % name)
                continue
            if opcode == "idiv":
                if not re.search(r"(?m)^  idiv qword ptr \[rsp \+ \d+\]$",
                                 body):
                    fail("div_shape: integer division is not using its "
                         "memory operand")
                if not re.search(r"(?m)^  cqo$", body):
                    fail("div_shape: the dividend is not sign-extended to "
                         "rdx")
            else:
                if not re.search(r"(?m)^  mov rcx, \[rsp \+ \d+\]$", body):
                    fail("shift_shape: the count is not loaded into rcx")
                if not re.search(r"(?m)^  (?:shl|sar|shr) [a-z0-9]+, cl$",
                                 body):
                    fail("shift_shape: the variable shift does not use cl")

        # --- Indirect call -------------------------------------------------
        path = source("indirect_shape", INDIRECT)
        asm = os.path.join(tmp, "indirect_shape.s")
        built = compile_fast(path, asm, no_opt=True)
        if built.returncode != 0:
            fail("indirect shape compile: " + describe(built))
        else:
            body = function_body(read_text(asm), "via")
            if body is None:
                fail("indirect shape: via is missing")
            elif not re.search(r"(?m)^  call r(?:ax|11)$", body):
                fail("indirect shape: the pointer call did not use a staged "
                     "register (body: %r)" % body)

        # --- Stack alignment ----------------------------------------------
        path = source("align_shape", INT7)
        asm = os.path.join(tmp, "align_shape.s")
        built = compile_fast(path, asm, no_opt=True)
        if built.returncode != 0:
            fail("alignment shape compile: " + describe(built))
        else:
            text = read_text(asm)
            for name, want_odd in (("_start", True), ("add7", False)):
                body = function_body(text, name)
                if body is None:
                    fail("alignment shape: %s is missing" % name)
                    continue
                match = re.search(r"(?m)^  sub rsp, (\d+)$", body)
                if match is None:
                    fail("alignment shape: %s has no frame adjustment" % name)
                    continue
                size = int(match.group(1))
                # `_start` enters with rsp 16-aligned; after push rbp its
                # span carries the extra word.  A normal function enters 8
                # mod 16, so push rbp makes a 16-multiple span aligned.
                if want_odd and size % 16 != 8:
                    fail("alignment shape: _start span %d is not 8 mod 16" %
                         size)
                if not want_odd and size % 16 != 0:
                    fail("alignment shape: %s span %d is not a 16-multiple" %
                         (name, size))

        # --- C variadic called from Cider ---------------------------------
        helper_obj = os.path.join(tmp, "varargs_helper.o")
        compiled = run([
            "gcc", "-c", "-fno-stack-protector", "-o", helper_obj, HELPER,
        ])
        if compiled.returncode != 0:
            fail("gcc helper compile: " + describe(compiled))
        else:
            path = source("c_variadic", C_VARIADIC)
            asm = os.path.join(tmp, "c_variadic.s")
            obj = os.path.join(tmp, "c_variadic.o")
            binary = os.path.join(tmp, "c_variadic.bin")
            built = compile_fast(path, asm)
            if built.returncode != 0:
                fail("C variadic caller compile: " + describe(built))
            else:
                text = read_text(asm)
                if "mov al," not in text:
                    fail("C variadic caller emitted no SysV al count")
                assembled = run(["as", "-o", obj, asm])
                if assembled.returncode != 0:
                    fail("C variadic caller assemble: " + describe(assembled))
                else:
                    linked = run([
                        "ld", "-nostdlib", "-e", "_start", "-o", binary,
                        obj, helper_obj,
                    ])
                    if linked.returncode != 0:
                        fail("Cider-to-C variadic link: " +
                             describe(linked))
                    else:
                        executed = run([binary])
                        if executed.returncode != 0:
                            fail("Cider-to-C variadic: " +
                                 describe(executed))

        # --- Cider variadic called from C ---------------------------------
        path = source("cider_variadic", CIDER_VARIADIC)
        c_caller = os.path.join(tmp, "c_caller.c")
        with open(c_caller, "w") as handle:
            handle.write(C_CALLER)
        sum_obj = os.path.join(tmp, "sum.o")
        c_obj = os.path.join(tmp, "c_caller.o")
        binary = os.path.join(tmp, "cider_variadic.bin")
        built = run([
            CIDER, "compile", "-c", "--module", "main", "--codegen=fast",
            path, "-o", sum_obj,
        ], env)
        if built.returncode != 0:
            fail("Cider variadic compile: " + describe(built))
        compiled = run([
            "gcc", "-c", "-fno-stack-protector", "-o", c_obj, c_caller,
        ])
        if compiled.returncode != 0:
            fail("gcc caller compile: " + describe(compiled))
        if os.path.exists(sum_obj) and os.path.exists(c_obj):
            linked = run([
                "ld", "-nostdlib", "-e", "_start", "-o", binary,
                sum_obj, c_obj,
            ])
            if linked.returncode != 0:
                fail("C-to-Cider variadic link: " + describe(linked))
            else:
                executed = run([binary])
                if executed.returncode != 0:
                    fail("C-to-Cider variadic: " + describe(executed))

        # --- A callee clobbering every caller-saved register ---------------
        clobber_c = os.path.join(tmp, "clobber.c")
        with open(clobber_c, "w") as handle:
            handle.write(CLOBBER_C)
        clobber_obj = os.path.join(tmp, "clobber.o")
        compiled = run([
            "gcc", "-c", "-O0", "-fno-stack-protector", "-o", clobber_obj,
            clobber_c,
        ])
        if compiled.returncode != 0:
            fail("clobber helper compile: " + describe(compiled))
        else:
            path = source("clobber", CLOBBER)
            asm = os.path.join(tmp, "clobber.s")
            obj = os.path.join(tmp, "clobber_caller.o")
            binary = os.path.join(tmp, "clobber.bin")
            built = compile_fast(path, asm)
            if built.returncode != 0:
                fail("clobber caller compile: " + describe(built))
            else:
                assembled = run(["as", "-o", obj, asm])
                if assembled.returncode != 0:
                    fail("clobber caller assemble: " + describe(assembled))
                else:
                    linked = run([
                        "ld", "-nostdlib", "-e", "_start", "-o", binary,
                        obj, clobber_obj,
                    ])
                    if linked.returncode != 0:
                        fail("clobber link: " + describe(linked))
                    else:
                        executed = run([binary])
                        if executed.returncode != 111:
                            fail("deep clobber: %s; expected exit 111" %
                                 describe(executed))

        # --- Alignment observed by a real C callee ------------------------
        align_c = os.path.join(tmp, "align.c")
        with open(align_c, "w") as handle:
            handle.write(ALIGN_C)
        align_obj = os.path.join(tmp, "align.o")
        compiled = run([
            "gcc", "-c", "-O0", "-fno-stack-protector", "-o", align_obj,
            align_c,
        ])
        if compiled.returncode != 0:
            fail("alignment helper compile: " + describe(compiled))
        else:
            path = source("align", ALIGN)
            asm = os.path.join(tmp, "align.s")
            obj = os.path.join(tmp, "align_caller.o")
            binary = os.path.join(tmp, "align.bin")
            built = compile_fast(path, asm)
            if built.returncode != 0:
                fail("alignment caller compile: " + describe(built))
            else:
                assembled = run(["as", "-o", obj, asm])
                if assembled.returncode != 0:
                    fail("alignment caller assemble: " + describe(assembled))
                else:
                    linked = run([
                        "ld", "-nostdlib", "-e", "_start", "-o", binary,
                        obj, align_obj,
                    ])
                    if linked.returncode != 0:
                        fail("alignment link: " + describe(linked))
                    else:
                        executed = run([binary])
                        if executed.returncode != 0:
                            fail("call alignment: " + describe(executed))

        # --- Microsoft x64 nonvolatile XMM save ---------------------------
        path = source("msabi_xmm", MSXMM)
        asm = os.path.join(tmp, "msabi_xmm.s")
        built = compile_fast(path, asm)
        if built.returncode != 0:
            fail("msabi xmm shape compile: " + describe(built))
        else:
            body = function_body(read_text(asm), "work")
            if body is None:
                fail("msabi xmm shape: work is missing")
            else:
                if "movdqu xmmword ptr [rsp + " not in body:
                    fail("msabi xmm shape: xmm6-xmm15 are not saved at full "
                         "width (body: %r)" % body)
                if not re.search(r"(?m)^  movdqu xmm\d+, xmmword ptr \[rsp \+ ",
                                 body):
                    fail("msabi xmm shape: xmm6-xmm15 are not restored at "
                         "full width")

    if failures:
        for text in failures:
            print("FAIL: " + text, file=sys.stderr)
        print("%d fast ABI failure(s)" % len(failures), file=sys.stderr)
        return 1
    print("fast ABI: staging, returns, fixed registers, varargs, "
          "callee-saved and alignment passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
