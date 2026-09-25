#!/bin/bash
# CID-005 fix round 3 regression: MS-ABI definitions must preserve all 128
# bits of every callee-saved XMM register they use. The pre-fix prologue
# saved each with `movq` into an 8-byte slot, so an MS-ABI caller's upper
# halves came back zeroed after the body used the register. This drives a
# Cider `--target uefi` definition from a NASM MS-ABI caller holding
# full-width sentinels (distinct upper/lower halves) in xmm8-xmm15.
#
# Usage: ms_abi_xmm_smoke.sh <cider-binary>
# Exits 77 when nasm/objcopy/ld are missing (no compiled-toolchain SKIP).
set -euo pipefail

BIN=${1:?usage: ms_abi_xmm_smoke.sh <cider-binary>}
for tool in nasm objcopy ld; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ms_abi_xmm_smoke: SKIP ($tool not found)"
        exit 77
    fi
done

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

cat > "$DIR/lib.cid" <<'CID'
func bump(f: float) -> float { var x: float = f; x = x + 1.0; return x; }
func f3(a: float, b: float, c: float) -> float { return a + b + c; }
func main() -> int { return 0; }
CID

cat > "$DIR/harness.asm" <<'ASM'
; MS-ABI caller for the Cider definitions: bump saves 4 XMM registers,
; f3 saves 5 (odd count); both must round-trip all 16 bytes.
default rel
section .data
; Upper and lower halves are distinct in every sentinel.
sent8:  dq 0x1111111111111118, 0xAAAAAAAAAAAAAAA8
sent9:  dq 0x2222222222222229, 0xBBBBBBBBBBBBBBB9
sent10: dq 0x333333333333333A, 0xCCCCCCCCCCCCCCCA
sent11: dq 0x444444444444444B, 0xDDDDDDDDDDDDDDDB
sent12: dq 0x555555555555555C, 0xEEEEEEEEEEEEEEEC
sent13: dq 0x666666666666666D, 0xFFFFFFFFFFFFFFFD
sent14: dq 0x777777777777777E, 0x0123456789ABCDEE
sent15: dq 0x888888888888888F, 0xFEDCBA987654320F
f20:    dd 0x40000000          ; 2.0f
f125:   dd 0x3FA00000          ; 1.25f
f25:    dd 0x40200000          ; 2.5f
f025:   dd 0x3E800000          ; 0.25f

section .text
extern bump
extern f3
global _start

_start:
    call load_sentinels
    movss xmm0, [rel f20]
    call bump
    movd eax, xmm0
    cmp eax, 0x40400000          ; 2.0f + 1.0f = 3.0f
    jne .bad_bump
    call check_xmm
    test eax, eax
    jne .bad_bump_xmm

    call load_sentinels
    movss xmm0, [rel f125]
    movss xmm1, [rel f25]
    movss xmm2, [rel f025]
    call f3
    movd eax, xmm0
    cmp eax, 0x40800000          ; 1.25f + 2.5f + 0.25f = 4.0f
    jne .bad_f3
    call check_xmm
    test eax, eax
    jne .bad_f3_xmm

    xor edi, edi
    jmp .exit
.bad_bump:     mov edi, 1
    jmp .exit
.bad_bump_xmm: mov edi, 2
    jmp .exit
.bad_f3:       mov edi, 3
    jmp .exit
.bad_f3_xmm:   mov edi, 4
.exit:
    mov eax, 60
    syscall

; Full-width loads: `movsd xmm8, [mem]` would zero the upper halves.
load_sentinels:
    movdqu xmm8, [rel sent8]
    movdqu xmm9, [rel sent9]
    movdqu xmm10, [rel sent10]
    movdqu xmm11, [rel sent11]
    movdqu xmm12, [rel sent12]
    movdqu xmm13, [rel sent13]
    movdqu xmm14, [rel sent14]
    movdqu xmm15, [rel sent15]
    ret

; check_xmm: full 16-byte compare of xmm8-xmm15 against the sentinels.
; Uses xmm0 as scratch; returns 0 when all survive, else the failing
; register number (8-15).
check_xmm:
    movdqu xmm0, [rel sent8]
    pcmpeqb xmm0, xmm8
    pmovmskb eax, xmm0
    cmp eax, 0xFFFF
    jne .ret8
    movdqu xmm0, [rel sent9]
    pcmpeqb xmm0, xmm9
    pmovmskb eax, xmm0
    cmp eax, 0xFFFF
    jne .ret9
    movdqu xmm0, [rel sent10]
    pcmpeqb xmm0, xmm10
    pmovmskb eax, xmm0
    cmp eax, 0xFFFF
    jne .ret10
    movdqu xmm0, [rel sent11]
    pcmpeqb xmm0, xmm11
    pmovmskb eax, xmm0
    cmp eax, 0xFFFF
    jne .ret11
    movdqu xmm0, [rel sent12]
    pcmpeqb xmm0, xmm12
    pmovmskb eax, xmm0
    cmp eax, 0xFFFF
    jne .ret12
    movdqu xmm0, [rel sent13]
    pcmpeqb xmm0, xmm13
    pmovmskb eax, xmm0
    cmp eax, 0xFFFF
    jne .ret13
    movdqu xmm0, [rel sent14]
    pcmpeqb xmm0, xmm14
    pmovmskb eax, xmm0
    cmp eax, 0xFFFF
    jne .ret14
    movdqu xmm0, [rel sent15]
    pcmpeqb xmm0, xmm15
    pmovmskb eax, xmm0
    cmp eax, 0xFFFF
    jne .ret15
    xor eax, eax
    ret
.ret8:  mov eax, 8
    ret
.ret9:  mov eax, 9
    ret
.ret10: mov eax, 10
    ret
.ret11: mov eax, 11
    ret
.ret12: mov eax, 12
    ret
.ret13: mov eax, 13
    ret
.ret14: mov eax, 14
    ret
.ret15: mov eax, 15
    ret
ASM

"$BIN" compile --target uefi "$DIR/lib.cid" -o "$DIR/lib.asm"
nasm -O0 -f elf64 "$DIR/lib.asm" -o "$DIR/lib.o"
objcopy --globalize-symbol=bump --globalize-symbol=f3 \
        --localize-symbol=main "$DIR/lib.o" "$DIR/lib.g.o"
nasm -O0 -f elf64 "$DIR/harness.asm" -o "$DIR/harness.o"
ld "$DIR/harness.o" "$DIR/lib.g.o" -o "$DIR/smoke"

if "$DIR/smoke"; then
    echo "ms_abi_xmm_smoke: OK"
else
    echo "ms_abi_xmm_smoke: FAIL (exit $?)"
    exit 1
fi
