#!/bin/sh
# mkdisk.sh — build boot.img with MBR, boot sectors, kernel, FAT32 partition
set -e

IMG=$1
BOOT=$2
KERN=$3
KERN_LBA=$4
PART_START=$5
SHELL=${6:-}
ECHO=${7:-}
EDIT=${8:-}
FM=${9:-}
LESS=${10:-}

TOTAL_SECTORS=65536
PART_SECTORS=$((TOTAL_SECTORS - PART_START))

dd if=/dev/zero of="$IMG" bs=512 count=$TOTAL_SECTORS status=none
dd if="$BOOT" of="$IMG" conv=notrunc status=none
dd if="$KERN" of="$IMG" bs=512 seek="$KERN_LBA" conv=notrunc status=none

printf 'label: dos\nstart=%s,size=%s,type=c\n' "$PART_START" "$PART_SECTORS" | sfdisk "$IMG" >/dev/null

PART_KB=$((PART_SECTORS * 512 / 1024))
FAT_TMP=$(mktemp -u)
mkfs.vfat -F 32 -n "FOS" -S 512 -C "$FAT_TMP" "$PART_KB" >/dev/null
dd if="$FAT_TMP" of="$IMG" bs=512 seek="$PART_START" conv=notrunc status=none
rm -f "$FAT_TMP"

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

add_one() {
    name=$1
    file=$2
    if command -v mcopy >/dev/null 2>&1; then
        mcopy -i "$IMG" -s "$file" ::"$name"
    elif command -v python3 >/dev/null 2>&1; then
        python3 "$SCRIPT_DIR/fat32_add.py" "$IMG" "$PART_START" "$name" "$file"
    fi
}

add_one KERNEL.BIN "$KERN"
if [ -n "$SHELL" ] && [ -f "$SHELL" ]; then
    add_one SHELL.COM "$SHELL"
fi
if [ -n "$ECHO" ] && [ -f "$ECHO" ]; then
    add_one ECHO.COM "$ECHO"
fi
if [ -n "$EDIT" ] && [ -f "$EDIT" ]; then
    add_one EDIT.COM "$EDIT"
fi
if [ -n "$FM" ] && [ -f "$FM" ]; then
    add_one FM.COM "$FM"
fi
if [ -n "$LESS" ] && [ -f "$LESS" ]; then
    add_one LESS.COM "$LESS"
fi
if [ -f "$SCRIPT_DIR/../README.md" ]; then
    add_one README.TXT "$SCRIPT_DIR/../README.md"
fi
add_one SYSTEM.INI "$SCRIPT_DIR/../system.ini"

echo "Built $IMG — drive 0: FAT32 (LBA $PART_START, ${PART_SECTORS} sectors)"
