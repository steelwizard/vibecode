#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ISO=$ROOT/chime.iso
if [ ! -f "$ISO" ]; then
    echo "missing $ISO — run make image" >&2
    exit 1
fi

KVM=
if [ -r /dev/kvm ]; then
    KVM=-enable-kvm
fi

VGA_ARGS="-vga std"
if [ "${DUAL:-}" = "1" ]; then
    VGA_ARGS="-vga none -device virtio-vga,max_outputs=2"
fi

exec qemu-system-x86_64 \
    $KVM \
    -m "${MEM:-512}" \
    -cdrom "$ISO" \
    $VGA_ARGS \
    -display gtk \
    -usb -device usb-tablet \
    -nic none \
    "$@"
