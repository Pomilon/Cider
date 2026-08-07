#!/bin/bash
set -euo pipefail
BIN=${1:?usage: parse_cli_test.sh <cider-binary>}
SRC=$(mktemp --suffix=.cdr)
trap 'rm -f "$SRC"' EXIT
printf 'func f() -> int {\n  return 1 + 2;\n}\n' > "$SRC"
OUT=$("$BIN" parse "$SRC")
echo "$OUT" | grep -q "(FunctionDecl f" || { echo "no FunctionDecl in: $OUT"; exit 1; }
echo "$OUT" | grep -q "(Return (Binary + (Number 1) (Number 2)))" || exit 1
printf 'var x: int = ;\n' > "$SRC"
set +e
"$BIN" parse "$SRC" >/dev/null 2>&1
RC=$?
set -e
[ "$RC" -eq 1 ] || { echo "expected exit 1, got $RC"; exit 1; }
echo "parse_cli_test: OK"
