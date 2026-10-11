"""Generate minimal hand-crafted glTF/GLB/3MF test files for the loader.

Covers: base64 data URI buffers, external .bin buffers, GLB BIN chunk, TRS node
transforms, material base colours, and a 3MF with a base material + build item.
"""
import base64, json, os, struct, zipfile

OUT = r"build\models"
os.makedirs(OUT, exist_ok=True)

# --- a unit cube: 8 verts, 12 triangles -----------------------------------
verts = [
    (-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
    (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1),
]
tris = [
    0, 1, 2, 0, 2, 3,   4, 6, 5, 4, 7, 6,   0, 4, 5, 0, 5, 1,
    3, 2, 6, 3, 6, 7,   0, 3, 7, 0, 7, 4,   1, 5, 6, 1, 6, 2,
]
pos = b"".join(struct.pack("<3f", *v) for v in verts)      # 96 bytes
idx = b"".join(struct.pack("<H", i) for i in tris)          # 72 bytes
buf = pos + idx                                             # 168 bytes

GREEN = [0.20, 0.90, 0.30, 1.0]


def gltf_json(buffer_uri=None, byte_length=len(buf)):
    gltf = {
        "asset": {"version": "2.0", "generator": "handmade"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "translation": [0.0, 0.0, 0.0],
                   "rotation": [0.0, 0.0, 0.0, 1.0], "scale": [1.0, 1.0, 1.0]}],
        "meshes": [{"primitives": [
            {"attributes": {"POSITION": 0}, "indices": 1, "material": 0, "mode": 4}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorFactor": GREEN}}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 8, "type": "VEC3"},
            {"bufferView": 1, "componentType": 5123, "count": len(tris), "type": "SCALAR"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": len(pos), "target": 34962},
            {"buffer": 0, "byteOffset": len(pos), "byteLength": len(idx), "target": 34963},
        ],
        "buffers": [{"byteLength": byte_length}],
    }
    if buffer_uri is not None:
        gltf["buffers"][0]["uri"] = buffer_uri
    return gltf


# 1) glTF with an embedded base64 buffer
data_uri = "data:application/octet-stream;base64," + base64.b64encode(buf).decode()
with open(os.path.join(OUT, "min_b64.gltf"), "w", encoding="utf-8") as f:
    json.dump(gltf_json(data_uri), f, indent=1)
print("wrote min_b64.gltf")

# 2) glTF with an external .bin
with open(os.path.join(OUT, "min_ext.bin"), "wb") as f:
    f.write(buf)
with open(os.path.join(OUT, "min_ext.gltf"), "w", encoding="utf-8") as f:
    json.dump(gltf_json("min_ext.bin"), f, indent=1)
print("wrote min_ext.gltf + min_ext.bin")

# 3) GLB with a BIN chunk (and a 90-degree rotation about Y in the node)
js = gltf_json(None)
js["nodes"][0]["rotation"] = [0.0, 0.7071068, 0.0, 0.7071068]
jbytes = json.dumps(js, separators=(",", ":")).encode()
while len(jbytes) % 4:
    jbytes += b" "
bbytes = buf
while len(bbytes) % 4:
    bbytes += b"\x00"
total = 12 + 8 + len(jbytes) + 8 + len(bbytes)
glb = struct.pack("<4sII", b"glTF", 2, total)
glb += struct.pack("<I4s", len(jbytes), b"JSON") + jbytes
glb += struct.pack("<I4s", len(bbytes), b"BIN\x00") + bbytes
with open(os.path.join(OUT, "min_glb.glb"), "wb") as f:
    f.write(glb)
print("wrote min_glb.glb")

# 4) 3MF with a base material colour and a build item
model = ['<?xml version="1.0" encoding="UTF-8"?>',
         '<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">',
         ' <resources>',
         '  <basematerials id="1"><base name="green" displaycolor="#33CC44FF"/></basematerials>',
         '  <object id="2" type="model" pid="1" pindex="0"><mesh><vertices>']
for v in verts:
    model.append('    <vertex x="%g" y="%g" z="%g"/>' % v)
model.append('   </vertices><triangles>')
for t in range(0, len(tris), 3):
    model.append('    <triangle v1="%d" v2="%d" v3="%d"/>' % (tris[t], tris[t + 1], tris[t + 2]))
model += ['   </triangles></mesh></object>',
          ' </resources>',
          ' <build><item objectid="2"/></build>',
          '</model>']
xml = "\n".join(model).encode("utf-8")

ct = (b'<?xml version="1.0" encoding="UTF-8"?>'
      b'<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
      b'<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
      b'<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
rels = (b'<?xml version="1.0" encoding="UTF-8"?>'
        b'<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        b'<Relationship Target="/3D/3dmodel.model" Id="rel0" '
        b'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
with zipfile.ZipFile(os.path.join(OUT, "min_mat.3mf"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", ct)
    z.writestr("_rels/.rels", rels)
    z.writestr("3D/3dmodel.model", xml)
print("wrote min_mat.3mf")
