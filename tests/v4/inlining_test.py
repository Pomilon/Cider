#!/usr/bin/env python3
"""v4 inlining end-to-end test.

The unit test pins eligibility, clone shape and refusal counts. This test
proves the pass inside the real pipeline on all three legs: native codegen,
the optimized VM, and the unoptimized VM. Each program is small enough that
the helper call must disappear from optimized IR, while the recursive case
must keep its call because a direct self-recursive callee is refused.

It also pins the property the whole compiler promises at OUTPUT.md L5: two
compiles of the same source produce byte-identical assembly, now with cloned
bodies in the output.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

CASES = {
    # Two calls in one original block: the second lives in the continuation
    # the first split creates. g counts side effects in order.
    "side_effects": ("""var g: i32 = 0;

func bump(x: i32) -> i32 {
    g = g + x;
    return g;
}

func work(x: i32) -> i32 {
    var a: i32 = bump(x);
    var b: i32 = bump(a);
    return a * 10 + b;
}

func _start() -> void {
    var code: i32 = work(3);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 36, "work", "bump", 2, 0),

    # Two return paths: the continuation gets one return phi.
    "two_returns": ("""func pick(c: bool, a: i32, b: i32) -> i32 {
    if (c) { return a; }
    return b;
}

func _start() -> void {
    var code: i32 = pick(true, 4, 9) * 10 + pick(false, 4, 9);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 49, "_start", "pick", 2, 0),

    # Two words come home in ordinary result values.
    "multiword": ("""func pair() -> [u64; 2] {
    var r: [u64; 2] = [11, 22];
    return r;
}

func _start() -> void {
    var p: [u64; 2] = pair();
    var code: i32 = (p[0] + p[1]) as i32;
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 33, "_start", "pair", 1, 0),

    # A three-word struct takes the hidden sret pointer.
    "sret": ("""struct Wide { var a: i64; var b: i64; var c: i64; }

func make(x: i64) -> Wide {
    return Wide { a: x, b: x + 1, c: x + 2 };
}

func _start() -> void {
    var w: Wide = make(5);
    var code: i32 = (w.a + w.b + w.c) as i32;
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 18, "_start", "make", 1, 0),

    # Direct self-recursion is refused, so the call must survive optimization.
    "recursion": ("""func fact(n: i32) -> i32 {
    if (n <= 1) { return 1; }
    return n * fact(n - 1);
}

func _start() -> void {
    var code: i32 = fact(5);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 120, "_start", "fact", 1, 1),

    # A call in a loop body is inlined without changing the loop's meaning.
    "loop_inline": ("""func bump(x: i32) -> i32 { return x + 1; }

func loop_sum(n: i32) -> i32 {
    var s: i32 = 0;
    var i: i32 = 0;
    while (i < n) {
        s = s + bump(i);
        i = i + 1;
    }
    return s;
}

func _start() -> void {
    var code: i32 = loop_sum(5);
    asm { "mov rax, 60" "mov edi, {code}" "syscall" }
}
""", 15, "loop_sum", "bump", 1, 0),
}


def run(args, env=None, timeout=180):
    return subprocess.run(args, capture_output=True, text=True,
                          env=env, timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout[:500], proc.stderr[:500])


def function_ir(dump, name):
    match = re.search(r"^func %s\n(.*?)(?=^func |\Z)" % re.escape(name),
                      dump, re.MULTILINE | re.DOTALL)
    return match.group(1) if match else None


def call_count(body, callee):
    return len(re.findall(r"^  .*?call [^\n]*@" + re.escape(callee) + r"\b",
                          body, re.MULTILINE))


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    failures = []
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))

    with tempfile.TemporaryDirectory(prefix="cider-inline-") as tmp:
        for name, (source_text, expected, probe, callee, unopt_min,
                   opt_expected) in CASES.items():
            source = os.path.join(tmp, name + ".cid")
            with open(source, "w") as handle:
                handle.write(source_text)

            for leg, extra in (("native", []),
                               ("vm-opt", ["--target", "vm"]),
                               ("vm-unopt", ["--target", "vm", "--no-opt"])):
                got = run([CIDER, "run"] + extra + [source], env)
                if (got.returncode, got.stdout, got.stderr) != (expected, "", ""):
                    failures.append(
                        "%s %s: %s; expected exit %d, '', ''" %
                        (name, leg, describe(got), expected))

            unopt = run([CIDER, "ir", "--no-opt", source], env)
            opt = run([CIDER, "ir", source], env)
            if unopt.returncode != 0 or opt.returncode != 0:
                failures.append("%s ir: unopt=%s opt=%s" %
                                (name, describe(unopt), describe(opt)))
                continue
            before_body = function_ir(unopt.stdout, probe)
            after_body = function_ir(opt.stdout, probe)
            if before_body is None or after_body is None:
                failures.append("%s: function %s is missing from the IR dump" %
                                (name, probe))
                continue
            before = call_count(before_body, callee)
            after = call_count(after_body, callee)
            if before < unopt_min:
                failures.append(
                    "%s: unoptimised %s has %d calls to %s, expected at least %d" %
                    (name, probe, before, callee, unopt_min))
            if after != opt_expected:
                failures.append(
                    "%s: optimised %s has %d calls to %s, expected %d" %
                    (name, probe, after, callee, opt_expected))
            if name == "two_returns" and " phi " not in after_body:
                failures.append("two_returns: the return phi is missing")
            if name == "sret" and not re.search(r"^  .*?load .*\n", after_body,
                                                re.MULTILINE):
                failures.append("sret: the caller has no buffer read")

        # Two compiles of the same source must be byte-identical. The inliner
        # allocates ids and labels from a deterministic scan order.
        source = os.path.join(tmp, "side_effects.cid")
        first = os.path.join(tmp, "first.s")
        second = os.path.join(tmp, "second.s")
        one = run([CIDER, "compile", source, "-o", first], env)
        two = run([CIDER, "compile", source, "-o", second], env)
        if one.returncode != 0 or two.returncode != 0:
            failures.append("determinism compile: %s / %s" %
                            (describe(one), describe(two)))
        elif open(first, "rb").read() != open(second, "rb").read():
            failures.append("two compiles produced different assembly")

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("inlining: native, vm-unopt and vm-opt passed; assembly is stable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
