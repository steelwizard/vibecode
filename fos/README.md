# FOS — Flash Operating System

A small x86-64 hobby OS: assembly bootloader, C kernel, DOS-style shell, FAT32/exFAT, and FOSCOM `.COM` programs.

## Quick start

```bash
make          # boot.img (FAT32, drive 0:) + data.img (exFAT, drive 1:)
make run      # QEMU with serial console + bochs-display
```

**Requirements:** nasm, gcc, ld, objcopy, sfdisk, mkfs.vfat, qemu-system-x86_64  
**Optional:** mkfs.exfat, mtools, python3 (packs `.COM` files onto the boot volume)

### QEMU command line

`make run` is equivalent to:

```bash
qemu-system-x86_64 \
  -drive format=raw,file=boot.img,index=0,media=disk \
  -drive format=raw,file=data.img,index=1,media=disk \
  -device bochs-display \
  -m 128M \
  -serial stdio
```

Type in the terminal that launches QEMU — input goes to **COM1 serial** (`-serial stdio`), not PS/2. The kernel reads both serial and PS/2.

> **Do not use `-vga none` with `-device bochs-display`.** Under QEMU this combination reboots in a loop before the kernel starts. Keep the default VGA and add `-device bochs-display` for framebuffer modes.

## What you get

- **Drive letters** — `0:\`, `1:\`, … (boot disk is always `0:`)
- **Backslash paths** — `0:\folder\file.txt`
- **FAT32** on drive 0 — read/write, directories, `mkdir`, `del`, `copy`
- **exFAT** on drive 1 — read-only listing and file read
- **FOSCOM** — `.COM` programs call a fixed kernel API at `0xFF0000`
- **Console** — scrollback, Page Up/Down, pipes (`|`), redirect (`>`)
- **Video** — VGA text or framebuffer modes set in `SYSTEM.INI` (kernel-side, after mount)
- **Keyboard layouts** — German QWERTZ or US QWERTY via config

### Included programs

| Program | Description |
|---------|-------------|
| `SHELL.COM` | Interactive shell (also built into the kernel) |
| `ECHO.COM` | Sample FOSCOM program |
| `EDIT.COM` | Text editor — Ctrl+S save, Ctrl+X exit, arrows, Delete |
| `LESS.COM` | File pager — Space/b page, q quit (`more` alias in shell) |
| `FM.COM` | File manager browser |
| `README.TXT` | Full project README (copy of `README.md` from the repo) |

## Shell commands

| Command | Description |
|---------|-------------|
| `help` / `?` | Command list |
| `ver` | Version string |
| `cls` / `clear` | Clear screen |
| `dir [path]` | List directory |
| `cd <path>` | Change directory on current drive |
| `mkdir` / `md <path>` | Create directory (FAT32 only) |
| `del` / `erase <path>` | Delete file or empty folder (FAT32, confirms Y/N) |
| `copy <src> <dst>` | Copy file (FAT32, max 64 KiB) |
| `type <path>` | Print file (or piped input) |
| `drives` / `df` | List physical disks and mounted volumes |
| `edit [file]` | Run the text editor |
| `less [file]` / `more` | Page through a file (`type file \| less`) |
| `fm` | Run the file manager |
| `echo …` / `*.com` | Run a FOSCOM program |
| `0:` / `1:` | Switch current drive |
| `cmd1 \| cmd2` | Pipe stdout to stdin |
| `cmd > file` | Redirect stdout to a file |
| `reboot` | Reboot the machine |

Prompt shows the current path, e.g. `0:\>` or `0:\docs>`.

## Configuration (`SYSTEM.INI`)

At build time, `system.ini` is copied to `0:\SYSTEM.INI` on the boot volume. Edit that file on the FAT disk to change settings; reboot to apply.

Syntax: INI sections, `key=value` pairs, `;` comments. Section and key names are case-insensitive. Use plain ASCII values only.

### `[keyboard]`

| Key | Values | Default | Description |
|-----|--------|---------|-------------|
| `layout` | `de`, `us` | `de` | `de` = German QWERTZ, `us` = US QWERTY |

### `[video]`

Video mode is selected by the **kernel** after the filesystem is mounted (not in the bootloader).

| `mode` value | Resolution | Terminal (8×16 font) |
|--------------|------------|----------------------|
| `text`, `vga`, `80x25` | 80×25 VGA text | 80×25 |
| `480p`, `640x480` | 640×480 | 80×30 |
| `svga`, `800x600` | 800×600 | 100×37 |
| `xga`, `1024x768` | 1024×768 | 128×48 |
| `720p`, `1280x720` | 1280×720 | 160×45 |
| `1080p`, `1920x1080` | 1920×1080 | 240×67 |
| `WxH` (custom) | 640×480 … 1920×1080 | depends on size |

Notes:

- If `[video]` or `mode` is omitted, FOS stays on **VGA text**.
- Names are case-insensitive (`720p` and `1280x720` are the same).
- Framebuffer modes need **`-device bochs-display`** (included in `make run`). The kernel maps PCI BAR0 (VRAM) and uses BAR2 MMIO for mode setup.
- Without bochs-display, FOS prints a warning and stays on VGA text.
- Early boot messages appear on VGA text (or serial); after the mode switch the screen is cleared and output continues on the framebuffer.

Example — US keyboard and 1080p:

```ini
[keyboard]
layout=us

[video]
mode=1080p
```

Example — classic VGA text only:

```ini
[video]
mode=text
```

## Disk layout

```
boot.img (drive 0)
  LBA 0       MBR + stage1
  LBA 1–8     stage2 (enters long mode)
  LBA 9+      kernel.bin
  LBA 2048+   FAT32: KERNEL.BIN, SHELL.COM, ECHO.COM, EDIT.COM, LESS.COM, FM.COM, README.TXT, SYSTEM.INI, …

data.img (drive 1)
  LBA 2048+   exFAT (or FAT32 if mkfs.exfat is unavailable)
```

## Source layout

```
fos/
├── boot_stage1.asm / boot_stage2.asm   # Bootloader
├── boot.inc / boot_serial.inc
├── kernel/
│   ├── entry.asm              # → kmain()
│   ├── kernel.c               # Boot sequence
│   ├── shell.c                # Command shell
│   ├── console.c / video.c    # VGA text + framebuffer console
│   ├── config.c               # SYSTEM.INI parser
│   ├── exec.c / fos_api.c     # .COM loader + program API
│   ├── vfs.c / block.c        # Drives and ATA PIO
│   ├── fat32.c / exfat.c      # Filesystems
│   └── include/
├── apps/
│   ├── echo/   echo.com
│   ├── edit/   edit.com
│   ├── less/   less.com
│   └── fm/     fm.com
├── scripts/                   # mkdisk.sh, foscom_pack.py, …
├── system.ini                 # Template → 0:\SYSTEM.INI
└── Makefile
```

Almost all logic is **C**. Only the bootloader and short `entry.asm` stubs are assembly.

## Boot sequence (summary)

1. Stage1/stage2 load the kernel and enter long mode.
2. Kernel brings up console (VGA text), CPU info, keyboard, and disks.
3. `0:\SYSTEM.INI` is read — keyboard layout and video mode are applied here.
4. Disk/volume report and boot logo are shown.
5. Shell starts on a black background.
