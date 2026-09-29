#!/bin/bash
# CIDER-BUG-211: a method call on a nil interface is a defined runtime error
# on every backend. A nil interface carries a zero vtable word, so dispatch
# used to read a method address out of address 0: the VM caught it only by
# accident (load's CIDER-BUG-059 zero-address guard, reporting the unrelated
# "load: null pointer dereference") while native x86 executed `mov rax, [rbx]`
# with rbx = 0 and died with SIGSEGV and no output at all (exit 139).
#
# The guard in ir-gen is NOT gated on --checks, so the three backend paths
# agree in the DEFAULT configuration:
#   stderr = "vm error: nil interface dispatch", exit = 2
# This script pins that agreement plus the codegen-side invariants a
# triple-differential run cannot see: that the helper is emitted without any
# flag, that a user-defined `_nil_dispatch_fail` suppresses it (no duplicate
# NASM label), and that the uefi target traps with ud2.
#
# Usage: nil_iface_dispatch_smoke.sh <cider-binary>
# Exits 77 when nasm/gcc are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: nil_iface_dispatch_smoke.sh <cider-binary>}
for tool in nasm gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "nil_iface_dispatch_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "nil_iface_dispatch_smoke: FAIL ($1)"
    exit 1
}

# Mirrors tests/run_tests.py:run_native — -nostartfiles only when the object
# defines its own _start, so libc's crt0 provides the entry otherwise.
vm_run() { # <src> [flags...]
    local src=$1; shift
    set +e
    VM_OUT=$("$BIN" run "$@" "$src" 2>&1 >/dev/null)
    VM_RC=$?
    set -e
}

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

EXPECT_NIL="vm error: nil interface dispatch"

# --- the nil receivers that all used to SIGSEGV ---------------------------
# Each source drives a distinct path to the dispatch: a local, an array
# element (the ticket's reproducer), a parameter in a non-inlined callee, a
# class field, and a value returned through an sret slot.
write_case() { # <name> <body>
    cat > "$DIR/$1.cid" <<CID
interface Shape {
  func kind() -> int;
  func grow(n: int) -> int;
}

class Circle {
  var r: int;
  func kind() -> int { return 1; }
  func grow(n: int) -> int { return this.r + n; }
}

class Holder {
  var id: int;
  var shape: Shape;
}

func viaParam(s: Shape) -> int { return s.kind(); }
func makeNil() -> Shape { var s: Shape; return s; }

func main() -> int {
$2
}
CID
}

write_case local '  var s: Shape;
  s.kind();
  return 0;'

write_case array_elem '  var z: [Shape; 1];
  z[0].kind();
  return 0;'

write_case array_nonzero '  var z: [Shape; 3];
  z[2].grow(1);
  return 0;'

write_case param '  var s: Shape;
  viaParam(s);
  return 0;'

write_case field '  var h: Holder;
  h.shape.kind();
  return 0;'

write_case field_ptr '  var h: Holder;
  var hp: *Holder = &h;
  hp->shape.grow(1);
  return 0;'

write_case sret '  var t: Shape = makeNil();
  t.grow(1);
  return 0;'

for case in local array_elem array_nonzero param field field_ptr sret; do
    for opt in "--no-opt" ""; do
        vm_run "$DIR/$case.cid" $opt
        [ "$VM_RC" -eq 2 ] || fail "vm $opt $case: exit $VM_RC (want 2)"
        [ "$VM_OUT" = "$EXPECT_NIL" ] || fail "vm $opt $case stderr: [$VM_OUT]"
    done
    native_run "$DIR/$case.cid"
    [ "$NATIVE_RC" -eq 2 ] || fail "native $case: exit $NATIVE_RC (want 2, not a signal)"
    [ "$NATIVE_OUT" = "$EXPECT_NIL" ] || fail "native $case stderr: [$NATIVE_OUT]"
done

# --- a non-nil receiver still dispatches on every backend -----------------
# The guard must cost a not-taken branch and nothing more.
cat > "$DIR/live.cid" <<'CID'
interface Shape {
  func kind() -> int;
  func grow(n: int) -> int;
}

class Circle {
  var r: int;
  func kind() -> int { return 1; }
  func grow(n: int) -> int { return this.r + n; }
}

class Square {
  var s: int;
  func kind() -> int { return 2; }
  func grow(n: int) -> int { return this.s + n; }
}

func main() -> int {
  var c: Circle;
  c.r = 10;
  var q: Square;
  q.s = 20;
  var a: Shape = c;
  var b: Shape = q;
  if (a.kind() != 1) { return 1; }
  if (b.kind() != 2) { return 2; }
  if (a.grow(5) != 15) { return 3; }
  if (b.grow(5) != 25) { return 4; }
  // Repeated dispatch in a loop keeps working (the guard is per-call).
  var i: int = 0;
  var acc: int = 0;
  while (i < 50) {
    acc = acc + a.grow(1) + b.grow(1);
    i = i + 1;
  }
  if (acc != 50 * (11 + 21)) { return 5; }
  return 0;
}
CID

for opt in "--no-opt" ""; do
    vm_run "$DIR/live.cid" $opt
    [ "$VM_RC" -eq 0 ] || fail "vm $opt live dispatch: exit $VM_RC"
    [ -z "$VM_OUT" ] || fail "vm $opt live dispatch stderr: [$VM_OUT]"
done
native_run "$DIR/live.cid"
[ "$NATIVE_RC" -eq 0 ] || fail "native live dispatch: exit $NATIVE_RC"
[ -z "$NATIVE_OUT" ] || fail "native live dispatch stderr: [$NATIVE_OUT]"

# --- the helper is emitted in the DEFAULT configuration --------------------
# Unlike --checks, the guard needs no flag, so the default assembly already
# references and defines the routine.
"$BIN" compile "$DIR/local.cid" -o "$DIR/default.asm"
grep -q "call _nil_dispatch_fail" "$DIR/default.asm" \
    || fail "default assembly never calls _nil_dispatch_fail"
grep -q "^_nil_dispatch_fail:" "$DIR/default.asm" \
    || fail "default assembly does not define _nil_dispatch_fail"
# The message payload must match the VM's exactly, byte for byte.
grep -q 'db "vm error: nil interface dispatch", 10' "$DIR/default.asm" \
    || fail "native message payload differs from the VM's"
# One call, one definition: no duplicate NASM label (nasm would fail anyway,
# but the check names the reason).
[ "$(grep -c '^_nil_dispatch_fail:' "$DIR/default.asm")" -eq 1 ] \
    || fail "duplicate _nil_dispatch_fail label"

# --- a user-defined _nil_dispatch_fail suppresses the helper ---------------
# Same convention as _uncaught_exception (CIDER-ISS-010) and _bounds_fail
# (CIDER-ISS-001): a user function of that name wins, so no duplicate label is
# emitted. The user's function is free to return, in which case the dispatch
# resumes and the nil load faults the pre-existing way — the override opts out
# of the diagnostic, it does not get two of them.
cat > "$DIR/override.cid" <<'CID'
interface Shape { func kind() -> int; }
func _nil_dispatch_fail() -> int { return 5; }
func main() -> int { var s: Shape; return s.kind(); }
CID
"$BIN" compile "$DIR/override.cid" -o "$DIR/override.asm" \
    || fail "user-defined _nil_dispatch_fail does not compile"
[ "$(grep -c '^_nil_dispatch_fail:' "$DIR/override.asm")" -eq 1 ] \
    || fail "user-defined _nil_dispatch_fail emitted a duplicate label"
# The single definition must be the user's compiled function, not the builtin
# helper: a user body starts with a real prologue, the helper with a syscall.
# (Its `return 5` also shows up as `mov eax, 5` before the epilogue.)
sed -n '/^_nil_dispatch_fail:/,/^\._nil_dispatch_fail_dead0:/p' "$DIR/override.asm" \
    | grep -q "push rbp" \
    || fail "user-defined _nil_dispatch_fail body was replaced by the builtin"
if sed -n '/^_nil_dispatch_fail:/,/^\._nil_dispatch_fail_dead0:/p' "$DIR/override.asm" \
        | grep -qE '^[[:space:]]*syscall'; then
    fail "user-defined _nil_dispatch_fail got the builtin helper body"
fi

# --- the uefi target traps instead of calling write/exit ------------------
# UEFI has no write/exit syscalls at this layer, so the helper traps (ud2),
# exactly like the uncaught-exception and bounds helpers.
"$BIN" compile --target uefi "$DIR/local.cid" -o "$DIR/uefi.asm" \
    || fail "uefi compile failed"
grep -q "^_nil_dispatch_fail:" "$DIR/uefi.asm" \
    || fail "uefi assembly does not define _nil_dispatch_fail"
# The helper is the last thing codegen emits, so bound the range at 4 lines
# (label + body) rather than to EOF: an unbounded range would pick up
# unrelated syscalls from a later section and give a false positive.
UEFI_BODY=$(sed -n '/^_nil_dispatch_fail:/,+3p' "$DIR/uefi.asm")
# Match the instruction, not the word: the ud2 comment itself says
# "no write/exit syscalls", so a bare `grep syscall` matches the comment.
if printf '%s\n' "$UEFI_BODY" | grep -qE '^[[:space:]]*syscall'; then
    fail "uefi helper uses a syscall (must ud2, like the sibling helpers)"
fi
printf '%s\n' "$UEFI_BODY" | grep -qE '^[[:space:]]*ud2' \
    || fail "uefi helper does not trap with ud2"

# --- --checks composes: the bounds diagnostic still wins, unchanged -------
cat > "$DIR/both.cid" <<'CID'
interface Shape { func kind() -> int; }
func main() -> int {
  var a: [int; 2];
  var v: int = a[9];
  var s: Shape;
  return s.kind() + v;
}
CID
for opt in "--no-opt" ""; do
    vm_run "$DIR/both.cid" --checks $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt --checks composition: exit $VM_RC"
    [ "$VM_OUT" = "vm error: index out of bounds" ] \
        || fail "vm $opt --checks composition stderr: [$VM_OUT]"
done
native_run "$DIR/both.cid" --checks
[ "$NATIVE_RC" -eq 2 ] || fail "native --checks composition: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "vm error: index out of bounds" ] \
    || fail "native --checks composition stderr: [$NATIVE_OUT]"

echo "nil_iface_dispatch_smoke: OK"
