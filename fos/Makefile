# FOS — Flash Operating System (asm bootloader + C kernel + FAT32/exFAT)

IMG         = boot.img
BOOT_BIN    = boot.bin
KERNEL_ELF  = kernel/kernel.elf
KERNEL_BIN  = kernel.bin
SHELL_BIN   = shell.bin
ECHO_BIN    = apps/echo/echo.bin
ECHO_COM    = apps/echo/echo.com
EDIT_BIN    = apps/edit/edit.bin
EDIT_COM    = apps/edit/edit.com
FM_BIN      = apps/fm/fm.bin
FM_COM      = apps/fm/fm.com
LESS_BIN    = apps/less/less.bin
LESS_COM    = apps/less/less.com
DATE_BIN    = apps/date/date.bin
DATE_COM    = apps/date/date.com
MEM_BIN     = apps/mem/mem.bin
MEM_COM     = apps/mem/mem.com
BEEP_BIN    = apps/beep/beep.bin
BEEP_COM    = apps/beep/beep.com
PLAY_BIN    = apps/play/play.bin
PLAY_COM    = apps/play/play.com
PAINT_BIN   = apps/paint/paint.bin
PAINT_COM   = apps/paint/paint.com
GREP_BIN    = apps/grep/grep.bin
GREP_COM    = apps/grep/grep.com
EFI_EFI     = efi/BOOTX64.EFI
DATA_IMG    = data.img

BOOT_SECTORS = 9
KERNEL_LBA   = 9
KERNEL_MAX_SECTORS = 256
PART_START   = 2048

COM_LOAD     = 0x300000
COM_STACK    = 0x4300000

CC      = gcc
LD      = ld
NASM    = nasm
OBJCOPY = objcopy
PYTHON  = python3

QEMU := $(shell command -v qemu-system-x86_64 2>/dev/null || command -v qemu-system-i386 2>/dev/null)

KERNEL_CFLAGS = -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
                -Wall -Wextra -O2 -I kernel/include

# fos_api.h is the .COM ABI. Without this, a header-only change leaves stale
# app objects that read NULL function pointers (mem / edit / play).
KERNEL_HDRS = $(wildcard kernel/include/*.h)

KERNEL_ASM = kernel/entry.asm kernel/irqvec.asm kernel/comcall.asm
KERNEL_C   = kernel/kernel.c kernel/shell.c kernel/vfs.c kernel/block.c \
             kernel/partition.c kernel/fat32.c kernel/exfat.c \
             kernel/console.c kernel/keyboard.c kernel/string.c \
             kernel/cpu.c kernel/boot_report.c kernel/exec.c kernel/fos_api.c \
             kernel/config.c kernel/video.c kernel/font8x16.c \
             kernel/timer.c kernel/rtc.c kernel/memory.c kernel/heap.c \
             kernel/irq.c kernel/sb16.c kernel/env.c kernel/mouse.c

SHELL_ASM  = shell/entry.asm
SHELL_C    = kernel/shell.c kernel/vfs.c kernel/block.c \
             kernel/partition.c kernel/fat32.c kernel/exfat.c \
             kernel/console.c kernel/keyboard.c kernel/string.c \
             kernel/boot_report.c
SHELL_OBJ  = $(SHELL_ASM:.asm=.o) $(SHELL_C:.c=.o)

ECHO_ASM   = apps/echo/entry.asm
ECHO_C     = apps/echo/echo.c
ECHO_OBJ   = $(ECHO_ASM:.asm=.o) $(ECHO_C:.c=.o)
ECHO_ELF   = apps/echo/echo.elf

EDIT_ASM   = apps/edit/entry.asm
EDIT_C     = apps/edit/edit.c
EDIT_OBJ   = $(EDIT_ASM:.asm=.o) $(EDIT_C:.c=.o)
EDIT_ELF   = apps/edit/edit.elf

FM_ASM     = apps/fm/entry.asm
FM_C       = apps/fm/fm.c
FM_OBJ     = $(FM_ASM:.asm=.o) $(FM_C:.c=.o)
FM_ELF     = apps/fm/fm.elf

LESS_ASM   = apps/less/entry.asm
LESS_C     = apps/less/less.c
LESS_OBJ   = $(LESS_ASM:.asm=.o) $(LESS_C:.c=.o)
LESS_ELF   = apps/less/less.elf

DATE_ASM   = apps/date/entry.asm
DATE_C     = apps/date/date.c
DATE_OBJ   = $(DATE_ASM:.asm=.o) $(DATE_C:.c=.o)
DATE_ELF   = apps/date/date.elf

MEM_ASM    = apps/mem/entry.asm
MEM_C      = apps/mem/mem.c
MEM_OBJ    = $(MEM_ASM:.asm=.o) $(MEM_C:.c=.o)
MEM_ELF    = apps/mem/mem.elf

BEEP_ASM   = apps/beep/entry.asm
BEEP_C     = apps/beep/beep.c
BEEP_OBJ   = $(BEEP_ASM:.asm=.o) $(BEEP_C:.c=.o)
BEEP_ELF   = apps/beep/beep.elf

PLAY_ASM   = apps/play/entry.asm
PLAY_C     = apps/play/play.c apps/play/minimp3_impl.c
PLAY_OBJ   = apps/play/entry.o apps/play/play.o apps/play/minimp3_impl.o
PLAY_ELF   = apps/play/play.elf

PAINT_ASM  = apps/paint/entry.asm
PAINT_C    = apps/paint/paint.c
PAINT_OBJ  = $(PAINT_ASM:.asm=.o) $(PAINT_C:.c=.o)
PAINT_ELF  = apps/paint/paint.elf

GREP_ASM   = apps/grep/entry.asm
GREP_C     = apps/grep/grep.c
GREP_OBJ   = $(GREP_ASM:.asm=.o) $(GREP_C:.c=.o)
GREP_ELF   = apps/grep/grep.elf

KERNEL_OBJ = $(KERNEL_ASM:.asm=.o) $(KERNEL_C:.c=.o)

# bochs-display VRAM. 32 MiB covers every mode up to 2560x1440 at 32bpp.
VGAMEM ?= 32M

# Where exec.c loads .COM images; the shell's static data must stay below it.
COM_LOAD_ADDR = 3145728

.PHONY: all run run-uefi clean check-font qemu-clean

all: $(IMG) $(DATA_IMG)

# The console font is only exercised in framebuffer mode, so a bad table can
# ship unnoticed. Validate glyph alignment and coverage.
check-font:
	@python3 scripts/genfont.py --check

$(BOOT_BIN): boot_stage1.asm boot_stage2.asm boot.inc
	$(NASM) -f bin boot_stage1.asm -o boot_stage1.bin
	$(NASM) -f bin boot_stage2.asm -o boot_stage2.bin
	cat boot_stage1.bin boot_stage2.bin > $@
	rm -f boot_stage1.bin boot_stage2.bin

kernel/%.o: kernel/%.asm
	$(NASM) -f elf64 $< -o $@

kernel/%.o: kernel/%.c $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

shell/%.o: shell/%.asm
	$(NASM) -f elf64 $< -o $@

apps/echo/%.o: apps/echo/%.asm
	$(NASM) -f elf64 $< -o $@

apps/echo/%.o: apps/echo/%.c $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/edit/%.o: apps/edit/%.asm
	$(NASM) -f elf64 $< -o $@

apps/edit/%.o: apps/edit/%.c $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/fm/%.o: apps/fm/%.asm
	$(NASM) -f elf64 $< -o $@

apps/fm/%.o: apps/fm/%.c $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/less/%.o: apps/less/%.asm
	$(NASM) -f elf64 $< -o $@

apps/less/%.o: apps/less/%.c $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/date/%.o: apps/date/%.asm
	$(NASM) -f elf64 $< -o $@

apps/date/%.o: apps/date/%.c $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/mem/%.o: apps/mem/%.asm
	$(NASM) -f elf64 $< -o $@

apps/mem/%.o: apps/mem/%.c $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/beep/%.o: apps/beep/%.asm
	$(NASM) -f elf64 $< -o $@

apps/beep/%.o: apps/beep/%.c $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/play/%.o: apps/play/%.asm
	$(NASM) -f elf64 $< -o $@

apps/play/play.o: apps/play/play.c apps/play/mp3dec_fos.h $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -I apps/play -c $< -o $@

apps/play/minimp3_impl.o: apps/play/minimp3_impl.c apps/play/minimp3.h apps/play/mp3dec_fos.h
	$(CC) -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
	      -mno-red-zone -mstackrealign -Os -Wall -I apps/play -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
	      -Wno-unused-function -Wno-unused-variable -c $< -o $@

apps/paint/%.o: apps/paint/%.asm
	$(NASM) -f elf64 $< -o $@

apps/paint/%.o: apps/paint/%.c $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/grep/%.o: apps/grep/%.asm
	$(NASM) -f elf64 $< -o $@

apps/grep/%.o: apps/grep/%.c $(KERNEL_HDRS)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJ) kernel/linker.ld
	$(LD) -m elf_x86_64 -T kernel/linker.ld -nostdlib -o $@ $(KERNEL_OBJ)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@
	@sz=$$(wc -c < $@); max=$$(( $(KERNEL_MAX_SECTORS) * 512 )); \
	 if [ "$$sz" -gt "$$max" ]; then \
	   echo "ERROR: $@ is $$sz bytes — loaders only read $$max (KERNEL_MAX_SECTORS=$(KERNEL_MAX_SECTORS))."; \
	   echo "       Raise KERNEL_MAX_SECTORS in boot.inc, Makefile, and the UEFI -D together."; \
	   exit 1; \
	 fi; \
	 echo "kernel.bin $$sz / $$max bytes ($$(( max - sz )) free)"

SHELL_ELF = shell/shell.elf

$(SHELL_ELF): $(SHELL_OBJ) kernel/exec.o kernel/fos_api.o kernel/font8x16.o kernel/timer.o kernel/rtc.o kernel/memory.o kernel/heap.o kernel/sb16.o kernel/config.o kernel/env.o kernel/mouse.o kernel/irqvec.o kernel/comcall.o kernel/irq.o shell/linker.ld
	$(LD) -m elf_x86_64 -T shell/linker.ld -nostdlib -o $@ shell/entry.o \
		kernel/shell.o kernel/vfs.o kernel/block.o kernel/partition.o \
		kernel/fat32.o kernel/exfat.o kernel/console.o kernel/keyboard.o \
		kernel/string.o kernel/boot_report.o kernel/exec.o kernel/fos_api.o \
		kernel/font8x16.o kernel/timer.o kernel/rtc.o kernel/irqvec.o kernel/comcall.o kernel/irq.o kernel/memory.o kernel/heap.o kernel/sb16.o kernel/config.o kernel/env.o kernel/mouse.o

$(SHELL_BIN): $(SHELL_ELF)
	$(OBJCOPY) -O binary $< $@
	@end=$$(( 0x$$(nm $(SHELL_ELF) | awk '$$3=="_end"{print $$1; exit}') )); \
	 if [ "$$end" -ge $(COM_LOAD_ADDR) ]; then \
	   echo "ERROR: shell static data ends at $$end, at or past the .COM load address $(COM_LOAD_ADDR)."; \
	   echo "       Shrink a static buffer (e.g. CONSOLE_MAX_COLS or SCROLLBACK_MAX in kernel/console.c)."; \
	   exit 1; \
	 fi; \
	 echo "shell _end=$$end — $$(( $(COM_LOAD_ADDR) - $$end )) bytes free below .COM load"

$(ECHO_ELF): $(ECHO_OBJ) apps/echo/linker.ld
	$(LD) -m elf_x86_64 -T apps/echo/linker.ld -nostdlib -o $@ $(ECHO_OBJ)

$(ECHO_BIN): $(ECHO_ELF)
	$(OBJCOPY) -O binary $< $@

$(ECHO_COM): $(ECHO_BIN) $(ECHO_ELF) scripts/foscom_pack.py
	$(PYTHON) scripts/foscom_pack.py $(ECHO_BIN) $@ \
		--load $(COM_LOAD) \
		--entry 0x$$(nm -n $(ECHO_ELF) | awk '$$3=="_start"{print $$1; exit}') \
		--mem-size 0x$$(nm -n $(ECHO_ELF) | awk '$$3=="_end"{print $$1; exit}') \
		--stack $(COM_STACK) --name ECHO

$(EDIT_ELF): $(EDIT_OBJ) apps/edit/linker.ld
	$(LD) -m elf_x86_64 -T apps/edit/linker.ld -nostdlib -o $@ $(EDIT_OBJ)

$(EDIT_BIN): $(EDIT_ELF)
	$(OBJCOPY) -O binary $< $@

$(EDIT_COM): $(EDIT_BIN) $(EDIT_ELF) scripts/foscom_pack.py
	$(PYTHON) scripts/foscom_pack.py $(EDIT_BIN) $@ \
		--load $(COM_LOAD) \
		--entry 0x$$(nm -n $(EDIT_ELF) | awk '$$3=="_start"{print $$1; exit}') \
		--mem-size 0x$$(nm -n $(EDIT_ELF) | awk '$$3=="_end"{print $$1; exit}') \
		--stack $(COM_STACK) --name EDIT

$(FM_ELF): $(FM_OBJ) apps/fm/linker.ld
	$(LD) -m elf_x86_64 -T apps/fm/linker.ld -nostdlib -o $@ $(FM_OBJ)

$(FM_BIN): $(FM_ELF)
	$(OBJCOPY) -O binary $< $@

$(FM_COM): $(FM_BIN) $(FM_ELF) scripts/foscom_pack.py
	$(PYTHON) scripts/foscom_pack.py $(FM_BIN) $@ \
		--load $(COM_LOAD) \
		--entry 0x$$(nm -n $(FM_ELF) | awk '$$3=="_start"{print $$1; exit}') \
		--mem-size 0x$$(nm -n $(FM_ELF) | awk '$$3=="_end"{print $$1; exit}') \
		--stack $(COM_STACK) --name FM

$(LESS_ELF): $(LESS_OBJ) apps/less/linker.ld
	$(LD) -m elf_x86_64 -T apps/less/linker.ld -nostdlib -o $@ $(LESS_OBJ)

$(LESS_BIN): $(LESS_ELF)
	$(OBJCOPY) -O binary $< $@

$(LESS_COM): $(LESS_BIN) $(LESS_ELF) scripts/foscom_pack.py
	$(PYTHON) scripts/foscom_pack.py $(LESS_BIN) $@ \
		--load $(COM_LOAD) \
		--entry 0x$$(nm -n $(LESS_ELF) | awk '$$3=="_start"{print $$1; exit}') \
		--mem-size 0x$$(nm -n $(LESS_ELF) | awk '$$3=="_end"{print $$1; exit}') \
		--stack $(COM_STACK) --name LESS

$(DATE_ELF): $(DATE_OBJ) apps/date/linker.ld
	$(LD) -m elf_x86_64 -T apps/date/linker.ld -nostdlib -o $@ $(DATE_OBJ)

$(DATE_BIN): $(DATE_ELF)
	$(OBJCOPY) -O binary $< $@

$(DATE_COM): $(DATE_BIN) $(DATE_ELF) scripts/foscom_pack.py
	$(PYTHON) scripts/foscom_pack.py $(DATE_BIN) $@ \
		--load $(COM_LOAD) \
		--entry 0x$$(nm -n $(DATE_ELF) | awk '$$3=="_start"{print $$1; exit}') \
		--mem-size 0x$$(nm -n $(DATE_ELF) | awk '$$3=="_end"{print $$1; exit}') \
		--stack $(COM_STACK) --name DATE

$(MEM_ELF): $(MEM_OBJ) apps/mem/linker.ld
	$(LD) -m elf_x86_64 -T apps/mem/linker.ld -nostdlib -o $@ $(MEM_OBJ)

$(MEM_BIN): $(MEM_ELF)
	$(OBJCOPY) -O binary $< $@

$(MEM_COM): $(MEM_BIN) $(MEM_ELF) scripts/foscom_pack.py
	$(PYTHON) scripts/foscom_pack.py $(MEM_BIN) $@ \
		--load $(COM_LOAD) \
		--entry 0x$$(nm -n $(MEM_ELF) | awk '$$3=="_start"{print $$1; exit}') \
		--mem-size 0x$$(nm -n $(MEM_ELF) | awk '$$3=="_end"{print $$1; exit}') \
		--stack $(COM_STACK) --name MEM

$(BEEP_ELF): $(BEEP_OBJ) apps/beep/linker.ld
	$(LD) -m elf_x86_64 -T apps/beep/linker.ld -nostdlib -o $@ $(BEEP_OBJ)

$(BEEP_BIN): $(BEEP_ELF)
	$(OBJCOPY) -O binary $< $@

$(BEEP_COM): $(BEEP_BIN) $(BEEP_ELF) scripts/foscom_pack.py
	$(PYTHON) scripts/foscom_pack.py $(BEEP_BIN) $@ \
		--load $(COM_LOAD) \
		--entry 0x$$(nm -n $(BEEP_ELF) | awk '$$3=="_start"{print $$1; exit}') \
		--mem-size 0x$$(nm -n $(BEEP_ELF) | awk '$$3=="_end"{print $$1; exit}') \
		--stack $(COM_STACK) --name BEEP

$(PLAY_ELF): $(PLAY_OBJ) apps/play/linker.ld kernel/string.o
	$(LD) -m elf_x86_64 -T apps/play/linker.ld -nostdlib -o $@ $(PLAY_OBJ) kernel/string.o

$(PLAY_BIN): $(PLAY_ELF)
	$(OBJCOPY) -O binary $< $@

$(PLAY_COM): $(PLAY_BIN) $(PLAY_ELF) scripts/foscom_pack.py
	$(PYTHON) scripts/foscom_pack.py $(PLAY_BIN) $@ \
		--load $(COM_LOAD) \
		--entry 0x$$(nm -n $(PLAY_ELF) | awk '$$3=="_start"{print $$1; exit}') \
		--mem-size 0x$$(nm -n $(PLAY_ELF) | awk '$$3=="_end"{print $$1; exit}') \
		--stack $(COM_STACK) --name PLAY

$(PAINT_ELF): $(PAINT_OBJ) apps/paint/linker.ld
	$(LD) -m elf_x86_64 -T apps/paint/linker.ld -nostdlib -o $@ $(PAINT_OBJ)

$(PAINT_BIN): $(PAINT_ELF)
	$(OBJCOPY) -O binary $< $@

$(PAINT_COM): $(PAINT_BIN) $(PAINT_ELF) scripts/foscom_pack.py
	$(PYTHON) scripts/foscom_pack.py $(PAINT_BIN) $@ \
		--load $(COM_LOAD) \
		--entry 0x$$(nm -n $(PAINT_ELF) | awk '$$3=="_start"{print $$1; exit}') \
		--mem-size 0x$$(nm -n $(PAINT_ELF) | awk '$$3=="_end"{print $$1; exit}') \
		--stack $(COM_STACK) --name PAINT

$(GREP_ELF): $(GREP_OBJ) apps/grep/linker.ld
	$(LD) -m elf_x86_64 -T apps/grep/linker.ld -nostdlib -o $@ $(GREP_OBJ)

$(GREP_BIN): $(GREP_ELF)
	$(OBJCOPY) -O binary $< $@

$(GREP_COM): $(GREP_BIN) $(GREP_ELF) scripts/foscom_pack.py
	$(PYTHON) scripts/foscom_pack.py $(GREP_BIN) $@ \
		--load $(COM_LOAD) \
		--entry 0x$$(nm -n $(GREP_ELF) | awk '$$3=="_start"{print $$1; exit}') \
		--mem-size 0x$$(nm -n $(GREP_ELF) | awk '$$3=="_end"{print $$1; exit}') \
		--stack $(COM_STACK) --name GREP

EFI_CFLAGS = -m64 -mabi=ms -fshort-wchar -ffreestanding -fno-stack-protector \
             -fno-builtin -fpie -mno-red-zone -fno-ident \
             -fno-asynchronous-unwind-tables -Wall -Wextra -O2 -I efi

efi/enter_kernel.bin: efi/enter_kernel.asm
	$(NASM) -f bin $< -o $@

efi/enter_kernel_bin.h: efi/enter_kernel.bin
	$(PYTHON) -c '\
import sys; data=open(sys.argv[1],"rb").read();\
sys.stdout.write("static const unsigned char enter_kernel_bin[] = {\n");\
sys.stdout.write(", ".join("0x%02x" % b for b in data));\
sys.stdout.write("\n};\nstatic const unsigned int enter_kernel_bin_len = %d;\n" % len(data))\
	' $< > $@

efi/efi_main.o: efi/efi_main.c efi/efi.h efi/enter_kernel_bin.h
	$(CC) $(EFI_CFLAGS) -DKERNEL_MAX_SECTORS=$(KERNEL_MAX_SECTORS) -c $< -o $@

$(EFI_EFI): efi/efi_main.o efi/efi.ld
	$(LD) -m i386pep -nostdlib -T efi/efi.ld --subsystem 10 -e efi_main \
	      --image-base 0x140000000 --disable-auto-import \
	      -o $@ efi/efi_main.o

$(IMG): $(BOOT_BIN) $(KERNEL_BIN) $(SHELL_BIN) $(ECHO_COM) $(EDIT_COM) $(FM_COM) $(LESS_COM) $(DATE_COM) $(MEM_COM) $(BEEP_COM) $(PLAY_COM) $(PAINT_COM) $(GREP_COM) $(EFI_EFI) system.ini demo.bat scripts/mkdisk.sh scripts/fat32_add.py
	scripts/mkdisk.sh $(IMG) $(BOOT_BIN) $(KERNEL_BIN) $(KERNEL_LBA) $(PART_START) $(SHELL_BIN) $(ECHO_COM) $(EDIT_COM) $(FM_COM) $(LESS_COM) $(DATE_COM) $(MEM_COM) $(EFI_EFI) $(BEEP_COM) $(PLAY_COM) $(PAINT_COM) $(GREP_COM)

$(DATA_IMG): scripts/mkdata.sh scripts/exfat_add.py data/LOREM.TXT data/IPSUM.TXT data/CICERO.TXT
	scripts/mkdata.sh $(DATA_IMG)

# Host audio for SB16. pipewire / pa / alsa / sdl / none / wav
# Default: PipeWire when the session socket exists, otherwise PulseAudio.
ifndef QEMU_AUDIO
PW_SOCK := $(or $(XDG_RUNTIME_DIR),/run/user/$(shell id -u))/pipewire-0
ifeq ($(wildcard $(PW_SOCK)),)
QEMU_AUDIO := pa
else
QEMU_AUDIO := pipewire
endif
endif
ifeq ($(QEMU_AUDIO),wav)
QEMU_AUDIODEV = -audiodev wav,id=snd0,path=fos-sb16.wav
else
QEMU_AUDIODEV = -audiodev $(QEMU_AUDIO),id=snd0
endif
QEMU_SB16 = $(QEMU_AUDIODEV) -device sb16,audiodev=snd0,iobase=0x220,irq=5,dma=1
# vmmouse = QEMU/VMware absolute pointer (no grab). Still works as PS/2 fallback.
QEMU_MOUSE = -device vmmouse,i8042=i8042

run: $(IMG) $(DATA_IMG)
ifndef QEMU
	$(error QEMU not found – install qemu-system-x86_64)
endif
	$(QEMU) -drive format=raw,file=$(IMG),index=0,media=disk \
	        -drive format=raw,file=$(DATA_IMG),index=1,media=disk \
	        -device bochs-display,vgamem=$(VGAMEM) \
	        $(QEMU_MOUSE) \
	        $(QEMU_SB16) \
	        -m 512M -serial stdio

OVMF ?= $(firstword $(wildcard /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/qemu/OVMF.fd))

run-uefi: $(IMG) $(DATA_IMG)
ifndef QEMU
	$(error QEMU not found – install qemu-system-x86_64)
endif
ifeq ($(OVMF),)
	$(error OVMF not found – install ovmf, or set OVMF=/path/to/OVMF.fd)
endif
	$(QEMU) -bios $(OVMF) \
	        -drive format=raw,file=$(IMG),index=0,media=disk \
	        -drive format=raw,file=$(DATA_IMG),index=1,media=disk \
	        -device bochs-display,vgamem=$(VGAMEM) \
	        $(QEMU_MOUSE) \
	        $(QEMU_SB16) \
	        -m 512M -serial stdio -net none

# Kill leftover FOS QEMU guests (smoke/shot often fail to terminate) and /tmp junk.
qemu-clean:
	scripts/qemu_cleanup.sh

clean:
	rm -f $(BOOT_BIN) $(KERNEL_BIN) $(SHELL_BIN) $(ECHO_BIN) $(ECHO_COM) \
	      $(EDIT_BIN) $(EDIT_COM) $(EDIT_ELF) $(FM_BIN) $(FM_COM) $(FM_ELF) \
	      $(LESS_BIN) $(LESS_COM) $(LESS_ELF) \
	      $(DATE_BIN) $(DATE_COM) $(DATE_ELF) \
	      $(MEM_BIN) $(MEM_COM) $(MEM_ELF) \
	      $(BEEP_BIN) $(BEEP_COM) $(BEEP_ELF) \
	      $(PLAY_BIN) $(PLAY_COM) $(PLAY_ELF) \
	      $(PAINT_BIN) $(PAINT_COM) $(PAINT_ELF) \
	      $(GREP_BIN) $(GREP_COM) $(GREP_ELF) \
	      $(EFI_EFI) efi/efi_main.o efi/enter_kernel.bin efi/enter_kernel_bin.h \
	      $(KERNEL_ELF) shell/shell.elf $(ECHO_ELF) \
	      $(IMG) $(DATA_IMG) fos-sb16.wav $(KERNEL_OBJ) $(SHELL_OBJ) $(ECHO_OBJ) $(EDIT_OBJ) $(FM_OBJ) $(LESS_OBJ) $(DATE_OBJ) $(MEM_OBJ) $(BEEP_OBJ) $(PLAY_OBJ) $(PAINT_OBJ) $(GREP_OBJ)
