#!/usr/bin/env python3
"""v4 CFG block cleanup end-to-end test.

`src/opt/blocks.cpp` removes an empty `Br target` block and merges a block
with one unconditional predecessor. The unit test pins every rule, including
Switch edges, phi rewrites and refusals. This gate proves the pass inside the
real pipeline on source programs:

- `nest` has nested if/else arms, one of them empty;
- `chain` has a C-style for loop whose step block is a single-predecessor
  successor of the body;
- `loopy` has a while loop with an empty then arm inside it.

Each function carries a harmless `mov rax, rax` asm block. Inlining refuses a
callee containing asm, so the functions survive in the whole-program assembly
and their block-label counts can be compared before and after optimization.
Native and both VM legs must agree on the answer, optimized IR must be a
subsequence of the unoptimized block order and shorter, assembly must emit
fewer branch labels for every function, and two compiles must be byte-identical.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

SOURCE = """func nest(a: i32) -> i32 {
    asm { "mov rax, rax" }
    var x: i32 = a;
    if (a > 0) {
        if (a > 1) {
            x = x + 1;
        } else {
        }
    } else {
        x = x + 2;
    }
    return x;
}

func chain(n: i32) -> i32 {
    asm { "mov rax, rax" }
    var s: i32 = 0;
    for (var i: i32 = 0; i < n; i = i + 1) {
        s = s + i;
    }
    return s;
}

func loopy(n: i32) -> i32 {
    asm { "mov rax, rax" }
    var s: i32 = 0;
    var i: i32 = 0;
    while (i < n) {
        if (i == 2) {
        } else {
            s = s + i;
        }
        i = i + 1;
    }
    return s;
}

func _start() -> void {
    var code: i32 = nest(7) + chain(4) + loopy(5);
    asm {
        "mov rax, 60"
        "mov edi, {code}"
        "syscall"
    }
}
"""

EXPECTED_EXIT = 22
FUNCTIONS = ("nest", "chain", "loopy")


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


def assembly_labels(text, name):
    return [int(label) for label in
            re.findall(r"^\.L_%s_(\d+):$" % re.escape(name), text,
                       re.MULTILINE)]


def total_assembly_labels(text):
    return len(re.findall(r"^\.L_[A-Za-z0-9_]+_\d+:$", text, re.MULTILINE))


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
    with tempfile.TemporaryDirectory(prefix="cider-blocks-") as tmp:
        source = os.path.join(tmp, "blocks.cid")
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
            for name in FUNCTIONS:
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
                if len(after) >= len(before):
                    failures.append(
                        "%s: block cleanup removed no block (%r -> %r)" %
                        (name, before, after))
                else:
                    removed_labels[name] = [label for label in before
                                            if label not in after]

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
            if total_assembly_labels(optimized_text) >= total_assembly_labels(
                    raw_text):
                failures.append(
                    "optimized assembly did not reduce branch labels: "
                    "%d -> %d" % (total_assembly_labels(raw_text),
                                  total_assembly_labels(optimized_text)))
            for name in FUNCTIONS:
                before = assembly_labels(raw_text, name)
                after = assembly_labels(optimized_text, name)
                if not after:
                    failures.append("%s has no assembly labels" % name)
                    continue
                if len(after) >= len(before):
                    failures.append(
                        "%s: assembly branch labels did not shrink for a "
                        "surviving function: %r -> %r" % (name, before, after))
                for label in removed_labels.get(name, []):
                    marker = ".L_%s_%d:" % (name, label)
                    if marker in optimized_text:
                        failures.append(
                            "assembly still defines removed label %s" % marker)
        if one.returncode == 0 and two.returncode == 0:
            if open(first, "rb").read() != open(second, "rb").read():
                failures.append("two optimized compiles are not byte-identical")

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("blocks: native, vm-unopt and vm-opt agree; empty blocks and a "
          "single-predecessor block disappear from IR and assembly; output "
          "is stable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
