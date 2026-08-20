#!/usr/bin/env python3
"""Enter on a .COM in FM should run it and return to FM."""

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


def dump(m, name):
    ppm = os.path.join(OUT, name + ".ppm")
    png = os.path.join(OUT, name + ".png")
    m.monitor_cmd(f"screendump {ppm}", settle=1.2)
    w, h, data = ppm_to_png(ppm, png)
    dark = count_color(data, (0x00, 0x00, 0xAA))
    cyan = count_color(data, (0x55, 0xFF, 0xFF), tol=20)
    purple = count_color(data, (0xAA, 0x00, 0xAA), tol=20)
    print(f"{name}: {w}x{h} dark-blue={dark} cyan={cyan} purple={purple}")
    return dark, cyan, purple


def wait_prompt(m, mark, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m.pump()
        out = m.buf[mark:].decode("latin1", "replace").rstrip()
        if out.endswith(">") and ":\\" in out.splitlines()[-1]:
            return True
        time.sleep(0.1)
    return False


def main():
    image = image_copy()
    intlog = os.path.join(OUT, "int-fm-run.log")
    if os.path.exists(intlog):
        os.unlink(intlog)
    m = Machine(image, intlog=intlog)
    try:
        if not m.wait_boot():
            print("FAIL: no boot")
            print(m.buf.decode("latin1", "replace")[-500:])
            return 1
        print("boot ok")

        m.send("fm\r")
        time.sleep(1.5)
        d0, c0, _ = dump(m, "fm-run-browse")
        if d0 < 10000:
            print("FAIL: FM browse is not the dark-blue TUI")
            return 1

        sendkeys(m, ["down", "ret"])  # FOS
        time.sleep(0.6)
        dump(m, "fm-run-fos")

        sendkeys(m, ["down", "down", "ret"])  # DATE.COM
        time.sleep(1.2)
        d1, c1, _ = dump(m, "fm-run-after-date")
        if d1 < 10000 or c1 < 500:
            print("FAIL: did not return to FM after DATE.COM")
            return 1

        mark = len(m.buf)
        sendkeys(m, ["end", "ret"])  # PLAY.COM
        time.sleep(1.0)
        dump(m, "fm-run-play")
        sendkeys(m, ["q"])  # dismiss play usage
        time.sleep(1.0)
        d2, c2, _ = dump(m, "fm-run-after-play")
        if wait_prompt(m, mark, timeout=1.5):
            print("FAIL: leftover q quit FM after PLAY.COM")
            print(m.buf[mark:].decode("latin1", "replace")[-300:])
            return 1
        if d2 < 10000 or c2 < 500:
            print("FAIL: did not return to FM after PLAY.COM")
            return 1

        mark = len(m.buf)
        sendkeys(m, ["q"])
        if not wait_prompt(m, mark, timeout=8):
            print("FAIL: q from FM did not reach the shell")
            print(m.buf[mark:].decode("latin1", "replace")[-500:])
            return 1
        print("ok returned to shell")
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
