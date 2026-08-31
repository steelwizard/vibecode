; Shared boot constants
;
; Memory map (real mode):
;   0x7C00   stage1 MBR
;   0x8000   stage2 (protected/long-mode trampoline)
;   0x10000  kernel staging (64K-aligned, loaded in chunks)
;   0x100000 final kernel load address (1 MiB)

%define BOOT_DRIVE_ADDR 0x7DFF

%define STAGE2_LOAD_ADDR 0x8000
%define STAGE2_SECTORS   8
%define STAGE2_LBA       1

%define KERNEL_STAGE_ADDR  0x10000
%define KERNEL_RUNTIME     0x100000
%define KERNEL_LBA         9
; Must fit the full kernel.bin — if the kernel grows, increase this and rebuild boot.bin.
%define KERNEL_MAX_SECTORS 288
; BIOS int 13h reads: stay well under 127 sectors and 64K DMA windows.
%define KERNEL_CHUNK_SECTORS 32

%define MEM_MAP_ADDR   0x4000
%define MEM_MAP_MAGIC  0x50414D4D
%define MEM_MAP_MAX    32

%define PML4_ADDR 0x5000
%define PDPT_ADDR 0x6000
%define PD_ADDR   0x7000
; 256 × 2 MiB = 512 MiB identity map. Slot 256 (0x20000000) is the framebuffer.
%define IDENTITY_2M_PAGES 256

%define CODE32_SEL 0x08
%define DATA32_SEL 0x10
%define CODE64_SEL 0x18
%define DATA64_SEL 0x20
