#!/bin/bash
set -euo pipefail
BIN=${1:?usage: native_smoke.sh <cider-binary>}
PROG=${2:-tests/programs/hello}
ASM=$(mktemp --suffix=.asm)
OBJ=$(mktemp --suffix=.o)
EXE=$(mktemp)
trap 'rm -f "$ASM" "$OBJ" "$EXE"' EXIT

"$BIN" compile "$PROG/main.ir" -o "$ASM"
nasm -O0 -f elf64 "$ASM" -o "$OBJ"
ld "$OBJ" -o "$EXE"
OUT=$("$EXE")
[ "$OUT" = "Hello, world!" ] || { echo "native output mismatch: [$OUT]"; exit 1; }
echo "native_smoke: OK"
