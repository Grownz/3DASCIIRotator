"""Inspect an OBJ: bbox, aspect, per-axis spread, and export an STL of the same
geometry so the two loaders can be benchmarked head-to-head."""
import sys, struct

path = sys.argv[1] if len(sys.argv) > 1 else r"build\models\DolLinkR1_fixed.obj"
verts, faces = [], []
for line in open(path, "r", errors="ignore"):
    if line.startswith("v "):
        p = line.split()
        verts.append((float(p[1]), float(p[2]), float(p[3])))
    elif line.startswith("f "):
        idx = []
        for tok in line.split()[1:]:
            idx.append(int(tok.split("/")[0]) - 1)
        for i in range(2, len(idx)):
            faces.append((idx[0], idx[i - 1], idx[i]))

print("verts", len(verts), "tris", len(faces))
mn = [min(v[i] for v in verts) for i in range(3)]
mx = [max(v[i] for v in verts) for i in range(3)]
size = [mx[i] - mn[i] for i in range(3)]
print("bbox min", [round(v, 4) for v in mn])
print("bbox max", [round(v, 4) for v in mx])
print("size    ", [round(v, 4) for v in size])
print("aspect  ", [round(v / max(size), 4) for v in size])

# export binary STL of the same geometry
out = path.rsplit(".", 1)[0] + "_conv.stl"
with open(out, "wb") as f:
    f.write(b"\0" * 80)
    f.write(struct.pack("<I", len(faces)))
    for a, b, c in faces:
        va, vb, vc = verts[a], verts[b], verts[c]
        ux, uy, uz = vb[0] - va[0], vb[1] - va[1], vb[2] - va[2]
        vx, vy, vz = vc[0] - va[0], vc[1] - va[1], vc[2] - va[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        l = (nx * nx + ny * ny + nz * nz) ** 0.5 or 1.0
        f.write(struct.pack("<3f", nx / l, ny / l, nz / l))
        for v in (va, vb, vc):
            f.write(struct.pack("<3f", *v))
        f.write(struct.pack("<H", 0))
print("wrote", out)
