#!/usr/bin/env python3
"""Shared helpers for driving FOS in QEMU over the serial console.

Tests run against a copy of the disk image so they never fight an interactive
QEMU session for the image write lock.
"""

import os
import shutil
import socket
import subprocess
import tempfile
import time

PROMPT = "0:\\>"


def image_copy(src="boot.img"):
    fd, path = tempfile.mkstemp(prefix="fos-test-", suffix=".img")
    os.close(fd)
    shutil.copyfile(src, path)
    return path


class Machine:
    """A booted FOS instance with serial and monitor sockets."""

    def __init__(self, image, intlog=None, audiodev="none"):
        self.dir = tempfile.mkdtemp(prefix="fos-qemu-")
        self.serial_path = os.path.join(self.dir, "serial.sock")
        self.monitor_path = os.path.join(self.dir, "monitor.sock")
        self.intlog = intlog
        cmd = [
            "qemu-system-x86_64",
            "-drive", f"format=raw,file={image},index=0,media=disk",
            "-device", "bochs-display",
            "-audiodev", f"{audiodev},id=snd0",
            "-device", "sb16,audiodev=snd0,iobase=0x220,irq=5,dma=1",
            "-m", "512M", "-display", "none",
            "-serial", f"unix:{self.serial_path},server=on,wait=off",
            "-monitor", f"unix:{self.monitor_path},server=on,wait=off",
            "-no-reboot",
        ]
        if intlog:
            cmd += ["-d", "int,guest_errors", "-D", intlog]
        self.proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL,
                                    stderr=subprocess.STDOUT)
        self.serial = self._connect(self.serial_path)
        self.monitor = self._connect(self.monitor_path)
        self.buf = bytearray()

    def _connect(self, path, timeout=20.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("qemu exited during startup")
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(path)
                s.settimeout(0.2)
                return s
            except (FileNotFoundError, ConnectionRefusedError, OSError):
                time.sleep(0.05)
        raise RuntimeError(f"qemu never created {path}")

    def pump(self):
        try:
            data = self.serial.recv(65536)
            if data:
                self.buf += data
        except socket.timeout:
            pass
        return self.buf.decode("latin1", "replace")

    def wait_boot(self, timeout=30.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if PROMPT in self.pump():
                return True
        return False

    def send(self, text):
        self.serial.sendall(text.encode())

    def run(self, cmd, timeout, interrupt_after=None, key="q"):
        """Send a command; return seconds until the prompt came back, or None."""
        self.pump()
        mark = len(self.buf)
        start = time.time()
        self.send(cmd + "\r")
        sent_key = interrupt_after is None
        deadline = start + timeout
        while time.time() < deadline:
            if not sent_key and time.time() - start >= interrupt_after:
                self.send(key)
                sent_key = True
            self.pump()
            out = self.buf[mark:].decode("latin1", "replace")
            if out.rstrip().endswith(PROMPT):
                return time.time() - start
        return None

    def monitor_cmd(self, cmd, settle=0.4):
        self.monitor.sendall(cmd.encode() + b"\n")
        time.sleep(settle)
        out = b""
        try:
            while True:
                data = self.monitor.recv(65536)
                if not data:
                    break
                out += data
        except socket.timeout:
            pass
        return out.decode("latin1", "replace")

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=10)
        except Exception:
            self.proc.kill()
        shutil.rmtree(self.dir, ignore_errors=True)


def cpu_exceptions(intlog):
    """Exception vectors (< 0x20) recorded by -d int, i.e. faults, not IRQs."""
    if not intlog or not os.path.exists(intlog):
        return []
    hits = []
    with open(intlog, errors="replace") as f:
        for line in f:
            marker = line.find(" v=")
            if marker < 0 or "IP=" not in line:
                continue
            try:
                vec = int(line[marker + 3:marker + 5], 16)
            except ValueError:
                continue
            if vec < 0x20:
                hits.append(line.strip())
    return hits
