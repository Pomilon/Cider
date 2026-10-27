#!/usr/bin/env python3
"""v4 constant control-flow simplification end-to-end test.

The unit test pins the IR rules, including Switch terminators. This test proves
the pass inside the real pipeline on source programs: a constant true branch
and a constant false branch each lose their dead arm, an already-unreachable
block disappears, and a loop's nonconstant branch survives. Native and both VM
legs agree; optimized assembly omits every block label the optimized IR no
longer has; two compiles are byte-identical.

Source has if/else but no switch syntax in this slice, so Switch folding is
covered only by `simplify_test` direct IR. The gate pins the source-level shape
and the runtime result of the remaining rules.

`pick` has a loop and then `if (1 == 1)`; `dead` has `if (1 == 0)`; `sum` only
has a loop. The pipeline folds each constant branch, removes the unreachable
arm, and leaves `sum`'s loop branch alone. `sum` is small enough to inline into
`_start` in the optimized output, so the loop check is made on `sum`'s IR, not
on a label that whole-program pruning may have removed from assembly.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

SOURCE = """func pick(a: i32) -> i32 {
    var t: i32 = 0;
    var i: i32 = 0;
    while (i < 3) {
        t = t + a * 3 + a * 5 + i;
        i = i + 1;
    }
    if (1 == 1) {
        return t + 10;
    }
    return t - 90;
}

func dead(a: i32) -> i32 {
    var x: i32 = a * 2 + a * 4 + a * 6 + a * 8;
    if (1 == 0) {
        return x * 100;
    }
    return x + 1;
}

func sum(n: i32) -> i32 {
    var s: i32 = 0;
    var i: i32 = 0;
    while (i < n) {
        s = s + i * 2;
        i = i + 1;
    }
    return s;
}

func _start() -> void {
    var code: i32 = pick(5) + dead(2) + sum(5);
    asm {
        "mov rax, 60"
        "mov edi, {code}"
        "syscall"
    }
}
"""

EXPECTED_EXIT = 194


def run(args, env):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=120)


def function_body(dump, name):
    match = re.search(r"^func %s\n(.*?)(?=^func |\Z)" % re.escape(name),
                      dump, re.MULTILINE | re.DOTALL)
    return match.group(1) if match else None


def block_labels(body):
    return [int(label) for label in re.findall(r"^block (\d+)\n", body,
                                               re.MULTILINE)]


def is_subsequence(small, large):
    remaining = iter(large)
    return all(any(value == candidate for candidate in remaining)
               for value in small)


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    failures = []
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    with tempfile.TemporaryDirectory(prefix="cider-simplify-") as tmp:
        source = os.path.join(tmp, "simplify.cid")
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
        removed_labels = {}
        if unopt.returncode != 0 or opt.returncode != 0:
            failures.append("ir failed: unopt=%r opt=%r" % (
                unopt.stderr, opt.stderr))
        else:
            for name in ("pick", "dead", "sum"):
                before_body = function_body(unopt.stdout, name)
                after_body = function_body(opt.stdout, name)
                if before_body is None or after_body is None:
                    failures.append("%s is missing from the IR dump" % name)
                    continue
                before = block_labels(before_body)
                after = block_labels(after_body)
                if not is_subsequence(after, before):
                    failures.append(
                        "%s: optimized block order %r is not a subsequence of "
                        "%r" % (name, after, before))
                if name == "sum":
                    if after != before:
                        failures.append(
                            "sum: the loop's nonconstant branch changed the "
                            "block set: %r -> %r" % (before, after))
                else:
                    removed = [label for label in before
                               if label not in after]
                    if not removed:
                        failures.append(
                            "%s: the constant branch did not remove a block "
                            "(%r)" % (name, before))
                    else:
                        removed_labels[name] = removed

        assembly = os.path.join(tmp, "first.s")
        compiled = run([CIDER, "compile", source, "-o", assembly], env)
        if compiled.returncode != 0:
            failures.append("compile failed: %r" % compiled.stderr)
        else:
            text = open(assembly, "r").read()
            for name, labels in removed_labels.items():
                for label in labels:
                    marker = ".L_%s_%d:" % (name, label)
                    if marker in text:
                        failures.append(
                            "assembly still has unreachable label %s" %
                            marker)

        second = os.path.join(tmp, "second.s")
        one = run([CIDER, "compile", source, "-o", assembly], env)
        two = run([CIDER, "compile", source, "-o", second], env)
        if one.returncode != 0 or two.returncode != 0:
            failures.append("determinism compile failed: %r / %r" % (
                one.stderr, two.stderr))
        elif open(assembly, "rb").read() != open(second, "rb").read():
            failures.append("two compiles produced different assembly")

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("simplify: native, vm-unopt and vm-opt passed; constant branches "
          "lost their dead arms; the loop branch survived; assembly is stable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
