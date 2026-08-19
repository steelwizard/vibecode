# FOS — Flash Operating System (asm bootloader + C kernel + FAT32/exFAT)

IMG         = boot.img
BOOT_BIN    = boot.bin
KERNEL_ELF  = kernel/kernel.elf
KERNEL_BIN  = kernel.bin
SHELL_BIN   = shell.bin
ECHO_BIN    = apps/echo/echo.bin
ECHO_COM    = apps/echo/echo.com
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
             kernel/config.c

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

$(KERNEL_ELF): $(KERNEL_OBJ) kernel/linker.ld
	$(LD) -m elf_x86_64 -T kernel/linker.ld -nostdlib -o $@ $(KERNEL_OBJ)

$(KERNEL_BIN): $(KERNEL_ELF)
	$(OBJCOPY) -O binary $< $@

SHELL_ELF = shell/shell.elf

$(SHELL_ELF): $(SHELL_OBJ) kernel/exec.o kernel/fos_api.o shell/linker.ld
	$(LD) -m elf_x86_64 -T shell/linker.ld -nostdlib -o $@ shell/entry.o \
		kernel/shell.o kernel/vfs.o kernel/block.o kernel/partition.o \
		kernel/fat32.o kernel/exfat.o kernel/console.o kernel/keyboard.o \
		kernel/string.o kernel/boot_report.o kernel/exec.o kernel/fos_api.o

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

$(IMG): $(BOOT_BIN) $(KERNEL_BIN) $(SHELL_BIN) $(ECHO_COM) scripts/mkdisk.sh
	scripts/mkdisk.sh $(IMG) $(BOOT_BIN) $(KERNEL_BIN) $(KERNEL_LBA) $(PART_START) $(SHELL_BIN) $(ECHO_COM)

$(DATA_IMG): scripts/mkdata.sh
	scripts/mkdata.sh $(DATA_IMG)

run: $(IMG) $(DATA_IMG)
ifndef QEMU
	$(error QEMU not found – install qemu-system-x86_64)
endif
	$(QEMU) -drive format=raw,file=$(IMG),index=0,media=disk \
	        -drive format=raw,file=$(DATA_IMG),index=1,media=disk \
	        -m 128M -serial stdio

clean:
	rm -f $(BOOT_BIN) $(KERNEL_BIN) $(SHELL_BIN) $(ECHO_BIN) $(ECHO_COM) \
	      $(KERNEL_ELF) shell/shell.elf $(ECHO_ELF) \
	      $(IMG) $(DATA_IMG) $(KERNEL_OBJ) $(SHELL_OBJ) $(ECHO_OBJ)
