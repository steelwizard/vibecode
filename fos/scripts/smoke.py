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
    ("pwd", 10, None, None, "0:\\\r\n"),
    ("which echo", 10, None, None, "0:\\FOS\\ECHO.COM"),
    ("which cd", 10, None, None, "shell builtin"),
    ("which demo", 10, None, None, "DEMO.BAT"),
    ("dir FOS", 10, None, None, "GREP.COM"),
    ("mem", 15, None, None, "image max"),
    ("mem test", 30, None, None, "RESULT: PASS"),
    ("mem leak", 15, None, None, None),
    ("mem", 15, None, None, "0 B across 0 block(s)"),  # leak reclaimed on exit
    ("date", 10, None, None, None),
    ("echo hello", 10, None, None, "hello"),
    ("i=4", 10, None, None, None),
    ("i=i+1", 10, None, None, None),
    ("echo n$i", 10, None, None, "n5"),
    ("i++", 10, None, None, None),
    ("echo n$i", 10, None, None, "n6"),
    ("i+=4", 10, None, None, None),
    ("echo n$i", 10, None, None, "n10"),
    ("i--", 10, None, None, None),
    ("echo n$i", 10, None, None, "n9"),
    ("++i", 10, None, None, None),
    ("echo n$i", 10, None, None, "n10"),
    ("echo $(i+1)", 10, None, None, "11"),
    ("k=hello", 10, None, None, None),
    ("echo z$k", 10, None, None, "zhello"),
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
    ("i=3", 10, None, None, None),
    ("y=10", 10, None, None, None),
    ("if i < y then echo LT", 10, None, None, "\nLT"),
    ("if i > y then echo GT else echo NG", 10, None, None, "\nNG"),
    ("if i <= 3 then echo LEOK", 10, None, None, "\nLEOK"),
    ("if i != y then echo NEOK", 10, None, None, "\nNEOK"),
    ("if i+1 < y then echo PLUS", 10, None, None, "\nPLUS"),
    ("i=0", 10, None, None, None),
    ("while i < 3 do i=i+1", 10, None, None, None),
    ("echo WI$i", 10, None, None, "WI3"),
    ("for i = 1 to 3 do echo NUM$i", 10, None, None, "NUM3"),
    ("while false do echo LOOPED", 10, None, None, None),
    ("false", 10, None, None, None),
    ("if errorlevel 1 then echo ELFAIL else echo ELOK", 10, None, None, "\nELFAIL"),
    ("demo", 15, None, None, "Hello from DEMO.BAT"),
    ("call demo.bat world", 15, None, None, "arg1=world"),
    ("beep 440 100", 15, None, None, None),
    ("edit README.TXT", 20, 3.0, "\x18", None),  # Ctrl+X exits
    ("less README.TXT", 20, 2.0, "q", None),
    ("type 1:\\LOREM.TXT", 10, None, None, "Lorem ipsum"),
    ("grep Lorem 1:\\LOREM.TXT", 10, None, None, "Lorem ipsum"),
    ("less 1:\\LOREM.TXT", 20, 3.0, "q", None),
    ("edit 1:\\IPSUM.TXT", 20, 3.0, "\x18", None),
    ("1:", 10, None, None, None),
    ("less LOREM.TXT", 20, 3.0, "q", None),
    ("0:", 10, None, None, None),
    ("play DEMO.WAV", 25, 2.0, "q", "DEMO.WAV"),
    ("play DEMO.MP3", 25, 2.0, "q", "DEMO.MP3"),
    ("play DEMO.MID", 25, 3.0, "q", "DEMO.MID"),
    ("dir MIDI", 10, None, None, "PREL1.MID"),
    ("cd MIDI", 10, None, None, None),
    ("pwd", 10, None, None, "0:\\MIDI\r\n"),
    ("dir", 10, None, None, "PREL1.MID"),
    ("cd ..", 10, None, None, None),
    ("play MIDI\\INV4.MID", 25, 3.0, "q", "INV4.MID"),
    ("for i = 1 to 500 do echo $i", 40, None, None, "500"),
    ("bench primes", 30, None, None, "RESULT: PASS"),
    ("bench mem", 40, None, None, "RESULT: PASS"),
    ("bench hw", 20, 2.0, "q", None),
    ("dir FOS", 10, None, None, "BENCH.COM"),  # shell alive after bench
    ("which tetris", 10, None, None, "0:\\GAMES\\TETRIS.COM"),
    ("tetris", 20, 2.0, "q", None),
    ("dir GAMES", 10, None, None, "TETRIS.COM"),
]


def main():
    image = image_copy()
    data = image_copy("data.img") if os.path.exists("data.img") else None
    intlog = "/tmp/fos-smoke-int.log"
    if os.path.exists(intlog):
        os.unlink(intlog)
    failures = []
    m = Machine(image, intlog=intlog, data=data)
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
        if data:
            os.unlink(data)

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
