; Shared boot constants

%define BOOT_DRIVE_ADDR 0x7DFF

%define STAGE2_LOAD_ADDR 0x8000
%define STAGE2_SECTORS   8
%define STAGE2_LBA       1

%define KERNEL_STAGE_ADDR  0x9000
%define KERNEL_RUNTIME     0x100000
%define KERNEL_LBA         9
%define KERNEL_MAX_SECTORS 128

%define PML4_ADDR 0x5000
%define PDPT_ADDR 0x6000
%define PD_ADDR   0x7000

%define CODE32_SEL 0x08
%define DATA32_SEL 0x10
%define CODE64_SEL 0x18
%define DATA64_SEL 0x20
