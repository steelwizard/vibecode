extern com_main
global _start

section .text.entry
_start:
    call com_main
    ret
