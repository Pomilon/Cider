#!/bin/bash
# CIDER-ISS-001: `--checks` emits runtime bounds checks for indexing a
# fixed-size array. The VM and the native backend must agree byte-for-byte
# on the failure: stderr `vm error: index out of bounds` and exit 2. A valid
# index still runs normally, and WITHOUT the flag the pre-existing silent
# out-of-bounds read is unchanged (default output stays byte-identical).
#
# Usage: bounds_check_smoke.sh <cider-binary>
# Exits 77 when nasm/gcc are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: bounds_check_smoke.sh <cider-binary>}
for tool in nasm gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "bounds_check_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "bounds_check_smoke: FAIL ($1)"
    exit 1
}

# Run the VM; captures stderr in VM_OUT and the exit code in VM_RC.
vm_run() { # <src> [flags...]
    local src=$1; shift
    set +e
    VM_OUT=$("$BIN" run "$@" "$src" 2>&1 >/dev/null)
    VM_RC=$?
    set -e
}

# Compile with the given flags and run the native binary; captures stderr
# in NATIVE_OUT and the exit code in NATIVE_RC (mirrors tests/run_tests.py's
# linker choice: -nostartfiles when the object defines its own _start).
native_run() { # <src> [flags...]
    local src=$1; shift
    "$BIN" compile "$@" "$src" -o "$DIR/prog.asm"
    nasm -O0 -f elf64 "$DIR/prog.asm" -o "$DIR/prog.o"
    if nm "$DIR/prog.o" 2>/dev/null | grep -q " _start$"; then
        gcc -nostartfiles "$DIR/prog.o" -o "$DIR/prog" -no-pie
    else
        gcc "$DIR/prog.o" -o "$DIR/prog" -no-pie
    fi
    set +e
    NATIVE_OUT=$("$DIR/prog" 2>&1 >/dev/null)
    NATIVE_RC=$?
    set -e
}

EXPECT_OOB="vm error: index out of bounds"

# Out-of-bounds READ: `idx` is a global so the optimizer cannot fold it.
cat > "$DIR/oob.cid" <<'CID'
var idx: int = 0;

func main() -> int {
    var a: [int; 3];
    a[0] = 10;
    a[1] = 20;
    a[2] = 30;
    idx = 5;
    var v: int = a[idx];
    return 0;
}
CID

# Out-of-bounds STORE (assignment target, not just a load).
cat > "$DIR/store_oob.cid" <<'CID'
var idx: int = 0;

func main() -> int {
    var a: [int; 3];
    idx = 4;
    a[idx] = 7;
    return 0;
}
CID

# Nested array: the trap must fire on the inner dimension too
# (CIDER-BUG-052 shape).
cat > "$DIR/nested_oob.cid" <<'CID'
var idx: int = 0;

func main() -> int {
    var m: [[int; 2]; 3];
    m[0][0] = 1;
    m[0][1] = 2;
    idx = 2;
    var v: int = m[0][idx];
    return 0;
}
CID

# Pointer indexing stays unchecked (no static bound): borrowed storage may
# be longer than any array the pointer was taken from.
cat > "$DIR/pointer.cid" <<'CID'
var idx: int = 0;

func main() -> int {
    var a: [int; 3];
    a[0] = 1;
    a[1] = 2;
    a[2] = 3;
    var p: int* = &a[0];
    idx = 5;
    var v: int = p[idx];
    return 0;
}
CID

# In-range runtime index: with --checks the program still runs and exits 0.
cat > "$DIR/valid.cid" <<'CID'
var idx: int = 0;

func main() -> int {
    var a: [int; 3];
    a[0] = 10;
    a[1] = 20;
    a[2] = 30;
    idx = 1;
    var v: int = a[idx];
    if (v != 20) { return 9; }
    return 0;
}
CID

# --- --checks: VM (unopt + opt) and native trap identically --------------
for opt in "--no-opt" ""; do
    vm_run "$DIR/oob.cid" --checks $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt oob read: exit $VM_RC"
    [ "$VM_OUT" = "$EXPECT_OOB" ] || fail "vm $opt oob read stderr: [$VM_OUT]"
done
native_run "$DIR/oob.cid" --checks
[ "$NATIVE_RC" -eq 2 ] || fail "native oob read: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "$EXPECT_OOB" ] || fail "native oob read stderr: [$NATIVE_OUT]"

vm_run "$DIR/store_oob.cid" --checks
[ "$VM_RC" -eq 2 ] || fail "vm oob store: exit $VM_RC"
[ "$VM_OUT" = "$EXPECT_OOB" ] || fail "vm oob store stderr: [$VM_OUT]"
native_run "$DIR/store_oob.cid" --checks
[ "$NATIVE_RC" -eq 2 ] || fail "native oob store: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "$EXPECT_OOB" ] || fail "native oob store stderr: [$NATIVE_OUT]"

vm_run "$DIR/nested_oob.cid" --checks
[ "$VM_RC" -eq 2 ] || fail "vm nested oob: exit $VM_RC"
[ "$VM_OUT" = "$EXPECT_OOB" ] || fail "vm nested oob stderr: [$VM_OUT]"
native_run "$DIR/nested_oob.cid" --checks
[ "$NATIVE_RC" -eq 2 ] || fail "native nested oob: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "$EXPECT_OOB" ] || fail "native nested oob stderr: [$NATIVE_OUT]"

vm_run "$DIR/pointer.cid" --checks
[ "$VM_RC" -eq 0 ] || fail "vm pointer index (unchecked): exit $VM_RC"
[ -z "$VM_OUT" ] || fail "vm pointer index stderr: [$VM_OUT]"
native_run "$DIR/pointer.cid" --checks
[ "$NATIVE_RC" -eq 0 ] || fail "native pointer index (unchecked): exit $NATIVE_RC"
[ -z "$NATIVE_OUT" ] || fail "native pointer index stderr: [$NATIVE_OUT]"

# --- --checks: a valid index is a no-op ----------------------------------
for opt in "--no-opt" ""; do
    vm_run "$DIR/valid.cid" --checks $opt
    [ "$VM_RC" -eq 0 ] || fail "vm $opt valid index: exit $VM_RC"
    [ -z "$VM_OUT" ] || fail "vm $opt valid index stderr: [$VM_OUT]"
done
native_run "$DIR/valid.cid" --checks
[ "$NATIVE_RC" -eq 0 ] || fail "native valid index: exit $NATIVE_RC"
[ -z "$NATIVE_OUT" ] || fail "native valid index stderr: [$NATIVE_OUT]"

# --- default (no --checks): the old silent read is unchanged -------------
for opt in "--no-opt" ""; do
    vm_run "$DIR/oob.cid" $opt
    [ "$VM_RC" -eq 0 ] || fail "vm $opt default oob read: exit $VM_RC"
    [ -z "$VM_OUT" ] || fail "vm $opt default oob read stderr: [$VM_OUT]"
done
native_run "$DIR/oob.cid"
[ "$NATIVE_RC" -eq 0 ] || fail "native default oob read: exit $NATIVE_RC"
[ -z "$NATIVE_OUT" ] || fail "native default oob read stderr: [$NATIVE_OUT]"

# --- default: the helper is not even emitted -----------------------------
"$BIN" compile "$DIR/oob.cid" -o "$DIR/default.asm"
if grep -q "_bounds_fail" "$DIR/default.asm"; then
    fail "default assembly references _bounds_fail"
fi
"$BIN" compile --checks "$DIR/oob.cid" -o "$DIR/checked.asm"
if ! grep -q "_bounds_fail" "$DIR/checked.asm"; then
    fail "checked assembly misses _bounds_fail"
fi

echo "bounds_check_smoke: OK"
