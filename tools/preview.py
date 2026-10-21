#!/usr/bin/env python3
"""Turn a raw panel PPM (from a LCD_MOCK build) into a PNG as the viewer sees it.

  preview.py in.ppm [in2.ppm ...] out.png [--rotate 180|0] [--scale 3] [--no-bezel]

Several inputs are placed side by side (one screen each).

Rotates into the viewing orientation and, unless --no-bezel, makes what the bezel hides transparent
(measured window, see docs/decisions.md): raw x 1..130, y 3..237 visible, corners rounded r=31.
"""
import struct, sys, zlib

args = sys.argv[1:]
pos = [a for i, a in enumerate(args) if not a.startswith("--") and (i == 0 or args[i - 1] not in ("--rotate", "--scale"))]
srcs, dst = pos[:-1], pos[-1]
rot = int(args[args.index("--rotate") + 1]) if "--rotate" in args else 180
scale = int(args[args.index("--scale") + 1]) if "--scale" in args else 3
bezel = "--no-bezel" not in args

VX0, VX1, VY0, VY1, R = 1, 130, 3, 237, 31   # corner radius measured with tools/measure-bezel.py


def visible(x, y):
    if not (VX0 <= x <= VX1 and VY0 <= y <= VY1):
        return False
    for cx, cy in ((VX0 + R, VY0 + R), (VX1 - R, VY0 + R), (VX0 + R, VY1 - R), (VX1 - R, VY1 - R)):
        if (x < VX0 + R or x > VX1 - R) and (y < VY0 + R or y > VY1 - R):
            if (x - cx) ** 2 + (y - cy) ** 2 > R * R and ((x < VX0 + R) == (cx == VX0 + R)) and ((y < VY0 + R) == (cy == VY0 + R)):
                return False
    return True


def load(src):
    d = open(src, "rb").read()
    parts = d.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    pix = parts[3]
    img = [[tuple(pix[(y * w + x) * 3:(y * w + x) * 3 + 3]) for x in range(w)] for y in range(h)]
    if bezel:
        for y in range(h):
            for x in range(w):
                if not visible(x, y):
                    img[y][x] = (0, 0, 0, 0)   # bezel-hidden: transparent, no frame
    img = [[p + (255,) if len(p) == 3 else p for p in row] for row in img]
    if rot == 180:
        img = [row[::-1] for row in img[::-1]]
    return w, h, img


shots = [load(s) for s in srcs]
w = sum(s[0] for s in shots) + 6 * (len(shots) - 1)
h = shots[0][1]
img = [[] for _ in range(h)]
for i, (sw, sh, simg) in enumerate(shots):
    for y in range(h):
        img[y] += ([(0, 0, 0, 0)] * 6 if i else []) + simg[y]

rows = []
for row in img:
    line = b"".join(bytes(p) * scale for p in row)
    rows += [b"\x00" + line] * scale
W, H = w * scale, h * scale


def chunk(t, c):
    return struct.pack(">I", len(c)) + t + c + struct.pack(">I", zlib.crc32(t + c) & 0xFFFFFFFF)


open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
                      + chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))
print(f"wrote {dst} {W}x{H}")
