; Kernel entry — called by bootloader at physical 0x100000.
; Sets stack and jumps into C (kmain). Interrupts stay off (no IDT yet).

section .text.entry
global _start
extern kmain

_start:
    cli
    mov rsp, 0x90000
    call kmain
.hang:
    hlt
    jmp .hang
