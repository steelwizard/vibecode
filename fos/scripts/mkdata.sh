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

FAT_TMP=$(mktemp -u)

if command -v mkfs.exfat >/dev/null 2>&1; then
    mkfs.exfat -n "EXDATA" -S 512 "$FAT_TMP" >/dev/null 2>&1 || \
        dd if=/dev/zero of="$FAT_TMP" bs=512 count=$PART_SECTORS status=none
    # mkfs.exfat may not support -C; build via loop or use fat32 fallback
    if ! command -v mkfs.exfat >/dev/null 2>&1 || [ ! -s "$FAT_TMP" ]; then
        mkfs.vfat -F 32 -n "EXDATA" -S 512 -C "$FAT_TMP" "$PART_KB" >/dev/null
    else
        truncate -s $((PART_SECTORS * 512)) "$FAT_TMP" 2>/dev/null || \
            dd if=/dev/zero of="$FAT_TMP" bs=512 count=$PART_SECTORS status=none
        mkfs.exfat -n "EXDATA" "$FAT_TMP" >/dev/null
    fi
    echo "Built $IMG — drive 1: exFAT"
else
    mkfs.vfat -F 32 -n "DATA1" -S 512 -C "$FAT_TMP" "$PART_KB" >/dev/null
    echo "Built $IMG — drive 1: FAT32 (install mkfs.exfat for exFAT)"
fi

dd if="$FAT_TMP" of="$IMG" bs=512 seek="$PART_START" conv=notrunc status=none
rm -f "$FAT_TMP"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
echo "Drive 1 test file." > /tmp/fos_d1.txt
if command -v mcopy >/dev/null 2>&1; then
    mcopy -i "$IMG" -s /tmp/fos_d1.txt ::TEST.TXT 2>/dev/null || true
elif command -v python3 >/dev/null 2>&1 && command -v mkfs.exfat >/dev/null 2>&1; then
    : # exFAT file injection not implemented; disk stays empty except label
elif command -v python3 >/dev/null 2>&1; then
    python3 "$SCRIPT_DIR/fat32_add.py" "$IMG" "$PART_START" TEST.TXT /tmp/fos_d1.txt || true
fi
rm -f /tmp/fos_d1.txt
