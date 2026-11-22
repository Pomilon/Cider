#!/usr/bin/env python3
"""Task 10's compact-frame end-to-end test.

The unit test pins the frame arithmetic in isolation. This test proves the
fast emitter's frame runs: a program with thirty locals and nested calls
compiles and exits with the expected code under `--codegen=fast`, both with
and without the optimiser.

The assembly comparison is the RED half of the test: before Task 10, `fast`
routed to the legacy emitter and the two outputs were byte-identical. A fast
backend with a compact frame necessarily changes the prologue/epilogue text,
so identical output means the fast path never ran.
"""

import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

SOURCE = """func step(x: i32) -> i32 {
    return x + 1;
}

func descend(n: i32) -> i32 {
    if (n <= 0) { return 0; }
    return n + descend(n - 1);
}

func workload(x: i32) -> i32 {
    var v0: i32 = x + 0;
    var v1: i32 = x + 1;
    var v2: i32 = x + 2;
    var v3: i32 = x + 3;
    var v4: i32 = x + 4;
    var v5: i32 = x + 5;
    var v6: i32 = x + 6;
    var v7: i32 = x + 7;
    var v8: i32 = x + 8;
    var v9: i32 = x + 9;
    var v10: i32 = x + 10;
    var v11: i32 = x + 11;
    var v12: i32 = x + 12;
    var v13: i32 = x + 13;
    var v14: i32 = x + 14;
    var v15: i32 = x + 15;
    var v16: i32 = x + 16;
    var v17: i32 = x + 17;
    var v18: i32 = x + 18;
    var v19: i32 = x + 19;
    var v20: i32 = x + 20;
    var v21: i32 = x + 21;
    var v22: i32 = x + 22;
    var v23: i32 = x + 23;
    var v24: i32 = x + 24;
    var v25: i32 = x + 25;
    var v26: i32 = x + 26;
    var v27: i32 = x + 27;
    var v28: i32 = x + 28;
    var v29: i32 = x + 29;
    return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9
         + v10 + v11 + v12 + v13 + v14 + v15 + v16 + v17 + v18 + v19
         + v20 + v21 + v22 + v23 + v24 + v25 + v26 + v27 + v28 + v29
         + step(x) + descend(4);
}

func _start() -> void {
    var code: i32 = workload(3);
    asm {
        "mov rax, 60"
        "mov edi, {code}"
        "syscall"
    }
}
"""

# 0+..+29 = 435, thirty copies of 3 = 90, step(3) = 4, descend(4) = 10.
EXPECTED_EXIT = (435 + 90 + 4 + 10) & 0xFF

# The outgoing-area case: eight integer arguments, a mixed seven-integer plus
# float call, and both integer and float variadic calls. Every one needs words
# past the SysV register row, so the fast path must stage them in the frame's
# outgoing area while the legacy path pushes them. The two emitters must agree
# on the answer.
STAGING_SOURCE = """from std.varargs import VaList, va_start, va_end, va_arg_i64,
    va_arg_f64;

func eight(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32, g: i32,
           h: i32) -> i32 {
    return a + b + c + d + e + f + g + h;
}

func mixed(a: i32, b: i32, c: i32, d: i32, e: i32, f: i32, g: i32,
           x: f64) -> i32 {
    return a + b + c + d + e + f + g + (x as i32);
}

func vsum(n: i32, ...) -> i64 {
    var ap: VaList = uninit;
    va_start(&ap);
    var total: i64 = 0;
    var i: i32 = 0;
    while (i < n) {
        total = total + va_arg_i64(&ap);
        i = i + 1;
    }
    va_end(&ap);
    return total;
}

func vsum_f64(n: i32, ...) -> f64 {
    var ap: VaList = uninit;
    va_start(&ap);
    var total: f64 = 0.0;
    var i: i32 = 0;
    while (i < n) {
        total = total + va_arg_f64(&ap);
        i = i + 1;
    }
    va_end(&ap);
    return total;
}

func _start() -> void {
    var bad: i32 = 0;
    if (eight(1, 2, 3, 4, 5, 6, 7, 8) != 36) { bad = 1; }
    if (mixed(1, 2, 3, 4, 5, 6, 7, 3.9) != 31) { bad = 2; }
    if (vsum(8, 1, 2, 3, 4, 5, 6, 7, 14) != 42) { bad = 3; }
    if (vsum_f64(8, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0)
            as i64 != 36) { bad = 4; }
    asm {
        "mov rax, 60"
        "mov edi, {bad}"
        "syscall"
    }
}
"""

POMEOS_SOURCE = """func _start() -> void {
    var x: i64 = 7;
    asm {
        "mov rax, 60"
        "mov rdi, {x}"
        "syscall"
    }
}
"""

UEFI_SOURCE = """msabi func efi_main(image_handle: *void,
                            system_table: *void) -> int {
    asm { "nop" }
    return 0;
}
"""


def run(args, env):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=120)


def compile_asm(kind, source, out, env, no_opt):
    command = [str(CIDER), "compile", "--codegen=" + kind]
    if no_opt:
        command.append("--no-opt")
    command.extend(["-o", str(out), str(source)])
    return run(command, env)


def compile_target(target, kind, source, out, env):
    return run([str(CIDER), "compile", "--target", target,
                "--codegen=" + kind, "-o", str(out), str(source)], env)


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    failures = []
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    with tempfile.TemporaryDirectory(prefix="cider-fast-frame-") as tmp:
        source = os.path.join(tmp, "frame.cid")
        with open(source, "w") as handle:
            handle.write(SOURCE)

        legacy = os.path.join(tmp, "legacy.s")
        fast = os.path.join(tmp, "fast.s")
        legacy_build = compile_asm("legacy", source, legacy, env, no_opt=True)
        fast_build = compile_asm("fast", source, fast, env, no_opt=True)
        if legacy_build.returncode != 0:
            failures.append("legacy compile failed: " + legacy_build.stderr)
        if fast_build.returncode != 0:
            failures.append("fast compile failed: " + fast_build.stderr)
        if not failures:
            with open(legacy, encoding="utf-8") as handle:
                legacy_text = handle.read()
            with open(fast, encoding="utf-8") as handle:
                fast_text = handle.read()
            if legacy_text == fast_text:
                failures.append(
                    "legacy and fast emitted identical text; the compact "
                    "frame path did not run")
            # The byte difference above is the discriminator; this is only a
            # sanity check that the fast frame still reserves storage. The
            # teardown itself is `leave` (or the eliminated frame's
            # `add rsp, ...; ret`), never a redundant adjustment before
            # `leave`.
            if "sub rsp," not in fast_text:
                failures.append(
                    "fast assembly has no frame setup")

        legs = [
            ("fast", [str(CIDER), "run", "--codegen=fast", source]),
            ("fast-no-opt", [str(CIDER), "run", "--codegen=fast",
                             "--no-opt", source]),
        ]
        for name, command in legs:
            got = run(command, env)
            if got.returncode != EXPECTED_EXIT:
                failures.append(
                    "%s: exit %r, stdout %r, stderr %r; expected %d" %
                    (name, got.returncode, got.stdout, got.stderr,
                     EXPECTED_EXIT))

        # Outgoing-area staging: seven-plus integer arguments, a float
        # argument and both variadic classes. Fast and legacy must agree.
        staging = os.path.join(tmp, "staging.cid")
        with open(staging, "w") as handle:
            handle.write(STAGING_SOURCE)
        outcomes = {}
        for name, command in (
                ("legacy", [str(CIDER), "run", "--codegen=legacy", staging]),
                ("legacy-no-opt", [str(CIDER), "run", "--codegen=legacy",
                                   "--no-opt", staging]),
                ("fast", [str(CIDER), "run", "--codegen=fast", staging]),
                ("fast-no-opt", [str(CIDER), "run", "--codegen=fast",
                                 "--no-opt", staging])):
            got = run(command, env)
            outcomes[name] = (got.stdout, got.stderr, got.returncode)
            if got.returncode != 0 or got.stdout != "" or got.stderr != "":
                failures.append(
                    "%s staging: exit %r, stdout %r, stderr %r; "
                    "expected exit 0 with no output" %
                    (name, got.returncode, got.stdout, got.stderr))
        if len(set(outcomes.values())) > 1:
            failures.append(
                "fast and legacy disagree on the staging program: %r" %
                (outcomes,))

        # Non-native targets stay on the legacy emitter during bring-up.
        # `--codegen=fast` must compile them, and the text must match legacy
        # exactly, proving the untested target did not select the fast frame.
        for target, text in (("pomeos", POMEOS_SOURCE), ("uefi", UEFI_SOURCE)):
            source_path = os.path.join(tmp, target + ".cid")
            with open(source_path, "w") as handle:
                handle.write(text)
            legacy_path = os.path.join(tmp, target + "-legacy.s")
            fast_path = os.path.join(tmp, target + "-fast.s")
            legacy_build = compile_target(
                target, "legacy", source_path, legacy_path, env)
            fast_build = compile_target(
                target, "fast", source_path, fast_path, env)
            if legacy_build.returncode != 0:
                failures.append(
                    "%s legacy compile failed: %s" %
                    (target, legacy_build.stderr))
            if fast_build.returncode != 0:
                failures.append(
                    "%s fast compile failed: %s" %
                    (target, fast_build.stderr))
            if legacy_build.returncode == 0 and fast_build.returncode == 0:
                with open(legacy_path, encoding="utf-8") as handle:
                    legacy_text = handle.read()
                with open(fast_path, encoding="utf-8") as handle:
                    fast_text = handle.read()
                if legacy_text != fast_text:
                    failures.append(
                        "%s fast targeted the untested compact frame; "
                        "non-native fast must fall back to legacy" % target)

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        return 1
    print("fast frame: compact assembly, staging and non-native fallback passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
