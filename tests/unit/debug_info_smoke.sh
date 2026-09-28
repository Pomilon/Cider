#!/bin/bash
# CIDER-FEAT-001: `cider compile -g` must emit DWARF line tables and
# function DIEs for the .cid source. The smoke compiles a small program with
# -g, assembles it with plain `nasm -f elf64` (no -g needed on NASM: the
# sections are in the source), links with gcc, and asks readelf/objdump for
# the decoded line table. It deliberately does not exercise the default
# (no -g) path.
#
# Exit 77 = skip (missing toolchain), matching the other smoke tests.
set -u

BIN=${1:?usage: debug_info_smoke.sh <cider-binary>}
skip() { echo "debug_info_smoke: SKIP: $1"; exit 77; }

command -v nasm >/dev/null 2>&1 || skip "nasm not found"
command -v gcc >/dev/null 2>&1 || skip "gcc not found"

if command -v readelf >/dev/null 2>&1; then
    dump_lines() { readelf --debug-dump=decodedline "$1"; }
elif command -v objdump >/dev/null 2>&1; then
    dump_lines() { objdump --dwarf=decodedline "$1"; }
else
    skip "readelf/objdump not found"
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SRC="$TMP/debug_smoke.cid"
ASM="$TMP/debug_smoke.asm"
OBJ="$TMP/debug_smoke.o"
EXE="$TMP/debug_smoke"

# Line numbers are asserted below: 7 = `var v` in main, 9 = body of the if.
cat > "$SRC" <<'EOF'
func square(x: i32) -> i32 {
    var r: i32 = x * x;
    return r;
}

func main() -> i32 {
    var v: i32 = square(6);
    if (v == 36) {
        v = v - 1;
    }
    return v;
}
EOF

"$BIN" compile -g "$SRC" -o "$ASM" \
    || { echo "debug_info_smoke: compile -g failed"; exit 1; }
grep -q "section .debug_line" "$ASM" \
    || { echo "debug_info_smoke: no .debug_line section emitted"; exit 1; }
nasm -O0 -f elf64 "$ASM" -o "$OBJ" \
    || { echo "debug_info_smoke: nasm failed"; exit 1; }
gcc "$OBJ" -o "$EXE" -no-pie \
    || { echo "debug_info_smoke: link failed"; exit 1; }

"$EXE"
RC=$?
[ "$RC" -eq 35 ] || { echo "debug_info_smoke: program exit $RC != 35"; exit 1; }

LINES=$(dump_lines "$EXE")
echo "$LINES" | grep -q "debug_smoke.cid" \
    || { echo "debug_info_smoke: source filename missing from line table:"; echo "$LINES"; exit 1; }
echo "$LINES" | grep -Eq "debug_smoke\.cid[[:space:]]+7[[:space:]]+0x" \
    || { echo "debug_info_smoke: line 7 missing from line table:"; echo "$LINES"; exit 1; }
echo "$LINES" | grep -Eq "debug_smoke\.cid[[:space:]]+9[[:space:]]+0x" \
    || { echo "debug_info_smoke: line 9 missing from line table:"; echo "$LINES"; exit 1; }

# Function DIEs: names + declaration lines (readelf/objdump both print these).
INFO=$(readelf --debug-dump=info "$EXE" 2>/dev/null || objdump --dwarf=info "$EXE" 2>/dev/null || true)
echo "$INFO" | grep -q "DW_TAG_subprogram" \
    || { echo "debug_info_smoke: no subprogram DIEs"; exit 1; }
echo "$INFO" | grep -q "square" \
    || { echo "debug_info_smoke: function 'square' missing from .debug_info"; exit 1; }

# If gdb is installed, the real consumer must resolve a function to the .cid.
if command -v gdb >/dev/null 2>&1; then
    GDBOUT=$(gdb -batch -ex "info line main" "$EXE" 2>/dev/null || true)
    echo "$GDBOUT" | grep -q "debug_smoke.cid" \
        || { echo "debug_info_smoke: gdb info line main lacks .cid source: $GDBOUT"; exit 1; }
fi

echo "debug_info_smoke: OK"
