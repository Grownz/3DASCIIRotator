/* ============================================================================
 * simplify.c - vertex-cluster decimation (LOD).
 *
 * A grid of cell size `cell` is laid over the mesh; every vertex is assigned to
 * a cell, each occupied cell becomes one output vertex (the average of the
 * vertices in it) and faces whose three vertices collapse onto fewer than three
 * distinct cells are dropped. The cell size is adapted until the triangle count
 * is at or below the target, so the overall shape is kept while the triangle
 * count drops.
 *
 * The per-cell grouping uses an LSD radix sort over the packed cell key (O(n))
 * and all working buffers are reused across the adaptation steps.
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

typedef struct {
    VKey         *keys; int kcap;
    VKey         *tmp;  int tcap;
    int          *vmap; int vcap;
    float        *cv;   int cvcap;   /* capacity in floats    */
    unsigned int *ct;   int ctcap;   /* capacity in indices   */
} Scratch;

static void scratch_free(Scratch *s) {
    free(s->keys); free(s->tmp); free(s->vmap); free(s->cv); free(s->ct);
    memset(s, 0, sizeof(*s));
}

static int grow(void **p, int *cap, int need, size_t elem) {
    if (need <= *cap) return 1;
    {
        void *np = realloc(*p, (size_t)need * elem);
        if (!np) return 0;
        *p = np; *cap = need;
    }
    return 1;
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

/* In-place LSD radix sort of `n` keys in `a` (using `tmp` as scratch). */
static void radix_sort_keys(VKey *a, VKey *tmp, int n) {
    int pass, i, count[256];
    for (pass = 0; pass < 8; ++pass) {
        int shift = pass * 8, sum = 0;
        memset(count, 0, sizeof(count));
        for (i = 0; i < n; ++i) count[(a[i].key >> shift) & 255u]++;
        for (i = 0; i < 256; ++i) { int c = count[i]; count[i] = sum; sum += c; }
        for (i = 0; i < n; ++i) {
            unsigned d = (unsigned)((a[i].key >> shift) & 255u);
            tmp[count[d]++] = a[i];
        }
        { VKey *t = a; a = tmp; tmp = t; }
    }
}

/* One clustering pass at a given cell size. Returns 0 on allocation failure.
 * On success *ov/*ot point into the scratch buffers (valid until the next
 * cluster_try or scratch_free call). */
static int cluster_try(Scratch *s, const float *V, int nv,
                       const unsigned int *T, int nt, double cell, const float *org,
                       float **ov, int *onv, unsigned int **ot, int *ont) {
    int i, cnv = 0, cnt = 0;

    if (!grow((void **)&s->keys, &s->kcap, nv, sizeof(VKey))) return 0;
    if (!grow((void **)&s->tmp,  &s->tcap, nv, sizeof(VKey))) return 0;
    if (!grow((void **)&s->vmap, &s->vcap, nv, sizeof(int))) return 0;
    if (!grow((void **)&s->cv,   &s->cvcap, nv * 3, sizeof(float))) return 0;
    if (!grow((void **)&s->ct,   &s->ctcap, nt * 3, sizeof(unsigned int))) return 0;

    for (i = 0; i < nv; ++i) {
        s->keys[i].idx = i;
        s->keys[i].key = cell_key(&V[i * 3], org, cell);
    }
    radix_sort_keys(s->keys, s->tmp, nv);

    for (i = 0; i < nv; ) {
        unsigned long long k = s->keys[i].key;
        double sx = 0, sy = 0, sz = 0;
        int j = i, c;
        while (j < nv && s->keys[j].key == k) {
            const float *p = &V[s->keys[j].idx * 3];
            sx += p[0]; sy += p[1]; sz += p[2];
            ++j;
        }
        c = j - i;
        s->cv[cnv * 3 + 0] = (float)(sx / c);
        s->cv[cnv * 3 + 1] = (float)(sy / c);
        s->cv[cnv * 3 + 2] = (float)(sz / c);
        for (; i < j; ++i) s->vmap[s->keys[i].idx] = cnv;
        ++cnv;
    }

    for (i = 0; i < nt; ++i) {
        unsigned int a = (unsigned int)s->vmap[T[i * 3 + 0]];
        unsigned int b = (unsigned int)s->vmap[T[i * 3 + 1]];
        unsigned int c = (unsigned int)s->vmap[T[i * 3 + 2]];
        if (a == b || b == c || a == c) continue;
        s->ct[cnt * 3 + 0] = a;
        s->ct[cnt * 3 + 1] = b;
        s->ct[cnt * 3 + 2] = c;
        ++cnt;
    }

    if (cnt == 0) return 0;
    *ov = s->cv; *onv = cnv;
    *ot = s->ct; *ont = cnt;
    return 1;
}

int simplify_cluster(const float *verts, int nvert,
                     const unsigned int *tris, int ntri,
                     int target_tris, RawMesh *out, char *err, int errsz) {
    float org[3], mx[3];
    double area = 0.0, maxext, cell;
    int i, iter, ok = 0;
    Scratch S;

    out->verts = NULL; out->tris = NULL; out->nvert = out->ntri = 0;
    memset(&S, 0, sizeof(S));

    if (nvert <= 0 || ntri <= 0) { if (errsz) err[0] = '\0'; return 0; }
    if (target_tris < 4) target_tris = 4;

    /* already small enough: just copy */
    if (ntri <= target_tris) {
        out->verts = (float *)malloc(sizeof(float) * 3 * (size_t)nvert);
        out->tris = (unsigned int *)malloc(sizeof(unsigned int) * 3 * (size_t)ntri);
        if (!out->verts || !out->tris) { free(out->verts); free(out->tris); out->verts = NULL; out->tris = NULL; return 0; }
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

    for (iter = 0; iter < 14; ++iter) {
        float *ov; int onv; unsigned int *ot; int ont;
        if (!cluster_try(&S, verts, nvert, tris, ntri, cell, org, &ov, &onv, &ot, &ont)) {
            if (errsz) { strncpy(err, "decimation failed", (size_t)errsz - 1); err[errsz - 1] = '\0'; }
            scratch_free(&S);
            return 0;
        }
        if (ont <= target_tris) {
            out->verts = (float *)malloc(sizeof(float) * 3 * (size_t)onv);
            out->tris = (unsigned int *)malloc(sizeof(unsigned int) * 3 * (size_t)ont);
            if (!out->verts || !out->tris) {
                free(out->verts); free(out->tris);
                out->verts = NULL; out->tris = NULL;
                if (errsz) { strncpy(err, "out of memory", (size_t)errsz - 1); err[errsz - 1] = '\0'; }
                scratch_free(&S);
                return 0;
            }
            memcpy(out->verts, ov, sizeof(float) * 3 * (size_t)onv);
            memcpy(out->tris, ot, sizeof(unsigned int) * 3 * (size_t)ont);
            out->nvert = onv;
            out->ntri = ont;
            ok = 1;
            break;
        }
        cell *= sqrt((double)ont / (double)target_tris) * 1.10;
        if (cell > maxext * 4.0) cell = maxext * 4.0;
    }
    scratch_free(&S);
    if (!ok) { if (errsz) { strncpy(err, "decimation failed", (size_t)errsz - 1); err[errsz - 1] = '\0'; } return 0; }
    if (errsz) err[0] = '\0';
    return 1;
}
