; Hardware IRQ stubs (PIC lines 0–15 -> IDT vectors 32–47)
; plus CPU exception stubs (vectors 0–31) that print then halt.

%macro IRQ 1
global irq_stub_%1
irq_stub_%1:
    push qword 0
    push qword %1
    jmp irq_common
%endmacro

; Exception with no CPU error code — push a dummy so the frame matches.
%macro EXC 1
global exc_stub_%1
exc_stub_%1:
    push qword 0
    push qword %1
    jmp exc_common
%endmacro

; Exception that already pushed an error code.
%macro EXC_ERR 1
global exc_stub_%1
exc_stub_%1:
    push qword %1
    jmp exc_common
%endmacro

extern irq_dispatch
extern exception_panic

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

EXC 0
EXC 1
EXC 2
EXC 3
EXC 4
EXC 5
EXC 6
EXC 7
EXC_ERR 8
EXC 9
EXC_ERR 10
EXC_ERR 11
EXC_ERR 12
EXC_ERR 13
EXC_ERR 14
EXC 15
EXC 16
EXC_ERR 17
EXC 18
EXC 19
EXC 20
EXC_ERR 21
EXC 22
EXC 23
EXC 24
EXC 25
EXC 26
EXC 27
EXC 28
EXC 29
EXC_ERR 30
EXC 31

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

; Stack on entry: [vec][err][RIP][CS][RFLAGS][RSP][SS]
; SysV: rdi, rsi, rdx, rcx
exc_common:
    cli
    mov rdi, [rsp]
    mov rsi, [rsp + 8]
    mov rdx, [rsp + 16]
    mov rcx, cr2
    call exception_panic
.hang:
    hlt
    jmp .hang

section .bss
align 16
irq_stack:
    resb 8192
irq_stack_end:
