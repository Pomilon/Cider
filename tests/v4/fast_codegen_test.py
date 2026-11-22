#!/usr/bin/env python3
"""Task 11's direct arithmetic and value-access end-to-end test.

The allocator's unit test pins register homes; the frame test pins the compact
fast frame. This test pins the next fast-only instruction-selection slice:
arithmetic writes its result home directly instead of round-tripping through
rax, integer compares fuse with the branch that consumes them, and the fast
backend keeps Cider's integer semantics for every arithmetic width.

The behaviour legs run every program on the fast backend (optimised and not)
and on the legacy backend. The assembly-shape legs are the RED half of the
test: before Task 11, fast still emitted `mov rax, left / add rax, right /
mov <home>, rax` for every homed add and `setcc` for every compare, so the
shape checks fail even though the values are correct.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))


def program(body):
    return body + """

func _start() -> void {
    var code: i32 = probe();
    asm {
        "mov rax, 60"
        "mov edi, {code}"
        "syscall"
    }
}
"""


# a=13, b=3. Every operation is computed once, then summed:
# 16 + 10 + 39 + 4 + 1 + 1 + 15 + 14 + 104 + 3 = 207.
OPS = program("""func probe() -> i32 {
    var a: i32 = 13;
    var b: i32 = 3;
    var added: i32 = a + b;
    var differ: i32 = a - b;
    var product: i32 = a * b;
    var quotient: i32 = a / b;
    var remainder: i32 = a % b;
    var band: i32 = a & b;
    var bor: i32 = a | b;
    var bxor: i32 = a ^ b;
    var shifted: i32 = a << 3;
    var shifted_back: i32 = a >> 2;
    return added + differ + product + quotient + remainder
         + band + bor + bxor + shifted + shifted_back;
}
""")

# (5 + 7 * 3) - (5 * (7 - 3)) = 26 - 20 = 6.
NESTED = program("""func probe() -> i32 {
    var a: i32 = 5;
    var b: i32 = 7;
    var c: i32 = 3;
    var left: i32 = a + b * c;
    var right: i32 = a * (b - c);
    return left - right;
}
""")

# -17 / 5 = -3 and -17 % 5 = -2, so -5; a native exit takes the low byte 251.
SIGNED_DIV = program("""func probe() -> i32 {
    var a: i32 = -17;
    var b: i32 = 5;
    return a / b + a % b;
}
""")

# 4000000000 / 3 = 1333333333 and % 3 = 1, so 1333333334; low byte 86.
UNSIGNED_DIV = program("""func probe() -> i32 {
    var a: u32 = 4000000000;
    var b: u32 = 3;
    var q: u32 = a / b;
    var r: u32 = a % b;
    return (q + r) as i32;
}
""")

# x = -64: <<5 = -2048, >>5 (signed) = -2, (u32)x >>5 = 0x07fffffe.
# Sum 134215676; low byte 252.
SHIFTS = program("""func probe() -> i32 {
    var x: i32 = -64;
    var left: i32 = x << 5;
    var right: i32 = x >> 5;
    var logical: u32 = (x as u32) >> 5;
    return left + right + (logical as i32);
}
""")

# -5 compared with 3: <, <= and != are true = 1 + 2 + 32 = 35.
SIGNED_CMP = program("""func probe() -> i32 {
    var a: i32 = -5;
    var b: i32 = 3;
    var r: i32 = 0;
    if (a < b) { r = r + 1; }
    if (a <= b) { r = r + 2; }
    if (a > b) { r = r + 4; }
    if (a >= b) { r = r + 8; }
    if (a == b) { r = r + 16; }
    if (a != b) { r = r + 32; }
    return r;
}
""")

# 4294967285 is above 3 unsigned: >, >= and != are true = 4 + 8 + 32 = 44.
UNSIGNED_CMP = program("""func probe() -> i32 {
    var a: u32 = 4294967285;
    var b: u32 = 3;
    var r: i32 = 0;
    if (a < b) { r = r + 1; }
    if (a <= b) { r = r + 2; }
    if (a > b) { r = r + 4; }
    if (a >= b) { r = r + 8; }
    if (a == b) { r = r + 16; }
    if (a != b) { r = r + 32; }
    return r;
}
""")

# -7: != 0 gives 2 and < 0 gives 4.
CMP_ZERO = program("""func probe() -> i32 {
    var a: i32 = -7;
    var r: i32 = 0;
    if (a == 0) { r = r + 1; }
    if (a != 0) { r = r + 2; }
    if (a < 0) { r = r + 4; }
    return r;
}
""")

# sum(2*i - 1, i in 0..9) = 2*45 - 10 = 80.
LOOP = program("""func probe() -> i32 {
    var s: i32 = 0;
    var i: i32 = 0;
    while (i < 10) {
        s = s + i * 2 - 1;
        i = i + 1;
    }
    return s;
}
""")

# Twenty live locals force more values than the register file holds.
PRESSURE = program("""func probe() -> i32 {
    var x: i32 = 1;
    var v0: i32 = x + 1;
    var v1: i32 = x + 2;
    var v2: i32 = x + 3;
    var v3: i32 = x + 4;
    var v4: i32 = x + 5;
    var v5: i32 = x + 6;
    var v6: i32 = x + 7;
    var v7: i32 = x + 8;
    var v8: i32 = x + 9;
    var v9: i32 = x + 10;
    var v10: i32 = x + 11;
    var v11: i32 = x + 12;
    var v12: i32 = x + 13;
    var v13: i32 = x + 14;
    var v14: i32 = x + 15;
    var v15: i32 = x + 16;
    var v16: i32 = x + 17;
    var v17: i32 = x + 18;
    var v18: i32 = x + 19;
    var v19: i32 = x + 20;
    return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9
         + v10 + v11 + v12 + v13 + v14 + v15 + v16 + v17 + v18 + v19;
}
""")

# Sum of 2..21 = 230.
# Narrow widths wrap at their own width: i8 120+10 = -126, i8 120*2 = -16,
# i16 30000+3000 = -32536, u8 200+100 = 44, u16 60000+10000 = 4464.
# The i64 sum 7,000,000,000 truncates to i32 as -1589934592; u64 wraps to 1.
# Summing and truncating to i32 gives -1589962761, low byte 247.
WIDTHS = program("""func probe() -> i32 {
    var a8: i8 = 120;
    var b8: i8 = 10;
    var s8: i8 = (a8 + b8) as i8;
    var m8: i8 = (a8 * (2 as i8)) as i8;
    var a16: i16 = 30000;
    var b16: i16 = 3000;
    var s16: i16 = (a16 + b16) as i16;
    var ua8: u8 = 200;
    var ub8: u8 = 100;
    var w8: u8 = (ua8 + ub8) as u8;
    var ua16: u16 = 60000;
    var ub16: u16 = 10000;
    var w16: u16 = (ua16 + ub16) as u16;
    var big1: i64 = 4000000000;
    var big2: i64 = 3000000000;
    var bigs: i64 = big1 + big2;
    var ub1: u64 = 0xFFFFFFFFFFFFFFFF;
    var ub2: u64 = 2;
    var ubw: u64 = ub1 + ub2;
    return (s8 as i32) + (m8 as i32) + (s16 as i32) + (w8 as i32)
         + (w16 as i32) + (bigs as i32) + (ubw as i32);
}
""")

# A compare whose boolean is live out of its defining block. The function is
# deliberately larger than the inliner's 24-instruction limit so the flag is
# not folded into the caller; mem2reg then leaves one Cmp result used by two
# CondBrs. The compare must materialise its boolean (setcc), never be fused to
# flags that would leave the later consumer without a value.
LIVE_OUT = (
    "func probe(a: i32) -> i32 {\n"
    "    var flag: bool = a == 0;\n"
    "    var r: i32 = 0;\n"
    "    if (flag) { r = r + 100; }\n"
    + "".join("    r = r + %d;\n" % i for i in range(1, 30))
    + "    if (flag) { r = r + 200; }\n"
    "    return r;\n"
    "}\n"
    "func _start() -> void {\n"
    "    var code: i32 = probe(0);\n"
    '    asm { "mov rax, 60" "mov edi, {code}" "syscall" }\n'
    "}\n"
)

# Enough simultaneously live values to force the constant 9 out of a register,
# give the doubled result a home, and therefore select `lea [left + 9]`.
# v0..v13 = 4..17, r = 4 + 9 = 13, answer = 13 + 13 + 147 = 173.
LEA = (
    "func probe(a: i32) -> i32 {\n"
    + "".join("    var v%d: i32 = a + %d;\n" % (i, i + 1)
              for i in range(14))
    + "    var r: i32 = v0 + 9;\n"
    + "    return r + r + " + " + ".join("v%d" % i for i in range(14))
    + ";\n"
    "}\n"
    "func _start() -> void {\n"
    "    var code: i32 = probe(3);\n"
    '    asm { "mov rax, 60" "mov edi, {code}" "syscall" }\n'
    "}\n"
)

# The allocator reuses the right operand's register for this subtraction's
# result. The fast selector must negate the held right and add the left rather
# than moving left into dest first, which would destroy the right operand.
# (3..17 inputs) The answer is 240, verified against the legacy emitter.
ALIAS = """func probe(a: i32, b: i32, c: i32, d: i32, e: i32,
           f: i32) -> i32 {
    var v0: i32 = (e * (1 - (1 * 3)));
    var v1: i32 = ((b | (1 | 3)) - b);
    var v2: i32 = f;
    var v3: i32 = ((3 ^ (a + c)) | b);
    var v4: i32 = (7 - (3 | (f + d)));
    var v5: i32 = 2;
    return v0 + v1 + v2 + v3 + v4 + v5;
}
func _start() -> void {
    var code: i32 = probe(3, 5, 7, 11, 13, 17);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""

CASES = [
    ("ops", OPS, 207),
    ("nested", NESTED, 6),
    ("signed_div", SIGNED_DIV, 251),
    ("unsigned_div", UNSIGNED_DIV, 86),
    ("shifts", SHIFTS, 252),
    ("signed_cmp", SIGNED_CMP, 35),
    ("unsigned_cmp", UNSIGNED_CMP, 44),
    ("cmp_zero", CMP_ZERO, 6),
    ("loop", LOOP, 80),
    ("pressure", PRESSURE, 230),
    ("widths", WIDTHS, 247),
    ("live_out_compare", LIVE_OUT, 223),
    ("lea_add", LEA, 173),
    ("alias_sub", ALIAS, 240),
]

# Shape probes. Kept free of the optimizer so no constant folding hides the
# selectors under test.
SHAPES = """func homed_add(a: i32, b: i32) -> i32 {
    return a + b;
}
func homed_mul(a: i32, b: i32) -> i32 {
    return a * b;
}
func compare_zero(a: i32) -> i32 {
    var r: i32 = 0;
    if (a == 0) { r = 1; }
    return r;
}
func compare_pair(a: i32, b: i32) -> i32 {
    var r: i32 = 0;
    if (a < b) { r = 1; }
    return r;
}
func probe() -> i32 {
    return homed_add(1, 2) + homed_mul(3, 4)
         + compare_zero(0) + compare_pair(1, 2);
}
func _start() -> void {
    var code: i32 = probe();
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
"""


def run(args, env, timeout=180):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=timeout)


def function_body(text, name):
    start = re.search(r"(?m)^%s:\n" % re.escape(name), text)
    if start is None:
        return None
    rest = text[start.end():]
    end = re.search(r"(?m)^(?:\.globl |\.weak |(?![.]L_)[A-Za-z_.][^\s:]*:\n)",
                    rest)
    return rest[:end.start()] if end else rest


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout[:400], proc.stderr[:400])


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    failures = []
    with tempfile.TemporaryDirectory(prefix="cider-fast-codegen-") as tmp:
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

        shapes = os.path.join(tmp, "shapes.cid")
        with open(shapes, "w") as handle:
            handle.write(SHAPES)
        fast_asm = os.path.join(tmp, "shapes-fast.s")
        compiled = run([str(CIDER), "compile", "--codegen=fast", "--no-opt",
                        "-o", fast_asm, shapes], env)
        if compiled.returncode != 0:
            failures.append("shapes compile: " + describe(compiled))
        else:
            text = open(fast_asm, encoding="utf-8").read()
            add_body = function_body(text, "homed_add")
            if add_body is None:
                failures.append("shapes: homed_add is missing")
            else:
                # The result of `a + b` lives across the return, so the
                # allocator cannot home it in rax/rdx. Direct emission writes
                # that non-rax home; the pre-Task-11 emitter always wrote rax
                # and then copied rax into the home.
                direct = re.search(
                    r"(?m)^  (?:add|imul) (r[a-z0-9]+), ", add_body)
                if direct is None or direct.group(1) in (
                        "rax", "rdx", "rsp", "rbp"):
                    failures.append(
                        "homed_add: no arithmetic wrote its result home "
                        "directly (body: %r)" % add_body)
                if re.search(r"(?m)^  mov rax, [^\n]+\n  add rax, ", add_body):
                    failures.append(
                        "homed_add: arithmetic still round-trips through rax")
            mul_body = function_body(text, "homed_mul")
            if mul_body is None:
                failures.append("shapes: homed_mul is missing")
            elif not re.search(r"(?m)^  imul (r(?:[a-z]+|\d+)), ", mul_body) \
                    or re.search(r"(?m)^  imul rax, ", mul_body):
                failures.append(
                    "homed_mul: multiply did not write a non-rax home")
            zero_body = function_body(text, "compare_zero")
            if zero_body is None:
                failures.append("shapes: compare_zero is missing")
            else:
                if "sete" in zero_body:
                    failures.append(
                        "compare_zero: a branch-only compare still emitted "
                        "setcc instead of branching on flags")
                if not re.search(r"(?m)^  test r", zero_body):
                    failures.append(
                        "compare_zero: the zero comparison did not emit test")
                # Task 16.5 pins the fused branch and inverts it when its
                # taken edge is the next block. The emitted mnemonic and
                # target are therefore fixed: `je` would name the following
                # label and is exactly the regression this asserts against.
                if not re.search(r"(?m)^  jne \.L_compare_zero_2$", zero_body):
                    failures.append(
                        "compare_zero: expected jne .L_compare_zero_2")
            pair_body = function_body(text, "compare_pair")
            if pair_body is None:
                failures.append("shapes: compare_pair is missing")
            else:
                if "setl" in pair_body:
                    failures.append(
                        "compare_pair: a branch-only compare still emitted "
                        "setcc instead of branching on flags")
                # Same fallthrough inversion as compare_zero: the branch must
                # be `jge` to the else block, not `jl` to the next label.
                if not re.search(r"(?m)^  jge \.L_compare_pair_2$", pair_body):
                    failures.append(
                        "compare_pair: expected jge .L_compare_pair_2")

        def compile_fast(name, text, no_opt=False):
            source_path = os.path.join(tmp, name + ".cid")
            asm_path = os.path.join(tmp, name + ".s")
            with open(source_path, "w") as handle:
                handle.write(text)
            command = [str(CIDER), "compile", "--codegen=fast"]
            if no_opt:
                command.append("--no-opt")
            command.extend(["-o", asm_path, source_path])
            proc = run(command, env)
            if proc.returncode != 0:
                failures.append(name + " compile: " + describe(proc))
                return None
            return open(asm_path, encoding="utf-8").read()

        # A compare result live out of its defining block has a consumer past
        # the branch. It must keep its setcc value; flags alone are not a
        # definition the later block can read.
        live_text = compile_fast("live_out_shape", LIVE_OUT)
        if live_text is not None:
            live_body = function_body(live_text, "probe")
            if live_body is None:
                failures.append("live_out_compare: probe is missing")
            elif "sete" not in live_body:
                failures.append(
                    "live_out_compare: a compare with a later consumer was "
                    "fused to flags (body: %r)" % live_body)

        # A rematerialised displacement add in a register home must select lea.
        lea_text = compile_fast("lea_shape", LEA)
        if lea_text is not None:
            lea_body = function_body(lea_text, "probe")
            if lea_body is None:
                failures.append("lea_add: probe is missing")
            elif not re.search(
                    r"(?m)^  lea r[a-z0-9]+, \[r[a-z0-9]+ \+ 9\]$", lea_body):
                failures.append(
                    "lea_add: the displacement add did not use lea "
                    "(body: %r)" % lea_body)

        # A result home that aliases the right operand must preserve the right
        # operand; subtraction takes the neg/add shape.
        alias_text = compile_fast("alias_shape", ALIAS)
        if alias_text is not None:
            alias_body = function_body(alias_text, "probe")
            if alias_body is None:
                failures.append("alias_sub: probe is missing")
            elif not re.search(
                    r"(?m)^  neg (r[a-z0-9]+)\n  add \1, ", alias_body):
                failures.append(
                    "alias_sub: the dest == right alias case did not take "
                    "the neg/add shape (body: %r)" % alias_body)

        # More values than registers: the no-opt fast body must fall back to
        # frame cells, proving the pressure case is not silently all-homed.
        pressure_text = compile_fast("pressure_shape", PRESSURE, no_opt=True)
        if pressure_text is not None:
            pressure_body = function_body(pressure_text, "probe")
            if pressure_body is None:
                failures.append("pressure: probe is missing")
            elif not re.search(r"\[rsp \+ \d+\]", pressure_body):
                failures.append(
                    "pressure: the over-pressure case produced no frame "
                    "operand (body: %r)" % pressure_body)

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast codegen: direct arithmetic, fused compares, spills and "
          "integer semantics passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
