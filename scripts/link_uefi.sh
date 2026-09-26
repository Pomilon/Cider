#!/usr/bin/env bash
# CID-005: thin NASM + GNU ld wrapper for PE32+ UEFI images.
#   scripts/link_uefi.sh <input.asm> -o <out.efi> [-T <linker-script>] [-- <extra ld args>]
# Requires: nasm (win64 output) and GNU ld with i386pep support.
set -euo pipefail

usage() {
  echo "usage: link_uefi.sh <input.asm> -o <out.efi> [-T <linker-script>] [-- <extra ld args>]" >&2
  exit 2
}

[ $# -ge 1 ] || usage
INPUT="$1"; shift
OUT=""
SCRIPT=""
EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    -o) [ $# -ge 2 ] || usage; OUT="$2"; shift 2 ;;
    -T) [ $# -ge 2 ] || usage; SCRIPT="$2"; shift 2 ;;
    --) shift; EXTRA=("$@"); break ;;
    *) usage ;;
  esac
done
[ -n "$OUT" ] || usage
[ -f "$INPUT" ] || { echo "link_uefi.sh: no such file: $INPUT" >&2; exit 2; }

TMPDIR_LINK="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_LINK"' EXIT

nasm -O0 -f win64 "$INPUT" -o "$TMPDIR_LINK/obj.o"

LD_ARGS=(-m i386pep -shared -Bsymbolic -nostdlib --subsystem 10 --image-base 0 -e efi_main)
[ -n "$SCRIPT" ] && LD_ARGS+=(-T "$SCRIPT")
if [ ${#EXTRA[@]} -gt 0 ]; then
  ld "${LD_ARGS[@]}" -o "$OUT" "$TMPDIR_LINK/obj.o" "${EXTRA[@]}"
else
  ld "${LD_ARGS[@]}" -o "$OUT" "$TMPDIR_LINK/obj.o"
fi
echo "linked $OUT"
