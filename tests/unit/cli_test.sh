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
echo "$USAGE" | grep -q -- "--target native|linux|x86_64-linux|pomeos|uefi" || { echo "usage missing --target"; exit 1; }
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

# CIDER-ISS-006: `linux` / `x86_64-linux` are the backend's names for the
# native ELF output (the CodeGenerator defaults to "x86_64-linux"). Both must
# take the identical native path; an unknown target must keep failing.
printf 'func main() -> int { return 7; }\n' > "$SRC"
TARGET_TMP=$(mktemp -d)
trap 'rm -f "$SRC"; rm -rf "$TARGET_TMP"' EXIT
set +e
"$BIN" run "$SRC" >/dev/null 2>&1
NATIVE_RC=$?
set -e
[ "$NATIVE_RC" -eq 7 ] || { echo "native run exit: $NATIVE_RC"; exit 1; }
"$BIN" compile "$SRC" -o "$TARGET_TMP/native.asm"
for T in linux x86_64-linux; do
    set +e
    "$BIN" run --target "$T" "$SRC" >/dev/null 2>&1
    TARGET_RC=$?
    set -e
    [ "$TARGET_RC" -eq "$NATIVE_RC" ] || { echo "$T run exit: $TARGET_RC"; exit 1; }
    "$BIN" compile --target "$T" "$SRC" -o "$TARGET_TMP/$T.asm"
    cmp -s "$TARGET_TMP/native.asm" "$TARGET_TMP/$T.asm" \
        || { echo "$T assembly differs from native"; exit 1; }
done
set +e
"$BIN" compile --target bogus "$SRC" >/dev/null 2>&1
BOGUS_RC=$?
set -e
[ "$BOGUS_RC" -eq 2 ] || { echo "bogus target accepted: $BOGUS_RC"; exit 1; }
echo "cli_test: target aliases OK"
