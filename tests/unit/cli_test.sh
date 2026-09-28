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
echo "$USAGE" | grep -q -- "--warn" || { echo "usage missing --warn"; exit 1; }
echo "$USAGE" | grep -q -- "--checks" || { echo "usage missing --checks"; exit 1; }
echo "$USAGE" | grep -q -- "-g|--debug" || { echo "usage missing -g/--debug"; exit 1; }
echo "$USAGE" | grep -q -- "--cache <dir>" || { echo "usage missing --cache"; exit 1; }
echo "$USAGE" | grep -q -- "--emit-meta" || { echo "usage missing --emit-meta"; exit 1; }
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

# CIDER-FEAT-005: warnings are opt-in. Without --warn the output stays
# byte-identical/empty; with --warn the warning goes to stderr and the exit
# code stays 0.
printf 'func main() -> void { var unused: int = 1; }\n' > "$SRC"
DEFAULT_OUT=$("$BIN" check "$SRC" 2>&1)
[ -z "$DEFAULT_OUT" ] || { echo "default check not silent: $DEFAULT_OUT"; exit 1; }
set +e
WARN_OUT=$("$BIN" check --warn "$SRC" 2>&1)
WARN_RC=$?
set -e
[ "$WARN_RC" -eq 0 ] || { echo "warn-check exit: $WARN_RC"; exit 1; }
echo "$WARN_OUT" | grep -q "warning: unused variable 'unused'" \
    || { echo "missing unused-variable warning: $WARN_OUT"; exit 1; }

# --warn must not disturb build/run: stdout stays exact and the exit code
# unchanged.
RUN_OUT=$("$BIN" run --warn "$SRC" 2>/dev/null)
[ -z "$RUN_OUT" ] || { echo "warn-run stdout not empty: $RUN_OUT"; exit 1; }

# An unreferenced from-import is reported too.
printf 'from std.io import print;\nfunc main() -> void { }\n' > "$SRC"
set +e
IMPORT_OUT=$("$BIN" check --warn "$SRC" 2>&1)
IMPORT_RC=$?
set -e
[ "$IMPORT_RC" -eq 0 ] || { echo "warn-import exit: $IMPORT_RC"; exit 1; }
echo "$IMPORT_OUT" | grep -q "warning: unused import 'print' from module 'std.io'" \
    || { echo "missing unused-import warning: $IMPORT_OUT"; exit 1; }

# CIDER-ISS-002: a local read before any assignment warns at the read; the
# default path stays silent and --warn keeps exit code 0.
printf 'func main() -> void { var x: int; var y: int = x; }\n' > "$SRC"
DEFAULT_OUT=$("$BIN" check "$SRC" 2>&1)
[ -z "$DEFAULT_OUT" ] || { echo "default uninit check not silent: $DEFAULT_OUT"; exit 1; }
set +e
UNINIT_OUT=$("$BIN" check --warn "$SRC" 2>&1)
UNINIT_RC=$?
set -e
[ "$UNINIT_RC" -eq 0 ] || { echo "warn-uninit exit: $UNINIT_RC"; exit 1; }
echo "$UNINIT_OUT" | grep -q "warning: local 'x' may be read before being assigned" \
    || { echo "missing read-before-assignment warning: $UNINIT_OUT"; exit 1; }
