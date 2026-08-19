; boot_serial.inc — COM1 (0x3F8) output for boot logging in real mode
; Works alongside INT 10h VGA text so QEMU -serial stdio shows boot messages.

%define COM1_DATA 0x3F8
%define COM1_LSR  0x3FD

; Init COM1 to 38400 8N1 (divisor 3 for 115200 base — standard PC UART)
serial_init16:
    mov dx, COM1_DATA + 1
    mov al, 0
    out dx, al              ; disable all interrupts

    mov dx, COM1_DATA + 3
    mov al, 0x80
    out dx, al              ; DLAB on — set baud divisor

    mov dx, COM1_DATA
    mov al, 3
    out dx, al              ; divisor low (38400)

    mov dx, COM1_DATA + 1
    xor al, al
    out dx, al              ; divisor high

    mov dx, COM1_DATA + 3
    mov al, 0x03
    out dx, al              ; 8N1, DLAB off

    mov dx, COM1_DATA + 2
    mov al, 0xC7
    out dx, al              ; enable FIFO, clear, 14-byte threshold

    mov dx, COM1_DATA + 4
    mov al, 0x0B
    out dx, al              ; IRQ enabled, RTS/DSR set
    ret

; AL = character — blocks until transmitter ready
serial_putc16:
    push dx
    push ax
    mov dx, COM1_LSR
.wait:
    in al, dx
    test al, 0x20           ; THRE — transmit holding register empty
    jz .wait
    pop ax
    pop dx
    push dx
    mov dx, COM1_DATA
    out dx, al
    pop dx
    ret

; SI = null-terminated string — sends to both serial and VGA (INT 10h)
print_both16:
    mov ah, 0x0E
    mov bh, 0
.next:
    lodsb
    test al, al
    jz .done
    int 0x10
    call serial_putc16
    jmp .next
.done:
    ret
