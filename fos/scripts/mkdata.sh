#!/bin/sh
# mkdata.sh — second disk for drive 1: (exFAT if available, else FAT32)
set -e

IMG=$1
TOTAL_SECTORS=65536
PART_START=2048
PART_SECTORS=$((TOTAL_SECTORS - PART_START))
PART_KB=$((PART_SECTORS * 512 / 1024))

dd if=/dev/zero of="$IMG" bs=512 count=$TOTAL_SECTORS status=none
printf 'label: dos\nstart=%s,size=%s,type=7\n' "$PART_START" "$PART_SECTORS" | sfdisk "$IMG" >/dev/null

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FAT_TMP=$(mktemp)
FS_KIND=

if command -v mkfs.exfat >/dev/null 2>&1; then
    dd if=/dev/zero of="$FAT_TMP" bs=512 count=$PART_SECTORS status=none
    if mkfs.exfat -n "EXDATA" "$FAT_TMP" >/dev/null 2>&1; then
        FS_KIND=exfat
        echo "Built $IMG — drive 1: exFAT"
    fi
fi

if [ -z "$FS_KIND" ]; then
    mkfs.vfat -F 32 -n "DATA1" -S 512 -C "$FAT_TMP" "$PART_KB" >/dev/null
    FS_KIND=fat32
    echo "Built $IMG — drive 1: FAT32 (install mkfs.exfat for exFAT)"
fi

dd if="$FAT_TMP" of="$IMG" bs=512 seek="$PART_START" conv=notrunc status=none
rm -f "$FAT_TMP"

echo "Drive 1 test file." > /tmp/fos_d1.txt
if [ "$FS_KIND" = fat32 ]; then
    if command -v mcopy >/dev/null 2>&1; then
        mcopy -i "$IMG"@@${PART_START}s -s /tmp/fos_d1.txt ::TEST.TXT 2>/dev/null || true
    elif command -v python3 >/dev/null 2>&1; then
        python3 "$SCRIPT_DIR/fat32_add.py" "$IMG" "$PART_START" TEST.TXT /tmp/fos_d1.txt || true
    fi
elif [ "$FS_KIND" = exfat ]; then
    # Offset-aware mcopy works for FAT; for exFAT use a loop mount when root,
    # otherwise leave the volume empty (read-only FS in FOS anyway).
    if command -v mcopy >/dev/null 2>&1; then
        mcopy -i "$IMG"@@${PART_START}s -s /tmp/fos_d1.txt ::TEST.TXT 2>/dev/null || true
    fi
fi
rm -f /tmp/fos_d1.txt
