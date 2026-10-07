#ifndef SIMPLIFY_H
#define SIMPLIFY_H

#include "loader.h"

/* Reduce a triangle mesh to at most target_tris triangles using vertex-cluster
 * decimation: vertices are snapshotted onto a grid and each occupied cell is
 * replaced by the average of its vertices, which preserves the overall shape
 * while cutting the triangle count. The grid resolution is adapted to hit the
 * target. Returns 1 on success (fills *out, caller frees), 0 on failure. */
int simplify_cluster(const float *verts, int nvert,
                     const unsigned int *tris, int ntri,
                     int target_tris, RawMesh *out, char *err, int errsz);

#endif /* SIMPLIFY_H */
