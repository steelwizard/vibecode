#!/usr/bin/env python3
"""Tap the SB16 mixer while play.com runs and check the audio it produced.

Continuity: silent stretches in the middle of a stream mean the DMA ran dry
between chunks, which is what glitching sounds like.
Fidelity: DEMO.MP3 is a synthesized 440 Hz sine, so its captured spectrum
should peak at 440 Hz with negligible harmonics.

    python3 scripts/audio_check.py            # DEMO.MP3 (tone)
    python3 scripts/audio_check.py FILE.MP3 10
"""

import math
import os
import struct
import sys
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_console import Machine, image_copy  # noqa: E402

WINDOW_MS = 10


def capture(image, target, seconds):
    path = os.path.join("/tmp", f"fos-capture-{target.replace('.', '-')}.wav")
    if os.path.exists(path):
        os.unlink(path)
    m = Machine(image)
    try:
        if not m.wait_boot():
            raise RuntimeError("no shell prompt")
        m.monitor_cmd(f"wavcapture {path} snd0 22050 16 1")
        m.run(f"play {target}", seconds, interrupt_after=seconds - 1.0)
        m.monitor_cmd("stopcapture 0")
    finally:
        m.close()
    return path


def read_mono(path):
    with wave.open(path, "rb") as w:
        rate, width, ch = w.getframerate(), w.getsampwidth(), w.getnchannels()
        raw = w.readframes(w.getnframes())
    if width != 2:
        raise RuntimeError(f"unexpected sample width {width}")
    samples = struct.unpack("<%dh" % (len(raw) // 2), raw)
    if ch == 2:
        samples = samples[::2]
    return rate, list(samples)


def envelope(rate, samples):
    win = max(1, rate * WINDOW_MS // 1000)
    out = []
    for i in range(0, len(samples) - win, win):
        acc = 0
        for v in samples[i:i + win]:
            acc += v * v
        out.append((acc / win) ** 0.5)
    return out


def check_continuity(rate, samples):
    rms = envelope(rate, samples)
    if not rms or max(rms) == 0:
        print("  FAIL: no audio captured")
        return False
    peak = max(rms)
    thresh = max(peak * 0.02, 30.0)
    loud = [i for i, r in enumerate(rms) if r > thresh]
    if not loud:
        print(f"  FAIL: stream is silent (peak rms {peak:.0f})")
        return False
    first, last = loud[0], loud[-1]
    longest = run = gaps = 0
    for i in range(first, last + 1):
        if rms[i] <= thresh:
            run += 1
            longest = max(longest, run)
        else:
            if run * WINDOW_MS >= 50:
                gaps += 1
            run = 0
    span = (last - first + 1) * WINDOW_MS
    print(f"  audio span {span} ms, peak rms {peak:.0f}, "
          f"gaps >=50 ms: {gaps}, longest silence {longest * WINDOW_MS} ms")
    ok = gaps == 0 and longest * WINDOW_MS <= 60
    print("  continuity:", "ok" if ok else "FAIL (dropouts)")
    return ok


def goertzel(x, freq, rate):
    coeff = 2 * math.cos(2 * math.pi * freq / rate)
    s1 = s2 = 0.0
    for v in x:
        s0 = v + coeff * s1 - s2
        s2, s1 = s1, s0
    return math.sqrt(max(s1 * s1 + s2 * s2 - coeff * s1 * s2, 0.0)) / len(x)


def check_tone(rate, samples, expect=440.0):
    mid = len(samples) // 2
    half = min(rate // 2, mid)
    seg = samples[mid - half:mid + half]
    if not seg:
        print("  FAIL: nothing to analyse")
        return False
    mean = sum(seg) / len(seg)
    seg = [v - mean for v in seg]
    best_amp, best_f = 0.0, 0
    for f in range(int(expect * 0.8), int(expect * 1.25)):
        amp = goertzel(seg, float(f), rate)
        if amp > best_amp:
            best_amp, best_f = amp, f
    h2 = goertzel(seg, expect * 2, rate)
    print(f"  spectral peak {best_f} Hz (expected {expect:.0f}), "
          f"2nd harmonic {h2 / best_amp if best_amp else 0:.4f} of peak")
    ok = abs(best_f - expect) <= 2 and h2 < best_amp * 0.05
    print("  fidelity:", "ok" if ok else "FAIL (wrong pitch or distortion)")
    return ok


def main():
    if len(sys.argv) > 1:
        jobs = [(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 10.0,
                 False)]
    else:
        jobs = [("DEMO.MP3", 5.0, True)]

    image = image_copy()
    ok = True
    try:
        for target, seconds, tone in jobs:
            print(f"{target}:")
            path = capture(image, target, seconds)
            rate, samples = read_mono(path)
            print(f"  captured {len(samples) / rate:.2f}s @ {rate} Hz")
            ok &= check_continuity(rate, samples)
            if tone:
                ok &= check_tone(rate, samples)
    finally:
        os.unlink(image)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
