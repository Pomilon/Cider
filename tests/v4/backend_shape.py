#!/usr/bin/env python3
"""Compile Cid to fast assembly and extract one function for shape tests."""
import os
import re
import subprocess
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CIDER = os.environ.get("CIDER_BIN", os.path.join(REPO, "build", "v3", "cider"))

def _extra_args(extra_args):
    # The CLI spells the target as two words; accept the documented
    # `--target=pomeos` form and pass it through correctly.
    args = []
    for arg in extra_args:
        if arg.startswith("--target="):
            args.extend(("--target", arg[len("--target="):]))
        else:
            args.append(arg)
    return args

def compile_asm(source, extra_args=()):
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    with tempfile.TemporaryDirectory(prefix="cider-shape-") as tmp:
        cid = os.path.join(tmp, "shape.cid")
        asm = os.path.join(tmp, "shape.s")
        with open(cid, "w", encoding="utf-8") as handle:
            handle.write(source)
        proc = subprocess.run(
            [str(CIDER), "compile", *_extra_args(extra_args), "-o", asm, cid],
            capture_output=True, text=True, env=env, timeout=180)
        if proc.returncode != 0:
            raise RuntimeError("compile failed: %r" % (proc.stderr[:500],))
        with open(asm, encoding="utf-8") as handle:
            return handle.read()

def function_body(text, name):
    start = re.search(r"(?m)^%s:\n" % re.escape(name), text)
    if start is None:
        return None
    rest = text[start.end():]
    end = re.search(r"(?m)^(?:\.globl |\.weak |(?![.]L_)[A-Za-z_.][^\s:]*:\n)", rest)
    return rest[:end.start()] if end else rest

def count(pattern, text):
    return len(re.findall(pattern, text))

def run_source(source, extra_args=()):
    env = dict(os.environ)
    env.setdefault("CIDER_LIB", os.path.join(REPO, "modules"))
    with tempfile.TemporaryDirectory(prefix="cider-shape-run-") as tmp:
        cid = os.path.join(tmp, "shape.cid")
        with open(cid, "w", encoding="utf-8") as handle:
            handle.write(source)
        return subprocess.run(
            [str(CIDER), "run", *_extra_args(extra_args), cid],
            capture_output=True, text=True, env=env, timeout=180)
