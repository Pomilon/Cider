#!/bin/bash
# CIDER-BUG-169: the asm exit syscall (std.io.exit / an inline `syscall` 60)
# must flush putchar's buffer exactly like the exit builtin, and in the same
# order (flush first, then drop the abandoned frames). Pre-fix execAsm only
# retired the frames, so a program mixing extern putchar with an asm exit lost
# every byte still buffered on both VM paths.
#
# The VM models libc's buffered putchar and flushes it at program exit. The
# native reference is linked against glibc, whose buffer a raw exit syscall
# does NOT flush when stdout is a pipe, so the mixed programs here pin the two
# VM paths against each other. The exit-builtin control (libc exit DOES flush)
# pins the full VM/native matrix for the shared buffering semantics.
#
# Usage: vm_asm_exit_flush_smoke.sh <cider-binary>
# Exits 77 when nasm/gcc are missing (the native control needs them).
set -euo pipefail

BIN=${1:?usage: vm_asm_exit_flush_smoke.sh <cider-binary>}
for tool in nasm gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "vm_asm_exit_flush_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "vm_asm_exit_flush_smoke: FAIL ($1)"
    exit 1
}

# stdout lands in $DIR/out, the exit code in VM_RC.
run_vm() { # <flag|-> <src>
    local flag=$1 src=$2
    set +e
    if [ "$flag" = "-" ]; then
        "$BIN" run "$src" > "$DIR/out" 2> "$DIR/err"
    else
        "$BIN" run "$flag" "$src" > "$DIR/out" 2> "$DIR/err"
    fi
    VM_RC=$?
    set -e
}

# Both VM paths must agree with <expected-stdout> (printf escapes, e.g. "a\n").
check_vm() { # <name> <src> <expected-stdout> <expected-exit>
    local name=$1 src=$2 out=$3 rc=$4 flag
    printf '%b' "$out" > "$DIR/want"
    for flag in "-" "--no-opt"; do
        run_vm "$flag" "$src"
        [ "$VM_RC" -eq "$rc" ] || fail "$name ($flag): exit $VM_RC != $rc"
        cmp -s "$DIR/out" "$DIR/want" ||
            fail "$name ($flag): stdout [$(cat "$DIR/out")] != [$out]"
    done
}

# VM paths plus the native reference (mirrors tests/run_tests.py's linker
# choice: -nostartfiles when the object defines its own _start).
check_all() { # <name> <src> <expected-stdout> <expected-exit>
    check_vm "$@"
    local name=$1 src=$2 out=$3 rc=$4
    "$BIN" compile "$src" -o "$DIR/prog.asm"
    nasm -O0 -f elf64 "$DIR/prog.asm" -o "$DIR/prog.o"
    if nm "$DIR/prog.o" 2>/dev/null | grep -q " _start$"; then
        gcc -nostartfiles "$DIR/prog.o" -o "$DIR/prog" -no-pie
    else
        gcc "$DIR/prog.o" -o "$DIR/prog" -no-pie
    fi
    set +e
    "$DIR/prog" > "$DIR/native_out" 2> "$DIR/native_err"
    NATIVE_RC=$?
    set -e
    [ "$NATIVE_RC" -eq "$rc" ] || fail "$name (native): exit $NATIVE_RC != $rc"
    printf '%b' "$out" > "$DIR/want"
    cmp -s "$DIR/native_out" "$DIR/want" ||
        fail "$name (native): stdout [$(cat "$DIR/native_out")] != [$out]"
}

# extern putchar, then std.io.exit (the module function lowering to the asm
# exit syscall). Pre-fix the VM dropped "HI\n".
cat > "$DIR/mixed_std_exit.cid" <<'CID'
extern func putchar(c: int) -> int;
from std.io import exit;

func main() -> int {
    putchar(72);
    putchar(73);
    putchar(10);
    exit(0);
    return 1;
}
CID
check_vm "mixed_std_exit" "$DIR/mixed_std_exit.cid" 'HI\n' 0

# The same mix with a hand-written exit syscall and a non-zero code: the
# flush must not disturb the exit code.
cat > "$DIR/mixed_asm_exit.cid" <<'CID'
extern func putchar(c: int) -> int;

func main() -> int {
    putchar(65);
    putchar(66);
    putchar(10);
    asm { "mov rax, 60" "mov rdi, 7" "syscall" }
    return 1;
}
CID
check_vm "mixed_asm_exit" "$DIR/mixed_asm_exit.cid" 'AB\n' 7

# Buffering order: the raw write goes out immediately, the putchar byte sits
# in the buffer until the asm exit flushes it -- the same result the exit
# builtin produces ("BA"), not the interleaved "AB".
cat > "$DIR/hold_order.cid" <<'CID'
extern func putchar(c: int) -> int;
var msg: string = "B";

func main() -> int {
    putchar(65);
    asm { "mov rax, 1" "mov rdi, 1" "mov rsi, {msg}" "mov rdx, 1" "syscall" }
    asm { "mov rax, 60" "mov rdi, 0" "syscall" }
    return 1;
}
CID
check_vm "hold_order" "$DIR/hold_order.cid" 'BA' 0

# Control: the libc-backed exit builtin flushes natively too, so this pins
# the full VM/native matrix and the ordering shared with the asm exit.
cat > "$DIR/exit_builtin.cid" <<'CID'
extern func putchar(c: int) -> int;
extern func exit(code: int) -> void;
var msg: string = "B";

func main() -> int {
    putchar(65);
    asm { "mov rax, 1" "mov rdi, 1" "mov rsi, {msg}" "mov rdx, 1" "syscall" }
    exit(0);
    return 1;
}
CID
check_all "exit_builtin" "$DIR/exit_builtin.cid" 'BA' 0

echo "vm_asm_exit_flush_smoke: OK"
