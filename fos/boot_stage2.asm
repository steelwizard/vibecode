%include "boot.inc"

; Stage 2 @ 0x8000 — load kernel, enter long mode, jump to C kernel at 1 MiB

[bits 16]
[org 0x8000]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x8000

    call serial_init16

    mov si, msg_stage2
    call print_both16

    mov si, msg_load_kern
    call print_both16

    mov ax, KERNEL_STAGE_ADDR >> 4
    mov es, ax
    xor bx, bx
    mov ah, 0x02
    mov al, KERNEL_MAX_SECTORS
    mov ch, 0
    mov dh, 0
    mov cl, KERNEL_LBA + 1
    mov dl, [BOOT_DRIVE_ADDR]
    int 0x13
    jc disk_fail

    mov si, msg_kern_ok
    call print_both16

    mov si, msg_a20
    call print_both16
    call enable_a20

    mov si, msg_gdt
    call print_both16
    lgdt [gdt_descriptor]

    cli
    mov si, msg_pm
    call print_both16

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE32_SEL:protected_mode

disk_fail:
    mov si, msg_disk_err
    call print_both16
halt:
    hlt
    jmp halt

enable_a20:
    in al, 0x92
    or al, 2
    out 0x92, al
    ret

; Serial helpers must live in this real-mode section (before [bits 32])
%include "boot_serial.inc"

[bits 32]
protected_mode:
    mov ax, DATA32_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    mov esi, KERNEL_STAGE_ADDR
    mov edi, KERNEL_RUNTIME
    mov ecx, KERNEL_MAX_SECTORS * 512 / 4
    rep movsd

    mov edi, PML4_ADDR
    xor eax, eax
    mov ecx, 4096 / 4
    rep stosd

    mov edi, PDPT_ADDR
    mov ecx, 4096 / 4
    rep stosd

    mov edi, PD_ADDR
    mov ecx, 4096 / 4
    rep stosd

    mov dword [PML4_ADDR], PDPT_ADDR + 0x03
    mov dword [PDPT_ADDR], PD_ADDR + 0x03

    xor ecx, ecx
.map_loop:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    mov [PD_ADDR + ecx * 8], eax
    inc ecx
    cmp ecx, 32
    jb .map_loop

    mov eax, PML4_ADDR
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    jmp CODE64_SEL:long_mode

[bits 64]
long_mode:
    mov ax, DATA64_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x90000
    mov rax, KERNEL_RUNTIME
    jmp rax

align 8
gdt:
    dq 0
    dw 0xFFFF, 0x0000, 0x9A00, 0x00CF
    dw 0xFFFF, 0x0000, 0x9200, 0x00CF
    dq 0x00209A0000000000
    dq 0x0000920000000000
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt - 1
    dd gdt

msg_stage2   db '[boot] stage2: entered at 0x8000', 13, 10, 0
msg_load_kern db '[boot] stage2: loading kernel from disk...', 13, 10, 0
msg_kern_ok  db '[boot] stage2: kernel read OK', 13, 10, 0
msg_a20      db '[boot] stage2: enabling A20', 13, 10, 0
msg_gdt      db '[boot] stage2: loading GDT', 13, 10, 0
msg_pm       db '[boot] stage2: entering protected mode', 13, 10, 0
msg_disk_err db '[boot] ERROR: disk read failed', 13, 10, 0

times (STAGE2_SECTORS * 512) - ($ - $$) db 0
