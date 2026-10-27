#!/usr/bin/env python3
"""v4 native entry stack-alignment test.

Linux enters a native process with rsp 16-byte aligned, while an ordinary
SysV callee enters with rsp 8 mod 16 (a call pushed its return address). A
normal Cider function therefore assumes the 8-mod-16 call frame: push rbp
aligns rbp, and a 16-multiple frame leaves rsp aligned for the next call.

Source-defined `_start` has no caller to create that frame. Before this test
existed, the native emitter used the ordinary prologue unchanged, so every
call from `_start` (and every call below it) ran 8 bytes out of alignment.
zlib's `compress2_z` is where Cid-MC saw it: `movaps` on the misaligned frame
raised SIGSEGV on /save.

The test runs the real linked binary. `probe` reports its pushed rbp: a
correctly aligned call makes that value 16-byte aligned. It also checks the
native prologue directly, and pins the scope of the fix by proving an
ordinary function and the PomeOS argc/argv `_start` keep their 16-multiple
frames.
"""

import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

NATIVE = """func probe() -> int {
    var r: int = 0;
    asm { "mov {r:q}, rbp" }
    return r;
}

func _start() -> void {
    var r: int = probe();
    var code: int = 0;
    // A correctly called probe enters at rsp % 16 == 8, so push rbp leaves
    // rbp 16-byte aligned.
    if ((r & 15) != 0) { code = 1; }
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}
"""

POMEOS_ARGS = """func _start(argc: int, argv: **char) -> void {
    asm {
        "mov rax, 2"
        "xor rdi, rdi"
        "syscall"
    }
}
"""

# The same entry carries a cross-block value live across a call, which the
# global allocator homes in rbx. Saving rbx adds a frame cell; the native
# _start frame must still be 8 mod 16 so its first call is aligned.
NATIVE_RBX = """func probe() -> int {
    var r: int = 0;
    asm { "mov {r:q}, rbp" }
    return r;
}

func finish(code: int) -> void {
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}

func _start() -> void {
    var a: int = 7;
    var r: int = 0;
    if (a > 0) {
        r = probe();
    } else {
        r = 2;
    }
    finish((r + a) & 15);
}
"""


def run(args, timeout=120):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def prologue_span(text, name):
    match = re.search(
        r"(?m)^" + re.escape(name) +
        r":\n  push rbp\n  mov rbp, rsp\n  sub rsp, (\d+)\n", text)
    return int(match.group(1)) if match else None


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix="cider-v4-entry-align-") as tmp:
        def source(name, text):
            path = os.path.join(tmp, name)
            with open(path, "w") as handle:
                handle.write(text)
            return path

        native_src = source("align.cid", NATIVE)
        native_asm = os.path.join(tmp, "align.s")
        native_obj = os.path.join(tmp, "align.o")
        native_bin = os.path.join(tmp, "align.bin")
        compiled = run([CIDER, "compile", native_src, "-o", native_asm])
        if compiled.returncode != 0:
            failures.append("native compile: " + describe(compiled))
        else:
            text = open(native_asm).read()
            start_span = prologue_span(text, "_start")
            probe_span = prologue_span(text, "probe")
            if start_span is None:
                failures.append("native _start prologue was not found")
            elif start_span % 16 != 8:
                failures.append(
                    "native _start frame is %d bytes; expected 8 mod 16 "
                    "so its first call is aligned" % start_span)
            if probe_span is None:
                failures.append("native probe prologue was not found")
            elif probe_span % 16 != 0:
                failures.append(
                    "ordinary function frame is %d bytes; expected a "
                    "16-byte multiple" % probe_span)

            assembled = run(["as", "-o", native_obj, native_asm])
            if assembled.returncode != 0:
                failures.append("assembling failed: " + describe(assembled))
            else:
                linked = run([
                    "ld", "-nostdlib", "-e", "_start", "-o", native_bin,
                    native_obj,
                ])
                if linked.returncode != 0:
                    failures.append("linking failed: " + describe(linked))
                else:
                    executed = run([native_bin])
                    if executed.returncode != 0:
                        failures.append(
                            "linked probe saw rbp %% 16 != 0: exit %r" %
                            executed.returncode)

        # A callee-saved rbx home adds a frame cell to _start. The entry's
        # frame must remain 8 mod 16, the save/restore pair must be present,
        # and the first call must still reach probe with an aligned rbp.
        native_rbx_src = source("align_rbx.cid", NATIVE_RBX)
        native_rbx_asm = os.path.join(tmp, "align_rbx.s")
        native_rbx_obj = os.path.join(tmp, "align_rbx.o")
        native_rbx_bin = os.path.join(tmp, "align_rbx.bin")
        compiled = run([CIDER, "compile", native_rbx_src, "-o", native_rbx_asm])
        if compiled.returncode != 0:
            failures.append("native rbx compile: " + describe(compiled))
        else:
            text = open(native_rbx_asm).read()
            start_span = prologue_span(text, "_start")
            if start_span is None:
                failures.append("native rbx _start prologue was not found")
            elif start_span % 16 != 8:
                failures.append(
                    "native rbx _start frame is %d bytes; expected 8 mod 16"
                    % start_span)
            body = text[text.find("_start:"):] if "_start:" in text else ""
            if not re.search(
                    r"(?m)^  mov qword ptr \[rsp \+ \d+\], rbx$", body):
                failures.append("native rbx _start does not save rbx")
            if not re.search(
                    r"(?m)^  mov rbx, qword ptr \[rsp \+ \d+\]$", body):
                failures.append("native rbx _start does not restore rbx")

            assembled = run(["as", "-o", native_rbx_obj, native_rbx_asm])
            if assembled.returncode != 0:
                failures.append(
                    "native rbx assembling failed: " + describe(assembled))
            else:
                linked = run([
                    "ld", "-nostdlib", "-e", "_start", "-o", native_rbx_bin,
                    native_rbx_obj,
                ])
                if linked.returncode != 0:
                    failures.append(
                        "native rbx linking failed: " + describe(linked))
                else:
                    executed = run([native_rbx_bin])
                    if executed.returncode != 7:
                        failures.append(
                            "native rbx _start lost call alignment: exit %r, "
                            "expected 7" % executed.returncode)

        pomeos_src = source("pomeos_args.cid", POMEOS_ARGS)
        pomeos_asm = os.path.join(tmp, "pomeos_args.s")
        compiled = run([
            CIDER, "compile", "--target", "pomeos", pomeos_src,
            "-o", pomeos_asm,
        ])
        if compiled.returncode != 0:
            failures.append("PomeOS argc/argv compile: " + describe(compiled))
        else:
            text = open(pomeos_asm).read()
            span = prologue_span(text, "_start")
            if span is None:
                failures.append("PomeOS _start prologue was not found")
            elif span % 16 != 0:
                failures.append(
                    "PomeOS _start frame is %d bytes; it must keep its "
                    "16-multiple call-like entry" % span)
            if not re.search(r"mov \[rsp \+ \d+\], rdi", text):
                failures.append("PomeOS entry no longer receives argc in rdi")
            if not re.search(r"mov \[rsp \+ \d+\], rsi", text):
                failures.append("PomeOS entry no longer receives argv in rsi")

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 entry-alignment failure(s)" % len(failures),
              file=sys.stderr)
        return 1
    print("v4 entry alignment: native _start aligns calls; "
          "ordinary/PomeOS frames unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
