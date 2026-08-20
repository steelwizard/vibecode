; com_call — run a .COM entry point on a stack of its own.
;
; void com_call(void (*entry)(void), uint64_t stack_top);
;
; The kernel RSP to come back to is parked in memory rather than a register,
; so a program that corrupts every register still returns to a valid kernel
; stack. Nested calls (api->run_com) chain the slot through the kernel stack.

global com_call

section .text

com_call:
    push rbp
    push rbx
    mov  rbx, [rel saved_rsp]
    push rbx
    mov  [rel saved_rsp], rsp

    mov  rsp, rsi
    and  rsp, ~0xF                  ; call below leaves the SysV entry alignment
    xor  ebp, ebp
    call rdi

    mov  rsp, [rel saved_rsp]
    pop  rbx
    mov  [rel saved_rsp], rbx       ; hand the slot back to the outer level
    pop  rbx
    pop  rbp
    ret

section .bss
align 8
saved_rsp:
    resq 1
