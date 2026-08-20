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
DATA_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../data" && pwd)
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

add_sample() {
    dest=$1
    src=$2
    [ -f "$src" ] || return 0
    if [ "$FS_KIND" = fat32 ]; then
        if command -v mcopy >/dev/null 2>&1; then
            mcopy -i "$FAT_TMP" -s "$src" "::$dest" 2>/dev/null || true
        elif command -v python3 >/dev/null 2>&1; then
            python3 "$SCRIPT_DIR/fat32_add.py" "$FAT_TMP" 0 "$dest" "$src" || true
        fi
    else
        python3 "$SCRIPT_DIR/exfat_add.py" "$FAT_TMP" 0 "$dest" "$src"
    fi
}

add_sample LOREM.TXT "$DATA_DIR/LOREM.TXT"
add_sample IPSUM.TXT "$DATA_DIR/IPSUM.TXT"
add_sample CICERO.TXT "$DATA_DIR/CICERO.TXT"

dd if="$FAT_TMP" of="$IMG" bs=512 seek="$PART_START" conv=notrunc status=none
rm -f "$FAT_TMP"
