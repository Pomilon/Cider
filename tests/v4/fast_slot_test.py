#!/usr/bin/env python3
"""Task 15.1's fast parameter homes and Task 15.2's stack-word stores.

Before Task 15.1 the allocator excluded every parameter and every call word,
and the prologue wrote parameters only to their frame slots. This test compiles
`probe` on the fast backend and asserts that its parameters are written to
register homes: no `mov [rsp + ...], rdi/rsi` (with or without the `qword ptr`
width spelling) may remain in the body.

Task 15.2 additionally pins the seventh integer argument, `keep`, which is
homed in a callee-saved register. The outgoing stack word must be stored
directly from that home, with no `mov rax, <home>` round-trip and no spill-slot
staging. The peephole already folds the store's source to the home, so the
round-trip assertion is what distinguishes the old emitter: it leaves the dead
`mov rax, <home>` immediately before the direct store. The legacy backend is
untouched and keeps the slot prologue.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

PROGRAM = """func sum7(a: i64, b: i64, c: i64, d: i64, e: i64, f: i64,
          g: i64) -> i64 {
    return a + b + c + d + e + f + g;
}

func probe(p: i64, q: i64) -> i64 {
    var keep: i64 = p + q;
    var r: i64 = sum7(1, 2, 3, 4, 5, 6, keep);
    return keep * 1000 + r;
}

func _start() -> void {
    var code: i32 = (probe(2, 3) as i32);
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
    with tempfile.TemporaryDirectory(prefix="cider-fast-slot-") as tmp:
        source = os.path.join(tmp, "slots.cid")
        with open(source, "w") as handle:
            handle.write(PROGRAM)
        asm = os.path.join(tmp, "slots.s")
        compiled = run([str(CIDER), "compile", "--codegen=fast", "--no-opt",
                        "-o", asm, source], env)
        if compiled.returncode != 0:
            failures.append("compile: " + describe(compiled))
        else:
            text = open(asm, encoding="utf-8").read()
            probe_body = function_body(text, "probe")
            if probe_body is None:
                failures.append("probe is missing (asm: %r)" % text[:400])
            else:
                check_failed = False
                # The emitter writes a parameter cell as `mov [rsp + N], rdi`
                # and, on paths that name the width, as `mov qword ptr
                # [rsp + N], rdi`. Either spelling means the parameter is still
                # spending its life in the frame slot.
                if re.search(r"mov(?: qword ptr)? \[rsp[^\n]*, rdi",
                             probe_body):
                    failures.append(
                        "parameter p is still stored to its frame slot")
                    check_failed = True
                if re.search(r"mov(?: qword ptr)? \[rsp[^\n]*, rsi",
                             probe_body):
                    failures.append(
                        "parameter q is still stored to its frame slot")
                    check_failed = True
                # Task 15.2: `keep` is the seventh integer argument, so it is
                # the one outgoing stack word. It is homed in a callee-saved
                # register; the store must name that home directly. The
                # peephole can already fold the store source, so also reject
                # the leftover `mov rax, <same home>` immediately above it.
                homes = r"r(?:bx|12|13|14|15|8|9|10|11)"
                if not re.search(
                        r"mov(?: qword ptr)? \[rsp \+ \d+\], %s" % homes,
                        probe_body):
                    failures.append(
                        "stack argument was not stored directly from a "
                        "register home")
                    check_failed = True
                round_trip = re.search(
                    r"(?m)^  mov rax, (%s)\n"
                    r"  mov qword ptr \[rsp \+ \d+\], \1$" % homes,
                    probe_body)
                if round_trip is not None:
                    failures.append(
                        "stack argument was round-tripped through rax "
                        "(register %s)" % round_trip.group(1))
                    check_failed = True
                if re.search(
                        r"(?m)^  mov rax, \[rsp \+ \d+\]\n"
                        r"  mov qword ptr \[rsp \+ \d+\], rax$",
                        probe_body):
                    failures.append(
                        "stack argument was staged through a spill slot")
                    check_failed = True
                if check_failed:
                    failures.append("probe body: %r" % probe_body)

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast slots: parameters are homed in registers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
