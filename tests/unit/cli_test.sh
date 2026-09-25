#!/bin/bash
set -euo pipefail
BIN=${1:?usage: cli_test.sh <cider-binary>}
[ "$("$BIN" --version)" = "cider 0.2.0" ]

set +e
"$BIN" >/dev/null 2>&1; [ $? -eq 2 ] || { echo "no-args: got $?"; exit 1; }
"$BIN" run >/dev/null 2>&1; [ $? -eq 2 ] || { echo "run-no-file: got $?"; exit 1; }
"$BIN" bogus file.ir >/dev/null 2>&1; [ $? -eq 2 ] || { echo "bogus-cmd: got $?"; exit 1; }
"$BIN" compile --target bogus /dev/null >/dev/null 2>&1; [ $? -eq 2 ] || { echo "bogus-target: got $?"; exit 1; }
"$BIN" compile --target >/dev/null 2>&1; [ $? -eq 2 ] || { echo "target-no-arg: got $?"; exit 1; }
USAGE=$("$BIN" 2>&1 || true)
echo "$USAGE" | grep -q -- "--target native|pomeos|uefi" || { echo "usage missing --target"; exit 1; }
set -e
echo "cli_test: OK"

SRC=$(mktemp --suffix=.cid)
trap 'rm -f "$SRC"' EXIT
printf 'func main() -> void { }\n' > "$SRC"
"$BIN" check "$SRC" || { echo "check clean failed"; exit 1; }
printf 'func main() -> void { var x: int = "s"; }\n' > "$SRC"
set +e
"$BIN" check "$SRC" >/dev/null 2>&1
RC=$?
set -e
[ "$RC" -eq 1 ] || { echo "check error-case failed: $RC"; exit 1; }
