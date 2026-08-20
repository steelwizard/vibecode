%include "boot.inc"

; Stage 1 — MBR @ 0x7C00
; Entry point MUST be first byte of the sector — include serial helpers at end.

[bits 16]
[org 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [BOOT_DRIVE_ADDR], dl

    mov ax, 0x0003
    int 0x10
    call serial_init16

    mov si, msg_stage1
    call print_both16

    mov si, msg_load_s2
    call print_both16

    ; Prefer EDD LBA (AH=42h); fall back to CHS for ancient BIOS.
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    jc .chs
    cmp bx, 0xAA55
    jne .chs

    mov si, dap
    mov ah, 0x42
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    jc disk_fail
    jmp .ok

.chs:
    mov ax, STAGE2_LOAD_ADDR >> 4
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, STAGE2_SECTORS
    mov ch, 0
    mov dh, 0
    mov cl, STAGE2_LBA + 1
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    jc disk_fail

.ok:
    mov si, msg_jump_s2
    call print_both16
    jmp STAGE2_LOAD_ADDR

disk_fail:
    mov si, msg_disk_err
    call print_both16
halt:
    hlt
    jmp halt

align 4
dap:
    db 16
    db 0
    dw STAGE2_SECTORS
    dw 0
    dw STAGE2_LOAD_ADDR >> 4
    dq STAGE2_LBA

msg_stage1  db 13, 10, '[boot] stage1', 13, 10, 0
msg_load_s2 db '[boot] load stage2', 13, 10, 0
msg_jump_s2 db '[boot] -> stage2', 13, 10, 0
msg_disk_err db '[boot] disk read fail', 13, 10, 0

%include "boot_serial.inc"

; Leave room for the partition table at 0x1BE–0x1FD.
times 0x1BE - ($ - $$) db 0
times 510 - ($ - $$) db 0
dw 0xAA55
