#!/bin/bash
# CIDER-BUG-219: a dereference through a nil pointer is a defined runtime error
# on every backend. The VM caught it only by accident, in `load`'s
# CIDER-BUG-059 exact-zero address guard, and reported the IR-shaped
# `vm error: load: null pointer dereference`; native x86 executed
# `mov rax, [rbx]` with rbx = 0 and died with SIGSEGV and NO OUTPUT at all
# (exit 139). The same shape diverged for a nil-receiver VIRTUAL_CALL, which
# reads the vtable out of address 0. The exact-zero test was also too weak:
# `p[1]` through a nil `p` has effective address 8, inside the VM's first heap
# block, so the VM silently read the block's size header and returned 0 while
# native still trapped.
#
# ir-gen now guards the POINTER, before the address arithmetic, and routes the
# dereference to the `_nil_deref_fail` routine — the same shape CIDER-ISS-001
# fixed for bounds and CIDER-BUG-211 for nil dispatch. The guard is NOT
# --checks-gated (the VM's zero-address report is unconditional, so gating would
# leave the backends disagreeing in the default configuration), so the three
# backend paths agree with no flag:
#   stderr = "vm error: null pointer dereference", exit = 2
#
# This script pins that agreement across every shape the guard is attached to,
# plus the codegen-side invariants a triple-differential run cannot see: that
# the helper is emitted without any flag, that the message payload matches the
# VM byte for byte, that a user-defined `_nil_deref_fail` suppresses the helper
# (no duplicate NASM label), that pomeos gets its own syscall numbers and uefi
# traps with ud2, and that --checks still reports the bounds error unchanged.
#
# Usage: nil_pointer_deref_smoke.sh <cider-binary>
# Exits 77 when nasm/gcc are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: nil_pointer_deref_smoke.sh <cider-binary>}
for tool in nasm gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "nil_pointer_deref_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "nil_pointer_deref_smoke: FAIL ($1)"
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

EXPECT_NIL="vm error: null pointer dereference"

# --- the preamble every case shares ----------------------------------------
# `class Box`/`Base`/`Derived` exist so the method-call, virtual-call and
# aggregate-deref shapes have something to dereference; `live*` first proves
# each shape still works with a real receiver, so a guard that fires
# unconditionally (or traps a live call) cannot pass unnoticed.
write_case() { # <name> <body>
    cat > "$DIR/$1.cid" <<CID
class Base {
  var n: int;
  virtual func kind() -> int { return this.n; }
}
class Derived extends Base {
  override func kind() -> int { return this.n + 1; }
}
class Box {
  var a: int;
  var b: int;
  func sum() -> int { return this.a + this.b; }
}
class Pair { var l: int; var r: int; }
class Holder { var p: *int; }
interface Shape { func kind() -> int; }
class Sq { var s: int; func kind() -> int { return this.s; } }

func readThrough(p: *int) -> int { return *p; }
func writeThrough(p: *int, v: int) -> int { *p = v; return *p; }
func readIndex(p: *int, i: int) -> int { return p[i]; }
func writeIndex(p: *int, i: int, v: int) -> int { p[i] = v; return p[i]; }
func readField(b: *Box) -> int { return b.a; }
func writeField(b: *Box, v: int) -> int { b.a = v; return b.a; }
func callMethod(b: *Box) -> int { return b.sum(); }
func callVirtual(p: *Base) -> int { return p.kind(); }
func readAggregate(p: *Box) -> int { var q: Box = *p; return q.a; }

func main() -> int {
$2
}
CID
}

# 1. The ticket's reproducer, verbatim: the plain read.
write_case deref_read '  var p: *int = nilPtr;
  return *p;'

# 2. A store through the nil pointer (`*p = v`, and the compound form).
write_case deref_store '  var p: *int = nilPtr;
  *p = 5;
  return 0;'
write_case deref_compound '  var p: *int = nilPtr;
  *p += 5;
  return 0;'

# 3. A member read and a member write through the nil pointer. The guard sits
#    on the object pointer, not on `obj + field.offset`, so a field at a
#    non-zero offset is caught too.
write_case field_read '  var b: *Box = nilPtr;
  return readField(b);'
write_case field_read_offset '  var p: *Pair = nilPtr;
  return p.r;'
write_case field_store '  var b: *Box = nilPtr;
  return writeField(b, 5);'

# 5. An element read and write through it. `p[1]` has effective address 8,
write_case index_read '  var p: *int = nilPtr;
  return readIndex(p, 1);'
write_case index_store '  var p: *int = nilPtr;
  writeIndex(p, 1, 5);
  return 0;'

# 6. A method call through the nil pointer — the direct and the virtual route.
#    Base has a subclass, so `p.kind()` is a real VIRTUAL_CALL, not a
#    devirtualised direct call.
write_case method_call '  var b: *Box = nilPtr;
  return callMethod(b);'
write_case vcall '  var p: *Base = nilPtr;
  return callVirtual(p);'
write_case vcall_inline '  var p: *Base = nilPtr;
  return p.kind();'

# 7. Copying the aggregate a nil pointer points at (`var q: Box = *p`), and
#    reading a field through it. The `*` hands back the POINTER as the address,
#    so the consumer dereferences it several frames away from the `*`.
write_case aggregate_copy '  var b: *Box = nilPtr;
  return readAggregate(b);'
write_case aggregate_field '  var p: *Box = nilPtr;
  return (*p).a;'

# 8. A pointer-to-pointer. (A nil-receiver interface call is a separate case
#    below: it needs the interface machinery, which the shared preamble
#    deliberately leaves out so these cases stay plain pointer arithmetic.)
write_case ptr_to_ptr '  var p: **int = nilPtr;
  return **p;'

# 9. The nil value threaded through a parameter, so the dereference happens in
#    a non-inlined callee rather than at the point the nil was created.
write_case via_param '  var p: *int = nilPtr;
  return readThrough(p);'

# 10. The nil value read back out of an array element, a class field and a
#     pointer-to-class field — the same routes CIDER-BUG-211 pinned for a nil
#     interface, so the general guard is pinned on them too.
write_case from_array '  var a: [*int; 2];
  return readThrough(a[0]);'
write_case from_field '  var h: Holder;
  return readThrough(h.p);'
write_case from_field_ptr '  var h: Holder;
  var hp: *Holder = &h;
  return readThrough(hp.p);'

for case in deref_read deref_store deref_compound \
            field_read field_read_offset field_store \
            index_read index_store \
            method_call vcall vcall_inline \
            aggregate_copy aggregate_field \
            ptr_to_ptr via_param \
            from_array from_field from_field_ptr; do
    for opt in "--no-opt" ""; do
        vm_run "$DIR/$case.cid" $opt
        [ "$VM_RC" -eq 2 ] || fail "vm $opt $case: exit $VM_RC (want 2, not a signal)"
        [ "$VM_OUT" = "$EXPECT_NIL" ] || fail "vm $opt $case stderr: [$VM_OUT]"
    done
    native_run "$DIR/$case.cid"
    [ "$NATIVE_RC" -eq 2 ] || fail "native $case: exit $NATIVE_RC (want 2, not a signal)"
    [ "$NATIVE_OUT" = "$EXPECT_NIL" ] || fail "native $case stderr: [$NATIVE_OUT]"
done

# A nil-receiver call through a pointer to an INTERFACE reads the fat pointer's
# two words out of address 0, so it needs the interface machinery. Kept out of
# the shared preamble (above) and pinned here.
cat > "$DIR/iface_ptr.cid" <<'CID'
interface Shape { func kind() -> int; }
class Sq { var s: int; func kind() -> int { return this.s; } }
func main() -> int {
  var p: *Shape = nilPtr;
  return (*p).kind();
}
CID
for opt in "--no-opt" ""; do
    vm_run "$DIR/iface_ptr.cid" $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt iface_ptr: exit $VM_RC (want 2, not a signal)"
    [ "$VM_OUT" = "$EXPECT_NIL" ] || fail "vm $opt iface_ptr stderr: [$VM_OUT]"
done
native_run "$DIR/iface_ptr.cid"
[ "$NATIVE_RC" -eq 2 ] || fail "native iface_ptr: exit $NATIVE_RC (want 2, not a signal)"
[ "$NATIVE_OUT" = "$EXPECT_NIL" ] || fail "native iface_ptr stderr: [$NATIVE_OUT]"

# --- a non-nil pointer still dereferences on every backend -----------------
# The guard must cost a not-taken branch and nothing more. Every shape the
# cases above guard is exercised here with a real pointer, so a guard that
# fired unconditionally, or that clobbered the pointer, fails loudly.
cat > "$DIR/live.cid" <<'CID'
class Base {
  var n: int;
  virtual func kind() -> int { return this.n; }
}
class Derived extends Base {
  override func kind() -> int { return this.n + 1; }
}
class Box {
  var a: int;
  var b: int;
  func sum() -> int { return this.a + this.b; }
}
class Pair { var l: int; var r: int; }
interface Shape { func kind() -> int; }
class Sq { var s: int; func kind() -> int { return this.s; } }

func readThrough(p: *int) -> int { return *p; }
func writeThrough(p: *int, v: int) -> int { *p = v; return *p; }
func readIndex(p: *int, i: int) -> int { return p[i]; }
func writeIndex(p: *int, i: int, v: int) -> int { p[i] = v; return p[i]; }
func readField(b: *Box) -> int { return b.a; }
func writeField(b: *Box, v: int) -> int { b.a = v; return b.a; }
func callMethod(b: *Box) -> int { return b.sum(); }
func callVirtual(p: *Base) -> int { return p.kind(); }
func readAggregate(p: *Box) -> int { var q: Box = *p; return q.a; }

func main() -> int {
  var cell: int = 41;
  var live: *int = &cell;
  if (readThrough(live) != 41) { return 1; }
  if (writeThrough(live, 42) != 42) { return 2; }
  if (readIndex(live, 0) != 42) { return 3; }
  if (writeIndex(live, 0, 43) != 43) { return 4; }
  if (*live != 43) { return 6; }

  var box: Box;
  box.a = 7;
  box.b = 8;
  var liveBox: *Box = &box;
  if (readField(liveBox) != 7) { return 7; }
  if (writeField(liveBox, 9) != 9) { return 8; }
  if (callMethod(liveBox) != 17) { return 9; }
  if (readAggregate(liveBox) != 9) { return 10; }

  var base: Base;
  base.n = 5;
  var derived: Derived;
  derived.n = 6;
  if (callVirtual(&derived) != 7) { return 11; }   // the override runs
  if (callVirtual(&base) != 5) { return 12; }      // and the base

  var pair: Pair;
  pair.l = 3;
  pair.r = 4;
  var livePair: *Pair = &pair;
  if (livePair.r != 4) { return 13; }

  // Pointer arithmetic behind an explicit nilPtr check — the idiom a linked
  // list or a manual walk uses. The guard composes with it: the check passes,
  // so the dereference proceeds.
  var arr: [int; 3];
  arr[0] = 1; arr[1] = 2; arr[2] = 3;
  var walk: *int = &arr[0];
  var seen: int = 0;
  var i: int = 0;
  while (i < 3) {
    if (walk == nilPtr) { break; }
    seen = seen + *walk;
    walk = walk + 1;
    i = i + 1;
  }
  if (seen != 6) { return 14; }

  var pp: **int = &live;
  if (**pp != 43) { return 15; }

  // A pointer stored in a field, in an array element and behind a pointer, so
  // the same routes the nil cases use are walked with a live value too.
  var holder: Holder;
  holder.p = live;
  if (readThrough(holder.p) != 43) { return 16; }
  var hp: *Holder = &holder;
  if (readThrough(hp.p) != 43) { return 17; }
  var elems: [*int; 2];
  elems[0] = live;
  if (readThrough(elems[0]) != 43) { return 18; }

  // A live interface value, dispatched through the fat pointer. A non-nil
  // `*Shape` is not expressible from source (`&sq` does not convert to a
  // pointer-to-interface), so the live half of the nil `*Shape` case above is
  // the value form: same fat-pointer load, same vtable word.
  var sq: Sq;
  sq.s = 21;
  var sv: Shape = sq;
  if (sv.kind() != 21) { return 19; }

  // Repeated dereference in a loop keeps working (the guard is per-deref).
  var acc: int = 0;
  var k: int = 0;
  while (k < 50) {
    acc = acc + readThrough(live) + readField(liveBox);
    k = k + 1;
  }
  if (acc != 50 * (43 + 9)) { return 20; }
  return 0;
}
CID
sed -i 's/^class Pair .*$/&\nclass Holder { var p: *int; }/' "$DIR/live.cid"
for opt in "--no-opt" ""; do
    vm_run "$DIR/live.cid" $opt
    [ "$VM_RC" -eq 0 ] || fail "vm $opt live deref: exit $VM_RC"
    [ -z "$VM_OUT" ] || fail "vm $opt live deref stderr: [$VM_OUT]"
done
native_run "$DIR/live.cid"
[ "$NATIVE_RC" -eq 0 ] || fail "native live deref: exit $NATIVE_RC"
[ -z "$NATIVE_OUT" ] || fail "native live deref stderr: [$NATIVE_OUT]"

# --- the helper is emitted in the DEFAULT configuration --------------------
# Unlike --checks, the guard needs no flag, so the default assembly already
# references and defines the routine.
"$BIN" compile "$DIR/deref_read.cid" -o "$DIR/default.asm"
grep -q "call _nil_deref_fail" "$DIR/default.asm" \
    || fail "default assembly never calls _nil_deref_fail"
grep -q "^_nil_deref_fail:" "$DIR/default.asm" \
    || fail "default assembly does not define _nil_deref_fail"
# The message payload must match the VM's exactly, byte for byte.
grep -q 'db "vm error: null pointer dereference", 10' "$DIR/default.asm" \
    || fail "native message payload differs from the VM's"
# The length in `mov rdx, N` must be the payload's, or native would truncate or
# run past the string.
grep -q "mov rdx, 35" "$DIR/default.asm" \
    || fail "native message length is not strlen of the payload"
# One call, one definition: no duplicate NASM label (nasm would fail anyway,
# but the check names the reason).
[ "$(grep -c '^_nil_deref_fail:' "$DIR/default.asm")" -eq 1 ] \
    || fail "duplicate _nil_deref_fail label"

# The guard must be on the POINTER, not the effective address: `p[1]` through a
# nil `p` has address 8, and the VM's old exact-zero guard read the first heap
# block's size header there. If the guard were placed after the index scale,
# the assembly would compare 8 (or the scaled address) instead of the pointer.
"$BIN" compile "$DIR/index_read.cid" -o "$DIR/index.asm"
grep -q "call _nil_deref_fail" "$DIR/index.asm" \
    || fail "pointer indexing never calls _nil_deref_fail"
# `readIndex` multiplies the index by the element size before the load, so a
# check on the scaled address would be compared against a value derived from
# the index. Pin the pointer check appearing before the multiply.
python3 - "$DIR/index.asm" <<'PY' || fail "the index guard does not precede the element scale"
import re, sys
lines = open(sys.argv[1]).read().splitlines()
fn = next(i for i, l in enumerate(lines) if l.startswith("readIndex:"))
end = next((i for i in range(fn + 1, len(lines))
            if re.match(r"^[A-Za-z_][A-Za-z0-9_.]*:", lines[i])), len(lines))
body = lines[fn:end]
guard = next((i for i, l in enumerate(body) if "call _nil_deref_fail" in l), None)
scale = next((i for i, l in enumerate(body) if re.match(r"^\s+imul ", l)), None)
if guard is None:
    sys.exit(1)
if scale is not None and guard > scale:
    sys.exit(1)
PY

# --- a user-defined _nil_deref_fail suppresses the helper ------------------
# Same convention as _uncaught_exception (CIDER-ISS-010), _bounds_fail
# (CIDER-ISS-001) and _nil_dispatch_fail (CIDER-BUG-211): a user function of
# that name wins, so no duplicate label is emitted. The user's function is free
# to return, in which case the dereference resumes and faults the pre-existing
# way — the override opts out of the diagnostic, it does not get two of them.
cat > "$DIR/override.cid" <<'CID'
func _nil_deref_fail() -> int { return 5; }
func main() -> int { var p: *int = nilPtr; return *p; }
CID
"$BIN" compile "$DIR/override.cid" -o "$DIR/override.asm" \
    || fail "user-defined _nil_deref_fail does not compile"
[ "$(grep -c '^_nil_deref_fail:' "$DIR/override.asm")" -eq 1 ] \
    || fail "user-defined _nil_deref_fail emitted a duplicate label"
# The single definition must be the user's compiled function, not the builtin
# helper: a user body starts with a real prologue, the helper with a syscall.
sed -n '/^_nil_deref_fail:/,/^\._nil_deref_fail_dead0:/p' "$DIR/override.asm" \
    | grep -q "push rbp" \
    || fail "user-defined _nil_deref_fail body was replaced by the builtin"
if sed -n '/^_nil_deref_fail:/,/^\._nil_deref_fail_dead0:/p' "$DIR/override.asm" \
        | grep -qE '^[[:space:]]*syscall'; then
    fail "user-defined _nil_deref_fail got the builtin helper body"
fi

# --- pomeos gets its own syscall numbers ------------------------------------
# PomeOS is not Linux: its write is 13 and its exit is 2, so a helper that kept
# the Linux numbers would trap or exit with the wrong code there. The rewrite
# is shared with the sibling helpers; pin that this one is covered too.
"$BIN" compile --target pomeos "$DIR/deref_read.cid" -o "$DIR/pomeos.asm" \
    || fail "pomeos compile failed"
grep -q "^_nil_deref_fail:" "$DIR/pomeos.asm" \
    || fail "pomeos assembly does not define _nil_deref_fail"
POMEOS_BODY=$(sed -n '/^_nil_deref_fail:/,+6p' "$DIR/pomeos.asm")
printf '%s\n' "$POMEOS_BODY" | grep -q "mov rax, 13" \
    || fail "pomeos helper does not use PomeOS's write syscall (13)"
printf '%s\n' "$POMEOS_BODY" | grep -q "mov rax, 2" \
    || fail "pomeos helper does not use PomeOS's exit syscall (2)"

# --- the uefi target traps instead of calling write/exit -------------------
# UEFI has no write/exit syscalls at this layer, so the helper traps (ud2),
# exactly like the uncaught-exception, bounds and nil-dispatch helpers.
"$BIN" compile --target uefi "$DIR/deref_read.cid" -o "$DIR/uefi.asm" \
    || fail "uefi compile failed"
grep -q "^_nil_deref_fail:" "$DIR/uefi.asm" \
    || fail "uefi assembly does not define _nil_deref_fail"
# The helper is the last thing codegen emits, so bound the range at 4 lines
# (label + body) rather than to EOF: an unbounded range would pick up
# unrelated syscalls from a later section and give a false positive.
UEFI_BODY=$(sed -n '/^_nil_deref_fail:/,+3p' "$DIR/uefi.asm")
# Match the instruction, not the word: the ud2 comment itself says
# "no write/exit syscalls", so a bare `grep syscall` matches the comment.
if printf '%s\n' "$UEFI_BODY" | grep -qE '^[[:space:]]*syscall'; then
    fail "uefi helper uses a syscall (must ud2, like the sibling helpers)"
fi
printf '%s\n' "$UEFI_BODY" | grep -qE '^[[:space:]]*ud2' \
    || fail "uefi helper does not trap with ud2"

# --- --checks composes: the bounds diagnostic still wins, unchanged -------
# A nil pointer and an out-of-range index are both runtime errors; the array
# bounds one is reported first because the guard for a fixed-size array VALUE
# sits ahead of the dereference, and its message must be untouched.
cat > "$DIR/both.cid" <<'CID'
func main() -> int {
  var a: [int; 2];
  var v: int = a[9];
  var p: *int = nilPtr;
  return v + *p;
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

# --- the nil-interface diagnostic is untouched -----------------------------
# CIDER-BUG-211's guard tests the vtable WORD, this one tests the receiver
# POINTER; a nil interface value (never assigned) still reports the dispatch
# error, not the dereference one, so the two guards stay distinguishable.
cat > "$DIR/iface.cid" <<'CID'
interface Shape { func kind() -> int; }
class Circle { func kind() -> int { return 1; } }
func main() -> int { var s: Shape; s.kind(); return 0; }
CID
for opt in "--no-opt" ""; do
    vm_run "$DIR/iface.cid" $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt nil interface: exit $VM_RC"
    [ "$VM_OUT" = "vm error: nil interface dispatch" ] \
        || fail "vm $opt nil interface stderr: [$VM_OUT]"
done
native_run "$DIR/iface.cid"
[ "$NATIVE_RC" -eq 2 ] || fail "native nil interface: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "vm error: nil interface dispatch" ] \
    || fail "native nil interface stderr: [$NATIVE_OUT]"

echo "nil_pointer_deref_smoke: OK"
