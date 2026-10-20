#!/usr/bin/env python3
"""Convert a PSF1/PSF2 console font (ASCII 32..126) to a C header."""
import gzip, struct, sys

data = gzip.open(sys.argv[1]).read() if sys.argv[1].endswith(".gz") else open(sys.argv[1], "rb").read()
if data[:2] == b"\x36\x04":
    h, cs, off, w = 16, data[3], 4, 8
    h = cs
elif data[:4] == b"\x72\xb5\x4a\x86":
    _, _, hdr, _, n, cs, h, w = struct.unpack("<8I", data[:32])[0:8]
    off = hdr
else:
    sys.exit("not a PSF font")
assert w == 8, "only 8px-wide fonts supported"
print(f"/* generated from {sys.argv[1].split('/')[-1]} by psf2h.py: ASCII 32..126, {w}x{h} */")
print(f"#define FONT_W {w}\n#define FONT_H {h}")
print(f"static const unsigned char font_data[95][{h}] = {{")
for c in range(32, 127):
    row = data[off + c * cs: off + c * cs + h]
    print("\t{" + ",".join(f"0x{b:02x}" for b in row) + f"}}, /* '{chr(c) if chr(c) not in '\\\\' else 'backslash'}' */")
print("};")
