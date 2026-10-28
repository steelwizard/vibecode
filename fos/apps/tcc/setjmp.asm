; setjmp/longjmp for the SysV x86-64 ABI.
; jmp_buf is long[32]:
;   0  rbx rbp r12 r13 r14 r15
;  48 rsp after return
;  56 rip
;  64 xmm6..xmm15

global setjmp
global longjmp

section .text

setjmp:
    mov [rdi + 0], rbx
    mov [rdi + 8], rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rax, [rsp + 8]
    mov [rdi + 48], rax
    mov rax, [rsp]
    mov [rdi + 56], rax
    movdqu [rdi + 64], xmm6
    movdqu [rdi + 80], xmm7
    movdqu [rdi + 96], xmm8
    movdqu [rdi + 112], xmm9
    movdqu [rdi + 128], xmm10
    movdqu [rdi + 144], xmm11
    movdqu [rdi + 160], xmm12
    movdqu [rdi + 176], xmm13
    movdqu [rdi + 192], xmm14
    movdqu [rdi + 208], xmm15
    xor eax, eax
    ret

longjmp:
    mov eax, esi
    test eax, eax
    jnz .ok
    mov eax, 1
.ok:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    movdqu xmm6, [rdi + 64]
    movdqu xmm7, [rdi + 80]
    movdqu xmm8, [rdi + 96]
    movdqu xmm9, [rdi + 112]
    movdqu xmm10, [rdi + 128]
    movdqu xmm11, [rdi + 144]
    movdqu xmm12, [rdi + 160]
    movdqu xmm13, [rdi + 176]
    movdqu xmm14, [rdi + 192]
    movdqu xmm15, [rdi + 208]
    mov rsp, [rdi + 48]
    jmp qword [rdi + 56]
