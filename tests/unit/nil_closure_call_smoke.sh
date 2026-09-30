#!/bin/bash
# CIDER-BUG-237: calling through a nil closure OBJECT is a defined runtime error
# on every backend. It is the case CIDER-BUG-225 found and left
# deliberately unpinned, and the boundary that ticket's note points at:
#
#   var c: func(int) -> int;
#   c(3);
#
# A `func(...)` value is a closure OBJECT {callable slot, captures...}, and the
# call reads the slot out of it. That read is a LOAD `ir-gen` emits itself, so
# none of CIDER-BUG-219's source-level guards (which attach to `*`, a pointer
# member/index base and a pointer receiver) can see it -- and CIDER-BUG-225's
# guard sits on the wrong side of it, because it tests the LOADED slot (the
# callee), which a nil object faults before ever producing.
#
# The VM reached load's exact-zero CIDER-BUG-059 guard and reported the IR
# instruction that faulted, `vm error: load: null pointer dereference`; native
# x86 executed `mov rax, [rbx]` with rbx = 0 and died with SIGSEGV and NO
# OUTPUT at all (exit 139). `ir-gen` now guards the object with CIDER-BUG-219's
# own `_nil_deref_fail` routine, so the diagnostic is the language-level one on
# every backend and stays distinct from the `call: nil function pointer` a nil
# callable slot reports. The guard is NOT --checks-gated (the VM's report is
# unconditional, so gating would leave the backends disagreeing in the default
# configuration), so the three backend paths agree with no flag:
#   stderr = "vm error: null pointer dereference", exit = 2
#
# This script pins that agreement across every route a nil closure object
# travels, plus the codegen-side invariants a triple-differential run cannot
# see: that the helper is emitted without any flag, that the message payload
# matches the VM byte for byte (with its length), that the guard sits on the
# object and branches BEFORE the load of the callable slot it protects, that the
# object guard and CIDER-BUG-225's slot guard each test the register the other
# one does not, that a live closure call is untouched, that a user-defined
# `_nil_deref_fail` suppresses the helper (no duplicate NASM label), that pomeos
# gets its own syscall numbers and uefi traps with ud2, and that the two sibling
# diagnostics are unchanged in both directions so all three guards stay
# distinguishable.
#
# Usage: nil_closure_call_smoke.sh <cider-binary>
# Exits 77 when nasm/gcc are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: nil_closure_call_smoke.sh <cider-binary>}
for tool in nasm gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "nil_closure_call_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "nil_closure_call_smoke: FAIL ($1)"
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
# `Cb` covers the alias spelling (an alias name in TypeInfo that has to be
# resolved before the guard), `Holder`/`hp`/`Outer` the class-field,
# pointer-to-class-field and nested-field routes, and `makeAdder`/`pick` the
# closure objects -- a live one built by a function and a nil one it returns --
# so the call happens in a non-inlined callee.
write_case() { # <name> <body>
    cat > "$DIR/$1.cid" <<CID
type Cb = func(int) -> int;

class Holder { var c: func(int) -> int; }
class Outer { var b: Holder; }

var globalNil: func(int) -> int;

func viaParam(c: func(int) -> int, x: int) -> int { return c(x); }
func pick(b: bool) -> func(int) -> int {
  if (b) { return func(x: int) -> int { return x * 3; }; }
  return nilPtr;
}

func main() -> int {
$2
}
CID
}

# 1. The ticket's reproducer, verbatim: a never-assigned closure local.
write_case local_nil '  var c: func(int) -> int;
  return c(3);'

# 2. The same call behind an alias spelling, which carries the alias name in
#    its TypeInfo and so resolves through a different spelling of the shape.
write_case alias_nil '  var c: Cb;
  return c(3);'

# 3. A nil closure object threaded through a PARAMETER, so the call happens in
#    a non-inlined callee rather than at the point the nil appeared.
write_case via_param '  return viaParam(nilPtr, 3);'

# 4. A nil closure object read back out of a class field, a pointer-to-class
#    field, a nested field and an array element -- the same routes
#    CIDER-BUG-211 pinned for a nil interface, CIDER-BUG-219 for a nil pointer
#    and CIDER-BUG-225 for a nil callee, so this guard is pinned on them too.
write_case from_field '  var h: Holder;
  return h.c(3);'
write_case from_field_ptr '  var h: Holder;
  var hp: *Holder = &h;
  return hp.c(3);'
write_case from_nested_field '  var o: Outer;
  return o.b.c(3);'
write_case from_array '  var a: [func(int) -> int; 2];
  return a[0](3);'

# 5. A nil closure object RETURNED by a function, called directly on the result
#    and through a temporary. `pick`'s live arm is the closure-literal build
#    route, which is the other half of this shape.
write_case ret_nil '  return pick(false)(3);'
write_case ret_nil_tmp '  var c: func(int) -> int = pick(false);
  return c(3);'

# 6. A never-assigned module-scope global (the shape CIDER-BUG-228 made
#    order-independent).
write_case global_nil '  return globalNil(3);'

# 7. An explicitly nil initializer. This is a different spelling of the same
#    fault -- the frame slot holds a zero rather than never being written -- and
#    it must report the same message, not the `call:` one, because the fault is
#    still the load of the callable slot.
write_case nil_init '  var c: func(int) -> int = nilPtr;
  return c(3);'

for case in local_nil alias_nil via_param \
            from_field from_field_ptr from_nested_field from_array \
            ret_nil ret_nil_tmp global_nil nil_init; do
    for opt in "--no-opt" ""; do
        vm_run "$DIR/$case.cid" $opt
        [ "$VM_RC" -eq 2 ] || fail "vm $opt $case: exit $VM_RC (want 2, not a signal)"
        [ "$VM_OUT" = "$EXPECT_NIL" ] || fail "vm $opt $case stderr: [$VM_OUT]"
    done
    native_run "$DIR/$case.cid"
    [ "$NATIVE_RC" -eq 2 ] || fail "native $case: exit $NATIVE_RC (want 2, not a signal)"
    [ "$NATIVE_OUT" = "$EXPECT_NIL" ] || fail "native $case stderr: [$NATIVE_OUT]"
done

# --- a live closure object still gets called on every backend ----------------
# The guard must cost a not-taken branch and nothing more. Every shape the
# cases above guard is exercised here with a real closure, so a guard that fired
# unconditionally, or that clobbered the object pointer, fails loudly.
cat > "$DIR/live.cid" <<'CID'
type Cb = func(int) -> int;

class Holder { var c: func(int) -> int; }
class Outer { var b: Holder; }

var globalNil: func(int) -> int;

func viaParam(c: func(int) -> int, x: int) -> int { return c(x); }
func pick(b: bool) -> func(int) -> int {
  if (b) { return func(x: int) -> int { return x * 3; }; }
  return nilPtr;
}
func makeAdder(base: int) -> func(int) -> int {
  return func(x: int) -> int { return x + base; };
}

func main() -> int {
  // A local closure, reassigned and then called repeatedly.
  var c: func(int) -> int = makeAdder(1);
  if (c(3) != 4) { return 1; }
  c = makeAdder(10);
  if (c(3) != 13) { return 2; }
  var acc: int = 0;
  var i: int = 0;
  while (i < 20) {
    acc = acc + c(1);
    i = i + 1;
  }
  if (acc != 220) { return 3; }

  // The alias spelling, a parameter, a class field, a pointer-to-class field,
  // a nested field and an array element: the routes the nil cases use, live.
  var a: Cb = makeAdder(100);
  if (a(3) != 103) { return 4; }
  if (viaParam(makeAdder(2), 3) != 5) { return 5; }
  var h: Holder;
  h.c = makeAdder(3);
  if (h.c(3) != 6) { return 6; }
  var hp: *Holder = &h;
  if (hp.c(3) != 6) { return 7; }
  var o: Outer;
  o.b.c = makeAdder(4);
  if (o.b.c(3) != 7) { return 8; }
  var arr: [func(int) -> int; 2];
  arr[0] = makeAdder(5);
  arr[1] = makeAdder(6);
  if (arr[0](3) != 8) { return 9; }
  if (arr[1](3) != 9) { return 10; }

  // A closure returned by a function, called on the result and via a temporary.
  if (pick(true)(3) != 9) { return 11; }
  var r: func(int) -> int = pick(true);
  if (r(3) != 9) { return 12; }

  // A module-scope global assigned a live closure.
  globalNil = makeAdder(7);
  if (globalNil(3) != 10) { return 13; }

  // A closure literal with a capture, called in place.
  var n: int = 5;
  var cap: func(int) -> int = func(x: int) -> int { return x + n; };
  if (cap(3) != 8) { return 14; }

  // Comparing a live closure against nilPtr reads the OBJECT pointer and is
  // not a dereference, so the guard does not change it.
  if (c == nilPtr) { return 15; }

  return 0;
}
CID
for opt in "--no-opt" ""; do
    vm_run "$DIR/live.cid" $opt
    [ "$VM_RC" -eq 0 ] || fail "vm $opt live closure object: exit $VM_RC"
    [ -z "$VM_OUT" ] || fail "vm $opt live closure object stderr: [$VM_OUT]"
done
native_run "$DIR/live.cid"
[ "$NATIVE_RC" -eq 0 ] || fail "native live closure object: exit $NATIVE_RC"
[ -z "$NATIVE_OUT" ] || fail "native live closure object stderr: [$NATIVE_OUT]"

# --- the helper is emitted in the DEFAULT configuration --------------------
# Unlike --checks, the guard needs no flag, so the default assembly already
# references and defines the routine.
"$BIN" compile "$DIR/local_nil.cid" -o "$DIR/default.asm"
grep -q "call _nil_deref_fail" "$DIR/default.asm" \
    || fail "default assembly never calls _nil_deref_fail"
grep -q "^_nil_deref_fail:" "$DIR/default.asm" \
    || fail "default assembly does not define _nil_deref_fail"
# The message payload must match the VM's exactly, byte for byte.
grep -q 'db "vm error: null pointer dereference", 10' "$DIR/default.asm" \
    || fail "native message payload differs from the VM's"
# The length in `mov rdx, N` must be the payload's (34 chars + the newline the
# `, 10` adds), or native would truncate the message or run past the string.
grep -q "mov rdx, 35" "$DIR/default.asm" \
    || fail "native message length is not strlen of the payload"
# One call, one definition: no duplicate NASM label (nasm would fail anyway,
# but the check names the reason).
[ "$(grep -c '^_nil_deref_fail:' "$DIR/default.asm")" -eq 1 ] \
    || fail "duplicate _nil_deref_fail label"

# --- the guard sits on the OBJECT and PRECEDES the slot load it protects -----
# A closure object is {callable slot, captures...}, so the call reads word 0 out
# of it. The guard has to test the object pointer BEFORE that read: after it the
# trap would be dead code and native would fault at address 0 first. Pin the
# order in the assembly, where the load is the `mov <reg>, [<obj>]` that feeds
# the slot.
python3 - "$DIR/default.asm" <<'PY' || fail "the guard does not precede the callable-slot load"
import re, sys
lines = open(sys.argv[1]).read().splitlines()
fn = next(i for i, l in enumerate(lines) if l.startswith("main:"))
end = next((i for i in range(fn + 1, len(lines))
            if re.match(r"^[A-Za-z_][A-Za-z0-9_.]*:", lines[i])), len(lines))
body = lines[fn:end]
guard = next((i for i, l in enumerate(body) if "call _nil_deref_fail" in l), None)
if guard is None:
    sys.exit(1)
# The slot load is `mov <reg>, [<reg>]` with no displacement; the immediate the
# closure call passes as its first argument is a separate `push`/`mov` of an
# integer, so a register-to-register `mov` is the load. Take the LAST one before
# the indirect call, which is the slot read the guard protects.
slot = None
for i, l in enumerate(body[:guard]):
    if re.match(r"^\s+mov\s+r[a-z0-9]+, qword \[r[a-z0-9]+", l):
        slot = i
if slot is None:
    sys.exit(1)
if guard <= slot:
    sys.exit(1)
# The compare that decides the branch must also be ahead of the load.
if not any(re.match(r"^\s+(cmp|sete|test)", l) for l in body[:guard]):
    sys.exit(1)
PY

# --- both guards are on the closure route, on different registers -----------
# CIDER-BUG-225 guards the LOADED callable slot (`_nil_fnptr_fail`, `call: nil
# function pointer`); CIDER-BUG-237 guards the object (`_nil_deref_fail`, `null
# pointer dereference`). Both must be present, each on the register the other
# one must not test, and the indirect call must still pass the guarded slot with
# the guarded object.
cat > "$DIR/clos.cid" <<'CID'
func makeAdder(base: int) -> func(int) -> int {
  return func(x: int) -> int { return x + base; };
}
func main() -> int { var c: func(int) -> int = makeAdder(1); return c(3); }
CID
"$BIN" build "$DIR/clos.cid" > "$DIR/clos.ir" \
    || fail "the live-closure program does not build"
python3 - "$DIR/clos.ir" <<'PY' || fail "the closure route does not guard the object and the slot separately"
import re, sys
ir = open(sys.argv[1]).read()
ir = ir[ir.index("define main() {"):]
# CIDER-BUG-225: the `call:` guard compares the LOADED slot.
m = re.search(r"%v\d+ = icmp eq (%v\d+), 0\s*\n\s*br %v\d+, \w*_nil_fnptr_fail", ir)
if m is None:
    sys.exit(1)
slot = m.group(1)
# The slot load it protects, and the object it came from.
m = re.search(re.escape(slot) + r" = load (%v\d+), 0", ir)
if m is None:
    sys.exit(1)
obj = m.group(1)
if obj == slot:
    sys.exit(1)
# CIDER-BUG-237: the `deref:` guard compares the OBJECT, before the slot load.
if not re.search(r"%v\d+ = icmp eq " + obj + r", 0\s*\n\s*br %v\d+, "
                 r"\w*_nil_deref_fail", ir):
    sys.exit(1)
# And it must not ALSO test the slot (that is the other ticket's guard).
if re.search(r"%v\d+ = icmp eq " + slot + r", 0\s*\n\s*br %v\d+, \w*_nil_deref_fail", ir):
    sys.exit(1)
# The indirect call must pass the guarded slot, not the object it came from.
if not re.search(r"call " + slot + ", " + obj, ir):
    sys.exit(1)
PY

# --- a nil closure object is NOT a nil function pointer, in either direction
# The two failures are distinguishable: a nil object faults a LOAD, a nil slot
# an ENTER. This is the assertion the CIDER-BUG-219 and CIDER-BUG-225 guards
# each could not make, because before this ticket the closure route had no
# object guard to confuse one with the other.
cat > "$DIR/fnptr_nil.cid" <<'CID'
func main() -> int { var g: fn(int) -> int = nilPtr; return g(3); }
CID
# Both guards are emitted on the closure route, so the object guard has to come
# FIRST: a nil object must report `null pointer dereference`, never the `call:`
# text, even though the slot guard is right there behind it.
"$BIN" compile "$DIR/local_nil.cid" -o "$DIR/object.asm"
python3 - "$DIR/object.asm" <<'PY' || fail "the nil-closure object guard is not the first of the two"
import re, sys
lines = open(sys.argv[1]).read().splitlines()
fn = next(i for i, l in enumerate(lines) if l.startswith("main:"))
end = next((i for i in range(fn + 1, len(lines))
            if re.match(r"^[A-Za-z_][A-Za-z0-9_.]*:", lines[i])), len(lines))
body = lines[fn:end]
guards = [(i, m.group(1)) for i, l in enumerate(body)
          for m in [re.search(r"call (_nil_\w+_fail)", l)] if m]
if not guards:
    sys.exit(1)
if guards[0][1] != "_nil_deref_fail":
    sys.exit(1)
# And the slot guard must still be emitted behind it, so a live object with a nil
# callable slot keeps CIDER-BUG-225's own diagnostic.
if [g[1] for g in guards] != ["_nil_deref_fail", "_nil_fnptr_fail"]:
    sys.exit(1)
PY
"$BIN" compile "$DIR/fnptr_nil.cid" -o "$DIR/slot.asm"
if grep -q "_nil_deref_fail" "$DIR/slot.asm"; then
    fail "a nil callable slot routes through _nil_deref_fail"
fi
for opt in "--no-opt" ""; do
    vm_run "$DIR/fnptr_nil.cid" $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt nil callee: exit $VM_RC"
    [ "$VM_OUT" = "vm error: call: nil function pointer" ] \
        || fail "vm $opt nil callee stderr: [$VM_OUT]"
done
native_run "$DIR/fnptr_nil.cid"
[ "$NATIVE_RC" -eq 2 ] || fail "native nil callee: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "vm error: call: nil function pointer" ] \
    || fail "native nil callee stderr: [$NATIVE_OUT]"

# --- the sibling diagnostics are unchanged ---------------------------------
# CIDER-BUG-219's guard tests a POINTER and CIDER-BUG-211's a vtable WORD.
# Both must still report their own messages, so all three guards stay
# distinguishable and none of them has borrowed this ticket's text.
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
    [ "$VM_OUT" = "$EXPECT_NIL" ] || fail "vm $opt nil pointer stderr: [$VM_OUT]"
    vm_run "$DIR/iface.cid" $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt nil interface: exit $VM_RC"
    [ "$VM_OUT" = "vm error: nil interface dispatch" ] \
        || fail "vm $opt nil interface stderr: [$VM_OUT]"
done
native_run "$DIR/deref.cid"
[ "$NATIVE_RC" -eq 2 ] || fail "native nil pointer: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "$EXPECT_NIL" ] || fail "native nil pointer stderr: [$NATIVE_OUT]"
native_run "$DIR/iface.cid"
[ "$NATIVE_RC" -eq 2 ] || fail "native nil interface: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "vm error: nil interface dispatch" ] \
    || fail "native nil interface stderr: [$NATIVE_OUT]"

# --- a user-defined _nil_deref_fail suppresses the helper -------------------
# Same convention as _uncaught_exception (CIDER-ISS-010), _bounds_fail
# (CIDER-ISS-001), _nil_dispatch_fail (CIDER-BUG-211), _nil_fnptr_fail
# (CIDER-BUG-225): a user function of that name wins, so no duplicate label is
# emitted. The user's function is free to return, in which case the call resumes
# and faults the pre-existing way -- the override opts out of the diagnostic, it
# does not get two of them.
cat > "$DIR/override.cid" <<'CID'
func _nil_deref_fail() -> int { return 5; }
func main() -> int { var c: func(int) -> int; return c(3); }
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
# the Linux numbers would trap or exit with the wrong code there.
"$BIN" compile --target pomeos "$DIR/local_nil.cid" -o "$DIR/pomeos.asm" \
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
# exactly like the uncaught-exception, bounds, nil-dispatch and nil-fnptr
# helpers.
"$BIN" compile --target uefi "$DIR/local_nil.cid" -o "$DIR/uefi.asm" \
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

# --- --checks composes: this guard is NOT gated by it ----------------------
# The guard is unconditional, so the diagnostic must be the same with and
# without the flag, and --checks must not turn a nil closure object into a
# silent crash or a different message.
for opt in "--no-opt" ""; do
    vm_run "$DIR/local_nil.cid" --checks $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt --checks composition: exit $VM_RC"
    [ "$VM_OUT" = "$EXPECT_NIL" ] \
        || fail "vm $opt --checks composition stderr: [$VM_OUT]"
done
native_run "$DIR/local_nil.cid" --checks
[ "$NATIVE_RC" -eq 2 ] || fail "native --checks composition: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "$EXPECT_NIL" ] \
    || fail "native --checks composition stderr: [$NATIVE_OUT]"

# --- a nil closure object that also indexes out of bounds ------------------
# Both are runtime errors, and the array-bounds one (CIDER-ISS-001) is
# reported first, because the index guard sits ahead of the call's guard. Its
# message must be untouched by this change.
cat > "$DIR/both.cid" <<'CID'
func makeAdder(base: int) -> func(int) -> int {
  return func(x: int) -> int { return x + base; };
}
func main() -> int {
  var a: [int; 2];
  var v: int = a[9];
  var c: func(int) -> int;
  return v + c(3);
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

# --- a direct call to a named function is not on the closure route ---------
# It cannot be nil and must pay nothing: no guard, no helper.
cat > "$DIR/direct.cid" <<'CID'
func dbl(x: int) -> int { return x * 2; }
func main() -> int { return dbl(3); }
CID
"$BIN" compile "$DIR/direct.cid" -o "$DIR/direct.asm"
if grep -q "_nil_deref_fail" "$DIR/direct.asm"; then
    fail "a direct call pays for the closure-object guard"
fi
for opt in "--no-opt" ""; do
    vm_run "$DIR/direct.cid" $opt
    [ "$VM_RC" -eq 6 ] || fail "vm $opt direct call: exit $VM_RC (want 6)"
    [ -z "$VM_OUT" ] || fail "vm $opt direct call stderr: [$VM_OUT]"
done
native_run "$DIR/direct.cid"
[ "$NATIVE_RC" -eq 6 ] || fail "native direct call: exit $NATIVE_RC (want 6)"
[ -z "$NATIVE_OUT" ] || fail "native direct call stderr: [$NATIVE_OUT]"

echo "nil_closure_call_smoke: OK"
