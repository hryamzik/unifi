#!/usr/bin/env python3
"""Turn a raw panel PPM (from a LCD_MOCK build) into a PNG as the viewer sees it.

  preview.py in.ppm out.png [--rotate 180|0] [--scale 3] [--no-bezel]

Rotates into the viewing orientation and, unless --no-bezel, greys out what the bezel hides
(measured window, see docs/decisions.md): raw x 1..130, y 3..237 visible, corners rounded r=31.
"""
import struct, sys, zlib

args = sys.argv[1:]
src, dst = args[0], args[1]
rot = int(args[args.index("--rotate") + 1]) if "--rotate" in args else 180
scale = int(args[args.index("--scale") + 1]) if "--scale" in args else 3
bezel = "--no-bezel" not in args

d = open(src, "rb").read()
parts = d.split(b"\n", 3)
w, h = map(int, parts[1].split())
pix = parts[3]
img = [[tuple(pix[(y * w + x) * 3:(y * w + x) * 3 + 3]) for x in range(w)] for y in range(h)]

VX0, VX1, VY0, VY1, R = 1, 130, 3, 237, 31   # corner radius measured with tools/measure-bezel.py


def visible(x, y):
    if not (VX0 <= x <= VX1 and VY0 <= y <= VY1):
        return False
    for cx, cy in ((VX0 + R, VY0 + R), (VX1 - R, VY0 + R), (VX0 + R, VY1 - R), (VX1 - R, VY1 - R)):
        if (x < VX0 + R or x > VX1 - R) and (y < VY0 + R or y > VY1 - R):
            if (x - cx) ** 2 + (y - cy) ** 2 > R * R and ((x < VX0 + R) == (cx == VX0 + R)) and ((y < VY0 + R) == (cy == VY0 + R)):
                return False
    return True


if bezel:
    for y in range(h):
        for x in range(w):
            if not visible(x, y):
                r, g, b = img[y][x]
                img[y][x] = (90 + r // 8, 20 + g // 8, 20 + b // 8) if (r or g or b) else (70, 70, 78)
if rot == 180:
    img = [row[::-1] for row in img[::-1]]

rows = []
for row in img:
    line = b"".join(bytes(p) * scale for p in row)
    rows += [b"\x00" + line] * scale
W, H = w * scale, h * scale


def chunk(t, c):
    return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)


open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
                      + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
print(f"wrote {dst} {W}x{H}")
