@echo off
:: build.bat — FOS (Flash Operating System)
setlocal

set BOOT_BIN=boot.bin
set KERNEL_ELF=kernel\kernel.elf
set KERNEL_BIN=kernel.bin
set SHELL_ELF=shell\shell.elf
set SHELL_BIN=shell.bin
set IMG=boot.img
set DATA=data.img
set CFLAGS=-m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -Wall -Wextra -O2 -I kernel/include

where nasm >nul 2>&1 || (echo ERROR: nasm not found & exit /b 1)
where gcc  >nul 2>&1 || (echo ERROR: gcc not found & exit /b 1)

echo Assembling bootloader ...
nasm -f bin boot_stage1.asm -o boot_stage1.bin
nasm -f bin boot_stage2.asm -o boot_stage2.bin
copy /b boot_stage1.bin+boot_stage2.bin %BOOT_BIN% >nul
del boot_stage1.bin boot_stage2.bin

echo Compiling kernel ...
nasm -f elf64 kernel/entry.asm -o kernel/entry.o
gcc %CFLAGS% -c kernel/kernel.c -o kernel/kernel.o
gcc %CFLAGS% -c kernel/shell.c -o kernel/shell.o
gcc %CFLAGS% -c kernel/vfs.c -o kernel/vfs.o
gcc %CFLAGS% -c kernel/block.c -o kernel/block.o
gcc %CFLAGS% -c kernel/partition.c -o kernel/partition.o
gcc %CFLAGS% -c kernel/fat32.c -o kernel/fat32.o
gcc %CFLAGS% -c kernel/exfat.c -o kernel/exfat.o
gcc %CFLAGS% -c kernel/console.c -o kernel/console.o
gcc %CFLAGS% -c kernel/keyboard.c -o kernel/keyboard.o
gcc %CFLAGS% -c kernel/string.c -o kernel/string.o
gcc %CFLAGS% -c kernel/cpu.c -o kernel/cpu.o
gcc %CFLAGS% -c kernel/boot_report.c -o kernel/boot_report.o
ld -m elf_x86_64 -T kernel/linker.ld -nostdlib -o %KERNEL_ELF% kernel/entry.o kernel/kernel.o kernel/shell.o kernel/vfs.o kernel/block.o kernel/partition.o kernel/fat32.o kernel/exfat.o kernel/console.o kernel/keyboard.o kernel/string.o kernel/cpu.o kernel/boot_report.o
objcopy -O binary %KERNEL_ELF% %KERNEL_BIN%

echo Building shell ...
nasm -f elf64 shell/entry.asm -o shell/entry.o
ld -m elf_x86_64 -T shell/linker.ld -nostdlib -o %SHELL_ELF% shell/entry.o kernel/shell.o kernel/vfs.o kernel/block.o kernel/partition.o kernel/fat32.o kernel/exfat.o kernel/console.o kernel/keyboard.o kernel/string.o kernel/boot_report.o
objcopy -O binary %SHELL_ELF% %SHELL_BIN%

bash scripts/mkdisk.sh %IMG% %BOOT_BIN% %KERNEL_BIN% 9 2048 %SHELL_BIN%
bash scripts/mkdata.sh %DATA%

echo OK: %IMG% %DATA%

if /i not "%1"=="run" goto :eof
qemu-system-x86_64 -drive format=raw,file=%IMG%,index=0,media=disk -drive format=raw,file=%DATA%,index=1,media=disk -m 512M -serial stdio -display none
