#!/bin/bash
# CIDER-BUG-132: .bss globals of heterogeneous alignment used to be padded
# with NASM's `align` macro, which fills with `nop` (initialized data) and
# made NASM warn "attempt to initialize memory in BSS section" once per
# padded global. Codegen now pads .bss with `alignb` (fill via `resb 1`).
# This smoke asserts (a) nasm assembles such a module warning-free and
# (b) the padding keeps the exact same symbol offsets in .bss.
#
# CIDER-BUG-142: a top-level user `asm {}` block used to be printed in the
# .bss context too, so a label + `align`/`dq` there warned "attempt to
# initialize memory in BSS section" and its code bytes never made it into a
# live section. The second half asserts the block now lands in .text and
# that its code really executes.
#
# Usage: bss_align_smoke.sh <cider-binary>
# Exits 77 when nasm/nm/gcc are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: bss_align_smoke.sh <cider-binary>}
for tool in nasm nm gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "bss_align_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "bss_align_smoke: FAIL ($1)"
    exit 1
}

# Same shape as tests/programs/bss_align_mixed: u8 -> u32 (align 4 padding)
# -> u64 -> u8 -> u64 (align 8 padding), so both padding widths are covered.
cat > "$DIR/bss.cid" <<'CID'
var g8: u8 = 0;
var g32: u32 = 0;
var g64: u64 = 0;
var tail: u8 = 0;
var g64b: u64 = 0;

func main() -> int {
  g8 = 0xF0;
  g32 = 0x12345678;
  g64 = 0x1122334455667788;
  tail = 0x5A;
  g64b = 0x8877665544332211;
  return 0;
}
CID

"$BIN" compile "$DIR/bss.cid" -o "$DIR/bss.asm" \
    || fail "cider compile failed"

NASM_ERR=$(nasm -O0 -f elf64 "$DIR/bss.asm" -o "$DIR/bss.o" 2>&1 >/dev/null) \
    || fail "nasm failed: $NASM_ERR"
if echo "$NASM_ERR" | grep -q "attempt to initialize memory"; then
    fail "nasm warned about initialized data in .bss: $NASM_ERR"
fi

# Offsets pin the padding: 3 bytes of align-4 after g8, 7 bytes of align-8
# after tail. Any change in padding direction/size shifts these.
check_sym() { # <name> <expected-hex-offset>
    local got
    got=$(nm "$DIR/bss.o" | awk -v s="$1" '$3 == s { print $1 }')
    [ "$got" = "$2" ] || fail "$1 at 0x${got:-?}, expected 0x$2"
}

check_sym g8   0000000000000000
check_sym g32  0000000000000004
check_sym g64  0000000000000008
check_sym tail 0000000000000010
check_sym g64b 0000000000000018

# CIDER-BUG-142: top-level user asm is file-scope code/data. It must be
# emitted in .text (not .bss), must not warn, and its label must be callable:
# main returns the value user_fn puts in eax, so a mis-sectioned or ignored
# block cannot produce exit 42.
cat > "$DIR/user_asm.cid" <<'CID'
extern func user_fn() -> int;

func main() -> int {
  return user_fn();
}

asm {
  "user_fn:"
  "mov eax, 42"
  "ret"
}

asm {
  "align 16"
  "user_data: dq 0x1122334455667788"
}
CID

"$BIN" compile "$DIR/user_asm.cid" -o "$DIR/user_asm.asm" \
    || fail "cider compile (user asm) failed"

NASM_ERR=$(nasm -O0 -f elf64 "$DIR/user_asm.asm" -o "$DIR/user_asm.o" 2>&1 >/dev/null) \
    || fail "nasm failed (user asm): $NASM_ERR"
if echo "$NASM_ERR" | grep -q "attempt to initialize memory"; then
    fail "user asm block emitted initialized data in .bss: $NASM_ERR"
fi

# nm's type letter: lowercase = local. `t`/`T` is .text; a `b` here means the
# block slid back into .bss, `d` means .data.
check_text_sym() { # <name>
    local got
    got=$(nm "$DIR/user_asm.o" | awk -v s="$1" '$3 == s { print $2 }')
    case "$got" in
        t|T) ;;
        *) fail "user asm symbol $1 in section type '${got:-missing}', want .text (t/T)" ;;
    esac
}
check_text_sym user_fn
check_text_sym user_data

gcc "$DIR/user_asm.o" -o "$DIR/user_asm" -no-pie || fail "link (user asm) failed"
set +e
"$DIR/user_asm"
USER_ASM_RC=$?
set -e
[ "$USER_ASM_RC" -eq 42 ] || fail "user asm body exited $USER_ASM_RC, want 42"

echo "bss_align_smoke: OK"
