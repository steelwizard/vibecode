OUTPUT_FORMAT("pei-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(efi_main)

/* PE headers occupy ImageBase .. ImageBase+0xFFF; first section at +0x1000. */
SECTIONS
{
    . = 0x140001000;
    .text : {
        *(.text)
        *(.text.*)
    }
    . = ALIGN(0x1000);
    .data : {
        *(.rdata)
        *(.rdata.*)
        *(.rodata)
        *(.rodata.*)
        *(.data)
        *(.data.*)
    }
    . = ALIGN(0x1000);
    .bss : {
        *(.bss)
        *(COMMON)
    }
    /DISCARD/ : {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.idata)
        *(.pdata)
        *(.xdata)
    }
}
