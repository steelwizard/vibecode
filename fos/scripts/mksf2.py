#!/usr/bin/env python3
"""Build a tiny General MIDI SoundFont (CC0) when TimGM6mb is not installed."""
import math
import struct
import sys

RATE = 22050
LOOP = 256
PAD = 46

GEN_ATTACK = 34
GEN_DECAY = 36
GEN_SUSTAIN = 37
GEN_RELEASE = 38
GEN_INSTRUMENT = 41
GEN_SAMPLE_ID = 53
GEN_SAMPLE_MODES = 54
GEN_ROOT_KEY = 58


def pad_name(s, n=20):
    b = s.encode("ascii")[: n - 1]
    return b + b"\x00" * (n - len(b))


def riff_chunk(tag, data):
    pad = b"\x00" if len(data) & 1 else b""
    return tag + struct.pack("<I", len(data)) + data + pad


def list_chunk(list_id, data):
    body = list_id + data
    pad = b"\x00" if len(body) & 1 else b""
    return b"LIST" + struct.pack("<I", len(body)) + body + pad


def wave(kind, n=LOOP):
    out = []
    for i in range(n):
        t = i / float(n)
        if kind == "saw":
            v = 2.0 * t - 1.0
        elif kind == "sine":
            v = math.sin(2 * math.pi * t)
        elif kind == "square":
            v = 1.0 if t < 0.5 else -1.0
        elif kind == "noise":
            x = (i * 1103515245 + 12345) & 0x7FFFFFFF
            v = (x / float(0x40000000)) - 1.0
        else:
            v = math.sin(2 * math.pi * t * 4) * math.exp(-t * 8)
        s = int(max(-1.0, min(1.0, v)) * 20000)
        out.append(struct.pack("<h", s))
    return b"".join(out)


def phdr(name, preset, bank, bag):
    return pad_name(name) + struct.pack("<HHHIII", preset, bank, bag, 0, 0, 0)


def bag(gen_ndx, mod_ndx=0):
    return struct.pack("<HH", gen_ndx, mod_ndx)


def gen(oper, amount):
    return struct.pack("<Hh", oper, amount)


def inst(name, bag_ndx):
    return pad_name(name) + struct.pack("<H", bag_ndx)


def shdr(name, start, end, loop0, loop1, root=60):
    return (
        pad_name(name)
        + struct.pack("<IIIII", start, end, loop0, loop1, RATE)
        + struct.pack("<BbHH", root, 0, 0, 1)
    )


def inst_for_program(p):
    if p < 8 or 16 <= p < 32 or 72 <= p < 80 or 88 <= p < 96 or 104 <= p < 112:
        return 1
    if 56 <= p < 80:
        return 2
    if 96 <= p < 104 or 112 <= p < 128:
        return 3
    return 0


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "GM.SF2"
    kinds = ["saw", "sine", "square", "noise", "click"]
    smpl = bytearray()
    headers = []
    off = 0
    for k in kinds:
        pcm = wave(k)
        start = off
        smpl.extend(pcm)
        end = off + LOOP
        loop0, loop1 = (start, start) if k == "click" else (start, end)
        headers.append((k, start, end, loop0, loop1))
        smpl.extend(b"\x00\x00" * PAD)
        off = end + PAD

    envelopes = [
        (-8000, 1500, 0, -1000, 1),
        (-6000, 4000, 3000, 0, 1),
        (-8000, 2000, 1000, -500, 1),
        (-12000, 0, 0, -2000, 1),
        (-12000, -3000, 1440, -8000, 0),
    ]
    igen = bytearray()
    ibag = bytearray()
    insts = bytearray()
    for i, env in enumerate(envelopes):
        insts += inst(kinds[i], i)
        ibag += bag(len(igen) // 4)
        atk, dec, sus, rel, mode = env
        igen += gen(GEN_ATTACK, atk)
        igen += gen(GEN_DECAY, dec)
        igen += gen(GEN_SUSTAIN, sus)
        igen += gen(GEN_RELEASE, rel)
        igen += gen(GEN_SAMPLE_MODES, mode)
        igen += gen(GEN_ROOT_KEY, 60)
        igen += gen(GEN_SAMPLE_ID, i)
    insts += inst("EOI", 5)
    ibag += bag(len(igen) // 4)
    igen += gen(0, 0)
    imod = b"\x00" * 10

    pgen = bytearray()
    pbag = bytearray()
    phdrs = bytearray()
    for p in range(128):
        phdrs += phdr("prg%03d" % p, p, 0, p)
        pbag += bag(len(pgen) // 4)
        pgen += gen(GEN_INSTRUMENT, inst_for_program(p))
    phdrs += phdr("Standard Kit", 0, 128, 128)
    pbag += bag(len(pgen) // 4)
    pgen += gen(GEN_INSTRUMENT, 4)
    phdrs += phdr("EOP", 0, 0, 129)
    pbag += bag(len(pgen) // 4)
    pgen += gen(0, 0)
    pmod = b"\x00" * 10

    shdrs = bytearray()
    for k, start, end, loop0, loop1 in headers:
        shdrs += shdr(k, start, end, loop0, loop1)
    shdrs += shdr("EOS", 0, 0, 0, 0)

    pdta = (
        riff_chunk(b"phdr", phdrs)
        + riff_chunk(b"pbag", pbag)
        + riff_chunk(b"pmod", pmod)
        + riff_chunk(b"pgen", pgen)
        + riff_chunk(b"inst", insts)
        + riff_chunk(b"ibag", ibag)
        + riff_chunk(b"imod", imod)
        + riff_chunk(b"igen", igen)
        + riff_chunk(b"shdr", shdrs)
    )
    info = (
        riff_chunk(b"ifil", struct.pack("<HH", 2, 1))
        + riff_chunk(b"isng", b"EMU8000\x00")
        + riff_chunk(b"INAM", b"FOS GM\x00")
        + riff_chunk(b"ICMT", b"CC0 wavetable GM for FOS\x00")
    )
    inner = (
        b"sfbk"
        + list_chunk(b"INFO", info)
        + list_chunk(b"sdta", riff_chunk(b"smpl", bytes(smpl)))
        + list_chunk(b"pdta", pdta)
    )
    riff = b"RIFF" + struct.pack("<I", len(inner)) + inner
    with open(path, "wb") as f:
        f.write(riff)
    print("wrote %s (%d bytes)" % (path, len(riff)), file=sys.stderr)


if __name__ == "__main__":
    main()
