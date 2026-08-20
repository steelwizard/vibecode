#!/usr/bin/env python3
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


def count_color(data, w, rgb, tol=8):
    r0, g0, b0 = rgb
    n = 0
    samples = []
    for i in range(0, len(data), 3):
        r, g, b = data[i], data[i + 1], data[i + 2]
        if abs(r - r0) <= tol and abs(g - g0) <= tol and abs(b - b0) <= tol:
            n += 1
            if len(samples) < 5:
                p = i // 3
                samples.append((p % w, p // w))
    return n, samples


def sendkeys(m, keys, settle=0.12):
    for k in keys:
        m.monitor_cmd(f"sendkey {k}", settle=settle)


def dump(m, name):
    ppm = os.path.join(OUT, name + ".ppm")
    png = os.path.join(OUT, name + ".png")
    m.monitor_cmd(f"screendump {ppm}", settle=1.2)
    if not os.path.exists(ppm):
        print(f"FAIL: no dump {ppm}")
        return None
    w, h, data = ppm_to_png(ppm, png)
    blue = count_color(data, w, (0x55, 0x55, 0xFF))
    dark = count_color(data, w, (0x00, 0x00, 0xAA))
    print(f"{name}: {w}x{h} light-blue={blue[0]}@{blue[1]} dark-blue={dark[0]}")
    return png


def main():
    image = image_copy()
    intlog = os.path.join(OUT, "int.log")
    if os.path.exists(intlog):
        os.unlink(intlog)
    m = Machine(image, intlog=intlog)
    try:
        if not m.wait_boot():
            print("FAIL: no boot")
            print(m.buf.decode("latin1", "replace")[-500:])
            return 1
        print("boot ok")
        dump(m, "shell")

        m.send("fm\r")
        time.sleep(1.5)
        dump(m, "fm-browse")

        sendkeys(m, ["down", "down"])  # DEMO.MP3
        time.sleep(0.3)
        sendkeys(m, ["c"])
        time.sleep(0.4)
        dump(m, "fm-pick")
        sendkeys(m, ["up"])  # FOS
        time.sleep(0.2)
        sendkeys(m, ["ret"])  # enter FOS
        time.sleep(0.5)
        dump(m, "fm-dest")
        sendkeys(m, ["s"])
        time.sleep(0.5)
        dump(m, "fm-prompt")
        sendkeys(m, ["left", "left", "left", "left"])
        time.sleep(0.3)
        dump(m, "fm-prompt-left")
        sendkeys(m, ["ret"])
        time.sleep(0.8)
        dump(m, "fm-after-copy")
        sendkeys(m, ["q"])
        time.sleep(1.0)
        mark = len(m.buf)
        m.send("dir\r")
        deadline = time.time() + 8
        while time.time() < deadline:
            m.pump()
            if m.buf[mark:].decode("latin1", "replace").rstrip().endswith("0:\\>"):
                break
            time.sleep(0.1)
        out = m.buf[mark:].decode("latin1", "replace")
        print("--- dir ---")
        print(out)
        if "DEMO.MP3" not in out:
            print("FAIL: DEMO.MP3 not copied into FOS")
            return 1
        print("ok copy into folder")
    finally:
        m.close()
        os.unlink(image)

    exc = cpu_exceptions(intlog)
    if exc:
        print("FAIL exceptions", exc[0])
        return 1
    print("ok no exceptions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
