#!/usr/bin/env python3
"""v4 module interfaces and the content-addressed build cache.

Proves the separate-compilation slice end to end:

- compiling a library emits a `.cidm` beside its object;
- the entry module compiles against that interface after the library source
  is renamed away, then links and runs;
- a stale interface (source hash changed) and an incompatible format are
  diagnostics, and `--allow-stale-interface` falls back to source;
- a missing interface falls back to source, while `--require-interface` names
  the missing artifact;
- `cider build` reuses cached objects on an unchanged tree, recompiles a
  changed module, and recompiles its dependents through the dependency hash.

The interface is a Cider-source-shaped declaration list: bodies are omitted,
so the importer never parses the imported module's function bodies.
"""

import os
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))


LIB = """export const N: int = 2;

export struct Pair {
    a: int;
    b: int;
    vals: [int; N];
}

export struct Wrap {
    pair: Pair;
}

export var table: [int; N] = [1, 2];

export var shared: int = 3;

export func add(a: int, b: int) -> int {
    return a + b;
}
"""

MAIN = """import lib;
from lib import add, Pair, Wrap;

func _start() -> void {
    var p: Pair = Pair { a: 30, b: 8, vals: [1, 2] };
    var w: Wrap = Wrap { pair: p };
    var code: int = add(w.pair.a, w.pair.b) + lib.table[0] + lib.shared;
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}
"""


def run(args, env=None, timeout=120):
    return subprocess.run(args, capture_output=True, text=True, env=env,
                          timeout=timeout)


def describe(proc):
    return "exit %r, stdout %r, stderr %r" % (
        proc.returncode, proc.stdout, proc.stderr)


def write(path, text):
    with open(path, "w") as handle:
        handle.write(text)


def read(path):
    with open(path) as handle:
        return handle.read()


def main():
    if not os.path.exists(CIDER):
        print("FAIL: compiler not found: %s" % CIDER, file=sys.stderr)
        return 1
    for tool in ("as", "ld"):
        if shutil.which(tool) is None:
            print("SKIP: %s is unavailable" % tool)
            return 77

    failures = []

    def fail(text):
        failures.append(text)

    with tempfile.TemporaryDirectory(prefix="cider-v4-interface-") as tmp:
        lib_path = os.path.join(tmp, "lib.cid")
        main_path = os.path.join(tmp, "main.cid")
        ifdir = os.path.join(tmp, "ifdir")
        os.makedirs(ifdir)
        write(lib_path, LIB)
        write(main_path, MAIN)

        # --- Library object + interface, entry against interface only ------
        lib_obj = os.path.join(ifdir, "lib.o")
        lib_iface = os.path.join(ifdir, "lib.cidm")
        main_obj = os.path.join(tmp, "main.o")
        app = os.path.join(tmp, "app")
        compiled = run([CIDER, "compile", "-c", "--module", "lib",
                        lib_path, "-o", lib_obj])
        if compiled.returncode != 0:
            fail("library object compile: " + describe(compiled))
        elif not os.path.exists(lib_iface):
            fail("library compile did not emit lib.cidm")
        else:
            text = read(lib_iface)
            for needle in ("cider-interface: 1", "cider-interface-module: lib",
                           "func add(a: i32, b: i32) -> i32;",
                           "struct Pair", "vals: [i32; N];",
                           "var table: [i32; N];", "var shared: i32;"):
                if needle not in text:
                    fail("interface does not contain %r:\n%s" % (needle, text))
            if "return" in text:
                fail("interface carries a function body")
        shutil.move(lib_path, lib_path + ".hidden")
        compiled = run([CIDER, "compile", "-c", "--module", "main",
                        "--interface-dir", ifdir, main_path, "-o", main_obj])
        if compiled.returncode != 0:
            fail("entry compile against interface: " + describe(compiled))
        else:
            linked = run([CIDER, "link", "-o", app, main_obj, lib_obj])
            if linked.returncode != 0:
                fail("interface split link: " + describe(linked))
            else:
                executed = run([app])
                if executed.returncode != 42:
                    fail("interface split binary exited %r, not 42" %
                         (executed.returncode,))
        shutil.move(lib_path + ".hidden", lib_path)

        # --- Missing interface: fallback, then --require-interface ---------
        os.unlink(lib_iface)
        fallback = run([CIDER, "compile", "-c", "--module", "main",
                        "--interface-dir", ifdir, main_path, "-o", main_obj])
        if fallback.returncode != 0:
            fail("missing interface did not fall back to source: " +
                 describe(fallback))
        required = run([CIDER, "compile", "-c", "--module", "main",
                        "--interface-dir", ifdir, "--require-interface",
                        main_path, "-o", main_obj])
        if required.returncode == 0:
            fail("--require-interface accepted a missing interface")
        else:
            for needle in ("missing interface", "lib"):
                if needle not in required.stderr:
                    fail("missing-interface diagnostic lacks %r: %s" %
                         (needle, describe(required)))

        # --- Rebuild the interface, then make it stale ---------------------
        rebuilt = run([CIDER, "compile", "-c", "--module", "lib",
                       lib_path, "-o", lib_obj])
        if rebuilt.returncode != 0:
            fail("library rebuild: " + describe(rebuilt))
        write(lib_path, LIB + "\n// changed after the interface was built\n")
        stale = run([CIDER, "compile", "-c", "--module", "main",
                     "--interface-dir", ifdir, main_path, "-o", main_obj])
        if stale.returncode == 0:
            fail("stale interface was accepted")
        else:
            for needle in ("stale", "rebuild"):
                if needle not in stale.stderr:
                    fail("stale diagnostic lacks %r: %s" %
                         (needle, describe(stale)))
        allowed = run([CIDER, "compile", "-c", "--module", "main",
                       "--interface-dir", ifdir, "--allow-stale-interface",
                       main_path, "-o", main_obj])
        if allowed.returncode != 0:
            fail("--allow-stale-interface did not fall back: " +
                 describe(allowed))

        # --- Incompatible format: diagnostic, then explicit fallback -------
        rebuilt = run([CIDER, "compile", "-c", "--module", "lib",
                       lib_path, "-o", lib_obj])
        if rebuilt.returncode != 0:
            fail("library rebuild before version test: " + describe(rebuilt))
        text = read(lib_iface).replace("cider-interface: 1",
                                       "cider-interface: 999", 1)
        write(lib_iface, text)
        bad_version = run([CIDER, "compile", "-c", "--module", "main",
                           "--interface-dir", ifdir, main_path,
                           "-o", main_obj])
        if bad_version.returncode == 0:
            fail("incompatible interface format was accepted")
        elif "interface format" not in bad_version.stderr:
            fail("format diagnostic does not name the format: " +
                 describe(bad_version))
        allowed_version = run([CIDER, "compile", "-c", "--module", "main",
                               "--interface-dir", ifdir,
                               "--allow-stale-interface", main_path,
                               "-o", main_obj])
        if allowed_version.returncode != 0:
            fail("--allow-stale-interface did not accept a bad format "
                 "through source fallback: " + describe(allowed_version))

        # --- Variadic signatures survive the interface round trip ----------
        # VARIADICS.md §9: `...` is rendered in functions and function-pointer
        # types and restored when the interface is imported without source.
        vlib_path = os.path.join(tmp, "vlib.cid")
        vmain_path = os.path.join(tmp, "vmain.cid")
        write(vlib_path, """export extern func printf(fmt: *char, ...) -> int;
export func count(n: int, ...) -> int { return n; }
export type Callback = *func(*char, ...) -> int;
""")
        vlib_obj = os.path.join(ifdir, "vlib.o")
        vlib_iface = os.path.join(ifdir, "vlib.cidm")
        vmain_obj = os.path.join(tmp, "vmain.o")
        vapp = os.path.join(tmp, "vapp")
        compiled = run([CIDER, "compile", "-c", "--module", "vlib",
                        vlib_path, "-o", vlib_obj])
        if compiled.returncode != 0:
            fail("variadic library compile: " + describe(compiled))
        elif not os.path.exists(vlib_iface):
            fail("variadic library compile did not emit vlib.cidm")
        else:
            text = read(vlib_iface)
            for needle in ("extern func printf(fmt: *char, ...) -> i32;",
                           "func count(n: i32, ...) -> i32;",
                           "type Callback = *func(*char, ...) -> i32;"):
                if needle not in text:
                    fail("variadic interface does not contain %r:\n%s" %
                         (needle, text))
        shutil.move(vlib_path, vlib_path + ".hidden")
        write(vmain_path, """import vlib;

func _start() -> void {
    var cb: vlib.Callback = nilPtr;
    var n: int = vlib.count(37, 1, 2);
    var code: int = n + 5;
    asm {
        "mov rax, 60"
        "mov rdi, {code:q}"
        "syscall"
    }
}
""")
        compiled = run([CIDER, "compile", "-c", "--module", "main",
                        "--interface-dir", ifdir, vmain_path, "-o", vmain_obj])
        if compiled.returncode != 0:
            fail("variadic entry compile against interface: " +
                 describe(compiled))
        else:
            linked = run([CIDER, "link", "-o", vapp, vmain_obj, vlib_obj])
            if linked.returncode != 0:
                fail("variadic interface split link: " + describe(linked))
            else:
                executed = run([vapp])
                if executed.returncode != 42:
                    fail("variadic interface binary exited %r, not 42" %
                         (executed.returncode,))
        shutil.move(vlib_path + ".hidden", vlib_path)

        # --- Build cache: hit, miss, dependent invalidation ----------------
        cache_dir = os.path.join(tmp, "cache")
        objects = os.path.join(tmp, "objects")
        app2 = os.path.join(tmp, "app2")
        write(lib_path, LIB)
        first = run([CIDER, "build", "--verbose", "--cache-dir", cache_dir,
                     "--objects-dir", objects, "-o", app2, main_path])
        if first.returncode != 0:
            fail("build first run: " + describe(first))
        else:
            for needle in ("module lib: compiled", "module main: compiled"):
                if needle not in first.stdout:
                    fail("first build did not compile %r: %s" %
                         (needle, first.stdout))
            for path in (os.path.join(objects, "lib.o"),
                         os.path.join(objects, "main.o")):
                if not os.path.exists(path):
                    fail("build did not copy object %s" % path)
            executed = run([app2])
            if executed.returncode != 42:
                fail("built binary exited %r, not 42" % (executed.returncode,))
        if not os.path.exists(os.path.join(cache_dir, "interfaces", "lib.cidm")):
            fail("build did not leave an active lib interface")

        second = run([CIDER, "build", "--verbose", "--cache-dir", cache_dir,
                      "-o", app2, main_path])
        if second.returncode != 0:
            fail("build second run: " + describe(second))
        else:
            for needle in ("module lib: cached", "module main: cached"):
                if needle not in second.stdout:
                    fail("second build did not cache %r: %s" %
                         (needle, second.stdout))
            if "compiled" in second.stdout:
                fail("second build recompiled something: " + second.stdout)

        # A comment changes the library's source hash, so the library and
        # every dependent (main records the interface hash) rebuild.
        write(lib_path, LIB + "\n// library edit\n")
        third = run([CIDER, "build", "--verbose", "--cache-dir", cache_dir,
                     "-o", app2, main_path])
        if third.returncode != 0:
            fail("build after library edit: " + describe(third))
        else:
            for needle in ("module lib: compiled", "module main: compiled"):
                if needle not in third.stdout:
                    fail("library edit did not rebuild %r: %s" %
                         (needle, third.stdout))

        # Only the entry changes: its dependency still hits.
        write(main_path, MAIN + "\n// entry edit\n")
        fourth = run([CIDER, "build", "--verbose", "--cache-dir", cache_dir,
                      "-o", app2, main_path])
        if fourth.returncode != 0:
            fail("build after entry edit: " + describe(fourth))
        else:
            if "module lib: cached" not in fourth.stdout:
                fail("entry edit recompiled the library: " + fourth.stdout)
            if "module main: compiled" not in fourth.stdout:
                fail("entry edit did not rebuild main: " + fourth.stdout)

    if failures:
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("%d v4 interface failure(s)" % len(failures), file=sys.stderr)
        return 1
    print("v4 interfaces: .cidm split compile/link/run, cache hit/miss, "
          "stale/version/missing diagnostics")
    return 0


if __name__ == "__main__":
    sys.exit(main())
