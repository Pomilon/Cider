#!/bin/bash
# CIDER-BUG-239: an inline-asm placeholder whose value the register
# allocator SPILLED must still be a fully defined operand, or the block
# writes nothing.
#
# `{name}` is register syntax -- the block names a register and `{name:q}`
# re-sizes that register -- but the allocator hands out only rbx/r12-r15, so
# past five simultaneously live values an operand lands in an 8-byte spill
# slot holding only the value's own 4 bytes. The substitution then produced
# `mov rdx, qword [rbp-N]`, whose upper four bytes are inherited stack, so the
# write syscall got a garbage byte count, returned EFAULT, and the program
# printed NOTHING -- silently, with a correct exit code, and only on the
# native backend (the VM keeps full int64 registers, so it printed).
#
# Note what is and is not deterministic here. The wrong value is stale stack,
# so a pre-fix binary misbehaves only where the process inherited non-zero
# bytes at that slot's depth -- which is why both corpus programs push a wide
# local below the frame and make a call first, and why the report's
# fat-pointer dispatch (itself a call) is what showed the bug up at all. The
# staging makes the value deterministic; these two things are pinned here:
#
#   1. the corpus programs print their message, byte for byte, on all three
#      backends (tests/programs/asm_spilled_operand{,_dispatch});
#   2. the emitted code stages every spilled placeholder -- a load at the
#      value's own width into a slot whose upper bytes are then written, and
#      the block naming THAT slot -- so the corpus programs keep covering the
#      path they exist for even where stale stack happens to read as zero.
#
# Usage: asm_spilled_operand_smoke.sh <cider-binary>
# Exits 77 when nasm/gcc are missing (the native path needs them).
set -euo pipefail

BIN=${1:?usage: asm_spilled_operand_smoke.sh <cider-binary>}
for tool in nasm gcc nm; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "asm_spilled_operand_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

REPO=$(cd "$(dirname "$0")/../.." && pwd)
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "asm_spilled_operand_smoke: FAIL ($1)"
    exit 1
}

# All three backends, the same way tests/run_tests.py runs them.
check_all() { # <name> <src> <expected-stdout> <expected-exit>
    local name=$1 src=$2 out=$3 rc=$4 flag
    printf '%b' "$out" > "$DIR/want"
    for flag in "--no-opt" "-"; do
        set +e
        if [ "$flag" = "-" ]; then
            "$BIN" run "$src" > "$DIR/out" 2> "$DIR/err"
        else
            "$BIN" run "$flag" "$src" > "$DIR/out" 2> "$DIR/err"
        fi
        local vmrc=$?
        set -e
        [ "$vmrc" -eq "$rc" ] || fail "$name (vm $flag): exit $vmrc != $rc"
        cmp -s "$DIR/out" "$DIR/want" ||
            fail "$name (vm $flag): stdout [$(cat "$DIR/out")] != [$out]"
    done

    "$BIN" compile "$src" -o "$DIR/prog.asm"
    nasm -O0 -f elf64 "$DIR/prog.asm" -o "$DIR/prog.o"
    if nm "$DIR/prog.o" 2>/dev/null | grep -q " _start$"; then
        gcc -nostartfiles "$DIR/prog.o" -o "$DIR/prog" -no-pie
    else
        gcc "$DIR/prog.o" -o "$DIR/prog" -no-pie
    fi
    set +e
    "$DIR/prog" > "$DIR/native_out" 2> "$DIR/native_err"
    local natrc=$?
    set -e
    [ "$natrc" -eq "$rc" ] || fail "$name (native): exit $natrc != $rc"
    cmp -s "$DIR/native_out" "$DIR/want" ||
        fail "$name (native): stdout [$(cat "$DIR/native_out")] != [$out]"
    cp "$DIR/prog.asm" "$DIR/$name.asm"
}

# The minimal root-cause case, and the reported shape (a callee dispatching
# through a fat pointer received as a parameter, with a three-method
# interface). Both print their message twice and exit 0.
check_all minimal "$REPO/tests/programs/asm_spilled_operand/main.cid" \
    'asm_spilled_operand\nasm_spilled_operand\n' 0
check_all dispatch "$REPO/tests/programs/asm_spilled_operand_dispatch/main.cid" \
    'asm_spilled_dispatch_ok\nasm_spilled_dispatch_ok\n' 0

# The staging invariant. A narrow placeholder is staged with
#   mov r10d, dword [rbp-N]     (the value at its OWN width -- the 32-bit
#   mov qword [rbp-M], r10      write is what defines the upper half)
# and the block names M, not N. The precise statement of the fix is that every
# slot the block READS is one the compiler filled 8 clean bytes into, so:
#   * a narrow placeholder is staged at all, and
#   * the slot the write syscall's length reads is a staged one.
# Without the staging the block names N -- a slot with four undefined bytes,
# which is the whole bug.
for name in minimal dispatch; do
    asmfile="$DIR/$name.asm"
    grep -qE '^ +mov r10d, dword \[rbp - [0-9]+\]$' "$asmfile" ||
        fail "$name: no narrow placeholder staged (mov r10d, dword [rbp - N])"
    # Every 64-bit slot the blocks read must be one the compiler wrote in
    # full: `mov rdx, qword [rbp-M]` needs a `mov qword [rbp-M], r10`.
    for off in $(sed -nE 's/^ +mov r(dx|si), qword \[rbp - ([0-9]+)\]$/\2/p' "$asmfile" |
                 sort -u); do
        grep -qE "^ +mov qword \[rbp - $off\], r10$" "$asmfile" ||
            fail "$name: block reads [rbp - $off], which was never staged"
    done
done

echo "asm_spilled_operand_smoke: OK"
