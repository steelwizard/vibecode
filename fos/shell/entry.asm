; Shell executable entry — loaded by the kernel at 0x200000 (future).
; For now the kernel calls shell_run() directly; this file is the on-disk image.

section .text.entry
global _start
extern shell_run

_start:
    call shell_run
.hang:
    hlt
    jmp .hang
