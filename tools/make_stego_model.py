#!/usr/bin/env python3
"""Bake the Quaternius stegosaurus GLB into src/stego_model.h.

The source model is a rigged glTF (GLB). This script:

  1. downloads the GLB if it is not already present,
  2. applies the glTF skinning by hand (trimesh ignores it for this rig),
  3. re-orients the mesh so length = +x, depth = +y, height = +z,
     centres and uniformly scales it,
  4. writes the vertices and triangles as C arrays.

The application ray-traces those triangles with a bounding-volume hierarchy
and shades them flat, which preserves the sharp back plates and tail spikes.

Model: "Stegosaurus" by Quaternius (https://quaternius.com), released under
the Creative Commons Zero (CC0 / public domain) licence. Obtained via
https://poly.pizza/m/eFcNbOlpvl

Requires: numpy, trimesh, pygltflib  (pip install numpy trimesh pygltflib)

Usage:  python tools/make_stego_model.py
"""
import os
import sys
import urllib.request

import numpy as np

GLB_URL = "https://static.poly.pizza/6f8f4ac6-f9e8-488d-97a8-220b9b2fd02a.glb"
HERE    = os.path.dirname(os.path.abspath(__file__))
ROOT    = os.path.dirname(HERE)
GLB     = os.path.join(HERE, "stegosaurus.glb")
OUT     = os.path.join(ROOT, "src", "stego_model.h")

TARGET = 3.0     # length of the scaled model along x

_CT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16,
       5125: np.uint32, 5126: np.float32}
_NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


# --------------------------------------------------------------- glTF helpers
def read_accessor(gltf, blob, idx):
    acc = gltf.accessors[idx]
    bv = gltf.bufferViews[acc.bufferView]
    dt = np.dtype(_CT[acc.componentType])
    ncomp = _NC[acc.type]
    start = (bv.byteOffset or 0) + (acc.byteOffset or 0)
    stride = bv.byteStride
    if stride and stride != ncomp * dt.itemsize:
        out = np.zeros((acc.count, ncomp), dtype=dt)
        for i in range(acc.count):
            out[i] = np.frombuffer(blob, dtype=dt, count=ncomp, offset=start + i * stride)
        return out
    return np.frombuffer(blob, dtype=dt, count=acc.count * ncomp, offset=start) \
             .reshape(acc.count, ncomp)


def quat_matrix(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
        [0, 0, 0, 1]], dtype=float)


def local_matrix(node):
    if node.matrix:
        return np.array(node.matrix, dtype=float).reshape(4, 4).T
    T = np.eye(4)
    if node.translation:
        T[:3, 3] = node.translation
    R = quat_matrix(node.rotation) if node.rotation else np.eye(4)
    S = np.eye(4)
    if node.scale:
        S[0, 0], S[1, 1], S[2, 2] = node.scale
    return T @ R @ S


def global_matrices(gltf):
    nodes = gltf.nodes
    parent = {}
    for i, n in enumerate(nodes):
        for c in (n.children or []):
            parent[c] = i
    mats = {}

    def g(i):
        if i in mats:
            return mats[i]
        m = local_matrix(nodes[i])
        if i in parent:
            m = g(parent[i]) @ m
        mats[i] = m
        return m

    for i in range(len(nodes)):
        g(i)
    return mats


def load_stego_mesh(path):
    import trimesh
    from pygltflib import GLTF2
    gltf = GLTF2().load(path)
    blob = gltf.binary_blob()
    gm = global_matrices(gltf)
    skin = gltf.skins[0]
    ibm = read_accessor(gltf, blob, skin.inverseBindMatrices).reshape(-1, 4, 4)
    ibm = np.transpose(ibm, (0, 2, 1))
    joint_mats = np.stack([gm[j] @ ibm[k] for k, j in enumerate(skin.joints)])

    verts, faces, base = [], [], 0
    for mesh in gltf.meshes:
        for prim in mesh.primitives:
            a = prim.attributes
            pos = read_accessor(gltf, blob, a.POSITION).astype(float)
            if a.JOINTS_0 is not None and a.WEIGHTS_0 is not None:
                joints = read_accessor(gltf, blob, a.JOINTS_0).astype(int)
                weights = read_accessor(gltf, blob, a.WEIGHTS_0).astype(float)
                M = np.zeros((len(pos), 4, 4))
                for k in range(4):
                    M += weights[:, k, None, None] * joint_mats[joints[:, k]]
                p4 = np.c_[pos, np.ones(len(pos))]
                pos = np.einsum('nij,nj->ni', M, p4)[:, :3]
            idx = read_accessor(gltf, blob, prim.indices).reshape(-1).astype(int) \
                if prim.indices is not None else np.arange(len(pos))
            verts.append(pos)
            faces.append(idx.reshape(-1, 3) + base)
            base += len(pos)
    return trimesh.Trimesh(vertices=np.vstack(verts), faces=np.vstack(faces),
                           process=False)


# ------------------------------------------------------------------- pipeline
def main():
    if not os.path.exists(GLB):
        print("downloading", GLB_URL)
        req = urllib.request.Request(GLB_URL, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req) as r, open(GLB, "wb") as out:
            out.write(r.read())

    mesh = load_stego_mesh(GLB)
    print("loaded  verts=%d faces=%d watertight=%s"
          % (len(mesh.vertices), len(mesh.faces), mesh.is_watertight))

    # model axes: x=width, y=height(up), z=length -> world x=length, y=depth, z=height
    R = np.array([[0., 0., 1.], [1., 0., 0.], [0., 1., 0.]])
    mesh.vertices = mesh.vertices @ R.T
    mesh.vertices -= mesh.bounds.mean(axis=0)
    mesh.vertices *= TARGET / float(mesh.extents.max())
    mesh.merge_vertices()
    mesh.update_faces(mesh.nondegenerate_faces())
    mesh.update_faces(mesh.unique_faces())
    mesh.remove_unreferenced_vertices()
    print("oriented extents (x=len,y=depth,z=height) =", np.round(mesh.extents, 4))

    verts = np.asarray(mesh.vertices, dtype=np.float32)
    tris = np.asarray(mesh.faces, dtype=np.uint32)
    nv, nf = len(verts), len(tris)

    with open(OUT, "w") as f:
        f.write("/* Auto-generated by tools/make_stego_model.py -- do not edit.\n")
        f.write(" * Stegosaurus model by Quaternius (CC0 / public domain), via poly.pizza.\n")
        f.write(" * Triangle mesh: vertices are float, indices index STEGO_VERT.\n */\n")
        f.write("#ifndef STEGO_MODEL_H\n#define STEGO_MODEL_H\n\n")
        f.write("#define STEGO_NVERT %d\n#define STEGO_NTRI %d\n\n" % (nv, nf))
        f.write("static const float STEGO_VERT[%d] = {\n" % (nv * 3))
        for i in range(0, nv, 5):
            f.write("  " + ",".join("%.6ff,%.6ff,%.6ff" % tuple(verts[j]) for j in range(i, min(i + 5, nv))) + ",\n")
        f.write("};\n\n")
        f.write("static const unsigned int STEGO_TRI[%d] = {\n" % (nf * 3))
        flat = tris.reshape(-1)
        for i in range(0, len(flat), 12):
            f.write("  " + ",".join("%d" % v for v in flat[i:i + 12]) + ",\n")
        f.write("};\n\n#endif /* STEGO_MODEL_H */\n")
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    sys.exit(main())
