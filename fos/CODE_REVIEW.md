# FOS code review — bugs & half-baked work

Prioritized by impact. Each item: what is wrong, then a ~3-sentence fix.

## P0 — break real boot / core usability

1. Boot disk mapping ignores BIOS drive (block.c) — PARTIAL
  Prefer ATA primary master as `0:`; if missing, use the first found disk so
   drive 0: always exists. BIOS DL is recorded but not mapped — no INT 13h after
   long mode. README now documents that USB sticks are not usable as `0:` once
   the kernel starts. Full fix needs INT 13h thunk or USB/AHCI.

2. Stage1 still uses CHS disk reads (boot_stage1.asm) — FIXED
  Stage1 tries EDD LBA (AH=42h) first, CHS fallback if EDD is absent.

3. Identity map is only 64 MiB while QEMU uses 128M (boot_stage2.asm, Makefile) — FIXED
  Page tables now identity-map 256 × 2 MiB = 512 MiB (BIOS, UEFI, and a kernel
   CR3 reload so an old bootloader still works). Framebuffer virt stays at
   0x20000000, just past that window. `make run` gives the VM 512 MiB to match.

4. FM launches EDIT.COM from the wrong path (apps/fm/fm.c) — FIXED
  Now calls `\\FOS\\EDIT.COM`.

5. Uninitialized prompt buffers in edit and fm (edit.c, fm.c) — FIXED
  Both prompts zero `buf[0]` before drawing.


## P1 — filesystem correctness

1. exFAT long names overwrite instead of concatenating (exfat.c) — FIXED
  File Name (0xC1) entries append into the name buffer.

2. exFAT ignores NoFatChain / contiguous files (exfat.c) — FIXED
  Stream Extension flag bit 1 is honored; contiguous reads step cluster+1
   instead of walking the FAT.

3. vfs_resolve claims to collapse .\ but does not (vfs.c) — FIXED
  `path_collapse()` implements `.` / `..` for absolute DOS paths.

4. Kernel size not enforced vs KERNEL_MAX_SECTORS (Makefile, boot.inc) — FIXED
  `make` fails if `kernel.bin` exceeds `KERNEL_MAX_SECTORS * 512`. BSS past
   that is zeroed in `entry.asm` (objcopy does not emit trailing BSS).

5. data.img / mkdata.sh often leaves drive 1: empty (scripts/mkdata.sh) — IMPROVED
  Uses offset-aware `mcopy @@${PART_START}s` / fat32_add for FAT32. exFAT
   injection still best-effort (volume may stay empty; FOS exFAT is read-only).


## P2 — apps & shell polish

1. less detect_md always returns true (apps/less/less.c) — FIXED
  Only `.md` / `.markdown` / `README.TXT` enable Markdown mode.

2. Ctrl+C only clears the line, does not interrupt work (shell.c) — FIXED
  `keyboard_check_ctrl_c()` is polled in dir listing, `type`, confirm prompts,
   and the pipeline loop. Full preemption still needs IRQs.

3. Relative paths in edit/less ignore cwd (apps) — FIXED
  Relative names are resolved against `api->get_cwd()`.

4. Pipe/redirect capture is 4 KiB; no quoting (shell.c) — FIXED
  Shell capture is 32 KiB (static buffers). `"` protects `|` and `>` and is
   stripped afterwards. `fos_api_t.pipe_in` stays 4096 — growing it shifts
   every later function pointer and breaks existing `.COM` binaries.

5. FAT32: no LFN, dirs don’t grow, FSInfo free count stale (fat32.c) — PARTIAL
  FSInfo is updated on alloc/free; a full directory grows by one cluster.
   `df` rereads the free count. LFN can wait if mkdisk sticks to 8.3.


## P3 — architecture debt (half-baked by design)

1. Physical allocator / heap (memory.c) — DONE
  Bitmap over usable E820 in [COM_STACK_TOP, 512 MiB), byte-granular heap on
    top, API mem_alloc/mem_free/mem_realloc, per-program owner reclaim on exit.
    .COM images occupy 0x300000–0x2300000 (32 MiB) and stacks sit in four 8 MiB
    slots below 0x4300000; the pool starts after that so no extra carve-outs.

2. ATA PIO only — no AHCI/USB storage (block.c) — DOCUMENTED
  Explains why USB flash boot dies after BIOS. LBA28 also caps ~128 GiB.
    README warns; AHCI/USB remains a larger project.

3. Nested exec shares the API block and the load address (exec.c, fos_api) — FIXED
  Cmdline/pipe are saved around nested `run_com`. The outer image is copied to
   the heap before the inner load and restored afterwards, so FM→EDIT no longer
   shreds FM's code. Images that overlap the API block at 0xFF0000 are refused
   (that address sits inside the COM window). Inner PIC handlers are cleared
   only when returning to the shell.

4. Exceptions halt with no message (irq.c) — FIXED
  Per-vector stubs pass vector, error code, RIP, and CR2 to `exception_panic`.

5. Apps hardcode 80×25 while video can be 1080p (edit/less/fm) — FIXED
  Framebuffer modes existed but UIs assumed classic text geometry. The API now
    exposes get_term_size; edit/less/fm/play read it at startup and lay out from
    it. Buffers are sized to MAX_COLS (320) with runtime bounds.

6. Framebuffer mode never actually engaged (video.c) — FIXED
  Over PCI MMIO the DISPI registers are a flat array of 16-bit words at
    BAR2+0x500, but dispi_read/write drove them as the legacy 0x1CE/0x1CF
    index/data pair. The ID probe therefore read XRES instead of 0xB0C5, so every
    mode fell back to VGA text with "framebuffer mode unavailable". Fixed by
    indexing the MMIO aperture directly, mapping it uncached, and falling back to
    the legacy ports only if the MMIO probe fails.

7. font8x16 was misaligned and truncated (font8x16.c) — FIXED
  Glyph shapes were offset from their character codes (index 0x41 held an 'N')
    and all data stopped at 0x72, so 's'–'z' and the whole CP437 upper half
    (box drawing used by fm/play) drew as blanks — spaces even rendered as '-'.
    Only reachable in framebuffer mode, which is why it went unnoticed. The table
    is now generated by scripts/genfont.py from the VGA BIOS ROM, so it matches
    text mode exactly; `make check-font` validates it.

8. App objects ignored fos_api.h (Makefile) — FIXED
  Every kernel/app `.c` rule now depends on `kernel/include/*.h`. A header-only
   ABI change used to leave stale `.COM`s that saw NULL `get_mem_info` /
   `get_term_size` / `read_key`. Frozen offsets are `_Static_assert`ed in
   `fos_api.c`; new pointers go at the end of `fos_api_t`.


## Suggested next work

Still open: FAT32 LFN, ATA/AHCI/USB storage, mapping BIOS DL to `0:`.
The API block at 0xFF0000 remains a hole in the COM load window — moving it
below `COM_LOAD_MIN` would need a coordinated kernel + app rebuild.
