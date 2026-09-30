#!/bin/bash
# CIDER-BUG-225: a call through a nil function pointer is a defined runtime
# error on every backend. The VM caught a zero callee only by accident, in
# doCall's 1-based-id convention (module function values start at 1, so 0 is
# nilPtr), and reported `vm error: call: nil function pointer`; native x86
# executed `call r11` with r11 = 0 and died with SIGSEGV and NO OUTPUT at all
# (exit 139). Nothing is read through a nil function pointer, so no
# address-level test can close this the way one closed the CIDER-BUG-219 `p[1]`
# hole: the callee is ENTERED, so the only sound place to look is the callee
# value itself, immediately before the CALL.
#
# ir-gen now guards the callee value at both indirect-call sites — the typed
# `fn(...)` value and the loaded callable slot of a closure — and routes it to
# its OWN `_nil_fnptr_fail` routine, NOT CIDER-BUG-219's `_nil_deref_fail`:
# this is a distinct message family (the call is what failed, not a
# dereference), so it keeps the VM's existing `call:` text. The guard is NOT
# --checks-gated (the VM's report is unconditional, so gating would leave the
# backends disagreeing in the default configuration), so the three backend
# paths agree with no flag:
#   stderr = "vm error: call: nil function pointer", exit = 2
#
# This script pins that agreement across every shape the guard is attached to,
# plus the codegen-side invariants a triple-differential run cannot see: that
# the helper is emitted without any flag, that the message payload matches the
# VM byte for byte (with its length), that the guard sits on the callee and
# branches BEFORE the indirect call it protects, that a statically known callee
# costs nothing, that a user-defined `_nil_fnptr_fail` suppresses the helper
# (no duplicate NASM label), that pomeos gets its own syscall numbers and uefi
# traps with ud2, and that the two sibling diagnostics are unchanged so the
# three guards stay distinguishable.
#
# Usage: nil_fnptr_call_smoke.sh <cider-binary>
# Exits 77 when nasm/gcc are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: nil_fnptr_call_smoke.sh <cider-binary>}
for tool in nasm gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "nil_fnptr_call_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "nil_fnptr_call_smoke: FAIL ($1)"
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

EXPECT_NIL="vm error: call: nil function pointer"

# --- the preamble every case shares ----------------------------------------
# `Cb` covers the alias spelling (an alias name in TypeInfo that has to be
# resolved before the guard), `Holder`/`hp` the class-field and
# pointer-to-class-field routes, `makeAdder` the closure object whose callable
# slot the guard reads, and `pick` a nil callee returned from a function, so the
# call happens in a non-inlined callee. The `msabi` case needs its own program
# (below) rather than a `msabi` declaration here: on the `--target uefi` path a
# function declared after an `msabi` one is mis-typed, which would make this
# preamble's uefi compiles fail for a reason that has nothing to do with the
# guard under test.
write_case() { # <name> <body>
    cat > "$DIR/$1.cid" <<CID
type Cb = fn(int) -> int;

class Holder { var g: fn(int) -> int; }
class Base { virtual func kind(x: int) -> int { return x; } }
class Derived extends Base { override func kind(x: int) -> int { return x + 100; } }

func dbl(x: int) -> int { return x * 2; }
func inc(x: int) -> int { return x + 1; }
var globalNil: fn(int) -> int = nilPtr;
var globalLive: fn(int) -> int = dbl;

func viaParam(g: fn(int) -> int, x: int) -> int { return g(x); }
func pick(b: bool) -> fn(int) -> int { if (b) { return dbl; } return nilPtr; }
func makeAdder(base: int) -> func(int) -> int {
  return func(x: int) -> int { return x + base; };
}

func main() -> int {
$2
}
CID
}

# A program with no reference to a plain function THROUGH a fn pointer. The
# --target pomeos/uefi helper checks below use this one instead of a `write_case`
# program, because on the uefi path the default ABI is MS: there, any fn-pointer
# naming a plain `func` is (separately and pre-existingly) a type error, since a
# Default-ABI function is taken to be msabi. That has nothing to do with this
# guard, and `var g: fn(int) -> int = nilPtr` names no function at all.
cat > "$DIR/nil_only.cid" <<'CID'
func main() -> int { var g: fn(int) -> int = nilPtr; return g(3); }
CID

# 1. The ticket's reproducer, verbatim: a nil local, called directly.
write_case local_call '  var g: fn(int) -> int = nilPtr;
  return g(3);'

# 2. The same call behind an alias spelling, which carries the alias name in
#    its TypeInfo and so resolves through a different spelling of the shape.
write_case alias_call '  var g: Cb = nilPtr;
  return g(3);'

# 3. A nil callee threaded through a PARAMETER, so the call happens in a
#    non-inlined callee rather than at the point the nil was created.
write_case via_param '  return viaParam(nilPtr, 3);'

# 4. A nil callee read back out of a class field, a pointer-to-class field and
#    an array element — the same routes CIDER-BUG-211 pinned for a nil
#    interface and CIDER-BUG-219 for a nil pointer, so this guard is pinned on
#    them too.
write_case from_field '  var h: Holder;
  return h.g(3);'
write_case from_field_ptr '  var h: Holder;
  var hp: *Holder = &h;
  return hp.g(3);'
write_case from_array '  var a: [fn(int) -> int; 2];
  return a[0](3);'

# 5. A nil callee RETURNED by a function and called on the result, both as a
#    direct call and through a temporary.
write_case ret_nil '  return pick(false)(3);'
write_case ret_nil_tmp '  var g: fn(int) -> int = pick(false);
  return g(3);'

# 6. A nil module-scope global (the shape CIDER-BUG-228 made
#    order-independent).
write_case global_call '  return globalNil(3);'

# The `msabi fn` route: the guard is on the callee, so it must be placed on the
# MS-convention call too, not just the SysV one. Its own program, for the reason
# given above `write_case`.
cat > "$DIR/msabi_call.cid" <<'CID'
msabi func dblMs(x: int) -> int { return x + 20; }
func main() -> int { var g: msabi fn(int) -> int = nilPtr; return g(3); }
CID
cat > "$DIR/msabi_live.cid" <<'CID'
msabi func dblMs(x: int) -> int { return x + 20; }
func main() -> int { var g: msabi fn(int) -> int = dblMs; return g(3); }
CID

# 7. A closure read into a `fn` slot and called, so the closure route (where
#    the guard sits on the LOADED callable slot, not on the closure object) is
#    exercised on the live path below. Its nil form -- a never-assigned closure
#    OBJECT -- is deliberately NOT in the list above: that faults the load
#    ir-gen emits for the callable slot, which is a nil DEREFERENCE of the
#    object, not a nil callee, so it belongs to CIDER-BUG-219's message family
#    and is not this ticket's case.
write_case closure_call '  var c: func(int) -> int = makeAdder(1);
  return c(3);'
write_case closure_field '  var c: func(int) -> int = makeAdder(1);
  var slot: fn(int) -> int = c;
  return slot(3);'

for case in local_call alias_call via_param \
            from_field from_field_ptr from_array \
            ret_nil ret_nil_tmp global_call msabi_call; do
    for opt in "--no-opt" ""; do
        vm_run "$DIR/$case.cid" $opt
        [ "$VM_RC" -eq 2 ] || fail "vm $opt $case: exit $VM_RC (want 2, not a signal)"
        [ "$VM_OUT" = "$EXPECT_NIL" ] || fail "vm $opt $case stderr: [$VM_OUT]"
    done
    native_run "$DIR/$case.cid"
    [ "$NATIVE_RC" -eq 2 ] || fail "native $case: exit $NATIVE_RC (want 2, not a signal)"
    [ "$NATIVE_OUT" = "$EXPECT_NIL" ] || fail "native $case stderr: [$NATIVE_OUT]"
done

# The closure route is live here (its nil form is a nil OBJECT, not a nil
# callee, so it is not in the loop above): both backends must run it and agree
# on the answer, so the guard on the loaded callable slot costs nothing.
for opt in "--no-opt" ""; do
    vm_run "$DIR/closure_call.cid" $opt
    [ "$VM_RC" -eq 4 ] || fail "vm $opt closure_call: exit $VM_RC (want 4)"
    [ -z "$VM_OUT" ] || fail "vm $opt closure_call stderr: [$VM_OUT]"
    vm_run "$DIR/closure_field.cid" $opt
    [ "$VM_RC" -eq 4 ] || fail "vm $opt closure_field: exit $VM_RC (want 4)"
    [ -z "$VM_OUT" ] || fail "vm $opt closure_field stderr: [$VM_OUT]"
done
native_run "$DIR/closure_call.cid"
[ "$NATIVE_RC" -eq 4 ] || fail "native closure_call: exit $NATIVE_RC (want 4)"
[ -z "$NATIVE_OUT" ] || fail "native closure_call stderr: [$NATIVE_OUT]"
native_run "$DIR/closure_field.cid"
[ "$NATIVE_RC" -eq 4 ] || fail "native closure_field: exit $NATIVE_RC (want 4)"
[ -z "$NATIVE_OUT" ] || fail "native closure_field stderr: [$NATIVE_OUT]"

# --- a non-nil callee still gets called on every backend --------------------
# The guard must cost a not-taken branch and nothing more. Every shape the
# cases above guard is exercised here with a real callee, so a guard that fired
# unconditionally, or that clobbered the callee, fails loudly.
cat > "$DIR/live.cid" <<'CID'
type Cb = fn(int) -> int;

class Holder { var g: fn(int) -> int; }
class Base { virtual func kind(x: int) -> int { return x; } }
class Derived extends Base { override func kind(x: int) -> int { return x + 100; } }

func dbl(x: int) -> int { return x * 2; }
func inc(x: int) -> int { return x + 1; }
var globalNil: fn(int) -> int = nilPtr;
var globalLive: fn(int) -> int = dbl;

func viaParam(g: fn(int) -> int, x: int) -> int { return g(x); }
func pick(b: bool) -> fn(int) -> int { if (b) { return dbl; } return nilPtr; }
func makeAdder(base: int) -> func(int) -> int {
  return func(x: int) -> int { return x + base; };
}

func main() -> int {
  // A local, reassigned between calls, and called repeatedly.
  var g: fn(int) -> int = dbl;
  if (g(3) != 6) { return 1; }
  g = inc;
  if (g(3) != 4) { return 2; }
  var acc: int = 0;
  var i: int = 0;
  while (i < 20) {
    acc = acc + g(1);
    i = i + 1;
  }
  if (acc != 40) { return 3; }

  // The alias spelling, a parameter, a class field, a pointer-to-class field
  // and an array element: the four routes the nil cases use, live.
  var a: Cb = dbl;
  if (a(5) != 10) { return 4; }
  if (viaParam(dbl, 4) != 8) { return 5; }
  var h: Holder;
  h.g = inc;
  if (h.g(3) != 4) { return 6; }
  var hp: *Holder = &h;
  if (hp.g(3) != 4) { return 7; }
  var arr: [fn(int) -> int; 2];
  arr[0] = dbl;
  arr[1] = inc;
  if (arr[0](3) != 6) { return 8; }
  if (arr[1](3) != 4) { return 9; }

  // A value returned by a function, and the same slot in a temporary.
  if (pick(true)(3) != 6) { return 10; }
  var r: fn(int) -> int = pick(true);
  if (r(3) != 6) { return 11; }

  // A live global, the MS route, and a live closure (its callable slot).
  if (globalLive(3) != 6) { return 12; }
  var add10: func(int) -> int = makeAdder(10);
  if (add10(5) != 15) { return 14; }
  var slot: fn(int) -> int = add10;
  if (slot(5) != 15) { return 15; }

  // A virtual method call is a different dispatch route (CIDER-BUG-219 guards
  // its nil receiver) and must be unaffected.
  var derived: Derived;
  if (derived.kind(3) != 103) { return 16; }
  var base: Base;
  if (base.kind(3) != 3) { return 17; }

  // Comparing a live callee against nilPtr is unaffected by the guard.
  if (g == nilPtr) { return 18; }
  if (globalNil != nilPtr) { return 19; }

  return 0;
}
CID
for opt in "--no-opt" ""; do
    vm_run "$DIR/live.cid" $opt
    [ "$VM_RC" -eq 0 ] || fail "vm $opt live callee: exit $VM_RC"
    [ -z "$VM_OUT" ] || fail "vm $opt live callee stderr: [$VM_OUT]"
    vm_run "$DIR/msabi_live.cid" $opt
    [ "$VM_RC" -eq 23 ] || fail "vm $opt live msabi callee: exit $VM_RC (want 23)"
    [ -z "$VM_OUT" ] || fail "vm $opt live msabi callee stderr: [$VM_OUT]"
done
native_run "$DIR/live.cid"
[ "$NATIVE_RC" -eq 0 ] || fail "native live callee: exit $NATIVE_RC"
[ -z "$NATIVE_OUT" ] || fail "native live callee stderr: [$NATIVE_OUT]"
native_run "$DIR/msabi_live.cid"
[ "$NATIVE_RC" -eq 23 ] || fail "native live msabi callee: exit $NATIVE_RC (want 23)"
[ -z "$NATIVE_OUT" ] || fail "native live msabi callee stderr: [$NATIVE_OUT]"

# --- the helper is emitted in the DEFAULT configuration --------------------
# Unlike --checks, the guard needs no flag, so the default assembly already
# references and defines the routine.
"$BIN" compile "$DIR/local_call.cid" -o "$DIR/default.asm"
grep -q "call _nil_fnptr_fail" "$DIR/default.asm" \
    || fail "default assembly never calls _nil_fnptr_fail"
grep -q "^_nil_fnptr_fail:" "$DIR/default.asm" \
    || fail "default assembly does not define _nil_fnptr_fail"
# The message payload must match the VM's exactly, byte for byte.
grep -q 'db "vm error: call: nil function pointer", 10' "$DIR/default.asm" \
    || fail "native message payload differs from the VM's"
# The length in `mov rdx, N` must be the payload's (36 chars + the newline the
# `, 10` adds), or native would truncate the message or run past the string.
grep -q "mov rdx, 37" "$DIR/default.asm" \
    || fail "native message length is not strlen of the payload"
# One call, one definition: no duplicate NASM label (nasm would fail anyway,
# but the check names the reason).
[ "$(grep -c '^_nil_fnptr_fail:' "$DIR/default.asm")" -eq 1 ] \
    || fail "duplicate _nil_fnptr_fail label"

# The guard must branch on the CALLEE and must do so BEFORE the indirect call
# it protects — the callee is entered, not dereferenced, so the test has to be
# the callee value immediately ahead of the `call <reg>`. If it sat after the
# call, the trap would be dead code and native would branch to address 0 first.
python3 - "$DIR/default.asm" <<'PY' || fail "the guard does not precede the indirect call"
import re, sys
lines = open(sys.argv[1]).read().splitlines()
fn = next(i for i, l in enumerate(lines) if l.startswith("main:"))
end = next((i for i in range(fn + 1, len(lines))
            if re.match(r"^[A-Za-z_][A-Za-z0-9_.]*:", lines[i])), len(lines))
body = lines[fn:end]
guard = next((i for i, l in enumerate(body) if "call _nil_fnptr_fail" in l), None)
# An indirect call is `call r<N>` / `callq`; a direct one is a named symbol or a
# bare `call`, so only the register form is the one the guard protects.
indirect = next((i for i, l in enumerate(body)
                 if re.match(r"^\s+call\s+r[a-z0-9]+$", l)), None)
if guard is None:
    sys.exit(1)
if indirect is None:
    sys.exit(1)
if guard > indirect:
    sys.exit(1)
# The compare that decides must also be before the call, and the fail block must
# rejoin the live path so a user-defined helper that RETURNS resumes the call.
if not any("cmp" in l for l in body[:guard]):
    sys.exit(1)
if not any("jmp" in l or "je" in l or "jne" in l for l in body[guard:indirect]):
    sys.exit(1)
PY

# --- a statically known callee costs nothing -------------------------------
# A direct call to a named function is not an indirect call, so it must not
# carry the guard at all: the branch is a cost, and a direct call cannot be nil.
cat > "$DIR/direct.cid" <<'CID'
func dbl(x: int) -> int { return x * 2; }
func main() -> int { return dbl(3); }
CID
"$BIN" compile "$DIR/direct.cid" -o "$DIR/direct.asm"
if grep -q "_nil_fnptr_fail" "$DIR/direct.asm"; then
    fail "a statically known callee pays for the guard"
fi
for opt in "--no-opt" ""; do
    vm_run "$DIR/direct.cid" $opt
    [ "$VM_RC" -eq 6 ] || fail "vm $opt direct call: exit $VM_RC (want 6)"
    [ -z "$VM_OUT" ] || fail "vm $opt direct call stderr: [$VM_OUT]"
done
native_run "$DIR/direct.cid"
[ "$NATIVE_RC" -eq 6 ] || fail "native direct call: exit $NATIVE_RC (want 6)"
[ -z "$NATIVE_OUT" ] || fail "native direct call stderr: [$NATIVE_OUT]"

# --- a user-defined _nil_fnptr_fail suppresses the helper -------------------
# Same convention as _uncaught_exception (CIDER-ISS-010), _bounds_fail
# (CIDER-ISS-001), _nil_dispatch_fail (CIDER-BUG-211) and _nil_deref_fail
# (CIDER-BUG-219): a user function of that name wins, so no duplicate label is
# emitted. The user's function is free to return, in which case the call resumes
# and faults the pre-existing way — the override opts out of the diagnostic, it
# does not get two of them.
cat > "$DIR/override.cid" <<'CID'
func _nil_fnptr_fail() -> int { return 5; }
func main() -> int { var g: fn(int) -> int = nilPtr; return g(3); }
CID
"$BIN" compile "$DIR/override.cid" -o "$DIR/override.asm" \
    || fail "user-defined _nil_fnptr_fail does not compile"
[ "$(grep -c '^_nil_fnptr_fail:' "$DIR/override.asm")" -eq 1 ] \
    || fail "user-defined _nil_fnptr_fail emitted a duplicate label"
# The single definition must be the user's compiled function, not the builtin
# helper: a user body starts with a real prologue, the helper with a syscall.
sed -n '/^_nil_fnptr_fail:/,/^\._nil_fnptr_fail_dead0:/p' "$DIR/override.asm" \
    | grep -q "push rbp" \
    || fail "user-defined _nil_fnptr_fail body was replaced by the builtin"
if sed -n '/^_nil_fnptr_fail:/,/^\._nil_fnptr_fail_dead0:/p' "$DIR/override.asm" \
        | grep -qE '^[[:space:]]*syscall'; then
    fail "user-defined _nil_fnptr_fail got the builtin helper body"
fi

# --- pomeos gets its own syscall numbers ------------------------------------
# PomeOS is not Linux: its write is 13 and its exit is 2, so a helper that kept
# the Linux numbers would trap or exit with the wrong code there. The rewrite
# is shared with the sibling helpers; pin that this one is covered too.
"$BIN" compile --target pomeos "$DIR/nil_only.cid" -o "$DIR/pomeos.asm" \
    || fail "pomeos compile failed"
grep -q "^_nil_fnptr_fail:" "$DIR/pomeos.asm" \
    || fail "pomeos assembly does not define _nil_fnptr_fail"
POMEOS_BODY=$(sed -n '/^_nil_fnptr_fail:/,+6p' "$DIR/pomeos.asm")
printf '%s\n' "$POMEOS_BODY" | grep -q "mov rax, 13" \
    || fail "pomeos helper does not use PomeOS's write syscall (13)"
printf '%s\n' "$POMEOS_BODY" | grep -q "mov rax, 2" \
    || fail "pomeos helper does not use PomeOS's exit syscall (2)"

# --- the uefi target traps instead of calling write/exit -------------------
# UEFI has no write/exit syscalls at this layer, so the helper traps (ud2),
# exactly like the uncaught-exception, bounds, nil-dispatch and nil-deref
# helpers.
"$BIN" compile --target uefi "$DIR/nil_only.cid" -o "$DIR/uefi.asm" \
    || fail "uefi compile failed"
grep -q "^_nil_fnptr_fail:" "$DIR/uefi.asm" \
    || fail "uefi assembly does not define _nil_fnptr_fail"
# The helper is the last thing codegen emits, so bound the range at 4 lines
# (label + body) rather than to EOF: an unbounded range would pick up
# unrelated syscalls from a later section and give a false positive.
UEFI_BODY=$(sed -n '/^_nil_fnptr_fail:/,+3p' "$DIR/uefi.asm")
# Match the instruction, not the word: the ud2 comment itself says
# "no write/exit syscalls", so a bare `grep syscall` matches the comment.
if printf '%s\n' "$UEFI_BODY" | grep -qE '^[[:space:]]*syscall'; then
    fail "uefi helper uses a syscall (must ud2, like the sibling helpers)"
fi
printf '%s\n' "$UEFI_BODY" | grep -qE '^[[:space:]]*ud2' \
    || fail "uefi helper does not trap with ud2"

# --- --checks composes: this guard is NOT gated by it ----------------------
# The guard is unconditional, so the diagnostic must be the same with and
# without the flag, and --checks must not turn a nil callee into a silent
# crash or a different message.
for opt in "--no-opt" ""; do
    vm_run "$DIR/local_call.cid" --checks $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt --checks composition: exit $VM_RC"
    [ "$VM_OUT" = "$EXPECT_NIL" ] \
        || fail "vm $opt --checks composition stderr: [$VM_OUT]"
done
native_run "$DIR/local_call.cid" --checks
[ "$NATIVE_RC" -eq 2 ] || fail "native --checks composition: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "$EXPECT_NIL" ] \
    || fail "native --checks composition stderr: [$NATIVE_OUT]"

# --- a nil callee that also indexes out of bounds -------------------------
# Both are runtime errors, and the array-bounds one (CIDER-ISS-001) is
# reported first, because the index guard sits ahead of the call's guard. Its
# message must be untouched by this change.
cat > "$DIR/both.cid" <<'CID'
func dbl(x: int) -> int { return x * 2; }
func main() -> int {
  var a: [int; 2];
  var v: int = a[9];
  var g: fn(int) -> int = nilPtr;
  return v + g(3);
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

# --- the sibling diagnostics are unchanged ---------------------------------
# CIDER-BUG-219's guard tests a POINTER and CIDER-BUG-211's a vtable WORD;
# this one tests the CALLEE. A nil pointer dereference and a nil interface
# dispatch must still report their own messages, so the three guards stay
# distinguishable and none of them has borrowed the `call:` text.
cat > "$DIR/deref.cid" <<'CID'
func main() -> int { var p: *int = nilPtr; return *p; }
CID
cat > "$DIR/iface.cid" <<'CID'
interface Shape { func kind() -> int; }
class Circle { func kind() -> int { return 1; } }
func main() -> int { var s: Shape; s.kind(); return 0; }
CID
for opt in "--no-opt" ""; do
    vm_run "$DIR/deref.cid" $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt nil pointer: exit $VM_RC"
    [ "$VM_OUT" = "vm error: null pointer dereference" ] \
        || fail "vm $opt nil pointer stderr: [$VM_OUT]"
    vm_run "$DIR/iface.cid" $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt nil interface: exit $VM_RC"
    [ "$VM_OUT" = "vm error: nil interface dispatch" ] \
        || fail "vm $opt nil interface stderr: [$VM_OUT]"
done
native_run "$DIR/deref.cid"
[ "$NATIVE_RC" -eq 2 ] || fail "native nil pointer: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "vm error: null pointer dereference" ] \
    || fail "native nil pointer stderr: [$NATIVE_OUT]"
native_run "$DIR/iface.cid"
[ "$NATIVE_RC" -eq 2 ] || fail "native nil interface: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "vm error: nil interface dispatch" ] \
    || fail "native nil interface stderr: [$NATIVE_OUT]"

# A nil function pointer must NOT be reported as a nil dereference: the two
# diagnostics are for different failures, and a program can reach one without
# the other. This is the assertion the 219 guard could not make.
"$BIN" compile "$DIR/local_call.cid" -o "$DIR/distinct.asm"
if grep -q "_nil_deref_fail" "$DIR/distinct.asm"; then
    fail "a nil callee routes through _nil_deref_fail (must be its own builtin)"
fi
"$BIN" compile "$DIR/deref.cid" -o "$DIR/distinct2.asm"
if grep -q "_nil_fnptr_fail" "$DIR/distinct2.asm"; then
    fail "a nil dereference routes through _nil_fnptr_fail"
fi

# --- the closure route guards the CALLABLE SLOT, not the object ------------
# A closure value is an object {fnptr, captures...}; the call is made through
# the loaded first word, not through the object. So the guard must compare the
# LOADED slot, and not the object pointer -- the two are different faults with
# different messages. A nil OBJECT faults the load ir-gen emits for the slot
# (ir-gen's own load, invisible to CIDER-BUG-219's source-level guards) and is
# CIDER-BUG-237's `null pointer dereference`; a nil SLOT is this ticket's `call:
# nil function pointer`. Since CIDER-BUG-237 both guards exist on the closure
# route, so pin BOTH and that each tests the register the other one does not:
# the `call:` guard immediately after the slot load, the `deref:` guard on the
# slot load's own base before it.
cat > "$DIR/clos.cid" <<'CID'
func makeAdder(base: int) -> func(int) -> int {
  return func(x: int) -> int { return x + base; };
}
func main() -> int { var c: func(int) -> int = makeAdder(1); return c(3); }
CID
"$BIN" build "$DIR/clos.cid" > "$DIR/clos.ir" \
    || fail "the closure program does not build"
grep -q "_nil_fnptr_fail" "$DIR/clos.ir" \
    || fail "the closure route does not guard the callable slot"
python3 - "$DIR/clos.ir" <<'PY' || fail "the closure guards are not on the loaded slot and the object"
import re, sys
ir = open(sys.argv[1]).read()
ir = ir[ir.index("define main() {"):]
# CIDER-BUG-225: the `call:` guard compares the LOADED slot, not the object.
m = re.search(r"%v\d+ = icmp eq (%v\d+), 0\s*\n\s*br %v\d+, \w*_nil_fnptr_fail", ir)
if m is None:
    sys.exit(1)
slot = m.group(1)
# The slot load it protects, and the object it came from.
m = re.search(re.escape(slot) + r" = load (%v\d+), 0", ir)
if m is None:
    sys.exit(1)
obj = m.group(1)
# CIDER-BUG-237: the `deref:` guard compares the OBJECT, before the slot load.
if not re.search(r"%v\d+ = icmp eq " + obj + r", 0\s*\n\s*br %v\d+, "
                 r"\w*_nil_deref_fail", ir):
    sys.exit(1)
# The indirect call must pass the guarded slot, not the object it came from.
if not re.search(r"call " + slot + ", " + obj, ir):
    sys.exit(1)
PY

echo "nil_fnptr_call_smoke: OK"