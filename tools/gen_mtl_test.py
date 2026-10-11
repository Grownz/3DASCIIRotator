"""Two cubes with distinct OBJ+MTL materials, written with the 'v//vn' index
form, to check per-cell colours and the fixed OBJ face parser."""
import os

OUT = r"build\models"
os.makedirs(OUT, exist_ok=True)

verts = [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
         (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]
tris = [0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
        3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2]

lines = ["mtllib mm.mtl"]
for dx in (-1.5, 1.5):
    for v in verts:
        lines.append("v %g %g %g" % (v[0] + dx, v[1], v[2]))
for n, v in enumerate(verts, 1):
    lines.append("vn 0 0 1")
# cube 1 -> green, cube 2 -> red (indices 1..8 and 9..16)
lines.append("usemtl green")
for t in range(0, len(tris), 3):
    lines.append("f %d//1 %d//2 %d//3" % (tris[t] + 1, tris[t + 1] + 1, tris[t + 2] + 1))
lines.append("usemtl red")
for t in range(0, len(tris), 3):
    lines.append("f %d//1 %d//2 %d//3" % (tris[t] + 9, tris[t + 1] + 9, tris[t + 2] + 9))
open(os.path.join(OUT, "mm.obj"), "w").write("\n".join(lines) + "\n")

open(os.path.join(OUT, "mm.mtl"), "w").write(
    "newmtl green\nKd 0.20 0.90 0.30\n\nnewmtl red\nKd 0.90 0.20 0.20\n")
print("wrote mm.obj + mm.mtl")
