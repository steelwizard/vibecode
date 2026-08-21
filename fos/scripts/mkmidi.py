#!/usr/bin/env python3
"""Write a short public-domain MIDI (Ode to Joy) as DEMO.MID."""
import struct
import sys

JOY = [
    64, 64, 65, 67, 67, 65, 64, 62,
    60, 60, 62, 64, 64, 62, 62, 0,
    64, 64, 65, 67, 67, 65, 64, 62,
    60, 60, 62, 64, 62, 60, 60, 0,
]


def vlq(n):
    buf = [n & 0x7F]
    n >>= 7
    while n:
        buf.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(buf))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "DEMO.MID"
    ev = bytearray()
    ev += vlq(0) + bytes([0xC0, 0])
    pending = 0
    for n in JOY:
        if n == 0:
            pending += 480
            continue
        ev += vlq(pending) + bytes([0x90, n, 96])
        pending = 0
        ev += vlq(420) + bytes([0x80, n, 0])
        pending = 60
    ev += vlq(pending) + bytes([0xFF, 0x2F, 0x00])
    trk = b"MTrk" + struct.pack(">I", len(ev)) + bytes(ev)
    hdr = b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480)
    with open(path, "wb") as f:
        f.write(hdr + trk)


if __name__ == "__main__":
    main()
