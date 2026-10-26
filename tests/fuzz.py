#!/usr/bin/env python3
"""Differential fuzzer for the v3 Cider compiler (integer surface, v1).

Strategy: generate a random program from a small expression/statement
grammar, evaluate the SAME tree in Python with C integer semantics, run
the compiled binary, and compare exit codes (low 8 bits). Any mismatch is
a miscompile; any crash/hang/refusal is a bug — the generator stays on
the covered surface, so a loud refusal means the generator is wrong.

What it covers (v1): integer vars, + - * / % == != < <= > >=, unary - ~,
 ternary-selected 0/1, ! && ||, if/else, bounded for(;;), explicit `as`
casts. What it does NOT cover yet: shifts (verify codegen semantics
first), floats, arrays, structs, strings, pointers, division in untaken
short-circuit arms (trap-expectation mode comes later).

Overflow discipline: every intermediate must provably fit the operand
width (interval analysis); anything that might overflow, divide by zero,
or hit MIN/-1 is discarded and regenerated. The point is defined-behavior
differential testing, not UB exploration.

Usage: python3 tests/fuzz.py [--seed N] [--count N] [--keep DIR]
Exit 0 = all pass, 1 = any failure.
"""
import argparse
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CIDER = os.path.join(ROOT, "build", "v3", "cider")
ENV = dict(os.environ, CIDER_LIB=os.path.join(ROOT, "modules"))
sys.path.insert(0, os.path.join(ROOT, "tests"))
from source_entry import with_main_entry  # noqa: E402

WIDTHS = {
    "i8": (-(2**7), 2**7 - 1, True),
    "i16": (-(2**15), 2**15 - 1, True),
    "i32": (-(2**31), 2**31 - 1, True),
    "i64": (-(2**63), 2**63 - 1, True),
    "u8": (0, 2**8 - 1, False),
    "u16": (0, 2**16 - 1, False),
    "u32": (0, 2**32 - 1, False),
    "u64": (0, 2**64 - 1, False),
}

WIDTH_BITS = {"i8": 8, "i16": 16, "i32": 32, "i64": 64,
              "u8": 8, "u16": 16, "u32": 32, "u64": 64}


def cdiv(a, b):
    q = abs(a) // abs(b)
    return -q if (a < 0) != (b < 0) else q


def cmod(a, b):
    return a - cdiv(a, b) * b


class Ctx:
    def __init__(self, rng):
        self.rng = rng
        self.vars = {}  # name -> (width, value)
        self.counter = 0
        self.banned = set()  # names gen_leaf must not pick (loop bodies)

    def fresh(self):
        self.counter += 1
        return f"v{self.counter}"


class Expr:
    """Source text + exact value + (lo,hi) interval + bool flag.

    A bool-typed Expr denotes a Cider bool: it may appear in conditions
    and ternaries, never in arithmetic. An int-typed one is a value of
    its width. The generator never mixes the two without a ternary.
    """

    def __init__(self, src, value, lo, hi, is_bool=False):
        self.src = src
        self.value = value
        self.lo = lo
        self.hi = hi
        self.is_bool = is_bool


def fits(width, v):
    lo, hi, _ = WIDTHS[width]
    return lo <= v <= hi


def gen_leaf(ctx, width):
    lo, hi, signed = WIDTHS[width]
    if ctx.vars and ctx.rng.random() < 0.5:
        cands = [n for n, (w, _) in ctx.vars.items()
                 if w == width and n not in ctx.banned]
        if cands:
            name = ctx.rng.choice(cands)
            _, v = ctx.vars[name]
            return Expr(name, v, v, v)
    v = ctx.rng.randint(-9, 9)
    v = max(lo, min(hi, v))
    if not signed and v < 0:
        v = -v
    return Expr(str(v), v, v, v)


def gen_cmp(ctx, width, depth):
    """A boolean comparison. Operands share the ambient width and each
    carries an explicit `as`, so the text forces the oracle's domain in
    the compiler: bare literals would adapt to the ambient signed type
    and a `~` operand would answer a different value there."""
    op = ctx.rng.choice(["==", "!=", "<", "<=", ">", ">="])
    a = gen_expr(ctx, width, depth)
    b = gen_expr(ctx, width, depth)
    av, bv = a.value, b.value
    r = {"==": av == bv, "!=": av != bv, "<": av < bv,
         "<=": av <= bv, ">": av > bv, ">=": av >= bv}[op]
    v = 1 if r else 0
    src = f"(({a.src}) as {width} {op} (({b.src}) as {width}))"
    return Expr(src, v, 0, 1, is_bool=True)


def gen_expr(ctx, width, depth):
    lo, hi, signed = WIDTHS[width]
    if depth <= 0 or ctx.rng.random() < 0.3:
        return gen_leaf(ctx, width)
    pick = ctx.rng.random()
    if pick < 0.12:
        # A comparison selected into an integer: answers 0/1 through the
        # ternary, which is also ternary coverage for free.
        c = gen_cmp(ctx, width, depth - 1)
        return Expr(f"({c.src} ? 1 : 0)", c.value, 0, 1)
    if pick < 0.2:
        return gen_unary(ctx, width, depth - 1)
    op = ctx.rng.choice(["+", "-", "*", "/", "%"])
    a = gen_expr(ctx, width, depth - 1)
    b = gen_expr(ctx, width, depth - 1)
    if op == "/" or op == "%":
        if b.lo <= 0 <= b.hi:
            return Expr("1", 1, 1, 1)
        if signed and b.lo <= -1 <= b.hi and a.lo == lo:
            return Expr("1", 1, 1, 1)  # MIN / -1 traps: stay away
    if op == "/":
        v = cdiv(a.value, b.value)
    elif op == "%":
        v = cmod(a.value, b.value)
    elif op == "+":
        v = a.value + b.value
    elif op == "-":
        v = a.value - b.value
    else:
        v = a.value * b.value
    if op == "+":
        nlo, nhi = a.lo + b.lo, a.hi + b.hi
    elif op == "-":
        nlo, nhi = a.lo - b.hi, a.hi - b.lo
    elif op == "*":
        ps = (a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi)
        nlo, nhi = min(ps), max(ps)
    elif op == "/":
        # Truncated division is monotone in each argument over a
        # single-signed divisor interval (zero is excluded above, MIN/-1
        # below), so the four endpoint combinations bracket the answer.
        # The old interval (a.lo, a.hi) was unsound: 4 / -1 is -4, which
        # escapes a dividend interval like [-3, 5], and a zero quotient
        # then carried a nonzero-excluding interval past the guard above.
        qs = [cdiv(x, y) for x in (a.lo, a.hi) for y in (b.lo, b.hi)]
        nlo, nhi = min(qs), max(qs)
    else:
        # A remainder takes the dividend's sign and stays below the
        # divisor in magnitude; endpoint combinations do NOT bracket it
        # (a in [2,4] % 3 hits 0 strictly inside), so the analytic bound
        # stands in: sound, slightly wide, never escapes.
        m = max(abs(b.lo), abs(b.hi)) - 1
        nlo, nhi = -m, m
    if nlo < lo or nhi > hi:
        return gen_leaf(ctx, width)  # might overflow: stay small instead
    # Always parenthesized: bare text can re-parse to a different tree
    # than the oracle AST (e.g. `3 / 3 + t % v2`), which frames the
    # compiler for the generator's precedence bug.
    src = f"({a.src} {op} {b.src})"
    return Expr(src, v, nlo, nhi)


def gen_unary(ctx, width, depth):
    lo, hi, signed = WIDTHS[width]
    bits = WIDTH_BITS[width]
    inner = gen_expr(ctx, width, depth)
    if ctx.rng.random() < 0.5:
        if inner.lo == lo and signed:
            return inner  # negating MIN is undefined: keep the value
        v = -inner.value
        if not fits(width, v):
            return inner
        return Expr(f"(-({inner.src}))", v, -inner.hi, -inner.lo)
    inner_src = f"(({inner.src}) as {width})"
    mask = (1 << bits) - 1
    v = (~inner.value) & mask
    if signed and v >= (1 << (bits - 1)):
        v -= 1 << bits
    # ~ is always defined; the interval widens because a range through ~
    # is not a contiguous range, and only definedness matters here.
    return Expr(f"(~{inner_src})", v, lo, hi)


def gen_bool(ctx, width, depth):
    """A boolean expression for conditions. Value is 0/1, is_bool set."""
    r = ctx.rng.random()
    if r < 0.4 or depth <= 0:
        e = gen_expr(ctx, width, depth)
        return Expr(f"((({e.src}) as {width}) != 0)",
                    1 if e.value != 0 else 0, 0, 1, is_bool=True)
    if r < 0.55:
        inner = gen_bool(ctx, width, depth - 1)
        return Expr(f"(!({inner.src}))", 1 - inner.value, 0, 1,
                    is_bool=True)
    op = ctx.rng.choice(["&&", "||"])
    # Both arms come from provably-defined trees, so short-circuit
    # skipping is observable only in the VALUE, never in definedness.
    a = gen_bool(ctx, width, depth - 1)
    b = gen_bool(ctx, width, depth - 1)
    av, bv = a.value, b.value
    if op == "&&":
        v = 1 if (av != 0 and bv != 0) else 0
    else:
        v = 1 if (av != 0 or bv != 0) else 0
    src = f"({a.src} {op} {b.src})"
    return Expr(src, v, 0, 1, is_bool=True)


def gen_program(rng):
    ctx = Ctx(rng)
    width = rng.choice(["i32", "i64", "u32", "u64"])
    lo, hi, _ = WIDTHS[width]
    lines = []
    for _ in range(rng.randint(1, 3)):
        name = ctx.fresh()
        e = gen_expr(ctx, width, 2)
        ctx.vars[name] = (width, e.value)
        # Literals adapt to the declaration, but a computed expression
        # keeps its own type (§13): the identity cast names the width.
        lines.append(f"    var {name}: {width} = ({e.src}) as {width};")
    acc = ctx.fresh()
    lines.append(f"    var {acc}: {width} = 0;")
    acc_val = 0
    for _ in range(rng.randint(2, 5)):
        pick = rng.random()
        if pick < 0.45:
            e = gen_expr(ctx, width, 3)
            if not fits(width, acc_val + e.value):
                continue
            acc_val += e.value
            lines.append(f"    {acc} = {acc} + ({e.src});")
        elif pick < 0.6 and [n for n in ctx.vars if n != acc]:
            name = rng.choice([n for n in ctx.vars if n != acc])
            w, _ = ctx.vars[name]
            e = gen_expr(ctx, w, 2)
            ctx.vars[name] = (w, e.value)
            # The identity cast, same as at declaration: a computed
            # expression keeps its own (default-signed) type and only a
            # literal would adapt to the destination (§13).
            lines.append(f"    {name} = ({e.src}) as {w};")
        elif pick < 0.75:
            cond = gen_bool(ctx, width, 2)
            then_e = gen_expr(ctx, width, 2)
            else_e = gen_expr(ctx, width, 2)
            chosen = then_e.value if cond.value != 0 else else_e.value
            if not fits(width, acc_val + chosen):
                continue
            acc_val += chosen
            lines.append(
                f"    if ({cond.src}) {{ {acc} = {acc} + ({then_e.src}); }} "
                f"else {{ {acc} = {acc} + ({else_e.src}); }}")
        else:
            # Bounded counted loop: the counter is fresh, the trip count
            # is a literal, so termination is by shape. The body must not
            # name the accumulator — it is re-evaluated every trip while
            # the oracle holds one value.
            ctr = ctx.fresh()
            n = rng.randint(1, 4)
            ctx.banned.add(acc)
            body = gen_expr(ctx, width, 2)
            ctx.banned.discard(acc)
            total, ok = 0, True
            for _ in range(n):
                if not fits(width, total + body.value):
                    ok = False
                    break
                total += body.value
            if not ok or not fits(width, acc_val + total):
                continue
            acc_val += total
            lines.append(
                f"    for (var {ctr}: {width} = 0; {ctr} < {n}; "
                f"{ctr} = {ctr} + 1) {{ {acc} = {acc} + ({body.src}); }}")
    lines.append(f"    return {acc} as int;")
    return "func main() -> int {\n" + "\n".join(lines) + "\n}\n", acc_val & 0xFF


def run_one(path):
    try:
        r = subprocess.run([CIDER, "run", path], capture_output=True,
                           timeout=120, env=ENV)
    except subprocess.TimeoutExpired:
        return "TIMEOUT", None, ""
    if r.returncode < 0:
        return "SIGNAL", r.returncode, (r.stdout + r.stderr).decode(errors="replace")
    return "ok", r.returncode, (r.stdout + r.stderr).decode(errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--keep", default=None)
    args = ap.parse_args()
    fails = 0
    for i in range(args.count):
        rng = random.Random(args.seed + i)
        try:
            src, want = gen_program(rng)
        except Exception as e:  # generator bug: loud, never silent
            print(f"[{i}] GENERATOR-BUG: {e}")
            fails += 1
            continue
        path = f"<tmp>/fuzz_{args.seed}_{i}.cid"
        with open(path, "w") as f:
            f.write(with_main_entry(src))
        status, rc, out = run_one(path)
        bad = (status != "ok") or (rc != want)
        if bad:
            if status != "ok":
                print(f"[{i}] {status} rc={rc}\nwant exit {want}\n{src}\n{out}")
            else:
                print(f"[{i}] MISMATCH want={want} got={rc}\n{src}\n{out}")
            fails += 1
            if args.keep:
                os.makedirs(args.keep, exist_ok=True)
                with open(os.path.join(args.keep,
                                       f"case_{args.seed}_{i}.cid"), "w") as f:
                    f.write(f"// want exit {want}, {status} rc={rc}\n{src}")
        try:
            os.unlink(path)
        except OSError:
            pass
    print(f"fuzz: {args.count - fails}/{args.count} pass, {fails} failures (seed={args.seed})")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
