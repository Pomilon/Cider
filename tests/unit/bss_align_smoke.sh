#!/bin/bash
# CIDER-BUG-132: .bss globals of heterogeneous alignment used to be padded
# with NASM's `align` macro, which fills with `nop` (initialized data) and
# made NASM warn "attempt to initialize memory in BSS section" once per
# padded global. Codegen now pads .bss with `alignb` (fill via `resb 1`).
# This smoke asserts (a) nasm assembles such a module warning-free and
# (b) the padding keeps the exact same symbol offsets in .bss.
#
# Usage: bss_align_smoke.sh <cider-binary>
# Exits 77 when nasm/nm are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: bss_align_smoke.sh <cider-binary>}
for tool in nasm nm; do
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

echo "bss_align_smoke: OK"
