section .bss
    O_RDONLY: resd 1

section .data
    str0: db 10, 0
    str1: db `0`, 0
    str2: db `-`, 0
    str3: db `0123456789abcdef`, 0
    str4: db `hexdump: missing filename`, 10, 0
    str5: db `hexdump: cannot open`, 10, 0
    O_WRONLY: dd 1
    O_RDWR: dd 2
    O_CREAT: dd 64
    O_TRUNC: dd 512
    O_APPEND: dd 1024

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
global _start
_start:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 5776
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
    lea r10, [rbp - 72]
    mov rbx, r10
    add rbx, 0
    lea rax, [rel str3]
    mov qword [rbx], rax
    lea r10, [rbp - 80]
    mov qword [rbp - 4360], r10
    mov rax, qword [rbx + 0]
    mov qword [rbp - 4368], rax
    mov rax, qword [rbp - 4368]
    mov qword [rbp - 4376], rax
    mov r11, [rbp - 4360]
    add r11, 0
    mov rax, qword [rbp - 4376]
    mov qword [r11], rax
    mov eax, dword [r15 + 0]
    mov dword [rbp - 4384], eax
    mov eax, dword [rbp - 4384]
    mov r11d, 2
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 4392], rax
    mov rax, [rbp - 4392]
    cmp rax, 0
    jne ._start_then0
    jmp ._start_else1
._start_then0:
    push str4
    pop rdi
    call puts
    mov qword [rbp - 4400], rax
    push 1
    pop rdi
    call exit
    mov qword [rbp - 4408], rax
    jmp ._start_ifcont2
._start_else1:
    jmp ._start_ifcont2
._start_ifcont2:
    lea r10, [rbp - 88]
    mov qword [rbp - 4416], r10
    mov rax, qword [r13 + 0]
    mov qword [rbp - 4424], rax
    mov r11d, 8
    mov eax, 1
    imul eax, r11d
    mov dword [rbp - 4432], eax
    mov rax, qword [rbp - 4424]
    movsxd r11, dword [rbp - 4432]
    add rax, r11
    mov qword [rbp - 4440], rax
    mov r10, [rbp - 4440]
    mov rax, qword [r10 + 0]
    mov qword [rbp - 4448], rax
    mov rax, qword [rbp - 4448]
    mov qword [rbp - 4456], rax
    mov eax, dword [rel O_RDONLY]
    mov dword [rbp - 4464], eax
    push [rbp - 4456]
    push [rbp - 4464]
    push 0
    pop rdx
    pop rsi
    pop rdi
    call open
    mov dword [rbp - 4472], eax
    mov r11, [rbp - 4416]
    add r11, 0
    mov eax, dword [rbp - 4472]
    mov dword [r11], eax
    mov r10, [rbp - 4416]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4480], eax
    mov eax, dword [rbp - 4480]
    mov r11d, 0
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 4488], rax
    mov rax, [rbp - 4488]
    cmp rax, 0
    jne ._start_then3
    jmp ._start_else4
._start_then3:
    push str5
    pop rdi
    call puts
    mov qword [rbp - 4496], rax
    push 1
    pop rdi
    call exit
    mov qword [rbp - 4504], rax
    jmp ._start_ifcont5
._start_else4:
    jmp ._start_ifcont5
._start_ifcont5:
    lea r10, [rbp - 4184]
    mov qword [rbp - 4512], r10
    lea r10, [rbp - 4192]
    mov qword [rbp - 4520], r10
    mov r11, [rbp - 4520]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    jmp ._start_while_header6
._start_while_header6:
    mov rax, 1
    cmp rax, 0
    jne ._start_while_body7
    jmp ._start_while_exit8
._start_while_body7:
    lea r10, [rbp - 4200]
    mov qword [rbp - 4528], r10
    mov r10, [rbp - 4416]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4536], eax
    mov rax, qword [rbp - 4512]
    mov qword [rbp - 4544], rax
    push [rbp - 4536]
    push [rbp - 4544]
    push 4096
    pop rdx
    pop rsi
    pop rdi
    call read
    mov dword [rbp - 4552], eax
    mov r11, [rbp - 4528]
    add r11, 0
    mov eax, dword [rbp - 4552]
    mov dword [r11], eax
    mov r10, [rbp - 4528]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4560], eax
    mov eax, dword [rbp - 4560]
    mov r11d, 0
    cmp eax, r11d
    setle al
    movzx rax, al
    mov [rbp - 4568], rax
    mov rax, [rbp - 4568]
    cmp rax, 0
    jne ._start_then9
    jmp ._start_else10
._start_then9:
    jmp ._start_while_exit8
._start_dead12:
    jmp ._start_ifcont11
._start_else10:
    jmp ._start_ifcont11
._start_ifcont11:
    lea r10, [rbp - 4208]
    mov qword [rbp - 4576], r10
    mov r11, [rbp - 4576]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    jmp ._start_while_header13
._start_while_header13:
    mov r10, [rbp - 4576]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4584], eax
    mov r10, [rbp - 4528]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4592], eax
    mov eax, dword [rbp - 4584]
    mov r11d, dword [rbp - 4592]
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 4600], rax
    mov rax, [rbp - 4600]
    cmp rax, 0
    jne ._start_while_body14
    jmp ._start_while_exit15
._start_while_body14:
    lea r10, [rbp - 4216]
    mov qword [rbp - 4608], r10
    mov r10, [rbp - 4528]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4616], eax
    mov r10, [rbp - 4576]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4624], eax
    mov eax, dword [rbp - 4616]
    mov r11d, dword [rbp - 4624]
    sub eax, r11d
    mov dword [rbp - 4632], eax
    mov r11, [rbp - 4608]
    add r11, 0
    mov eax, dword [rbp - 4632]
    mov dword [r11], eax
    lea r10, [rbp - 4224]
    mov qword [rbp - 4640], r10
    mov r11, [rbp - 4640]
    add r11, 0
    mov eax, 16
    mov dword [r11], eax
    mov r10, [rbp - 4608]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4648], eax
    mov eax, dword [rbp - 4648]
    mov r11d, 16
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 4656], rax
    mov rax, [rbp - 4656]
    cmp rax, 0
    jne ._start_then16
    jmp ._start_else17
._start_then16:
    mov r10, [rbp - 4608]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4664], eax
    mov r11, [rbp - 4640]
    add r11, 0
    mov eax, dword [rbp - 4664]
    mov dword [r11], eax
    jmp ._start_ifcont18
._start_else17:
    jmp ._start_ifcont18
._start_ifcont18:
    lea r10, [rbp - 4304]
    mov qword [rbp - 4672], r10
    lea r10, [rbp - 4312]
    mov qword [rbp - 4680], r10
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    lea r10, [rbp - 4320]
    mov qword [rbp - 4688], r10
    mov r10, [rbp - 4520]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4696], eax
    mov r11, [rbp - 4688]
    add r11, 0
    mov eax, dword [rbp - 4696]
    mov dword [r11], eax
    lea r10, [rbp - 4328]
    mov qword [rbp - 4704], r10
    mov r11, [rbp - 4704]
    add r11, 0
    mov eax, 7
    mov dword [r11], eax
    jmp ._start_while_header19
._start_while_header19:
    mov r10, [rbp - 4704]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4712], eax
    mov eax, dword [rbp - 4712]
    mov r11d, 0
    cmp eax, r11d
    setge al
    movzx rax, al
    mov [rbp - 4720], rax
    mov rax, [rbp - 4720]
    cmp rax, 0
    jne ._start_while_body20
    jmp ._start_while_exit21
._start_while_body20:
    mov r10, [rbp - 4704]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4728], eax
    mov r11d, 1
    mov eax, dword [rbp - 4728]
    imul eax, r11d
    mov dword [rbp - 4736], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 4736]
    add rax, r11
    mov qword [rbp - 4744], rax
    mov r10, [rbp - 4360]
    mov rax, qword [r10 + 0]
    mov qword [rbp - 4752], rax
    mov r10, [rbp - 4688]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4760], eax
    mov r10, [rbp - 4688]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4768], eax
    mov r11d, 16
    mov eax, dword [rbp - 4768]
    cdq
    idiv r11d
    mov dword [rbp - 4776], eax
    mov r11d, 16
    mov eax, dword [rbp - 4776]
    imul eax, r11d
    mov dword [rbp - 4784], eax
    mov eax, dword [rbp - 4760]
    mov r11d, dword [rbp - 4784]
    sub eax, r11d
    mov dword [rbp - 4792], eax
    mov r11d, 1
    mov eax, dword [rbp - 4792]
    imul eax, r11d
    mov dword [rbp - 4800], eax
    mov rax, qword [rbp - 4752]
    movsxd r11, dword [rbp - 4800]
    add rax, r11
    mov qword [rbp - 4808], rax
    mov r10, [rbp - 4808]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 4816], al
    mov r11, [rbp - 4744]
    add r11, 0
    mov al, byte [rbp - 4816]
    mov byte [r11], al
    mov r10, [rbp - 4688]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4824], eax
    mov r11d, 16
    mov eax, dword [rbp - 4824]
    cdq
    idiv r11d
    mov dword [rbp - 4832], eax
    mov r11, [rbp - 4688]
    add r11, 0
    mov eax, dword [rbp - 4832]
    mov dword [r11], eax
    mov r10, [rbp - 4704]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4840], eax
    mov eax, dword [rbp - 4840]
    mov r11d, 1
    sub eax, r11d
    mov dword [rbp - 4848], eax
    mov r11, [rbp - 4704]
    add r11, 0
    mov eax, dword [rbp - 4848]
    mov dword [r11], eax
    jmp ._start_while_header19
._start_while_exit21:
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, 8
    mov dword [r11], eax
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4856], eax
    mov r11d, 1
    mov eax, dword [rbp - 4856]
    imul eax, r11d
    mov dword [rbp - 4864], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 4864]
    add rax, r11
    mov qword [rbp - 4872], rax
    mov r11, [rbp - 4872]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4880], eax
    mov eax, dword [rbp - 4880]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 4888], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 4888]
    mov dword [r11], eax
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4896], eax
    mov r11d, 1
    mov eax, dword [rbp - 4896]
    imul eax, r11d
    mov dword [rbp - 4904], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 4904]
    add rax, r11
    mov qword [rbp - 4912], rax
    mov r11, [rbp - 4912]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4920], eax
    mov eax, dword [rbp - 4920]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 4928], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 4928]
    mov dword [r11], eax
    lea r10, [rbp - 4336]
    mov qword [rbp - 4936], r10
    mov r11, [rbp - 4936]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    jmp ._start_while_header22
._start_while_header22:
    mov r10, [rbp - 4936]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4944], eax
    mov eax, dword [rbp - 4944]
    mov r11d, 16
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 4952], rax
    mov rax, [rbp - 4952]
    cmp rax, 0
    jne ._start_while_body23
    jmp ._start_while_exit24
._start_while_body23:
    mov r10, [rbp - 4936]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4960], eax
    mov r10, [rbp - 4640]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4968], eax
    mov eax, dword [rbp - 4960]
    mov r11d, dword [rbp - 4968]
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 4976], rax
    mov rax, [rbp - 4976]
    cmp rax, 0
    jne ._start_then25
    jmp ._start_else26
._start_then25:
    lea r10, [rbp - 4344]
    mov qword [rbp - 4984], r10
    mov r10, [rbp - 4576]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 4992], eax
    mov r10, [rbp - 4936]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5000], eax
    mov eax, dword [rbp - 4992]
    mov r11d, dword [rbp - 5000]
    add eax, r11d
    mov dword [rbp - 5008], eax
    mov r11d, 1
    mov eax, dword [rbp - 5008]
    imul eax, r11d
    mov dword [rbp - 5016], eax
    mov rax, qword [rbp - 4512]
    movsxd r11, dword [rbp - 5016]
    add rax, r11
    mov qword [rbp - 5024], rax
    mov r10, [rbp - 5024]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 5032], al
    mov r11, [rbp - 4984]
    add r11, 0
    mov al, byte [rbp - 5032]
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5040], eax
    mov r11d, 1
    mov eax, dword [rbp - 5040]
    imul eax, r11d
    mov dword [rbp - 5048], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5048]
    add rax, r11
    mov qword [rbp - 5056], rax
    mov r10, [rbp - 4360]
    mov rax, qword [r10 + 0]
    mov qword [rbp - 5064], rax
    mov r10, [rbp - 4984]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5072], eax
    mov r11d, 16
    mov eax, dword [rbp - 5072]
    cdq
    idiv r11d
    mov dword [rbp - 5080], eax
    mov r11d, 1
    mov eax, dword [rbp - 5080]
    imul eax, r11d
    mov dword [rbp - 5088], eax
    mov rax, qword [rbp - 5064]
    movsxd r11, dword [rbp - 5088]
    add rax, r11
    mov qword [rbp - 5096], rax
    mov r10, [rbp - 5096]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 5104], al
    mov r11, [rbp - 5056]
    add r11, 0
    mov al, byte [rbp - 5104]
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5112], eax
    mov eax, dword [rbp - 5112]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5120], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 5120]
    mov dword [r11], eax
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5128], eax
    mov r11d, 1
    mov eax, dword [rbp - 5128]
    imul eax, r11d
    mov dword [rbp - 5136], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5136]
    add rax, r11
    mov qword [rbp - 5144], rax
    mov r10, [rbp - 4360]
    mov rax, qword [r10 + 0]
    mov qword [rbp - 5152], rax
    mov r10, [rbp - 4984]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5160], eax
    mov r10, [rbp - 4984]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5168], eax
    mov r11d, 16
    mov eax, dword [rbp - 5168]
    cdq
    idiv r11d
    mov dword [rbp - 5176], eax
    mov r11d, 16
    mov eax, dword [rbp - 5176]
    imul eax, r11d
    mov dword [rbp - 5184], eax
    mov eax, dword [rbp - 5160]
    mov r11d, dword [rbp - 5184]
    sub eax, r11d
    mov dword [rbp - 5192], eax
    mov r11d, 1
    mov eax, dword [rbp - 5192]
    imul eax, r11d
    mov dword [rbp - 5200], eax
    mov rax, qword [rbp - 5152]
    movsxd r11, dword [rbp - 5200]
    add rax, r11
    mov qword [rbp - 5208], rax
    mov r10, [rbp - 5208]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 5216], al
    mov r11, [rbp - 5144]
    add r11, 0
    mov al, byte [rbp - 5216]
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5224], eax
    mov eax, dword [rbp - 5224]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5232], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 5232]
    mov dword [r11], eax
    jmp ._start_ifcont27
._start_else26:
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5240], eax
    mov r11d, 1
    mov eax, dword [rbp - 5240]
    imul eax, r11d
    mov dword [rbp - 5248], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5248]
    add rax, r11
    mov qword [rbp - 5256], rax
    mov r11, [rbp - 5256]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5264], eax
    mov eax, dword [rbp - 5264]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5272], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 5272]
    mov dword [r11], eax
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5280], eax
    mov r11d, 1
    mov eax, dword [rbp - 5280]
    imul eax, r11d
    mov dword [rbp - 5288], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5288]
    add rax, r11
    mov qword [rbp - 5296], rax
    mov r11, [rbp - 5296]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5304], eax
    mov eax, dword [rbp - 5304]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5312], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 5312]
    mov dword [r11], eax
    jmp ._start_ifcont27
._start_ifcont27:
    mov r10, [rbp - 4936]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5320], eax
    mov eax, dword [rbp - 5320]
    mov r11d, 7
    cmp eax, r11d
    sete al
    movzx rax, al
    mov [rbp - 5328], rax
    mov rax, [rbp - 5328]
    cmp rax, 0
    jne ._start_then28
    jmp ._start_else29
._start_then28:
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5336], eax
    mov r11d, 1
    mov eax, dword [rbp - 5336]
    imul eax, r11d
    mov dword [rbp - 5344], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5344]
    add rax, r11
    mov qword [rbp - 5352], rax
    mov r11, [rbp - 5352]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5360], eax
    mov eax, dword [rbp - 5360]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5368], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 5368]
    mov dword [r11], eax
    jmp ._start_ifcont30
._start_else29:
    jmp ._start_ifcont30
._start_ifcont30:
    mov r10, [rbp - 4936]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5376], eax
    mov eax, dword [rbp - 5376]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5384], eax
    mov r11, [rbp - 4936]
    add r11, 0
    mov eax, dword [rbp - 5384]
    mov dword [r11], eax
    jmp ._start_while_header22
._start_while_exit24:
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5392], eax
    mov r11d, 1
    mov eax, dword [rbp - 5392]
    imul eax, r11d
    mov dword [rbp - 5400], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5400]
    add rax, r11
    mov qword [rbp - 5408], rax
    mov r11, [rbp - 5408]
    add r11, 0
    mov al, 32
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5416], eax
    mov eax, dword [rbp - 5416]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5424], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 5424]
    mov dword [r11], eax
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5432], eax
    mov r11d, 1
    mov eax, dword [rbp - 5432]
    imul eax, r11d
    mov dword [rbp - 5440], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5440]
    add rax, r11
    mov qword [rbp - 5448], rax
    mov r11, [rbp - 5448]
    add r11, 0
    mov al, 124
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5456], eax
    mov eax, dword [rbp - 5456]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5464], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 5464]
    mov dword [r11], eax
    mov r11, [rbp - 4936]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    jmp ._start_while_header31
._start_while_header31:
    mov r10, [rbp - 4936]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5472], eax
    mov r10, [rbp - 4640]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5480], eax
    mov eax, dword [rbp - 5472]
    mov r11d, dword [rbp - 5480]
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 5488], rax
    mov rax, [rbp - 5488]
    cmp rax, 0
    jne ._start_while_body32
    jmp ._start_while_exit33
._start_while_body32:
    lea r10, [rbp - 4352]
    mov qword [rbp - 5496], r10
    mov r10, [rbp - 4576]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5504], eax
    mov r10, [rbp - 4936]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5512], eax
    mov eax, dword [rbp - 5504]
    mov r11d, dword [rbp - 5512]
    add eax, r11d
    mov dword [rbp - 5520], eax
    mov r11d, 1
    mov eax, dword [rbp - 5520]
    imul eax, r11d
    mov dword [rbp - 5528], eax
    mov rax, qword [rbp - 4512]
    movsxd r11, dword [rbp - 5528]
    add rax, r11
    mov qword [rbp - 5536], rax
    mov r10, [rbp - 5536]
    movzx eax, byte [r10 + 0]
    mov byte [rbp - 5544], al
    mov r11, [rbp - 5496]
    add r11, 0
    mov al, byte [rbp - 5544]
    mov byte [r11], al
    mov r10, [rbp - 5496]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5552], eax
    mov eax, dword [rbp - 5552]
    mov r11d, 32
    cmp eax, r11d
    setge al
    movzx rax, al
    mov [rbp - 5560], rax
    mov rax, [rbp - 5560]
    cmp rax, 0
    jne ._start_then34
    jmp ._start_else35
._start_then34:
    mov r10, [rbp - 5496]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5568], eax
    mov eax, dword [rbp - 5568]
    mov r11d, 127
    cmp eax, r11d
    setl al
    movzx rax, al
    mov [rbp - 5576], rax
    mov rax, [rbp - 5576]
    cmp rax, 0
    jne ._start_then37
    jmp ._start_else38
._start_then37:
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5584], eax
    mov r11d, 1
    mov eax, dword [rbp - 5584]
    imul eax, r11d
    mov dword [rbp - 5592], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5592]
    add rax, r11
    mov qword [rbp - 5600], rax
    mov r10, [rbp - 5496]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5608], eax
    mov r11, [rbp - 5600]
    add r11, 0
    mov eax, dword [rbp - 5608]
    mov dword [r11], eax
    jmp ._start_ifcont39
._start_else38:
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5616], eax
    mov r11d, 1
    mov eax, dword [rbp - 5616]
    imul eax, r11d
    mov dword [rbp - 5624], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5624]
    add rax, r11
    mov qword [rbp - 5632], rax
    mov r11, [rbp - 5632]
    add r11, 0
    mov al, 46
    mov byte [r11], al
    jmp ._start_ifcont39
._start_ifcont39:
    jmp ._start_ifcont36
._start_else35:
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5640], eax
    mov r11d, 1
    mov eax, dword [rbp - 5640]
    imul eax, r11d
    mov dword [rbp - 5648], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5648]
    add rax, r11
    mov qword [rbp - 5656], rax
    mov r11, [rbp - 5656]
    add r11, 0
    mov al, 46
    mov byte [r11], al
    jmp ._start_ifcont36
._start_ifcont36:
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5664], eax
    mov eax, dword [rbp - 5664]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5672], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 5672]
    mov dword [r11], eax
    mov r10, [rbp - 4936]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5680], eax
    mov eax, dword [rbp - 5680]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5688], eax
    mov r11, [rbp - 4936]
    add r11, 0
    mov eax, dword [rbp - 5688]
    mov dword [r11], eax
    jmp ._start_while_header31
._start_while_exit33:
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5696], eax
    mov r11d, 1
    mov eax, dword [rbp - 5696]
    imul eax, r11d
    mov dword [rbp - 5704], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5704]
    add rax, r11
    mov qword [rbp - 5712], rax
    mov r11, [rbp - 5712]
    add r11, 0
    mov al, 124
    mov byte [r11], al
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5720], eax
    mov eax, dword [rbp - 5720]
    mov r11d, 1
    add eax, r11d
    mov dword [rbp - 5728], eax
    mov r11, [rbp - 4680]
    add r11, 0
    mov eax, dword [rbp - 5728]
    mov dword [r11], eax
    mov r10, [rbp - 4680]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5736], eax
    mov r11d, 1
    mov eax, dword [rbp - 5736]
    imul eax, r11d
    mov dword [rbp - 5744], eax
    mov rax, qword [rbp - 4672]
    movsxd r11, dword [rbp - 5744]
    add rax, r11
    mov qword [rbp - 5752], rax
    mov r11, [rbp - 5752]
    add r11, 0
    mov eax, 0
    mov dword [r11], eax
    mov rax, qword [rbp - 4672]
    mov qword [rbp - 5760], rax
    push [rbp - 5760]
    pop rdi
    call puts
    mov qword [rbp - 5768], rax
    mov r10, [rbp - 4520]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5776], eax
    mov eax, dword [rbp - 5776]
    mov r11d, 16
    add eax, r11d
    mov dword [rbp - 5784], eax
    mov r11, [rbp - 4520]
    add r11, 0
    mov eax, dword [rbp - 5784]
    mov dword [r11], eax
    mov r10, [rbp - 4576]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5792], eax
    mov eax, dword [rbp - 5792]
    mov r11d, 16
    add eax, r11d
    mov dword [rbp - 5800], eax
    mov r11, [rbp - 4576]
    add r11, 0
    mov eax, dword [rbp - 5800]
    mov dword [r11], eax
    jmp ._start_while_header13
._start_while_exit15:
    jmp ._start_while_header6
._start_while_exit8:
    mov r10, [rbp - 4416]
    mov eax, dword [r10 + 0]
    mov dword [rbp - 5808], eax
    push [rbp - 5808]
    pop rdi
    call close
    mov dword [rbp - 5816], eax
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
