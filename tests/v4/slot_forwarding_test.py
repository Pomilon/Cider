#!/usr/bin/env python3
"""v4 mem2reg and slot-forwarding end-to-end test.

mem2reg promotes non-escaping scalar Alloca slots to SSA values and inserts
real phis at joins and loop headers. forwardMemory remains the conservative
fallback for every slot mem2reg leaves in memory.

The unit tests pin the individual rules. This test pins the two things a unit
test cannot: the passes run inside the real optimizer pipeline, and native plus
both VM legs still agree. `work`, `branchy` and `loop_swap` have only direct
scalar slot accesses, so their optimized IR must lose loads and stores (and the
control-flow functions must show phis). `escape_of` takes a local's address, so
its memory shape must stay exactly as the unoptimised IR wrote it.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

SOURCE = """func work(x: i32) -> i32 {
    var a: i32 = x;
    var b: i32 = a + 1;
    a = b * 2;
    var c: i32 = a;
    c = c + b;
    b = c - a;
    return c + b;
}

func escape_of(x: i32) -> i32 {
    var a: i32 = x;
    var p: *i32 = &a;
    *p = *p + 1;
    return a;
}

func branchy(c: bool, a: i32, b: i32) -> i32 {
    var x: i32 = 0;
    if (c) { x = a; } else { x = b; }
    return x;
}

func loop_swap() -> i32 {
    var x: i32 = 1;
    var y: i32 = 2;
    var i: i32 = 0;
    while (i < 3) {
        var t: i32 = x;
        x = y;
        y = t;
        i = i + 1;
    }
    return x * 10 + y;
}

func _start() -> void {
    var code: i32 = work(5) + escape_of(5) + branchy(true, 3, 4) + loop_swap();
    asm {
        "mov rax, 60"
        "mov edi, {code}"
        "syscall"
    }
}
"""


def run(args, env):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=120)


def function_body(dump, name):
    match = re.search(r"^func %s\n(.*?)(?=^func |\Z)" % re.escape(name),
                      dump, re.MULTILINE | re.DOTALL)
    return match.group(1) if match else None


def memory_counts(body):
    loads = len(re.findall(r" = load ", body))
    stores = len(re.findall(r"^  store ", body, re.MULTILINE))
    return loads, stores


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    failures = []
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    with tempfile.TemporaryDirectory(prefix="cider-slot-forward-") as tmp:
        source = os.path.join(tmp, "forward.cid")
        with open(source, "w") as handle:
            handle.write(SOURCE)

        unopt = run([CIDER, "ir", "--no-opt", source], env)
        opt = run([CIDER, "ir", source], env)
        if unopt.returncode != 0 or opt.returncode != 0:
            failures.append("ir failed: unopt=%r opt=%r" % (
                unopt.stderr, opt.stderr))
        else:
            before = function_body(unopt.stdout, "work")
            after = function_body(opt.stdout, "work")
            if before is None or after is None:
                failures.append("the work function is missing from the IR dump")
            else:
                before_ld, before_st = memory_counts(before)
                after_ld, after_st = memory_counts(after)
                if not (after_ld < before_ld):
                    failures.append(
                        "work loads did not fall: %d -> %d" %
                        (before_ld, after_ld))
                if not (after_st < before_st):
                    failures.append(
                        "work stores did not fall: %d -> %d" %
                        (before_st, after_st))
                if after_ld != 0 or after_st != 0:
                    failures.append(
                        "work memory shape is %d loads / %d stores, "
                        "expected 0 / 0" % (after_ld, after_st))
            escaped_before = function_body(unopt.stdout, "escape_of")
            escaped_after = function_body(opt.stdout, "escape_of")
            if escaped_before is None or escaped_after is None:
                failures.append(
                    "the escape_of function is missing from the IR dump")
            elif memory_counts(escaped_before) != memory_counts(escaped_after):
                failures.append(
                    "an escaping Alloca changed shape: %r -> %r" % (
                        memory_counts(escaped_before),
                        memory_counts(escaped_after)))
            for name in ("branchy", "loop_swap"):
                body = function_body(opt.stdout, name)
                if body is None:
                    failures.append("%s is missing from the IR dump" % name)
                elif memory_counts(body) != (0, 0):
                    failures.append(
                        "%s memory shape is %r, expected (0, 0)" %
                        (name, memory_counts(body)))
                elif " phi " not in body:
                    failures.append("%s has no phi" % name)

        legs = [
            ("native", [CIDER, "run", source]),
            ("vm-opt", [CIDER, "run", "--target", "vm", source]),
            ("vm-unopt", [CIDER, "run", "--target", "vm", "--no-opt", source]),
        ]
        for name, command in legs:
            got = run(command, env)
            if got.returncode != 54 or got.stdout != "" or got.stderr != "":
                failures.append(
                    "%s: exit %r, stdout %r, stderr %r; expected 54, '', ''" %
                    (name, got.returncode, got.stdout, got.stderr))

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("slot forwarding: native, vm-unopt and vm-opt passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
