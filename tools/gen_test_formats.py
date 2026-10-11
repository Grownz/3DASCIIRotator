import numpy as np, trimesh, os
out = r"build\models"
os.makedirs(out, exist_ok=True)
m = trimesh.creation.torus(major_radius=1.0, minor_radius=0.4, major_segments=48, minor_segments=24)
print("tris", len(m.faces), "verts", len(m.vertices))
# GLB
p = os.path.join(out, "torus_test.glb")
open(p, "wb").write(m.export(file_type="glb"))
print("wrote", p, os.path.getsize(p))
# glTF (may return dict of files)
d = m.export(file_type="gltf")
if isinstance(d, dict):
    for name, data in d.items():
        fp = os.path.join(out, name if name != "model.gltf" else "torus_test.gltf")
        open(fp, "wb").write(data if isinstance(data, bytes) else data.encode())
        print("wrote", fp, os.path.getsize(fp))
else:
    p = os.path.join(out, "torus_test.gltf")
    open(p, "wb").write(d)
    print("wrote", p, os.path.getsize(p))
try:
    p = os.path.join(out, "torus_test.3mf")
    open(p, "wb").write(m.export(file_type="3mf"))
    print("wrote", p, os.path.getsize(p))
except Exception as e:
    print("3mf export failed:", repr(e))
