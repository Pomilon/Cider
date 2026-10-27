#!/usr/bin/env python3
"""v4 dominance-scoped value numbering end-to-end test.

The unit test pins the table's scope and the opcode refusals. This test proves
the pass inside the real pipeline: duplicate pure arithmetic across a join and
inside a loop disappears from optimized IR, native and both VM legs agree on
the answer, and two compiles remain byte-identical.

`mix` repeats `a + b` in the entry, on both sides of an if/else and after the
join. Value numbering must keep one leader in the entry (it dominates every
path) and still leave each branch's own `(...) + z` alone, because those are
siblings. `loop` computes `i * 2` twice in one body; the second is a duplicate
of the first.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

SOURCE = """func mix(a: i32, b: i32) -> i32 {
    var x: i32 = a + b;
    var y: i32 = a + b;
    var z: i32 = x + y;
    if (z > 10) {
        z = (a + b) + z;
    } else {
        z = (b + a) + z;
    }
    return z + (a + b);
}

func loop(n: i32) -> i32 {
    var s: i32 = 0;
    var i: i32 = 0;
    var step: i32 = n * 2;
    while (i < n) {
        var t: i32 = i * 2;
        s = s + (i * 2);
        i = i + 1;
    }
    return s + step;
}

func _start() -> void {
    var code: i32 = mix(1, 2) * 10 + loop(5);
    asm {
        "mov rax, 60"
        "mov edi, {code}"
        "syscall"
    }
}
"""

EXPECTED_EXIT = 150


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


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    failures = []
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    with tempfile.TemporaryDirectory(prefix="cider-gvn-") as tmp:
        source = os.path.join(tmp, "gvn.cid")
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
            before = function_body(unopt.stdout, "mix")
            after = function_body(opt.stdout, "mix")
            if before is None or after is None:
                failures.append("the mix function is missing from the IR dump")
            else:
                before_add = op_count(before, "add")
                after_add = op_count(after, "add")
                if before_add < 9:
                    failures.append(
                        "mix unoptimised has %d adds, expected at least 9" %
                        before_add)
                if after_add != 5:
                    failures.append(
                        "mix optimised has %d adds, expected 5" % after_add)
                if " phi " not in after:
                    failures.append("mix has no join phi")
            before_loop = function_body(unopt.stdout, "loop")
            after_loop = function_body(opt.stdout, "loop")
            if before_loop is None or after_loop is None:
                failures.append("the loop function is missing from the IR dump")
            else:
                before_mul = op_count(before_loop, "mul")
                after_mul = op_count(after_loop, "mul")
                if before_mul < 3:
                    failures.append(
                        "loop unoptimised has %d muls, expected at least 3" %
                        before_mul)
                if after_mul != 2:
                    failures.append(
                        "loop optimised has %d muls, expected 2" % after_mul)
                if " phi " not in after_loop:
                    failures.append("loop has no header phi")

        first = os.path.join(tmp, "first.s")
        second = os.path.join(tmp, "second.s")
        one = run([CIDER, "compile", source, "-o", first], env)
        two = run([CIDER, "compile", source, "-o", second], env)
        if one.returncode != 0 or two.returncode != 0:
            failures.append("determinism compile failed: %r / %r" % (
                one.stderr, two.stderr))
        elif open(first, "rb").read() != open(second, "rb").read():
            failures.append("two compiles produced different assembly")

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("gvn: native, vm-unopt and vm-opt passed; duplicates removed; "
          "assembly is stable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
