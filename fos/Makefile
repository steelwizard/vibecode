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
DATA_IMG    = data.img

BOOT_SECTORS = 9
KERNEL_LBA   = 9
PART_START   = 2048

COM_LOAD     = 0x300000
COM_STACK    = 0x8F000

CC      = gcc
LD      = ld
NASM    = nasm
OBJCOPY = objcopy
PYTHON  = python3

QEMU := $(shell command -v qemu-system-x86_64 2>/dev/null || command -v qemu-system-i386 2>/dev/null)

KERNEL_CFLAGS = -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
                -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
                -Wall -Wextra -O2 -I kernel/include

KERNEL_ASM = kernel/entry.asm
KERNEL_C   = kernel/kernel.c kernel/shell.c kernel/vfs.c kernel/block.c \
             kernel/partition.c kernel/fat32.c kernel/exfat.c \
             kernel/console.c kernel/keyboard.c kernel/string.c \
             kernel/cpu.c kernel/boot_report.c kernel/exec.c kernel/fos_api.c \
             kernel/config.c kernel/video.c kernel/font8x16.c

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

KERNEL_OBJ = $(KERNEL_ASM:.asm=.o) $(KERNEL_C:.c=.o)

.PHONY: all run clean

all: $(IMG) $(DATA_IMG)

$(BOOT_BIN): boot_stage1.asm boot_stage2.asm boot.inc
	$(NASM) -f bin boot_stage1.asm -o boot_stage1.bin
	$(NASM) -f bin boot_stage2.asm -o boot_stage2.bin
	cat boot_stage1.bin boot_stage2.bin > $@
	rm -f boot_stage1.bin boot_stage2.bin

kernel/%.o: kernel/%.asm
	$(NASM) -f elf64 $< -o $@

kernel/%.o: kernel/%.c
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

shell/%.o: shell/%.asm
	$(NASM) -f elf64 $< -o $@

apps/echo/%.o: apps/echo/%.asm
	$(NASM) -f elf64 $< -o $@

apps/echo/%.o: apps/echo/%.c
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/edit/%.o: apps/edit/%.asm
	$(NASM) -f elf64 $< -o $@

apps/edit/%.o: apps/edit/%.c
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/fm/%.o: apps/fm/%.asm
	$(NASM) -f elf64 $< -o $@

apps/fm/%.o: apps/fm/%.c
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

apps/less/%.o: apps/less/%.asm
	$(NASM) -f elf64 $< -o $@

apps/less/%.o: apps/less/%.c
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(KERNEL_ELF): $(KERNEL_OBJ) kernel/linker.ld
	$(LD) -m elf_x86_64 -T kernel/linker.ld -nostdlib -o $@ $(KERNEL_OBJ)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

SHELL_ELF = shell/shell.elf

$(SHELL_ELF): $(SHELL_OBJ) kernel/exec.o kernel/fos_api.o kernel/font8x16.o shell/linker.ld
	$(LD) -m elf_x86_64 -T shell/linker.ld -nostdlib -o $@ shell/entry.o \
		kernel/shell.o kernel/vfs.o kernel/block.o kernel/partition.o \
		kernel/fat32.o kernel/exfat.o kernel/console.o kernel/keyboard.o \
		kernel/string.o kernel/boot_report.o kernel/exec.o kernel/fos_api.o \
		kernel/font8x16.o

$(SHELL_BIN): $(SHELL_ELF)
	$(OBJCOPY) -O binary $< $@

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

$(IMG): $(BOOT_BIN) $(KERNEL_BIN) $(SHELL_BIN) $(ECHO_COM) $(EDIT_COM) $(FM_COM) $(LESS_COM) scripts/mkdisk.sh scripts/fat32_add.py
	scripts/mkdisk.sh $(IMG) $(BOOT_BIN) $(KERNEL_BIN) $(KERNEL_LBA) $(PART_START) $(SHELL_BIN) $(ECHO_COM) $(EDIT_COM) $(FM_COM) $(LESS_COM)

$(DATA_IMG): scripts/mkdata.sh
	scripts/mkdata.sh $(DATA_IMG)

run: $(IMG) $(DATA_IMG)
ifndef QEMU
	$(error QEMU not found – install qemu-system-x86_64)
endif
	$(QEMU) -drive format=raw,file=$(IMG),index=0,media=disk \
	        -drive format=raw,file=$(DATA_IMG),index=1,media=disk \
	        -device bochs-display \
	        -m 128M -serial stdio

clean:
	rm -f $(BOOT_BIN) $(KERNEL_BIN) $(SHELL_BIN) $(ECHO_BIN) $(ECHO_COM) \
	      $(EDIT_BIN) $(EDIT_COM) $(EDIT_ELF) $(FM_BIN) $(FM_COM) $(FM_ELF) \
	      $(LESS_BIN) $(LESS_COM) $(LESS_ELF) \
	      $(KERNEL_ELF) shell/shell.elf $(ECHO_ELF) \
	      $(IMG) $(DATA_IMG) $(KERNEL_OBJ) $(SHELL_OBJ) $(ECHO_OBJ) $(EDIT_OBJ) $(FM_OBJ) $(LESS_OBJ)
