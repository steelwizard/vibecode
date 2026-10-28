extern com_main
global _start

section .text.entry
_start:
    ; com_call enters with the SysV alignment (RSP % 16 == 8). One more
    ; call would hand com_main an 8-off stack, and gcc's movaps faults.
    push rax
    call com_main
    pop rcx
    ret
