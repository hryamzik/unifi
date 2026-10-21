#!/usr/bin/env python3
"""Measure the visible window and corner radius of the LCD from a photo of tools/extras/lcdfill.

  tools/lcd-shot.sh build/cam/fill.jpg build/extras/lcdfill -t 40
  tools/measure-bezel.py build/cam/fill.jpg [x0 y0 x1 y1]     # crop box around the panel, photo pixels

The photo must be upright (lcd-shot.sh rotates it), taken from the front; the lit window is found by
its blue-white colour, and the 10 px grid in the pattern gives the photo-to-panel scale. Output is in
viewer (mounted) panel coordinates. Two diagonal corners usually fit a circle well; the other two are
disturbed by glare and a slightly tilted camera, so trust the pair with the small rms.
Needs numpy and Pillow.
"""
import sys
import numpy as np
from PIL import Image
im = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(int)
x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6]) if len(sys.argv) >= 6 else (940, 500, 1260, 1000)
c = im[y0:y1, x0:x1]
r, g, b = c[..., 0], c[..., 1], c[..., 2]
lum = (r + g + b) / 3
print("crop", c.shape, "lum range", lum.min(), lum.max())
# lit panel is bluish-white; plastic is grey/pink-ish. Try b-r and brightness
for t in (10, 15, 20, 30):
    m = ((b - r) > t) & (lum > 120)
    ys, xs = np.nonzero(m)
    print("thr", t, "count", m.sum(), "bbox x", xs.min() + x0, xs.max() + x0, "y", ys.min() + y0, ys.max() + y0)

m = ((b - r) > 15) & (lum > 120)
# grid line positions: darker columns / rows inside the lit window
def dips(profile, lo, hi):
    p = profile[lo:hi]
    out = []
    for i in range(2, len(p) - 2):
        if p[i] < p[i-1] and p[i] <= p[i+1] and p[i] < np.median(p) - 8 and (not out or i - out[-1] > 8):
            out.append(i)
    return [o + lo for o in out]
ys, xs = np.nonzero(m)
bx0, bx1, by0, by1 = xs.min(), xs.max(), ys.min(), ys.max()
cy0, cy1 = by0 + (by1 - by0) // 3, by0 + 2 * (by1 - by0) // 3
colp = lum[cy0:cy1, :].mean(axis=0)
cx0, cx1 = bx0 + (bx1 - bx0) // 3, bx0 + 2 * (bx1 - bx0) // 3
rowp = lum[:, cx0:cx1].mean(axis=1)
dx = dips(colp, bx0 + 2, bx1 - 2)
dy = dips(rowp, by0 + 2, by1 - 2)
print("vertical-line dips", len(dx), [d + x0 for d in dx])
print("horizontal-line dips", len(dy), [d + y0 for d in dy])
if len(dx) > 2: print("x spacing", np.diff(dx).mean())
if len(dy) > 2: print("y spacing", np.diff(dy).mean())

# photo -> viewer panel coordinates (first dips = viewer x 5 / y 9)
sx = np.diff(dx).mean() / 10.0
sy = np.diff(dy).mean() / 10.0
def vx(px): return 5 + (px - (dx[0] + x0)) / sx
def vy(py): return 9 + (py - (dy[0] + y0)) / sy
rows = {}
for yy in range(by0, by1 + 1):
    xr = np.nonzero(m[yy])[0]
    if len(xr): rows[yy] = (xr.min(), xr.max())
print("visible window in panel px: x %.1f..%.1f  y %.1f..%.1f" % (vx(bx0 + x0), vx(bx1 + x0), vy(by0 + y0), vy(by1 + y0)))
L = bx0; Rr = bx1
print("top-left corner: depth(y from top) -> inset(x), panel px")
for d in (0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 16, 20, 24, 28):
    py = by0 + int(round(d * sy))
    if py in rows:
        print("  d=%2d  left inset %.1f  right inset %.1f" % (d, (rows[py][0] - L) / sx, (Rr - rows[py][1]) / sx))
def inset_at_row(py, side): return ((rows[py][0] - L) if side == 0 else (Rr - rows[py][1])) / sx
# fit radius with a circle model to top-left & top-right, bottom
best = None
for R in np.arange(8, 40, 0.5):
    err = 0; n = 0
    for d in np.arange(0, R, 1.0):
        py = by0 + int(round(d * sy))
        if py not in rows: continue
        model = R - np.sqrt(max(R * R - (R - d - 0.5) ** 2, 0))
        for side in (0, 1):
            err += (inset_at_row(py, side) - model) ** 2; n += 1
    if n and (best is None or err / n < best[0]): best = (err / n, R)
print("best-fit circular radius at top: %.1f panel px (rms %.2f)" % (best[1], best[0] ** 0.5))
best = None
for R in np.arange(8, 40, 0.5):
    err = 0; n = 0
    for d in np.arange(0, R, 1.0):
        py = by1 - int(round(d * sy))
        if py not in rows: continue
        model = R - np.sqrt(max(R * R - (R - d - 0.5) ** 2, 0))
        for side in (0, 1):
            err += (inset_at_row(py, side) - model) ** 2; n += 1
    if n and (best is None or err / n < best[0]): best = (err / n, R)
print("best-fit circular radius at bottom: %.1f panel px (rms %.2f)" % (best[1], best[0] ** 0.5))

print("viewer row -> left/right visible x (panel px)")
for d in range(0, 236, 12):
    py = by0 + int(round(d * sy))
    if py in rows:
        print("  vy=%5.1f  x %.1f .. %.1f  width %.1f" % (vy(py + y0), vx(rows[py][0] + x0), vx(rows[py][1] + x0), (rows[py][1] - rows[py][0]) / sx))

# --- rotation-corrected corner fit ---
ys_ = np.array(sorted(rows))
mid = ys_[(ys_ > by0 + 60 * sy) & (ys_ < by1 - 60 * sy)]
lx = np.array([rows[y][0] for y in mid]); rx = np.array([rows[y][1] for y in mid])
al, bl = np.polyfit(mid, lx, 1); ar, br_ = np.polyfit(mid, rx, 1)
print("edge tilt: left %.4f, right %.4f px/px (rotation ~%.2f deg)" % (al, ar, np.degrees(np.arctan(al))))
def corner(top, left):
    pts = []
    for d in np.arange(0, 50, 0.5):
        py = int(round((by0 + d * sy) if top else (by1 - d * sy)))
        if py not in rows: continue
        edge = (al * py + bl) if left else (ar * py + br_)
        v = (rows[py][0] - edge) if left else (edge - rows[py][1])
        pts.append((d, max(v, 0) / sx))
    best = None
    for R in np.arange(10, 60, 0.5):
        for off in np.arange(-3, 6, 0.5):          # allow the window top/bottom to be a bit off
            err = n = 0
            for d, v in pts:
                dd = d - off
                if dd < 0: model = R   # beyond the tangent
                elif dd > R: model = 0
                else: model = R - np.sqrt(max(R * R - (R - dd) ** 2, 0))
                err += (v - model) ** 2; n += 1
            if best is None or err / n < best[0]: best = (err / n, R, off)
    return best
for name, top, left in (("top-left", 1, 1), ("top-right", 1, 0), ("bottom-left", 0, 1), ("bottom-right", 0, 0)):
    e, R, off = corner(top, left)
    print("%-13s radius %.1f panel px (edge offset %.1f, rms %.2f)" % (name, R, off, e ** 0.5))
