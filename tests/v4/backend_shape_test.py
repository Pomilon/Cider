#!/usr/bin/env python3
"""Smoke-test the shared asm helpers used by the Stage 1.5 shape tests."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

PROGRAM = """
func add(a: i64, b: i64) -> i64 {
    return a + b;
}
func _start() -> void {
    asm { "mov rax, 60" "mov edi, 3" "syscall" }
}
"""

def main():
    text = backend_shape.compile_asm(PROGRAM, ("--codegen=fast", "--no-opt"))
    body = backend_shape.function_body(text, "add")
    if body is None:
        print("FAIL: add body missing", file=sys.stderr)
        return 1
    if backend_shape.count(r"\b(add|lea)\b", body) == 0:
        print("FAIL: add body has no add/lea", file=sys.stderr)
        return 1
    print("backend shape helper: ok")
    return 0

if __name__ == "__main__":
    sys.exit(main())
