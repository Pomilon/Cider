#!/usr/bin/env python3
"""Fast trap bodies must be out of line, not inline at the guard."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import backend_shape

# Fast codegen is native-only during bring-up and the native target lets a
# null dereference fault, so `emitNullCheck` has no reachable CLI target. A
# diagnosed lowering trap (the runtime slice-index guard) is the reachable
# fast trap: its syscall/message/ud2 body must move out of line exactly like
# a null check's cold ud2. The fast null-check stub uses the same machinery;
# it is unreachable from the CLI until a null-checking target selects fast.
PROGRAM = """
func pick(n: i64) -> i64 {
    var a: [i64; 3] = [5, 6, 7];
    var s: []i64 = a[0:3];
    return s[n];
}
func _start() -> void {
    asm { "mov rax, 60" "mov edi, 0" "syscall" }
}
"""

def main():
    text = backend_shape.compile_asm(
        PROGRAM, ("--codegen=fast", "--no-opt"))
    body = backend_shape.function_body(text, "pick")
    if body is None:
        print("FAIL: pick body missing", file=sys.stderr)
        return 1
    if not re.search(r"jmp \.L_cider_trap_\d+", body):
        print("FAIL: trap reason does not jump to a cold stub",
              file=sys.stderr)
        return 1
    if re.search(r"jnz \.L_cider_trap_\d+", body):
        print("FAIL: trap branches to the cold stub on the wrong condition",
              file=sys.stderr)
        return 1
    labels = list(re.finditer(r"(?m)^(\.L_cider_trap_\d+):$", body))
    if not labels:
        print("FAIL: cold trap stub is missing", file=sys.stderr)
        return 1
    hot = body[:labels[0].start()]
    if "syscall" in hot:
        print("FAIL: trap body is still inline in the hot path",
              file=sys.stderr)
        return 1
    if re.search(r"(?m)^  ud2$", hot):
        print("FAIL: inline ud2 remains in the hot path", file=sys.stderr)
        return 1
    for index, label in enumerate(labels):
        end = (labels[index + 1].start()
               if index + 1 < len(labels) else len(body))
        stub = body[label.start():end]
        if not re.search(r"(?m)^  ud2$", stub):
            print("FAIL: cold trap stub does not contain ud2", file=sys.stderr)
            return 1
        if "syscall" not in stub:
            print("FAIL: diagnosed cold trap stub has no syscall body",
                  file=sys.stderr)
            return 1
    if not re.search(r"(?m)^  ret\n\.L_cider_trap_\d+:", body):
        print("FAIL: cold trap stub is not after the last block",
              file=sys.stderr)
        return 1
    if not re.search(r"\.L_cider_trap_msg_\d+:", body):
        print("FAIL: trap message bytes are missing from the cold stub",
              file=sys.stderr)
        return 1
    # Fast is native-only during bring-up, so Pomeos still falls back to the
    # legacy emitter. Its inline `jnz` null check must remain untouched; this
    # is the target policy that leaves the fast null-check stub unreachable
    # from the CLI until a null-checking target selects the fast emitter.
    pomeos = backend_shape.compile_asm(
        "func deref(p: *i64) -> i64 { return *p; }\n"
        "func _start() -> void {\n"
        "    asm { \"mov rax, 60\" \"mov edi, 0\" \"syscall\" }\n"
        "}\n",
        ("--target=pomeos", "--codegen=fast", "--no-opt"))
    if ".L_cider_trap_" in pomeos:
        print("FAIL: Pomeos fast no longer falls back to legacy",
              file=sys.stderr)
        return 1
    if not re.search(r"jnz \.L_pomeos_null_\d+", pomeos):
        print("FAIL: legacy Pomeos null check changed", file=sys.stderr)
        return 1
    print("fast traps: cold stubs are out of line")
    return 0

if __name__ == "__main__":
    sys.exit(main())
