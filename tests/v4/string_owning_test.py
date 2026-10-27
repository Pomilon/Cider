#!/usr/bin/env python3
"""v4 owning String test.

`String` is ordinary library code in `std.string`, built on `std.alloc`.
It is never a prelude name: a program must import it explicitly, and the
native link must resolve its storage through the library allocator rather
than a compiler-owned heap. The positive program runs on native and both
VM legs, formats into the string and into caller storage, and cleans up
with an explicit deinit call.
"""

import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))
LIB = os.path.join(REPO, "modules")

POSITIVE = """from std.alloc import malloc, free;
from std.io import print, exit;
from std.string import String, string_from, string_deinit;
from std.string import string_append, string_append_byte, string_append_int;
from std.string import string_append_bytes, string_view, string_len, format_hex;

var keep_malloc: *void = &malloc as *void;

func _start() -> void {
    var raw: *u8 = malloc(8) as *u8;
    if (raw == nilPtr) {
        exit(1);
    }
    free(raw as *void);

    var s: String = string_from("v4");
    string_append_byte(&s, 33);
    string_append(&s, " hex=");
    string_append_int(&s, -7);
    string_append(&s, " ");
    var buf: [u8; 8] = [0, ...];
    var n: int = format_hex(&buf[0], 8, 255 as u64);
    if (n != 2) {
        exit(2);
    }
    if (buf[0] != 102 || buf[1] != 102) {
        exit(3);
    }
    string_append_bytes(&s, &buf[0], n);
    if (string_len(&s) != 13) {
        exit(4);
    }
    print(string_view(&s));
    print("\\n");
    string_deinit(&s);
    if (string_len(&s) != 0) {
        exit(5);
    }
    exit(0);
}
"""

NO_IMPORT = """func takes(s: String) -> void {
}

func _start() -> void {
}
"""

FORBIDDEN = (
    "cider_print",
    "cider_panic",
    "cider_alloc",
    "cider_free",
    "cider_realloc",
    "cider_itoa_signed",
    "cider_itoa_unsigned",
    "cider_ftoa",
    "cider_fini_run",
)

LEGS = (
    ("native", []),
    ("vm-unopt", ["--target", "vm", "--no-opt"]),
    ("vm-opt", ["--target", "vm"]),
)


def run(args, env=None, timeout=60):
    return subprocess.run(
        args, capture_output=True, text=True, env=env, timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def library_env():
    env = dict(os.environ)
    env["CIDER_LIB"] = LIB
    return env


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1

    failures = []
    with tempfile.TemporaryDirectory(prefix="cider-v4-owning-") as tmp:
        def source(name, text):
            path = os.path.join(tmp, name)
            with open(path, "w") as handle:
                handle.write(text)
            return path

        # `String` is not a prelude or compiler type: without the explicit
        # import, the name never binds.
        no_import = source("no_import.cid", NO_IMPORT)
        checked = run([CIDER, "check", no_import], env=library_env())
        if checked.returncode == 0:
            failures.append("an unimported String was recognized")
        elif "String" not in checked.stderr:
            failures.append(
                "unimported String diagnostic does not name it: " +
                describe(checked))

        positive = source("positive.cid", POSITIVE)

        # Native: compile to assembly, prove the library (not a compiler
        # helper) supplies allocation and String operations, assemble and
        # link with `ld -nostdlib`, then run.
        asm = os.path.join(tmp, "positive.s")
        obj = os.path.join(tmp, "positive.o")
        binary = os.path.join(tmp, "positive.bin")
        compiled = run([CIDER, "compile", positive, "-o", asm],
                       env=library_env())
        if compiled.returncode != 0:
            failures.append("owning String compile: " + describe(compiled))
        else:
            text = open(asm).read()
            if "std_alloc__malloc:" not in text:
                failures.append("std.alloc malloc was not emitted as a library function")
            if "std_string__string_from:" not in text:
                failures.append("std.string String was not emitted as a library function")
            for symbol in FORBIDDEN:
                if symbol in text:
                    failures.append(
                        "compiler runtime symbol leaked: %s" % symbol)

            assembled = run(["as", "-o", obj, asm])
            if assembled.returncode != 0:
                failures.append("owning String assembly: " + describe(assembled))
            else:
                linked = run([
                    "ld", "-nostdlib", "-e", "_start", "-o", binary, obj,
                ])
                if linked.returncode != 0:
                    failures.append("owning String link: " + describe(linked))
                else:
                    executed = run([binary])
                    if (executed.returncode, executed.stdout,
                            executed.stderr) != (0, "v4! hex=-7 ff\n", ""):
                        failures.append(
                            "owning String native run: " + describe(executed))

        # Both VM legs interpret the same library source and answer the
        # same bytes.
        for leg, extra in LEGS:
            ran = run([CIDER, "run"] + extra + [positive], env=library_env())
            if (ran.returncode, ran.stdout, ran.stderr) != (
                    0, "v4! hex=-7 ff\n", ""):
                failures.append(
                    "owning String %s run: %s" % (leg, describe(ran)))

    for failure in failures:
        print("FAIL: " + failure, file=sys.stderr)
    if failures:
        return 1
    print("owning String: native, vm-unopt and vm-opt passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
