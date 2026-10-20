#!/usr/bin/env python3
"""Convert a raw RGB565 (little-endian) framebuffer dump to PNG, with optional integer upscale.
Usage: rgb565_to_png.py fb.raw out.png [width=135] [height=240] [scale=3]"""
import struct, sys, zlib

raw, out = sys.argv[1], sys.argv[2]
W = int(sys.argv[3]) if len(sys.argv) > 3 else 135
H = int(sys.argv[4]) if len(sys.argv) > 4 else 240
S = int(sys.argv[5]) if len(sys.argv) > 5 else 3
data = open(raw, "rb").read()
rows = []
for y in range(H):
    row = bytearray()
    for x in range(W):
        (v,) = struct.unpack_from("<H", data, (y * W + x) * 2)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        px = bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31))
        row += px * S
    rows += [b"\x00" + bytes(row)] * S

def chunk(t, d):
    c = struct.pack(">I", len(d)) + t + d
    return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W * S, H * S, 8, 2, 0, 0, 0)) \
    + chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b"")
open(out, "wb").write(png)
print("wrote", out, W * S, "x", H * S)
