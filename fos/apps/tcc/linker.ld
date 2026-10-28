ENTRY(_start)

COM_BASE = 0x300000;

SECTIONS
{
    . = COM_BASE;

    .text : ALIGN(16) {
        *(.text.entry)
        *(.text*)
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
    }

    .data : ALIGN(16) {
        *(.data*)
    }

    .bss : ALIGN(16) {
        *(COMMON)
        *(.bss*)
    }

    _end = .;
}
