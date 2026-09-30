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

# CIDER-FEAT-013: `--help` (also `-h` and the `help` subcommand) prints the
# long help on stdout, keeps stderr empty and exits 0.
HELP=""
for FORM in --help -h help; do
    set +e
    OUT=$("$BIN" "$FORM" 2>/dev/null)
    HELP_RC=$?
    ERR=$("$BIN" "$FORM" 2>&1 >/dev/null)
    set -e
    [ "$HELP_RC" -eq 0 ] || { echo "help ($FORM) exit: $HELP_RC"; exit 1; }
    # The long help is a successful, expected answer: stdout, never stderr.
    [ -z "$ERR" ] || { echo "help ($FORM) wrote to stderr: $ERR"; exit 1; }
    [ -n "$OUT" ] || { echo "help ($FORM) printed nothing"; exit 1; }
    # The three spellings are aliases: identical output.
    [ -z "$HELP" ] || [ "$OUT" = "$HELP" ] \
        || { echo "help ($FORM) differs from --help"; exit 1; }
    HELP="$OUT"
done

# The help must cover the whole real surface: every command the CLI accepts.
for CMD in parse check build run compile bundle; do
    echo "$HELP" | grep -q "  $CMD " || { echo "help missing command '$CMD'"; exit 1; }
done
# ... every flag it accepts, including the `--target` spellings.
for FLAG in --no-opt --warn --checks --debug --cache --emit-meta -o; do
    echo "$HELP" | grep -q -- "$FLAG" || { echo "help missing flag '$FLAG'"; exit 1; }
done
echo "$HELP" | grep -q -- "native|linux|x86_64-linux|pomeos|uefi" \
    || { echo "help missing --target spellings"; exit 1; }
echo "$HELP" | grep -q -- "-g, --debug" || { echo "help missing -g spelling"; exit 1; }
# The constraints the parser really enforces must be stated, not just the
# flag names: `run` rejects both separate-compilation flags, `--emit-meta`
# needs `--cache`, `compile` needs `-o`, and `-o` is optional for bundle.
echo "$HELP" | grep -q "rejected by parse, bundle, run" \
    || { echo "help does not say which commands reject --emit-meta"; exit 1; }
echo "$HELP" | grep -q "requires --cache" \
    || { echo "help does not say --emit-meta requires --cache"; exit 1; }
echo "$HELP" | grep -q "required by compile, optional for bundle" \
    || { echo "help does not document the -o convention"; exit 1; }
# The separate-compilation artifacts the flag really writes.
echo "$HELP" | grep -q "<module>.cider-meta" || { echo "help missing .cider-meta"; exit 1; }
echo "$HELP" | grep -q "<module>.hash" || { echo "help missing .hash"; exit 1; }

# CIDER-BUG-129: the help's exit-code table must name the allocation-failure
# convention, so the documented contract and the real one cannot drift. The
# exit code is asserted for real in compiler_oom_smoke.sh; here it is only the
# documentation that is pinned.
echo "$HELP" | grep -q "compiler: out of memory" \
    || { echo "help does not document the compiler OOM diagnostic"; exit 1; }
echo "$HELP" | grep -q "vm error: out of" \
    || { echo "help does not document the VM OOM diagnostic"; exit 1; }
echo "$HELP" | grep -q "memory" || { echo "help does not mention memory"; exit 1; }

# CIDER-BUG-258: the write-failure convention and the partial-file policy are
# part of the driver's contract, so the help must state both: every failed
# write is reported as `cannot write <target>` with exit 1, and a partially
# written output is left in place rather than removed. The behaviour itself is
# asserted for real in output_write_fail_smoke.sh; here the documentation is
# pinned so the two cannot drift.
echo "$HELP" | grep -q "cannot write <target>" \
    || { echo "help does not document the write-failure diagnostic"; exit 1; }
echo "$HELP" | grep -q "partially written output is left in place" \
    || { echo "help does not document the partial-output policy"; exit 1; }
echo "$HELP" | grep -q "never unlinks the target" \
    || { echo "help does not document that the driver never unlinks -o"; exit 1; }

# Help must not dispatch a subcommand: the token wins whatever follows it, and
# no file is read (a runnable path would have failed, never exited 0).
set +e
"$BIN" help /nonexistent/never-compiled.cid >/dev/null 2>&1
HELP_NOSUB_RC=$?
"$BIN" --help run /nonexistent/never-compiled.cid >/dev/null 2>&1
HELP_NOSUB2_RC=$?
set -e
[ "$HELP_NOSUB_RC" -eq 0 ] || { echo "help ran a subcommand: $HELP_NOSUB_RC"; exit 1; }
[ "$HELP_NOSUB2_RC" -eq 0 ] || { echo "--help ran a subcommand: $HELP_NOSUB2_RC"; exit 1; }

# The help token is only recognised as the first argument, so the unknown-flag
# usage error is unchanged for every other position.
set +e
"$BIN" --help-bogus >/dev/null 2>&1
HELPBOGUS_RC=$?
"$BIN" run --help /nonexistent/never-compiled.cid >/dev/null 2>&1
RUNHELP_RC=$?
set -e
[ "$HELPBOGUS_RC" -eq 2 ] || { echo "--help-bogus exit: $HELPBOGUS_RC"; exit 1; }
[ "$RUNHELP_RC" -eq 2 ] || { echo "run --help exit: $RUNHELP_RC"; exit 1; }
echo "cli_test: help OK"

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
