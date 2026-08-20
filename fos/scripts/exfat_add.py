#!/usr/bin/env python3
"""Add a contiguous file to the root of an exFAT partition in a raw image.

FOS reads exFAT as NoFatChain when that flag is set, so files are stored as
a run of clusters and the allocation bitmap is updated.
"""

import os
import struct
import sys


def rd32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def rd64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def wr32(b, o, v):
    struct.pack_into("<I", b, o, v)


def wr64(b, o, v):
    struct.pack_into("<Q", b, o, v)


def wr16(b, o, v):
    struct.pack_into("<H", b, o, v)


def entry_set_checksum(blob: bytes) -> int:
    csum = 0
    for i, byte in enumerate(blob):
        if i == 2 or i == 3:
            continue
        csum = ((csum << 15) | (csum >> 1)) + byte
        csum &= 0xFFFF
    return csum


def name_hash(name: str) -> int:
    """exFAT NameHash: rotate-right-1 and add each UTF-16LE byte."""
    h = 0
    for ch in name.upper():
        c = ord(ch)
        h = (((h << 15) | (h >> 1)) + (c & 0xFF)) & 0xFFFF
        h = (((h << 15) | (h >> 1)) + (c >> 8)) & 0xFFFF
    return h


def parse_boot(f, part_lba):
    f.seek(part_lba * 512)
    b = f.read(512)
    if b[3:11] != b"EXFAT   ":
        raise SystemExit("not an exFAT volume")
    bps_shift = b[108]
    spc_shift = b[109]
    return {
        "part_lba": part_lba,
        "bps": 1 << bps_shift,
        "spc": 1 << spc_shift,
        "fat_lba": part_lba + rd32(b, 80),
        "heap_lba": part_lba + rd32(b, 88),
        "cluster_count": rd32(b, 92),
        "root": rd32(b, 96),
        "num_fats": b[110],
    }


def clus_lba(vol, clus):
    return vol["heap_lba"] + (clus - 2) * vol["spc"]


def read_clus(f, vol, clus):
    f.seek(clus_lba(vol, clus) * 512)
    return bytearray(f.read(vol["spc"] * 512))


def write_clus(f, vol, clus, data):
    f.seek(clus_lba(vol, clus) * 512)
    f.write(data)


def walk_root_entries(f, vol):
    clus = vol["root"]
    data = read_clus(f, vol, clus)
    i = 0
    while i + 32 <= len(data):
        yield i, data[i : i + 32]
        i += 32


def find_bitmap(f, vol):
    for off, e in walk_root_entries(f, vol):
        if e[0] == 0x81:
            return rd32(e, 20), rd64(e, 24)
    raise SystemExit("no exFAT allocation bitmap")


def bitmap_get(bits, cluster):
    bit = cluster - 2
    return (bits[bit // 8] >> (bit % 8)) & 1


def bitmap_set(bits, cluster):
    bit = cluster - 2
    bits[bit // 8] |= 1 << (bit % 8)


def alloc_run(bits, vol, nclus):
    start = 2
    while start + nclus - 1 < vol["cluster_count"] + 2:
        if all(bitmap_get(bits, start + i) == 0 for i in range(nclus)):
            for i in range(nclus):
                bitmap_set(bits, start + i)
            return start
        start += 1
    raise SystemExit("exFAT volume is full")


def find_dir_slot(data, need):
    """Return offset of `need` consecutive 32-byte free slots in one sector."""
    i = 0
    while i + 32 * need <= len(data):
        sector = i // 512
        if (i + 32 * need - 1) // 512 != sector:
            i = (sector + 1) * 512
            continue
        chunk = data[i : i + 32 * need]
        if all(chunk[j] == 0 for j in range(0, 32 * need, 32)):
            return i
        if chunk[0] == 0:
            # 0x00 ends the directory; this slot is free if the rest of the
            # sector is still unused.
            return i
        i += 32
    raise SystemExit("root directory is full")


def build_entries(name: str, first: int, size: int) -> bytes:
    nlen = len(name)
    nname = (nlen + 14) // 15
    secondary = 1 + nname
    file_e = bytearray(32)
    file_e[0] = 0x85
    file_e[1] = secondary
    wr16(file_e, 4, 0x20)  # archive

    stream = bytearray(32)
    stream[0] = 0xC0
    stream[1] = 0x03  # AllocationPossible | NoFatChain
    stream[3] = nlen
    wr16(stream, 4, name_hash(name))
    wr64(stream, 8, size)
    wr32(stream, 20, first)
    wr64(stream, 24, size)

    names = []
    for i in range(nname):
        e = bytearray(32)
        e[0] = 0xC1
        chunk = name[i * 15 : (i + 1) * 15]
        for j, ch in enumerate(chunk):
            wr16(e, 2 + j * 2, ord(ch))
        names.append(e)

    blob = bytes(file_e) + bytes(stream) + b"".join(bytes(x) for x in names)
    wr16(file_e, 2, entry_set_checksum(blob))
    return bytes(file_e) + bytes(stream) + b"".join(bytes(x) for x in names)


def add_file(img, part_lba, dest_name, src_path):
    data = open(src_path, "rb").read()
    with open(img, "r+b") as f:
        vol = parse_boot(f, part_lba)
        bclus, blen = find_bitmap(f, vol)
        bits = bytearray(read_clus(f, vol, bclus)[:blen])
        clus_bytes = vol["spc"] * 512
        nclus = max(1, (len(data) + clus_bytes - 1) // clus_bytes)
        first = alloc_run(bits, vol, nclus)

        padded = data + b"\x00" * (nclus * clus_bytes - len(data))
        for i in range(nclus):
            chunk = padded[i * clus_bytes : (i + 1) * clus_bytes]
            write_clus(f, vol, first + i, chunk)

        bm = read_clus(f, vol, bclus)
        bm[: len(bits)] = bits
        write_clus(f, vol, bclus, bm)

        root = read_clus(f, vol, vol["root"])
        entries = build_entries(dest_name, first, len(data))
        slot = find_dir_slot(root, len(entries) // 32)
        root[slot : slot + len(entries)] = entries
        write_clus(f, vol, vol["root"], root)


def main():
    if len(sys.argv) != 5:
        print("usage: exfat_add.py IMAGE PART_LBA DEST_NAME SRC_FILE", file=sys.stderr)
        return 1
    img, part, dest, src = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    if not os.path.isfile(src):
        print("missing", src, file=sys.stderr)
        return 1
    add_file(img, part, dest, src)
    return 0


if __name__ == "__main__":
    sys.exit(main())
