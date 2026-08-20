#!/usr/bin/env python3
"""Boot FOS in QEMU and check that shell commands and .COM programs return.

A program that overruns its stack used to take the kernel down with it, so the
pass criteria are: every command comes back to the prompt, and the CPU never
takes an exception along the way.

    python3 scripts/smoke.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_console import Machine, cpu_exceptions, image_copy  # noqa: E402

# (command, timeout, interrupt_after, expected substring of the output)
CASES = [
    ("ver", 10, None, "FOS"),
    ("dir", 10, None, "SYSTEM.INI"),
    ("dir FOS", 10, None, "PLAY.COM"),
    ("mem", 15, None, "stack top"),
    ("date", 10, None, None),
    ("echo hello", 10, None, "hello"),
    ("beep 440 100", 15, None, None),
    ("play DEMO.WAV", 25, None, "DEMO.WAV"),
    ("play DEMO.MP3", 25, None, "DEMO.MP3"),
    ("play BABY.MP3", 25, 4.0, "BABY.MP3"),
    ("dir FOS", 10, None, "PLAY.COM"),  # shell still alive after the audio runs
]


def main():
    image = image_copy()
    intlog = "/tmp/fos-smoke-int.log"
    if os.path.exists(intlog):
        os.unlink(intlog)
    failures = []
    m = Machine(image, intlog=intlog)
    try:
        if not m.wait_boot():
            print("FAIL: never reached the shell prompt")
            return 1
        print("boot: reached prompt")
        for cmd, timeout, interrupt, expect in CASES:
            mark = len(m.buf)
            dt = m.run(cmd, timeout, interrupt_after=interrupt)
            out = m.buf[mark:].decode("latin1", "replace")
            if dt is None:
                print(f"FAIL {cmd:16s} no prompt within {timeout}s")
                failures.append(cmd)
            elif expect and expect not in out:
                print(f"FAIL {cmd:16s} output missing {expect!r}")
                failures.append(cmd)
            else:
                note = " (quit early)" if interrupt else ""
                print(f"ok   {cmd:16s} {dt:5.1f}s{note}")
    finally:
        m.close()
        os.unlink(image)

    exc = cpu_exceptions(intlog)
    if exc:
        print(f"FAIL: {len(exc)} CPU exception(s); first:\n  {exc[0]}")
        failures.append("cpu-exception")
    else:
        print("ok   no CPU exceptions")

    print("RESULT:", "PASS" if not failures else f"FAIL {failures}")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
