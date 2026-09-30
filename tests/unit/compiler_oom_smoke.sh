#!/bin/bash
# CIDER-BUG-129: an allocation failure in ANY compiler stage must be reported
# the same way the runtime already reports one — a clean, greppable message
# and the VM's exit code 2 — never as a raw C++ exception name.
#
# Before this, only `VM::run` had a boundary (CIDER-BUG-121: `vm error: out of
# memory`, exit 2). Every other stage escaped into the CLI's generic
# `catch (std::exception&)`, so a compiler-stage allocation failure printed
# `<file>: std::bad_alloc` with exit 1 — a C++ type name where a diagnostic
# belongs, and exit 1, which means "the program is wrong". The `bundle` stage
# was worse: it ran OUTSIDE that try block entirely, so it escaped main and
# std::terminate printed `terminate called after throwing an instance of
# 'std::bad_alloc'` and aborted (SIGABRT / exit 134, core dumped).
#
# Two halves:
#
#   1. FAULT INJECTION (the deterministic half). `CIDER_FAULT_ALLOC=<stage>`
#    makes one real allocation in the named stage fail — a request larger than
#    any address space for std::bad_alloc, a resize past max_size() for
#    std::length_error. Neither touches a page, so this half needs no memory
#    pressure at all and cannot endanger the machine. Every stage is checked,
#    and so is both exception type and the absence of any `std::` noise.
#
#   2. A REAL OOM under `ulimit -v` (the end-to-end half), so the mapping is
#    proven against the actual failure mode and not only against the hook.
#    The generated program needs far more than the 100 MiB cap uncapped, and
#    the cap is >10x the ~10 MiB the binary needs just to load, so it is
#    unambiguously below the working set and unambiguously above the floor.
#    Every run is under `timeout`, and the cap is per-child (`ulimit` in a
#    subshell), never applied globally. The hard RLIMIT_AS is only read, and
#    the test SKIPs (77) rather than failing when it cannot allow the cap.
#
# Usage: compiler_oom_smoke.sh <cider-binary>
set -uo pipefail

BIN=${1:?usage: compiler_oom_smoke.sh <cider-binary>}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

EXPECT_OOM="compiler: out of memory"
OOM_RC=2
LIMIT_KB=102400   # 100 MiB

fails=0
fail() {
    echo "compiler_oom_smoke: FAIL ($1)"
    fails=$((fails + 1))
}

# Assert one invocation reported an allocation failure cleanly: the exact
# diagnostic on stderr, exit 2, and no C++ exception name anywhere.
# Usage: expect_oom <label> <stderr> <rc>
expect_oom() {
    local label=$1 out=$2 rc=$3
    if [ "$rc" -ne "$OOM_RC" ]; then
        fail "$label: exit $rc, want $OOM_RC (stderr: [$out])"
        return
    fi
    if [ "$out" != "$EXPECT_OOM" ]; then
        fail "$label: stderr [$out], want [$EXPECT_OOM]"
        return
    fi
    # The no-C++-noise rule, pinned independently of the exact wording.
    case "$out" in
        *std::*|*bad_alloc*|*bad_array_new_length*|*length_error*|*terminate*|*"what():"*)
            fail "$label: C++ exception noise leaked: [$out]" ;;
    esac
}

# Run the CLI, capturing stderr in OUT and the exit code in RC.
# Usage: run_cider <src> [args...]
run_cider() { # <src> [args...]
    local src=$1
    shift
    OUT=$(timeout 120 "$BIN" "$@" "$src" 2>&1 >/dev/null)
    RC=$?
}

# A tiny valid program: enough to reach every stage, trivial enough that an
# injected failure cannot be confused with a real error.
cat > "$DIR/tiny.cid" <<'CID'
func add(a: int, b: int) -> int { return a + b; }

func main() -> int { return add(20, 22); }
CID

# ---------------------------------------------------------------- part 1 ----
# Every stage, both exception types, through the command that reaches it.
#
# Each row is "<stage> <command...>": the stage marker is only visited by the
# commands that run it, so the row doubles as a check that a stage maps
# identically on every path that reaches it.
echo "compiler_oom_smoke: fault-injected stages"

check_stage() { # <kind-suffix> <label> <stage> <src> [args...]
    local suffix=$1 label=$2 stage=$3 src=$4
    shift 4
    local out rc
    out=$(CIDER_FAULT_ALLOC="$stage$suffix" timeout 120 "$BIN" "$@" "$src" 2>&1 >/dev/null)
    rc=$?
    expect_oom "$label" "$out" "$rc"
}

# Command variants, so the same stage is pinned on more than one path where
# cheap: `compile` adds the deconstructor and codegen, `run` adds the VM.
for SUFFIX in "" ":len"; do
    if [ -z "$SUFFIX" ]; then
        KIND=bad_alloc
    else
        KIND=length_error
    fi
    check_stage "$SUFFIX" "$KIND/bundle"          bundle          "$DIR/tiny.cid" bundle
    check_stage "$SUFFIX" "$KIND/read/parse"       read            "$DIR/tiny.cid" parse
    check_stage "$SUFFIX" "$KIND/lex/parse"        lex             "$DIR/tiny.cid" parse
    check_stage "$SUFFIX" "$KIND/parse/parse"      parse           "$DIR/tiny.cid" parse
    check_stage "$SUFFIX" "$KIND/symbol/check"     symbol          "$DIR/tiny.cid" check
    check_stage "$SUFFIX" "$KIND/typecheck/check"  typecheck       "$DIR/tiny.cid" check
    check_stage "$SUFFIX" "$KIND/ir_gen/build"     ir_gen          "$DIR/tiny.cid" build
    check_stage "$SUFFIX" "$KIND/ir_gen/run"       ir_gen          "$DIR/tiny.cid" run
    check_stage "$SUFFIX" "$KIND/optimize/compile" optimize        "$DIR/tiny.cid" compile -o "$DIR/out.asm"
    check_stage "$SUFFIX" "$KIND/optimize/run"     optimize        "$DIR/tiny.cid" run
    check_stage "$SUFFIX" "$KIND/deconstruct"      deconstruct     "$DIR/tiny.cid" compile -o "$DIR/out.asm"
    check_stage "$SUFFIX" "$KIND/codegen"          codegen         "$DIR/tiny.cid" compile -o "$DIR/out.asm"
done

# The `--emit-meta` snapshot write (makeDirs + writeFileOrThrow), which only
# runs in module mode and so has no non-module path to compare against.
mkdir -p "$DIR/cache"
cat > "$DIR/mod.cid" <<'CID'
func add(a: int, b: int) -> int { return a + b; }
CID
for SUFFIX in "" ":len"; do
    OUT=$(CIDER_FAULT_ALLOC="meta$SUFFIX" timeout 120 \
          "$BIN" compile --cache "$DIR/cache" --emit-meta "$DIR/mod.cid" \
          -o "$DIR/mod.asm" 2>&1 >/dev/null)
    RC=$?
    expect_oom "meta$SUFFIX/emit-meta" "$OUT" "$RC"
done

# The hook must be inert without the env var: the same commands still succeed.
run_cider "$DIR/tiny.cid" parse
[ "$RC" -eq 0 ] || fail "hook is not inert: parse exit $RC (stderr: [$OUT])"
run_cider "$DIR/tiny.cid" check
[ "$RC" -eq 0 ] || fail "hook is not inert: check exit $RC (stderr: [$OUT])"
run_cider "$DIR/tiny.cid" run
[ "$RC" -eq 42 ] || fail "hook is not inert: run exit $RC, want 42 (stderr: [$OUT])"
run_cider "$DIR/tiny.cid" compile -o "$DIR/clean.asm"
[ "$RC" -eq 0 ] || fail "hook is not inert: compile exit $RC (stderr: [$OUT])"
run_cider "$DIR/tiny.cid" bundle
[ "$RC" -eq 0 ] || fail "hook is not inert: bundle exit $RC (stderr: [$OUT])"
# A stage name that does not exist must not match anything either (the hook
# matches a prefix, so an unrelated name must leave the run untouched).
OUT=$(CIDER_FAULT_ALLOC="nosuchstage" timeout 120 "$BIN" check "$DIR/tiny.cid" 2>&1 >/dev/null)
RC=$?
[ "$RC" -eq 0 ] || fail "unknown stage name matched: check exit $RC (stderr: [$OUT])"

# ---------------------------------------------------------------- part 2 ----
# The end-to-end half: a real allocation failure from real memory pressure.
echo "compiler_oom_smoke: real out-of-memory under RLIMIT_AS"

# SKIP rather than fail when the inherited hard RLIMIT_AS cannot allow the
# cap this test needs (77 is the suite's environment-dependent convention).
HARD_KB=$(ulimit -Hv)
if [ "$HARD_KB" != "unlimited" ] && [ "$HARD_KB" -lt "$LIMIT_KB" ]; then
    echo "compiler_oom_smoke: SKIP (hard RLIMIT_AS ${HARD_KB} KiB < ${LIMIT_KB})"
    exit 77
fi

# ~8.4 MB of source, generated rather than checked in. Uncapped it peaks
# ~150 MiB for `bundle` and several hundred MiB for `build`/`compile`, so the
# 100 MiB cap lands squarely in the allocator's failure range while still
# sitting >10x above the ~10 MiB the binary needs just to load. Memory spent
# here is bounded: the only uncapped heavy run is `bundle` (~150 MiB), and
# every capped child is confined to the cap itself.
awk 'BEGIN {
    print "func main() -> int {"
    for (i = 0; i < 200000; i++) printf "    var v%d: int = %d + 1 * 2 - 3;\n", i, i
    print "    return 0;"
    print "}"
}' > "$DIR/huge.cid"

# Control: the SAME source must succeed uncapped, so the capped run below
# proves something about allocation failure and not about a bad input.
# `bundle` is the cheap command on this size (~1 s, ~150 MiB); `build` and
# `compile` take minutes here, which is why the capped runs are the only
# thing those two are asked to do — and they abort on the cap in ~1 s.
run_cider "$DIR/huge.cid" bundle
if [ "$RC" -ne 0 ]; then
    fail "control: uncapped bundle of huge.cid exit $RC (stderr: [$OUT])"
else
    # Same command, same source, now under the cap: the only difference is
    # available address space.
    OUT=$( ( ulimit -v "$LIMIT_KB"; timeout 300 "$BIN" bundle "$DIR/huge.cid" 2>&1 >/dev/null ) )
    RC=$?
    expect_oom "real-OOM/bundle" "$OUT" "$RC"
fi

# Control: the IR-generating path is fine without pressure too, on a source
# small enough to build quickly.
awk 'BEGIN {
    print "func main() -> int {"
    for (i = 0; i < 20000; i++) printf "    var v%d: int = %d + 1 * 2 - 3;\n", i, i
    print "    return 0;"
    print "}"
}' > "$DIR/mid.cid"
run_cider "$DIR/mid.cid" build
[ "$RC" -eq 0 ] || fail "control: uncapped build of mid.cid exit $RC (stderr: [$OUT])"
run_cider "$DIR/mid.cid" compile -o "$DIR/mid.asm"
[ "$RC" -eq 0 ] || fail "control: uncapped compile of mid.cid exit $RC (stderr: [$OUT])"

# The capped IR path: `build` reaches the allocator from the IR/optimizer
# side and `compile` from the deconstructor/codegen side. Both must look
# exactly like the `bundle` failure above.
OUT=$( ( ulimit -v "$LIMIT_KB"; timeout 300 "$BIN" build "$DIR/huge.cid" 2>&1 >/dev/null ) )
RC=$?
expect_oom "real-OOM/build" "$OUT" "$RC"

OUT=$( ( ulimit -v "$LIMIT_KB"; timeout 300 "$BIN" compile "$DIR/huge.cid" -o "$DIR/huge.asm" 2>&1 >/dev/null ) )
RC=$?
expect_oom "real-OOM/compile" "$OUT" "$RC"

# ---------------------------------------------------------------- part 3 ----
# One exit code for the WHOLE compiler: the VM's own allocation failure must
# report the same code 2, so an OOM is an OOM whichever stage hit it.
# CIDER-BUG-121 owns the VM-side message and pins it in
# tests/programs/{map_new_oom,vector_reserve_oom} and tests/unit/vm_oom_smoke.sh;
# what is checked here is that this boundary did not move it.
echo "compiler_oom_smoke: exit-code agreement with the VM boundary"
for PROG in map_new_oom vector_reserve_oom; do
    SRC=$REPO/tests/programs/$PROG/main.cid
    if [ ! -f "$SRC" ]; then
        fail "missing fixture $SRC"
        continue
    fi
    run_cider "$SRC" run
    if [ "$RC" -ne "$OOM_RC" ]; then
        fail "vm-$PROG: exit $RC, want $OOM_RC (stderr: [$OUT])"
    fi
    case "$OUT" in
        *std::*|*bad_alloc*|*terminate*|*"what():"*)
            fail "vm-$PROG: C++ exception noise leaked: [$OUT]" ;;
    esac
done

if [ "$fails" -ne 0 ]; then
    echo "compiler_oom_smoke: FAIL ($fails failing case(s))"
    exit 1
fi

echo "compiler_oom_smoke: OK"