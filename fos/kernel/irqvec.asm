; Hardware IRQ stubs (PIC lines 0–15 -> IDT vectors 32–47).

%macro IRQ 1
global irq_stub_%1
irq_stub_%1:
    push qword 0
    push qword %1
    jmp irq_common
%endmacro

extern irq_dispatch
extern exc_halt_stub

section .text

IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

irq_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    cld

    ; Handlers run on a kernel stack of their own so that a .COM never has to
    ; leave headroom for interrupt frames. Interrupt gates keep IF clear, but
    ; skip the switch if we are somehow already on it rather than trample it.
    mov rdi, [rsp + 15 * 8]
    mov rax, rsp
    lea rbx, [rel irq_stack]
    cmp rax, rbx
    jb .switch
    lea rbx, [rel irq_stack_end]
    cmp rax, rbx
    jb .dispatch
.switch:
    lea rsp, [rel irq_stack_end]
    and rsp, ~0xF
.dispatch:
    push rax
    call irq_dispatch
    pop rsp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

exc_halt_stub:
    cli
.hang:
    hlt
    jmp .hang

section .bss
align 16
irq_stack:
    resb 8192
irq_stack_end:
