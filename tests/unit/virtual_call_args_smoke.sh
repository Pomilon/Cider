#!/bin/bash
# CIDER-BUG-234: VIRTUAL_CALL argument pushes must go through pushArg, exactly
# like every other call site. The emitter pushed `objPtr` and
# `getOperand(args[i])` raw, so a virtual-method argument missed the four
# cases pushArg stages:
#
#   * LABEL: `push twice` assembles but pushes a 32-bit address, truncating
#     any code address above 4 GiB; the fix materialises the full 64 bits
#     (`mov rax, twice` / `push rax`).
#   * xmm: `push xmm15` is not an instruction -- NASM rejects the file
#     outright ("invalid combination of opcode and operands") while the VM
#     runs it fine.
#   * spilled narrow integer: the slot holds only the value's own bytes, so
#     a raw qword push reads stale stack above it (the CIDER-BUG-239 class);
#     the fix sign/zero-extends out of the slot first.
#   * wide immediate: `push 8589934591` warns and truncates to 32 bits, so
#     the callee receives 0xFFFFFFFF instead of 0x1FFFFFFFF.
#
# Note what is and is not deterministic here. The float and wide-immediate
# cases fail on every pre-fix build (a NASM hard error, a truncated value);
# the label case passes by luck in small non-PIE binaries (code addresses
# fit 32 bits); the spilled-narrow value is stale stack, and every integer
# consumer in the callee re-reads at 32 bits, so no wrong value was forced
# out of a probe -- the raw qword push of a dword-written slot is in the
# pre-fix assembly all the same. The staging makes every case defined; these
# two things are pinned here:
#
#   1. the corpus program passes a spilled narrow int, a float, a wide u64
#      immediate and a function through virtual calls and prints its marker
#      on all three backends (tests/programs/virtual_call_args);
#   2. the emitted VIRTUAL_CALL sites stage every special argument -- an
#      xmm spill, a materialised wide immediate, a materialised label, and a
#      widened spilled narrow int -- so the corpus program keeps covering
#      the paths it exists for even if allocation shifts values around.
#
# Usage: virtual_call_args_smoke.sh <cider-binary>
# Exits 77 when nasm/gcc are missing (the native path needs them).
set -euo pipefail

BIN=${1:?usage: virtual_call_args_smoke.sh <cider-binary>}
for tool in nasm gcc nm; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "virtual_call_args_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

REPO=$(cd "$(dirname "$0")/../.." && pwd)
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "virtual_call_args_smoke: FAIL ($1)"
    exit 1
}

SRC="$REPO/tests/programs/virtual_call_args/main.cid"

# All three backends, the same way tests/run_tests.py runs them.
printf 'vcall_args_ok\n' > "$DIR/want"
for flag in "--no-opt" "-"; do
    set +e
    if [ "$flag" = "-" ]; then
        "$BIN" run "$SRC" > "$DIR/out" 2> "$DIR/err"
    else
        "$BIN" run "$flag" "$SRC" > "$DIR/out" 2> "$DIR/err"
    fi
    vmrc=$?
    set -e
    [ "$vmrc" -eq 0 ] || fail "corpus (vm $flag): exit $vmrc != 0"
    cmp -s "$DIR/out" "$DIR/want" ||
        fail "corpus (vm $flag): stdout [$(cat "$DIR/out")] != [vcall_args_ok]"
done

"$BIN" compile "$SRC" -o "$DIR/prog.asm"
nasm -O0 -f elf64 "$DIR/prog.asm" -o "$DIR/prog.o"
if nm "$DIR/prog.o" 2>/dev/null | grep -q " _start$"; then
    gcc -nostartfiles "$DIR/prog.o" -o "$DIR/prog" -no-pie
else
    gcc "$DIR/prog.o" -o "$DIR/prog" -no-pie
fi
set +e
"$DIR/prog" > "$DIR/native_out" 2> "$DIR/native_err"
natrc=$?
set -e
[ "$natrc" -eq 0 ] || fail "corpus (native): exit $natrc != 0"
cmp -s "$DIR/native_out" "$DIR/want" ||
    fail "corpus (native): stdout [$(cat "$DIR/native_out")] != [vcall_args_ok]"

ASM="$DIR/prog.asm"

# `push xmmN` is not an instruction; a VIRTUAL_CALL site that still raw-pushes
# a float argument never assembles (nasm above would already have failed, but
# pin the absence explicitly so the message names the cause).
if grep -qE '^ +push xmm[0-9]+' "$ASM"; then
    fail "raw xmm push at a call site (push xmmN is not encodable)"
fi

# A wide immediate must be materialised in rax first: `push 8589934591`
# warns and truncates to 32 bits.
grep -qE '^ +mov rax, 8589934591$' "$ASM" ||
    fail "wide u64 immediate not materialised (mov rax, 8589934591)"
if grep -qE '^ +push [0-9]{10,}$' "$ASM"; then
    fail "raw wide-immediate push (truncates to 32 bits)"
fi

# A function (label) argument must be materialised in rax first: `push twice`
# pushes a 32-bit address. (Register pushes are fine; only a bare non-register
# symbol is a missed label.)
grep -qE '^ +mov rax, twice$' "$ASM" ||
    fail "label argument not materialised (mov rax, twice)"
if grep -E '^ +push [a-zA-Z_][a-zA-Z0-9_]*$' "$ASM" |
        grep -qvE '^ +push (rax|rbx|rcx|rdx|rsi|rdi|rbp|rsp|r8|r9|r10|r11|r12|r13|r14|r15)$'; then
    fail "raw label push (pushes a 32-bit address)"
fi

# A spilled narrow int argument must be widened out of its slot: the corpus
# program spills `v` (fourteen keeps hold their registers across the calls),
# so the pick site must stage it, not push the dword-written slot whole.
grep -qE '^ +movsxd rax, dword \[rbp - [0-9]+\]$' "$ASM" ||
    fail "spilled narrow argument not staged (movsxd rax, dword [rbp - N])"

echo "virtual_call_args_smoke: OK"
