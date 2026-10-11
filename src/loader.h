#ifndef LOADER_H
#define LOADER_H

#include <wchar.h>

/* A parsed triangle mesh: verts is 3*nvert floats, tris is 3*ntri indices.
 * face_rgb is optional: 3 bytes (R,G,B) per triangle from an OBJ+MTL material,
 * or NULL when the format has no colours. */
typedef struct {
    float         *verts;
    unsigned int  *tris;
    unsigned char *face_rgb;
    int nvert, ntri;
} RawMesh;

/* Load a .stl / .obj / .ply file (format chosen by extension).
 * Returns 1 on success and fills *out (caller must free verts/tris with free()).
 * Returns 0 on failure and writes a short message to err. */
int loader_load(const wchar_t *path, int max_tris, RawMesh *out, char *err, int errsz);

/* Copies a short message into err (always NUL-terminated). */
void seterr(char *e, int n, const char *m);

/* glTF 2.0 (.gltf/.glb) and 3MF (.3mf); implemented in gltf.c / zip3mf.c. */
int gltf_load(const unsigned char *b, size_t n, const wchar_t *path,
              int max_tris, RawMesh *out, char *err, int errsz);
int mf3_load(const unsigned char *b, size_t n, int max_tris,
             RawMesh *out, char *err, int errsz);

#endif /* LOADER_H */
