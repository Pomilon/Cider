#!/usr/bin/env python3
"""v4 integer strength-reduction end-to-end test.

`src/opt/strength.cpp` rewrites only bit-exact integer identities. The unit
test pins every rule, width and refusal. This gate proves the pass inside the
real pipeline on source programs:

- `ident` exercises every rule on `i32`;
- `wide` exercises the same rules on signed 64-bit values;
- `uns` and `uns64` exercise zero/one multiply, division, remainder and the
  all-ones mask on unsigned 32- and 64-bit values.

Module constants are used so each constant reaches the IR at the operation's
own width and language type; the pass deliberately refuses a constant declared
at another width. Each function contains a harmless `mov rax, rax` asm block so
inlining leaves it in the whole-program assembly. Native, vm-opt and vm-unopt
must agree on the answer. Optimized IR must lose every identity operation,
optimized assembly must lose every multiply/divide, and two compiles must be
byte-identical.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

SOURCE = """const ZERO_I32: i32 = 0;
const ONE_I32: i32 = 1;
const ALL_I32: i32 = -1;
const ZERO_I64: i64 = 0;
const ONE_I64: i64 = 1;
const ALL_I64: i64 = -1;
const ZERO_U32: u32 = 0;
const ONE_U32: u32 = 1;
const ALL_U32: u32 = 4294967295;
const ZERO_U64: u64 = 0;
const ONE_U64: u64 = 1;
const ALL_U64: u64 = 18446744073709551615;

func ident(n: i32) -> i32 {
    asm { "mov rax, rax" }
    var a: i32 = n + ZERO_I32;
    var b: i32 = ZERO_I32 + a;
    var c: i32 = b - ZERO_I32;
    var d: i32 = c | ZERO_I32;
    var e: i32 = ZERO_I32 | d;
    var f: i32 = e ^ ZERO_I32;
    var g: i32 = ZERO_I32 ^ f;
    var h: i32 = g << ZERO_I32;
    var i: i32 = h >> ZERO_I32;
    var j: i32 = i * ONE_I32;
    var k: i32 = ONE_I32 * j;
    var l: i32 = k & ALL_I32;
    var m: i32 = l / ONE_I32;
    var o: i32 = m % ONE_I32;
    var p: i32 = o * ZERO_I32;
    return p + n;
}

func wide(n: i64) -> i64 {
    asm { "mov rax, rax" }
    var a: i64 = n + ZERO_I64;
    var b: i64 = ZERO_I64 + a;
    var c: i64 = b - ZERO_I64;
    var d: i64 = c | ZERO_I64;
    var e: i64 = ZERO_I64 | d;
    var f: i64 = e ^ ZERO_I64;
    var g: i64 = ZERO_I64 ^ f;
    var h: i64 = g << ZERO_I64;
    var i: i64 = h >> ZERO_I64;
    var j: i64 = i * ONE_I64;
    var k: i64 = ONE_I64 * j;
    var l: i64 = k & ALL_I64;
    var m: i64 = l / ONE_I64;
    var o: i64 = m % ONE_I64;
    var p: i64 = o * ZERO_I64;
    return p + n;
}

func uns(n: u32) -> u32 {
    asm { "mov rax, rax" }
    var a: u32 = n + ZERO_U32;
    var b: u32 = ZERO_U32 + a;
    var c: u32 = b - ZERO_U32;
    var d: u32 = c | ZERO_U32;
    var e: u32 = ZERO_U32 | d;
    var f: u32 = e ^ ZERO_U32;
    var g: u32 = ZERO_U32 ^ f;
    var h: u32 = g << ZERO_U32;
    var i: u32 = h >> ZERO_U32;
    var j: u32 = i * ONE_U32;
    var k: u32 = ONE_U32 * j;
    var l: u32 = k & ALL_U32;
    var m: u32 = l / ONE_U32;
    var o: u32 = m % ONE_U32;
    var p: u32 = o * ZERO_U32;
    return p + n;
}

func uns64(n: u64) -> u64 {
    asm { "mov rax, rax" }
    var a: u64 = n + ZERO_U64;
    var b: u64 = ZERO_U64 + a;
    var c: u64 = b - ZERO_U64;
    var d: u64 = c | ZERO_U64;
    var e: u64 = ZERO_U64 | d;
    var f: u64 = e ^ ZERO_U64;
    var g: u64 = ZERO_U64 ^ f;
    var h: u64 = g << ZERO_U64;
    var i: u64 = h >> ZERO_U64;
    var j: u64 = i * ONE_U64;
    var k: u64 = ONE_U64 * j;
    var l: u64 = k & ALL_U64;
    var m: u64 = l / ONE_U64;
    var o: u64 = m % ONE_U64;
    var p: u64 = o * ZERO_U64;
    return p + n;
}

func _start() -> void {
    var code: i32 = ident(37) + (wide(11) as i32) + (uns(7) as i32) + (uns64(5) as i32);
    asm {
        "mov rax, 60"
        "mov edi, {code}"
        "syscall"
    }
}
"""

EXPECTED_EXIT = 60
FUNCTIONS = ("ident", "wide", "uns", "uns64")
IDENTITY_OPS = ("sub", "or", "xor", "shl", "shr", "mul", "and", "div", "rem")


def run(args, env):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=120)


def function_body(dump, name):
    match = re.search(r"^func %s\n(.*?)(?=^func |\Z)" % re.escape(name),
                      dump, re.MULTILINE | re.DOTALL)
    return match.group(1) if match else None


def op_count(body, op):
    pattern = r"^  (?:%\d+ = )?" + re.escape(op) + r" "
    return len(re.findall(pattern, body, re.MULTILINE))


def mnemonic_count(text, mnemonic):
    return len(re.findall(r"\b" + re.escape(mnemonic) + r"\b", text))


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    failures = []
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    with tempfile.TemporaryDirectory(prefix="cider-strength-") as tmp:
        source = os.path.join(tmp, "strength.cid")
        with open(source, "w") as handle:
            handle.write(SOURCE)

        for leg, extra in (("native", []),
                           ("vm-opt", ["--target", "vm"]),
                           ("vm-unopt", ["--target", "vm", "--no-opt"])):
            got = run([CIDER, "run"] + extra + [source], env)
            if (got.returncode, got.stdout, got.stderr) != (
                    EXPECTED_EXIT, "", ""):
                failures.append(
                    "%s: exit %r, stdout %r, stderr %r; expected %d, '', ''" %
                    (leg, got.returncode, got.stdout, got.stderr,
                     EXPECTED_EXIT))

        unopt = run([CIDER, "ir", "--no-opt", source], env)
        opt = run([CIDER, "ir", source], env)
        if unopt.returncode != 0 or opt.returncode != 0:
            failures.append("ir failed: unopt=%r opt=%r" % (
                unopt.stderr, opt.stderr))
        else:
            for name in FUNCTIONS:
                before = function_body(unopt.stdout, name)
                after = function_body(opt.stdout, name)
                if before is None or after is None:
                    failures.append("%s is missing from the IR dump" % name)
                    continue
                if op_count(before, "add") < 2:
                    failures.append(
                        "%s: unoptimised IR has %d adds, expected at least 2" %
                        (name, op_count(before, "add")))
                if op_count(after, "add") != 1:
                    failures.append(
                        "%s: optimized IR has %d adds, expected the single "
                        "non-identity return add" %
                        (name, op_count(after, "add")))
                for op in IDENTITY_OPS:
                    if op_count(before, op) == 0:
                        failures.append(
                            "%s: unoptimised IR has no %s to remove" %
                            (name, op))
                    if op_count(after, op) != 0:
                        failures.append(
                            "%s: optimized IR still has %d %s" %
                            (name, op_count(after, op), op))

        first = os.path.join(tmp, "first.s")
        second = os.path.join(tmp, "second.s")
        noopt = os.path.join(tmp, "noopt.s")
        one = run([CIDER, "compile", source, "-o", first], env)
        two = run([CIDER, "compile", source, "-o", second], env)
        raw = run([CIDER, "compile", "--no-opt", source, "-o", noopt], env)
        for made, result in (("optimized", one), ("second", two),
                             ("no-opt", raw)):
            if result.returncode != 0:
                failures.append("%s compile: %r" %
                                (made, result.stderr))
        if one.returncode == 0 and raw.returncode == 0:
            optimized_text = open(first).read()
            raw_text = open(noopt).read()
            if len(optimized_text.encode()) >= len(raw_text.encode()):
                failures.append(
                    "optimized assembly did not shrink: %d -> %d bytes" %
                    (len(raw_text.encode()), len(optimized_text.encode())))
            for mnemonic in ("imul", "idiv", "div"):
                before = mnemonic_count(raw_text, mnemonic)
                after = mnemonic_count(optimized_text, mnemonic)
                if before == 0:
                    failures.append(
                        "unoptimised assembly has no %s to remove" % mnemonic)
                if after != 0:
                    failures.append(
                        "optimized assembly still has %d %s" % (after, mnemonic))
        if one.returncode == 0 and two.returncode == 0:
            if open(first, "rb").read() != open(second, "rb").read():
                failures.append("two optimized compiles are not byte-identical")

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("strength: native, vm-unopt and vm-opt agree; identities disappear "
          "from IR and assembly; output is stable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
