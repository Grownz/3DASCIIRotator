#ifndef LOADER_H
#define LOADER_H

#include <wchar.h>

/* A parsed triangle mesh: verts is 3*nvert floats, tris is 3*ntri indices. */
typedef struct {
    float        *verts;
    unsigned int *tris;
    int nvert, ntri;
} RawMesh;

/* Load a .stl / .obj / .ply file (format chosen by extension).
 * Returns 1 on success and fills *out (caller must free verts/tris with free()).
 * Returns 0 on failure and writes a short message to err. */
int loader_load(const wchar_t *path, int max_tris, RawMesh *out, char *err, int errsz);

#endif /* LOADER_H */
