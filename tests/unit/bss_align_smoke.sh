#!/bin/bash
# CIDER-BUG-132: .bss globals of heterogeneous alignment used to be padded
# with NASM's `align` macro, which fills with `nop` (initialized data) and
# made NASM warn "attempt to initialize memory in BSS section" once per
# padded global. Codegen now pads .bss with `alignb` (fill via `resb 1`).
# This smoke asserts (a) nasm assembles such a module warning-free and
# (b) the padding keeps the exact same symbol offsets in .bss.
#
# CIDER-BUG-142: a top-level user `asm {}` block used to be printed in the
# .bss context too, so a label + `align`/`dq` there warned "attempt to
# initialize memory in BSS section" and its code bytes never made it into a
# live section. The second half asserts the block now lands in .text and
# that its code really executes.
#
# CIDER-BUG-152: .text is not a BSS section, so NASM's uninitialized-space
# directives (`resb`/`resw`/`resd`/`resq`) have no legal context there. The
# policy is: file-scope data uses `db`/`dq`, or a block brackets itself into
# .bss and back. The third part pins all three halves of that policy — the
# understood `res*`-in-.text warning, the warning-free .bss bracket, and the
# .text re-anchor that keeps a block from swallowing the generated code.
#
# Usage: bss_align_smoke.sh <cider-binary>
# Exits 77 when nasm/nm/gcc are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: bss_align_smoke.sh <cider-binary>}
for tool in nasm nm gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "bss_align_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

fail() {
    echo "bss_align_smoke: FAIL ($1)"
    exit 1
}

# Same shape as tests/programs/bss_align_mixed: u8 -> u32 (align 4 padding)
# -> u64 -> u8 -> u64 (align 8 padding), so both padding widths are covered.
cat > "$DIR/bss.cid" <<'CID'
var g8: u8 = 0;
var g32: u32 = 0;
var g64: u64 = 0;
var tail: u8 = 0;
var g64b: u64 = 0;

func main() -> int {
  g8 = 0xF0;
  g32 = 0x12345678;
  g64 = 0x1122334455667788;
  tail = 0x5A;
  g64b = 0x8877665544332211;
  return 0;
}
CID

"$BIN" compile "$DIR/bss.cid" -o "$DIR/bss.asm" \
    || fail "cider compile failed"

NASM_ERR=$(nasm -O0 -f elf64 "$DIR/bss.asm" -o "$DIR/bss.o" 2>&1 >/dev/null) \
    || fail "nasm failed: $NASM_ERR"
if echo "$NASM_ERR" | grep -q "attempt to initialize memory"; then
    fail "nasm warned about initialized data in .bss: $NASM_ERR"
fi

# Offsets pin the padding: 3 bytes of align-4 after g8, 7 bytes of align-8
# after tail. Any change in padding direction/size shifts these.
check_sym() { # <name> <expected-hex-offset>
    local got
    got=$(nm "$DIR/bss.o" | awk -v s="$1" '$3 == s { print $1 }')
    [ "$got" = "$2" ] || fail "$1 at 0x${got:-?}, expected 0x$2"
}

check_sym g8   0000000000000000
check_sym g32  0000000000000004
check_sym g64  0000000000000008
check_sym tail 0000000000000010
check_sym g64b 0000000000000018

# CIDER-BUG-142: top-level user asm is file-scope code/data. It must be
# emitted in .text (not .bss), must not warn, and its label must be callable:
# main returns the value user_fn puts in eax, so a mis-sectioned or ignored
# block cannot produce exit 42.
cat > "$DIR/user_asm.cid" <<'CID'
extern func user_fn() -> int;

func main() -> int {
  return user_fn();
}

asm {
  "user_fn:"
  "mov eax, 42"
  "ret"
}

asm {
  "align 16"
  "user_data: dq 0x1122334455667788"
}
CID

"$BIN" compile "$DIR/user_asm.cid" -o "$DIR/user_asm.asm" \
    || fail "cider compile (user asm) failed"

NASM_ERR=$(nasm -O0 -f elf64 "$DIR/user_asm.asm" -o "$DIR/user_asm.o" 2>&1 >/dev/null) \
    || fail "nasm failed (user asm): $NASM_ERR"
if echo "$NASM_ERR" | grep -q "attempt to initialize memory"; then
    fail "user asm block emitted initialized data in .bss: $NASM_ERR"
fi

# nm's type letter: lowercase = local. `t`/`T` is .text; a `b` here means the
# block slid back into .bss, `d` means .data.
check_text_sym() { # <name>
    local got
    got=$(nm "$DIR/user_asm.o" | awk -v s="$1" '$3 == s { print $2 }')
    case "$got" in
        t|T) ;;
        *) fail "user asm symbol $1 in section type '${got:-missing}', want .text (t/T)" ;;
    esac
}
check_text_sym user_fn
check_text_sym user_data

gcc "$DIR/user_asm.o" -o "$DIR/user_asm" -no-pie || fail "link (user asm) failed"
set +e
"$DIR/user_asm"
USER_ASM_RC=$?
set -e
[ "$USER_ASM_RC" -eq 42 ] || fail "user asm body exited $USER_ASM_RC, want 42"

# ---------------------------------------------------------------- BUG-152 ---
# CIDER-BUG-152: `res*` at file scope. Under the .text emission above, NASM
# has no legal context for uninitialized space, so it warns while still
# emitting the zeroed space. That space is read-only, so this form is
# documented-but-avoided rather than useful: the test pins the warning so it
# is an understood outcome instead of an accident, and pins the two forms
# that are actually usable — `db`/`dq`, and a block that brackets itself
# into .bss and back.
sym_type() { # <object> <name>
    nm "$1" | awk -v s="$2" '$3 == s { print $2 }'
}
check_sym_type() { # <object> <name> <t|T> <label>
    local got upper
    got=$(sym_type "$1" "$2")
    upper=$(printf '%s' "$3" | tr '[:lower:]' '[:upper:]')
    [ "$got" = "$3" ] || [ "$got" = "$upper" ] || \
        fail "$4: symbol $2 has type '${got:-missing}', want $3/$upper"
}
run_rc() { # <binary> -> echoes exit code
    set +e
    "$1"
    local rc=$?
    set -e
    echo "$rc"
}

# (a) The understood case: `resq` in .text. Must still assemble, and the
# space must be emitted and zero-initialized. It is read-only (it lives in
# the R E .text mapping), so this probes a read, never a store.
cat > "$DIR/res_text.cid" <<'CID'
extern func probe_res() -> int;

func main() -> int {
  return probe_res();
}

asm {
  "buf_res:"
  "resq 4"
  "probe_res:"
  "mov eax, [rel buf_res]"
  "test eax, eax"
  "jnz .res_notzero"
  "xor eax, eax"
  "ret"
  ".res_notzero:"
  "mov eax, 1"
  "ret"
}
CID

"$BIN" compile "$DIR/res_text.cid" -o "$DIR/res_text.asm" \
    || fail "cider compile (res* in .text) failed"

NASM_ERR=$(nasm -O0 -f elf64 "$DIR/res_text.asm" -o "$DIR/res_text.o" 2>&1 >/dev/null) \
    || fail "nasm failed (res* in .text): $NASM_ERR"
# The documented warning, and nothing else: no BSS-initialization warning
# (BUG-142 regression) and no assembler error.
if ! echo "$NASM_ERR" | grep -q "uninitialized space declared in non-BSS section"; then
    fail "missing the documented zeroing warning for res* in .text: ${NASM_ERR:-<none>}"
fi
if echo "$NASM_ERR" | grep -q "attempt to initialize memory"; then
    fail "res* block emitted initialized data in .bss: $NASM_ERR"
fi
NONZERO=$(echo "$NASM_ERR" | grep -vc "uninitialized space declared in non-BSS section" || true)
[ "$NONZERO" -eq 0 ] || fail "unexpected nasm diagnostics for res* in .text: $NASM_ERR"

check_sym_type "$DIR/res_text.o" buf_res t "res* in .text"
check_sym_type "$DIR/res_text.o" probe_res t "res* in .text"
gcc "$DIR/res_text.o" -o "$DIR/res_text" -no-pie || fail "link (res* in .text) failed"
[ "$(run_rc "$DIR/res_text")" -eq 0 ] \
    || fail "res* in .text: space not emitted/zero-initialized (want 0)"

# (b) The warning-free alternative: bracket the storage into .bss. Must
# assemble with no diagnostics at all, the data must really be BSS, and it
# must be writable — the whole reason to prefer this form.
cat > "$DIR/res_bss.cid" <<'CID'
extern func probe_bss() -> int;

func main() -> int {
  return probe_bss();
}

asm {
  "section .bss"
  "buf_bss:"
  "resq 4"
  "section .text"
  "probe_bss:"
  "mov eax, [rel buf_bss]"
  "test eax, eax"
  "jnz .bss_notzero"
  "mov qword [rel buf_bss], 7"
  "mov eax, [rel buf_bss]"
  "ret"
  ".bss_notzero:"
  "mov eax, 1"
  "ret"
}
CID

"$BIN" compile "$DIR/res_bss.cid" -o "$DIR/res_bss.asm" \
    || fail "cider compile (res* in .bss bracket) failed"

NASM_ERR=$(nasm -O0 -f elf64 "$DIR/res_bss.asm" -o "$DIR/res_bss.o" 2>&1 >/dev/null) \
    || fail "nasm failed (res* in .bss bracket): $NASM_ERR"
[ -z "$NASM_ERR" ] || fail "bracketed .bss storage must be warning-free: $NASM_ERR"

check_sym_type "$DIR/res_bss.o" buf_bss b "res* in .bss bracket"
check_sym_type "$DIR/res_bss.o" probe_bss t "res* in .bss bracket"
gcc "$DIR/res_bss.o" -o "$DIR/res_bss" -no-pie || fail "link (res* in .bss bracket) failed"
[ "$(run_rc "$DIR/res_bss")" -eq 7 ] \
    || fail "res* in .bss bracket: not zeroed/writable (want 7)"

# (c) The bracket is safe even when the block forgets to switch back. Without
# the compiler's .text re-anchor, every generated line below inherits .bss:
# NASM discards the code and the binary has no entry point at all.
cat > "$DIR/res_stray.cid" <<'CID'
func main() -> int {
  return 42;
}

asm {
  "section .bss"
  "buf_stray:"
  "resq 4"
}
CID

"$BIN" compile "$DIR/res_stray.cid" -o "$DIR/res_stray.asm" \
    || fail "cider compile (unterminated .bss bracket) failed"

NASM_ERR=$(nasm -O0 -f elf64 "$DIR/res_stray.asm" -o "$DIR/res_stray.o" 2>&1 >/dev/null) \
    || fail "nasm failed (unterminated .bss bracket): $NASM_ERR"
if echo "$NASM_ERR" | grep -q "attempt to initialize memory"; then
    fail "generated code was emitted in .bss: $NASM_ERR"
fi
check_sym_type "$DIR/res_stray.o" buf_stray b "unterminated .bss bracket"
check_sym_type "$DIR/res_stray.o" main t "unterminated .bss bracket"
gcc "$DIR/res_stray.o" -o "$DIR/res_stray" -no-pie \
    || fail "link (unterminated .bss bracket) failed"
[ "$(run_rc "$DIR/res_stray")" -eq 42 ] \
    || fail "generated code lost to .bss (want 42)"

echo "bss_align_smoke: OK"
