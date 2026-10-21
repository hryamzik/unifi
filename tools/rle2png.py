#!/usr/bin/env python3
"""Decode the run-length preview text of `utr-lcd-status -P` (see luci-preview) into a PNG, side by side.

  rle2png.py out.png [--scale 3] in1.rle [in2.rle ...]
"""
import struct, sys, zlib

args = sys.argv[1:]
out = args[0]
scale = 3
if "--scale" in args:
    i = args.index("--scale"); scale = int(args[i + 1]); del args[i:i + 2]
files = args[1:]


def dec(path):
    lines = open(path).read().split("\n")
    w, h = map(int, lines[0].split())
    px = []
    for t in lines[1].split():
        n, c = t.split(":")
        px += [bytes.fromhex(c)] * int(n)
    assert len(px) == w * h, (len(px), w * h)
    return w, h, px


imgs = [dec(f) for f in files]
W = (sum(i[0] for i in imgs) + 6 * (len(imgs) - 1)) * scale
H = imgs[0][1] * scale
rows = []
for y in range(imgs[0][1]):
    line = b""
    for k, (w, h, px) in enumerate(imgs):
        line += b"".join(px[y * w + x] * scale for x in range(w))
        if k < len(imgs) - 1:
            line += b"\x20\x20\x20" * 6 * scale
    rows += [b"\x00" + line] * scale


def ch(t, c):
    return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)


open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                      + ch(b"IDAT", zlib.compress(b"".join(rows))) + ch(b"IEND", b""))
print("wrote", out)
