#!/usr/bin/env python3
"""Boot boot.img in QEMU headlessly, screendump it via the monitor, and exit.

Handy for checking a video mode or a full-screen app without a display.

Usage: shot.py OUT.{png,ppm} [--wait SECONDS] [--keys KEY[,KEY...]]

Keys are QEMU 'sendkey' names, sent after the wait so the shot can capture an
app (e.g. --keys l,e,s,s,spc,r,e,a,d,m,e,dot,t,x,t,ret). Note the guest keyboard
layout applies: with layout=de, 'y' and 'z' swap.
"""

import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def ppm_to_png(src, dst):
    with open(src, "rb") as fh:
        assert fh.readline().strip() == b"P6", "expected a P6 PPM"
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
    return w, h


def monitor_cmd(sock_path, cmd, settle=0.4):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    time.sleep(0.2)
    s.recv(65536)
    s.sendall(cmd.encode() + b"\n")
    time.sleep(settle)
    try:
        out = s.recv(65536).decode(errors="replace")
    except OSError:
        out = ""
    s.close()
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--wait", type=float, default=14.0)
    ap.add_argument("--keys", default="")
    ap.add_argument("--vgamem", default="32M")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp(dir=HERE, prefix=".shot-")
    boot = os.path.join(tmp, "boot.img")
    data = os.path.join(tmp, "data.img")
    mon = os.path.join(tmp, "mon.sock")
    serial = os.path.join(tmp, "serial.log")
    shot = os.path.join(tmp, "shot.ppm")

    shutil.copy(os.path.join(HERE, "boot.img"), boot)
    shutil.copy(os.path.join(HERE, "data.img"), data)

    qemu = [
        "qemu-system-x86_64",
        "-drive", f"format=raw,file={boot},index=0,media=disk",
        "-drive", f"format=raw,file={data},index=1,media=disk",
        "-device", f"bochs-display,vgamem={args.vgamem}",
        "-m", "512M",
        "-display", "none",
        "-serial", f"file:{serial}",
        "-monitor", f"unix:{mon},server,nowait",
    ]
    proc = subprocess.Popen(qemu, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    try:
        time.sleep(args.wait)
        if proc.poll() is not None:
            sys.stderr.write(proc.stderr.read().decode(errors="replace"))
            return 1

        if args.keys:
            for key in args.keys.split(","):
                monitor_cmd(mon, f"sendkey {key}", settle=0.15)
            time.sleep(2.5)

        monitor_cmd(mon, f"screendump {shot}", settle=2.0)
        monitor_cmd(mon, "quit", settle=0.3)

        time.sleep(0.5)
        if not os.path.exists(shot):
            print("no screenshot produced")
        elif args.out.endswith(".png"):
            w, h = ppm_to_png(shot, args.out)
            print(f"wrote {args.out} ({w}x{h})")
        else:
            shutil.copy(shot, args.out)
            print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes)")

        if os.path.exists(serial):
            with open(serial, errors="replace") as fh:
                print("--- serial ---")
                print(fh.read())
    finally:
        proc.kill()
        proc.wait()
        shutil.rmtree(tmp, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
