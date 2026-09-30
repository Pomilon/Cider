#!/bin/bash
# CIDER-BUG-258: every path that PRODUCES OUTPUT must report a failed write --
# nonzero exit and a diagnostic naming the target -- instead of exiting 0 over
# an empty, truncated or garbage file.
#
# Before this the driver checked the OPEN of a file and nothing else. An
# ofstream reports a failed WRITE on its own state (failbit/badbit) and never
# throws, so an ENOSPC/EIO/EFBIG reached the destructor and was discarded:
#
#   cider compile -o /dev/full                            exit 0
#   cider bundle -o /dev/full                             exit 0
#   cider build f.cid >/dev/full                          exit 0
#   cider parse f.cid >/dev/full                          exit 0
#   cider bundle f.cid >/dev/full                         exit 0
#   CIDER_DEBUG_IR=1 cider compile ... >/dev/full          exit 0
#   cider run printer.cid >/dev/full                      exit 0 (the bytes were lost)
#   cider compile --cache <d> --emit-meta (write fails)    exit 0
#   cider compile --target pomeos -o /dev/full            exit 137 -- the OOM killer:
#       the failed first pass was read back from /dev/full, which reads as an
#       endless stream of NULs, into an unbounded std::string
#
# What is pinned here:
#
#   1. the WRITE failure on every output path (exit 1, exact message bytes,
#      clean stdout). /dev/full is the canonical case: the open succeeds and
#      the write deterministically fails with ENOSPC -- no privileges, no full
#      disk, no assembler.
#   2. the OPEN failure that already worked, so the fix cannot have been
#      bought by breaking it (read-only directory, missing directory).
#   3. the partial-file POLICY on a regular file: reported, and left in place.
#   4. the inverse: a successful write still exits 0, stderr stays empty and
#      stdout stays byte-exact.
#
# Partial-file policy (CIDER-BUG-258): a partially written output is left where
# it is and reported, never unlinked. The open already truncated the target, so
# its previous contents are gone either way; and `-o` may name a device, a FIFO
# or a socket, where unlinking is destructive rather than tidy (it would take
# /dev/null with it). gcc and clang keep the partial output too. The guarantee
# this driver makes is that the failure is LOUD: the file is not trustworthy,
# and the message says so.
#
# Usage: output_write_fail_smoke.sh <cider-binary>
set -uo pipefail

BIN=${1:?usage: output_write_fail_smoke.sh <cider-binary>}

# SKIP (77) where /dev/full does not exist (non-Linux hosts): there is no
# deterministic write failure available and the test would pass vacuously.
if [ ! -c /dev/full ]; then
    echo "output_write_fail_smoke: SKIP (no /dev/full character device)"
    exit 77
fi

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fails=0
fail() {
    echo "output_write_fail_smoke: FAIL ($1)"
    fails=$((fails + 1))
}

size_of() { wc -c < "$1" 2>/dev/null | tr -d ' ' || echo 0; }

# A tiny valid program, one that prints, one that writes to stderr with the raw
# syscall, and a module-mode source for the cache snapshot.
cat > "$DIR/tiny.cid" <<'CID'
func add(a: int, b: int) -> int { return a + b; }

func main() -> int { return add(20, 22); }
CID
cat > "$DIR/printer.cid" <<'CID'
extern func putchar(c: int) -> int;

func main() -> int {
  putchar(72);
  putchar(105);
  return 42;
}
CID
# 4 bytes to fd 2, so `run` hands the driver real stderrText (putchar only
# reaches stdout).
cat > "$DIR/writerr.cid" <<'CID'
var msg: string = "boom\n";

func main() -> int {
  var s: char* = msg as char*;
  asm {
    "mov rax, 1"
    "mov rdi, 2"
    "mov rsi, {s}"
    "mov rdx, 5"
    "syscall"
  }
  return 0;
}
CID
cat > "$DIR/mod.cid" <<'CID'
func add(a: int, b: int) -> int { return a + b; }
CID

# expect_fail <label> <expected-stderr> <args...>
# Runs with stdout on /dev/full: the artifact goes there, and nothing may leak
# to the real stdout -- every assertion below is about stderr and the exit code.
expect_fail() {
    local label=$1 want=$2
    shift 2
    local rc got
    timeout 300 "$BIN" "$@" >/dev/full 2>"$DIR/err"; rc=$?
    got=$(cat "$DIR/err")
    if [ "$rc" -ne 1 ]; then
        fail "$label: exit $rc, want 1 (stderr: [$got])"
    fi
    if [ "$got" != "$want" ]; then
        fail "$label: stderr [$got], want [$want]"
    fi
}

# expect_open_fail <label> <expected-stderr> <args...>
# Same, for the open failure that already worked.
expect_open_fail() {
    local label=$1 want=$2
    shift 2
    local rc got
    timeout 300 "$BIN" "$@" >/dev/null 2>"$DIR/err"; rc=$?
    got=$(cat "$DIR/err")
    if [ "$rc" -ne 1 ]; then
        fail "$label: exit $rc, want 1 (stderr: [$got])"
    fi
    if [ "$got" != "$want" ]; then
        fail "$label: stderr [$got], want [$want]"
    fi
}

# ---------------------------------------------------------------- part 1 ----
# The WRITE failure on every output path.
echo "output_write_fail_smoke: write failures (/dev/full)"

# `compile -o`: the codegen ofstream, in every shape that reaches it.
expect_fail "compile -o"           "cannot write /dev/full" compile "$DIR/tiny.cid" -o /dev/full
expect_fail "compile -o -g"        "cannot write /dev/full" compile -g "$DIR/tiny.cid" -o /dev/full
expect_fail "compile -o --no-opt"  "cannot write /dev/full" compile --no-opt "$DIR/tiny.cid" -o /dev/full
expect_fail "compile -o pomeos"    "cannot write /dev/full" compile --target pomeos "$DIR/tiny.cid" -o /dev/full
expect_fail "compile -o pomeos -g" "cannot write /dev/full" compile -g --target pomeos "$DIR/tiny.cid" -o /dev/full
# `bundle -o`: the driver's own ofstream.
expect_fail "bundle -o"            "cannot write /dev/full" bundle "$DIR/tiny.cid" -o /dev/full
# The cache snapshot: the shared writeFileOrThrow, whose message carries the
# input file because it is reported through the driver's generic handler. A
# symlink to /dev/full is the write-failure version of it (the open follows the
# link and succeeds, the write gets ENOSPC); `--cache /dev/full` in part 2 is
# the open-failure version.
mkdir -p "$DIR/cache"
ln -sf /dev/full "$DIR/cache/mod.cider-meta"
expect_fail "emit-meta cache"      "$DIR/mod.cid: cannot write $DIR/cache/mod.cider-meta" \
    compile --cache "$DIR/cache" --emit-meta "$DIR/mod.cid" -o "$DIR/mod.asm"
# No-unlink again, this time on a path that is not a regular file: the driver
# must leave the user's target exactly as it found it.
if [ ! -L "$DIR/cache/mod.cider-meta" ]; then
    fail "emit-meta cache: the failed target was removed or replaced"
fi
# The hash is written only after the snapshot, so a failed snapshot means the
# cache is not left with a snapshot/hash pair that disagrees.
if [ -e "$DIR/cache/mod.hash" ]; then
    fail "emit-meta cache: wrote mod.hash after the snapshot failed"
fi
rm -f "$DIR/cache/mod.cider-meta"

# The stdout artifacts: parse, build, bundle-without--o, CIDER_DEBUG_IR.
expect_fail "parse stdout"         "cannot write <stdout>" parse "$DIR/tiny.cid"
expect_fail "build stdout"         "cannot write <stdout>" build "$DIR/tiny.cid"
expect_fail "bundle stdout"        "cannot write <stdout>" bundle "$DIR/tiny.cid"
CIDER_DEBUG_IR=1 timeout 300 "$BIN" compile "$DIR/tiny.cid" -o "$DIR/ok.asm" \
    >/dev/full 2>"$DIR/err"; rc=$?
got=$(cat "$DIR/err")
if [ "$rc" -ne 1 ] || [ "$got" != "cannot write <stdout>" ]; then
    fail "compile DEBUG_IR stdout: exit $rc, stderr [$got], want 1 / [cannot write <stdout>]"
fi

# `run`: the program's own output. A program that printed nothing has no bytes
# to lose (part 4); one that printed must be reported, and the driver's own
# write failure (exit 1) takes the place of the program's exit code.
timeout 300 "$BIN" run "$DIR/printer.cid" >/dev/full 2>"$DIR/err"; rc=$?
got=$(cat "$DIR/err")
if [ "$rc" -ne 1 ] || [ "$got" != "cannot write <stdout>" ]; then
    fail "run stdout: exit $rc, stderr [$got], want 1 / [cannot write <stdout>]"
fi
# `run`'s stderr copy. The diagnostic names <stderr> and is written to that
# same stream, so a stream that rejects writes swallows the message with the
# bytes it describes: what is pinned is that the failure is reported AT ALL
# (exit 1, not the program's own 0) and that nothing leaks to stdout. The
# message bytes for this path come from the same helper as the rows above.
timeout 300 "$BIN" run "$DIR/writerr.cid" >"$DIR/out" 2>/dev/full; rc=$?
if [ "$rc" -ne 1 ] || [ -s "$DIR/out" ]; then
    fail "run stderr: exit $rc, stdout [$(cat "$DIR/out")], want 1 / nothing on stdout"
fi

# ---------------------------------------------------------------- part 2 ----
# The OPEN failure that already worked, pinned so the write check cannot have
# been bought by breaking it -- plus the guarantee that nothing is created on
# the way out.
echo "output_write_fail_smoke: open failures (still reported)"

mkdir -p "$DIR/ro"
chmod 555 "$DIR/ro"

expect_open_fail "compile -o read-only dir" "cannot write $DIR/ro/x.asm" \
    compile "$DIR/tiny.cid" -o "$DIR/ro/x.asm"
expect_open_fail "bundle -o read-only dir"  "cannot write $DIR/ro/b.cid" \
    bundle "$DIR/tiny.cid" -o "$DIR/ro/b.cid"
expect_open_fail "compile -o missing dir"   "cannot write $DIR/nodir/x.asm" \
    compile "$DIR/tiny.cid" -o "$DIR/nodir/x.asm"
expect_open_fail "bundle -o missing dir"    "cannot write $DIR/nodir/b.cid" \
    bundle "$DIR/tiny.cid" -o "$DIR/nodir/b.cid"
expect_open_fail "emit-meta read-only cache" "$DIR/mod.cid: cannot write $DIR/ro/mod.cider-meta" \
    compile --cache "$DIR/ro" --emit-meta "$DIR/mod.cid" -o "$DIR/mod.asm"
# A cache dir that is not a directory at all (ENOTDIR on the snapshot path).
expect_open_fail "emit-meta cache not a dir" "$DIR/mod.cid: cannot write /dev/full/mod.cider-meta" \
    compile --cache /dev/full --emit-meta "$DIR/mod.cid" -o "$DIR/mod.asm"

for P in "$DIR/ro/x.asm" "$DIR/nodir/x.asm" "$DIR/nodir/b.cid" "$DIR/ro/mod.cider-meta"; do
    if [ -e "$P" ]; then
        fail "open failure left $P behind"
    fi
done

# ---------------------------------------------------------------- part 3 ----
# A partial write to a REGULAR file: the policy on the case a user actually
# hits (a full disk mid-emit). RLIMIT_FSIZE makes write(2) fail with EFBIG
# after the open succeeded; SIGXFSZ is ignored so the process reaches the
# driver instead of dying from the signal first (that death is the same silent
# failure from another angle, and is why the trap is here).
echo "output_write_fail_smoke: partial output is reported and left in place"

run_limited() { # <limit-in-blocks> <args...>
    local limit=$1
    shift
    ( trap '' XFSZ; ulimit -f "$limit"; timeout 300 "$@" )
}

# The same cap with the child's stderr on a PIPE: under `ulimit -f 0` not one
# byte may be written to a regular file, which includes the diagnostic the
# driver is about to print (a pipe is not subject to RLIMIT_FSIZE). This is
# the purest write failure there is -- the open succeeds and no byte is
# allowed -- so the diagnostic comes back through the substitution.
run_limited_captured() { # <args...>
    ( trap '' XFSZ; ulimit -f 0; timeout 300 "$@" ) 2>&1
}

# Control: uncapped, so the cap below is known to truncate a real output.
timeout 300 "$BIN" compile "$DIR/tiny.cid" -o "$DIR/full.asm" 2>"$DIR/err"; rc=$?
FULL_SIZE=$(size_of "$DIR/full.asm")
if [ "$rc" -ne 0 ] || [ "$FULL_SIZE" -le 0 ] || [ -s "$DIR/err" ]; then
    fail "partial-file control: compile exit $rc, size $FULL_SIZE, stderr [$(cat "$DIR/err")]"
fi

# The truncated write: nonzero exit, the target named, and the partial file
# still there (NOT removed -- that is the policy).
printf 'STALE-CONTENT\n' > "$DIR/partial.asm"
run_limited 1 "$BIN" compile "$DIR/tiny.cid" -o "$DIR/partial.asm" 2>"$DIR/err"; rc=$?
got=$(cat "$DIR/err")
if [ "$rc" -ne 1 ]; then
    fail "compile partial: exit $rc, want 1 (stderr: [$got])"
fi
if [ "$got" != "cannot write $DIR/partial.asm" ]; then
    fail "compile partial: stderr [$got], want [cannot write $DIR/partial.asm]"
fi
if [ ! -f "$DIR/partial.asm" ]; then
    fail "compile partial: the partial output was removed (policy: leave it in place)"
fi
PART_SIZE=$(size_of "$DIR/partial.asm")
if [ "$PART_SIZE" -le 0 ] || [ "$PART_SIZE" -ge "$FULL_SIZE" ]; then
    fail "compile partial: file is $PART_SIZE bytes of a $FULL_SIZE-byte output; expected a truncated one"
fi

# Zero allowed bytes: the open still succeeds, so this is a pure write failure
# on a path that did not exist before -- reported the same way.
rm -f "$DIR/zero.asm"
got=$(run_limited_captured "$BIN" compile "$DIR/tiny.cid" -o "$DIR/zero.asm"); rc=$?
if [ "$rc" -ne 1 ] || [ "$got" != "cannot write $DIR/zero.asm" ]; then
    fail "compile size-capped: exit $rc, output [$got], want 1 / [cannot write $DIR/zero.asm]"
fi
if [ ! -f "$DIR/zero.asm" ]; then
    fail "compile size-capped: the partial output was removed (policy: leave it in place)"
fi

got=$(run_limited_captured "$BIN" bundle "$DIR/tiny.cid" -o "$DIR/zero.cid"); rc=$?
if [ "$rc" -ne 1 ] || [ "$got" != "cannot write $DIR/zero.cid" ]; then
    fail "bundle size-capped: exit $rc, output [$got], want 1 / [cannot write $DIR/zero.cid]"
fi

mkdir -p "$DIR/cache2"
got=$(run_limited_captured "$BIN" compile --cache "$DIR/cache2" --emit-meta "$DIR/mod.cid" \
    -o "$DIR/mod.asm"); rc=$?
if [ "$rc" -ne 1 ] || [ "$got" != "$DIR/mod.cid: cannot write $DIR/cache2/mod.cider-meta" ]; then
    fail "emit-meta size-capped: exit $rc, output [$got], want 1 / [cannot write $DIR/cache2/mod.cider-meta]"
fi
if [ -e "$DIR/cache2/mod.hash" ]; then
    fail "emit-meta size-capped: wrote mod.hash after the snapshot failed"
fi

# ---------------------------------------------------------------- part 4 ----
# The inverse: every successful write still exits 0, with clean stderr and
# byte-exact stdout. A fix that reported phantom failures would break here.
echo "output_write_fail_smoke: successful writes still succeed"

expect_ok() { # <label> <args...>
    local label=$1
    shift
    local rc
    timeout 300 "$BIN" "$@" >"$DIR/out" 2>"$DIR/err"; rc=$?
    if [ "$rc" -ne 0 ]; then
        fail "$label: exit $rc, want 0 (stderr: [$(cat "$DIR/err")])"
    fi
    if [ -s "$DIR/err" ]; then
        fail "$label: wrote to stderr: [$(cat "$DIR/err")]"
    fi
}

expect_ok "compile -o"  compile "$DIR/tiny.cid" -o "$DIR/ok.asm"
[ -s "$DIR/ok.asm" ] || fail "compile -o: no output written"
expect_ok "compile -g"  compile -g "$DIR/tiny.cid" -o "$DIR/ok-g.asm"
expect_ok "compile pomeos" compile --target pomeos "$DIR/tiny.cid" -o "$DIR/ok-pomeos.asm"
# The PomeOS rewrite (the second, formerly unchecked ofstream) must still run:
# the runtime block only the rewrite adds has to be in the file.
grep -q "global _setjmp" "$DIR/ok-pomeos.asm" \
    || fail "compile pomeos: the PomeOS rewrite did not run"
expect_ok "bundle -o"   bundle "$DIR/tiny.cid" -o "$DIR/ok.cid"
[ -s "$DIR/ok.cid" ] || fail "bundle -o: no output written"
expect_ok "emit-meta"   compile --cache "$DIR/cache" --emit-meta "$DIR/mod.cid" -o "$DIR/mod.asm"
[ -s "$DIR/cache/mod.cider-meta" ] || fail "emit-meta: no snapshot written"
[ -s "$DIR/cache/mod.hash" ] || fail "emit-meta: no hash written"

# The stdout artifacts, byte for byte: bundle to a pipe must match the -o form.
"$BIN" bundle "$DIR/tiny.cid" >"$DIR/out" 2>"$DIR/err"; rc=$?
if [ "$rc" -ne 0 ] || [ -s "$DIR/err" ]; then
    fail "bundle stdout: exit $rc, stderr [$(cat "$DIR/err")]"
fi
cmp -s "$DIR/out" "$DIR/ok.cid" || fail "bundle stdout: bytes differ from the -o form"

"$BIN" build "$DIR/tiny.cid" >"$DIR/out" 2>"$DIR/err"; rc=$?
if [ "$rc" -ne 0 ] || [ -s "$DIR/err" ]; then
    fail "build stdout: exit $rc, stderr [$(cat "$DIR/err")]"
fi
case "$(cat "$DIR/out")" in
    "define "*) : ;;
    *) fail "build stdout: output does not look like an IR dump" ;;
esac

"$BIN" parse "$DIR/tiny.cid" >"$DIR/out" 2>"$DIR/err"; rc=$?
if [ "$rc" -ne 0 ] || [ -s "$DIR/err" ] || [ ! -s "$DIR/out" ]; then
    fail "parse stdout: exit $rc, stderr [$(cat "$DIR/err")], empty stdout"
fi

"$BIN" run "$DIR/printer.cid" >"$DIR/out" 2>"$DIR/err"; rc=$?
if [ "$rc" -ne 42 ] || [ -s "$DIR/err" ]; then
    fail "run stdout: exit $rc, want the program's own 42, stderr [$(cat "$DIR/err")]"
fi
if [ "$(cat "$DIR/out")" != "Hi" ]; then
    fail "run stdout: got [$(cat "$DIR/out")], want [Hi]"
fi

# A program that prints NOTHING still forwards its own exit code with an
# unwritable stdout: there were no bytes to lose, so there is nothing to report.
timeout 300 "$BIN" run "$DIR/tiny.cid" >/dev/full 2>"$DIR/err"; rc=$?
if [ "$rc" -ne 42 ] || [ -s "$DIR/err" ]; then
    fail "run silent stdout: exit $rc, stderr [$(cat "$DIR/err")], want 42 / no diagnostic"
fi

CIDER_DEBUG_IR=1 timeout 300 "$BIN" compile "$DIR/tiny.cid" -o "$DIR/ok.asm" \
    >"$DIR/out" 2>"$DIR/err"; rc=$?
if [ "$rc" -ne 0 ] || [ ! -s "$DIR/out" ] || [ -s "$DIR/err" ]; then
    fail "compile DEBUG_IR stdout: exit $rc, stderr [$(cat "$DIR/err")]"
fi

if [ "$fails" -ne 0 ]; then
    echo "output_write_fail_smoke: FAIL ($fails failing case(s))"
    exit 1
fi

echo "output_write_fail_smoke: OK"