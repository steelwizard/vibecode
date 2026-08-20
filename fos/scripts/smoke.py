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

# (command, timeout, seconds before sending key, key to send, expected output)
CASES = [
    ("ver", 10, None, None, "FOS"),
    ("dir", 10, None, None, "SYSTEM.INI"),
    ("dir FOS", 10, None, None, "GREP.COM"),
    ("mem", 15, None, None, "image max"),
    ("mem test", 30, None, None, "RESULT: PASS"),
    ("mem leak", 15, None, None, None),
    ("mem", 15, None, None, "0 B across 0 block(s)"),  # leak reclaimed on exit
    ("date", 10, None, None, None),
    ("echo hello", 10, None, None, "hello"),
    ("grep Flash README.TXT", 10, None, None, "Flash Operating System"),
    ("grep -n Flash README.TXT", 10, None, None, "1:# FOS"),
    ("grep -i operating README.TXT", 10, None, None, "Flash Operating"),
    ("type SYSTEM.INI | grep keyboard", 10, None, None, "[keyboard]"),
    ("echo $PATH", 10, None, None, "\\FOS"),
    ("echo $(1+5)", 10, None, None, "6"),
    ("echo $((2*3+1))", 10, None, None, "7"),
    ("env", 10, None, None, "PATH="),
    ("if exist SYSTEM.INI then echo IFYES", 10, None, None, "\nIFYES"),
    ("if exist nosuch.fil then echo IFYES else echo NOPEZ", 10, None, None, "\nNOPEZ"),
    ("for i = 1 to 3 do echo NUM$i", 10, None, None, "NUM3"),
    ("while false do echo LOOPED", 10, None, None, None),
    ("false", 10, None, None, None),
    ("if errorlevel 1 then echo ELFAIL else echo ELOK", 10, None, None, "\nELFAIL"),
    ("demo", 15, None, None, "Hello from DEMO.BAT"),
    ("call demo.bat world", 15, None, None, "arg1=world"),
    ("beep 440 100", 15, None, None, None),
    ("edit README.TXT", 20, 3.0, "\x18", None),  # Ctrl+X exits
    ("play DEMO.WAV", 25, 2.0, "q", "DEMO.WAV"),
    ("play DEMO.MP3", 25, 2.0, "q", "DEMO.MP3"),
    ("dir FOS", 10, None, None, "PLAY.COM"),  # shell alive after the audio runs
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
        for cmd, timeout, interrupt, key, expect in CASES:
            mark = len(m.buf)
            dt = m.run(cmd, timeout, interrupt_after=interrupt, key=key or "q")
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
