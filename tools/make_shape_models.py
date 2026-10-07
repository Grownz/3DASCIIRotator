#!/usr/bin/env python3
"""Bake the extra shapes (F1 car, companion cube, Maus) into C headers.

Writes, next to src/stego_model.h:
  src/f1_model.h          from Kenney's Car Kit "race" model (CC0)
  src/companion_model.h   from the "Portal: Companion Cube" model (CC BY 3.0)
  src/maus_model.h        built from primitives here (original work)

Each header exposes <PREFIX>_NVERT, <PREFIX>_NTRI, <PREFIX>_VERT[] and
<PREFIX>_TRI[]. The application ray-traces the triangles.

Requires: numpy, trimesh  (pip install numpy trimesh)

Usage:  python tools/make_shape_models.py
"""
import os
import sys
import zipfile
import urllib.request

import numpy as np
import trimesh

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC  = os.path.join(ROOT, "src")
MODELS = os.path.join(HERE, "models")

KENNEY_URL = "https://kenney.nl/media/pages/assets/car-kit/1a312ec241-1775131960/kenney_car-kit.zip"
COMPANION_URL = "https://static.poly.pizza/026e4915-82c8-4cda-abc2-fd85341284ba.glb"
FISH_URL = "https://static.poly.pizza/1dfba885-5874-4e3f-8926-0ded89b0894f.glb"
KEBAB_URL = "https://static.poly.pizza/4466ba21-e267-4807-8d6c-762d5bab8f31.glb"
CHINA_MAN_URL = "https://api.thingiverse.com/v2/files/13049662/download"  # Man_W_Bags_28mm.stl
UA = {"User-Agent": "Mozilla/5.0"}


def fetch(url, out):
    print("downloading", url)
    req = urllib.request.Request(url, headers=UA)
    with urllib.request.urlopen(req) as r, open(out, "wb") as f:
        f.write(r.read())


def get_kenney_race():
    """Return a path to Kenney's 'race' car GLB, downloading the kit if needed."""
    path = os.path.join(MODELS, "race.glb")
    if os.path.exists(path):
        return path
    os.makedirs(MODELS, exist_ok=True)
    zip_path = os.path.join(MODELS, "kenney_car-kit.zip")
    if not os.path.exists(zip_path):
        fetch(KENNEY_URL, zip_path)
    with zipfile.ZipFile(zip_path) as z:
        name = [n for n in z.namelist() if n.replace("\\", "/").endswith("GLB format/race.glb")][0]
        with z.open(name) as fin, open(path, "wb") as fout:
            fout.write(fin.read())
    return path


def get_companion():
    path = os.path.join(MODELS, "companion.glb")
    if not os.path.exists(path):
        os.makedirs(MODELS, exist_ok=True)
        fetch(COMPANION_URL, path)
    return path


def get_glb(url, name):
    path = os.path.join(MODELS, name)
    if not os.path.exists(path):
        os.makedirs(MODELS, exist_ok=True)
        fetch(url, path)
    return path


# ------------------------------------------------------------------ primitives
def ellipsoid(center, radii, sub=2):
    m = trimesh.creation.icosphere(subdivisions=sub, radius=1.0)
    m.apply_scale(radii)
    m.apply_translation(center)
    return m


def capsule_between(a, b, radius):
    a = np.asarray(a, float); b = np.asarray(b, float)
    d = b - a; h = float(np.linalg.norm(d))
    m = trimesh.creation.capsule(height=max(h, 1e-4), radius=radius, count=[8, 8])
    if h > 1e-6:
        m.apply_transform(trimesh.geometry.align_vectors([0, 0, 1], d))
    m.apply_translation((a + b) / 2.0)
    return m


def build_maus():
    """'Die Maus' built from primitives (original geometry, faces -y)."""
    p = []
    p.append(ellipsoid((0.0, 0.0, 0.80), (0.40, 0.34, 0.52), sub=3))   # body
    p.append(ellipsoid((0.0, -0.06, 1.55), (0.36, 0.34, 0.36), sub=3))  # head
    p.append(ellipsoid((0.0, -0.42, 1.46), (0.15, 0.20, 0.14)))         # snout
    p.append(ellipsoid((0.0, -0.60, 1.46), (0.10, 0.08, 0.09)))         # nose
    for sx in (-1, 1):
        p.append(ellipsoid((sx * 0.30, 0.06, 1.98), (0.27, 0.07, 0.30)))   # ears
        p.append(capsule_between((sx * 0.30, -0.05, 0.98), (sx * 0.30, -0.24, 0.58), 0.075))  # arms
        p.append(capsule_between((sx * 0.18, 0.00, 0.38), (sx * 0.18, -0.04, 0.06), 0.10))    # legs
        p.append(ellipsoid((sx * 0.18, -0.16, 0.05), (0.13, 0.20, 0.06)))                     # feet
    p.append(capsule_between((0.0, 0.30, 0.70), (0.0, 0.52, 0.30), 0.055))  # tail
    p.append(capsule_between((0.0, 0.52, 0.30), (0.0, 0.60, 0.02), 0.045))
    return trimesh.util.concatenate(p)


def build_berlin():
    """Brandenburg Gate built from primitives (original geometry).
    x = width (colonnade), y = depth, z = up."""
    def box(c, e):
        m = trimesh.creation.box(extents=e)
        m.apply_translation(c)
        return m
    p = []
    W = 6.0
    colx = [-2.5, -1.5, -0.5, 0.5, 1.5, 2.5]
    p.append(box((0, 0, 0.10), (W + 0.4, 1.30, 0.20)))          # stylobate
    for x in colx:                                              # 6 columns x 2 rows
        for y in (-0.34, 0.34):
            p.append(trimesh.creation.cylinder(radius=0.13, height=1.55, sections=10)
                     .apply_translation((x, y, 0.20 + 1.55 / 2)))
    p.append(box((0, 0, 1.90), (W + 0.5, 1.10, 0.34)))          # entablature
    p.append(box((0, 0, 2.22), (4.6, 0.95, 0.30)))              # attic
    p.append(box((0.0, 0.0, 2.50), (0.55, 0.45, 0.30)))         # quadriga chariot
    for y in (-0.30, -0.10, 0.10, 0.30):
        p.append(ellipsoid((0.55, y, 2.62), (0.34, 0.07, 0.12)))   # horses
        p.append(ellipsoid((0.86, y, 2.70), (0.10, 0.06, 0.10)))
        for x in (0.35, 0.72):
            p.append(capsule_between((x, y, 2.55), (x, y, 2.40), 0.035))
    p.append(ellipsoid((-0.05, 0.0, 2.80), (0.10, 0.10, 0.22)))    # driver
    return trimesh.util.concatenate(p)


# --------------------------------------------------- "Tank Man" (China)
def _box(c, e):
    m = trimesh.creation.box(extents=e)
    m.apply_translation(c)
    return m


def _cyl(radius, height, axis, c, sections=16):
    m = trimesh.creation.cylinder(radius=radius, height=height, sections=sections)
    if axis == "x":
        m.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [0, 1, 0]))
    elif axis == "y":
        m.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [1, 0, 0]))
    m.apply_translation(c)
    return m


def build_tank():
    """A Type 59-style tank, length along x (original geometry)."""
    p = []
    p.append(_box((0, 0, 0.30), (3.30, 1.20, 0.40)))     # lower hull
    p.append(_box((0, 0, 0.62), (2.70, 1.02, 0.30)))     # upper hull
    p.append(_box((1.55, 0, 0.46), (0.50, 1.20, 0.30)))  # glacis
    for sy in (-1, 1):
        p.append(_box((0, sy * 0.74, 0.24), (3.50, 0.30, 0.48)))               # track
        for i in range(5):
            p.append(_cyl(0.20, 0.28, "y", (-1.30 + i * 0.65, sy * 0.74, 0.20)))  # road wheels
    p.append(ellipsoid((-0.20, 0, 0.92), (0.78, 0.70, 0.34), sub=3))           # turret
    p.append(_cyl(0.09, 1.60, "x", (1.10, 0, 1.08), sections=12))             # gun barrel
    p.append(_box((0.42, 0, 1.08), (0.30, 0.34, 0.30)))                        # mantlet
    p.append(_cyl(0.14, 0.16, "z", (-0.20, 0, 1.22), sections=12))            # cupola
    return trimesh.util.concatenate(p)


def build_china():
    """'Tank Man' scene: the Thingiverse figure in front of a tank."""
    path = os.path.join(MODELS, "man_w_bags.stl")
    if not os.path.exists(path):
        os.makedirs(MODELS, exist_ok=True)
        fetch(CHINA_MAN_URL, path)
    man = trimesh.load(path)
    if isinstance(man, trimesh.Scene):
        man = man.to_geometry()
    man = man.simplify_quadric_decimation(face_count=6000)
    R = np.array([[0., 0., 1.], [1., 0., 0.], [0., 1., 0.]])   # model y=up, z=facing
    man.vertices = man.vertices @ R.T
    Rz = np.array([[-1., 0., 0.], [0., -1., 0.], [0., 0., 1.]])  # turn 180 deg: face the tank
    man.vertices = man.vertices @ Rz.T
    man.vertices -= man.bounds.mean(axis=0)
    man.vertices *= 1.35 / float(man.extents[2])
    man.vertices[:, 2] -= man.vertices[:, 2].min()
    # further from the muzzle; left shoulder sits on the gun axis (y=0)
    man.apply_translation((2.75, 0.15, 0.0))
    return trimesh.util.concatenate([man, build_tank()])


# ----------------------------------------------------------------------- bake
def load_mesh(path):
    obj = trimesh.load(path, force='scene')
    m = obj.to_geometry() if isinstance(obj, trimesh.Scene) else obj
    m.merge_vertices()
    m.update_faces(m.nondegenerate_faces())
    m.update_faces(m.unique_faces())
    m.remove_unreferenced_vertices()
    return m


def to_final_frame(mesh, mapping):
    """mapping: 'zyx' = model (x,y,z) -> world (z,x,y) (length=z, up=y);
    'yzx' = model (x,y,z) -> world (y,z,x) (length=y); 'identity' = as is."""
    m = mesh.copy()
    if mapping == "zyx":
        R = np.array([[0., 0., 1.], [1., 0., 0.], [0., 1., 0.]])
        m.vertices = m.vertices @ R.T
    elif mapping == "yzx":
        R = np.array([[0., 1., 0.], [0., 0., 1.], [1., 0., 0.]])
        m.vertices = m.vertices @ R.T
    elif mapping == "identity":
        pass
    else:
        raise ValueError(mapping)
    m.vertices -= m.bounds.mean(axis=0)
    return m


def bake(mesh, prefix, comment, out_path, target=3.0):
    v = np.asarray(mesh.vertices, np.float64)
    v = v - (v.min(0) + v.max(0)) / 2.0
    v = v * (target / float((v.max(0) - v.min(0)).max()))
    verts = v.astype(np.float32)
    tris = np.asarray(mesh.faces, np.uint32)
    nv, nf = len(verts), len(tris)
    with open(out_path, "w") as f:
        f.write("/* Auto-generated by tools/make_shape_models.py -- do not edit.\n")
        for line in comment.splitlines():
            f.write(" * " + line + "\n")
        f.write(" * Triangle mesh: float vertices, indices index %s_VERT.\n */\n" % prefix)
        f.write("#ifndef %s_MODEL_H\n#define %s_MODEL_H\n\n" % (prefix, prefix))
        f.write("#define %s_NVERT %d\n#define %s_NTRI %d\n\n" % (prefix, nv, prefix, nf))
        f.write("static const float %s_VERT[%d] = {\n" % (prefix, nv * 3))
        for i in range(0, nv, 5):
            f.write("  " + ",".join("%.6ff,%.6ff,%.6ff" % tuple(verts[j]) for j in range(i, min(i + 5, nv))) + ",\n")
        f.write("};\n\n")
        f.write("static const unsigned int %s_TRI[%d] = {\n" % (prefix, nf * 3))
        flat = tris.reshape(-1)
        for i in range(0, len(flat), 12):
            f.write("  " + ",".join("%d" % v for v in flat[i:i + 12]) + ",\n")
        f.write("};\n\n#endif /* %s_MODEL_H */\n" % prefix)
    print("wrote %s (%d verts, %d tris, %d bytes)" % (out_path, nv, nf, os.path.getsize(out_path)))


def main():
    # F1: Kenney Car Kit "race" (length=z, up=y, width=x) -> world (x=len,y=width,z=up)
    f1 = to_final_frame(load_mesh(get_kenney_race()), "zyx")
    a = np.radians(35.0)                       # default 3/4 view reads better
    Rz = np.array([[np.cos(a), -np.sin(a), 0.], [np.sin(a), np.cos(a), 0.], [0., 0., 1.]])
    f1.vertices = f1.vertices @ Rz.T
    bake(f1, "F1",
         "Formula-style race car from Kenney's Car Kit.\n"
         "Source: https://kenney.nl/assets/car-kit  License: CC0 1.0 (public domain).",
         os.path.join(SRC, "f1_model.h"))

    # Companion cube (already axis-aligned)
    comp = to_final_frame(load_mesh(get_companion()), "identity")
    bake(comp, "COMPANION",
         "\"Portal: Companion Cube\" by Prateek Karajgikar (via poly.pizza).\n"
         "License: CC BY 3.0 -- attribution required. Portal is a Valve trademark.",
         os.path.join(SRC, "companion_model.h"))

    # Maus (built here, faces -y)
    bake(build_maus(), "MAUS",
         "\"Die Maus\" built from primitives by tools/make_shape_models.py.\n"
         "Original geometry authored for this project (no external model).",
         os.path.join(SRC, "maus_model.h"))

    # Trout (length=z, up=y -> world length=x)
    fish = to_final_frame(load_mesh(get_glb(FISH_URL, "trout.glb")), "zyx")
    bake(fish, "FISH",
         "\"Trout\" by Poly by Google (via poly.pizza).\n"
         "License: CC BY 3.0 -- attribution required.",
         os.path.join(SRC, "fish_model.h"))

    # Kebab (skewer long axis=y -> world length=x)
    kebab = to_final_frame(load_mesh(get_glb(KEBAB_URL, "kebab.glb")), "yzx")
    bake(kebab, "KEBAB",
         "\"Kebab\" by Poly by Google (via poly.pizza).\n"
         "License: CC BY 3.0 -- attribution required.",
         os.path.join(SRC, "kebab_model.h"))

    # Brandenburg Gate (built here, already in the final frame)
    bake(build_berlin(), "BERLIN",
         "Brandenburg Gate built from primitives by tools/make_shape_models.py.\n"
         "Original geometry authored for this project (no external model).",
         os.path.join(SRC, "berlin_model.h"))

    # Tank Man: Thingiverse figure in front of an original tank
    bake(build_china(), "CHINA",
         "\"Tank Man\": figure from the \"Tiananmen Square Playset\" by WindhamGraves\n"
         "(https://www.thingiverse.com/thing:6007266).\n"
         "The tank is original geometry by tools/make_shape_models.py.",
         os.path.join(SRC, "china_model.h"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
