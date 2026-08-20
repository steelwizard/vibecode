#!/usr/bin/env python3
"""FM: line-oriented .COM output is shown on a grey screen with OK."""

import os
import sys
import time
import struct
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "scripts"))
from qemu_console import Machine, image_copy, cpu_exceptions  # noqa: E402

HERE = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(HERE, ".tmp")
os.chdir(HERE)

GREY = (0xAA, 0xAA, 0xAA)
BLUE = (0x00, 0x00, 0xAA)


def ppm_to_png(src, dst):
    with open(src, "rb") as fh:
        assert fh.readline().strip() == b"P6"
        line = fh.readline()
        while line.startswith(b"#"):
            line = fh.readline()
        w, h = (int(v) for v in line.split())
        fh.readline()
        data = fh.read()
    raw = b"".join(b"\x00" + data[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        body = tag + payload
        return (struct.pack(">I", len(payload)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(raw, 6)) +
           chunk(b"IEND", b""))
    with open(dst, "wb") as fh:
        fh.write(png)
    return w, h, data


def count_color(data, rgb, tol=12):
    r0, g0, b0 = rgb
    n = 0
    for i in range(0, len(data), 3):
        r, g, b = data[i], data[i + 1], data[i + 2]
        if abs(r - r0) <= tol and abs(g - g0) <= tol and abs(b - b0) <= tol:
            n += 1
    return n


def sendkeys(m, keys, settle=0.12):
    for k in keys:
        m.monitor_cmd(f"sendkey {k}", settle=settle)


def dump(m, name, settle=0.8):
    ppm = os.path.join(OUT, name + ".ppm")
    png = os.path.join(OUT, name + ".png")
    m.monitor_cmd(f"screendump {ppm}", settle=settle)
    w, h, data = ppm_to_png(ppm, png)
    grey = count_color(data, GREY)
    blue = count_color(data, BLUE)
    print(f"{name}: {w}x{h} grey={grey} dark-blue={blue}")
    return grey, blue


def wait_prompt(m, mark, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m.pump()
        out = m.buf[mark:].decode("latin1", "replace").rstrip()
        if out.endswith(">") and ":\\" in out.splitlines()[-1]:
            return out
        time.sleep(0.05)
    return None


def main():
    image = image_copy()
    intlog = os.path.join(OUT, "int-fm-out.log")
    if os.path.exists(intlog):
        os.unlink(intlog)
    m = Machine(image, intlog=intlog)
    try:
        if not m.wait_boot():
            print("FAIL: no boot")
            return 1
        print("boot ok")

        m.send("fm\r")
        time.sleep(1.4)
        dump(m, "fm-out-browse")
        sendkeys(m, ["down", "ret"])  # FOS
        time.sleep(0.5)
        sendkeys(m, ["home", "down", "down", "ret"])  # DATE.COM
        time.sleep(0.8)
        g, b = dump(m, "fm-out-date")
        if g < 50000:
            print("FAIL: DATE.COM did not open the grey output screen")
            return 1
        if b < 200:
            print("FAIL: output screen missing blue border/button")
            return 1
        print("ok DATE output screen")

        sendkeys(m, ["ret"])
        time.sleep(0.6)
        g2, b2 = dump(m, "fm-out-after-date")
        if g2 > 20000:
            print("FAIL: Enter on OK did not return to FM")
            return 1
        if b2 < 10000:
            print("FAIL: FM desktop missing after OK")
            return 1
        print("ok returned to FM")

        sendkeys(m, ["end", "ret"])  # PLAY.COM
        time.sleep(1.0)
        g3, b3 = dump(m, "fm-out-play")
        if g3 > 400000:
            print("FAIL: PLAY.COM was wrapped in the grey output screen")
            return 1
        sendkeys(m, ["q"])
        time.sleep(0.8)
        g4, b4 = dump(m, "fm-out-after-play")
        if g4 > 20000 or b4 < 10000:
            print("FAIL: PLAY did not return to FM")
            return 1
        print("ok PLAY still a TUI")

        mark = len(m.buf)
        sendkeys(m, ["q"])
        if not wait_prompt(m, mark):
            print("FAIL: q from FM did not reach the shell")
            return 1
        print("ok shell")
    finally:
        m.close()
        os.unlink(image)

    exc = cpu_exceptions(intlog)
    if exc:
        print("FAIL exceptions", exc[0])
        return 1
    print("ok no exceptions")
    print("RESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
