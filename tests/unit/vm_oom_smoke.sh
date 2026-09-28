#!/bin/bash
# CIDER-BUG-121: VM allocation failures under host memory pressure must
# surface through the VM's error convention (`vm error: out of memory` on
# stderr, exit 2), never as an escaping std::bad_alloc that the CLI's generic
# handler reports as a compiler failure (`<file>: std::bad_alloc`, exit 1).
#
# Shape: RLIMIT_AS 1536 MiB lets std.alloc map a full 1 GiB arena into
# VM::heap_, but not the host vector's next grow (it doubles the ~1 GiB
# buffer). The allocation after the arena is either a fresh 4096-byte
# frame-local chunk (a Cider call) or an oversized local's heap fallback;
# both escaped VM::run as std::bad_alloc before the fix.
#
# Non-OOM behavior is unchanged: the same allocation shapes run cleanly
# without the cap.
#
# Usage: vm_oom_smoke.sh <cider-binary>
set -euo pipefail

BIN=${1:?usage: vm_oom_smoke.sh <cider-binary>}

# The scenario maps ~1 GiB; skip when the inherited hard RLIMIT_AS cannot
# allow the 1536 MiB cap this test sets for its children (SKIP_RETURN_CODE
# 77, the suite's convention for environment-dependent smokes).
HARD_KB=$(ulimit -Hv)
if [ "$HARD_KB" != "unlimited" ] && [ "$HARD_KB" -lt 1572864 ]; then
    echo "vm_oom_smoke: SKIP (hard RLIMIT_AS ${HARD_KB} KiB < 1572864)"
    exit 77
fi

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "vm_oom_smoke: FAIL ($1)"
    exit 1
}

# Run the VM under an optional address-space cap (0 = inherit); captures the
# exit code in VM_RC and stderr in VM_OUT.
run_vm() { # <limit_kb|0> <src> [flags...]
    local limit=$1; shift
    local src=$1; shift
    set +e
    if [ "$limit" -gt 0 ]; then
        VM_OUT=$( ( ulimit -v "$limit"; "$BIN" run "$@" "$src" 2>&1 >/dev/null ) )
    else
        VM_OUT=$( "$BIN" run "$@" "$src" 2>&1 >/dev/null )
    fi
    VM_RC=$?
    set -e
}

EXPECT_OOM="vm error: out of memory"
LIMIT_KB=1572864  # 1536 MiB

# A Cider call after a full 1 GiB arena needs a fresh 4096-byte frame-local
# chunk per live frame; enough nesting exhausts the heap's remaining
# capacity and the host vector grow fails.
cat > "$DIR/chunks.cid" <<'CID'
from std.alloc import malloc;

func sink(p: *int) -> int {
    return *p;
}

func deep(n: int) -> int {
    var y: int = n;
    var keep: int = sink(&y);
    if (n <= 0) { return keep - keep; }
    return deep(n - 1) + keep - keep;
}

func main() -> int {
    var p: *u64 = malloc(1073741808) as *u64;  // 1 GiB - 16
    if (p == nilPtr) return 65;
    *p = 1;
    return deep(128);
}
CID

# After the same arena, a local larger than a frame chunk falls back to
# VM::malloc and forces the host vector to grow.
cat > "$DIR/biglocal.cid" <<'CID'
from std.alloc import malloc;

func touch(b: *u8) -> int {
    return *b as int;
}

func helper() -> int {
    var buf: [u8; 8388608];  // 8 MiB local: heap fallback path
    buf[0] = 1;
    return touch(&buf[0]) - 1;
}

func main() -> int {
    var p: *u64 = malloc(1073741808) as *u64;  // 1 GiB - 16
    if (p == nilPtr) return 65;
    *p = 1;
    return helper();
}
CID

# The same allocation shapes on a small arena: without memory pressure the
# VM must run to completion exactly as before.
cat > "$DIR/ok.cid" <<'CID'
from std.alloc import malloc;

func sink(p: *int) -> int {
    return *p;
}

func deep(n: int) -> int {
    var y: int = n;
    var keep: int = sink(&y);
    if (n <= 0) { return keep - keep; }
    return deep(n - 1) + keep - keep;
}

func touch(b: *u8) -> int {
    return *b as int;
}

func helper() -> int {
    var buf: [u8; 1048576];  // 1 MiB local: heap fallback path
    buf[0] = 7;
    return touch(&buf[0]) - 7;
}

func main() -> int {
    var p: *u64 = malloc(16777200) as *u64;  // 16 MiB - 16 arena
    if (p == nilPtr) return 65;
    *p = 1;
    return deep(16) + helper();
}
CID

for opt in "--no-opt" ""; do
    run_vm 0 "$DIR/ok.cid" $opt
    [ "$VM_RC" -eq 0 ] || fail "vm $opt ok.cid: exit $VM_RC (stderr: [$VM_OUT])"
    [ -z "$VM_OUT" ] || fail "vm $opt ok.cid stderr: [$VM_OUT]"
done

for opt in "--no-opt" ""; do
    run_vm "$LIMIT_KB" "$DIR/chunks.cid" $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt chunks.cid: exit $VM_RC (stderr: [$VM_OUT])"
    [ "$VM_OUT" = "$EXPECT_OOM" ] || fail "vm $opt chunks.cid stderr: [$VM_OUT]"
done

for opt in "--no-opt" ""; do
    run_vm "$LIMIT_KB" "$DIR/biglocal.cid" $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt biglocal.cid: exit $VM_RC (stderr: [$VM_OUT])"
    [ "$VM_OUT" = "$EXPECT_OOM" ] || fail "vm $opt biglocal.cid stderr: [$VM_OUT]"
done

echo "vm_oom_smoke: OK"
