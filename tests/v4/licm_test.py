#!/usr/bin/env python3
"""v4 loop-invariant code motion end-to-end test.

The unit test pins the natural-loop, preheader and candidate rules. This test
proves the pass inside the real pipeline: an invariant used four times in one
loop moves to the preheader once, an invariant inside two nested loops leaves
both levels, and a loop body's call and global volatile load stay in the loop.
Native and both VM legs agree, and two compiles are byte-identical.

`many` repeats `a * b` four times in its body. Value numbering first collapses
the duplicates; LICM then moves the one survivor to block 0, the entry
preheader. `nested` computes `a * b` in the inner body; it must pass through the
inner preheader and then the outer preheader. `barrier` reads a volatile global
and calls a self-recursive helper inside its loop, so neither the load nor the
call may move, while `n * 5` still hoists.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

SOURCE = """var gv: volatile i32 = 0;

func many(n: i32, a: i32, b: i32) -> i32 {
    var s: i32 = 0;
    var i: i32 = 0;
    while (i < n) {
        s = s + (a * b) + (a * b) + (a * b) + (a * b);
        i = i + 1;
    }
    return s;
}

func nested(n: i32, a: i32, b: i32) -> i32 {
    var s: i32 = 0;
    var i: i32 = 0;
    while (i < n) {
        var j: i32 = 0;
        while (j < n) {
            s = s + (a * b);
            j = j + 1;
        }
        i = i + 1;
    }
    return s;
}

func bump(x: i32) -> i32 {
    if (x <= 0) {
        return 0;
    }
    return bump(x - 1) + 1;
}

func barrier(n: i32) -> i32 {
    var s: i32 = 0;
    var i: i32 = 0;
    while (i < n) {
        s = s + gv;
        s = s + bump(i);
        s = s + (n * 5);
        i = i + 1;
    }
    return s;
}

func _start() -> void {
    var code: i32 = many(3, 7, 6) + nested(3, 7, 6) + barrier(3);
    asm {
        "mov rax, 60"
        "mov edi, {code}"
        "syscall"
    }
}
"""

EXPECTED_EXIT = 162


def run(args, env):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=120)


def function_body(dump, name):
    match = re.search(r"^func %s\n(.*?)(?=^func |\Z)" % re.escape(name),
                      dump, re.MULTILINE | re.DOTALL)
    return match.group(1) if match else None


def block_bodies(body):
    blocks = {}
    for match in re.finditer(r"^block (\d+)\n(.*?)(?=^block |\Z)", body,
                             re.MULTILINE | re.DOTALL):
        blocks[int(match.group(1))] = match.group(2)
    return blocks


def op_count(text, op):
    pattern = r"^  (?:%\d+ = )?" + re.escape(op) + r" "
    return len(re.findall(pattern, text, re.MULTILINE))


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    failures = []
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    with tempfile.TemporaryDirectory(prefix="cider-licm-") as tmp:
        source = os.path.join(tmp, "licm.cid")
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
            before_many = function_body(unopt.stdout, "many")
            after_many = function_body(opt.stdout, "many")
            if before_many is None or after_many is None:
                failures.append("the many function is missing from the IR dump")
            else:
                before_blocks = block_bodies(before_many)
                after_blocks = block_bodies(after_many)
                before_body_mul = op_count(before_blocks.get(2, ""), "mul")
                after_body_mul = op_count(after_blocks.get(2, ""), "mul")
                after_entry_mul = op_count(after_blocks.get(0, ""), "mul")
                if before_body_mul < 4:
                    failures.append(
                        "many unoptimised loop body has %d muls, expected 4" %
                        before_body_mul)
                if after_body_mul != 0:
                    failures.append(
                        "many optimised loop body still has %d muls" %
                        after_body_mul)
                if after_entry_mul != 1:
                    failures.append(
                        "many optimised preheader has %d muls, expected 1" %
                        after_entry_mul)
                if op_count(after_many, "mul") != 1:
                    failures.append(
                        "many optimised has %d muls, expected 1" %
                        op_count(after_many, "mul"))

            before_nested = function_body(unopt.stdout, "nested")
            after_nested = function_body(opt.stdout, "nested")
            if before_nested is None or after_nested is None:
                failures.append(
                    "the nested function is missing from the IR dump")
            else:
                before_blocks = block_bodies(before_nested)
                after_blocks = block_bodies(after_nested)
                before_inner_mul = op_count(before_blocks.get(5, ""), "mul")
                after_inner_mul = op_count(after_blocks.get(5, ""), "mul")
                after_entry_mul = op_count(after_blocks.get(0, ""), "mul")
                if before_inner_mul < 1:
                    failures.append(
                        "nested unoptimised inner body has no mul")
                if after_inner_mul != 0:
                    failures.append(
                        "nested optimised inner body still has %d muls" %
                        after_inner_mul)
                if after_entry_mul != 1:
                    failures.append(
                        "nested optimised outer preheader has %d muls, "
                        "expected 1" % after_entry_mul)
                if op_count(after_nested, "mul") != 1:
                    failures.append(
                        "nested optimised has %d muls, expected 1" %
                        op_count(after_nested, "mul"))

            before_barrier = function_body(unopt.stdout, "barrier")
            after_barrier = function_body(opt.stdout, "barrier")
            if before_barrier is None or after_barrier is None:
                failures.append(
                    "the barrier function is missing from the IR dump")
            else:
                before_blocks = block_bodies(before_barrier)
                after_blocks = block_bodies(after_barrier)
                before_loop = before_blocks.get(2, "")
                after_loop = after_blocks.get(2, "")
                after_entry = after_blocks.get(0, "")
                if op_count(before_loop, "mul") != 1:
                    failures.append(
                        "barrier unoptimised loop body has %d muls, "
                        "expected 1" % op_count(before_loop, "mul"))
                if op_count(after_entry, "mul") != 1:
                    failures.append(
                        "barrier invariant mul did not reach the preheader")
                if op_count(after_loop, "mul") != 0:
                    failures.append(
                        "barrier still computes the invariant mul in the loop")
                if op_count(after_entry, "call") != 0:
                    failures.append(
                        "barrier moved a call into the preheader")
                if op_count(after_loop, "call") != 1:
                    failures.append(
                        "barrier loop body has %d calls, expected 1" %
                        op_count(after_loop, "call"))
                if op_count(after_entry, "load") != 0:
                    failures.append(
                        "barrier moved a volatile load into the preheader")
                if op_count(after_loop, "load") < 1:
                    failures.append(
                        "barrier loop body lost its volatile global load")

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
    print("licm: native, vm-unopt and vm-opt passed; invariants moved out of "
          "loops; calls and volatile loads stayed; assembly is stable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
