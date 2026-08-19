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

    mov si, msg_jump_s2
    call print_both16
    jmp STAGE2_LOAD_ADDR

disk_fail:
    mov si, msg_disk_err
    call print_both16
halt:
    hlt
    jmp halt

msg_stage1  db 13, 10, '[boot] stage1: MBR loaded at 0x7C00', 13, 10, 0
msg_load_s2 db '[boot] stage1: reading stage2 to 0x8000...', 13, 10, 0
msg_jump_s2 db '[boot] stage1: jumping to stage2', 13, 10, 0
msg_disk_err db '[boot] ERROR: disk read failed', 13, 10, 0

%include "boot_serial.inc"

times 510 - ($ - $$) db 0
dw 0xAA55
