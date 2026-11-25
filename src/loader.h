#ifndef LOADER_H
#define LOADER_H

#include <wchar.h>

/* A parsed triangle mesh: verts is 3*nvert floats, tris is 3*ntri indices.
 * face_rgb is optional and, when present, exactly 3*ntri bytes (R,G,B per
 * triangle) from a material-aware format, or NULL. */
typedef struct {
    float         *verts;
    unsigned int  *tris;
    unsigned char *face_rgb;
    int nvert, ntri;
} RawMesh;

/* Load a mesh file (format chosen by extension: .stl/.obj/.ply/.off/.gltf/
 * .glb/.3mf/.fbx). Returns 1 on success and fills *out (caller must free
 * verts/tris/face_rgb with free()). Returns 0 on failure and writes a short
 * message to err. */
int loader_load(const wchar_t *path, int max_tris, RawMesh *out, char *err, int errsz);

/* Parse an in-memory file. ext is the extension including the dot (".obj"...),
 * path is only used to resolve sidecar files (.mtl, external glTF buffers).
 * b must hold n bytes plus one NUL byte at b[n] and may be modified. Same
 * result convention as loader_load, which this is split out of (also the
 * entry point of the fuzz harness in tests/fuzz). */
int loader_load_buf(unsigned char *b, size_t n, const wchar_t *ext, const wchar_t *path,
                    int max_tris, RawMesh *out, char *err, int errsz);

/* Copies a short message into err (always NUL-terminated). */
void seterr(char *e, int n, const char *m);

/* glTF 2.0 (.gltf/.glb) and 3MF (.3mf); implemented in gltf.c / zip3mf.c. */
int gltf_load(const unsigned char *b, size_t n, const wchar_t *path,
              int max_tris, RawMesh *out, char *err, int errsz);
int mf3_load(const unsigned char *b, size_t n, int max_tris,
             RawMesh *out, char *err, int errsz);
int fbx_load(const unsigned char *b, size_t n, int max_tris,
             RawMesh *out, char *err, int errsz);

#endif /* LOADER_H */
