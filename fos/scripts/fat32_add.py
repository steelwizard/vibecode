#!/usr/bin/env python3
"""Add 8.3-named files to a FAT32 partition inside a raw disk image.

Names may be nested paths (EFI/BOOT/BOOTX64.EFI); missing directories are created.
"""

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
    if name == ".":
        return b".          "
    if name == "..":
        return b"..         "
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


def find_free_cluster(f, bpb, skip=None):
    skip = skip or set()
    cluster = 2
    while cluster < 0x0FFFFFF0:
        if cluster not in skip and read_fat_entry(f, bpb, cluster) == 0:
            return cluster
        cluster += 1
    raise SystemExit("no free clusters")


def first_cluster(ent: bytes) -> int:
    return rd16(ent, 26) | (rd16(ent, 20) << 16)


def write_dir_entry(buf, slot, nm, attr, cluster, size):
    buf[slot : slot + 11] = nm
    buf[slot + 11] = attr
    wr16(buf, slot + 26, cluster & 0xFFFF)
    wr16(buf, slot + 20, (cluster >> 16) & 0xFFFF)
    wr32(buf, slot + 28, size)


def alloc_zero_cluster(f, bpb, used=None):
    cl = find_free_cluster(f, bpb, used)
    write_fat_entry(f, bpb, cl, 0x0FFFFFF8)
    lba = cluster_lba(bpb, cl)
    zeros = bytes(bpb["bytes_per_sector"])
    for s in range(bpb["sectors_per_cluster"]):
        write_sector(f, bpb, lba + s, zeros)
    return cl


def cluster_chain(f, bpb, start):
    cluster = start
    while cluster >= 2 and cluster < 0x0FFFFFF0:
        yield cluster
        nxt = read_fat_entry(f, bpb, cluster)
        if nxt >= 0x0FFFFFF8:
            break
        cluster = nxt


def find_named(f, bpb, dir_cluster, nm: bytes):
    for cluster in cluster_chain(f, bpb, dir_cluster):
        lba0 = cluster_lba(bpb, cluster)
        for s in range(bpb["sectors_per_cluster"]):
            sector = read_sector(f, bpb, lba0 + s)
            for i in range(0, len(sector), 32):
                if sector[i] == 0x00:
                    return None
                if sector[i] == 0xE5:
                    continue
                attr = sector[i + 11]
                if attr == 0x0F or (attr & 0x08):
                    continue
                if sector[i : i + 11] == nm:
                    return lba0 + s, i, sector[i : i + 32]
    return None


def find_dir_slot(f, bpb, dir_cluster):
    last = dir_cluster
    for cluster in cluster_chain(f, bpb, dir_cluster):
        last = cluster
        lba0 = cluster_lba(bpb, cluster)
        for s in range(bpb["sectors_per_cluster"]):
            sector = read_sector(f, bpb, lba0 + s)
            for i in range(0, len(sector), 32):
                if sector[i] == 0x00 or sector[i] == 0xE5:
                    return lba0 + s, i
    new_cl = alloc_zero_cluster(f, bpb)
    write_fat_entry(f, bpb, last, new_cl)
    return cluster_lba(bpb, new_cl), 0


def ensure_dir(f, bpb, parent_cluster, name: str) -> int:
    nm = name83(name)
    found = find_named(f, bpb, parent_cluster, nm)
    if found:
        _lba, _slot, ent = found
        if ent[11] & 0x10:
            cl = first_cluster(ent)
            return cl if cl else bpb["root_cluster"]
        raise SystemExit(f"{name} exists and is not a directory")

    new_cl = alloc_zero_cluster(f, bpb)
    lba = cluster_lba(bpb, new_cl)
    sector = bytearray(read_sector(f, bpb, lba))
    parent_dot = 0 if parent_cluster == bpb["root_cluster"] else parent_cluster
    write_dir_entry(sector, 0, name83("."), 0x10, new_cl, 0)
    write_dir_entry(sector, 32, name83(".."), 0x10, parent_dot, 0)
    write_sector(f, bpb, lba, sector)

    slot_lba, slot = find_dir_slot(f, bpb, parent_cluster)
    parent = bytearray(read_sector(f, bpb, slot_lba))
    write_dir_entry(parent, slot, nm, 0x10, new_cl, 0)
    write_sector(f, bpb, slot_lba, parent)
    return new_cl


def add_file(img_path, part_lba, fat_path, content: bytes):
    parts = fat_path.replace("\\", "/").strip("/").split("/")
    if not parts or not parts[-1]:
        raise SystemExit(f"bad path {fat_path!r}")
    dirs, fat_name = parts[:-1], parts[-1]

    with open(img_path, "r+b") as f:
        bpb = parse_bpb(f, part_lba)
        dir_cluster = bpb["root_cluster"]
        for d in dirs:
            dir_cluster = ensure_dir(f, bpb, dir_cluster, d)

        data = content
        spc_bytes = bpb["sectors_per_cluster"] * bpb["bytes_per_sector"]

        clusters = []
        used = set()
        remaining = max(len(data), 1)
        while remaining > 0:
            cl = find_free_cluster(f, bpb, used)
            clusters.append(cl)
            used.add(cl)
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

        slot_lba, slot = find_dir_slot(f, bpb, dir_cluster)
        if slot < 0:
            raise SystemExit("directory full")

        dsec = bytearray(read_sector(f, bpb, slot_lba))
        write_dir_entry(dsec, slot, name83(fat_name), 0x20, clusters[0], len(data))
        write_sector(f, bpb, slot_lba, dsec)


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
