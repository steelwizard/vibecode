; Hello World OS - x86 Real Mode Bootloader
; Assembled with NASM, runs as a raw disk image in QEMU
;
; BIOS loads this 512-byte sector to 0x7C00 and jumps to it.
; We use BIOS INT 10h (teletype output) to print to the screen,
; then spin forever in a halt loop.

[bits 16]
[org 0x7C00]

start:
    ; Clear segment registers and set up a small stack below us
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    ; Set video mode 3 (80x25 colour text) for a clean screen
    mov ax, 0x0003
    int 0x10

    ; Print the greeting string
    mov si, msg_hello
    call print_string

    ; Print a second line for fun
    mov si, msg_info
    call print_string

halt:
    hlt
    jmp halt

; ------------------------------------------------------------------
; print_string: prints a null-terminated string pointed to by SI
;   Uses BIOS INT 10h, AH=0Eh (teletype output)
; ------------------------------------------------------------------
print_string:
    mov ah, 0x0E        ; teletype output function
    mov bh, 0x00        ; page 0
    mov bl, 0x0F        ; bright white on black
.next_char:
    lodsb               ; AL = [SI], SI++
    test al, al         ; null terminator?
    jz .done
    int 0x10
    jmp .next_char
.done:
    ret

; ------------------------------------------------------------------
; Data
; ------------------------------------------------------------------
msg_hello   db 'Hello, World!', 13, 10, 0
msg_info    db 'Running in QEMU -- press Ctrl+A then X to quit', 13, 10, 0

; ------------------------------------------------------------------
; Pad to 510 bytes and append the required MBR boot signature
; ------------------------------------------------------------------
times 510 - ($ - $$) db 0
dw 0xAA55
