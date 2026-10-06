/* ============================================================================
 * 3D ASCII Rotator - version 0.1.0
 *
 * A tiny native Windows x64 console application that renders a shaded
 * three-dimensional solid as animated ASCII art.
 *
 * Rendering: signed-distance-field ray marching with per-pixel surface
 * normals, Blinn-Phong (diffuse + specular) shading, a hemispheric ambient
 * term, a fresnel rim light and screen-space-free SDF ambient occlusion.
 * Luminance is mapped onto a 10-level ASCII density ramp. Each character cell
 * is supersampled 2x2 for smoother silhouettes.
 *
 * License: MIT. See LICENSE for the full text. No third-party dependencies.
 * ==========================================================================*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <conio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stego_model.h"
#include "f1_model.h"
#include "companion_model.h"
#include "maus_model.h"
#include "fish_model.h"
#include "kebab_model.h"
#include "berlin_model.h"
#include "china_model.h"

/* ------------------------------------------------------------------ config */

#define APP_NAME    "3D ASCII Rotator"
#define APP_VERSION "0.1.0"

#define DEFAULT_SPEED 15.0   /* degrees per second                  */
#define MIN_SPEED      5.0   /* degrees per second                  */
#define MAX_SPEED     90.0   /* degrees per second                  */
#define SPEED_STEP     5.0   /* degrees per second per key press    */

#define TILT_STEP      5.0   /* degrees per arrow key press         */
#define MAX_TILT      90.0   /* degrees                             */

#define GRAVITY       80.0   /* shatter particle accel, cells/s^2   */
#define RESTITUTION    0.35  /* shatter bounce factor               */
#define PILE_MAX      10     /* max debris height above the floor   */

#define CAM_DIST   4.5       /* camera distance from the origin     */
#define CAM_ELEV  18.0       /* camera elevation above the horizon  */
#define VIEW_HALF  1.55      /* half height of the view plane       */
#define CHAR_ASPECT 0.5      /* console cell width / height         */

#define PI 3.14159265358979323846

#define RAMP    " .,:;~-=+*oO#%@"  /* dark -> bright, 15 levels       */
#define RAMP_N  15

/* ------------------------------------------------------------------- shapes */

typedef enum { SH_CUBE = 0, SH_CYLINDER, SH_DIAMOND, SH_SPHERE,
               SH_STEGO, SH_F1, SH_COMPANION, SH_MAUS,
               SH_FISH, SH_KEBAB, SH_BERLIN, SH_CHINA } Shape;

static Shape        g_shape       = SH_CUBE;
static const char  *g_shape_name  = "cube";
static volatile int g_running     = 1;
static double       g_speed       = DEFAULT_SPEED; /* deg/s */
static double       g_angle       = 0.0;           /* radians */
static double       g_tilt        = 0.0;           /* degrees, + = axis tips away */
static int          g_snapshot    = 0;
static double       g_start_angle = 0.0;           /* degrees */
static double       g_start_tilt  = 0.0;           /* degrees */
static int          g_shatter     = 0;             /* --shatter (snapshot) */
static double       g_sim         = 3.0;           /* --sim seconds        */
static int          g_shattered   = 0;             /* runtime shatter state */

/* ------------------------------------------------------------------ vectors */

typedef struct { double x, y, z; } v3;

static v3 v3_make(double x, double y, double z) { v3 r; r.x = x; r.y = y; r.z = z; return r; }
static v3 v3_add(v3 a, v3 b)   { return v3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
static v3 v3_sub(v3 a, v3 b)   { return v3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
static v3 v3_mul(v3 a, double s){ return v3_make(a.x * s, a.y * s, a.z * s); }
static double v3_dot(v3 a, v3 b){ return a.x * b.x + a.y * b.y + a.z * b.z; }
static double v3_len(v3 a)     { return sqrt(v3_dot(a, a)); }
static v3 v3_norm(v3 a) {
    double l = v3_len(a);
    return (l > 1e-12) ? v3_mul(a, 1.0 / l) : a;
}

/* Rotate a point around the vertical (z) axis by a radians. */
static v3 rot_z(v3 p, double a) {
    double c = cos(a), s = sin(a);
    return v3_make(p.x * c - p.y * s, p.x * s + p.y * c, p.z);
}

/* Rotate a point around the horizontal (x) axis by a radians. */
static v3 rot_x(v3 p, double a) {
    double c = cos(a), s = sin(a);
    return v3_make(p.x, p.y * c - p.z * s, p.y * s + p.z * c);
}

/* ------------------------------------------------------------------ geometry
 * World space: x = right, y = depth (camera on -y looking towards +y),
 * z = vertical. Shapes are centered at the origin. The rotation about the
 * vertical axis is applied by rotating the query point into object space.
 */

/* Map a world point into the object's own frame (undo spin, then tilt). */
static v3 to_object(v3 p) {
    return rot_z(rot_x(p, g_tilt * (PI / 180.0)), -g_angle);
}

/* Map a direction from object space back to world space. */
static v3 from_object(v3 n) {
    return rot_x(rot_z(n, g_angle), -g_tilt * (PI / 180.0));
}

/* ---- mesh shapes: ray traced with a bounding-volume hierarchy ----------- */

typedef struct { float mn[3], mx[3]; int start, count, right; } BvhNode;

typedef struct {
    const float          *verts;
    const unsigned short *tris;
    int    nvert, ntri;
    BvhNode *bvh;
    int     *order;
    float   *cent;
    double   r, hz;   /* rotation-invariant half-extents used for auto-fit */
    double   elev;    /* camera elevation used for this shape              */
    float    bmin[3], bmax[3]; /* object-space bounding box                 */
} MeshDef;

static int g_bvh_n = 0;          /* node counter while building        */
static int g_axis  = 0;          /* split axis while sorting           */
static const MeshDef *g_sort_mesh = NULL;

static const float *tri_p(const MeshDef *m, int tri, int k) {
    int vi = (int)m->tris[tri * 3 + k];
    return &m->verts[(size_t)vi * 3];
}

static void tri_bounds(const MeshDef *m, int tri, float *mn, float *mx) {
    const float *a = tri_p(m, tri, 0), *b = tri_p(m, tri, 1), *c = tri_p(m, tri, 2);
    int i;
    for (i = 0; i < 3; ++i) {
        float lo = a[i], hi = a[i];
        if (b[i] < lo) lo = b[i];
        if (b[i] > hi) hi = b[i];
        if (c[i] < lo) lo = c[i];
        if (c[i] > hi) hi = c[i];
        mn[i] = lo; mx[i] = hi;
    }
}

static int tri_cmp(const void *pa, const void *pb) {
    const MeshDef *m = g_sort_mesh;
    int a = *(const int *)pa, b = *(const int *)pb;
    float ca = m->cent[a * 3 + g_axis], cb = m->cent[b * 3 + g_axis];
    return (ca < cb) ? -1 : (ca > cb) ? 1 : 0;
}

static int bvh_build(MeshDef *m, int lo, int hi) {
    int node = g_bvh_n++;
    float mn[3], mx[3];
    int i, a;
    for (a = 0; a < 3; ++a) { mn[a] = 1e30f; mx[a] = -1e30f; }
    for (i = lo; i < hi; ++i) {
        float tmn[3], tmx[3];
        tri_bounds(m, m->order[i], tmn, tmx);
        for (a = 0; a < 3; ++a) {
            if (tmn[a] < mn[a]) mn[a] = tmn[a];
            if (tmx[a] > mx[a]) mx[a] = tmx[a];
        }
    }
    for (a = 0; a < 3; ++a) { m->bvh[node].mn[a] = mn[a]; m->bvh[node].mx[a] = mx[a]; }
    m->bvh[node].start = lo;
    m->bvh[node].count = hi - lo;
    m->bvh[node].right = -1;
    if (hi - lo <= 4) return node;

    a = 0;
    {
        float best = -1.0f;
        int k;
        for (k = 0; k < 3; ++k) { float e = mx[k] - mn[k]; if (e > best) { best = e; a = k; } }
    }
    g_axis = a;
    g_sort_mesh = m;
    qsort(&m->order[lo], (size_t)(hi - lo), sizeof(int), tri_cmp);
    {
        int mid = (lo + hi) / 2;
        bvh_build(m, lo, mid);
        m->bvh[node].right = bvh_build(m, mid, hi);
    }
    return node;
}


/* Ray versus axis-aligned box; returns 1 and the entry/exit distances. */
static int aabb_hit(v3 ro, v3 rd, const float *mn, const float *mx,
                    double margin, double *tn, double *tf) {
    double t0 = -1e30, t1 = 1e30, o[3], d[3];
    int i;
    o[0] = ro.x; o[1] = ro.y; o[2] = ro.z;
    d[0] = rd.x; d[1] = rd.y; d[2] = rd.z;
    for (i = 0; i < 3; ++i) {
        double lo = (double)mn[i] - margin, hi = (double)mx[i] + margin;
        if (fabs(d[i]) < 1e-9) {
            if (o[i] < lo || o[i] > hi) return 0;
        } else {
            double inv = 1.0 / d[i];
            double ta = (lo - o[i]) * inv, tb = (hi - o[i]) * inv, tmp;
            if (ta > tb) { tmp = ta; ta = tb; tb = tmp; }
            if (ta > t0) t0 = ta;
            if (tb < t1) t1 = tb;
            if (t0 > t1) return 0;
        }
    }
    *tn = t0; *tf = t1;
    return 1;
}

/* Moller-Trumbore ray/triangle intersection (two-sided). */
static int ray_tri(const MeshDef *m, v3 ro, v3 rd, int tri, double *tout) {
    const float *v0 = tri_p(m, tri, 0), *v1 = tri_p(m, tri, 1), *v2 = tri_p(m, tri, 2);
    double e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
    double e2x = v2[0] - v0[0], e2y = v2[1] - v0[1], e2z = v2[2] - v0[2];
    double px = rd.y * e2z - rd.z * e2y;
    double py = rd.z * e2x - rd.x * e2z;
    double pz = rd.x * e2y - rd.y * e2x;
    double det = e1x * px + e1y * py + e1z * pz;
    double inv, u, v, t, qx, qy, qz;
    double tx = ro.x - v0[0], ty = ro.y - v0[1], tz = ro.z - v0[2];
    if (fabs(det) < 1e-12) return 0;
    inv = 1.0 / det;
    u = (tx * px + ty * py + tz * pz) * inv;
    if (u < 0.0 || u > 1.0) return 0;
    qx = ty * e1z - tz * e1y;
    qy = tz * e1x - tx * e1z;
    qz = tx * e1y - ty * e1x;
    v = (rd.x * qx + rd.y * qy + rd.z * qz) * inv;
    if (v < 0.0 || u + v > 1.0) return 0;
    t = (e2x * qx + e2y * qy + e2z * qz) * inv;
    if (t < 1e-4) return 0;
    *tout = t;
    return 1;
}

/* Nearest triangle hit along an object-space ray. */
static int mesh_trace(const MeshDef *m, v3 ro, v3 rd, double *tout, int *triout) {
    int stack[64], sp = 0, best_tri = -1;
    double best = 1e30;
    if (!m || !m->bvh) return 0;
    stack[sp++] = 0;
    while (sp > 0) {
        int ni = stack[--sp];
        BvhNode *nd = &m->bvh[ni];
        double tn, tf;
        if (!aabb_hit(ro, rd, nd->mn, nd->mx, 0.0, &tn, &tf)) continue;
        if (tn > best) continue;
        if (nd->right < 0) {
            int i;
            for (i = nd->start; i < nd->start + nd->count; ++i) {
                double t;
                if (ray_tri(m, ro, rd, m->order[i], &t) && t < best) {
                    best = t; best_tri = m->order[i];
                }
            }
        } else if (sp < 62) {
            stack[sp++] = ni + 1;
            stack[sp++] = nd->right;
        }
    }
    if (best_tri < 0) return 0;
    *tout = best; *triout = best_tri;
    return 1;
}

/* Build the BVH and the fit extents for a mesh. */
static void mesh_init(MeshDef *m) {
    int i;
    if (!m || m->bvh) return;
    m->bvh   = (BvhNode *)malloc(sizeof(BvhNode) * (size_t)(2 * m->ntri + 1));
    m->order = (int *)malloc(sizeof(int) * (size_t)m->ntri);
    m->cent  = (float *)malloc(sizeof(float) * (size_t)m->ntri * 3);
    if (!m->bvh || !m->order || !m->cent) return;
    for (i = 0; i < m->ntri; ++i) {
        const float *a = tri_p(m, i, 0), *b = tri_p(m, i, 1), *c = tri_p(m, i, 2);
        int k;
        m->order[i] = i;
        for (k = 0; k < 3; ++k) m->cent[i * 3 + k] = (a[k] + b[k] + c[k]) / 3.0f;
    }
    g_bvh_n = 0;
    bvh_build(m, 0, m->ntri);

    m->r = 0.0; m->hz = 0.0;
    for (i = 0; i < 3; ++i) { m->bmin[i] = 1e30f; m->bmax[i] = -1e30f; }
    for (i = 0; i < m->nvert; ++i) {
        const float *v = &m->verts[(size_t)i * 3];
        double rr = sqrt((double)v[0] * v[0] + (double)v[1] * v[1]);
        int k;
        if (rr > m->r) m->r = rr;
        if (fabs((double)v[2]) > m->hz) m->hz = fabs((double)v[2]);
        for (k = 0; k < 3; ++k) {
            if (v[k] < m->bmin[k]) m->bmin[k] = v[k];
            if (v[k] > m->bmax[k]) m->bmax[k] = v[k];
        }
    }
}

/* Does the model fit the view at scale s? (perspective, current orientation) */
static int mesh_fits(const MeshDef *m, double s, double hw, double hh) {
    double e = m->elev * (PI / 180.0), ce = cos(e), se = sin(e);
    double D = CAM_DIST;
    int i;
    for (i = 0; i < 8; ++i) {
        v3 c = v3_make((i & 1) ? m->bmax[0] : m->bmin[0],
                       (i & 2) ? m->bmax[1] : m->bmin[1],
                       (i & 4) ? m->bmax[2] : m->bmin[2]);
        v3 W = from_object(c);
        double den = D + s * (W.y * ce - W.z * se);
        double hx, vz;
        if (den <= 0.1) return 0;
        hx = D * s * W.x / den;
        vz = D * s * (W.y * se + W.z * ce) / den;
        if (fabs(hx) > 0.94 * hw) return 0;
        if (fabs(vz) > 0.94 * hh) return 0;
    }
    return 1;
}

/* Largest scale that fits the (perspective) view at the current orientation. */
static double mesh_scale(const MeshDef *m, double aspect) {
    double hw = VIEW_HALF * aspect, hh = VIEW_HALF;
    double lo = 0.0, hi = 8.0;
    int i;
    for (i = 0; i < 40; ++i) {
        double mid = (lo + hi) * 0.5;
        if (mesh_fits(m, mid, hw, hh)) lo = mid; else hi = mid;
    }
    return lo;
}

static MeshDef g_mesh_stego = { STEGO_VERT, STEGO_TRI, STEGO_NVERT, STEGO_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 12.0 };
static MeshDef g_mesh_f1    = { F1_VERT, F1_TRI, F1_NVERT, F1_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 16.0 };
static MeshDef g_mesh_comp  = { COMPANION_VERT, COMPANION_TRI, COMPANION_NVERT, COMPANION_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 18.0 };
static MeshDef g_mesh_maus  = { MAUS_VERT, MAUS_TRI, MAUS_NVERT, MAUS_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 12.0 };
static MeshDef g_mesh_fish  = { FISH_VERT, FISH_TRI, FISH_NVERT, FISH_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 12.0 };
static MeshDef g_mesh_kebab = { KEBAB_VERT, KEBAB_TRI, KEBAB_NVERT, KEBAB_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 14.0 };
static MeshDef g_mesh_berlin = { BERLIN_VERT, BERLIN_TRI, BERLIN_NVERT, BERLIN_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 8.0 };
static MeshDef g_mesh_china = { CHINA_VERT, CHINA_TRI, CHINA_NVERT, CHINA_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 6.0 };

static MeshDef *mesh_for_shape(Shape s) {
    switch (s) {
    case SH_STEGO:     return &g_mesh_stego;
    case SH_F1:        return &g_mesh_f1;
    case SH_COMPANION: return &g_mesh_comp;
    case SH_MAUS:      return &g_mesh_maus;
    case SH_FISH:      return &g_mesh_fish;
    case SH_KEBAB:     return &g_mesh_kebab;
    case SH_BERLIN:    return &g_mesh_berlin;
    case SH_CHINA:     return &g_mesh_china;
    default:           return NULL;
    }
}

static MeshDef *g_mesh = NULL;       /* mesh for the current shape, or NULL */
static double   g_mesh_scale = 1.0;  /* current fit-to-view scale           */

/* All shapes in insertion order; Space cycles through them. */
static const char *SHAPE_NAMES[] = {
    "cube", "cylinder", "diamond", "sphere", "stego", "f1",
    "companion", "maus", "fish", "kebab", "berlin", "china"
};
#define N_SHAPES ((int)(sizeof(SHAPE_NAMES) / sizeof(SHAPE_NAMES[0])))

static void select_shape(Shape s) {
    g_shape = s;
    g_shape_name = SHAPE_NAMES[(int)s];
    g_mesh = mesh_for_shape(s);
    if (g_mesh) mesh_init(g_mesh);
}

static double shape_sdf(v3 q) {
    switch (g_shape) {
    case SH_SPHERE:
        return v3_len(q) - 1.0;

    case SH_CUBE: {
        /* rounded box: soft edges catch highlights nicely */
        const double h = 0.72, r = 0.06;
        double dx = fabs(q.x) - h, dy = fabs(q.y) - h, dz = fabs(q.z) - h;
        double mx = dx > 0 ? dx : 0, my = dy > 0 ? dy : 0, mz = dz > 0 ? dz : 0;
        return sqrt(mx * mx + my * my + mz * mz) - r;
    }

    case SH_CYLINDER: {
        /* capped cylinder aligned with the vertical axis */
        const double r = 0.70, hh = 1.00;
        double dx = sqrt(q.x * q.x + q.y * q.y) - r;
        double dy = fabs(q.z) - hh;
        double mx = dx > 0 ? dx : 0, my = dy > 0 ? dy : 0;
        double outside = sqrt(mx * mx + my * my);
        double inside  = (dx > dy ? dx : dy);
        if (inside > 0) inside = 0;
        return inside + outside;
    }

    case SH_DIAMOND: {
        /* octahedron (|x|+|y|+|z| = s), the classic gem silhouette */
        const double s = 1.20;
        return (fabs(q.x) + fabs(q.y) + fabs(q.z) - s) / 1.7320508075688772;
    }

    case SH_STEGO:
        return 1e9; /* the stego is a triangle mesh, not an implicit surface */
    }
    return 1e9;
}

static double world_sdf(v3 p) {
    /* map into the object frame (tilt the spin axis, then spin) */
    return shape_sdf(to_object(p));
}

static v3 world_normal(v3 p) {
    const double h = 0.0015;
    double dx = world_sdf(v3_add(p, v3_make(h, 0, 0))) - world_sdf(v3_sub(p, v3_make(h, 0, 0)));
    double dy = world_sdf(v3_add(p, v3_make(0, h, 0))) - world_sdf(v3_sub(p, v3_make(0, h, 0)));
    double dz = world_sdf(v3_add(p, v3_make(0, 0, h))) - world_sdf(v3_sub(p, v3_make(0, 0, h)));
    return v3_norm(v3_make(dx, dy, dz));
}

/* Ambient occlusion estimated from the distance field along the normal. */
static double ambient_occlusion(v3 p, v3 n) {
    double occ = 0.0, sca = 1.0;
    int i;
    for (i = 0; i < 5; ++i) {
        double hd = 0.02 + 0.10 * ((double)i / 4.0);
        double d  = world_sdf(v3_add(p, v3_mul(n, hd)));
        occ += (hd - d) * sca;
        sca *= 0.90;
    }
    {
        double ao = 1.0 - 2.6 * occ;
        if (ao < 0.0) ao = 0.0;
        if (ao > 1.0) ao = 1.0;
        return ao;
    }
}

/* ------------------------------------------------------------------- shading */

/* Lambert/Blinn-Phong shading of a surface point. */
static double light_surface(v3 n, v3 view, double ao) {
    v3 light  = v3_norm(v3_make(-0.45, -0.75, 0.55)); /* upper-left, front */
    v3 half   = v3_norm(v3_add(light, view));
    double ndl = v3_dot(n, light);
    double ndh = v3_dot(n, half);
    double ndv = v3_dot(n, view);
    double amb, spec, rim, lum;

    if (ndl < 0.0) ndl = 0.0;
    if (ndh < 0.0) ndh = 0.0;
    if (ndv < 0.0) ndv = 0.0;

    amb  = 0.10 + 0.10 * (0.5 + 0.5 * n.z);   /* sky above, ground below */
    spec = 0.70 * pow(ndh, 60.0);             /* tight Blinn-Phong highlight */
    rim  = 0.22 * pow(1.0 - ndv, 3.0);        /* fresnel edge light */
    lum  = (amb + 0.85 * ndl + spec + rim) * ao;
    if (lum < 0.0) lum = 0.0;
    if (lum > 1.0) lum = 1.0;
    return lum;
}

/* Returns luminance in [0,1] for a single view-plane ray (0 = background). */
static double shade_ray(double hx, double vz) {
    double elev = g_mesh ? g_mesh->elev : CAM_ELEV;
    double e    = elev * (PI / 180.0);
    double ce   = cos(e), se = sin(e);
    v3 ro       = v3_make(0.0, -CAM_DIST * ce, CAM_DIST * se);
    v3 fwd      = v3_make(0.0, ce, -se);
    v3 right    = v3_make(1.0, 0.0, 0.0);
    v3 up       = v3_make(0.0, se, ce);
    v3 rd       = v3_norm(v3_add(v3_add(v3_mul(fwd, CAM_DIST), v3_mul(right, hx)),
                                 v3_mul(up, vz)));
    v3 view     = v3_mul(rd, -1.0);

    if (g_mesh) {
        /* ray trace the embedded triangle mesh (sharp silhouette) */
        double s = g_mesh_scale;
        v3 ro2 = v3_mul(to_object(ro), 1.0 / s);
        v3 rd2 = v3_mul(to_object(rd), 1.0 / s);
        double t;
        int tri;
        v3 n;
        const float *a, *b, *c;
        if (!mesh_trace(g_mesh, ro2, rd2, &t, &tri)) return 0.0;
        a = tri_p(g_mesh, tri, 0); b = tri_p(g_mesh, tri, 1); c = tri_p(g_mesh, tri, 2);
        n = v3_norm(v3_make(
                (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]),
                (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]),
                (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])));
        if (v3_dot(n, rd2) > 0.0) n = v3_mul(n, -1.0);
        n = from_object(n);
        return light_surface(n, view, 1.0);
    }

    {
        v3 p = ro;
        double t = 0.0, t_end = 12.0, stepf = 0.9;
        int hit = 0, i;

        for (i = 0; i < 200; ++i) {
            double d;
            if (t > t_end) break;
            p = v3_add(ro, v3_mul(rd, t));
            d = world_sdf(p);
            if (d < 0.0008) { hit = 1; break; }
            t += d * stepf + 0.0005;
        }
        if (!hit) return 0.0;
        {
            v3 n = world_normal(p);
            return light_surface(n, view, ambient_occlusion(p, n));
        }
    }
}

/* 2x2 supersampled luminance for one character cell. */
static double shade_cell(double cx, double cy, int cols, int rows) {
    double ox = 0.5 / (double)cols;
    double oy = 0.5 / (double)rows;
    double a = shade_ray(cx - ox, cy - oy);
    double b = shade_ray(cx + ox, cy - oy);
    double c = shade_ray(cx - ox, cy + oy);
    double d = shade_ray(cx + ox, cy + oy);
    return (a + b + c + d) * 0.25;
}

/* ----------------------------------------------------------------- rendering */

/* Fill a cols*rows character grid with the current shape. */
static void render_grid(char *grid, int cols, int rows) {
    double aspect = CHAR_ASPECT * (double)cols / (double)rows;
    int r, c;

    g_mesh_scale = g_mesh ? mesh_scale(g_mesh, aspect) : 1.0;

    for (r = 0; r < rows; ++r) {
        double ndc_y = 1.0 - ((double)r + 0.5) / (double)rows * 2.0;
        double vz    = ndc_y * VIEW_HALF;
        for (c = 0; c < cols; ++c) {
            double ndc_x = ((double)c + 0.5) / (double)cols * 2.0 - 1.0;
            double hx    = ndc_x * VIEW_HALF * aspect;
            double lum   = shade_cell(hx, vz, cols, rows);
            int idx      = (int)(lum * (RAMP_N - 1) + 0.5);
            if (idx < 0) idx = 0;
            if (idx >= RAMP_N) idx = RAMP_N - 1;
            grid[(size_t)r * cols + c] = RAMP[idx];
        }
    }
}

/* Overlay the status line in the top-left corner. */
static void draw_hud(char *grid, int cols, int rows) {
    char hud[300];
    int n, i;
    if (g_shattered) {
        _snprintf(hud, sizeof(hud),
                  " %s v%s  |  shape: %s  |  tilt: %d deg   "
                  "[ESC] quit   [ENTER] rebuild   [SPACE] next shape ",
                  APP_NAME, APP_VERSION, g_shape_name, (int)g_tilt);
    } else {
        _snprintf(hud, sizeof(hud),
                  " %s v%s  |  shape: %s  |  spin: %d deg/s  |  tilt: %d deg   "
                  "[ESC] quit  [-/+] speed  [up/down] tilt  [SPACE] next shape  [ENTER] shatter ",
                  APP_NAME, APP_VERSION, g_shape_name, (int)(g_speed + 0.5), (int)g_tilt);
    }
    hud[sizeof(hud) - 1] = '\0';
    n = (int)strlen(hud);
    if (n > cols) n = cols;
    for (i = 0; i < n; ++i) grid[i] = hud[i];
    (void)rows;
}

/* --------------------------------------------------------------- particles
 * When the object is shattered (ENTER / --shatter) every visible character
 * becomes an independent particle that falls under gravity onto the invisible
 * floor the object was resting on. The debris spreads sideways and settles
 * into a shallow layer no higher than PILE_MAX rows.
 */

typedef struct {
    float x, y;    /* screen cell coordinates (x = right, y = down) */
    float vx, vy;  /* cells per second                              */
    char  c;
    int   rest;
} Particle;

static Particle *g_parts     = NULL;
static int       g_part_cap  = 0;
static int       g_part_n    = 0;
static int       g_p_cols    = 0;
static int       g_floor_row = 0;
static int      *g_col_h     = NULL;   /* resting particles per column */
static int       g_col_cap   = 0;

static unsigned int g_rng = 2463534242u;
static void   rng_seed(unsigned int s) { g_rng = s ? s : 1u; }
static double rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (double)g_rng / 4294967296.0;
}

static int particle_room(int need) {
    if (need <= g_part_cap) return 1;
    {
        Particle *p = (Particle *)realloc(g_parts, (size_t)need * sizeof(Particle));
        if (!p) return 0;
        g_parts = p; g_part_cap = need;
    }
    return 1;
}

static int col_h_room(int cols) {
    if (cols <= g_col_cap) return 1;
    {
        int *p = (int *)realloc(g_col_h, (size_t)cols * sizeof(int));
        if (!p) return 0;
        g_col_h = p; g_col_cap = cols;
    }
    return 1;
}

/* Capture the current character frame as falling particles. */
static void shatter_spawn(const char *grid, int cols, int rows) {
    int r, c, maxr = 0;
    if (!particle_room(cols * rows) || !col_h_room(cols)) return;
    g_part_n = 0; g_p_cols = cols;
    for (r = 0; r < rows; ++r) {
        for (c = 0; c < cols; ++c) {
            char ch = grid[(size_t)r * cols + c];
            if (ch == ' ' || ch == '\0') continue;
            {
                Particle *p = &g_parts[g_part_n++];
                p->x  = (float)c;
                p->y  = (float)r;
                p->vx = (float)((rnd() - 0.5) * 9.0);
                p->vy = (float)(-rnd() * 5.0);   /* small upward pop */
                p->c  = ch;
                p->rest = 0;
            }
            if (r > maxr) maxr = r;
        }
    }
    g_floor_row = maxr;
    for (c = 0; c < cols; ++c) g_col_h[c] = 0;
}

static void shatter_step(double dt) {
    int i;
    if (!g_parts) return;
    for (i = 0; i < g_part_n; ++i) {
        Particle *p = &g_parts[i];
        int col, surface;
        if (p->rest) continue;

        p->vy += (float)(GRAVITY * dt);
        p->x  += (float)(p->vx * dt);
        p->y  += (float)(p->vy * dt);

        if (p->x < 0.0f)            { p->x = 0.0f;                    p->vx = -p->vx * 0.5f; }
        if (p->x > g_p_cols - 1.0f) { p->x = (float)(g_p_cols - 1);    p->vx = -p->vx * 0.5f; }

        col = (int)(p->x + 0.5f);
        if (col < 0) col = 0;
        if (col >= g_p_cols) col = g_p_cols - 1;
        surface = g_floor_row - g_col_h[col];

        if (p->y >= surface) {
            if (p->vy > 8.0f) {                  /* fast enough to bounce */
                p->y  = (float)surface;
                p->vy = -p->vy * (float)RESTITUTION;
                p->vx *= 0.6f;
            } else {                             /* settle into the debris layer */
                int target = col, d;
                for (d = 1; d <= 24; ++d) {      /* spill towards a shorter column */
                    int l = col - d, r = col + d;
                    if (l >= 0   && g_col_h[l] < PILE_MAX) { target = l; break; }
                    if (r < g_p_cols && g_col_h[r] < PILE_MAX) { target = r; break; }
                }
                if (g_col_h[target] < PILE_MAX) {
                    p->y = (float)(g_floor_row - g_col_h[target]);
                    g_col_h[target]++;
                } else {                         /* layer full: let it overlap */
                    p->y = (float)(g_floor_row - (int)(rnd() * PILE_MAX));
                }
                p->x  = (float)target;
                p->vy = 0.0f;
                p->vx = 0.0f;
                p->rest = 1;
            }
        }
    }
}

static void shatter_draw(char *grid, int cols, int rows) {
    int i;
    memset(grid, ' ', (size_t)cols * rows);
    if (!g_parts) return;
    for (i = 0; i < g_part_n; ++i) {
        Particle *p = &g_parts[i];
        int c = (int)(p->x + 0.5f);
        int r = (int)(p->y + 0.5f);
        if (c < 0 || c >= cols || r < 0 || r >= rows) continue;
        grid[(size_t)r * cols + c] = p->c;
    }
}

static int console_size(HANDLE h, int *cols, int *rows) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(h, &csbi)) return 0;
    *cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (*cols < 8)  *cols = 8;
    if (*rows < 4)  *rows = 4;
    return 1;
}

/* --------------------------------------------------------------- command line */

static void print_help(void) {
    printf(
        "%s %s\n\n"
        "Render a shaded 3D shape as animated ASCII art in the console.\n\n"
        "Usage:\n"
        "  ascii3d [options]\n\n"
        "Options:\n"
        "  -s, --shape <name>   Shape to render: cube | cylinder | diamond |\n"
        "                       sphere | stego | f1 | companion | maus |\n"
        "                       fish | kebab | berlin | china (default: cube)\n"
        "  -h, --help           Show this help and exit\n"
        "  -v, --version        Show version and exit\n"
        "      --snapshot       Render a single frame to stdout and exit\n"
        "      --angle <deg>    Initial rotation angle in degrees (default: 0)\n"
        "      --tilt <deg>     Initial axis tilt in degrees, -90..90 (default: 0)\n"
        "      --shatter        With --snapshot: shatter and simulate the fall\n"
        "      --sim <sec>      With --shatter: seconds to simulate (default: 3)\n\n"
        "Controls (interactive):\n"
        "  ESC                  Quit\n"
        "  +                    Increase spin by %d deg/s (max %d deg/s)\n"
        "  -                    Decrease spin by %d deg/s (min %d deg/s)\n"
        "  Up / Down            Tilt the spin axis away from / towards the viewer\n"
        "                       in %d deg steps (max %d deg)\n"
        "  ENTER                Shatter the object; press again to rebuild it\n"
        "  SPACE                Switch to the next shape (insertion order)\n"
        "  q                    Quit\n",
        APP_NAME, APP_VERSION,
        (int)SPEED_STEP, (int)MAX_SPEED, (int)SPEED_STEP, (int)MIN_SPEED,
        (int)TILT_STEP, (int)MAX_TILT);
}

static int set_shape(const char *name) {
    struct { const char *key; Shape shape; const char *canon; } table[] = {
        { "cube",       SH_CUBE,     "cube"     },
        { "cubus",      SH_CUBE,     "cube"     },
        { "box",        SH_CUBE,     "cube"     },
        { "cylinder",   SH_CYLINDER, "cylinder" },
        { "cyl",        SH_CYLINDER, "cylinder" },
        { "diamond",    SH_DIAMOND,  "diamond"  },
        { "octahedron", SH_DIAMOND,  "diamond"  },
        { "gem",        SH_DIAMOND,  "diamond"  },
        { "sphere",     SH_SPHERE,   "sphere"   },
        { "ball",       SH_SPHERE,   "sphere"   },
        { "stego",      SH_STEGO,     "stego"     },
        { "stegosaurus",SH_STEGO,     "stego"     },
        { "dinosaur",   SH_STEGO,     "stego"     },
        { "f1",         SH_F1,        "f1"        },
        { "formula",    SH_F1,        "f1"        },
        { "formula1",   SH_F1,        "f1"        },
        { "race",       SH_F1,        "f1"        },
        { "companion",  SH_COMPANION, "companion" },
        { "companioncube", SH_COMPANION, "companion" },
        { "portal",     SH_COMPANION, "companion" },
        { "maus",       SH_MAUS,      "maus"      },
        { "mouse",      SH_MAUS,      "maus"      },
        { "fish",       SH_FISH,      "fish"      },
        { "trout",      SH_FISH,      "fish"      },
        { "forelle",    SH_FISH,      "fish"      },
        { "kebab",      SH_KEBAB,     "kebab"     },
        { "doner",      SH_KEBAB,     "kebab"     },
        { "doener",     SH_KEBAB,     "kebab"     },
        { "berlin",     SH_BERLIN,    "berlin"    },
        { "tor",        SH_BERLIN,    "berlin"    },
        { "brandenburg",SH_BERLIN,    "berlin"    },
        { "gate",       SH_BERLIN,    "berlin"    },
        { "china",      SH_CHINA,     "china"     },
        { "tiananmen",  SH_CHINA,     "china"     },
        { "tankman",    SH_CHINA,     "china"     },
        { "tank",       SH_CHINA,     "china"     }
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (_stricmp(name, table[i].key) == 0) {
            g_shape      = table[i].shape;
            g_shape_name = table[i].canon;
            return 1;
        }
    }
    return 0;
}

/* Returns 1 on success, 0 to stop (help/version already handled). */
static int parse_args(int argc, char **argv) {
    const char *shape = NULL;
    int i;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (_stricmp(a, "-h") == 0 || _stricmp(a, "--help") == 0 || _stricmp(a, "-?") == 0) {
            print_help();
            exit(0);
        } else if (_stricmp(a, "-v") == 0 || _stricmp(a, "--version") == 0) {
            printf("%s %s\n", APP_NAME, APP_VERSION);
            exit(0);
        } else if (_stricmp(a, "--snapshot") == 0) {
            g_snapshot = 1;
        } else if (_stricmp(a, "-s") == 0 || _stricmp(a, "--shape") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: %s requires a value\n", a);
                return 0;
            }
            shape = argv[++i];
        } else if (_strnicmp(a, "--shape=", 8) == 0) {
            shape = a + 8;
        } else if (_stricmp(a, "--angle") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --angle requires a value\n");
                return 0;
            }
            g_start_angle = atof(argv[++i]);
        } else if (_stricmp(a, "--tilt") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --tilt requires a value\n");
                return 0;
            }
            g_start_tilt = atof(argv[++i]);
        } else if (_stricmp(a, "--shatter") == 0) {
            g_shatter = 1;
        } else if (_stricmp(a, "--sim") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --sim requires a value\n");
                return 0;
            }
            g_sim = atof(argv[++i]);
        } else {
            fprintf(stderr, "error: unknown argument '%s' (try --help)\n", a);
            return 0;
        }
    }

    if (shape && !set_shape(shape)) {
        fprintf(stderr,
                "error: unknown shape '%s'\n"
                "       valid shapes: cube, cylinder, diamond, sphere, stego, f1,\n"
                "                     companion, maus, fish, kebab, berlin, china\n", shape);
        return 0;
    }
    return 1;
}

/* --------------------------------------------------------------------- main */

static BOOL WINAPI on_ctrl(DWORD type) {
    (void)type;
    g_running = 0;
    return TRUE;
}

static void write_all(HANDLE h, int is_console, const char *buf, int len) {
    if (len < 0) len = (int)strlen(buf);
    if (is_console) {
        DWORD written = 0;
        WriteConsoleA(h, buf, (DWORD)len, &written, NULL);
    } else {
        fwrite(buf, 1, (size_t)len, stdout);
    }
}

int main(int argc, char **argv) {
    HANDLE h_out;
    DWORD  orig_mode = 0;
    int    is_console, cols = 0, rows = 0;
    char  *grid = NULL, *out = NULL;
    LARGE_INTEGER freq, now, last;
    double angle_deg;

    if (!parse_args(argc, argv)) return 1;

    select_shape(g_shape);

    h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    is_console = GetConsoleMode(h_out, &orig_mode) ? 1 : 0;

    /* ---- snapshot mode: one plain frame, no console required ------------- */
    if (g_snapshot) {
        cols = 100; rows = 40;
        if (is_console) console_size(h_out, &cols, &rows);
        if (g_start_tilt < -MAX_TILT) g_start_tilt = -MAX_TILT;
        if (g_start_tilt >  MAX_TILT) g_start_tilt =  MAX_TILT;
        g_tilt  = g_start_tilt;
        g_angle = g_start_angle * (PI / 180.0);
        grid = (char *)malloc((size_t)cols * rows);
        if (!grid) return 1;
        render_grid(grid, cols, rows);
        if (g_shatter) {
            double t = 0.0;
            rng_seed(12345u);
            shatter_spawn(grid, cols, rows);
            while (t < g_sim) {
                double step = 1.0 / 120.0;
                if (t + step > g_sim) step = g_sim - t;
                shatter_step(step);
                t += step;
            }
            shatter_draw(grid, cols, rows);
        }
        {
            int r;
            for (r = 0; r < rows; ++r) {
                fwrite(grid + (size_t)r * cols, 1, (size_t)cols, stdout);
                fputc('\n', stdout);
            }
        }
        free(grid);
        free(g_parts);
        free(g_col_h);
        return 0;
    }

    /* ---- interactive mode requires a real console ------------------------ */
    if (!is_console) {
        fprintf(stderr,
                "error: this application needs an interactive console.\n"
                "       For a one-off frame use: ascii3d --snapshot -s cube\n");
        return 1;
    }

    /* enable ANSI/VT escape sequences, hide cursor, use the alt screen */
    SetConsoleMode(h_out, orig_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                          ENABLE_PROCESSED_OUTPUT);
    SetConsoleTitleA(APP_NAME " " APP_VERSION " - press ESC to quit");
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    write_all(h_out, is_console, "\x1b[?1049h\x1b[?25l", -1);

    angle_deg = g_start_angle;
    g_tilt = g_start_tilt;
    rng_seed((unsigned int)GetTickCount());
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    while (g_running) {
        double dt;

        /* --- input ---------------------------------------------------- */
        while (_kbhit()) {
            int ch = _getch();
            if (ch == 0 || ch == 0xE0) {          /* extended (arrow) keys */
                int k = _getch();
                if (k == 72) {                     /* up: tilt axis away  */
                    g_tilt += TILT_STEP;
                    if (g_tilt > MAX_TILT) g_tilt = MAX_TILT;
                } else if (k == 80) {              /* down: tilt towards  */
                    g_tilt -= TILT_STEP;
                    if (g_tilt < -MAX_TILT) g_tilt = -MAX_TILT;
                }
                continue;
            }
            if (ch == 27 || ch == 'q' || ch == 'Q') {
                g_running = 0;
            } else if (ch == ' ') {                /* SPACE: next shape */
                g_shattered = 0;
                select_shape((Shape)(((int)g_shape + 1) % N_SHAPES));
            } else if (ch == 13 || ch == 10) {     /* ENTER: shatter / rebuild */
                if (grid && cols > 0) {
                    if (!g_shattered) {
                        render_grid(grid, cols, rows);
                        shatter_spawn(grid, cols, rows);
                        g_shattered = 1;
                    } else {
                        g_shattered = 0;
                    }
                }
            } else if (ch == '+' || ch == '=') {
                g_speed += SPEED_STEP;
                if (g_speed > MAX_SPEED) g_speed = MAX_SPEED;
            } else if (ch == '-' || ch == '_') {
                g_speed -= SPEED_STEP;
                if (g_speed < MIN_SPEED) g_speed = MIN_SPEED;
            }
        }
        if (!g_running) break;

        /* --- console size (may change on resize) ---------------------- */
        {
            int nc = cols, nr = rows;
            console_size(h_out, &nc, &nr);
            if (nc != cols || nr != rows) {
                cols = nc; rows = nr;
                free(grid);
                grid = (char *)malloc((size_t)cols * rows);
                if (!grid) break;
                free(out);
                out = (char *)malloc((size_t)cols * rows + (size_t)rows * 2 + 32);
                if (!out) break;
                g_shattered = 0; /* particle field is tied to the old size */
                write_all(h_out, is_console, "\x1b[2J", -1);
            }
        }
        if (!grid) { Sleep(16); continue; } /* no valid console size yet */

        /* --- time step ------------------------------------------------ */
        QueryPerformanceCounter(&now);
        dt = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        last = now;
        if (dt > 0.10) dt = 0.10;
        if (dt < 0.0)  dt = 0.0;

        /* --- update + render ------------------------------------------ */
        if (!g_shattered) {
            angle_deg += g_speed * dt;
            while (angle_deg >= 360.0) angle_deg -= 360.0;
            g_angle = angle_deg * (PI / 180.0);
            render_grid(grid, cols, rows);
        } else {
            shatter_step(dt);
            shatter_draw(grid, cols, rows);
        }
        draw_hud(grid, cols, rows);

        /* --- present -------------------------------------------------- */
        {
            char *o = out;
            int r;
            *o++ = '\x1b'; *o++ = '['; *o++ = 'H';
            for (r = 0; r < rows; ++r) {
                memcpy(o, grid + (size_t)r * cols, (size_t)cols);
                o += cols;
                if (r < rows - 1) { *o++ = '\r'; *o++ = '\n'; }
            }
            *o = '\0';
            write_all(h_out, is_console, out, (int)(o - out));
        }

        Sleep(16); /* ~60 fps cap; rotation is time-based so this stays smooth */
    }

    /* ---- restore the console ----------------------------------------- */
    write_all(h_out, is_console, "\x1b[0m\x1b[?25h\x1b[?1049l", -1);
    SetConsoleMode(h_out, orig_mode);
    free(grid);
    free(out);
    free(g_parts);
    free(g_col_h);
    return 0;
}
