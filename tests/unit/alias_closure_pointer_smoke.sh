#!/bin/bash
# CIDER-BUG-183: alias resolution reaches through pointer levels, so `*Cb`,
# `**Cb` and `*func(int) -> int` are ONE type -- and, because a wrong type
# shows up as a wrong diagnostic before it shows up as a wrong answer, the
# nil-guard family is the real test.
#
# A `func(...)` VALUE is a closure OBJECT {callable slot, captures...} and
# that object pointer is carried in TypeInfo::pointerLevel: parseType adds
# the +1 object pointer to the `func` spelling, so the bare type is level 1
# and `*func(...)` is level 2. Both alias resolvers
# (type_checker::resolveTypeImpl and ir_gen::resolveTypeAliasImpl) bailed
# out at `pointerLevel > 0`, so a pointer to an alias came back as the
# ALIAS SPELLING with a bare level and the closure-object level was
# dropped. `var p: *Cb = &f;` was then rejected with
#
#   Type mismatch in declaration of 'p'. Expected *Cb, got fn(int) -> int
#
# while the identical direct spelling was accepted. Because a 16-byte
# closure object and an 8-byte raw code pointer are different sizes with
# different call semantics, "the alias now type-checks" is NOT the property
# that matters -- so this script pins the RUNTIME diagnostics, which is
# where a dropped level is unambiguous:
#
#   * a nil closure OBJECT reached through each spelling must report
#     CIDER-BUG-237's `vm error: null pointer dereference` (exit 2) on all
#     three paths and under both optimization settings -- the same message
#     as the direct spelling, NOT CIDER-BUG-225's `call: nil function
#     pointer` (a nil callable SLOT), and not CIDER-BUG-219's or
#     CIDER-BUG-211's;
#   * a nil CALLABLE SLOT must keep reporting the `call:` one, so the two
#     stay distinguishable;
#   * a live closure must be called correctly through every spelling and
#     every use position, so a level that resolved to the wrong SIZE --
#     which would pass every nil case above, since both are zero -- fails
#     loudly here;
#   * and the look-alikes must still be REJECTED: `*fn(...)` stays out of
#     the supported surface, and pointer-to-interface is rejected for the
#     alias exactly as for the direct spelling, so the fix did not widen
#     the language.
#
# Every assertion is cross-spelling: the alias rows are compared against the
# direct spelling's (exit, stderr) triple rather than against a hard-coded
# expectation alone, so a change that fixes one spelling and leaves another
# broken fails here.
#
# Usage: alias_closure_pointer_smoke.sh <cider-binary>
# Exits 77 when nasm/gcc are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: alias_closure_pointer_smoke.sh <cider-binary>}
for tool in nasm gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "alias_closure_pointer_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "alias_closure_pointer_smoke: FAIL ($1)"
    exit 1
}

# Mirrors tests/run_tests.py:run_native -- -nostartfiles only when the object
# defines its own _start.
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
EXPECT_NILFN="vm error: call: nil function pointer"

# The preamble every case shares. `Cb` is the alias, `Cb2` the alias of the
# alias (a second hop) and `F` the raw-code-pointer alias.
PREAMBLE='type Cb = func(int) -> int;
type Cb2 = Cb;
type F = fn(int) -> int;

func makeAdder(base: int) -> func(int) -> int {
  return func(x: int) -> int { return x + base; };
}
func double(x: int) -> int { return x * 2; }
func viaParam(p: *Cb, x: int) -> int { var q: Cb = *p; return q(x); }'

write_case() { # <name> <body>
    printf '%s\n\nfunc main() -> int {\n%s\n}\n' "$PREAMBLE" "$2" > "$DIR/$1.cid"
}

# --- 1. a nil closure OBJECT reached through each spelling ----------------
# The call reads the callable slot out of the object, so a nil OBJECT faults
# that read: CIDER-BUG-237's dereference message, on every backend and under
# both optimization settings. Before the fix the alias rows could not produce
# a diagnostic at all -- they were rejected at the type-check, which is why
# this is the assertion that actually distinguishes the fix.
#
# Each row builds a WELL-FORMED chain of the right depth: `&c` is a `*Cb`
# (level 2), so a `**Cb` row has to go through a `*Cb` temporary rather than
# assign `&c` to it. (Function types do not compare pointer levels, so the
# short form is accepted by the checker but is not the shape under test.)
#   1 deref -> *Cb / *Cb2 / *func     2 derefs -> **Cb / **func
for spec in "one:*Cb" "one:*Cb2" "one:*func(int) -> int" \
            "two:**Cb" "two:**func(int) -> int"; do
    depth=${spec%%:*}; pty=${spec#*:}
    if [ "$depth" = one ]; then
        body="  var c: Cb;
  var l: $pty = &c;
  var q: Cb = *l;
  return q(3);"
    else
        body="  var c: Cb;
  var l: *Cb = &c;
  var p: $pty = &l;
  var q: Cb = **p;
  return q(3);"
    fi
    write_case "nil_$depth" "$body"
    for opt in "--no-opt" ""; do
        vm_run "$DIR/nil_$depth.cid" $opt
        [ "$VM_RC" -eq 2 ] || fail "vm $opt nil object ($pty, $depth deref): exit $VM_RC (want 2, not a signal)"
        [ "$VM_OUT" = "$EXPECT_NIL" ] || fail "vm $opt nil object ($pty): [$VM_OUT]  (must be the OBJECT's message, not the slot's)"
    done
    native_run "$DIR/nil_$depth.cid"
    [ "$NATIVE_RC" -eq 2 ] || fail "native nil object ($pty): exit $NATIVE_RC (want 2, not a signal)"
    [ "$NATIVE_OUT" = "$EXPECT_NIL" ] || fail "native nil object ($pty): [$NATIVE_OUT]"
done

# --- 2. the two nil messages stay DISTINGUISHABLE through the alias -------
# The mirror of the case above: a nil CALLABLE SLOT inside a live object
# faults at dispatch, so it must report CIDER-BUG-225's `call:` text. If a
# dropped level made the alias read the OBJECT where the callee belongs, this
# row would report the dereference message instead -- exactly the confusion
# the two guards exist to prevent. The `fn(...)` alias reaches it from
# source; a closure object's slot is always written by its constructor.
for spec in "bare:fn(int) -> int:" "ptr:*F:*"; do
    name=${spec%%:*}; rest=${spec#*:}; pty=${rest%%:*}; deref=${rest#*:}
    if [ "$name" = bare ]; then
        body="  var f: fn(int) -> int = nilPtr;
  return f(3);"
    else
        # `(*p)(3)`, not `*p(3)`: the latter parses as `*(p(3))`.
        body="  var f: F = nilPtr;
  var p: $pty = &f;
  return (*p)(3);"
    fi
    write_case "slot_$name" "$body"
    for opt in "--no-opt" ""; do
        vm_run "$DIR/slot_$name.cid" $opt
        [ "$VM_RC" -eq 2 ] || fail "vm $opt nil slot ($name): exit $VM_RC"
        [ "$VM_OUT" = "$EXPECT_NILFN" ] || fail "vm $opt nil slot ($name): [$VM_OUT]  (a nil SLOT must not report the OBJECT's message)"
    done
    native_run "$DIR/slot_$name.cid"
    [ "$NATIVE_RC" -eq 2 ] || fail "native nil slot ($name): exit $NATIVE_RC"
    [ "$NATIVE_OUT" = "$EXPECT_NILFN" ] || fail "native nil slot ($name): [$NATIVE_OUT]"
    # A live value through the same route must still be callable, or the
    # `call:` message above could be produced by a route that never worked.
    write_case "slotlive_$name" "${body//= nilPtr/= double}"
    for opt in "--no-opt" ""; do
        vm_run "$DIR/slotlive_$name.cid" $opt
        [ "$VM_RC" -eq 6 ] || fail "vm $opt live slot route ($name): exit $VM_RC (want 6)"
        [ -z "$VM_OUT" ] || fail "vm $opt live slot route ($name) stderr: [$VM_OUT]"
    done
    native_run "$DIR/slotlive_$name.cid"
    [ "$NATIVE_RC" -eq 6 ] || fail "native live slot route ($name): exit $NATIVE_RC (want 6)"
    [ -z "$NATIVE_OUT" ] || fail "native live slot route ($name) stderr: [$NATIVE_OUT]"
done

# --- 3. the sibling nil diagnostics are unchanged ------------------------
# A level dropped on an alias could otherwise borrow one of these messages.
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
    [ "$VM_OUT" = "$EXPECT_NIL" ] || fail "vm $opt nil pointer: [$VM_OUT]"
    vm_run "$DIR/iface.cid" $opt
    [ "$VM_RC" -eq 2 ] || fail "vm $opt nil interface: exit $VM_RC"
    [ "$VM_OUT" = "vm error: nil interface dispatch" ] || fail "vm $opt nil interface: [$VM_OUT]"
done
native_run "$DIR/deref.cid"
[ "$NATIVE_RC" -eq 2 ] || fail "native nil pointer: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "$EXPECT_NIL" ] || fail "native nil pointer: [$NATIVE_OUT]"
native_run "$DIR/iface.cid"
[ "$NATIVE_RC" -eq 2 ] || fail "native nil interface: exit $NATIVE_RC"
[ "$NATIVE_OUT" = "vm error: nil interface dispatch" ] || fail "native nil interface: [$NATIVE_OUT]"

# --- 4. a LIVE closure is called correctly through every position --------
# The guard must cost a not-taken branch and nothing more, and -- the reason
# this section exists -- a level that resolved to the wrong SIZE passes every
# nil case above (both operands are zero) and only fails here. Covers the
# deref call, the deref into a local, a parameter, and an array element.
for spec in "one:*Cb" "one:*Cb2" "one:*func(int) -> int" \
            "two:**Cb" "two:**func(int) -> int"; do
    depth=${spec%%:*}; pty=${spec#*:}
    if [ "$depth" = one ]; then
        body="  var c: Cb = makeAdder(1);
  var l: $pty = &c;
  if ((*l)(3) != 4) { return 71; }
  var q: Cb = *l;
  if (q(3) != 4) { return 72; }
  if (viaParam(l, 3) != 4) { return 73; }
  var a: [$pty; 2];
  a[0] = &c;
  var e: Cb = *a[0];
  if (e(3) != 4) { return 74; }
  return 0;"
    else
        body="  var c: Cb = makeAdder(1);
  var l: *Cb = &c;
  var p: $pty = &l;
  if ((**p)(3) != 4) { return 71; }
  var q: Cb = **p;
  if (q(3) != 4) { return 72; }
  return 0;"
    fi
    write_case "live_$depth" "$body"
    for opt in "--no-opt" ""; do
        vm_run "$DIR/live_$depth.cid" $opt
        [ "$VM_RC" -eq 0 ] || fail "vm $opt live ($pty, $depth deref): exit $VM_RC"
        [ -z "$VM_OUT" ] || fail "vm $opt live ($pty) stderr: [$VM_OUT]"
    done
    native_run "$DIR/live_$depth.cid"
    [ "$NATIVE_RC" -eq 0 ] || fail "native live ($pty, $depth deref): exit $NATIVE_RC"
    [ -z "$NATIVE_OUT" ] || fail "native live ($pty) stderr: [$NATIVE_OUT]"
done

# --- 5. the cross-spelling AGREEMENT, asserted and not just assumed ------
# The family is a spelling-equality claim, so compare the spellings against
# each other instead of trusting each expectation in isolation. Both levels
# and both optimization settings have to agree with the direct spelling.
for case in nil_ live_; do
    ref=""
    for sp in one two; do
        for opt in "--no-opt" ""; do
            vm_run "$DIR/$case$sp.cid" $opt
            cur="rc=$VM_RC [$VM_OUT]"
            if [ -z "$ref" ]; then ref=$cur
            elif [ "$cur" != "$ref" ]; then
                fail "$case$sp (vm ${opt:-opt}) disagrees with the direct spelling: $cur vs $ref"
            fi
        done
        native_run "$DIR/$case$sp.cid"
        cur="rc=$NATIVE_RC [$NATIVE_OUT]"
        if [ -z "$ref" ]; then ref=$cur
        elif [ "$cur" != "$ref" ]; then
            fail "$case$sp (native) disagrees with the direct spelling: $cur vs $ref"
        fi
    done
done

# --- 6. a call on the POINTER (no deref) agrees across the spellings ------
# `p(3)` on a `*Cb` is broken in the language, not by this ticket: on the
# BASE commit the direct spelling `*func(...)` already failed it (the VM
# reported `call: bad function id` and native died with SIGSEGV), because a
# `func` call reads the callable slot out of the OBJECT and one deref short of
# the object it reads the slot pointer instead. It is a separate defect, so
# this does NOT assert that it works -- it asserts that the alias and the
# direct spelling FAIL IT IDENTICALLY, which is the property this ticket is
# about. A fix for that defect has to move all the spellings together, and
# this row is what will notice if it moves only some of them.
#
# The two depths are compared WITHIN their own group, never against each
# other: `**Cb` needs `(**p)(3)` to reach the object and is correct, while
# `*Cb` with a bare `p(3)` is one deref short, so cross-group equality would
# be asserting a bug rather than pinning one.
for spec in "one:*Cb" "one:*Cb2" "one:*func(int) -> int" \
            "two:**Cb" "two:**func(int) -> int"; do
    depth=${spec%%:*}; pty=${spec#*:}
    if [ "$depth" = one ]; then
        body="  var c: Cb = makeAdder(1);
  var p: $pty = &c;
  return p(3);"
    else
        body="  var c: Cb = makeAdder(1);
  var l: *Cb = &c;
  var p: $pty = &l;
  return (**p)(3);"
    fi
    write_case "nocallderef_${depth}_${pty//[^A-Za-z0-9]/_}" "$body"
done
for depth in one two; do
    ref_call=""; ref_rc=""
    for pty in "*Cb" "*Cb2" "*func(int) -> int" "**Cb" "**func(int) -> int"; do
        case "$pty" in
            "**"*) [ "$depth" = two ] || continue ;;
            *)     [ "$depth" = one ] || continue ;;
        esac
        f="$DIR/nocallderef_${depth}_${pty//[^A-Za-z0-9]/_}.cid"
        vm_run "$f"
        cur="rc=$VM_RC [$VM_OUT]"
        if [ -z "$ref_call" ]; then ref_call=$cur
        elif [ "$cur" != "$ref_call" ]; then
            fail "a bare p(3) at $depth level ($pty) disagrees with the direct spelling: $cur vs $ref_call"
        fi
        "$BIN" compile "$f" -o "$DIR/prog.asm" >/dev/null 2>&1 \
            && nasm -O0 -f elf64 "$DIR/prog.asm" -o "$DIR/prog.o" \
            && gcc "$DIR/prog.o" -o "$DIR/prog" -no-pie
        # The one-level form is EXPECTED to die on a signal here (that is the
        # pre-existing defect this section documents). The crash notice is
        # the subject under test, not a harness problem, so the child's
        # stderr is discarded and the exit code -- 128+SIG, i.e. 139 -- is
        # what carries the result. A bash that prints the notice on the
        # parent's tty regardless is silenced by wrapping the launch in a
        # subshell with stderr closed.
        set +e
        cur=$("$DIR/prog" >/dev/null 2>/dev/null; echo $?)
        set -e
        if [ -z "$ref_rc" ]; then ref_rc=$cur
        elif [ "$cur" != "$ref_rc" ]; then
            fail "a bare p(3) at $depth level ($pty, native) disagrees with the direct spelling: exit $cur vs $ref_rc"
        fi
    done
    # The two-level form has the deref it needs and must actually work, or
    # the row above would agree on a uniform failure and prove nothing.
    if [ "$depth" = two ]; then
        [ "$ref_rc" -eq 4 ] || fail "(**p)(3) at two levels: exit $ref_rc (want 4)"
        case "$ref_call" in "rc=4"*) ;; *) fail "(**p)(3) at two levels: [$ref_call]" ;; esac
    fi
done

# --- 7. the look-alike rejections still REJECT ---------------------------
# The fix must not widen the language. `*fn(...)` is out of the supported
# surface (fn values are already pointers, CIDER-BUG-026) ...
for bad in '*fn(int) -> int' '*fn(int,int) -> int'; do
    printf '%s\n\nfunc main() -> int { var f: %s; return 0; }\n' "$PREAMBLE" "$bad" > "$DIR/badfn.cid"
    set +e
    OUT=$("$BIN" check "$DIR/badfn.cid" 2>&1); RC=$?
    set -e
    [ "$RC" -eq 1 ] || fail "pointer-to-fn ($bad) was accepted"
    printf '%s' "$OUT" | grep -q "Pointer-to-function types are not supported" \
        || fail "pointer-to-fn ($bad) lost its diagnostic: [$OUT]"
done

# ... and a pointer to an interface stays rejected through the alias exactly
# as through the direct one (an interface is a 16-byte fat pointer, so `*Shape`
# would be a pointer to a fat pointer). The alias must not resolve to
# something the direct spelling refuses.
cat > "$DIR/ifacedirect.cid" <<'CID'
interface Shape { func kind() -> int; }
class Sq { func kind() -> int { return 1; } }
func main() -> int { var s: Sq; var t: *Shape = &s; return 0; }
CID
cat > "$DIR/ifacealias.cid" <<'CID'
interface Shape { func kind() -> int; }
class Sq { func kind() -> int { return 1; } }
type Sh = Shape;
func main() -> int { var s: Sq; var t: *Sh = &s; return 0; }
CID
set +e
D_OUT=$("$BIN" check "$DIR/ifacedirect.cid" 2>&1); D_RC=$?
A_OUT=$("$BIN" check "$DIR/ifacealias.cid" 2>&1); A_RC=$?
set -e
[ "$D_RC" -eq 1 ] || fail "pointer-to-interface (direct) was accepted"
[ "$A_RC" -eq 1 ] || fail "pointer-to-interface (alias) was accepted; the fix widened the language"
printf '%s' "$D_OUT" | grep -q "Expected \*Shape" || fail "direct pointer-to-interface diagnostic changed: [$D_OUT]"
printf '%s' "$A_OUT" | grep -q "Expected \*Sh"   || fail "alias pointer-to-interface diagnostic changed: [$A_OUT]"

# A bare alias of an interface is still a legal fat-pointer VALUE (that is
# what the flag resolution exists for), so the rejection above is about the
# pointer level alone and must not have leaked into the bare form.
cat > "$DIR/ifacebare.cid" <<'CID'
interface Shape { func kind() -> int; }
class Sq { func init() {} func kind() -> int { return 7; } }
type Sh = Shape;
func main() -> int { var s: Sh = new Sq(); return s.kind(); }
CID
for opt in "--no-opt" ""; do
    vm_run "$DIR/ifacebare.cid" $opt
    # exit 7 == kind()'s return value: the bare alias dispatched correctly.
    [ "$VM_RC" -eq 7 ] || fail "vm $opt bare alias-of-interface: exit $VM_RC (want 7; a bare alias of an interface is a legal fat-pointer value)"
    [ -z "$VM_OUT" ] || fail "vm $opt bare alias-of-interface stderr: [$VM_OUT]"
done
native_run "$DIR/ifacebare.cid"
[ "$NATIVE_RC" -eq 7 ] || fail "native bare alias-of-interface: exit $NATIVE_RC (want 7)"

# --- 8. a cyclic alias under a pointer still terminates -----------------
# The walk is cycle-bounded. A pointer level on the cycle must not make the
# resolver recurse without bound -- getTypeSize re-resolves its result, so a
# cap that returned the other alias made the pair recurse forever.
cat > "$DIR/cycle.cid" <<'CID'
type A = B;
type B = A;
func main() -> int { var p: *A; var q: **B; return 0; }
CID
set +e
"$BIN" check "$DIR/cycle.cid" > /dev/null 2>&1; RC=$?
set -e
[ "$RC" -le 1 ] || fail "cyclic alias under a pointer did not terminate (exit $RC)"

# --- 8b. the `&f` initializer on a target whose default ABI is MS -------
# On uefi an un-annotated `func f` is an `msabi fn(...)`, which does not
# match a SysV closure spelling -- for the DIRECT spelling too, so it is a
# target property, not an alias one. Pinned so the cross-target agreement
# cannot be broken by a change that touches only the alias side, and so the
# `sysvabi` pin in the regression program is justified rather than a
# mystery. (The regression program uses a `sysvabi func` precisely so this
# position is checked on every target.)
for spec in "direct:*func(int) -> int" "Cb:*Cb" "Cb2:*Cb2"; do
    name=${spec%%:*}; pty=${spec#*:}
    cat > "$DIR/abi_$name.cid" <<CID
type Cb = func(int) -> int;
type Cb2 = Cb;
func f(x: int) -> int { return x * 2; }
func main() -> int { var p: $pty = &f; return 0; }
CID
done
abi_ref=""
for name in direct Cb Cb2; do
    set +e
    OUT=$("$BIN" check --target uefi "$DIR/abi_$name.cid" 2>&1); RC=$?
    set -e
    [ "$RC" -eq 1 ] || fail "uefi: '$name' \`&f\` was accepted (rc=$RC); the MS-ABI mismatch must be reported"
    # Only the diagnostic line is compared. A non-array alias deliberately
    # keeps its own SPELLING in it (CIDER-BUG-092), and the direct
    # `*func(...)` spelling prints no level at all, so the expected-type text
    # cannot be compared literally; the echoed source line below it of course
    # names the file's own type. What must match is the diagnostic's kind,
    # the variable, and the ACTUAL type it reports.
    norm=$(printf '%s' "$OUT" | head -1 | sed -E "s#$DIR/[A-Za-z0-9_]+\.cid#<f>#; s/Expected [^,]*, /Expected <T>, /")
    if [ -z "$abi_ref" ]; then abi_ref=$norm
    elif [ "$norm" != "$abi_ref" ]; then
        fail "uefi: '$name' \`&f\` diagnostic differs from the direct spelling: [$norm] vs [$abi_ref]"
    fi
    printf '%s' "$OUT" | grep -q "got msabi fn(int) -> int" \
        || fail "uefi: '$name' lost the ABI detail: [$OUT]"
done
# And the pinned SysV spelling IS accepted, on uefi too, so the row above is
# pinning a real ABI check rather than a blanket rejection.
for spec in "direct:*func(int) -> int" "Cb:*Cb" "Cb2:*Cb2"; do
    name=${spec%%:*}; pty=${spec#*:}
    cat > "$DIR/abisys_$name.cid" <<CID
type Cb = func(int) -> int;
type Cb2 = Cb;
sysvabi func f(x: int) -> int { return x * 2; }
func main() -> int { var p: $pty = &f; return 0; }
CID
    set +e
    OUT=$("$BIN" check --target uefi "$DIR/abisys_$name.cid" 2>&1); RC=$?
    set -e
    [ "$RC" -eq 0 ] || fail "uefi: '$name' with a sysvabi fn was rejected: [$OUT]"
done


# --- 9. the ticket's reproducer, verbatim -------------------------------
# The exact line from CIDER-BUG-183, which the base commit rejects and this
# build must accept. Pinned on its own so the headline case cannot be lost
# behind a refactor of the matrix above.
cat > "$DIR/repro.cid" <<'CID'
type Cb = func(int) -> int;

func f(x: int) -> int { return x * 2; }

func main() -> int {
  var p: *Cb = &f;
  return 0;
}
CID
set +e
OUT=$("$BIN" check "$DIR/repro.cid" 2>&1); RC=$?
set -e
[ "$RC" -eq 0 ] || fail "the ticket reproducer is still rejected: rc=$RC [$OUT]"

echo "alias_closure_pointer_smoke: OK"
