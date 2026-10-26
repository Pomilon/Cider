#!/usr/bin/env python3
"""Source-level entry wrapper for the test harness.

v4 has no compiler startup shim. Native programs define their own `_start`
and UEFI programs define their own `efi_main`. Many fixtures still express
`main` because their subject is the language or a library, not the process
entry spelling. This helper appends a real Cider `_start` that calls `main`
and exits with its value. The compiler sees ordinary source, never a hidden
entry trampoline.
"""

import os
import re
import shutil
import tempfile

MAIN_ENTRY = """
func _start() -> void {
    var code: int = main();
    asm {
        "mov rax, 60"
        "mov edi, {code}"
        "syscall"
    }
}
"""

SOURCE_ENTRY = re.compile(r"(?m)^\s*(?:export\s+)?func\s+(_start|efi_main)\s*\(")
MAIN_FUNCTION = re.compile(r"(?m)^\s*(?:export\s+)?func\s+main\s*\(([^)]*)\)")
PARAMETER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*\s*:")


def with_main_entry(text):
    """Append a source `_start` wrapper when the fixture defines main only."""
    if SOURCE_ENTRY.search(text):
        return text
    main = MAIN_FUNCTION.search(text)
    if main is None:
        return text
    if PARAMETER.search(main.group(1)):
        raise ValueError(
            "fixtures with parameterized main must define their own _start")
    return text.rstrip() + "\n" + MAIN_ENTRY


def wrap_file(path, directory=None):
    """Write wrapped source beside its imports.

    A compiler run resolves plain imports relative to the entry file, so a
    copied entry in /tmp would lose them. Copy the fixture directory to a
    temporary tree and replace the entry only when a wrapper is needed.
    Returns (entry_path, temporary_directory_or_None).
    """
    with open(path, "r") as handle:
        text = handle.read()
    wrapped = with_main_entry(text)
    if wrapped == text:
        return path, None
    target = directory or tempfile.mkdtemp(prefix="cider-entry-")
    if directory is None:
        shutil.copytree(
            os.path.dirname(os.path.abspath(path)), target, dirs_exist_ok=True)
    entry = os.path.join(target, os.path.basename(path))
    with open(entry, "w") as handle:
        handle.write(wrapped)
    return entry, target if directory is None else None
