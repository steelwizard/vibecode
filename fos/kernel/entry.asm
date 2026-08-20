; Kernel entry — called by bootloader at physical 0x100000.
; Zero .bss (the BIOS copy is only KERNEL_MAX_SECTORS), set stack, kmain.

section .text.entry
global _start
extern kmain
extern __bss_start
extern _end

_start:
    cli
    mov rsp, 0x90000
    lea rdi, [rel __bss_start]
    lea rcx, [rel _end]
    xor eax, eax
    sub rcx, rdi
    add rcx, 7
    shr rcx, 3
    jz .bss_done
    rep stosq
.bss_done:
    call kmain
.hang:
    hlt
    jmp .hang
