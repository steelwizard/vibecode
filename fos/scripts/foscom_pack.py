#!/usr/bin/env python3
"""Wrap a flat binary in a FOS .COM (FOSCOM) header."""

import argparse
import struct
import sys

FOSCOM_MAGIC = 0x4F435346  # 'FSCO'
HEADER_SIZE = 64


def pack_header(entry, load_addr, payload_size, mem_size, stack_top, name):
    name_bytes = name.upper()[:15].encode("ascii")
    name_field = name_bytes.ljust(16, b"\x00")
    return struct.pack(
        "<IIQQQQQ16s",
        FOSCOM_MAGIC,
        1,
        entry,
        load_addr,
        payload_size,
        mem_size,
        stack_top,
        name_field,
    )


def main():
    p = argparse.ArgumentParser(description="Create FOS .COM file")
    p.add_argument("flat", help="flat binary payload")
    p.add_argument("out", help="output .COM path")
    p.add_argument("--load", type=lambda x: int(x, 0), required=True)
    p.add_argument("--entry", type=lambda x: int(x, 0), required=True)
    p.add_argument("--mem-size", type=lambda x: int(x, 0), required=True)
    p.add_argument("--stack", type=lambda x: int(x, 0), default=0x8F000)
    p.add_argument("--name", default="APP")
    args = p.parse_args()

    with open(args.flat, "rb") as f:
        payload = f.read()

    if args.mem_size < len(payload):
        print("mem-size must be >= payload size", file=sys.stderr)
        sys.exit(1)

    hdr = pack_header(
        args.entry,
        args.load,
        len(payload),
        args.mem_size,
        args.stack,
        args.name,
    )
    if len(hdr) != HEADER_SIZE:
        print(f"header size {len(hdr)} != {HEADER_SIZE}", file=sys.stderr)
        sys.exit(1)

    with open(args.out, "wb") as f:
        f.write(hdr)
        f.write(payload)

    print(
        f"packed {args.out}: {len(payload)} byte payload, "
        f"load {args.load:#x}, entry {args.entry:#x}, mem {args.mem_size}"
    )


if __name__ == "__main__":
    main()
