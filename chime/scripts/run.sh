#!/bin/sh
# Boot chime.iso in QEMU. -vga std gives vesafb (/dev/fb0) for Xfbdev.
# DUAL=1 uses virtio-vga with two outputs (host WM multi-head test).
# usb-tablet avoids pointer grab; nic none keeps the guest offline.
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

AUDIO_ARGS=${QEMU_AUDIO:-}
if [ -z "$AUDIO_ARGS" ]; then
    AUDIO_ARGS="-device intel-hda -device hda-duplex,audiodev=snd"
    if command -v pactl >/dev/null 2>&1 && pactl info >/dev/null 2>&1; then
        AUDIO_ARGS="$AUDIO_ARGS -audiodev pa,id=snd"
    elif [ -d /dev/snd ]; then
        AUDIO_ARGS="$AUDIO_ARGS -audiodev alsa,id=snd"
    else
        AUDIO_ARGS="$AUDIO_ARGS -audiodev none,id=snd"
    fi
fi

exec qemu-system-x86_64 \
    $KVM \
    -m "${MEM:-512}" \
    -cdrom "$ISO" \
    $VGA_ARGS \
    $AUDIO_ARGS \
    -display gtk \
    -usb -device usb-tablet \
    -nic none \
    "$@"
