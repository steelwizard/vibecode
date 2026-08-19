# FOS — Flash Operating System

A tiny x86-64 OS: **assembly bootloader**, **C kernel**, DOS-style shell with **FAT32** and **exFAT** support.

## Features

- Drive letters: `0:\`, `1:\`, … (boot disk is always **0:**)
- Paths use **backslashes** (`0:\folder\file.txt`)
- FAT32 and exFAT read/write (list dirs, read/write files)
- **FOSCOM** `.COM` executables with kernel API
- Shell pipes (`|`) and output redirect (`>`)
- ATA PIO block layer for up to 4 drives

## Shell commands

| Command | Description |
|---------|-------------|
| `dir [path]` | List directory |
| `cd path` | Change directory on current drive |
| `type path` | Print file (or piped input) |
| `drives` / `df` | Show disks and volumes |
| `echo args` | Run `echo.com` |
| `1:` / `0:` | Switch current drive |
| `help` `ver` `cls` `reboot` | Built-ins |

Prompt shows current location, e.g. `0:\>` or `0:\docs>`.

## Build & Run

```bash
make          # builds boot.img (FAT32 on 0:) + data.img (exFAT on 1:)
make run      # QEMU with -serial stdio (type in the terminal)
```

**Keyboard note:** `make run` uses `-serial stdio`, so your terminal keystrokes go to **COM1 serial**, not PS/2. The kernel reads both.

Requires: **nasm**, **gcc**, **ld**, **objcopy**, **sfdisk**, **mkfs.vfat**, **qemu-system-x86_64**  
Optional: **mkfs.exfat**, **mtools**, **python3** (packs `.COM` files onto the boot volume)

## Disk layout

```
boot.img (drive 0 / boot disk)
  LBA 0      MBR + stage1
  LBA 1–8    stage2 (long mode)
  LBA 9+     kernel.bin
  LBA 2048+  FAT32 partition (KERNEL.BIN, SHELL.COM, ECHO.COM, …)

data.img (drive 1, second QEMU disk)
  LBA 2048+  exFAT (or FAT32 if mkfs.exfat missing)
```

## Source layout

```
fos/
├── boot_stage1.asm / boot_stage2.asm   # Bootloader (asm)
├── kernel/
│   ├── entry.asm          # call kmain()
│   ├── kernel.c           # init
│   ├── shell.c            # Command shell
│   ├── exec.c / fos_api.c # .COM loader + program API
│   ├── vfs.c              # Drive letters, paths
│   ├── block.c            # ATA disk I/O
│   ├── fat32.c / exfat.c  # Filesystems
│   └── include/
├── apps/echo/             # echo.com sample program
├── scripts/               # mkdisk.sh, foscom_pack.py, …
└── Makefile
```

All OS logic is **C**. Only the bootloader and short `entry.asm` stubs are assembly.
