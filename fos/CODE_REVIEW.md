FOS code review — bugs & half-baked work
========================================
Prioritized by impact. Each item: what is wrong, then a ~3-sentence fix.

P0 — break real boot / core usability
-------------------------------------

1. Boot disk mapping ignores BIOS drive (block.c)
   Comment says DL from stage1 is logical 0:, but boot_bios is read and never used;
   drive 0: is hardcoded as ATA bus0/drive0. USB or non-primary-master boot leaves
   0: missing or pointing at the wrong disk.
   Fix: Keep using BIOS INT 13h for the boot volume after handoff (or map boot_bios
   to the right device), and only fall back to ATA 0:0 in QEMU. Until USB/AHCI
   exists, document that real USB boot will hang after the kernel starts.

2. Stage1 still uses CHS disk reads (boot_stage1.asm)
   Stage2 correctly uses EDD LBA (AH=42h); stage1 still does CHS AH=02 with cyl=0
   head=0. Many USB sticks and large disks fail before stage2 even runs.
   Fix: Copy the stage2 DAP/LBA loader into stage1 for the stage2 sectors. Keep a
   CHS fallback only if EDD is absent. Test with qemu -drive if=ide and raw USB
   images.

3. Identity map is only 64 MiB while QEMU uses 128M (boot_stage2.asm, Makefile)
   Page tables map 32 × 2 MiB = 64 MiB, but make run sets -m 128M. Touching RAM or
   devices above 64 MiB (and mem.com advertising those regions) can #PF into a silent hang.
   Fix: Raise the identity-map loop to cover at least QEMU’s RAM size (e.g. 64 entries
   for 128 MiB), or map E820 usable regions on demand. Align mem.com with what is
   actually mapped.

4. FM launches EDIT.COM from the wrong path (apps/fm/fm.c)
   mkdisk installs apps under \FOS\, but FM calls run_com("\\EDIT.COM", …). Edit from
   the file manager always fails.
   Fix: Call "\\FOS\\EDIT.COM" (or resolve via PATH / shell lookup). Same for any
   other hardcoded COM paths. Add a quick smoke test: fm → e on a file.

5. Uninitialized prompt buffers in edit and fm (edit.c, fm.c)
   prompt_filename / prompt_name declare char buf[] and api->write(buf) before any
   bytes are written — undefined behavior / garbage on screen / possible overrun.
   Fix: buf[0] = 0 (or memset) before the draw loop. Also bound writes with len so
   write_n is used instead of write on a possibly non-terminated buffer.


P1 — filesystem correctness
---------------------------

6. exFAT long names overwrite instead of concatenating (exfat.c)
   Each 0xC1 File Name entry replaces name instead of appending. Names longer than
   one entry (15 UTF-16 chars) are wrong; list/lookup break for real exFAT volumes.
   Fix: Append decoded UTF-16 chunks into name with remaining capacity. Walk
   secondary count correctly across the entry set.

7. exFAT ignores NoFatChain / contiguous files (exfat.c)
   Contiguous files never walk the FAT in the real world; FOS always walks the FAT.
   Those files read as garbage or fail.
   Fix: If the Stream Extension flag says contiguous, read linear clusters from
   first_cluster for size bytes and skip FAT. Keep the FAT walk for fragmented files.

8. vfs_resolve claims to collapse \..\ but does not (vfs.c)
   Comment says collapse \.\ and \..\; code only strips a trailing slash. cd .. and
   relative .. paths fail or mis-resolve.
   Fix: Implement real path normalization (stack of components, pop on .., skip .).
   Cover with a few shell tests: cd sub; cd ..; type ..\file.

9. Kernel size not enforced vs KERNEL_MAX_SECTORS (Makefile, boot.inc)
   Bootloader always loads 192 sectors (96 KiB). kernel.bin is already ~70+ KiB with
   little headroom; growth past the limit silently truncates and bricks boot.
   Fix: Add a Makefile check: fail the build if kernel.bin > 192*512. Optionally bump
   KERNEL_MAX_SECTORS and the chunked loader together when needed.

10. data.img / mkdata.sh often leaves drive 1: empty (scripts/mkdata.sh)
    mcopy on a partitioned image without @@offset, and “exFAT injection not
    implemented,” means TEST.TXT and friends often never appear on 1:.
    Fix: Use the same offset-aware packing as mkdisk (python fat/exfat helper or
    mcopy @@start). Document that 1: is read-only exFAT until write support exists.


P2 — apps & shell polish
------------------------

11. less detect_md always returns true (apps/less/less.c)
    Extension checks are dead code; final return 1 styles every file as Markdown.
    Fix: return 0 when the extension is not .md/.markdown (and empty path / pipe
    policy as you prefer). Only enable md_mode for real Markdown.

12. Ctrl+C only clears the line, does not interrupt work (shell.c)
    Running builtins/COMs ignore Ctrl+C; only the readline path handles it.
    Fix: Poll for 0x03 in long loops (dir, type, pipelines) and abort cleanly.
    Full preemption needs IRQs; for now cooperative cancel is enough.

13. Relative paths in edit/less ignore cwd (apps)
    normalize_path turns "foo.txt" into \foo.txt from volume root, not the current
    directory. Saves/opens from a subfolder write to the wrong place.
    Fix: If the path is not absolute, prepend api->get_cwd() (or pass cwd into the
    COM). Match shell path rules.

14. Pipe/redirect capture is 4 KiB; no quoting (shell.c)
    Long command output truncates; > and | inside arguments cannot be escaped.
    Fix: Raise CAPTURE_MAX for hobby use, and document limits. Later: simple quotes
    so "a>b" is one token.

15. FAT32: no LFN, dirs don’t grow, FSInfo free count stale (fat32.c)
    Long names, full directories, and df free space are half-baked for a writeable 0:.
    Fix: Short-term — update FSInfo on alloc/free and grow directory clusters when
    full. LFN can wait if mkdisk sticks to 8.3.


P3 — architecture debt (half-baked by design)
---------------------------------------------

16. No physical allocator / heap (memory.c)
    memory_get_info only reports E820; nothing allocates pages or a kernel heap.
    Fix: Next real step after extending the identity map: a simple bitmap/buddy over
    usable E820 above the kernel. Then give COM apps a brk or fixed heap region.
    Note: 0x300000–0x800000 holds .COM images and their stacks, and 0x20000–0x22000
    is the SB16 DMA buffer; E820 calls all of it free, so carve them out first.

17. ATA PIO only — no AHCI/USB storage (block.c)
    Explains why USB flash boot dies after BIOS. LBA28 also caps ~128 GiB.
    Fix: For hobby scope, keep ATA+QEMU and optionally INT 13h thunk for the boot
    disk only. AHCI is a larger project; don’t pretend USB works yet.

18. Nested exec shares the API block and the load address (exec.c, fos_api)
    FM→EDIT reuses cmdline/pipe_in on the same API block, and both images load at
    COM_LOAD, so an inner program overwrites the outer one's code.
    Fix: Save/restore API cmdline around nested run_com; relocate or stage the
    inner image. (Fixed: program stacks moved to 0x800000 with a separate 1 MiB
    slot per nesting level — they used to sit at 0x8F000, inside the kernel's own
    live frames, so any program with deep frames shredded the kernel stack.)

19. Exceptions halt with no message (irq.c)
    #PF/#GP go to exc_halt_stub — silent freeze, hard to debug map bugs.
    Fix: Print vector + CR2 + RIP on the console before hlt. Worth a day for every
    later memory/video bug.

20. Apps hardcode 80×25 while video can be 1080p (edit/less/fm) — FIXED
    Framebuffer modes existed but UIs assumed classic text geometry. The API now
    exposes get_term_size; edit/less/fm/play read it at startup and lay out from
    it. Buffers are sized to MAX_COLS (320) with runtime bounds.

21. Framebuffer mode never actually engaged (video.c) — FIXED
    Over PCI MMIO the DISPI registers are a flat array of 16-bit words at
    BAR2+0x500, but dispi_read/write drove them as the legacy 0x1CE/0x1CF
    index/data pair. The ID probe therefore read XRES instead of 0xB0C5, so every
    mode fell back to VGA text with "framebuffer mode unavailable". Fixed by
    indexing the MMIO aperture directly, mapping it uncached, and falling back to
    the legacy ports only if the MMIO probe fails.

22. font8x16 was misaligned and truncated (font8x16.c) — FIXED
    Glyph shapes were offset from their character codes (index 0x41 held an 'N')
    and all data stopped at 0x72, so 's'–'z' and the whole CP437 upper half
    (box drawing used by fm/play) drew as blanks — spaces even rendered as '-'.
    Only reachable in framebuffer mode, which is why it went unnoticed. The table
    is now generated by scripts/genfont.py from the VGA BIOS ROM, so it matches
    text mode exactly; `make check-font` validates it.


Suggested order of work
-----------------------
1–5, then 6–9, then 11–13. Treat 16–18 as the next “make it a real OS” milestone,
not drive-by cleanup.
