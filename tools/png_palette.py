"""Print the palette (PLTE) of an APNG produced by ascii3D, grouped by hue."""
import struct, sys

path = sys.argv[1] if len(sys.argv) > 1 else "ascii3D_min_mat.png"
data = open(path, "rb").read()
assert data[:8] == b"\x89PNG\r\n\x1a\n"
pos = 8
pal = None
while pos + 8 <= len(data):
    length = struct.unpack(">I", data[pos:pos + 4])[0]
    ctype = data[pos + 4:pos + 8]
    body = data[pos + 8:pos + 8 + length]
    if ctype == b"PLTE":
        pal = body
    pos += 12 + length
    if ctype == b"IEND":
        break
if not pal:
    print("no PLTE")
    sys.exit(0)
cols = [(pal[i], pal[i + 1], pal[i + 2]) for i in range(0, len(pal), 3)]
print("palette entries:", len(cols))
greens = [c for c in cols if c[1] > c[0] + 20 and c[1] > c[2] + 20]
reds = [c for c in cols if c[0] > c[1] + 20 and c[0] > c[2] + 20]
print("green-dominant:", len(greens), "e.g.", greens[:3])
print("red-dominant:  ", len(reds), "e.g.", reds[:3])
print("first 6:", cols[:6])
