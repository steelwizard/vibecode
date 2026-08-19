#!/usr/bin/env python3
"""Add a small 8.3-named file to a FAT32 partition inside a raw disk image."""

import struct
import sys


def rd16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def rd32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def wr32(b, o, v):
    struct.pack_into("<I", b, o, v)


def wr16(b, o, v):
    struct.pack_into("<H", b, o, v)


def name83(name: str) -> bytes:
    base, _, ext = name.upper().partition(".")
    base = base[:8].ljust(8)
    ext = ext[:3].ljust(3)
    return (base + ext).encode("ascii")


def cluster_lba(bpb, cluster):
    return (
        bpb["data_lba"]
        + (cluster - 2) * bpb["sectors_per_cluster"]
    )


def read_fat_entry(f, bpb, cluster):
    off = cluster * 4
    fat_lba = bpb["fat_lba"] + off // bpb["bytes_per_sector"]
    ent_off = off % bpb["bytes_per_sector"]
    f.seek(fat_lba * bpb["bytes_per_sector"])
    sector = f.read(bpb["bytes_per_sector"])
    return rd32(sector, ent_off) & 0x0FFFFFFF


def write_fat_entry(f, bpb, cluster, value):
    off = cluster * 4
    fat_lba = bpb["fat_lba"] + off // bpb["bytes_per_sector"]
    ent_off = off % bpb["bytes_per_sector"]
    f.seek(fat_lba * bpb["bytes_per_sector"])
    sector = bytearray(f.read(bpb["bytes_per_sector"]))
    old = rd32(sector, ent_off)
    wr32(sector, ent_off, (old & 0xF0000000) | (value & 0x0FFFFFFF))
    f.seek(fat_lba * bpb["bytes_per_sector"])
    f.write(sector)
    # mirror second FAT if present
    if bpb["num_fats"] > 1:
        f.seek((fat_lba + bpb["fat_size"]) * bpb["bytes_per_sector"])
        f.write(sector)


def read_sector(f, bpb, lba):
    f.seek(lba * bpb["bytes_per_sector"])
    return f.read(bpb["bytes_per_sector"])


def write_sector(f, bpb, lba, data):
    f.seek(lba * bpb["bytes_per_sector"])
    f.write(data)


def parse_bpb(f, part_lba):
    boot = read_sector(f, {"bytes_per_sector": 512}, part_lba)
    if boot[82:90] not in (b"FAT32   ", b"FAT     "):
        raise SystemExit("not FAT32")
    bps = rd16(boot, 11)
    spc = boot[13]
    reserved = rd16(boot, 14)
    num_fats = boot[16]
    fat_size = rd32(boot, 36)
    root_cluster = rd32(boot, 44)
    fat_lba = part_lba + reserved
    data_lba = fat_lba + num_fats * fat_size
    return {
        "bytes_per_sector": bps,
        "sectors_per_cluster": spc,
        "num_fats": num_fats,
        "fat_size": fat_size,
        "fat_lba": fat_lba,
        "data_lba": data_lba,
        "root_cluster": root_cluster,
    }


def find_free_cluster(f, bpb):
    cluster = 2
    while cluster < 0x0FFFFFF0:
        if read_fat_entry(f, bpb, cluster) == 0:
            return cluster
        cluster += 1
    raise SystemExit("no free clusters")


def find_dir_slot(f, bpb):
    cluster = bpb["root_cluster"]
    while cluster >= 2 and cluster < 0x0FFFFFF0:
        lba = cluster_lba(bpb, cluster)
        for s in range(bpb["sectors_per_cluster"]):
            root = read_sector(f, bpb, lba + s)
            for i in range(0, len(root), 32):
                if root[i] == 0x00 or root[i] == 0xE5:
                    return lba + s, i
        nxt = read_fat_entry(f, bpb, cluster)
        if nxt >= 0x0FFFFFF8:
            break
        cluster = nxt
    return None, -1


def add_file(img_path, part_lba, fat_name, content: bytes):
    with open(img_path, "r+b") as f:
        bpb = parse_bpb(f, part_lba)
        data = content
        spc_bytes = bpb["sectors_per_cluster"] * bpb["bytes_per_sector"]

        clusters = []
        remaining = max(len(data), 1)
        while remaining > 0:
            clusters.append(find_free_cluster(f, bpb))
            remaining -= spc_bytes

        if not clusters:
            clusters = [find_free_cluster(f, bpb)]

        for i, cl in enumerate(clusters):
            nxt = 0x0FFFFFF8 if i + 1 == len(clusters) else clusters[i + 1]
            write_fat_entry(f, bpb, cl, nxt)

        offset = 0
        for cl in clusters:
            lba = cluster_lba(bpb, cl)
            chunk = data[offset : offset + spc_bytes]
            chunk = chunk.ljust(spc_bytes, b"\x00")
            write_sector(f, bpb, lba, chunk)
            offset += spc_bytes

        root_lba, slot = find_dir_slot(f, bpb)
        if slot < 0:
            raise SystemExit("root directory full")

        root = bytearray(read_sector(f, bpb, root_lba))
        nm = name83(fat_name)
        root[slot : slot + 11] = nm
        root[slot + 11] = 0x20  # archive
        wr16(root, slot + 26, clusters[0] & 0xFFFF)
        wr16(root, slot + 20, (clusters[0] >> 16) & 0xFFFF)
        wr32(root, slot + 28, len(data))
        write_sector(f, bpb, root_lba, root)


def add_files(img_path, part_lba, pairs):
    for fat_name, src in pairs:
        with open(src, "rb") as fh:
            data = fh.read()
        add_file(img_path, part_lba, fat_name, data)
        print(f"added {fat_name} ({len(data)} bytes) to {img_path} @ LBA {part_lba}")


def main():
    if len(sys.argv) < 5 or (len(sys.argv) - 3) % 2 != 0:
        print(
            f"usage: {sys.argv[0]} image.img part_lba NAME file [NAME file ...]",
            file=sys.stderr,
        )
        sys.exit(1)
    img = sys.argv[1]
    part_lba = int(sys.argv[2])
    pairs = []
    args = sys.argv[3:]
    for i in range(0, len(args), 2):
        pairs.append((args[i], args[i + 1]))
    add_files(img, part_lba, pairs)


if __name__ == "__main__":
    main()
