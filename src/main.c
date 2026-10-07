/* ============================================================================
 * 3D ASCII Rotator - version 0.2.5
 *
 * A tiny native Windows x64 console application that renders a shaded
 * three-dimensional solid as animated ASCII art.
 *
 * Shapes are analytic signed-distance fields (cube, cylinder, diamond, sphere)
 * or triangle meshes: the built-in models and any .stl/.obj/.ply dropped into
 * a "models" folder next to the executable, which is rescanned live.
 *
 * Meshes are ray traced with a bounding-volume hierarchy and shaded flat;
 * analytic shapes use SDF ray marching. Luminance is mapped onto a 15-level
 * ASCII density ramp. Cells are supersampled 2x2 (1x for very heavy meshes).
 *
 * License: MIT. See LICENSE for the full text. No third-party dependencies.
 * ==========================================================================*/

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#include "loader.h"
#include "simplify.h"

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
#define APP_VERSION "0.2.5"

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

#define MAX_TRIS     500000  /* soft cap for loaded models          */
#define MAX_LOAD_TRIS 5000000 /* hard reader cap before LOD          */
#define SUPER_TRIS   100000  /* above this, single sample per cell  */
#define SUPER_TH       0.03  /* edge threshold for adaptive 2x2     */
#define MAX_MODELS   32      /* max user models kept                */
#define BVH_CACHE     3      /* BVHs kept alive (LRU)               */

#define PI 3.14159265358979323846

#define RAMP    " .,:;~-=+*oO#%@"  /* dark -> bright, 15 levels       */
#define RAMP_N  15

/* ------------------------------------------------------------------- shapes */

typedef enum { SH_CUBE = 0, SH_CYLINDER, SH_DIAMOND, SH_SPHERE } Shape;

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
static char         g_req_shape[64] = "";          /* -s value, resolved later */
static int          g_req_shape_set = 0;
static char         g_model_msg[160] = "";         /* last model load message  */

/* model menu (right-hand list toggled with Tab) */
static int          g_menu_open = 0;
static int          g_menu_sel = 0;
static int          g_menu_scroll = 0;
static int          g_menu_start = 0;              /* --menu (snapshot preview) */
static int          g_lod_arg = -1;                /* --lod level (snapshot) */
static int          g_bench = 0;                   /* --bench <frames>      */
static int          g_target_fps = 60;             /* --fps (0 = unlimited) */

/* --- status message (auto-hides), FPS HUD, shape colour, menu slider ----- */
static int    g_show_fps = 0;
static double g_fps = 0.0;
static double g_msg_time = 0.0;                    /* seconds left to show msg */
static int    g_fg_color = 7;                      /* ANSI colour of the shape */
static int    g_color_sel = 0;                     /* index into COLORS[]      */
static short *g_cc = NULL;                         /* per-cell colour override */
static int    g_cc_cap = 0;
static double *g_lum = NULL;                       /* per-cell luminance buffer */
static int    g_lum_cap = 0;

static void lum_ensure(int n) {
    if (n > g_lum_cap) {
        double *p = (double *)realloc(g_lum, sizeof(double) * (size_t)n);
        if (p) { g_lum = p; g_lum_cap = n; }
    }
}
static int    g_hud_len = 0, g_msg_len = 0, g_fps_len = 0, g_menu_x0 = -1, g_rows = 0;

/* Usable 256-colour indices: everything except black (0) and the five
 * darkest greys (232..236). */
static int COLORS[300];
static int COLOR_N = 0;

static void colors_init(void) {
    int i;
    COLOR_N = 0;
    for (i = 0; i < 256; ++i) {
        if (i == 0) continue;                 /* black */
        if (i >= 232 && i <= 236) continue;   /* five darkest greys */
        COLORS[COLOR_N++] = i;
    }
}

static void msg_setf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(g_model_msg, sizeof(g_model_msg) - 1, fmt, ap);
    va_end(ap);
    g_model_msg[sizeof(g_model_msg) - 1] = '\0';
    g_msg_time = 5.0;
}

static void cc_ensure(int n) {
    if (n > g_cc_cap) {
        short *p = (short *)realloc(g_cc, sizeof(short) * (size_t)n);
        if (p) { g_cc = p; g_cc_cap = n; }
    }
}

/* Is this character cell part of the UI (HUD / message / FPS / menu)? */
static int is_ui_cell(int r, int c) {
    if (r == 0) return c < g_hud_len;
    if (r == 1 && c < g_msg_len) return 1;
    if (g_show_fps && r == g_rows - 1 && c < g_fps_len) return 1;
    if (g_menu_open && g_menu_x0 >= 0 && c >= g_menu_x0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ vectors */

typedef struct { double x, y, z; } v3;

static v3 v3_make(double x, double y, double z) { v3 r; r.x = x; r.y = y; r.z = z; return r; }
static v3 v3_add(v3 a, v3 b)   { return v3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
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

/* Per-frame transform cache: cos/sin of the spin angle and tilt plus the
 * camera basis are computed once per frame so the hot per-ray / per-SDF code
 * never calls the trig functions. */
static double g_ca = 1.0, g_sa = 0.0, g_ct = 1.0, g_st = 0.0;
static v3     g_cam_ro, g_cam_fwd, g_cam_right, g_cam_up;

static void update_transform(void) {
    double t = g_tilt * (PI / 180.0);
    g_ca = cos(g_angle); g_sa = sin(g_angle);
    g_ct = cos(t);       g_st = sin(t);
}

/* Map a world point into the object's own frame (undo spin, then tilt). */
static v3 to_object(v3 p) {
    double qx = p.x, qy = p.y * g_ct - p.z * g_st, qz = p.y * g_st + p.z * g_ct;
    return v3_make(qx * g_ca + qy * g_sa, -qx * g_sa + qy * g_ca, qz);
}

/* Map a direction from object space back to world space. */
static v3 from_object(v3 n) {
    double qx = n.x * g_ca - n.y * g_sa, qy = n.x * g_sa + n.y * g_ca, qz = n.z;
    return v3_make(qx, qy * g_ct + qz * g_st, -qy * g_st + qz * g_ct);
}

/* ---- mesh shapes: ray traced with a bounding-volume hierarchy ----------- */

typedef struct { float mn[3], mx[3]; int start, count, right, axis; } BvhNode;

typedef struct {
    const float        *verts;
    const unsigned int *tris;
    int    nvert, ntri;
    BvhNode *bvh;
    int     *order;
    float   *cent;
    double   r, hz;   /* rotation-invariant half-extents used for auto-fit */
    double   elev;    /* camera elevation used for this shape              */
    double   zoom;    /* multiplier on the auto-fit scale                  */
    float   *tdata;   /* ntri*9: the three vertices, packed per triangle   */
    float    bmin[3], bmax[3]; /* object-space bounding box                 */
    int      owned;   /* 1 = verts/tris are malloc'd (free on destroy)     */
    int      built;   /* 1 = BVH is built                                  */
    /* optional LOD source (kept when the mesh had too many triangles) */
    const float        *src_verts;
    const unsigned int *src_tris;
    int      src_nvert, src_ntri;
    int      lod_level;
} MeshDef;

static int g_bvh_n = 0;          /* node counter while building        */
static int g_axis  = 0;          /* split axis while sorting           */
static const MeshDef *g_sort_mesh = NULL;

static const float *tri_p(const MeshDef *m, int tri, int k) {
    unsigned int vi = m->tris[tri * 3 + k];
    return &m->verts[(size_t)vi * 3];
}

static void tri_bounds(const MeshDef *m, int tri, float *mn, float *mx) {
    const float *t = &m->tdata[(size_t)tri * 9];
    const float *a = t, *b = t + 3, *c = t + 6;
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
    m->bvh[node].axis = a;
    g_sort_mesh = m;
    qsort(&m->order[lo], (size_t)(hi - lo), sizeof(int), tri_cmp);
    {
        int mid = (lo + hi) / 2;
        bvh_build(m, lo, mid);
        m->bvh[node].right = bvh_build(m, mid, hi);
    }
    return node;
}


/* Ray versus axis-aligned box (float, inverse direction precomputed). */
static int aabb_hit(const float *o, const float *d, const float *inv,
                    const float *mn, const float *mx, float *tn, float *tf) {
    float t0 = -1e30f, t1 = 1e30f;
    int i;
    for (i = 0; i < 3; ++i) {
        float ta, tb, tmp;
        if (d[i] == 0.0f) {
            if (o[i] < mn[i] || o[i] > mx[i]) return 0;
            continue;
        }
        ta = (mn[i] - o[i]) * inv[i];
        tb = (mx[i] - o[i]) * inv[i];
        if (ta > tb) { tmp = ta; ta = tb; tb = tmp; }
        if (ta > t0) t0 = ta;
        if (tb < t1) t1 = tb;
        if (t0 > t1) return 0;
    }
    *tn = t0; *tf = t1;
    return 1;
}

/* Moller-Trumbore ray/triangle intersection (two-sided, float). */
static int ray_tri(const MeshDef *m, const float *ro, const float *rd, int tri, float *tout) {
    const float *v0 = &m->tdata[(size_t)tri * 9], *v1 = v0 + 3, *v2 = v0 + 6;
    float e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
    float e2x = v2[0] - v0[0], e2y = v2[1] - v0[1], e2z = v2[2] - v0[2];
    float px = rd[1] * e2z - rd[2] * e2y;
    float py = rd[2] * e2x - rd[0] * e2z;
    float pz = rd[0] * e2y - rd[1] * e2x;
    float det = e1x * px + e1y * py + e1z * pz;
    float inv, u, v, t, qx, qy, qz;
    float tx = ro[0] - v0[0], ty = ro[1] - v0[1], tz = ro[2] - v0[2];
    if (fabsf(det) < 1e-12f) return 0;
    inv = 1.0f / det;
    u = (tx * px + ty * py + tz * pz) * inv;
    if (u < 0.0f || u > 1.0f) return 0;
    qx = ty * e1z - tz * e1y;
    qy = tz * e1x - tx * e1z;
    qz = tx * e1y - ty * e1x;
    v = (rd[0] * qx + rd[1] * qy + rd[2] * qz) * inv;
    if (v < 0.0f || u + v > 1.0f) return 0;
    t = (e2x * qx + e2y * qy + e2z * qz) * inv;
    if (t < 1e-4f) return 0;
    *tout = t;
    return 1;
}

/* Nearest triangle hit along an object-space ray (float, near child first). */
static int mesh_trace(const MeshDef *m, v3 ro, v3 rd, double *tout, int *triout) {
    int stack[64], sp = 0, best_tri = -1;
    float best = 1e30f;
    float rof[3], rdf[3], inv[3];
    int k;
    if (!m || !m->bvh) return 0;
    rof[0] = (float)ro.x; rof[1] = (float)ro.y; rof[2] = (float)ro.z;
    rdf[0] = (float)rd.x; rdf[1] = (float)rd.y; rdf[2] = (float)rd.z;
    for (k = 0; k < 3; ++k) inv[k] = (rdf[k] != 0.0f) ? 1.0f / rdf[k] : 0.0f;
    stack[sp++] = 0;
    while (sp > 0) {
        int ni = stack[--sp];
        const BvhNode *nd = &m->bvh[ni];
        float tn, tf;
        if (!aabb_hit(rof, rdf, inv, nd->mn, nd->mx, &tn, &tf)) continue;
        if (tn > best) continue;
        if (nd->right < 0) {
            int i;
            for (i = nd->start; i < nd->start + nd->count; ++i) {
                float t;
                if (ray_tri(m, rof, rdf, m->order[i], &t) && t < best) {
                    best = t; best_tri = m->order[i];
                }
            }
        } else if (sp < 62) {
            int left = ni + 1, right = nd->right;
            if (rdf[nd->axis] >= 0.0f) { stack[sp++] = right; stack[sp++] = left; }
            else                        { stack[sp++] = left;  stack[sp++] = right; }
        }
    }
    if (best_tri < 0) return 0;
    *tout = (double)best; *triout = best_tri;
    return 1;
}

/* Release the BVH/centroid data of a mesh (keeps verts/tris). */
static void mesh_release(MeshDef *m) {
    if (!m) return;
    free(m->bvh);  m->bvh = NULL;
    free(m->order); m->order = NULL;
    free(m->cent); m->cent = NULL;
    free(m->tdata); m->tdata = NULL;
    m->built = 0;
}

/* Build the BVH and the fit extents for a mesh. */
static void mesh_build(MeshDef *m) {
    int i;
    if (!m || m->built || m->ntri <= 0) return;
    m->bvh   = (BvhNode *)malloc(sizeof(BvhNode) * (size_t)(2 * m->ntri + 1));
    m->order = (int *)malloc(sizeof(int) * (size_t)m->ntri);
    m->cent  = (float *)malloc(sizeof(float) * (size_t)m->ntri * 3);
    m->tdata = (float *)malloc(sizeof(float) * 9 * (size_t)m->ntri);
    if (!m->bvh || !m->order || !m->cent || !m->tdata) { mesh_release(m); return; }
    for (i = 0; i < m->ntri; ++i) {
        const float *a = tri_p(m, i, 0), *b = tri_p(m, i, 1), *c = tri_p(m, i, 2);
        float *t = &m->tdata[(size_t)i * 9];
        int k;
        for (k = 0; k < 3; ++k) {
            t[k]     = a[k];   /* pack the three vertices together so ray/tri */
            t[3 + k] = b[k];   /* tests stay in one cache line                */
            t[6 + k] = c[k];
            m->cent[i * 3 + k] = (a[k] + b[k] + c[k]) / 3.0f;
        }
        m->order[i] = i;
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
    m->built = 1;
}

/* Fully destroy a mesh (BVH + owned verts/tris + the struct). */
static void mesh_destroy(MeshDef *m) {
    if (!m) return;
    mesh_release(m);
    if (m->owned) {
        free((void *)m->verts);
        free((void *)m->tris);
        free((void *)m->src_verts);
        free((void *)m->src_tris);
    }
    free(m);
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
    return lo * (m->zoom > 0.01 ? m->zoom : 1.0);
}

static MeshDef g_mesh_stego = { STEGO_VERT, STEGO_TRI, STEGO_NVERT, STEGO_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 12.0, 1.0 };
static MeshDef g_mesh_f1    = { F1_VERT, F1_TRI, F1_NVERT, F1_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 16.0, 1.0 };
static MeshDef g_mesh_comp  = { COMPANION_VERT, COMPANION_TRI, COMPANION_NVERT, COMPANION_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 18.0, 1.0 };
static MeshDef g_mesh_maus  = { MAUS_VERT, MAUS_TRI, MAUS_NVERT, MAUS_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 12.0, 1.0 };
static MeshDef g_mesh_fish  = { FISH_VERT, FISH_TRI, FISH_NVERT, FISH_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 12.0, 1.0 };
static MeshDef g_mesh_kebab = { KEBAB_VERT, KEBAB_TRI, KEBAB_NVERT, KEBAB_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 14.0, 1.0 };
static MeshDef g_mesh_berlin = { BERLIN_VERT, BERLIN_TRI, BERLIN_NVERT, BERLIN_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 8.0, 1.0 };
static MeshDef g_mesh_china = { CHINA_VERT, CHINA_TRI, CHINA_NVERT, CHINA_NTRI,
                                NULL, NULL, NULL, 0.0, 0.0, 6.0, 1.0 };

/* ---- registry: built-in shapes + user models, in insertion order --------- */

typedef struct {
    char     name[64];
    int      analytic;   /* SH_* for analytic shapes, -1 for meshes */
    int      model;      /* index into the user-model list, or -1   */
    MeshDef *mesh;       /* mesh of the item, or NULL               */
} ShapeItem;

/* defined in the user-models section below */
static MeshDef *model_load(int idx);

static ShapeItem *g_items = NULL;
static int        g_item_count = 0, g_item_cap = 0, g_item_index = 0;

static MeshDef *g_mesh = NULL;        /* mesh of the current item, or NULL */
static double   g_mesh_scale = 1.0;   /* current fit-to-view scale         */
static int      g_super = 1;          /* 1 = 2x2 supersample, 0 = single   */
static int      g_analytic = SH_CUBE; /* analytic kind when g_mesh == NULL */
static char     g_shape_name_buf[64] = "cube";
static const char *g_shape_name = g_shape_name_buf;

/* ---- LRU cache of built BVHs (only the last BVH_CACHE are kept) ---------- */

static MeshDef *g_lru[BVH_CACHE];
static int      g_lru_n = 0;

static void lru_touch(MeshDef *m) {
    int i, j;
    for (i = 0; i < g_lru_n; ++i)
        if (g_lru[i] == m) { for (j = i; j < g_lru_n - 1; ++j) g_lru[j] = g_lru[j + 1]; --g_lru_n; break; }
    if (!m->built) mesh_build(m);
    if (g_lru_n < BVH_CACHE) {
        g_lru[g_lru_n++] = m;
    } else {
        mesh_release(g_lru[0]);
        for (j = 0; j < BVH_CACHE - 1; ++j) g_lru[j] = g_lru[j + 1];
        g_lru[BVH_CACHE - 1] = m;
    }
}

static void lru_clear(void) {
    int i;
    for (i = 0; i < g_lru_n; ++i) mesh_release(g_lru[i]);
    g_lru_n = 0;
}

/* ---- item list ---------------------------------------------------------- */

static int items_add(const char *name, int analytic, MeshDef *mesh) {
    if (g_item_count == g_item_cap) {
        int nc = g_item_cap ? g_item_cap * 2 : 32;
        ShapeItem *np = (ShapeItem *)realloc(g_items, (size_t)nc * sizeof(ShapeItem));
        if (!np) return -1;
        g_items = np; g_item_cap = nc;
    }
    {
        ShapeItem *it = &g_items[g_item_count];
        strncpy(it->name, name, sizeof(it->name) - 1);
        it->name[sizeof(it->name) - 1] = '\0';
        it->analytic = analytic;
        it->model = -1;
        it->mesh = mesh;
    }
    return g_item_count++;
}

static int find_item(const char *name) {
    int i;
    for (i = 0; i < g_item_count; ++i)
        if (_stricmp(g_items[i].name, name) == 0) return i;
    return -1;
}

/* Map a CLI alias to the canonical built-in name (or pass the name through). */
static const char *canonical_name(const char *name) {
    static const struct { const char *key, *canon; } t[] = {
        { "cube", "cube" }, { "cubus", "cube" }, { "box", "cube" },
        { "cylinder", "cylinder" }, { "cyl", "cylinder" },
        { "diamond", "diamond" }, { "octahedron", "diamond" }, { "gem", "diamond" },
        { "sphere", "sphere" }, { "ball", "sphere" },
        { "stego", "stego" }, { "stegosaurus", "stego" }, { "dinosaur", "stego" },
        { "f1", "f1" }, { "formula", "f1" }, { "formula1", "f1" }, { "race", "f1" },
        { "companion", "companion" }, { "companioncube", "companion" }, { "portal", "companion" },
        { "maus", "maus" }, { "mouse", "maus" },
        { "fish", "fish" }, { "trout", "fish" }, { "forelle", "fish" },
        { "kebab", "kebab" }, { "doner", "kebab" }, { "doener", "kebab" },
        { "berlin", "berlin" }, { "tor", "berlin" }, { "brandenburg", "berlin" }, { "gate", "berlin" },
        { "china", "china" }, { "tiananmen", "china" }, { "tankman", "china" }, { "tank", "china" }
    };
    size_t i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); ++i)
        if (_stricmp(name, t[i].key) == 0) return t[i].canon;
    return name;
}

static void select_index(int i) {
    ShapeItem *it;
    if (g_item_count == 0) return;
    i %= g_item_count;
    if (i < 0) i += g_item_count;
    g_item_index = i;
    it = &g_items[i];
    strncpy(g_shape_name_buf, it->name, sizeof(g_shape_name_buf) - 1);
    g_shape_name_buf[sizeof(g_shape_name_buf) - 1] = '\0';
    g_shattered = 0;
    if (it->model >= 0) {
        MeshDef *m = it->mesh ? it->mesh : model_load(it->model);
        it->mesh = m;
        g_analytic = -1;
        g_mesh = m;
        if (m) lru_touch(m);
    } else if (it->mesh) {
        g_analytic = -1; g_mesh = it->mesh; lru_touch(it->mesh);
    } else {
        g_analytic = it->analytic; g_mesh = NULL;
    }
}

static int select_by_name(const char *name) {
    int i = find_item(canonical_name(name));
    if (i < 0) return 0;
    select_index(i);
    return 1;
}

static void build_registry(void) {
    items_add("cube",      SH_CUBE,     NULL);
    items_add("cylinder",  SH_CYLINDER, NULL);
    items_add("diamond",   SH_DIAMOND,  NULL);
    items_add("sphere",    SH_SPHERE,   NULL);
    items_add("stego",      -1, &g_mesh_stego);
    items_add("f1",         -1, &g_mesh_f1);
    items_add("companion",  -1, &g_mesh_comp);
    items_add("maus",       -1, &g_mesh_maus);
    items_add("fish",       -1, &g_mesh_fish);
    items_add("kebab",      -1, &g_mesh_kebab);
    items_add("berlin",     -1, &g_mesh_berlin);
    items_add("china",      -1, &g_mesh_china);
}

static double shape_sdf(v3 q) {
    switch (g_analytic) {
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
    }
    return 1e9;
}

static double world_sdf(v3 p) {
    /* map into the object frame (tilt the spin axis, then spin) */
    return shape_sdf(to_object(p));
}

/* Surface normal from the SDF gradient. The tetrahedron technique needs only
 * four samples instead of the six of central differences, with the same
 * (first-order) accuracy. */
static v3 world_normal(v3 p) {
    const double h = 0.0015;
    const double kx[4] = { 1.0, -1.0, -1.0, 1.0 };
    const double ky[4] = { -1.0, -1.0, 1.0, 1.0 };
    const double kz[4] = { -1.0, 1.0, -1.0, 1.0 };
    double nx = 0.0, ny = 0.0, nz = 0.0;
    int i;
    for (i = 0; i < 4; ++i) {
        double d = world_sdf(v3_make(p.x + kx[i] * h, p.y + ky[i] * h, p.z + kz[i] * h));
        nx += kx[i] * d;
        ny += ky[i] * d;
        nz += kz[i] * d;
    }
    return v3_norm(v3_make(nx, ny, nz));
}

/* Ambient occlusion estimated from the distance field along the normal. */
static double ambient_occlusion(v3 p, v3 n) {
    double occ = 0.0, sca = 1.0;
    int i;
    for (i = 0; i < 4; ++i) {
        double hd = 0.02 + 0.12 * ((double)i / 3.0);
        double d  = world_sdf(v3_add(p, v3_mul(n, hd)));
        occ += (hd - d) * sca;
        sca *= 0.90;
        if (sca < 0.05) break;
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

/* Camera basis for the current shape; recomputed once per frame. */
static void update_camera(void) {
    double elev = g_mesh ? g_mesh->elev : CAM_ELEV;
    double e = elev * (PI / 180.0);
    double ce = cos(e), se = sin(e);
    g_cam_ro    = v3_make(0.0, -CAM_DIST * ce, CAM_DIST * se);
    g_cam_fwd   = v3_make(0.0, ce, -se);
    g_cam_right = v3_make(1.0, 0.0, 0.0);
    g_cam_up    = v3_make(0.0, se, ce);
}

/* Returns luminance in [0,1] for a single view-plane ray (0 = background). */
static double shade_ray(double hx, double vz) {
    v3 ro   = g_cam_ro;
    v3 rd   = v3_norm(v3_add(v3_add(v3_mul(g_cam_fwd, CAM_DIST), v3_mul(g_cam_right, hx)),
                             v3_mul(g_cam_up, vz)));
    v3 view = v3_mul(rd, -1.0);

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
        a = &g_mesh->tdata[(size_t)tri * 9]; b = a + 3; c = a + 6;
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

/* 2x2 supersampled luminance for one character cell (1x for heavy meshes). */
static double shade_cell(double cx, double cy, int cols, int rows) {
    double ox, oy, a, b, c, d;
    if (!g_super) return shade_ray(cx, cy);
    ox = 0.5 / (double)cols;
    oy = 0.5 / (double)rows;
    a = shade_ray(cx - ox, cy - oy);
    b = shade_ray(cx + ox, cy - oy);
    c = shade_ray(cx - ox, cy + oy);
    d = shade_ray(cx + ox, cy + oy);
    return (a + b + c + d) * 0.25;
}

/* --------------------------------------------------------------- worker pool
 * A small pool of persistent threads that block on an event between frames
 * (no spin-wait, so the CPU stays idle when nothing is drawn) and split each
 * render across the cores. Unlike OpenMP this adds no runtime DLL dependency.
 */
typedef void (*RowFn)(int r0, int r1, void *ctx);

typedef struct { HANDLE start, done; int r0, r1; } PoolWorker;

static PoolWorker   *g_pool = NULL;
static int           g_pool_n = 0;
static RowFn         g_job_fn = NULL;
static void         *g_job_ctx = NULL;
static volatile LONG g_pool_quit = 0;

static DWORD WINAPI pool_thread(LPVOID p) {
    PoolWorker *w = (PoolWorker *)p;
    for (;;) {
        WaitForSingleObject(w->start, INFINITE);
        if (g_pool_quit) { SetEvent(w->done); return 0; }
        g_job_fn(w->r0, w->r1, g_job_ctx);
        SetEvent(w->done);
    }
}

static void pool_ensure(void) {
    static int tried = 0;
    int cores, i;
    if (tried) return;
    tried = 1;
    cores = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (cores < 1) cores = 1;
    if (cores > 16) cores = 16;
    if (cores < 2) return;
    g_pool = (PoolWorker *)calloc((size_t)(cores - 1), sizeof(PoolWorker));
    if (!g_pool) return;
    for (i = 0; i < cores - 1; ++i) {
        HANDLE h;
        g_pool[i].start = CreateEventW(NULL, FALSE, FALSE, NULL);
        g_pool[i].done  = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!g_pool[i].start || !g_pool[i].done) break;
        h = CreateThread(NULL, 0, pool_thread, &g_pool[i], 0, NULL);
        if (!h) break;
        CloseHandle(h);
    }
    g_pool_n = i;
}

/* Run `fn` over the rows, splitting them across the pool; the caller also
 * renders one share so all cores stay busy during a frame. */
static void pool_run(RowFn fn, void *ctx, int rows) {
    int i, parts, n = g_pool_n;
    if (n <= 0 || rows <= 0) { fn(0, rows, ctx); return; }
    parts = n + 1;
    for (i = 0; i < n; ++i) {
        g_pool[i].r0 = rows * i / parts;
        g_pool[i].r1 = rows * (i + 1) / parts;
    }
    g_job_fn = fn; g_job_ctx = ctx;
    for (i = 0; i < n; ++i) SetEvent(g_pool[i].start);
    fn(rows * n / parts, rows, ctx);
    for (i = 0; i < n; ++i) WaitForSingleObject(g_pool[i].done, INFINITE);
}

/* ----------------------------------------------------------------- rendering */

/* Map a luminance to a ramp character. */
static char lum_char(double lum) {
    int idx = (int)(lum * (RAMP_N - 1) + 0.5);
    if (idx < 0) idx = 0;
    if (idx >= RAMP_N) idx = RAMP_N - 1;
    return RAMP[idx];
}

/* Per-frame render context handed to the worker pool. `pass` selects the
 * sampling strategy (see render_grid). */
typedef struct {
    char  *grid;
    int    cols, rows;
    double aspect;
    int    pass;   /* 0 = 1x, 3 = fixed 2x2 (mesh), 1/2 = adaptive passes */
} RenderCtx;

static void render_rows(int r0, int r1, void *vctx) {
    RenderCtx *c = (RenderCtx *)vctx;
    int r, cc;
    for (r = r0; r < r1; ++r) {
        double vz = (1.0 - ((double)r + 0.5) / (double)c->rows * 2.0) * VIEW_HALF;
        for (cc = 0; cc < c->cols; ++cc) {
            double hx = (((double)cc + 0.5) / (double)c->cols * 2.0 - 1.0) * VIEW_HALF * c->aspect;
            size_t idx = (size_t)r * c->cols + cc;
            double L;
            if (c->pass == 0) {
                c->grid[idx] = lum_char(shade_ray(hx, vz));
            } else if (c->pass == 3) {
                c->grid[idx] = lum_char(shade_cell(hx, vz, c->cols, c->rows));
            } else if (c->pass == 1) {
                L = shade_ray(hx, vz);
                if (g_lum) g_lum[idx] = L;
                c->grid[idx] = lum_char(L);
            } else {                                     /* pass 2: adaptive edges */
                L = g_lum[idx];
                if ((cc > 0               && fabs(L - g_lum[idx - 1]) > SUPER_TH) ||
                    (cc < c->cols - 1     && fabs(L - g_lum[idx + 1]) > SUPER_TH) ||
                    (r > 0                && fabs(L - g_lum[idx - c->cols]) > SUPER_TH) ||
                    (r < c->rows - 1      && fabs(L - g_lum[idx + c->cols]) > SUPER_TH))
                    L = shade_cell(hx, vz, c->cols, c->rows);
                c->grid[idx] = lum_char(L);
            }
        }
    }
}

/* Fill a cols*rows character grid with the current shape. */
static void render_grid(char *grid, int cols, int rows) {
    double aspect = CHAR_ASPECT * (double)cols / (double)rows;
    RenderCtx ctx;

    pool_ensure();
    update_transform();
    update_camera();
    g_mesh_scale = g_mesh ? mesh_scale(g_mesh, aspect) : 1.0;
    g_super = (g_mesh && g_mesh->ntri > SUPER_TRIS) ? 0 : 1;

    ctx.grid = grid; ctx.cols = cols; ctx.rows = rows; ctx.aspect = aspect;

    if (!g_super) {                       /* heavy mesh: one sample per cell */
        ctx.pass = 0;
        pool_run(render_rows, &ctx, rows);
        return;
    }
    if (g_mesh) {                         /* small mesh: fixed 2x2 */
        ctx.pass = 3;
        pool_run(render_rows, &ctx, rows);
        return;
    }

    lum_ensure(cols * rows);
    if (!g_lum) {                         /* allocation failed: plain 1x */
        ctx.pass = 0;
        pool_run(render_rows, &ctx, rows);
        return;
    }

    /* analytic SDF: adaptive 2x2 (pass 1 = 1 sample, pass 2 = edges only) */
    ctx.pass = 1;
    pool_run(render_rows, &ctx, rows);
    ctx.pass = 2;
    pool_run(render_rows, &ctx, rows);
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
    g_hud_len = n;

    g_msg_len = 0;
    if (g_model_msg[0]) {                 /* last message on line 2 */
        int len = (int)strlen(g_model_msg);
        if (len > cols) len = cols;
        if (rows > 1) { for (i = 0; i < len; ++i) grid[cols + i] = g_model_msg[i]; g_msg_len = len; }
    }

    g_fps_len = 0;
    if (g_show_fps) {                     /* FPS display, bottom-left */
        char fb[32];
        int len;
        _snprintf(fb, sizeof(fb), "FPS: %d", (int)(g_fps + 0.5));
        fb[sizeof(fb) - 1] = '\0';
        len = (int)strlen(fb);
        if (len > cols) len = cols;
        for (i = 0; i < len; ++i) grid[(size_t)(rows - 1) * cols + i] = fb[i];
        g_fps_len = len;
    }
    g_rows = rows;
}

/* Right-hand model list (Tab): about a quarter of the width, top to bottom. */
static void draw_menu(char *grid, int cols, int rows) {
    int pw = cols / 4;
    int x0, listrows, i, r, c;
    int b0, sw;
    if (pw < 12) pw = 12;
    if (pw > cols - 4) pw = cols - 4;
    if (pw < 4) return;
    x0 = cols - pw;
    g_menu_x0 = x0;

    /* the bottom third of the panel holds the colour slider */
    b0 = rows * 2 / 3;
    if (b0 < 5) b0 = 5;
    if (b0 > rows - 3) b0 = rows - 3;
    if (b0 < 1) b0 = 1;
    listrows = b0 - 1;
    if (listrows < 1) return;

    if (g_menu_sel >= g_item_count) g_menu_sel = g_item_count - 1;
    if (g_menu_sel < 0) g_menu_sel = 0;

    /* scroll the window so the selection stays visible */
    if (g_menu_sel < g_menu_scroll) g_menu_scroll = g_menu_sel;
    if (g_menu_sel >= g_menu_scroll + listrows) g_menu_scroll = g_menu_sel - listrows + 1;
    if (g_menu_scroll > g_item_count - listrows) g_menu_scroll = g_item_count - listrows;
    if (g_menu_scroll < 0) g_menu_scroll = 0;

    for (r = 0; r < rows; ++r) {
        for (c = x0; c < cols; ++c) grid[(size_t)r * cols + c] = ' ';
        grid[(size_t)r * cols + x0] = '|';
    }
    {
        const char *t = " models ";
        c = x0 + 1;
        while (*t && c < cols) grid[c++] = *t++;
    }
    for (i = 0; i < listrows && g_menu_scroll + i < g_item_count; ++i) {
        int idx = g_menu_scroll + i;
        const char *nm = g_items[idx].name;
        int avail, k;
        r = 1 + i;
        grid[(size_t)r * cols + (x0 + 1)] = (idx == g_menu_sel) ? '>' : ' ';
        avail = cols - x0 - 2;                 /* name cells (up to the last column) */
        if (avail < 1) avail = 1;
        for (k = 0; k < avail && nm[k]; ++k) grid[(size_t)r * cols + (x0 + 2 + k)] = nm[k];
        if (nm[k]) grid[(size_t)r * cols + (cols - 1)] = '~';   /* truncated */
    }
    if (g_menu_scroll > 0)
        grid[(size_t)1 * cols + (cols - 1)] = '^';
    if (g_menu_scroll + listrows < g_item_count)
        grid[(size_t)(b0 - 1) * cols + (cols - 1)] = 'v';

    /* --- colour slider (bottom third): pick the shape colour live --------- */
    sw = cols - x0 - 2;                        /* slider cells: x0+1 .. cols-2 */
    if (sw >= 2) {
        char lbl[48];
        const char *t;
        _snprintf(lbl, sizeof(lbl), " colour %d/%d  #%d ", g_color_sel + 1, COLOR_N,
                  COLORS[g_color_sel]);
        lbl[sizeof(lbl) - 1] = '\0';
        t = lbl;
        c = x0 + 1;
        while (*t && c < cols - 1) grid[(size_t)b0 * cols + c++] = *t++;

        for (i = 0; i < sw; ++i) {             /* gradient bar */
            int p = (sw > 1) ? (int)((double)i * (COLOR_N - 1) / (sw - 1)) : 0;
            size_t idx = (size_t)(b0 + 1) * cols + (x0 + 1 + i);
            grid[idx] = '=';
            if (g_cc) g_cc[idx] = (short)COLORS[p];
        }
        if (b0 + 2 < rows) {                   /* caret under the current value */
            int pos = (sw > 1) ? (int)((double)g_color_sel * (sw - 1) / (COLOR_N - 1) + 0.5) : 0;
            size_t idx;
            if (pos < 0) pos = 0;
            if (pos >= sw) pos = sw - 1;
            idx = (size_t)(b0 + 2) * cols + (x0 + 1 + pos);
            grid[idx] = '^';
            if (g_cc) g_cc[idx] = (short)COLORS[g_color_sel];
        }
    }
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

/* -------------------------------------------------------------- user models
 * Files dropped into the "models" folder next to the exe are loaded and added
 * to the shape list. The folder is polled; .stl/.obj/.ply are supported, with
 * an optional "<name>.json" sidecar (up/yaw/pitch/elev/zoom/name).
 */

typedef struct {
    int    up;        /* 0 = z, 1 = y, 2 = x */
    double yaw, pitch, elev, zoom;
    char   name[64];
    int    has_name;
} ModelMeta;

typedef struct {
    wchar_t            path[MAX_PATH];
    wchar_t            wname[MAX_PATH];
    unsigned long long mtime, size;
} DirEnt;

static wchar_t g_models_dir[MAX_PATH] = L"";
static char    g_filesig[8192] = "";
static int     g_filesig_valid = 0;

static void meta_defaults(ModelMeta *m) {
    m->up = 0; m->yaw = 0; m->pitch = 0; m->elev = 15.0; m->zoom = 1.0;
    m->name[0] = '\0'; m->has_name = 0;
}

/* ---- tiny JSON value lookup (flat object, no nesting) ------------------- */
static const char *json_find(const char *buf, const char *key) {
    char pat[64];
    const char *p;
    _snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(buf, pat);
    if (!p) return NULL;
    p += strlen(pat);
    p = strchr(p, ':');
    if (!p) return NULL;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    return p;
}
static int json_num(const char *buf, const char *key, double *out) {
    const char *v = json_find(buf, key);
    char *end;
    if (!v) return 0;
    *out = strtod(v, &end);
    return end != v;
}
static int json_str(const char *buf, const char *key, char *out, int cap) {
    const char *v = json_find(buf, key);
    int i = 0;
    if (!v) return 0;
    if (*v == '"') ++v;
    while (*v && *v != '"' && *v != ',' && *v != '}' && *v != '\r' && *v != '\n' && i < cap - 1)
        out[i++] = *v++;
    while (i > 0 && out[i - 1] == ' ') --i;
    out[i] = '\0';
    return 1;
}

static void meta_read(const wchar_t *model_path, ModelMeta *m) {
    wchar_t jp[MAX_PATH];
    const wchar_t *dot;
    HANDLE h;
    DWORD sz, got;
    char *buf;

    wcscpy(jp, model_path);
    dot = wcsrchr(jp, L'.');
    if (dot) wcscpy((wchar_t *)dot, L".json");
    else return;
    h = CreateFileW(jp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    sz = GetFileSize(h, NULL);
    if (sz == 0 || sz > 65536) { CloseHandle(h); return; }
    buf = (char *)malloc(sz + 1);
    if (!buf) { CloseHandle(h); return; }
    ReadFile(h, buf, sz, &got, NULL);
    buf[got] = '\0';
    CloseHandle(h);
    {
        double d;
        char s[64];
        if (json_str(buf, "up", s, sizeof(s))) {
            if (_stricmp(s, "y") == 0) m->up = 1;
            else if (_stricmp(s, "x") == 0) m->up = 2;
            else m->up = 0;
        }
        if (json_num(buf, "yaw", &d))   m->yaw = d;
        if (json_num(buf, "pitch", &d)) m->pitch = d;
        if (json_num(buf, "elev", &d))  m->elev = d;
        if (json_num(buf, "zoom", &d))  m->zoom = d;
        if (json_str(buf, "name", s, sizeof(s))) {
            strncpy(m->name, s, sizeof(m->name) - 1);
            m->name[sizeof(m->name) - 1] = '\0';
            m->has_name = 1;
        }
    }
    free(buf);
}

static v3 rot_y(v3 p, double a) {
    double c = cos(a), s = sin(a);
    return v3_make(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);
}

/* ---- LOD levels (keep-fractions of the source triangle count) ------------ */
static const double LOD_KEEP[] = { 1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625 };
#define LOD_N ((int)(sizeof(LOD_KEEP) / sizeof(LOD_KEEP[0])))

static int lod_target(int src_ntri, int level) {
    int t;
    if (level < 0) level = 0;
    if (level >= LOD_N) level = LOD_N - 1;
    t = (int)((double)src_ntri * LOD_KEEP[level]);
    if (t < 64) t = 64;
    if (t > src_ntri) t = src_ntri;
    return t;
}

static int default_lod_level(int src_ntri) {
    int i;
    for (i = 0; i < LOD_N; ++i)
        if (lod_target(src_ntri, i) <= MAX_TRIS) return i;
    return LOD_N - 1;
}

/* Transform, centre and scale a RawMesh into an owned MeshDef. Steals rm->tris. */
static MeshDef *make_mesh(RawMesh *rm, const ModelMeta *meta) {
    MeshDef *m;
    float *verts;
    double mn[3] = { 1e30, 1e30, 1e30 }, mx[3] = { -1e30, -1e30, -1e30 };
    double c[3], maxext, scale;
    int i, k;

    if (rm->nvert <= 0 || rm->ntri <= 0) return NULL;
    verts = (float *)malloc(sizeof(float) * 3 * (size_t)rm->nvert);
    if (!verts) return NULL;
    for (i = 0; i < rm->nvert; ++i) {
        v3 v = v3_make(rm->verts[i * 3 + 0], rm->verts[i * 3 + 1], rm->verts[i * 3 + 2]);
        if (meta->up == 1) v = rot_x(v, PI / 2.0);
        else if (meta->up == 2) v = rot_y(v, -PI / 2.0);
        v = rot_z(v, meta->yaw * (PI / 180.0));
        v = rot_x(v, meta->pitch * (PI / 180.0));
        verts[i * 3 + 0] = (float)v.x;
        verts[i * 3 + 1] = (float)v.y;
        verts[i * 3 + 2] = (float)v.z;
        for (k = 0; k < 3; ++k) {
            double val = (&v.x)[k];
            if (val < mn[k]) mn[k] = val;
            if (val > mx[k]) mx[k] = val;
        }
    }
    for (k = 0; k < 3; ++k) c[k] = (mn[k] + mx[k]) * 0.5;
    maxext = mx[0] - mn[0];
    if (mx[1] - mn[1] > maxext) maxext = mx[1] - mn[1];
    if (mx[2] - mn[2] > maxext) maxext = mx[2] - mn[2];
    if (maxext <= 1e-9) { free(verts); return NULL; }
    scale = 2.0 / maxext;
    for (i = 0; i < rm->nvert; ++i) {
        verts[i * 3 + 0] = (float)((verts[i * 3 + 0] - c[0]) * scale);
        verts[i * 3 + 1] = (float)((verts[i * 3 + 1] - c[1]) * scale);
        verts[i * 3 + 2] = (float)((verts[i * 3 + 2] - c[2]) * scale);
    }

    m = (MeshDef *)calloc(1, sizeof(MeshDef));
    if (!m) { free(verts); return NULL; }
    m->verts  = verts;
    m->tris   = rm->tris; rm->tris = NULL;      /* take ownership */
    m->nvert  = rm->nvert;
    m->ntri   = rm->ntri;
    m->elev   = (meta->elev > 0.0 && meta->elev < 89.0) ? meta->elev : 15.0;
    m->zoom   = (meta->zoom > 0.01 && meta->zoom < 100.0) ? meta->zoom : 1.0;
    m->owned  = 1;
    m->built  = 0;

    if (m->ntri > MAX_TRIS) {           /* too detailed: keep a source and LOD it */
        int lvl = default_lod_level(m->ntri);
        RawMesh out;
        char err[64];
        if (simplify_cluster(m->verts, m->nvert, m->tris, m->ntri,
                             lod_target(m->ntri, lvl), &out, err, sizeof(err))) {
            m->src_verts = m->verts;  m->src_tris = m->tris;
            m->src_nvert = m->nvert;  m->src_ntri = m->ntri;
            m->verts = out.verts;     m->tris = out.tris;
            m->nvert = out.nvert;     m->ntri = out.ntri;
            m->lod_level = lvl;
            msg_setf("LOD: reduced to %d tris (PageUp/PageDown)", m->ntri);
        } else {
            m->lod_level = 0;
        }
    }
    return m;
}

/* Re-run the LOD decimation for a mesh at the given level. */
static void set_lod(MeshDef *m, int level) {
    RawMesh out;
    char err[64];
    if (!m || !m->src_tris) return;
    if (level < 0) level = 0;
    if (level >= LOD_N) level = LOD_N - 1;
    if (!simplify_cluster(m->src_verts, m->src_nvert, m->src_tris, m->src_ntri,
                          lod_target(m->src_ntri, level), &out, err, sizeof(err))) {
        msg_setf("LOD failed: %s", err);
        return;
    }
    free((void *)m->verts);
    free((void *)m->tris);
    m->verts = out.verts; m->tris = out.tris;
    m->nvert = out.nvert; m->ntri = out.ntri;
    m->lod_level = level;
    mesh_release(m);
    lru_touch(m);
    msg_setf("LOD %d/%d: %d triangles", level + 1, LOD_N, m->ntri);
}

static void apply_lod(int delta) {
    if (!g_mesh || !g_mesh->src_tris) {
        msg_setf("LOD: only for meshes reduced from too many triangles");
        return;
    }
    set_lod(g_mesh, g_mesh->lod_level + delta);
}

static void stem_of(const wchar_t *name, char *out, int cap) {
    wchar_t w[64];
    int i = 0;
    while (name[i] && name[i] != L'.' && i < 63) { w[i] = name[i]; ++i; }
    w[i] = L'\0';
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
}

static MeshDef *load_model_mesh(const wchar_t *path, const char *stem,
                                char *disp, int disp_cap, char *err, int errsz) {
    RawMesh rm;
    ModelMeta meta;
    MeshDef *m;

    if (!loader_load(path, MAX_LOAD_TRIS, &rm, err, errsz)) return NULL;
    meta_defaults(&meta);
    meta_read(path, &meta);
    m = make_mesh(&rm, &meta);
    free(rm.verts);
    free(rm.tris);
    if (!m) { strncpy(err, "degenerate or empty mesh", (size_t)errsz - 1); err[errsz - 1] = '\0'; return NULL; }
    if (meta.has_name) { strncpy(disp, meta.name, (size_t)disp_cap - 1); }
    else               { strncpy(disp, stem, (size_t)disp_cap - 1); }
    disp[disp_cap - 1] = '\0';
    return m;
}

static void unique_name(char *buf, int cap) {
    char base[64];
    int n = 2;
    strncpy(base, buf, sizeof(base) - 1); base[sizeof(base) - 1] = '\0';
    while (find_item(buf) >= 0) {
        _snprintf(buf, (size_t)cap, "%s-%d", base, n++);
    }
}

/* ---- user models (loaded lazily, so a scan stays cheap) ----------------- */
typedef struct {
    wchar_t            path[MAX_PATH];
    wchar_t            wname[MAX_PATH];
    char               stem[64];
    char               disp[64];
    unsigned long long mtime, size;
    MeshDef           *mesh;   /* NULL until first shown */
} ModelFile;

static ModelFile g_models[MAX_MODELS];
static int       g_model_count = 0;

static MeshDef *model_load(int idx) {
    char tmp[64], err[128];
    MeshDef *m;
    if (idx < 0 || idx >= g_model_count) return NULL;
    if (g_models[idx].mesh) return g_models[idx].mesh;
    m = load_model_mesh(g_models[idx].path, g_models[idx].stem, tmp, sizeof(tmp), err, sizeof(err));
    if (!m) {
        msg_setf("models: %ls: %s", g_models[idx].wname, err);
        return NULL;
    }
    g_models[idx].mesh = m;
    return m;
}

static void models_clear_loaded(void) {
    int i;
    lru_clear();                              /* release any BVHs first */
    for (i = 0; i < g_model_count; ++i)
        if (g_models[i].mesh) { mesh_destroy(g_models[i].mesh); g_models[i].mesh = NULL; }
    g_model_count = 0;
    for (i = g_item_count - 1; i >= 0; --i) { /* drop stale loaded items */
        if (g_items[i].model >= 0) {
            memmove(&g_items[i], &g_items[i + 1],
                    (size_t)(g_item_count - i - 1) * sizeof(ShapeItem));
            --g_item_count;
        }
    }
}

static int dir_cmp(const void *a, const void *b) {
    return _wcsicmp(((const DirEnt *)a)->wname, ((const DirEnt *)b)->wname);
}

static void models_init(void) {
    wchar_t exe[MAX_PATH];
    wchar_t *p;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        wcscpy(g_models_dir, L"models");
    } else {
        p = wcsrchr(exe, L'\\');
        if (p) *p = L'\0';
        _snwprintf(g_models_dir, MAX_PATH, L"%ls\\models", exe);
    }
    CreateDirectoryW(g_models_dir, NULL);
}

static void models_poll(int force) {
    DirEnt ents[256];
    int ne = 0, i;
    wchar_t pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char sig[8192];
    int sp = 0;

    _snwprintf(pat, MAX_PATH, L"%ls\\*", g_models_dir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t *ext;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ext = wcsrchr(fd.cFileName, L'.');
            if (!ext) continue;
            if (_wcsicmp(ext, L".stl") && _wcsicmp(ext, L".obj") && _wcsicmp(ext, L".ply")) continue;
            if (ne < (int)(sizeof(ents) / sizeof(ents[0]))) {
                DirEnt *d = &ents[ne++];
                _snwprintf(d->path, MAX_PATH, L"%ls\\%ls", g_models_dir, fd.cFileName);
                wcsncpy(d->wname, fd.cFileName, MAX_PATH - 1); d->wname[MAX_PATH - 1] = L'\0';
                d->mtime = ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32)
                         | fd.ftLastWriteTime.dwLowDateTime;
                d->size  = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    qsort(ents, (size_t)ne, sizeof(DirEnt), dir_cmp);

    sig[0] = '\0';
    for (i = 0; i < ne; ++i) {
        int k = _snprintf(sig + sp, sizeof(sig) - (size_t)sp, "%ls|%llu|%llu;",
                          ents[i].wname, ents[i].mtime, ents[i].size);
        if (k < 0) { sp = (int)sizeof(sig) - 1; break; }
        sp += k;
    }
    sig[sizeof(sig) - 1] = '\0';

    if (!force && g_filesig_valid && strcmp(sig, g_filesig) == 0) return;

    g_model_msg[0] = '\0';
    g_msg_time = 0.0;
    {
        char keep[64];
        strncpy(keep, g_shape_name, sizeof(keep) - 1); keep[sizeof(keep) - 1] = '\0';

        models_clear_loaded();
        for (i = 0; i < ne && i < MAX_MODELS; ++i) {
            ModelFile *mf = &g_models[g_model_count];
            ModelMeta meta;
            int it;
            wcscpy(mf->path, ents[i].path);
            wcscpy(mf->wname, ents[i].wname);
            mf->mtime = ents[i].mtime;
            mf->size = ents[i].size;
            mf->mesh = NULL;
            stem_of(mf->wname, mf->stem, sizeof(mf->stem));
            meta_defaults(&meta);
            meta_read(mf->path, &meta);           /* cheap: read the name only */
            if (meta.has_name) strncpy(mf->disp, meta.name, sizeof(mf->disp) - 1);
            else               strncpy(mf->disp, mf->stem, sizeof(mf->disp) - 1);
            mf->disp[sizeof(mf->disp) - 1] = '\0';
            unique_name(mf->disp, sizeof(mf->disp));
            it = items_add(mf->disp, -1, NULL);   /* mesh is loaded on demand */
            if (it >= 0) g_items[it].model = g_model_count;
            ++g_model_count;
        }
        if (ne > MAX_MODELS)
            msg_setf("models: only first %d files loaded", MAX_MODELS);

        strncpy(g_filesig, sig, sizeof(g_filesig) - 1);
        g_filesig[sizeof(g_filesig) - 1] = '\0';
        g_filesig_valid = 1;

        if (!select_by_name(keep)) select_index(0);
    }
}

/* --------------------------------------------------------------- command line */

static void print_help(void) {
    printf(
        "%s %s\n\n"
        "Render a shaded 3D shape as animated ASCII art in the console.\n\n"
        "Usage:\n"
        "  ascii3D [options]\n\n"
        "Options:\n"
        "  -s, --shape <name>   Shape to render: cube | cylinder | diamond |\n"
        "                       sphere | stego | f1 | companion | maus |\n"
        "                       fish | kebab | berlin | china, or a model\n"
        "                       name from the models/ folder (default: cube)\n"
        "  -h, --help           Show this help and exit\n"
        "  -v, --version        Show version and exit\n"
        "      --snapshot       Render a single frame to stdout and exit\n"
        "      --angle <deg>    Initial rotation angle in degrees (default: 0)\n"
        "      --tilt <deg>     Initial axis tilt in degrees, -90..90 (default: 0)\n"
        "      --shatter        With --snapshot: shatter and simulate the fall\n"
        "      --sim <sec>      With --shatter: seconds to simulate (default: 3)\n"
        "      --menu           With --snapshot: draw the model list (preview)\n"
        "      --lod <level>    With --snapshot: apply an LOD level (0..%d)\n"
        "      --bench <frames> Benchmark offscreen rendering and exit\n"
        "      --fps <n>        Frame-rate cap for interactive mode (default 60)\n\n"
        "Controls (interactive):\n"
        "  ESC                  Quit\n"
        "  +                    Increase spin by %d deg/s (max %d deg/s)\n"
        "  -                    Decrease spin by %d deg/s (min %d deg/s)\n"
        "  Up / Down            Tilt the spin axis away from / towards the viewer\n"
        "                       in %d deg steps (max %d deg)\n"
        "  ENTER                Shatter the object; press again to rebuild it\n"
        "  SPACE                Switch to the next shape (insertion order)\n"
        "  TAB                  Toggle the model list on the right; then pick\n"
        "                       with Up/Down (tilt pauses) and load with\n"
        "                       SPACE/ENTER; scroll with the mouse wheel\n"
        "  LEFT / RIGHT         In the list: move the shape-colour slider\n"
        "                       (256-colour scale, applied live)\n"
        "  PAGE UP / PAGE DOWN  Weaker / stronger LOD for meshes that were\n"
        "                       reduced from too many triangles\n"
        "  POS1 (HOME)          Toggle the FPS display (bottom-left)\n"
        "  R                    Rescan the models/ folder\n"
        "  q                    Quit\n\n"
        "Drop .stl/.obj/.ply files into the 'models' folder next to this\n"
        "executable; they are detected automatically and added to the cycle.\n",
        APP_NAME, APP_VERSION,
        (int)SPEED_STEP, (int)MAX_SPEED, (int)SPEED_STEP, (int)MIN_SPEED,
        (int)TILT_STEP, (int)MAX_TILT, LOD_N - 1);
}

/* Shape aliases are resolved by canonical_name() / select_by_name(). */


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
        } else if (_stricmp(a, "--menu") == 0) {
            g_menu_start = 1;
        } else if (_stricmp(a, "--lod") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --lod requires a value\n");
                return 0;
            }
            g_lod_arg = atoi(argv[++i]);
        } else if (_stricmp(a, "--bench") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --bench requires a value\n");
                return 0;
            }
            g_bench = atoi(argv[++i]);
        } else if (_stricmp(a, "--fps") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --fps requires a value\n");
                return 0;
            }
            g_target_fps = atoi(argv[++i]);
            if (g_target_fps < 0) g_target_fps = 0;
            if (g_target_fps > 240) g_target_fps = 240;
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

    if (shape) {
        strncpy(g_req_shape, shape, sizeof(g_req_shape) - 1);
        g_req_shape[sizeof(g_req_shape) - 1] = '\0';
        g_req_shape_set = 1;
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

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
static HANDLE g_wait_timer = NULL;

/* Sleep for `sec` seconds with sub-millisecond accuracy (falls back to Sleep). */
static void precise_sleep(double sec) {
    if (sec <= 0.0) return;
    if (g_wait_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(sec * 1.0e7);   /* relative, 100 ns units */
        if (SetWaitableTimer(g_wait_timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(g_wait_timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD)(sec * 1000.0 + 0.5));
}

/* Offscreen render benchmark: renders `frames` frames over one full turn and
 * reports the average time per frame (no console required). */
static int run_bench(int frames) {
    const int cols = 120, rows = 40;
    char *grid;
    LARGE_INTEGER f, a, b;
    int i;
    double total_ms, per;
    if (frames < 1) frames = 1;
    grid = (char *)malloc((size_t)cols * rows);
    if (!grid) return 1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (i = 0; i < frames; ++i) {
        g_angle = (double)i * (2.0 * PI / (double)frames);
        render_grid(grid, cols, rows);
    }
    QueryPerformanceCounter(&b);
    total_ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    per = total_ms / (double)frames;
    {
        unsigned long long sum = 0;
        int n = cols * rows, k;
        for (k = 0; k < n; ++k) sum = sum * 131 + (unsigned char)grid[k];
        printf("bench: %-10s %4d frames @ %dx%d  %8.3f ms/frame  %7.1f fps  (%s) cs=%llu\n",
               g_shape_name, frames, cols, rows, per, (per > 0.0) ? 1000.0 / per : 0.0,
               g_mesh ? "mesh" : "analytic", sum);
    }
    free(grid);
    return 0;
}

int main(int argc, char **argv) {
    HANDLE h_out, h_in;
    DWORD  orig_mode = 0, in_mode = 0;
    int    is_console, cols = 0, rows = 0;
    char  *grid = NULL, *out = NULL;
    LARGE_INTEGER freq, now, last, frame_start;
    double angle_deg;
    double scan_acc = 0.0;

    if (!parse_args(argc, argv)) return 1;
    colors_init();

    build_registry();
    models_init();
    models_poll(1);                    /* initial scan of the models folder */
    if (g_req_shape_set) {
        if (!select_by_name(g_req_shape)) {
            int i;
            fprintf(stderr, "error: unknown shape '%s'\n       available:", g_req_shape);
            for (i = 0; i < g_item_count; ++i) fprintf(stderr, " %s", g_items[i].name);
            fprintf(stderr, "\n");
            return 1;
        }
    } else {
        select_index(0);
    }

    if (g_bench > 0) return run_bench(g_bench);

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
        if (g_lod_arg >= 0 && g_mesh && g_mesh->src_tris) set_lod(g_mesh, g_lod_arg);
        render_grid(grid, cols, rows);
        if (g_model_msg[0]) { fprintf(stderr, "%s\n", g_model_msg); g_model_msg[0] = '\0'; }
        if (g_menu_start) {
            g_menu_open = 1;
            g_menu_sel = g_item_index;
            draw_menu(grid, cols, rows);
        }
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
                "       For a one-off frame use: ascii3D --snapshot -s cube\n");
        return 1;
    }

    /* enable ANSI/VT escape sequences, hide cursor, use the alt screen */
    SetConsoleMode(h_out, orig_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                          ENABLE_PROCESSED_OUTPUT);
    h_in = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(h_in, &in_mode);
    SetConsoleMode(h_in, (in_mode | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS)
                          & ~((DWORD)ENABLE_QUICK_EDIT_MODE));
    SetConsoleTitleA(APP_NAME " " APP_VERSION " - press ESC to quit");
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    write_all(h_out, is_console, "\x1b[?1049h\x1b[?25l", -1);

    angle_deg = g_start_angle;
    g_tilt = g_start_tilt;
    rng_seed((unsigned int)GetTickCount());
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    g_wait_timer = CreateWaitableTimerExW(NULL, NULL,
                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    timeBeginPeriod(1);   /* ~1 ms Sleep granularity (fallback path) */

    while (g_running) {
        double dt;

        QueryPerformanceCounter(&frame_start);

        /* --- input: keys + mouse wheel -------------------------------- */
        {
            DWORD nev = 0;
            if (GetNumberOfConsoleInputEvents(h_in, &nev) && nev > 0) {
                INPUT_RECORD recs[64];
                DWORD got = 0, i;
                if (nev > 64) nev = 64;
                if (ReadConsoleInputW(h_in, recs, nev, &got)) {
                    for (i = 0; i < got; ++i) {
                        INPUT_RECORD *r = &recs[i];
                        if (r->EventType == KEY_EVENT && r->Event.KeyEvent.bKeyDown) {
                            WORD vk = r->Event.KeyEvent.wVirtualKeyCode;
                            char ch = r->Event.KeyEvent.uChar.AsciiChar;
                            if (g_menu_open) {
                                if (vk == VK_TAB) g_menu_open = 0;
                                else if (vk == VK_ESCAPE) g_running = 0;
                                else if (vk == VK_UP)   { if (g_menu_sel > 0) --g_menu_sel; }
                                else if (vk == VK_DOWN) { if (g_menu_sel < g_item_count - 1) ++g_menu_sel; }
                                else if (vk == VK_RETURN || vk == VK_SPACE) select_index(g_menu_sel);
                                else if (ch == '+' || ch == '=') { g_speed += SPEED_STEP; if (g_speed > MAX_SPEED) g_speed = MAX_SPEED; }
                                else if (ch == '-' || ch == '_') { g_speed -= SPEED_STEP; if (g_speed < MIN_SPEED) g_speed = MIN_SPEED; }
                                else if (vk == VK_PRIOR) apply_lod(-1);
                                else if (vk == VK_NEXT)  apply_lod(1);
                                else if (vk == VK_HOME)  g_show_fps = !g_show_fps;
                                else if (vk == VK_LEFT)  { if (g_color_sel > 0) { --g_color_sel; g_fg_color = COLORS[g_color_sel]; } }
                                else if (vk == VK_RIGHT) { if (g_color_sel < COLOR_N - 1) { ++g_color_sel; g_fg_color = COLORS[g_color_sel]; } }
                                else if (ch == 'q' || ch == 'Q') g_running = 0;
                                else if (ch == 'r' || ch == 'R') models_poll(1);
                            } else {
                                if (vk == VK_ESCAPE || ch == 'q' || ch == 'Q') g_running = 0;
                                else if (vk == VK_TAB) { g_menu_open = 1; g_menu_sel = g_item_index; g_menu_scroll = 0; }
                                else if (vk == VK_UP)   { g_tilt += TILT_STEP; if (g_tilt > MAX_TILT) g_tilt = MAX_TILT; }
                                else if (vk == VK_DOWN) { g_tilt -= TILT_STEP; if (g_tilt < -MAX_TILT) g_tilt = -MAX_TILT; }
                                else if (vk == VK_SPACE) select_index(g_item_index + 1);
                                else if (vk == VK_RETURN) {
                                    if (grid && cols > 0) {
                                        if (!g_shattered) {
                                            render_grid(grid, cols, rows);
                                            shatter_spawn(grid, cols, rows);
                                            g_shattered = 1;
                                        } else {
                                            g_shattered = 0;
                                        }
                                    }
                                } else if (ch == '+' || ch == '=') { g_speed += SPEED_STEP; if (g_speed > MAX_SPEED) g_speed = MAX_SPEED; }
                                else if (ch == '-' || ch == '_') { g_speed -= SPEED_STEP; if (g_speed < MIN_SPEED) g_speed = MIN_SPEED; }
                                else if (vk == VK_PRIOR) apply_lod(-1);
                                else if (vk == VK_NEXT)  apply_lod(1);
                                else if (vk == VK_HOME)  g_show_fps = !g_show_fps;
                                else if (ch == 'r' || ch == 'R') models_poll(1);
                            }
                        } else if (r->EventType == MOUSE_EVENT && g_menu_open) {
                            if (r->Event.MouseEvent.dwEventFlags == MOUSE_WHEELED) {
                                short d = (short)HIWORD(r->Event.MouseEvent.dwButtonState);
                                if (d > 0) { if (g_menu_sel > 0) --g_menu_sel; }
                                else if (d < 0) { if (g_menu_sel < g_item_count - 1) ++g_menu_sel; }
                            }
                        }
                    }
                }
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
                out = (char *)malloc((size_t)cols * rows * 6 + (size_t)rows * 4 + 256);
                if (!out) break;
                cc_ensure(cols * rows);
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

        /* auto-hide the status message a few seconds after it was set */
        if (g_msg_time > 0.0) {
            g_msg_time -= dt;
            if (g_msg_time <= 0.0) { g_msg_time = 0.0; g_model_msg[0] = '\0'; }
        }

        /* FPS estimate over ~0.5 s windows */
        {
            static int    frames = 0;
            static double facc = 0.0;
            ++frames;
            facc += dt;
            if (facc >= 0.5) { g_fps = (double)frames / facc; frames = 0; facc = 0.0; }
        }

        /* --- poll the models folder (~1 s) ---------------------------- */
        scan_acc += dt;
        if (scan_acc >= 1.0) { scan_acc = 0.0; models_poll(0); }

        /* --- update + render ------------------------------------------ */
        if (g_cc) memset(g_cc, -1, sizeof(short) * (size_t)cols * rows);
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
        if (g_menu_open) draw_menu(grid, cols, rows);

        /* --- present (per-cell colour) -------------------------------- */
        {
            char *o = out;
            int r, c, cur = -2;
            *o++ = '\x1b'; *o++ = '['; *o++ = 'H';
            for (r = 0; r < rows; ++r) {
                for (c = 0; c < cols; ++c) {
                    size_t idx = (size_t)r * cols + c;
                    int want;
                    if (g_cc && g_cc[idx] >= 0) want = g_cc[idx];
                    else if (is_ui_cell(r, c))  want = -1;
                    else                        want = g_fg_color;
                    if (want != cur) {
                        if (want < 0) o += sprintf(o, "\x1b[39m");
                        else          o += sprintf(o, "\x1b[38;5;%dm", want);
                        cur = want;
                    }
                    *o++ = grid[idx];
                }
                if (cur != -1) { memcpy(o, "\x1b[39m", 5); o += 5; cur = -1; }
                if (r < rows - 1) { *o++ = '\r'; *o++ = '\n'; }
            }
            *o = '\0';
            write_all(h_out, is_console, out, (int)(o - out));
        }

        /* --- frame limiter: only sleep if we are ahead of the target ----- */
        if (g_target_fps > 0) {
            LARGE_INTEGER t;
            double spent, wait;
            QueryPerformanceCounter(&t);
            spent = (double)(t.QuadPart - frame_start.QuadPart) / (double)freq.QuadPart;
            wait  = 1.0 / (double)g_target_fps - spent;
            if (wait > 0.0005) precise_sleep(wait);
        }
    }

    /* ---- restore the console ----------------------------------------- */
    if (g_wait_timer) CloseHandle(g_wait_timer);
    timeEndPeriod(1);
    write_all(h_out, is_console, "\x1b[0m\x1b[?25h\x1b[?1049l", -1);
    SetConsoleMode(h_out, orig_mode);
    SetConsoleMode(h_in, in_mode);
    free(grid);
    free(out);
    free(g_parts);
    free(g_col_h);
    return 0;
}
