/*
 * efi_main.c — UEFI loader: read \KERNEL.BIN, identity-map low RAM, jump
 * to the same kernel entry the BIOS stage2 uses (physical 0x100000).
 *
 * Linked as a PE32+ EFI application (\EFI\BOOT\BOOTX64.EFI).
 */

#include "efi.h"
#include "enter_kernel_bin.h"

#define KERNEL_ADDR   0x100000ULL
#ifndef KERNEL_MAX_SECTORS
#define KERNEL_MAX_SECTORS 256u
#endif
#define KERNEL_MAX    ((UINTN)KERNEL_MAX_SECTORS * 512u)
#define MEM_MAP_ADDR  0x4000ULL
#define MEM_MAP_MAGIC 0x50414D4Du
#define MEM_MAP_MAX   32u
#define PML4_ADDR     0x5000ULL
#define PDPT_ADDR     0x6000ULL
#define PD_ADDR       0x7000ULL
#define IDENTITY_2M_PAGES 256u
#define GDT_ADDR      0x8000ULL
#define TRAMPOLINE    0x8100ULL

static inline void outb(UINT16 port, UINT8 value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline UINT8 inb(UINT16 port) {
    UINT8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void) {
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x80);
    outb(0x3F8, 0x01);
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x03);
    outb(0x3FA, 0xC7);
    outb(0x3FC, 0x0B);
}

static void serial_putc(char c) {
    int spins = 100000;
    while ((inb(0x3FD) & 0x20) == 0 && spins-- > 0) {
    }
    if (c == '\n') {
        outb(0x3F8, '\r');
    }
    outb(0x3F8, (UINT8)c);
}

static void serial_write(const char *s) {
    while (*s) {
        serial_putc(*s++);
    }
}

static void efi_print(EFI_SYSTEM_TABLE *st, CHAR16 *s) {
    if (st && st->ConOut && st->ConOut->OutputString) {
        st->ConOut->OutputString(st->ConOut, s);
    }
}

static void *memset_local(void *dst, int value, UINTN n) {
    UINT8 *p = dst;
    while (n--) {
        *p++ = (UINT8)value;
    }
    return dst;
}

static void *memcpy_local(void *dst, const void *src, UINTN n) {
    UINT8 *d = dst;
    const UINT8 *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/* Same layout as boot_stage2.asm so kernel IDT selector 0x18 is 64-bit CS. */
static UINT64 gdt_src[] __attribute__((aligned(8))) = {
    0x0000000000000000ULL,
    0x00CF9A000000FFFFULL, /* 0x08 32-bit code */
    0x00CF92000000FFFFULL, /* 0x10 32-bit data */
    0x00209A0000000000ULL, /* 0x18 64-bit code */
    0x0000920000000000ULL  /* 0x20 64-bit data */
};

static EFI_PHYSICAL_ADDRESS kernel_src;
static UINTN kernel_bytes;

static void enter_kernel(void) {
    volatile UINT64 *pml4 = (volatile UINT64 *)PML4_ADDR;
    volatile UINT64 *pdpt = (volatile UINT64 *)PDPT_ADDR;
    volatile UINT64 *pd = (volatile UINT64 *)PD_ADDR;
    UINT32 i;

    if (kernel_src != KERNEL_ADDR) {
        memcpy_local((void *)(UINTN)KERNEL_ADDR, (void *)(UINTN)kernel_src, kernel_bytes);
    }

    memset_local((void *)PML4_ADDR, 0, 4096);
    memset_local((void *)PDPT_ADDR, 0, 4096);
    memset_local((void *)PD_ADDR, 0, 4096);
    memcpy_local((void *)GDT_ADDR, gdt_src, sizeof(gdt_src));
    memcpy_local((void *)TRAMPOLINE, enter_kernel_bin, enter_kernel_bin_len);

    pml4[0] = PDPT_ADDR | 0x03;
    pdpt[0] = PD_ADDR | 0x03;
    for (i = 0; i < IDENTITY_2M_PAGES; i++) {
        pd[i] = ((UINT64)i << 21) | 0x83;
    }

    /* Jump to low memory first — CR3 would unmap this EFI image. */
    __asm__ volatile("cli; jmp *%0" : : "r"(TRAMPOLINE) : "memory");
    __builtin_unreachable();
}

static EFI_STATUS alloc_low(EFI_BOOT_SERVICES *bs, EFI_PHYSICAL_ADDRESS addr, UINTN pages) {
    EFI_PHYSICAL_ADDRESS got = addr;
    return bs->AllocatePages(AllocateAddress, EfiLoaderData, pages, &got);
}

static EFI_STATUS load_kernel(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    EFI_FILE_PROTOCOL *root = 0;
    EFI_FILE_PROTOCOL *file = 0;
    EFI_STATUS status;
    CHAR16 path[] = u"\\KERNEL.BIN";
    UINT8 info_buf[256];
    UINTN info_sz;
    EFI_FILE_INFO *info;
    EFI_PHYSICAL_ADDRESS addr;
    UINTN pages;
    UINTN read_sz;
    EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID info_guid = EFI_FILE_INFO_ID;

    /* E820-style map at 0x4000; page tables + GDT at 0x5000..0x8FFF.
     * The kernel stack grows down from 0x90000 and runs ~13 KB deep before it
     * starts a .COM, so reserve 32 KB rather than the depth of kmain alone. */
    (void)alloc_low(bs, MEM_MAP_ADDR, 1);
    (void)alloc_low(bs, PML4_ADDR, 4);
    (void)alloc_low(bs, 0x88000ULL, 8);

    status = bs->HandleProtocol(image, &loaded_guid, (void **)&loaded);
    if (status != EFI_SUCCESS || !loaded || !loaded->DeviceHandle) {
        serial_write("[uefi] no loaded image protocol\n");
        return status ? status : EFI_LOAD_ERROR;
    }

    status = bs->HandleProtocol(loaded->DeviceHandle, &fs_guid, (void **)&fs);
    if (status != EFI_SUCCESS || !fs) {
        serial_write("[uefi] no file system on boot device\n");
        return status ? status : EFI_LOAD_ERROR;
    }

    status = fs->OpenVolume(fs, &root);
    if (status != EFI_SUCCESS || !root) {
        serial_write("[uefi] OpenVolume failed\n");
        return status;
    }

    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS || !file) {
        serial_write("[uefi] cannot open \\KERNEL.BIN\n");
        return status ? status : EFI_NOT_FOUND;
    }

    info_sz = sizeof(info_buf);
    status = file->GetInfo(file, &info_guid, &info_sz, info_buf);
    if (status != EFI_SUCCESS) {
        serial_write("[uefi] GetInfo failed\n");
        return status;
    }
    info = (EFI_FILE_INFO *)info_buf;
    if (info->FileSize == 0 || info->FileSize > KERNEL_MAX) {
        serial_write("[uefi] KERNEL.BIN too big or empty\n");
        return EFI_LOAD_ERROR;
    }

    pages = (UINTN)((info->FileSize + 4095) / 4096);
    addr = KERNEL_ADDR;
    status = bs->AllocatePages(AllocateAddress, EfiLoaderData, pages, &addr);
    if (status != EFI_SUCCESS) {
        addr = 0;
        status = bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, &addr);
        if (status != EFI_SUCCESS) {
            serial_write("[uefi] AllocatePages for kernel failed\n");
            return status;
        }
    }

    memset_local((void *)(UINTN)addr, 0, pages * 4096);
    read_sz = (UINTN)info->FileSize;
    status = file->Read(file, &read_sz, (void *)(UINTN)addr);
    file->Close(file);
    root->Close(root);
    if (status != EFI_SUCCESS || read_sz != (UINTN)info->FileSize) {
        serial_write("[uefi] kernel read failed\n");
        return EFI_LOAD_ERROR;
    }

    kernel_src = addr;
    kernel_bytes = (UINTN)info->FileSize;
    serial_write("[uefi] KERNEL.BIN loaded\n");
    return EFI_SUCCESS;
}

static UINT32 efi_to_e820(UINT32 type) {
    switch (type) {
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiConventionalMemory:
    case EfiPersistentMemory:
        return 1;
    case EfiACPIReclaimMemory:
        return 3;
    case EfiACPIMemoryNVS:
        return 4;
    case EfiUnusableMemory:
        return 5;
    default:
        return 2;
    }
}

static void store_mem_map(void *efi_map, UINTN map_size, UINTN desc_size) {
    UINT32 *magic = (UINT32 *)MEM_MAP_ADDR;
    UINT32 *count = magic + 1;
    UINT8 *out = (UINT8 *)(MEM_MAP_ADDR + 8);
    UINT8 *p;
    UINT32 n = 0;

    memset_local((void *)MEM_MAP_ADDR, 0, 4096);
    magic[0] = MEM_MAP_MAGIC;
    if (!efi_map || desc_size < 32) {
        return;
    }

    for (p = efi_map; p < (UINT8 *)efi_map + map_size && n < MEM_MAP_MAX;
         p += desc_size) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)p;
        UINT64 base = d->PhysicalStart;
        UINT64 length = d->NumberOfPages * 4096ULL;
        UINT32 type = efi_to_e820(d->Type);
        UINT8 *slot;

        if (length == 0) {
            continue;
        }
        if (n > 0) {
            UINT8 *prev = out + (n - 1) * 24;
            UINT64 pbase = *(UINT64 *)prev;
            UINT64 plen = *(UINT64 *)(prev + 8);
            UINT32 ptype = *(UINT32 *)(prev + 16);
            if (ptype == type && pbase + plen == base) {
                *(UINT64 *)(prev + 8) = plen + length;
                continue;
            }
        }
        slot = out + n * 24;
        *(UINT64 *)slot = base;
        *(UINT64 *)(slot + 8) = length;
        *(UINT32 *)(slot + 16) = type;
        *(UINT32 *)(slot + 20) = 1;
        n++;
    }
    *count = n;
}

static EFI_STATUS exit_boot(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_BOOT_SERVICES *bs = st->BootServices;
    UINTN map_size = 0;
    UINTN map_key = 0;
    UINTN desc_size = 0;
    UINT32 desc_ver = 0;
    void *map = 0;
    EFI_STATUS status;

    bs->SetWatchdogTimer(0, 0, 0, 0);
    status = bs->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
    (void)status;
    map_size += 4096;
    status = bs->AllocatePool(EfiLoaderData, map_size, &map);
    if (status != EFI_SUCCESS) {
        return status;
    }
    status = bs->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
    if (status != EFI_SUCCESS) {
        return status;
    }
    store_mem_map(map, map_size, desc_size);
    status = bs->ExitBootServices(image, map_key);
    if (status != EFI_SUCCESS) {
        serial_write("[uefi] ExitBootServices retry\n");
        map_size += 4096;
        status = bs->GetMemoryMap(&map_size, map, &map_key, &desc_size, &desc_ver);
        if (status != EFI_SUCCESS) {
            return status;
        }
        store_mem_map(map, map_size, desc_size);
        status = bs->ExitBootServices(image, map_key);
    }
    return status;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    EFI_STATUS status;

    serial_init();
    serial_write("[uefi] FOS UEFI loader\n");
    if (st && st->ConOut && st->ConOut->ClearScreen) {
        st->ConOut->ClearScreen(st->ConOut);
    }
    efi_print(st, u"FOS UEFI loader\r\n");

    status = load_kernel(image, st);
    if (status != EFI_SUCCESS) {
        efi_print(st, u"Failed to load KERNEL.BIN\r\n");
        return status;
    }

    efi_print(st, u"ExitBootServices...\r\n");
    status = exit_boot(image, st);
    if (status != EFI_SUCCESS) {
        serial_write("[uefi] ExitBootServices failed\n");
        return status;
    }

    serial_write("[uefi] entering kernel\n");
    enter_kernel();
    return EFI_LOAD_ERROR;
}
