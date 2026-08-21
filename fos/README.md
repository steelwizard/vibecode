# FOS — Flash Operating System

A small x86-64 hobby OS: assembly bootloader, C kernel, DOS-style shell, FAT32/exFAT, and FOSCOM `.COM` programs.

## Quick start

```bash
make          # boot.img (FAT32 ESP, drive 0:) + data.img (exFAT, drive 1:)
make run      # QEMU BIOS (SeaBIOS) + serial console + bochs-display
make run-uefi # QEMU UEFI (OVMF) — same disk image
```

**Requirements:** nasm, gcc, ld, objcopy, sfdisk, mkfs.vfat, qemu-system-x86_64  
**Optional:** mkfs.exfat, mtools, python3 (packs `.COM` files onto the boot volume), ovmf (`make run-uefi`)

### QEMU command line

`make run` is equivalent to:

```bash
qemu-system-x86_64 \
  -drive format=raw,file=boot.img,index=0,media=disk \
  -drive format=raw,file=data.img,index=1,media=disk \
  -device bochs-display \
  -device vmmouse,i8042=i8042 \
  -audiodev pipewire,id=snd0 -device sb16,audiodev=snd0 \
  -m 512M \
  -serial stdio
```

Type in the terminal that launches QEMU — input goes to **COM1 serial** (`-serial stdio`), not PS/2. The kernel reads both serial and PS/2.

> **Real USB boot:** the BIOS bootloader can load the kernel from a USB stick (stage1 now uses LBA), but once the kernel starts it only talks to **legacy ATA PIO**. There is no USB mass-storage or AHCI driver, so `0:` will not be the stick after handoff. Use `make run` / IDE disks for now.

`make run-uefi` is the same plus `-bios /usr/share/ovmf/OVMF.fd` (override with `OVMF=/path/to/OVMF.fd`) and `-net none` so OVMF skips PXE. The disk is dual-boot: SeaBIOS uses the MBR, OVMF uses `\EFI\BOOT\BOOTX64.EFI`.

Audio backend defaults to **PipeWire** when that session socket exists, otherwise PulseAudio. Override with `QEMU_AUDIO=pa`, `pipewire`, `alsa`, `sdl`, `none`, or `wav` (writes `fos-sb16.wav`).

> **Do not use `-vga none` with `-device bochs-display`.** Under QEMU this combination reboots in a loop before the kernel starts. Keep the default VGA and add `-device bochs-display` for framebuffer modes.

## What you get

- **Drive letters** — `0:\`, `1:\`, … (boot disk is always `0:`)
- **Backslash paths** — `0:\folder\file.txt`
- **FAT32** on drive 0 — read/write, directories, `mkdir`, `del`, `copy`, `move`
- **exFAT** on drive 1 — read-only listing and file read
- **FOSCOM** — `.COM` programs call a fixed kernel API at `0xFF0000`
- **Heap** — `mem_alloc` / `mem_free` / `mem_realloc`; leftovers are reclaimed when the program exits
- **Console** — scrollback, Page Up/Down, pipes (`|`), redirect (`>`)
- **Video** — VGA text or framebuffer modes set in `SYSTEM.INI` (kernel-side, after mount)
- **Keyboard layouts** — German QWERTZ or US QWERTY via config
- **Sound** — Sound Blaster 16 (ISA DSP + DMA); `beep` from the shell

### Included programs

| Program | Description |
|---------|-------------|
| `SHELL.COM` | Interactive shell (also built into the kernel) |
| `\FOS\ECHO.COM` | Sample FOSCOM program |
| `\FOS\EDIT.COM` | Text editor — Ctrl+S save, Ctrl+X exit, arrows, Delete |
| `\FOS\LESS.COM` | File pager — Space/b page, q quit (`more` alias in shell) |
| `\FOS\FM.COM` | File manager — boxed TUI, Enter runs `.COM` / `.BAT` (line output on a grey OK screen), `0-3` drive, `c`/`r` copy/move, `d` delete |
| `\FOS\DATE.COM` | Show or set the CMOS RTC (`date YYYY-MM-DD HH:MM:SS`) |
| `\FOS\MEM.COM` | RAM map, heap stats (`mem test` stress-tests the allocator) |
| `\FOS\BEEP.COM` | Tone (`beep`, `beep 880 300`) |
| `\FOS\PLAY.COM` | WAV/MP3/MIDI player TUI (`play DEMO.MID`, q stops) |
| `\FOS\PAINT.COM` | Mouse paint (`paint [file.pnt]`; B fill, S save, O open, Q quit) |
| `\FOS\GREP.COM` | Fixed-string search (`grep [-inv] PATTERN [FILE …]`) |
| `\FOS\BENCH.COM` | Test bench TUI — primes, 60 s soak, RAM, graphics, audio, hardware monitor (`bench`, `test`) |
| `\GAMES\TETRIS.COM` | Tetris — 7-bag, ghost, hold, levels (`tetris`; q quits) |
| `README.TXT` | Full project README (copy of `README.md` from the repo) |

Programs under `\FOS` and `\GAMES` are found via `$PATH` (seeded from `[shell] path=` in `SYSTEM.INI`).

A `.COM` can be 32 MiB of code+data+BSS at `0x300000`, with an 8 MiB stack. Larger working sets go on the heap (`api->mem_alloc`), which is the rest of identity-mapped RAM (512 MiB map, `make run` gives the VM 512 MiB). The loader streams the file straight to `load_addr`, so the old 128 KiB kernel bounce buffer is gone.

## Shell commands

| Command | Description |
|---------|-------------|
| `help` / `?` | Command list |
| `ver` | Version string |
| `cls` / `clear` | Clear screen |
| `dir [path]` | List directory |
| `cd <path>` | Change directory on current drive (`cd` alone uses `$HOME` or `\`) |
| `pwd` | Print the current directory (`0:\` or `0:\MIDI`) |
| `which` / `where <name>` | Locate a command (builtin, `$PATH` `.COM`, then `.BAT`) |
| `mkdir` / `md <path>` | Create directory (FAT32 only) |
| `del` / `erase <path>` | Delete file or empty folder (FAT32, confirms Y/N) |
| `copy <src> <dst>` | Copy file (FAT32) — progress window |
| `move` / `ren <src> <dst>` | Move or rename file (FAT32) — same window |
| `type` / `cat <path>` | Print file (or piped input) |
| `drives` / `df` | List physical disks and mounted volumes |
| `edit [file]` | Run the text editor |
| `less [file]` / `more` | Page through a file (`cat file \| less`) |
| `fm` | Run the file manager |
| `date [stamp]` | RTC via `date.com` |
| `mem` | RAM map via `mem.com` |
| `beep [hz [ms]]` | SB16 tone via `beep.com` |
| `play <file>` | WAV/MP3/MIDI player (`play DEMO.MID`; q quits) |
| `paint [file]` | Cell paint (`paint SKETCH.PNT`; left draw, right erase, `b` fill) |
| `grep [-inv] PAT [file]` | Find lines (`grep Flash README.TXT`; `-i` case, `-n` numbers, `-v` invert) |
| `bench` / `test` | Test bench TUI (`bench primes` / `bench mem` headless; `bench burn` = 60 s; `bench hw` = live meters) |
| `tetris` | Tetromino game (arrows / WASD, `z`/`y` rotate, space drop, `c` hold, `p` pause, `q` quit) |
| `echo …` / `*.com` | Run a FOSCOM program |
| `demo` / `*.bat` | Run a `.BAT` script (`call name` also works) |
| `NAME=value` | Set `$NAME` (`i++`, `i=i+1`, `i+=n`; `export NAME=value` is the same) |
| `env` / `set` | List environment variables |
| `unset NAME` | Remove a variable |
| `echo $PATH` | Expand `$NAME` or `${NAME}` (`'` quotes disable it) |
| `echo $(1+5)` | Integer math (`+ - * / %`, parentheses, variable names); `$((2*3))` works too |
| `0:` / `1:` | Switch current drive |
| `cmd1 \| cmd2` | Pipe stdout to stdin |
| `cmd > file` | Redirect stdout to a file |
| `reboot` | Reboot the machine |
| `if` / `for` / `while` | Conditionals and loops (`end` closes a block) |
| `true` / `false` | Set `$ERRORLEVEL` to 0 or 1 |
| `break` / `continue` | Leave or restart the innermost `for`/`while` |

Prompt shows the current path, e.g. `0:\>` or `0:\docs>`. It is green after a successful command and red after a failure (`false`, unknown command, `ERRORLEVEL` ≠ 0).

### Mouse

A yellow arrow follows the host mouse in QEMU (no grab — `make run` adds `-device vmmouse`). Left-drag selects text; right-click copies the selection, or pastes if nothing is selected. Left-click activates `[ OK ]` / Y/N buttons, moves the caret in the shell and editor, opens FM entries on double-click, and pages `less` (upper/lower half). Click the bottom row in `play` to quit. In `paint`, left-drag draws, right-drag erases, `B` or **Fill** flood-fills, and the bottom swatches pick a colour. In `bench`, click a menu row to select and click again to run; in the hardware monitor, click the load bar to set synthetic CPU load. In `tetris`, click the well to rotate, the sides to nudge, or the bottom bar.

### Scripts (`if` / `for` / `while` / `.BAT`)

Keywords are case-insensitive. `then` and `do` are optional. Close a multi-line block with `end` (also `endif`, `done`, or `wend`). The prompt becomes `> ` while a block is open. Ctrl+C cancels it.

`.BAT` files are the same language, one command per line. `$PATH` is searched the same way as for `.COM` (a `.COM` wins if both exist). `demo`, `demo.bat`, and `call demo.bat [args]` all run `FOS\DEMO.BAT`. In the file manager, Enter on a `.BAT` runs it the same way. Leading `@` is ignored. `@echo off` / `echo on` do nothing. Arguments are `%0`…`%9` and `%*`; `%%` is a literal `%`; `%PATH%` is the same as `$PATH`. Nesting is allowed (`call` another `.BAT`, up to 4 deep). Cap is 64 lines per script (or typed block) and 10000 loop iterations.

```
if exist README.TXT then echo yes else echo no
if exist README.TXT
  echo yes
else
  echo no
end

for i in a b c do echo $i
for i = 1 to 3
  echo $i
end

while false do echo never

i=0
i=i+1
i++
echo $i
if i < 10 then echo small
```

Conditions: `exist PATH`, `not exist PATH`, `errorlevel N` (`$ERRORLEVEL` ≥ N), `true` / `false`, string `A == B` (or `=` / `!=` / `<>`), and integer `i < y` (`< > <= >=`; names are variables, unset is 0). `$ERRORLEVEL` is 0 after a successful command and 1 after a failure. `;` and `&` separate commands on one line. Nested `if`/`for`/`while` work.

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
| `1200p`, `1920x1200` | 1920×1200 | 240×75 |
| `1440p`, `2560x1440` | 2560×1440 | 320×90 |
| `WxH` (custom) | 640×480 … 2560×1440 | depends on size |

The default in `SYSTEM.INI` is `1080p`.

Notes:

- If `[video]` or `mode` is omitted, FOS stays on **VGA text**.
- Names are case-insensitive (`720p` and `1280x720` are the same).
- Framebuffer modes need **`-device bochs-display`** (included in `make run`). The kernel maps PCI BAR0 (VRAM) and uses BAR2 MMIO for mode setup.
- 32bpp needs `width × height × 4` bytes of VRAM. Every mode above fits bochs-display's 16 MiB default (1440p needs ~14 MiB); `make run` passes `vgamem=32M` for headroom. Override with `make run VGAMEM=64M`.
- If a mode does not fit VRAM or the display rejects it, FOS prints a warning and stays on VGA text.
- Without bochs-display, FOS prints a warning and stays on VGA text.
- Early boot messages appear on VGA text (or serial); after the mode switch the screen is cleared and output continues on the framebuffer.
- Full-screen apps (`edit`, `fm`, `less`, `play`) read the console size at startup via `get_term_size` and lay out to fill whatever mode is active.
- Errors (failed commands, missing files, and so on) open a modal dialog: red screen, blue box with a bomb glyph and a selected `[ OK ]` button. Enter or Space dismisses it and restores the previous screen. Pipes and redirects still get the message as a plain line.
- The framebuffer console draws text with the CP437 8×16 VGA font in `kernel/font8x16.c`, generated by `scripts/genfont.py` from a VGA BIOS ROM so it matches text mode exactly. `make check-font` validates it.
- `python3 scripts/shot.py out.ppm --wait 16 [--keys l,e,s,s,ret]` boots the image headless and screendumps it, which is the quickest way to check a mode without a display.

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

### `[shell]`

| Key | Values | Default | Description |
|-----|--------|---------|-------------|
| `path` | colon-separated dirs | `\FOS:\GAMES` | Initial `$PATH` for `.COM` lookup |

`$PATH` is a real environment variable (`echo $PATH`, `PATH=\FOS:\BIN`). The `[shell] path=` key only seeds it at boot. Entries are absolute directories on the **current drive** (no drive letters). The shell checks the current directory first, then each PATH entry. Other variables: `$PWD`, `$HOME` (`\`), `$DRIVE`, `$ERRORLEVEL`.

### `[sound]`

Optional. If omitted, FOS probes the usual ISA bases (`220h`, `240h`, `260h`, `280h`) with IRQ 5 and 8-bit DMA 1 (QEMU’s `sb16` defaults).

| Key | Values | Default | Description |
|-----|--------|---------|-------------|
| `port` | hex (`220`, `0x220`) | probe | DSP base |
| `irq` | `5`, `7`, `10` | `5` | PIC line |
| `dma` | `1` | `1` | 8-bit ISA DMA channel |

Needs **`-device sb16`** (included in `make run`). Without a card, boot prints `no Sound Blaster` and `beep` says the same.

Playback is 8-bit unsigned mono through DMA. `play FILE` streams PCM WAV (8- or 16-bit, mono or stereo), MPEG-1/2 Layer III (MP3), or Standard MIDI (`MThd`) through TinySoundFont. Stereo is mixed down to mono. Press `q` to stop. `beep FILE.WAV` still plays a short clip loaded in one go (32 KiB).

A short `DEMO.WAV` is packed onto the boot volume; `DEMO.MP3` is added when `ffmpeg` is available at build time. MIDI uses `\FOS\GM.SF2`, a tiny CC0 wavetable GM bank from `scripts/mksf2.py`. For sampled instruments, pack a real SoundFont with `FOS_SF2=/usr/share/sounds/sf2/TimGM6mb.sf2 make` or copy one to `data/GM.SF2`.

`DEMO.MID` is a public-domain Ode to Joy. `\MIDI` has Bach pieces (inventions, WTC prelude/fugue, Toccata and Fugue, *Bist du bei mir*) from the Mutopia Project and Wikimedia Commons — all public domain. Play with `play MIDI\PREL1.MID`. AC/DC and other still-copyrighted songs are not included; copy your own `.MID` files onto a data volume if you have a license to use them.

## Disk layout

```
boot.img (drive 0, 64 MiB)
  LBA 0       MBR + stage1 (BIOS)
  LBA 1–8     stage2 (enters long mode)
  LBA 9+      kernel.bin (BIOS load; UEFI reads KERNEL.BIN from FAT)
  LBA 2048+   FAT32 ESP (type 0xEF, ≥65525 clusters so OVMF accepts it):
              KERNEL.BIN, \EFI\BOOT\BOOTX64.EFI, SHELL.COM,
              \FOS\*.COM (ECHO, EDIT, LESS, FM, DATE, MEM, BEEP, PLAY, PAINT, GREP, BENCH),
              \FOS\GM.SF2, DEMO.WAV, DEMO.MID, DEMO.MP3 (if ffmpeg at build),
              \MIDI\*.MID (Bach),
              README.TXT, SYSTEM.INI, …

data.img (drive 1)
  LBA 2048+   exFAT (or FAT32 if mkfs.exfat is unavailable)
              Sample texts: LOREM.TXT, IPSUM.TXT, CICERO.TXT
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
├── efi/
│   ├── efi_main.c             # UEFI loader → same kernel at 1 MiB
│   └── BOOTX64.EFI            # built PE32+ EFI app
├── apps/
│   ├── echo/   echo.com
│   ├── edit/   edit.com
│   ├── less/   less.com
│   ├── date/   date.com
│   ├── mem/    mem.com
│   ├── beep/   beep.com
│   ├── play/   play.com (WAV/MP3/MIDI TUI, minimp3 + TinySoundFont)
│   ├── grep/   grep.com
│   ├── bench/  bench.com (TUI: primes, soak, RAM, graphics, audio, hardware monitor)
│   └── fm/     fm.com
├── games/
│   └── tetris/ tetris.com (7-bag, ghost, hold, levels)
├── scripts/                   # mkdisk.sh, foscom_pack.py, …
├── system.ini                 # Template → 0:\SYSTEM.INI
└── Makefile
```

Almost all logic is **C**. Only the bootloader, UEFI trampoline, and short `entry.asm` stubs are assembly.

## Boot sequence (summary)

**BIOS (`make run`):** Stage1/stage2 load the kernel from LBA 9, collect E820, enter long mode, jump to `0x100000`.

**UEFI (`make run-uefi`):** OVMF runs `\EFI\BOOT\BOOTX64.EFI`, which reads `\KERNEL.BIN` from the ESP, `ExitBootServices`, installs the same GDT/page tables as stage2, and jumps to `0x100000`.

Then both paths:

1. Kernel brings up console (VGA text), CPU info, keyboard, and disks.
2. `0:\SYSTEM.INI` is read — keyboard layout and video mode are applied here.
3. Disk/volume report and boot logo are shown.
4. Shell starts on a black background.
