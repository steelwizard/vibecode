# Hello World OS

A minimal x86 real-mode operating system that prints **"Hello, World!"** and runs entirely inside QEMU. The whole OS is a single 512-byte MBR bootloader written in x86 assembly.

## How it works

When a PC powers on, the BIOS reads the first 512 bytes of the boot device into memory at address `0x7C00` and jumps there. That's our entire "OS":

1. Sets up segment registers and a stack.
2. Switches to 80×25 text mode via BIOS INT 10h.
3. Prints the greeting using BIOS INT 10h teletype output.
4. Halts forever.

The final two bytes are the magic MBR signature `0x55 0xAA` that tells the BIOS this sector is bootable.

## Prerequisites

| Tool | Install |
|------|---------|
| [NASM](https://nasm.us) | `winget install nasm` / `brew install nasm` / `apt install nasm` |
| [QEMU](https://www.qemu.org/download/) | `winget install qemu` / `brew install qemu` / `apt install qemu-system-x86` |

## Build & Run

### Windows

```bat
:: Build only
build.bat

:: Build and launch in QEMU
build.bat run
```

### Linux / macOS

```bash
# Build only
make

# Build and launch in QEMU
make run
```

### Manual QEMU command (any platform)

```bash
qemu-system-i386 -drive format=raw,file=boot.img -nographic
```

> **Quit QEMU:** press `Ctrl+A` then `X` when running with `-nographic`.  
> Without `-nographic`, close the QEMU window normally.

## File layout

```
helloworld-os/
├── boot.asm    # The entire OS — 512-byte x86 bootloader
├── boot.img    # (generated) raw disk image
├── build.bat   # Windows build script
├── Makefile    # Linux/macOS build script
└── README.md
```
