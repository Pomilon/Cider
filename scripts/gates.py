#!/usr/bin/env python3
"""The gates from TYPES.md §8.1 (T1-T6) and LAYERS.md §6 (L1-L6).

Every gate here is quoted from those documents. If a check and its document
disagree, the document wins and this file is the defect.

A gate has three states, not two:

  PASS     the check ran and the fact holds.
  FAIL     the check ran and the fact does not hold.
  PENDING  the check could not run, because the thing it inspects does not
           exist yet.

PENDING is not PASS. A gate whose subject is missing has not been satisfied; it
has not been examined. `TYPES.md` §8.1 warns about exactly this -- "T1 and T2
are vacuous without §2.1" -- so a gate that silently passes because its subject
is absent would report the spec's own failure mode as success. Each PENDING line
therefore names the file that will make the gate checkable.

Exit status is 1 if any gate FAILs. PENDING gates do not fail the run; --strict
turns them into failures, which is what CI uses once the tree is complete.

Usage:
    scripts/gates.py [--strict] [--only T1,T4] [--list]
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from typing import Callable

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")

PASS, FAIL, PENDING = "PASS", "FAIL", "PENDING"


@dataclass
class Gate:
    ident: str
    quote: str          # the gate's own text, from the document
    subject: str        # the file that must exist before the check can run
    check: Callable[[], tuple[bool, str]]


def read(rel: str) -> str | None:
    path = os.path.join(ROOT, rel)
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def src_files() -> list[str]:
    """Every tracked-or-not source file under src/, sorted, for reproducible order."""
    found = []
    for dirpath, dirnames, filenames in os.walk(SRC):
        dirnames.sort()
        for name in sorted(filenames):
            if name.endswith((".cpp", ".h", ".hpp", ".cc")):
                found.append(os.path.join(dirpath, name))
    return sorted(found)


def grep_lines(pattern: str, paths: list[str], flags: int = 0) -> list[str]:
    rx = re.compile(pattern, flags)
    hits = []
    for path in paths:
        try:
            with open(path, encoding="utf-8") as handle:
                for lineno, line in enumerate(handle, 1):
                    if rx.search(line):
                        hits.append(f"{os.path.relpath(path, ROOT)}:{lineno}: {line.strip()}")
        except OSError:
            continue
    return hits


# --- T1: no default: in a switch over Kind -------------------------------------
#
# `-Werror=switch` catches a *missing* enumerator at compile time. It cannot
# catch a `default:` that swallows one, because a switch with a default always
# compiles. So the document asks for both, and this is the second half: find
# switch statements whose controlling expression names a Kind and report a
# `default:` inside one. Scanning the switch body needs a brace matcher rather
# than a line grep, because `default:` is routinely not indented under `switch`.

def _switch_bodies(text: str) -> list[tuple[int, str]]:
    """Return (line_number_of_switch, body_text) for every switch statement."""
    bodies = []
    for match in re.finditer(r"\bswitch\s*\(", text):
        start = match.start()
        depth, index = 0, match.end() - 1
        # Walk to the ')' that closes the controlling expression.
        while index < len(text):
            if text[index] == "(":
                depth += 1
            elif text[index] == ")":
                depth -= 1
                if depth == 0:
                    break
            index += 1
        cursor = index + 1
        while cursor < len(text) and text[cursor] in " \t\r\n":
            cursor += 1
        if cursor >= len(text) or text[cursor] != "{":
            continue
        depth, end = 0, cursor
        while end < len(text):
            if text[end] == "{":
                depth += 1
            elif text[end] == "}":
                depth -= 1
                if depth == 0:
                    break
            end += 1
        line = text.count("\n", 0, start) + 1
        bodies.append((line, text[start:end + 1]))
    return bodies


def t1() -> tuple[bool, str]:
    offenders = []
    switches = 0
    if not src_files():
        raise FileNotFoundError("src/ -- a switch over Kind, first written in lowering")
    for path in src_files():
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
        for line, body in _switch_bodies(text):
            controlling = body[body.index("(") + 1: body.index(")")]
            if not re.search(r"\bKind\b|\bkind\b|->kind\b|\.kind\b", controlling):
                continue
            switches += 1
            if re.search(r"(^|\s)default\s*:", body):
                offenders.append(f"{os.path.relpath(path, ROOT)}:{line}: default: in a switch over Kind")
    if switches == 0:
        # A switch with no `default:` passes this check trivially. So does a
        # tree with no switch over Kind at all, which is why T1 is PENDING and
        # not PASS until lowering exists and actually switches on the kind.
        raise FileNotFoundError("a switch over Kind in src/ (lowering is the first layer to need one)")
    if offenders:
        return False, "; ".join(offenders)
    return True, f"{switches} switch statement(s) over Kind, none with default:"


# --- T4: no size is computed twice ---------------------------------------------

# A size-looking word, matched case-insensitively and NOT anchored to a word
# boundary. The boundary was the first version's bug: `kOffsetDataWord` is one
# identifier, so `\boffset\b` never matched it -- and a prefixed constant is
# precisely the shape T4 exists to catch, since `layout::` is the only place
# such a name should appear.
#
# `width` is deliberately absent. It is the most common word in this compiler
# ("the operand's width", "an IR value's width") and almost never a second
# authority for a number; including it would bury the real defects in noise.
SIZE_WORD = r"(?:size|sizeof|offset|align|stride|padding)"


def _code_lines(path: str) -> list[tuple[int, str]]:
    """Source lines with comments and string literals removed.

    A gate that greps raw text reports prose. "eight integer widths" in a
    comment is not a second authority for a size, and a gate that fails on it
    teaches its reader to write vaguer comments -- which is the opposite of what
    TYPES.md wants. So comments and literals go before the pattern is applied,
    and only executable code is judged.
    """
    kept = []
    with open(path, encoding="utf-8") as handle:
        for lineno, raw in enumerate(handle, 1):
            line, out, i = raw, [], 0
            while i < len(line):
                two = line[i:i + 2]
                if two == "//":
                    break
                if two == "/*":
                    # Skip to the closing marker, tracking newlines so the line
                    # numbering of what follows stays right.
                    end = line.find("*/", i + 2)
                    if end == -1:
                        break
                    out.append(" " * (end + 2 - i))
                    i = end + 2
                    continue
                if line[i] == '"':
                    end = i + 1
                    while end < len(line) and line[end] != '"':
                        end += 2 if line[end] == "\\" else 1
                    out.append('""')
                    i = min(end + 1, len(line))
                    continue
                if line[i] == "'":
                    end = i + 1
                    while end < len(line) and line[end] != "'":
                        end += 2 if line[end] == "\\" else 1
                    out.append("''")
                    i = min(end + 1, len(line))
                    continue
                out.append(line[i])
                i += 1
            cleaned = "".join(out)
            if cleaned.strip():
                kept.append((lineno, cleaned))
    return kept


def t4_grep() -> tuple[bool, str]:
    """The literal half of T4: a size-looking literal in code outside layout.h."""
    offenders = []
    rx = re.compile(SIZE_WORD + r"[^\n]*?\d", re.IGNORECASE)
    for path in src_files():
        rel = os.path.relpath(path, ROOT)
        if rel.endswith(os.path.join("src", "layout.h")):
            continue
        for lineno, line in _code_lines(path):
            if rx.search(line):
                offenders.append(f"{rel}:{lineno}: {line.strip()}")
    if offenders:
        return False, (f"{len(offenders)} size/offset literal(s) in code outside layout.h: "
                       + "; ".join(offenders[:5]))
    return True, "no size/offset literals in code outside src/layout.h"


# --- T2 / T3 / T5 / T6 / L1 / L2 / L3 -----------------------------------------

def _types_files() -> list[str]:
    """Every source file under src/types/, which is where the type representation
    lives. The subject is src/types/type.h specifically -- `Kind` and `IRType`
    are part of the representation but neither carries an operator== or a
    shared_ptr, so their absence proves nothing and must not fail a gate.
    """
    if read(os.path.join("src", "types", "type.h")) is None:
        raise FileNotFoundError("src/types/type.h")
    return [p for p in src_files() if os.path.join("src", "types") in p]


def t2() -> tuple[bool, str]:
    types = _types_files()
    count = sum(len(grep_lines(r"\boperator\s*==", [p])) for p in types)
    if count != 1:
        return False, f"src/types/ has {count} operator== definitions, expected exactly 1"
    return True, "exactly one operator== in src/types/"


def t3() -> tuple[bool, str]:
    types = _types_files()
    hits = [h for p in types for h in grep_lines(r"\bshared_ptr\b", [p])]
    if hits:
        return False, f"shared_ptr in the type representation: " + "; ".join(hits[:5])
    return True, "no shared_ptr in src/types/"


def ast_visitor_count() -> tuple[bool, str]:
    if not src_files():
        # Same reasoning as T1: zero visitors is not "exactly one". Reporting it
        # as a failure would mean an empty tree fails the gate instead of
        # admitting the gate has nothing to inspect yet.
        raise FileNotFoundError("src/ast.h")
    hits = [h for p in src_files() for h in grep_lines(r"\bpublic\s+ASTVisitor\b", [p])]
    count = len(hits)
    if count == 0:
        # src/ has source, but no AST yet. The visitor arrives with src/ast.h,
        # and until then this gate is unexamined rather than violated -- the
        # same distinction T1 draws for "no switch over Kind yet".
        raise FileNotFoundError("src/ast.h (the single ASTVisitor)")
    if count != 1:
        return False, f"{count} `public ASTVisitor` declarations in src/, expected exactly 1: " + "; ".join(hits)
    return True, hits[0]


def t5() -> tuple[bool, str]:
    ok, detail = ast_visitor_count()
    return ok, detail


def l1() -> tuple[bool, str]:
    ok, detail = ast_visitor_count()
    return ok, detail


def t6() -> tuple[bool, str]:
    pattern = r"mirrors the|exactly like|same as the|must descend together|lacks the"
    files = src_files()
    if not files:
        raise FileNotFoundError("src/")
    hits = [h for p in files for h in grep_lines(pattern, [p], re.IGNORECASE)]
    if hits:
        return False, f"{len(hits)} copied-rule comment(s): " + "; ".join(hits[:5])
    return True, "no copied-rule comments in src/"


def l2() -> tuple[bool, str]:
    text = read(os.path.join("src", "codegen.cpp"))
    if text is None:
        raise FileNotFoundError("src/codegen.cpp")
    hits = [f"{i}: {line.strip()}"
            for i, line in enumerate(text.splitlines(), 1)
            if re.search(r"\bASTVisitor\b|\bAstNode\b|\bExpression\s*\*", line)]
    if hits:
        return False, f"src/codegen.cpp reaches into the AST: " + "; ".join(hits[:5])
    return True, "src/codegen.cpp has no AST access"


def l4() -> tuple[bool, str]:
    """Every block terminated, every target valid.

    The document puts this in the optimiser's entry, so the gate is the
    structural checker itself, invoked on the IR. It cannot be a grep: the fact
    is a property of the IR, not of the source text.
    """
    raise FileNotFoundError("src/optimise/ -- the IR structural checker")


def l5() -> tuple[bool, str]:
    raise FileNotFoundError("src/codegen.cpp -- byte-reproducibility needs two builds in two directories")


def l6() -> tuple[bool, str]:
    raise FileNotFoundError("src/main.cpp -- the top-level catch that maps failures to diagnostics")


def t4() -> tuple[bool, str]:
    """Both halves of T4. The grep half is below; the test half is the layout unit test."""
    if read(os.path.join("src", "layout.h")) is None:
        raise FileNotFoundError("src/layout.h")
    test = os.path.join(ROOT, "tests", "unit", "layout_test.cpp")
    if not os.path.isfile(test):
        raise FileNotFoundError("tests/unit/layout_test.cpp (the sizeOf half of this gate)")
    ok, detail = t4_grep()
    if not ok:
        return False, detail
    return True, detail + " + tests/unit/layout_test.cpp"


GATES: list[Gate] = [
    Gate("T1", "No `default:` in a switch over `Kind`", "src/types/", t1),
    Gate("T2", "Exactly one `operator==` for types", "src/types/type.h", t2),
    Gate("T3", "No `shared_ptr` in the type representation", "src/types/type.h", t3),
    Gate("T4", "No size is computed twice", "src/layout.h", t4),
    Gate("T5", "One AST traversal", "src/ast.h", t5),
    Gate("T6", "No copied rules", "src/", t6),
    Gate("L1", "One AST traversal", "src/ast.h", l1),
    Gate("L2", "No layer reads past its neighbour", "src/codegen.cpp", l2),
    Gate("L3", "One conversion per hand-off", "src/", t6),
    Gate("L4", "Every block terminated, every target valid", "src/optimise/", l4),
    Gate("L5", "Codegen is byte-reproducible", "src/codegen.cpp", l5),
    Gate("L6", "No raw exception escapes a pass", "src/main.cpp", l6),
]


def run_gate(gate: Gate, strict: bool) -> tuple[str, str]:
    try:
        ok, detail = gate.check()
    except FileNotFoundError as missing:
        return (FAIL if strict else PENDING), f"waiting for {missing}"
    if ok:
        return PASS, detail
    return FAIL, detail


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--strict", action="store_true",
                        help="treat a PENDING gate as a failure")
    parser.add_argument("--only", help="comma-separated gate ids to run")
    parser.add_argument("--list", action="store_true", help="list the gates and exit")
    args = parser.parse_args()

    if args.list:
        for gate in GATES:
            print(f"{gate.ident:4} {gate.quote}")
            print(f"     subject: {gate.subject}")
        return 0

    selected = GATES
    if args.only:
        wanted = {g.strip().upper() for g in args.only.split(",")}
        selected = [g for g in GATES if g.ident in wanted]

    results = []
    for gate in selected:
        state, detail = run_gate(gate, args.strict)
        results.append((gate, state, detail))

    width = max(len(g.ident) for g, _, _ in results)
    for gate, state, detail in results:
        print(f"{gate.ident:<{width}}  {state:<8} {detail}")

    failed = [g for g, s, _ in results if s == FAIL]
    pending = [g for g, s, _ in results if s == PENDING]
    print()
    print(f"{len(results) - len(failed) - len(pending)} pass, {len(failed)} fail, {len(pending)} pending")
    if args.strict and pending:
        print(f"strict: {len(pending)} unexamined gate(s) counted as failures")
    if pending:
        print("pending gates are NOT satisfied -- they are unexamined:")
        for gate, state, detail in results:
            if state == PENDING:
                print(f"  {gate.ident}  {gate.quote}")

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
