/* ============================================================================
 * simplify.c - vertex-cluster decimation (LOD).
 *
 * A grid of cell size `cell` is laid over the mesh; every vertex is assigned to
 * a cell, each occupied cell becomes one output vertex (the average of the
 * vertices in it) and faces whose three vertices collapse onto fewer than three
 * distinct cells are dropped. The cell size is adapted until the triangle count
 * is at or below the target, so the overall shape is kept while the triangle
 * count drops.
 * ==========================================================================*/
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include "simplify.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned long long key;
    int idx;
} VKey;

static int vkey_cmp(const void *a, const void *b) {
    unsigned long long ka = ((const VKey *)a)->key, kb = ((const VKey *)b)->key;
    if (ka < kb) return -1;
    if (ka > kb) return 1;
    return 0;
}

static unsigned long long cell_key(const float *v, const float *org, double cell) {
    long long i = (long long)floor(((double)v[0] - org[0]) / cell);
    long long j = (long long)floor(((double)v[1] - org[1]) / cell);
    long long k = (long long)floor(((double)v[2] - org[2]) / cell);
    const long long OFF = 1LL << 20, MASK = (1LL << 21) - 1;
    i = (i + OFF) & MASK;
    j = (j + OFF) & MASK;
    k = (k + OFF) & MASK;
    return ((unsigned long long)i << 42) | ((unsigned long long)j << 21) | (unsigned long long)k;
}

/* One clustering pass at a given cell size. Returns 0 on allocation failure. */
static int cluster_try(const float *V, int nv, const unsigned int *T, int nt,
                       double cell, const float *org,
                       float **ov, int *onv, unsigned int **ot, int *ont) {
    VKey *keys = (VKey *)malloc(sizeof(VKey) * (size_t)nv);
    int *vmap = (int *)malloc(sizeof(int) * (size_t)nv);
    float *cv = (float *)malloc(sizeof(float) * 3 * (size_t)nv);
    unsigned int *ct = (unsigned int *)malloc(sizeof(unsigned int) * 3 * (size_t)nt);
    int i, cnv = 0, cnt = 0;

    if (!keys || !vmap || !cv || !ct) {
        free(keys); free(vmap); free(cv); free(ct);
        return 0;
    }
    for (i = 0; i < nv; ++i) {
        keys[i].idx = i;
        keys[i].key = cell_key(&V[i * 3], org, cell);
    }
    qsort(keys, (size_t)nv, sizeof(VKey), vkey_cmp);

    for (i = 0; i < nv; ) {
        unsigned long long k = keys[i].key;
        double sx = 0, sy = 0, sz = 0;
        int j = i, c;
        while (j < nv && keys[j].key == k) {
            const float *p = &V[keys[j].idx * 3];
            sx += p[0]; sy += p[1]; sz += p[2];
            ++j;
        }
        c = j - i;
        cv[cnv * 3 + 0] = (float)(sx / c);
        cv[cnv * 3 + 1] = (float)(sy / c);
        cv[cnv * 3 + 2] = (float)(sz / c);
        for (; i < j; ++i) vmap[keys[i].idx] = cnv;
        ++cnv;
    }

    for (i = 0; i < nt; ++i) {
        unsigned int a = (unsigned int)vmap[T[i * 3 + 0]];
        unsigned int b = (unsigned int)vmap[T[i * 3 + 1]];
        unsigned int c = (unsigned int)vmap[T[i * 3 + 2]];
        if (a == b || b == c || a == c) continue;
        ct[cnt * 3 + 0] = a;
        ct[cnt * 3 + 1] = b;
        ct[cnt * 3 + 2] = c;
        ++cnt;
    }

    free(keys); free(vmap);
    if (cnt == 0) { free(cv); free(ct); return 0; }
    *ov = cv; *onv = cnv;
    *ot = ct; *ont = cnt;
    return 1;
}

int simplify_cluster(const float *verts, int nvert,
                     const unsigned int *tris, int ntri,
                     int target_tris, RawMesh *out, char *err, int errsz) {
    float org[3], mx[3];
    double area = 0.0, maxext, cell;
    float *ov = NULL; int onv = 0; unsigned int *ot = NULL; int ont = 0;
    int i, iter;

    out->verts = NULL; out->tris = NULL; out->nvert = out->ntri = 0;

    if (nvert <= 0 || ntri <= 0) { if (errsz) err[0] = '\0'; return 0; }
    if (target_tris < 4) target_tris = 4;

    /* already small enough: just copy */
    if (ntri <= target_tris) {
        out->verts = (float *)malloc(sizeof(float) * 3 * (size_t)nvert);
        out->tris = (unsigned int *)malloc(sizeof(unsigned int) * 3 * (size_t)ntri);
        if (!out->verts || !out->tris) { free(out->verts); free(out->tris); return 0; }
        memcpy(out->verts, verts, sizeof(float) * 3 * (size_t)nvert);
        memcpy(out->tris, tris, sizeof(unsigned int) * 3 * (size_t)ntri);
        out->nvert = nvert;
        out->ntri = ntri;
        return 1;
    }

    for (i = 0; i < 3; ++i) { org[i] = verts[i]; mx[i] = verts[i]; }
    for (i = 1; i < nvert; ++i) {
        int k;
        for (k = 0; k < 3; ++k) {
            if (verts[i * 3 + k] < org[k]) org[k] = verts[i * 3 + k];
            if (verts[i * 3 + k] > mx[k]) mx[k] = verts[i * 3 + k];
        }
    }
    maxext = mx[0] - org[0];
    if (mx[1] - org[1] > maxext) maxext = mx[1] - org[1];
    if (mx[2] - org[2] > maxext) maxext = mx[2] - org[2];
    if (maxext <= 1e-12) { if (errsz) err[0] = '\0'; return 0; }

    for (i = 0; i < ntri; ++i) {
        const float *a = &verts[tris[i * 3 + 0] * 3];
        const float *b = &verts[tris[i * 3 + 1] * 3];
        const float *c = &verts[tris[i * 3 + 2] * 3];
        double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
        double vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
        area += 0.5 * sqrt(cx * cx + cy * cy + cz * cz);
    }
    if (area <= 1e-12) area = (double)maxext * maxext;

    /* first guess: occupied cells ~ area / cell^2, aim for the target */
    cell = sqrt(area / (double)target_tris);
    if (cell < maxext * 1e-6) cell = maxext * 1e-6;

    for (iter = 0; iter < 12; ++iter) {
        if (!cluster_try(verts, nvert, tris, ntri, cell, org, &ov, &onv, &ot, &ont)) {
            if (errsz) { strncpy(err, "decimation failed", (size_t)errsz - 1); err[errsz - 1] = '\0'; }
            return 0;
        }
        if (ont <= target_tris) break;
        free(ov); free(ot); ov = NULL; ot = NULL;
        cell *= sqrt((double)ont / (double)target_tris) * 1.15;
        if (cell > maxext * 4.0) cell = maxext * 4.0;
    }
    if (!ov) { if (errsz) { strncpy(err, "decimation failed", (size_t)errsz - 1); err[errsz - 1] = '\0'; } return 0; }

    out->verts = ov;
    out->tris = ot;
    out->nvert = onv;
    out->ntri = ont;
    if (errsz) err[0] = '\0';
    return 1;
}
