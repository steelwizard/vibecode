extern com_main
global _start

section .text.entry
_start:
    push rbp
    mov rbp, rsp
    and rsp, ~0xF
    call com_main
    mov rsp, rbp
    pop rbp
    ret
