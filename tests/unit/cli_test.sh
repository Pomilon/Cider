#!/bin/bash
set -euo pipefail
BIN=${1:?usage: cli_test.sh <cider-binary>}
[ "$("$BIN" --version)" = "cider 0.2.0" ]

set +e
"$BIN" >/dev/null 2>&1; [ $? -eq 2 ] || { echo "no-args: got $?"; exit 1; }
"$BIN" run >/dev/null 2>&1; [ $? -eq 2 ] || { echo "run-no-file: got $?"; exit 1; }
"$BIN" bogus file.ir >/dev/null 2>&1; [ $? -eq 2 ] || { echo "bogus-cmd: got $?"; exit 1; }
set -e
echo "cli_test: OK"
