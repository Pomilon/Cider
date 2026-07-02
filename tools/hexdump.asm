section .bss
    O_RDONLY: resd 1

section .data
    str0: db 10, 0
    str1: db `0`, 0
    str2: db `-`, 0
    str3: db `hexdump: missing filename`, 10, 0
    str4: db `:`, 10, 0
    str5: db `hexdump: cannot open: `, 0
    str6: db 10, 0
    O_WRONLY: dd 1
    O_RDWR: dd 2
    O_CREAT: dd 64
    O_TRUNC: dd 512
    O_APPEND: dd 1024
    _gs0: db '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f', 0
    HEX: dq _gs0

section .text
    extern malloc
    extern _setjmp
    extern longjmp
strlen:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 96
.entry:
    push rdi
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov rax, r14
    mov qword [r15], rax
    lea r10, [rbp - 64]
    mov r13, r10
    add r13, 0
    mov eax, 0
    mov dword [r13], eax
    lea r10, [rbp - 72]
    mov r12, r10
    mov rax, qword [r15 + 0]
    mov rbx, rax
    add r12, 0
    mov rax, rbx
    mov qword [r12], rax
    jmp .strlen_while_header0
.strlen_while_header0:
    mov rax, qword [r12 + 0]
    mov qword [rbp - 80], rax
    mov r10, [rbp - 80]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 88], al
    mov byte [rbp - 96], 0
    and byte [rbp - 96], 255
    mov al, byte [rbp - 88]
    mov r11b, byte [rbp - 96]
    cmp al, r11b
    setne al
    movzx rax, al
    mov [rbp - 104], rax
    mov rax, [rbp - 104]
    cmp rax, 0
    jne .strlen_while_body1
    jmp .strlen_while_exit2
.strlen_while_body1:
    mov eax, dword [r13 + 0]
    mov dword [rbp - 112], eax
    mov eax, dword [rbp - 112]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 120], eax
    add r13, 0
    mov eax, dword [rbp - 120]
    mov dword [r13], eax
    mov rax, qword [r12 + 0]
    mov qword [rbp - 128], rax
    mov rax, qword [rbp - 128]
    mov r11, 1
    add rax, r11
    mov qword [rbp - 136], rax
    add r12, 0
    mov rax, qword [rbp - 136]
    mov qword [r12], rax
    jmp .strlen_while_header0
.strlen_while_exit2:
    mov eax, dword [r13 + 0]
    mov dword [rbp - 144], eax
    mov eax, dword [rbp - 144]
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.strlen_dead3:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
printChar:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 64
.entry:
    push rdi
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov al, r14b
    mov byte [r15], al
    lea r10, [rbp - 64]
    mov r13, r10
    mov r11d, 1
    mov eax, 0
    imul eax, r11d
    mov r12d, eax
    mov rax, r13
    movsxd r11, r12d
    add rax, r11
    mov rbx, rax
    movzx eax, byte [r15 + 0]
    mov byte [rbp - 72], al
    add rbx, 0
    mov al, byte [rbp - 72]
    mov byte [rbx], al
    mov r11d, 1
    mov eax, 1
    imul eax, r11d
    mov dword [rbp - 80], eax
    mov rax, r13
    movsxd r11, dword [rbp - 80]
    add rax, r11
    mov qword [rbp - 88], rax
    mov byte [rbp - 96], 0
    and byte [rbp - 96], 255
    mov r11, [rbp - 88]
    add r11, 0
    mov al, byte [rbp - 96]
    mov byte [r11], al
    mov qword [rbp - 104], r13
    push [rbp - 104]
    pop rdi
    call print
    mov qword [rbp - 112], rax
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
print:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 80
.entry:
    push rdi
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov rax, r14
    mov qword [r15], rax
    lea r10, [rbp - 64]
    mov r13, r10
    mov rax, qword [r15 + 0]
    mov r12, rax
    mov rbx, r12
    add r13, 0
    mov rax, rbx
    mov qword [r13], rax
    lea r10, [rbp - 72]
    mov qword [rbp - 80], r10
    mov rax, qword [r13 + 0]
    mov qword [rbp - 88], rax
    push [rbp - 88]
    pop rdi
    call strlen
    mov dword [rbp - 96], eax
    mov r11, [rbp - 80]
    add r11, 0
    mov eax, dword [rbp - 96]
    mov dword [r11], eax
    mov r10, [rbp - 80]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 104], eax
    mov eax, dword [rbp - 104]
    mov r11d, 0
    cmp eax, r11d
    sete al
    movzx rax, al
    mov [rbp - 112], rax
    mov rax, [rbp - 112]
    cmp rax, 0
    jne .print_then0
    jmp .print_else1
.print_then0:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.print_dead3:
    jmp .print_ifcont2
.print_else1:
    jmp .print_ifcont2
.print_ifcont2:
    mov r10, [rbp - 80]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 120], eax
    mov rax, qword [r13 + 0]
    mov qword [rbp - 128], rax
    mov rax, 1
    mov rdi, 1
    mov rsi, [rbp - 128]
    movsxd rdx, [rbp - 120]
    syscall
    mov r11, [rbp - 80]
    add r11, 0
    mov eax, dword [rbp - 120]
    mov dword [r11], eax
    add r13, 0
    mov rax, qword [rbp - 128]
    mov qword [r13], rax
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
puts:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 16
.entry:
    push rdi
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov rax, r14
    mov qword [r15], rax
    mov rax, qword [r15 + 0]
    mov r13, rax
    push r13
    pop rdi
    call print
    mov r12, rax
    push str0
    pop rdi
    call print
    mov rbx, rax
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
printInt:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 336
.entry:
    push rdi
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov eax, r14d
    mov dword [r15], eax
    mov eax, dword [r15 + 0]
    mov r13d, eax
    mov eax, r13d
    mov r11d, 0
    cmp eax, r11d
    sete al
    movzx rax, al
    mov r12, rax
    cmp r12, 0
    jne .printInt_then0
    jmp .printInt_else1
.printInt_then0:
    push str1
    pop rdi
    call print
    mov rbx, rax
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.printInt_dead3:
    jmp .printInt_ifcont2
.printInt_else1:
    jmp .printInt_ifcont2
.printInt_ifcont2:
    lea r10, [rbp - 64]
    mov qword [rbp - 120], r10
    mov eax, dword [r15 + 0]
    mov dword [rbp - 128], eax
    mov r11, [rbp - 120]
    add r11, 0
    mov eax, dword [rbp - 128]
    mov dword [r11], eax
    mov r10, [rbp - 120]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 136], eax
    mov eax, dword [rbp - 136]
    mov r11d, 0
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 144], rax
    mov rax, [rbp - 144]
    cmp rax, 0
    jne .printInt_then4
    jmp .printInt_else5
.printInt_then4:
    push str2
    pop rdi
    call print
    mov qword [rbp - 152], rax
    mov r10, [rbp - 120]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 160], eax
    mov eax, 0
    mov r11d, dword [rbp - 160]
    sub eax, r11d
    mov dword [rbp - 168], eax
    mov r11, [rbp - 120]
    add r11, 0
    mov eax, dword [rbp - 168]
    mov dword [r11], eax
    jmp .printInt_ifcont6
.printInt_else5:
    jmp .printInt_ifcont6
.printInt_ifcont6:
    lea r10, [rbp - 96]
    mov qword [rbp - 176], r10
    lea r10, [rbp - 104]
    mov qword [rbp - 184], r10
    mov r11, [rbp - 184]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    jmp .printInt_while_header7
.printInt_while_header7:
    mov r10, [rbp - 120]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 192], eax
    mov eax, dword [rbp - 192]
    mov r11d, 0
    cmp eax, r11d
    setg al
    movzx rax, al
    mov [rbp - 200], rax
    mov rax, [rbp - 200]
    cmp rax, 0
    jne .printInt_while_body8
    jmp .printInt_while_exit9
.printInt_while_body8:
    lea r10, [rbp - 112]
    mov qword [rbp - 208], r10
    mov r10, [rbp - 120]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 216], eax
    mov r11d, 10
    mov eax, dword [rbp - 216]
    cdq
    idiv r11d
    mov dword [rbp - 224], edx
    mov r11, [rbp - 208]
    add r11, 0
    mov eax, dword [rbp - 224]
    mov dword [r11], eax
    mov r10, [rbp - 184]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 232], eax
    mov r11d, 1
    mov eax, dword [rbp - 232]
    imul eax, r11d
    mov dword [rbp - 240], eax
    mov rax, qword [rbp - 176]
    movsxd r11, dword [rbp - 240]
    add rax, r11
    mov qword [rbp - 248], rax
    mov r10, [rbp - 208]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 256], eax
    mov eax, dword [rbp - 256]
    mov r11d, 48
    add eax, r11d
    mov dword [rbp - 264], eax
    mov al, byte [rbp - 264]
    mov byte [rbp - 272], al
    and byte [rbp - 272], 255
    mov r11, [rbp - 248]
    add r11, 0
    mov al, byte [rbp - 272]
    mov byte [r11], al
    mov r10, [rbp - 120]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 280], eax
    mov r11d, 10
    mov eax, dword [rbp - 280]
    cdq
    idiv r11d
    mov dword [rbp - 288], eax
    mov r11, [rbp - 120]
    add r11, 0
    mov eax, dword [rbp - 288]
    mov dword [r11], eax
    mov r10, [rbp - 184]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 296], eax
    mov eax, dword [rbp - 296]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 304], eax
    mov r11, [rbp - 184]
    add r11, 0
    mov eax, dword [rbp - 304]
    mov dword [r11], eax
    jmp .printInt_while_header7
.printInt_while_exit9:
    jmp .printInt_while_header10
.printInt_while_header10:
    mov r10, [rbp - 184]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 312], eax
    mov eax, dword [rbp - 312]
    mov r11d, 0
    cmp eax, r11d
    setg al
    movzx rax, al
    mov [rbp - 320], rax
    mov rax, [rbp - 320]
    cmp rax, 0
    jne .printInt_while_body11
    jmp .printInt_while_exit12
.printInt_while_body11:
    mov r10, [rbp - 184]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 328], eax
    mov eax, dword [rbp - 328]
    mov r11d, 1
    sub eax, r11d
    mov dword [rbp - 336], eax
    mov r11, [rbp - 184]
    add r11, 0
    mov eax, dword [rbp - 336]
    mov dword [r11], eax
    mov r10, [rbp - 184]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 344], eax
    mov r11d, 1
    mov eax, dword [rbp - 344]
    imul eax, r11d
    mov dword [rbp - 352], eax
    mov rax, qword [rbp - 176]
    movsxd r11, dword [rbp - 352]
    add rax, r11
    mov qword [rbp - 360], rax
    mov r10, [rbp - 360]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 368], al
    push [rbp - 368]
    pop rdi
    call printChar
    mov qword [rbp - 376], rax
    jmp .printInt_while_header10
.printInt_while_exit12:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
exit:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 16
.entry:
    push rdi
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov eax, r14d
    mov dword [r15], eax
    mov eax, dword [r15 + 0]
    mov r13d, eax
    mov rax, 60
    mov rdi, r13
    syscall
    add r15, 0
    mov eax, r13d
    mov dword [r15], eax
    jmp .exit_while_header0
.exit_while_header0:
    mov rax, 1
    cmp rax, 0
    jne .exit_while_body1
    jmp .exit_while_exit2
.exit_while_body1:
    jmp .exit_while_header0
.exit_while_exit2:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
open:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 176
.entry:
    push rdi
    push rsi
    push rdx
    pop r11
    mov qword [rbp - 112], r11
    pop r11
    mov r12, r11
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov rax, r14
    mov qword [r15], rax
    lea r10, [rbp - 64]
    mov r13, r10
    add r13, 0
    mov eax, r12d
    mov dword [r13], eax
    lea r10, [rbp - 72]
    mov rbx, r10
    add rbx, 0
    mov eax, dword [rbp - 112]
    mov dword [rbx], eax
    lea r10, [rbp - 80]
    mov qword [rbp - 120], r10
    mov rax, qword [r15 + 0]
    mov qword [rbp - 128], rax
    mov rax, qword [rbp - 128]
    mov qword [rbp - 136], rax
    mov r11, [rbp - 120]
    add r11, 0
    mov rax, qword [rbp - 136]
    mov qword [r11], rax
    lea r10, [rbp - 88]
    mov qword [rbp - 144], r10
    mov eax, dword [r13 + 0]
    mov dword [rbp - 152], eax
    mov r11, [rbp - 144]
    add r11, 0
    mov eax, dword [rbp - 152]
    mov dword [r11], eax
    lea r10, [rbp - 96]
    mov qword [rbp - 160], r10
    mov eax, dword [rbx + 0]
    mov dword [rbp - 168], eax
    mov r11, [rbp - 160]
    add r11, 0
    mov eax, dword [rbp - 168]
    mov dword [r11], eax
    lea r10, [rbp - 104]
    mov qword [rbp - 176], r10
    mov r10, [rbp - 144]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 184], eax
    mov r10, [rbp - 120]
    mov rax, qword [r10 + 0]
    mov qword [rbp - 192], rax
    mov r10, [rbp - 160]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 200], eax
    mov r10, [rbp - 176]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 208], eax
    mov rax, 2
    mov rdi, [rbp - 192]
    mov rsi, [rbp - 184]
    mov rdx, [rbp - 200]
    syscall
    mov [rbp - 208], rax
    mov r11, [rbp - 144]
    add r11, 0
    mov eax, dword [rbp - 184]
    mov dword [r11], eax
    mov r11, [rbp - 120]
    add r11, 0
    mov rax, qword [rbp - 192]
    mov qword [r11], rax
    mov r11, [rbp - 160]
    add r11, 0
    mov eax, dword [rbp - 200]
    mov dword [r11], eax
    mov r11, [rbp - 176]
    add r11, 0
    mov eax, dword [rbp - 208]
    mov dword [r11], eax
    mov r10, [rbp - 176]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 216], eax
    mov eax, dword [rbp - 216]
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.open_dead0:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
read:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 160
.entry:
    push rdi
    push rsi
    push rdx
    pop r11
    mov qword [rbp - 112], r11
    pop r11
    mov r12, r11
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov eax, r14d
    mov dword [r15], eax
    lea r10, [rbp - 64]
    mov r13, r10
    add r13, 0
    mov rax, r12
    mov qword [r13], rax
    lea r10, [rbp - 72]
    mov rbx, r10
    add rbx, 0
    mov eax, dword [rbp - 112]
    mov dword [rbx], eax
    lea r10, [rbp - 80]
    mov qword [rbp - 120], r10
    mov eax, dword [r15 + 0]
    mov dword [rbp - 128], eax
    mov r11, [rbp - 120]
    add r11, 0
    mov eax, dword [rbp - 128]
    mov dword [r11], eax
    lea r10, [rbp - 88]
    mov qword [rbp - 136], r10
    mov rax, qword [r13 + 0]
    mov qword [rbp - 144], rax
    mov r11, [rbp - 136]
    add r11, 0
    mov rax, qword [rbp - 144]
    mov qword [r11], rax
    lea r10, [rbp - 96]
    mov qword [rbp - 152], r10
    mov eax, dword [rbx + 0]
    mov dword [rbp - 160], eax
    mov r11, [rbp - 152]
    add r11, 0
    mov eax, dword [rbp - 160]
    mov dword [r11], eax
    lea r10, [rbp - 104]
    mov qword [rbp - 168], r10
    mov r10, [rbp - 136]
    mov rax, qword [r10 + 0]
    mov qword [rbp - 176], rax
    mov r10, [rbp - 152]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 184], eax
    mov r10, [rbp - 120]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 192], eax
    mov r10, [rbp - 168]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 200], eax
    mov rax, 0
    mov rdi, [rbp - 192]
    mov rsi, [rbp - 176]
    mov rdx, [rbp - 184]
    syscall
    mov [rbp - 200], rax
    mov r11, [rbp - 136]
    add r11, 0
    mov rax, qword [rbp - 176]
    mov qword [r11], rax
    mov r11, [rbp - 152]
    add r11, 0
    mov eax, dword [rbp - 184]
    mov dword [r11], eax
    mov r11, [rbp - 120]
    add r11, 0
    mov eax, dword [rbp - 192]
    mov dword [r11], eax
    mov r11, [rbp - 168]
    add r11, 0
    mov eax, dword [rbp - 200]
    mov dword [r11], eax
    mov r10, [rbp - 168]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 208], eax
    mov eax, dword [rbp - 208]
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.read_dead0:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
write:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 160
.entry:
    push rdi
    push rsi
    push rdx
    pop r11
    mov qword [rbp - 112], r11
    pop r11
    mov r12, r11
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov eax, r14d
    mov dword [r15], eax
    lea r10, [rbp - 64]
    mov r13, r10
    add r13, 0
    mov rax, r12
    mov qword [r13], rax
    lea r10, [rbp - 72]
    mov rbx, r10
    add rbx, 0
    mov eax, dword [rbp - 112]
    mov dword [rbx], eax
    lea r10, [rbp - 80]
    mov qword [rbp - 120], r10
    mov eax, dword [r15 + 0]
    mov dword [rbp - 128], eax
    mov r11, [rbp - 120]
    add r11, 0
    mov eax, dword [rbp - 128]
    mov dword [r11], eax
    lea r10, [rbp - 88]
    mov qword [rbp - 136], r10
    mov rax, qword [r13 + 0]
    mov qword [rbp - 144], rax
    mov r11, [rbp - 136]
    add r11, 0
    mov rax, qword [rbp - 144]
    mov qword [r11], rax
    lea r10, [rbp - 96]
    mov qword [rbp - 152], r10
    mov eax, dword [rbx + 0]
    mov dword [rbp - 160], eax
    mov r11, [rbp - 152]
    add r11, 0
    mov eax, dword [rbp - 160]
    mov dword [r11], eax
    lea r10, [rbp - 104]
    mov qword [rbp - 168], r10
    mov r10, [rbp - 136]
    mov rax, qword [r10 + 0]
    mov qword [rbp - 176], rax
    mov r10, [rbp - 152]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 184], eax
    mov r10, [rbp - 120]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 192], eax
    mov r10, [rbp - 168]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 200], eax
    mov rax, 1
    mov rdi, [rbp - 192]
    mov rsi, [rbp - 176]
    mov rdx, [rbp - 184]
    syscall
    mov [rbp - 200], rax
    mov r11, [rbp - 136]
    add r11, 0
    mov rax, qword [rbp - 176]
    mov qword [r11], rax
    mov r11, [rbp - 152]
    add r11, 0
    mov eax, dword [rbp - 184]
    mov dword [r11], eax
    mov r11, [rbp - 120]
    add r11, 0
    mov eax, dword [rbp - 192]
    mov dword [r11], eax
    mov r11, [rbp - 168]
    add r11, 0
    mov eax, dword [rbp - 200]
    mov dword [r11], eax
    mov r10, [rbp - 168]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 208], eax
    mov eax, dword [rbp - 208]
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.write_dead0:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
close:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 48
.entry:
    push rdi
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov eax, r14d
    mov dword [r15], eax
    lea r10, [rbp - 64]
    mov r13, r10
    mov eax, dword [r15 + 0]
    mov r12d, eax
    add r13, 0
    mov eax, r12d
    mov dword [r13], eax
    lea r10, [rbp - 72]
    mov rbx, r10
    mov eax, dword [r13 + 0]
    mov dword [rbp - 80], eax
    mov eax, dword [rbx + 0]
    mov dword [rbp - 88], eax
    mov rax, 3
    mov rdi, [rbp - 80]
    syscall
    mov [rbp - 88], rax
    add r13, 0
    mov eax, dword [rbp - 80]
    mov dword [r13], eax
    add rbx, 0
    mov eax, dword [rbp - 88]
    mov dword [rbx], eax
    mov eax, dword [rbx + 0]
    mov dword [rbp - 96], eax
    mov eax, dword [rbp - 96]
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.close_dead0:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
lseek:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 160
.entry:
    push rdi
    push rsi
    push rdx
    pop r11
    mov qword [rbp - 112], r11
    pop r11
    mov r12, r11
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov eax, r14d
    mov dword [r15], eax
    lea r10, [rbp - 64]
    mov r13, r10
    add r13, 0
    mov eax, r12d
    mov dword [r13], eax
    lea r10, [rbp - 72]
    mov rbx, r10
    add rbx, 0
    mov eax, dword [rbp - 112]
    mov dword [rbx], eax
    lea r10, [rbp - 80]
    mov qword [rbp - 120], r10
    mov eax, dword [r15 + 0]
    mov dword [rbp - 128], eax
    mov r11, [rbp - 120]
    add r11, 0
    mov eax, dword [rbp - 128]
    mov dword [r11], eax
    lea r10, [rbp - 88]
    mov qword [rbp - 136], r10
    mov eax, dword [r13 + 0]
    mov dword [rbp - 144], eax
    mov r11, [rbp - 136]
    add r11, 0
    mov eax, dword [rbp - 144]
    mov dword [r11], eax
    lea r10, [rbp - 96]
    mov qword [rbp - 152], r10
    mov eax, dword [rbx + 0]
    mov dword [rbp - 160], eax
    mov r11, [rbp - 152]
    add r11, 0
    mov eax, dword [rbp - 160]
    mov dword [r11], eax
    lea r10, [rbp - 104]
    mov qword [rbp - 168], r10
    mov r10, [rbp - 120]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 176], eax
    mov r10, [rbp - 136]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 184], eax
    mov r10, [rbp - 168]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 192], eax
    mov r10, [rbp - 152]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 200], eax
    mov rax, 8
    mov rdi, [rbp - 176]
    mov rsi, [rbp - 184]
    mov rdx, [rbp - 200]
    syscall
    mov [rbp - 192], rax
    mov r11, [rbp - 120]
    add r11, 0
    mov eax, dword [rbp - 176]
    mov dword [r11], eax
    mov r11, [rbp - 136]
    add r11, 0
    mov eax, dword [rbp - 184]
    mov dword [r11], eax
    mov r11, [rbp - 168]
    add r11, 0
    mov eax, dword [rbp - 192]
    mov dword [r11], eax
    mov r11, [rbp - 152]
    add r11, 0
    mov eax, dword [rbp - 200]
    mov dword [r11], eax
    mov r10, [rbp - 168]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 208], eax
    mov eax, dword [rbp - 208]
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.lseek_dead0:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
unlink:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 64
.entry:
    push rdi
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov rax, r14
    mov qword [r15], rax
    lea r10, [rbp - 64]
    mov r13, r10
    mov rax, qword [r15 + 0]
    mov r12, rax
    mov rbx, r12
    add r13, 0
    mov rax, rbx
    mov qword [r13], rax
    lea r10, [rbp - 72]
    mov qword [rbp - 80], r10
    mov rax, qword [r13 + 0]
    mov qword [rbp - 88], rax
    mov r10, [rbp - 80]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 96], eax
    mov rax, 87
    mov rdi, [rbp - 88]
    syscall
    mov [rbp - 96], rax
    add r13, 0
    mov rax, qword [rbp - 88]
    mov qword [r13], rax
    mov r11, [rbp - 80]
    add r11, 0
    mov eax, dword [rbp - 96]
    mov dword [r11], eax
    mov r10, [rbp - 80]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 104], eax
    mov eax, dword [rbp - 104]
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
.unlink_dead0:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
dump_file:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 5536
.entry:
    push rdi
    pop r11
    mov r14, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov eax, r14d
    mov dword [r15], eax
    lea r10, [rbp - 4152]
    mov r13, r10
    lea r10, [rbp - 4160]
    mov r12, r10
    add r12, 0
    mov eax, 0
    mov dword [r12], eax
    jmp .dump_file_while_header0
.dump_file_while_header0:
    mov rax, 1
    cmp rax, 0
    jne .dump_file_while_body1
    jmp .dump_file_while_exit2
.dump_file_while_body1:
    lea r10, [rbp - 4168]
    mov rbx, r10
    mov eax, dword [r15 + 0]
    mov dword [rbp - 4328], eax
    mov qword [rbp - 4336], r13
    push [rbp - 4328]
    push [rbp - 4336]
    push 4096
    pop rdx
    pop rsi
    pop rdi
    call read
    mov dword [rbp - 4344], eax
    add rbx, 0
    mov eax, dword [rbp - 4344]
    mov dword [rbx], eax
    mov eax, dword [rbx + 0]
    mov dword [rbp - 4352], eax
    mov eax, dword [rbp - 4352]
    mov r11d, 0
    cmp eax, r11d
    setle al
    movzx rax, al
    mov [rbp - 4360], rax
    mov rax, [rbp - 4360]
    cmp rax, 0
    jne .dump_file_then3
    jmp .dump_file_else4
.dump_file_then3:
    jmp .dump_file_while_exit2
.dump_file_dead6:
    jmp .dump_file_ifcont5
.dump_file_else4:
    jmp .dump_file_ifcont5
.dump_file_ifcont5:
    lea r10, [rbp - 4176]
    mov qword [rbp - 4368], r10
    mov r11, [rbp - 4368]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    jmp .dump_file_while_header7
.dump_file_while_header7:
    mov r10, [rbp - 4368]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4376], eax
    mov eax, dword [rbx + 0]
    mov dword [rbp - 4384], eax
    mov eax, dword [rbp - 4376]
    mov r11d, dword [rbp - 4384]
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 4392], rax
    mov rax, [rbp - 4392]
    cmp rax, 0
    jne .dump_file_while_body8
    jmp .dump_file_while_exit9
.dump_file_while_body8:
    lea r10, [rbp - 4184]
    mov qword [rbp - 4400], r10
    mov eax, dword [rbx + 0]
    mov dword [rbp - 4408], eax
    mov r10, [rbp - 4368]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4416], eax
    mov eax, dword [rbp - 4408]
    mov r11d, dword [rbp - 4416]
    sub eax, r11d
    mov dword [rbp - 4424], eax
    mov r11, [rbp - 4400]
    add r11, 0
    mov eax, dword [rbp - 4424]
    mov dword [r11], eax
    lea r10, [rbp - 4192]
    mov qword [rbp - 4432], r10
    mov r11, [rbp - 4432]
    add r11, 0
    mov eax, 16
    mov dword [r11], eax
    mov r10, [rbp - 4400]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4440], eax
    mov eax, dword [rbp - 4440]
    mov r11d, 16
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 4448], rax
    mov rax, [rbp - 4448]
    cmp rax, 0
    jne .dump_file_then10
    jmp .dump_file_else11
.dump_file_then10:
    mov r10, [rbp - 4400]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4456], eax
    mov r11, [rbp - 4432]
    add r11, 0
    mov eax, dword [rbp - 4456]
    mov dword [r11], eax
    jmp .dump_file_ifcont12
.dump_file_else11:
    jmp .dump_file_ifcont12
.dump_file_ifcont12:
    lea r10, [rbp - 4272]
    mov qword [rbp - 4464], r10
    lea r10, [rbp - 4280]
    mov qword [rbp - 4472], r10
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    lea r10, [rbp - 4288]
    mov qword [rbp - 4480], r10
    mov eax, dword [r12 + 0]
    mov dword [rbp - 4488], eax
    mov r11, [rbp - 4480]
    add r11, 0
    mov eax, dword [rbp - 4488]
    mov dword [r11], eax
    lea r10, [rbp - 4296]
    mov qword [rbp - 4496], r10
    mov r11, [rbp - 4496]
    add r11, 0
    mov eax, 7
    mov dword [r11], eax
    jmp .dump_file_while_header13
.dump_file_while_header13:
    mov r10, [rbp - 4496]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4504], eax
    mov eax, dword [rbp - 4504]
    mov r11d, 0
    cmp eax, r11d
    setge al
    movzx rax, al
    mov [rbp - 4512], rax
    mov rax, [rbp - 4512]
    cmp rax, 0
    jne .dump_file_while_body14
    jmp .dump_file_while_exit15
.dump_file_while_body14:
    mov r10, [rbp - 4496]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4520], eax
    mov r11d, 1
    mov eax, dword [rbp - 4520]
    imul eax, r11d
    mov dword [rbp - 4528], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 4528]
    add rax, r11
    mov qword [rbp - 4536], rax
    mov rax, qword [rel HEX]
    mov qword [rbp - 4544], rax
    mov r10, [rbp - 4480]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4552], eax
    mov r10, [rbp - 4480]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4560], eax
    mov r11d, 16
    mov eax, dword [rbp - 4560]
    cdq
    idiv r11d
    mov dword [rbp - 4568], eax
    mov r11d, 16
    mov eax, dword [rbp - 4568]
    imul eax, r11d
    mov dword [rbp - 4576], eax
    mov eax, dword [rbp - 4552]
    mov r11d, dword [rbp - 4576]
    sub eax, r11d
    mov dword [rbp - 4584], eax
    mov r11d, 1
    mov eax, dword [rbp - 4584]
    imul eax, r11d
    mov dword [rbp - 4592], eax
    mov rax, qword [rbp - 4544]
    movsxd r11, dword [rbp - 4592]
    add rax, r11
    mov qword [rbp - 4600], rax
    mov r10, [rbp - 4600]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 4608], al
    mov r11, [rbp - 4536]
    add r11, 0
    mov al, byte [rbp - 4608]
    mov byte [r11], al
    mov r10, [rbp - 4480]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4616], eax
    mov r11d, 16
    mov eax, dword [rbp - 4616]
    cdq
    idiv r11d
    mov dword [rbp - 4624], eax
    mov r11, [rbp - 4480]
    add r11, 0
    mov eax, dword [rbp - 4624]
    mov dword [r11], eax
    mov r10, [rbp - 4496]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4632], eax
    mov eax, dword [rbp - 4632]
    mov r11d, 1
    sub eax, r11d
    mov dword [rbp - 4640], eax
    mov r11, [rbp - 4496]
    add r11, 0
    mov eax, dword [rbp - 4640]
    mov dword [r11], eax
    jmp .dump_file_while_header13
.dump_file_while_exit15:
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, 8
    mov dword [r11], eax
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4648], eax
    mov r11d, 1
    mov eax, dword [rbp - 4648]
    imul eax, r11d
    mov dword [rbp - 4656], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 4656]
    add rax, r11
    mov qword [rbp - 4664], rax
    mov r11, [rbp - 4664]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4672], eax
    mov eax, dword [rbp - 4672]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 4680], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 4680]
    mov dword [r11], eax
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4688], eax
    mov r11d, 1
    mov eax, dword [rbp - 4688]
    imul eax, r11d
    mov dword [rbp - 4696], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 4696]
    add rax, r11
    mov qword [rbp - 4704], rax
    mov r11, [rbp - 4704]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4712], eax
    mov eax, dword [rbp - 4712]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 4720], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 4720]
    mov dword [r11], eax
    lea r10, [rbp - 4304]
    mov qword [rbp - 4728], r10
    mov r11, [rbp - 4728]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    jmp .dump_file_while_header16
.dump_file_while_header16:
    mov r10, [rbp - 4728]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4736], eax
    mov eax, dword [rbp - 4736]
    mov r11d, 16
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 4744], rax
    mov rax, [rbp - 4744]
    cmp rax, 0
    jne .dump_file_while_body17
    jmp .dump_file_while_exit18
.dump_file_while_body17:
    mov r10, [rbp - 4728]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4752], eax
    mov r10, [rbp - 4432]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4760], eax
    mov eax, dword [rbp - 4752]
    mov r11d, dword [rbp - 4760]
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 4768], rax
    mov rax, [rbp - 4768]
    cmp rax, 0
    jne .dump_file_then19
    jmp .dump_file_else20
.dump_file_then19:
    lea r10, [rbp - 4312]
    mov qword [rbp - 4776], r10
    mov r10, [rbp - 4368]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4784], eax
    mov r10, [rbp - 4728]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4792], eax
    mov eax, dword [rbp - 4784]
    mov r11d, dword [rbp - 4792]
    add eax, r11d
    mov dword [rbp - 4800], eax
    mov r11d, 1
    mov eax, dword [rbp - 4800]
    imul eax, r11d
    mov dword [rbp - 4808], eax
    mov rax, r13
    movsxd r11, dword [rbp - 4808]
    add rax, r11
    mov qword [rbp - 4816], rax
    mov r10, [rbp - 4816]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 4824], al
    mov r11, [rbp - 4776]
    add r11, 0
    mov al, byte [rbp - 4824]
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4832], eax
    mov r11d, 1
    mov eax, dword [rbp - 4832]
    imul eax, r11d
    mov dword [rbp - 4840], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 4840]
    add rax, r11
    mov qword [rbp - 4848], rax
    mov rax, qword [rel HEX]
    mov qword [rbp - 4856], rax
    mov r10, [rbp - 4776]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4864], eax
    mov r11d, 16
    mov eax, dword [rbp - 4864]
    cdq
    idiv r11d
    mov dword [rbp - 4872], eax
    mov r11d, 1
    mov eax, dword [rbp - 4872]
    imul eax, r11d
    mov dword [rbp - 4880], eax
    mov rax, qword [rbp - 4856]
    movsxd r11, dword [rbp - 4880]
    add rax, r11
    mov qword [rbp - 4888], rax
    mov r10, [rbp - 4888]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 4896], al
    mov r11, [rbp - 4848]
    add r11, 0
    mov al, byte [rbp - 4896]
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4904], eax
    mov eax, dword [rbp - 4904]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 4912], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 4912]
    mov dword [r11], eax
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4920], eax
    mov r11d, 1
    mov eax, dword [rbp - 4920]
    imul eax, r11d
    mov dword [rbp - 4928], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 4928]
    add rax, r11
    mov qword [rbp - 4936], rax
    mov rax, qword [rel HEX]
    mov qword [rbp - 4944], rax
    mov r10, [rbp - 4776]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4952], eax
    mov r10, [rbp - 4776]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4960], eax
    mov r11d, 16
    mov eax, dword [rbp - 4960]
    cdq
    idiv r11d
    mov dword [rbp - 4968], eax
    mov r11d, 16
    mov eax, dword [rbp - 4968]
    imul eax, r11d
    mov dword [rbp - 4976], eax
    mov eax, dword [rbp - 4952]
    mov r11d, dword [rbp - 4976]
    sub eax, r11d
    mov dword [rbp - 4984], eax
    mov r11d, 1
    mov eax, dword [rbp - 4984]
    imul eax, r11d
    mov dword [rbp - 4992], eax
    mov rax, qword [rbp - 4944]
    movsxd r11, dword [rbp - 4992]
    add rax, r11
    mov qword [rbp - 5000], rax
    mov r10, [rbp - 5000]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 5008], al
    mov r11, [rbp - 4936]
    add r11, 0
    mov al, byte [rbp - 5008]
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5016], eax
    mov eax, dword [rbp - 5016]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5024], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 5024]
    mov dword [r11], eax
    jmp .dump_file_ifcont21
.dump_file_else20:
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5032], eax
    mov r11d, 1
    mov eax, dword [rbp - 5032]
    imul eax, r11d
    mov dword [rbp - 5040], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 5040]
    add rax, r11
    mov qword [rbp - 5048], rax
    mov r11, [rbp - 5048]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5056], eax
    mov eax, dword [rbp - 5056]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5064], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 5064]
    mov dword [r11], eax
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5072], eax
    mov r11d, 1
    mov eax, dword [rbp - 5072]
    imul eax, r11d
    mov dword [rbp - 5080], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 5080]
    add rax, r11
    mov qword [rbp - 5088], rax
    mov r11, [rbp - 5088]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5096], eax
    mov eax, dword [rbp - 5096]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5104], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 5104]
    mov dword [r11], eax
    jmp .dump_file_ifcont21
.dump_file_ifcont21:
    mov r10, [rbp - 4728]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5112], eax
    mov eax, dword [rbp - 5112]
    mov r11d, 7
    cmp eax, r11d
    sete al
    movzx rax, al
    mov [rbp - 5120], rax
    mov rax, [rbp - 5120]
    cmp rax, 0
    jne .dump_file_then22
    jmp .dump_file_else23
.dump_file_then22:
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5128], eax
    mov r11d, 1
    mov eax, dword [rbp - 5128]
    imul eax, r11d
    mov dword [rbp - 5136], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 5136]
    add rax, r11
    mov qword [rbp - 5144], rax
    mov r11, [rbp - 5144]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5152], eax
    mov eax, dword [rbp - 5152]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5160], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 5160]
    mov dword [r11], eax
    jmp .dump_file_ifcont24
.dump_file_else23:
    jmp .dump_file_ifcont24
.dump_file_ifcont24:
    mov r10, [rbp - 4728]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5168], eax
    mov eax, dword [rbp - 5168]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5176], eax
    mov r11, [rbp - 4728]
    add r11, 0
    mov eax, dword [rbp - 5176]
    mov dword [r11], eax
    jmp .dump_file_while_header16
.dump_file_while_exit18:
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5184], eax
    mov r11d, 1
    mov eax, dword [rbp - 5184]
    imul eax, r11d
    mov dword [rbp - 5192], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 5192]
    add rax, r11
    mov qword [rbp - 5200], rax
    mov r11, [rbp - 5200]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5208], eax
    mov eax, dword [rbp - 5208]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5216], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 5216]
    mov dword [r11], eax
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5224], eax
    mov r11d, 1
    mov eax, dword [rbp - 5224]
    imul eax, r11d
    mov dword [rbp - 5232], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 5232]
    add rax, r11
    mov qword [rbp - 5240], rax
    mov r11, [rbp - 5240]
    add r11, 0
    mov al, 124
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5248], eax
    mov eax, dword [rbp - 5248]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5256], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 5256]
    mov dword [r11], eax
    mov r11, [rbp - 4728]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    jmp .dump_file_while_header25
.dump_file_while_header25:
    mov r10, [rbp - 4728]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5264], eax
    mov r10, [rbp - 4432]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5272], eax
    mov eax, dword [rbp - 5264]
    mov r11d, dword [rbp - 5272]
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 5280], rax
    mov rax, [rbp - 5280]
    cmp rax, 0
    jne .dump_file_while_body26
    jmp .dump_file_while_exit27
.dump_file_while_body26:
    lea r10, [rbp - 4320]
    mov qword [rbp - 5288], r10
    mov r10, [rbp - 4368]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5296], eax
    mov r10, [rbp - 4728]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5304], eax
    mov eax, dword [rbp - 5296]
    mov r11d, dword [rbp - 5304]
    add eax, r11d
    mov dword [rbp - 5312], eax
    mov r11d, 1
    mov eax, dword [rbp - 5312]
    imul eax, r11d
    mov dword [rbp - 5320], eax
    mov rax, r13
    movsxd r11, dword [rbp - 5320]
    add rax, r11
    mov qword [rbp - 5328], rax
    mov r10, [rbp - 5328]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 5336], al
    mov r11, [rbp - 5288]
    add r11, 0
    mov al, byte [rbp - 5336]
    mov byte [r11], al
    mov r10, [rbp - 5288]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5344], eax
    mov eax, dword [rbp - 5344]
    mov r11d, 32
    cmp eax, r11d
    setge al
    movzx rax, al
    mov [rbp - 5352], rax
    mov rax, [rbp - 5352]
    cmp rax, 0
    jne .dump_file_sc_true28
    jmp .dump_file_sc_false29
.dump_file_sc_true28:
    mov r10, [rbp - 5288]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5360], eax
    mov eax, dword [rbp - 5360]
    mov r11d, 127
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 5368], rax
    movsxd rax, dword [rbp - 5368]
    push rax
    pop rax
    mov qword [rbp - 5376], rax
    jmp .dump_file_sc_merge30
.dump_file_sc_false29:
    push 0
    pop rax
    mov qword [rbp - 5376], rax
    jmp .dump_file_sc_merge30
.dump_file_sc_merge30:
    mov rax, [rbp - 5376]
    cmp rax, 0
    jne .dump_file_then31
    jmp .dump_file_else32
.dump_file_then31:
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5384], eax
    mov r11d, 1
    mov eax, dword [rbp - 5384]
    imul eax, r11d
    mov dword [rbp - 5392], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 5392]
    add rax, r11
    mov qword [rbp - 5400], rax
    mov r10, [rbp - 5288]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5408], eax
    mov r11, [rbp - 5400]
    add r11, 0
    mov eax, dword [rbp - 5408]
    mov dword [r11], eax
    jmp .dump_file_ifcont33
.dump_file_else32:
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5416], eax
    mov r11d, 1
    mov eax, dword [rbp - 5416]
    imul eax, r11d
    mov dword [rbp - 5424], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 5424]
    add rax, r11
    mov qword [rbp - 5432], rax
    mov r11, [rbp - 5432]
    add r11, 0
    mov al, 46
    mov byte [r11], al
    jmp .dump_file_ifcont33
.dump_file_ifcont33:
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5440], eax
    mov eax, dword [rbp - 5440]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5448], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 5448]
    mov dword [r11], eax
    mov r10, [rbp - 4728]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5456], eax
    mov eax, dword [rbp - 5456]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5464], eax
    mov r11, [rbp - 4728]
    add r11, 0
    mov eax, dword [rbp - 5464]
    mov dword [r11], eax
    jmp .dump_file_while_header25
.dump_file_while_exit27:
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5472], eax
    mov r11d, 1
    mov eax, dword [rbp - 5472]
    imul eax, r11d
    mov dword [rbp - 5480], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 5480]
    add rax, r11
    mov qword [rbp - 5488], rax
    mov r11, [rbp - 5488]
    add r11, 0
    mov al, 124
    mov byte [r11], al
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5496], eax
    mov eax, dword [rbp - 5496]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5504], eax
    mov r11, [rbp - 4472]
    add r11, 0
    mov eax, dword [rbp - 5504]
    mov dword [r11], eax
    mov r10, [rbp - 4472]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5512], eax
    mov r11d, 1
    mov eax, dword [rbp - 5512]
    imul eax, r11d
    mov dword [rbp - 5520], eax
    mov rax, qword [rbp - 4464]
    movsxd r11, dword [rbp - 5520]
    add rax, r11
    mov qword [rbp - 5528], rax
    mov r11, [rbp - 5528]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    mov rax, qword [rbp - 4464]
    mov qword [rbp - 5536], rax
    push [rbp - 5536]
    pop rdi
    call puts
    mov qword [rbp - 5544], rax
    mov eax, dword [r12 + 0]
    mov dword [rbp - 5552], eax
    mov eax, dword [rbp - 5552]
    mov r11d, 16
    add eax, r11d
    mov dword [rbp - 5560], eax
    add r12, 0
    mov eax, dword [rbp - 5560]
    mov dword [r12], eax
    mov r10, [rbp - 4368]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5568], eax
    mov eax, dword [rbp - 5568]
    mov r11d, 16
    add eax, r11d
    mov dword [rbp - 5576], eax
    mov r11, [rbp - 4368]
    add r11, 0
    mov eax, dword [rbp - 5576]
    mov dword [r11], eax
    jmp .dump_file_while_header7
.dump_file_while_exit9:
    jmp .dump_file_while_header0
.dump_file_while_exit2:
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
global _start
_start:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 384
.entry:
    mov r11, [rbp + 8]
    mov r14, r11
    lea r11, [rbp + 16]
    mov r12, r11
    lea r10, [rbp - 56]
    mov r15, r10
    add r15, 0
    mov eax, r14d
    mov dword [r15], eax
    lea r10, [rbp - 64]
    mov r13, r10
    add r13, 0
    mov rax, r12
    mov qword [r13], rax
    mov eax, dword [r15 + 0]
    mov ebx, eax
    mov eax, ebx
    mov r11d, 2
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 88], rax
    mov rax, [rbp - 88]
    cmp rax, 0
    jne ._start_then0
    jmp ._start_else1
._start_then0:
    push str3
    pop rdi
    call puts
    mov qword [rbp - 96], rax
    push 1
    pop rdi
    call exit
    mov qword [rbp - 104], rax
    jmp ._start_ifcont2
._start_else1:
    jmp ._start_ifcont2
._start_ifcont2:
    lea r10, [rbp - 72]
    mov qword [rbp - 112], r10
    mov r11, [rbp - 112]
    add r11, 0
    mov eax, 1
    mov dword [r11], eax
    jmp ._start_while_header3
._start_while_header3:
    mov r10, [rbp - 112]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 120], eax
    mov eax, dword [r15 + 0]
    mov dword [rbp - 128], eax
    mov eax, dword [rbp - 120]
    mov r11d, dword [rbp - 128]
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 136], rax
    mov rax, [rbp - 136]
    cmp rax, 0
    jne ._start_while_body4
    jmp ._start_while_exit5
._start_while_body4:
    mov eax, dword [r15 + 0]
    mov dword [rbp - 144], eax
    mov eax, dword [rbp - 144]
    mov r11d, 2
    cmp eax, r11d
    setg al
    movzx rax, al
    mov [rbp - 152], rax
    mov rax, [rbp - 152]
    cmp rax, 0
    jne ._start_then6
    jmp ._start_else7
._start_then6:
    mov rax, qword [r13 + 0]
    mov qword [rbp - 160], rax
    mov r10, [rbp - 112]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 168], eax
    mov r11d, 8
    mov eax, dword [rbp - 168]
    imul eax, r11d
    mov dword [rbp - 176], eax
    mov rax, qword [rbp - 160]
    movsxd r11, dword [rbp - 176]
    add rax, r11
    mov qword [rbp - 184], rax
    mov r10, [rbp - 184]
    mov rax, qword [r10 + 0]
    mov qword [rbp - 192], rax
    mov rax, qword [rbp - 192]
    mov qword [rbp - 200], rax
    push [rbp - 200]
    pop rdi
    call puts
    mov qword [rbp - 208], rax
    push str4
    pop rdi
    call puts
    mov qword [rbp - 216], rax
    jmp ._start_ifcont8
._start_else7:
    jmp ._start_ifcont8
._start_ifcont8:
    lea r10, [rbp - 80]
    mov qword [rbp - 224], r10
    mov rax, qword [r13 + 0]
    mov qword [rbp - 232], rax
    mov r10, [rbp - 112]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 240], eax
    mov r11d, 8
    mov eax, dword [rbp - 240]
    imul eax, r11d
    mov dword [rbp - 248], eax
    mov rax, qword [rbp - 232]
    movsxd r11, dword [rbp - 248]
    add rax, r11
    mov qword [rbp - 256], rax
    mov r10, [rbp - 256]
    mov rax, qword [r10 + 0]
    mov qword [rbp - 264], rax
    mov rax, qword [rbp - 264]
    mov qword [rbp - 272], rax
    mov eax, dword [rel O_RDONLY]
    mov dword [rbp - 280], eax
    push [rbp - 272]
    push [rbp - 280]
    push 0
    pop rdx
    pop rsi
    pop rdi
    call open
    mov dword [rbp - 288], eax
    mov r11, [rbp - 224]
    add r11, 0
    mov eax, dword [rbp - 288]
    mov dword [r11], eax
    mov r10, [rbp - 224]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 296], eax
    mov eax, dword [rbp - 296]
    mov r11d, 0
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 304], rax
    mov rax, [rbp - 304]
    cmp rax, 0
    jne ._start_then9
    jmp ._start_else10
._start_then9:
    push str5
    pop rdi
    call puts
    mov qword [rbp - 312], rax
    mov rax, qword [r13 + 0]
    mov qword [rbp - 320], rax
    mov r10, [rbp - 112]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 328], eax
    mov r11d, 8
    mov eax, dword [rbp - 328]
    imul eax, r11d
    mov dword [rbp - 336], eax
    mov rax, qword [rbp - 320]
    movsxd r11, dword [rbp - 336]
    add rax, r11
    mov qword [rbp - 344], rax
    mov r10, [rbp - 344]
    mov rax, qword [r10 + 0]
    mov qword [rbp - 352], rax
    mov rax, qword [rbp - 352]
    mov qword [rbp - 360], rax
    push [rbp - 360]
    pop rdi
    call puts
    mov qword [rbp - 368], rax
    push str6
    pop rdi
    call puts
    mov qword [rbp - 376], rax
    push 1
    pop rdi
    call exit
    mov qword [rbp - 384], rax
    jmp ._start_ifcont11
._start_else10:
    jmp ._start_ifcont11
._start_ifcont11:
    mov r10, [rbp - 224]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 392], eax
    push [rbp - 392]
    pop rdi
    call dump_file
    mov qword [rbp - 400], rax
    mov r10, [rbp - 224]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 408], eax
    push [rbp - 408]
    pop rdi
    call close
    mov dword [rbp - 416], eax
    mov r10, [rbp - 112]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 424], eax
    mov eax, dword [rbp - 424]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 432], eax
    mov r11, [rbp - 112]
    add r11, 0
    mov eax, dword [rbp - 432]
    mov dword [r11], eax
    jmp ._start_while_header3
._start_while_exit5:
    mov rax, 60
    xor rdi, rdi
    syscall
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
    lea rsp, [rbp - 40]
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
