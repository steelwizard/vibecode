# Makefile for Hello World OS (Linux / macOS)
# Requires: nasm, qemu-system-i386 (or qemu-system-x86_64)

IMG = boot.img
SRC = boot.asm

QEMU := $(shell command -v qemu-system-i386 2>/dev/null || command -v qemu-system-x86_64 2>/dev/null)

.PHONY: all run clean

all: $(IMG)

$(IMG): $(SRC)
	nasm -f bin $< -o $@
	@echo "Built $@ ($$(wc -c < $@) bytes)"

run: $(IMG)
ifndef QEMU
	$(error QEMU not found – install qemu-system-i386 or qemu-system-x86_64)
endif
	$(QEMU) -drive format=raw,file=$(IMG) -nographic

clean:
	rm -f $(IMG)
