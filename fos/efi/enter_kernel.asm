; Low-memory UEFI trampoline. Copied to 0x8100, then jumped to *before*
; switching CR3 so we are not executing from the EFI image (usually ~5 GiB).
;
; Expects: identity map still active (OVMF), GDT already copied to 0x8000,
;          page tables already filled at 0x5000/0x6000/0x7000.

        default abs
        org     0x8100
        bits    64

        lgdt    [gdtr]
        mov     rax, 0x5000
        mov     cr3, rax
        mov     ax, 0x20
        mov     ds, ax
        mov     es, ax
        mov     ss, ax
        mov     fs, ax
        mov     gs, ax
        xor     rbp, rbp
        mov     rsp, 0x90000
        push    0x18                 ; CS
        mov     rax, 0x100000        ; kernel entry
        push    rax                  ; RIP
        retfq

        align   8
gdtr:
        dw      5 * 8 - 1
        dq      0x8000
