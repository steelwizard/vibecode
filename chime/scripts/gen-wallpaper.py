#!/usr/bin/env python3
"""Write the stock Chime clouds wallpaper (binary PPM) to argv[1]."""
import os
import sys

if len(sys.argv) != 2:
    print("usage: gen-wallpaper.py DEST.ppm", file=sys.stderr)
    sys.exit(2)

w, h = 320, 240
path = sys.argv[1]
os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
with open(path, "wb") as f:
    f.write(f"P6\n{w} {h}\n255\n".encode())
    for y in range(h):
        t = y / max(1, h - 1)
        for x in range(w):
            s = x / max(1, w - 1)
            r = int(18 + 36 * s + 40 * (1 - t))
            g = int(118 + 48 * (1 - t) + 18 * s)
            b = int(128 + 36 * t)
            n = ((x * 13 + y * 7) % 97) / 97.0
            if n > 0.74:
                r = min(255, r + 90)
                g = min(255, g + 78)
                b = min(255, b + 55)
            f.write(bytes((r, g, b)))
