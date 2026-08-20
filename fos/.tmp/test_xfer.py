#!/usr/bin/env python3
"""Copy/move progress window: grey desktop, blue box, filenames, status bar."""

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


def dump(m, name, settle=0.35):
    ppm = os.path.join(OUT, name + ".ppm")
    png = os.path.join(OUT, name + ".png")
    m.monitor_cmd(f"screendump {ppm}", settle=settle)
    w, h, data = ppm_to_png(ppm, png)
    grey = count_color(data, GREY)
    blue = count_color(data, BLUE)
    print(f"{name}: {w}x{h} grey={grey} dark-blue={blue}")
    return grey, blue


def wait_prompt(m, mark, timeout=12):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m.pump()
        out = m.buf[mark:].decode("latin1", "replace").rstrip()
        if out.endswith(">") and ":\\" in out.splitlines()[-1]:
            return out
        time.sleep(0.05)
    return None


def looks_like_xfer(grey, blue):
    return grey > 50000 and blue > 5000


def main():
    image = image_copy()
    intlog = os.path.join(OUT, "int-xfer.log")
    if os.path.exists(intlog):
        os.unlink(intlog)
    m = Machine(image, intlog=intlog)
    try:
        if not m.wait_boot():
            print("FAIL: no boot")
            return 1
        print("boot ok")

        mark = len(m.buf)
        m.send("copy README.TXT COPY.TXT\r")
        time.sleep(0.22)
        g, b = dump(m, "xfer-shell-copy")
        out = wait_prompt(m, mark)
        if not out:
            print("FAIL: copy never returned to the prompt")
            return 1
        if not looks_like_xfer(g, b):
            print("FAIL: shell copy did not show grey+blue progress window")
            return 1
        if "1 file(s) copied" not in out and "copied" not in out.lower():
            print("FAIL: copy confirmation missing")
            print(out[-400:])
            return 1
        print("ok shell copy window")

        mark = len(m.buf)
        m.send("dir\r")
        out = wait_prompt(m, mark)
        if not out or "COPY.TXT" not in out:
            print("FAIL: COPY.TXT not on disk")
            print(out[-400:] if out else "")
            return 1

        mark = len(m.buf)
        m.send("move COPY.TXT MOVED.TXT\r")
        time.sleep(0.22)
        g, b = dump(m, "xfer-shell-move")
        out = wait_prompt(m, mark)
        if not out:
            print("FAIL: move never returned")
            return 1
        if not looks_like_xfer(g, b):
            print("FAIL: shell move did not show grey+blue progress window")
            return 1
        print("ok shell move window")

        m.send("fm\r")
        time.sleep(1.4)
        dump(m, "xfer-fm-browse", settle=1.0)
        sendkeys(m, ["down", "down"])  # DEMO.MP3
        time.sleep(0.2)
        sendkeys(m, ["c"])
        time.sleep(0.3)
        sendkeys(m, ["up"])  # FOS
        time.sleep(0.2)
        sendkeys(m, ["ret"])
        time.sleep(0.4)
        sendkeys(m, ["s"])
        time.sleep(0.4)
        sendkeys(m, ["ret"])  # confirm name, starts copy
        time.sleep(0.22)
        g, b = dump(m, "xfer-fm-copy")
        if not looks_like_xfer(g, b):
            print("FAIL: FM copy did not show grey+blue progress window")
            return 1
        time.sleep(1.0)
        g2, b2 = dump(m, "xfer-fm-after", settle=1.0)
        if g2 > 20000:
            print("FAIL: FM did not redraw after copy")
            return 1
        print("ok FM copy window")

        mark = len(m.buf)
        sendkeys(m, ["q"])
        if not wait_prompt(m, mark, timeout=8):
            print("FAIL: q from FM did not reach the shell")
            print(m.buf[mark:].decode("latin1", "replace")[-400:])
            return 1
        mark = len(m.buf)
        m.send("dir\r")
        out = wait_prompt(m, mark)
        if not out or "DEMO.MP3" not in out:
            print("FAIL: FM copy into FOS missing DEMO.MP3")
            print(out[-400:] if out else "")
            return 1
        print("ok FM copy landed in FOS")
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
