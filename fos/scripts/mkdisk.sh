#!/bin/sh
# mkdisk.sh — build boot.img with MBR, boot sectors, kernel, FAT32 ESP
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
DATE=${11:-}
MEM=${12:-}
EFI=${13:-}
BEEP=${14:-}
PLAY=${15:-}
PAINT=${16:-}
GREP=${17:-}
BENCH=${18:-}

TOTAL_SECTORS=131072
PART_SECTORS=$((TOTAL_SECTORS - PART_START))

dd if=/dev/zero of="$IMG" bs=512 count=$TOTAL_SECTORS status=none
dd if="$BOOT" of="$IMG" conv=notrunc status=none
dd if="$KERN" of="$IMG" bs=512 seek="$KERN_LBA" conv=notrunc status=none

# 0xEF = EFI System Partition so OVMF will boot \EFI\BOOT\BOOTX64.EFI.
# The BIOS MBR path still works; the kernel also mounts type EF as FAT32.
printf 'label: dos\nstart=%s,size=%s,type=ef,bootable\n' "$PART_START" "$PART_SECTORS" | sfdisk "$IMG" >/dev/null

PART_KB=$((PART_SECTORS * 512 / 1024))
FAT_TMP=$(mktemp -u)
mkfs.vfat -F 32 -s 1 -n "FOSBOOT" -S 512 -C "$FAT_TMP" "$PART_KB" >/dev/null
dd if="$FAT_TMP" of="$IMG" bs=512 seek="$PART_START" conv=notrunc status=none
rm -f "$FAT_TMP"

# UEFI FatDxe rejects FAT32 that still has TotSec16 set (mkfs.vfat -C does that
# on this size). HiddenSectors must match the partition LBA.
python3 - "$IMG" "$PART_START" "$PART_SECTORS" <<'PY'
import struct, sys
img, part, sectors = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(img, "r+b") as f:
    def patch(lba):
        f.seek(lba * 512)
        boot = bytearray(f.read(512))
        if boot[82:90] not in (b"FAT32   ", b"FAT     "):
            return
        struct.pack_into("<H", boot, 19, 0)
        struct.pack_into("<I", boot, 28, part)
        struct.pack_into("<I", boot, 32, sectors)
        f.seek(lba * 512)
        f.write(boot)
    patch(part)
    f.seek(part * 512)
    boot0 = f.read(90)
    backup = struct.unpack_from("<H", boot0, 50)[0]
    if backup:
        patch(part + backup)
PY

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FAT_IMG="${IMG}@@${PART_START}s"

ensure_mtools_dir() {
    path=$1
    parent=$(dirname "$path")
    if [ "$parent" = "." ] || [ "$parent" = "/" ]; then
        return 0
    fi
    ensure_mtools_dir "$parent"
    mmd -i "$FAT_IMG" "::${parent}" >/dev/null 2>&1 || true
}

add_one() {
    name=$1
    file=$2
    if command -v mcopy >/dev/null 2>&1; then
        case "$name" in
            */*) ensure_mtools_dir "$name" ;;
        esac
        mcopy -i "$FAT_IMG" -o "$file" ::"$name"
    elif command -v python3 >/dev/null 2>&1; then
        python3 "$SCRIPT_DIR/fat32_add.py" "$IMG" "$PART_START" "$name" "$file"
    fi
}

add_one KERNEL.BIN "$KERN"
if [ -n "$SHELL" ] && [ -f "$SHELL" ]; then
    add_one SHELL.COM "$SHELL"
fi
if [ -n "$ECHO" ] && [ -f "$ECHO" ]; then
    add_one FOS/ECHO.COM "$ECHO"
fi
if [ -n "$EDIT" ] && [ -f "$EDIT" ]; then
    add_one FOS/EDIT.COM "$EDIT"
fi
if [ -n "$FM" ] && [ -f "$FM" ]; then
    add_one FOS/FM.COM "$FM"
fi
if [ -n "$LESS" ] && [ -f "$LESS" ]; then
    add_one FOS/LESS.COM "$LESS"
fi
if [ -n "$DATE" ] && [ -f "$DATE" ]; then
    add_one FOS/DATE.COM "$DATE"
fi
if [ -n "$MEM" ] && [ -f "$MEM" ]; then
    add_one FOS/MEM.COM "$MEM"
fi
if [ -f "$SCRIPT_DIR/../README.md" ]; then
    add_one README.TXT "$SCRIPT_DIR/../README.md"
fi
add_one SYSTEM.INI "$SCRIPT_DIR/../system.ini"
if [ -n "$EFI" ] && [ -f "$EFI" ]; then
    add_one EFI/BOOT/BOOTX64.EFI "$EFI"
fi
if [ -n "$BEEP" ] && [ -f "$BEEP" ]; then
    add_one FOS/BEEP.COM "$BEEP"
fi
if [ -n "$PLAY" ] && [ -f "$PLAY" ]; then
    add_one FOS/PLAY.COM "$PLAY"
fi
if [ -n "$PAINT" ] && [ -f "$PAINT" ]; then
    add_one FOS/PAINT.COM "$PAINT"
fi
if [ -n "$GREP" ] && [ -f "$GREP" ]; then
    add_one FOS/GREP.COM "$GREP"
fi
if [ -n "$BENCH" ] && [ -f "$BENCH" ]; then
    add_one FOS/BENCH.COM "$BENCH"
fi
if [ -f "$SCRIPT_DIR/../demo.bat" ]; then
    add_one FOS/DEMO.BAT "$SCRIPT_DIR/../demo.bat"
fi

DEMO_WAV=$(mktemp)
python3 - "$DEMO_WAV" <<'PY'
import struct, sys
path = sys.argv[1]
rate, ms = 8000, 2000
n = rate * ms // 1000
pcm = bytearray(n)
half = max(rate // (440 * 2), 1)
for i in range(n):
    pcm[i] = 0xE0 if ((i // half) & 1) == 0 else 0x20
data = bytes(pcm)
hdr = struct.pack(
    "<4sI4s4sIHHIIHH4sI",
    b"RIFF", 36 + len(data), b"WAVE",
    b"fmt ", 16, 1, 1, rate, rate, 1, 8,
    b"data", len(data),
)
open(path, "wb").write(hdr + data)
PY
add_one DEMO.WAV "$DEMO_WAV"
if command -v ffmpeg >/dev/null 2>&1; then
    DEMO_MP3=$(mktemp --suffix=.mp3)
    if ffmpeg -loglevel error -y -f lavfi -i sine=frequency=440:duration=2 -ac 1 -ar 22050 -c:a libmp3lame -q:a 7 "$DEMO_MP3"; then
        add_one DEMO.MP3 "$DEMO_MP3"
    fi
    rm -f "$DEMO_MP3"
fi
rm -f "$DEMO_WAV"

# GM soundfont for play FILE.MID. Default is a tiny CC0 wavetable bank
# (scripts/mksf2.py). Override with FOS_SF2=/path/to/file.sf2 or data/GM.SF2
# (e.g. TimGM6mb) for a full sampled GM set — loading a 6 MiB font over ATA PIO
# is slow.
if [ -n "${FOS_SF2:-}" ] && [ -f "$FOS_SF2" ]; then
    add_one FOS/GM.SF2 "$FOS_SF2"
elif [ -f "$SCRIPT_DIR/../data/GM.SF2" ]; then
    add_one FOS/GM.SF2 "$SCRIPT_DIR/../data/GM.SF2"
else
    GM_SF2=$(mktemp)
    python3 "$SCRIPT_DIR/mksf2.py" "$GM_SF2"
    add_one FOS/GM.SF2 "$GM_SF2"
    rm -f "$GM_SF2"
fi
DEMO_MID=$(mktemp)
python3 "$SCRIPT_DIR/mkmidi.py" "$DEMO_MID"
add_one DEMO.MID "$DEMO_MID"
rm -f "$DEMO_MID"

MIDI_DIR="$SCRIPT_DIR/../data/midi"
if [ -d "$MIDI_DIR" ]; then
    if [ -f "$MIDI_DIR/LIST.TXT" ]; then
        add_one MIDI/LIST.TXT "$MIDI_DIR/LIST.TXT"
    fi
    for f in "$MIDI_DIR"/*.mid; do
        [ -f "$f" ] || continue
        base=$(basename "$f" .mid | tr '[:lower:]' '[:upper:]')
        add_one "MIDI/${base}.MID" "$f"
    done
fi

echo "Built $IMG — drive 0: FAT32 ESP (LBA $PART_START, ${PART_SECTORS} sectors)"
