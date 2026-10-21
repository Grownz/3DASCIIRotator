/* ============================================================================
 * fbx.c - minimal binary FBX (Kaydara FBX Binary) reader for static meshes.
 *
 * Parses the node/property tree, reads the geometry (Vertices /
 * PolygonVertexIndex), the model transforms from Properties70 and the
 * per-model materials, then emits a triangle mesh. Compressed arrays are
 * unpacked with the same raw-DEFLATE decoder used by the 3MF reader.
 *
 * Not supported: ASCII FBX, animation, skinning, cameras, lights, textures.
 * ==========================================================================*/
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include "loader.h"
#include "meshbuf.h"
#include "inflate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- utilities */
static unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned rd16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned long long rd64(const unsigned char *p) {
    return (unsigned long long)rd32(p) | ((unsigned long long)rd32(p + 4) << 32);
}

/* FBX array properties are zlib streams (RFC 1950): skip the 2-byte header and
 * inflate the raw DEFLATE payload. Fall back to raw DEFLATE if it is not a
 * valid zlib header. */
static unsigned char *inflate_zlib(const unsigned char *in, size_t inlen, size_t *outlen) {
    if (inlen >= 2 && (in[0] & 0x0F) == 8 && ((((unsigned)in[0] << 8) + in[1]) % 31u) == 0)
        return inflate_raw(in + 2, inlen - 2, outlen);
    return inflate_raw(in, inlen, outlen);
}

/* ------------------------------------------------------------------- nodes */
typedef struct {
    char       type;
    long long  i;
    double     d;
    char      *s;      /* string / raw, owned */
    void      *a;      /* decoded array, owned */
    int        n;      /* array count */
    int        esize;  /* array element size */
} FbxProp;

typedef struct FbxNode {
    char  *name;
    FbxProp *props;
    int    nprop, capprop;
    struct FbxNode **kids;
    int    nkid, capkid;
} FbxNode;

static FbxNode *node_new(void) {
    return (FbxNode *)calloc(1, sizeof(FbxNode));
}

static void node_free(FbxNode *n) {
    int i;
    if (!n) return;
    for (i = 0; i < n->nprop; ++i) { free(n->props[i].s); free(n->props[i].a); }
    for (i = 0; i < n->nkid; ++i) node_free(n->kids[i]);
    free(n->props);
    free(n->kids);
    free(n->name);
    free(n);
}

static int node_addprop(FbxNode *n, const FbxProp *p) {
    if (n->nprop == n->capprop) {
        int nc = n->capprop ? n->capprop * 2 : 4;
        FbxProp *np = (FbxProp *)realloc(n->props, (size_t)nc * sizeof(FbxProp));
        if (!np) return 0;
        n->props = np; n->capprop = nc;
    }
    n->props[n->nprop++] = *p;
    return 1;
}

static int node_addkid(FbxNode *n, FbxNode *k) {
    if (n->nkid == n->capkid) {
        int nc = n->capkid ? n->capkid * 2 : 4;
        FbxNode **np = (FbxNode **)realloc(n->kids, (size_t)nc * sizeof(FbxNode *));
        if (!np) return 0;
        n->kids = np; n->capkid = nc;
    }
    n->kids[n->nkid++] = k;
    return 1;
}

static FbxNode *find_child(FbxNode *n, const char *name) {
    int i;
    if (!n) return NULL;
    for (i = 0; i < n->nkid; ++i)
        if (!strcmp(n->kids[i]->name, name)) return n->kids[i];
    return NULL;
}

static const FbxProp *prop_at(const FbxNode *n, int i) {
    return (n && i >= 0 && i < n->nprop) ? &n->props[i] : NULL;
}
static long long prop_i(const FbxNode *n, int i) {
    const FbxProp *p = prop_at(n, i);
    return p ? p->i : 0;
}
static double prop_d(const FbxNode *n, int i) {
    const FbxProp *p = prop_at(n, i);
    return p ? p->d : 0.0;
}
static const char *prop_s(const FbxNode *n, int i) {
    const FbxProp *p = prop_at(n, i);
    return (p && p->s) ? p->s : "";
}

static double arr_d(const FbxProp *p, int i) {
    if (!p || !p->a || i < 0 || i >= p->n) return 0.0;
    switch (p->type) {
    case 'd': return ((double *)p->a)[i];
    case 'f': return ((float *)p->a)[i];
    case 'i': return ((int *)p->a)[i];
    case 'l': return (double)((long long *)p->a)[i];
    case 'b': return ((unsigned char *)p->a)[i];
    }
    return 0.0;
}

static long long arr_i(const FbxProp *p, int i) {
    if (!p || !p->a || i < 0 || i >= p->n) return 0;
    switch (p->type) {
    case 'i': return ((int *)p->a)[i];
    case 'l': return ((long long *)p->a)[i];
    case 'b': return ((unsigned char *)p->a)[i];
    case 'f': return (long long)((float *)p->a)[i];
    case 'd': return (long long)((double *)p->a)[i];
    }
    return 0;
}

/* Only these arrays are worth keeping; everything else (animation curves,
 * normals, UVs, ...) is skipped to bound memory use. */
static int node_keeps_arrays(const char *name) {
    return !strcmp(name, "Vertices") ||
           !strcmp(name, "PolygonVertexIndex") ||
           !strcmp(name, "Materials");
}

/* ------------------------------------------------------------- binary parse */
typedef struct {
    const unsigned char *b;
    size_t n, pos;
    int    wide;   /* FBX 7.5+ uses 64-bit record fields */
    int    nodes;
} FR;

static int read_prop(FR *r, FbxProp *pr, int keep) {
    char t;
    memset(pr, 0, sizeof(*pr));
    if (r->pos >= r->n) return 0;
    t = (char)r->b[r->pos++];
    pr->type = t;
    switch (t) {
    case 'Y':
        if (r->pos + 2 > r->n) return 0;
        pr->i = (long long)(short)rd16(r->b + r->pos); r->pos += 2; break;
    case 'C':
        if (r->pos + 1 > r->n) return 0;
        pr->i = r->b[r->pos++]; break;
    case 'I':
        if (r->pos + 4 > r->n) return 0;
        pr->i = (long long)(int)rd32(r->b + r->pos); r->pos += 4; break;
    case 'F': {
        float f;
        if (r->pos + 4 > r->n) return 0;
        memcpy(&f, r->b + r->pos, 4); r->pos += 4;
        pr->d = (double)f; break;
    }
    case 'D': {
        double d;
        if (r->pos + 8 > r->n) return 0;
        memcpy(&d, r->b + r->pos, 8); r->pos += 8;
        pr->d = d; break;
    }
    case 'L':
        if (r->pos + 8 > r->n) return 0;
        pr->i = (long long)rd64(r->b + r->pos); r->pos += 8; break;
    case 'S':
    case 'R': {
        unsigned len;
        if (r->pos + 4 > r->n) return 0;
        len = rd32(r->b + r->pos); r->pos += 4;
        if (r->pos + len > r->n) return 0;
        pr->s = (char *)malloc((size_t)len + 1);
        if (!pr->s) return 0;
        memcpy(pr->s, r->b + r->pos, len);
        pr->s[len] = '\0';
        pr->n = (int)len;
        r->pos += len;
        break;
    }
    case 'f': case 'd': case 'i': case 'l': case 'b': {
        unsigned alen, enc, clen;
        int esize = (t == 'f' || t == 'i') ? 4 : (t == 'd' || t == 'l') ? 8 : 1;
        size_t bytes;
        if (r->pos + 12 > r->n) return 0;
        alen = rd32(r->b + r->pos);
        enc  = rd32(r->b + r->pos + 4);
        clen = rd32(r->b + r->pos + 8);
        r->pos += 12;
        if (alen > 200000000u || clen > 400000000u) return 0;
        bytes = (size_t)alen * (size_t)esize;
        if (!keep) {
            if (enc == 0) { if (r->pos + bytes > r->n) return 0; r->pos += bytes; }
            else if (enc == 1) { if (r->pos + clen > r->n) return 0; r->pos += clen; }
            else return 0;
            break;
        }
        if (enc == 0) {
            if (r->pos + bytes > r->n) return 0;
            pr->a = malloc(bytes ? bytes : 1);
            if (!pr->a) return 0;
            memcpy(pr->a, r->b + r->pos, bytes);
            r->pos += bytes;
        } else if (enc == 1) {
            size_t got = 0;
            unsigned char *dec;
            if (r->pos + clen > r->n) return 0;
            dec = inflate_zlib(r->b + r->pos, clen, &got);
            r->pos += clen;
            if (!dec || got < bytes) { free(dec); return 0; }
            pr->a = malloc(bytes ? bytes : 1);
            if (!pr->a) { free(dec); return 0; }
            memcpy(pr->a, dec, bytes);
            free(dec);
        } else {
            return 0;
        }
        pr->n = (int)alen;
        pr->esize = esize;
        break;
    }
    default:
        return 0;
    }
    return 1;
}

static int read_node(FR *r, FbxNode **out, int depth) {
    size_t start = r->pos;
    size_t hdr = r->wide ? 25 : 13;
    unsigned long long end, nprop;
    unsigned name_len;
    FbxNode *nd;
    unsigned i;

    *out = NULL;
    if (depth > 128) return 0;
    if (start + hdr > r->n) return 0;
    if (++r->nodes > 2000000) return 0;
    {
        size_t k;
        int zero = 1;
        for (k = 0; k < hdr; ++k) if (r->b[start + k]) { zero = 0; break; }
        if (zero) { r->pos += hdr; return 0; }    /* null record: end of list */
    }
    if (r->wide) {
        end = rd64(r->b + start);
        nprop = rd64(r->b + start + 8);
        name_len = r->b[start + 24];
        r->pos = start + 25;
    } else {
        end = rd32(r->b + start);
        nprop = rd32(r->b + start + 4);
        name_len = r->b[start + 12];
        r->pos = start + 13;
    }
    if (end <= (unsigned long long)start || end > (unsigned long long)r->n) return 0;
    if (name_len == 0 || r->pos + name_len > r->n) return 0;
    nd = node_new();
    if (!nd) return 0;
    nd->name = (char *)malloc((size_t)name_len + 1);
    if (!nd->name) { node_free(nd); return 0; }
    memcpy(nd->name, r->b + r->pos, name_len);
    nd->name[name_len] = '\0';
    r->pos += name_len;

    {
        int keep = node_keeps_arrays(nd->name);
        for (i = 0; i < nprop; ++i) {
            FbxProp pr;
            if (!read_prop(r, &pr, keep)) { node_free(nd); return 0; }
            if (!node_addprop(nd, &pr)) { free(pr.s); free(pr.a); node_free(nd); return 0; }
        }
    }
    while (r->pos < (size_t)end) {
        FbxNode *kid = NULL;
        if (!read_node(r, &kid, depth + 1)) break;  /* null record: children done */
        if (!node_addkid(nd, kid)) { node_free(kid); break; }
    }
    if (end > r->n) { node_free(nd); return 0; }
    r->pos = (size_t)end;
    *out = nd;
    return 1;
}

/* ---------------------------------------------------------------- matrices */
typedef struct { double m[16]; } M4;

static M4 m4_id(void) {
    M4 r = { { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 } };
    return r;
}
static M4 m4_mul(M4 a, M4 b) {
    M4 r;
    int i, j, k;
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) {
            double s = 0.0;
            for (k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}
static M4 m4_trans(double x, double y, double z) {
    M4 r = m4_id();
    r.m[3] = x; r.m[7] = y; r.m[11] = z;
    return r;
}
static M4 m4_scale(double x, double y, double z) {
    M4 r = m4_id();
    r.m[0] = x; r.m[5] = y; r.m[10] = z;
    return r;
}
static M4 m4_rotx(double deg) {
    double a = deg * (3.14159265358979323846 / 180.0), c = cos(a), s = sin(a);
    M4 r = m4_id();
    r.m[5] = c; r.m[6] = -s; r.m[9] = s; r.m[10] = c;
    return r;
}
static M4 m4_roty(double deg) {
    double a = deg * (3.14159265358979323846 / 180.0), c = cos(a), s = sin(a);
    M4 r = m4_id();
    r.m[0] = c; r.m[2] = s; r.m[8] = -s; r.m[10] = c;
    return r;
}
static M4 m4_rotz(double deg) {
    double a = deg * (3.14159265358979323846 / 180.0), c = cos(a), s = sin(a);
    M4 r = m4_id();
    r.m[0] = c; r.m[1] = -s; r.m[4] = s; r.m[5] = c;
    return r;
}
/* FBX default Euler order is XYZ; R = Rz * Ry * Rx. */
static M4 m4_euler(double x, double y, double z) {
    return m4_mul(m4_rotz(z), m4_mul(m4_roty(y), m4_rotx(x)));
}
/* Transpose of the rotation part (inverse of a pure rotation). */
static M4 m4_rot_inv(M4 a) {
    M4 r = m4_id();
    int i, j;
    for (i = 0; i < 3; ++i)
        for (j = 0; j < 3; ++j) r.m[i * 4 + j] = a.m[j * 4 + i];
    return r;
}
static void m4_apply(const M4 *m, double x, double y, double z, double *ox, double *oy, double *oz) {
    *ox = m->m[0] * x + m->m[1] * y + m->m[2]  * z + m->m[3];
    *oy = m->m[4] * x + m->m[5] * y + m->m[6]  * z + m->m[7];
    *oz = m->m[8] * x + m->m[9] * y + m->m[10] * z + m->m[11];
}

/* ------------------------------------------------------------- Properties70 */
/* Find a 3-vector in a Model's Properties70 ("Lcl Translation", ...). */
static int p70_vec(FbxNode *model, const char *name, double v[3]) {
    FbxNode *p70 = find_child(model, "Properties70");
    int i, k;
    v[0] = v[1] = v[2] = 0.0;
    if (!p70) return 0;
    for (i = 0; i < p70->nkid; ++i) {
        FbxNode *p = p70->kids[i];
        if (strcmp(p->name, "P")) continue;
        if (strcmp(prop_s(p, 0), name)) continue;
        for (k = 0; k < 3; ++k) {
            const FbxProp *pr = prop_at(p, 4 + k);
            if (pr) v[k] = (pr->type == 'D' || pr->type == 'F') ? pr->d : (double)pr->i;
        }
        return 1;
    }
    return 0;
}

static int p70_num(FbxNode *model, const char *name, double *out) {
    FbxNode *p70 = find_child(model, "Properties70");
    int i;
    if (!p70) return 0;
    for (i = 0; i < p70->nkid; ++i) {
        FbxNode *p = p70->kids[i];
        const FbxProp *pr;
        if (strcmp(p->name, "P") || strcmp(prop_s(p, 0), name)) continue;
        pr = prop_at(p, 4);
        if (!pr) return 0;
        *out = (pr->type == 'D' || pr->type == 'F') ? pr->d : (double)pr->i;
        return 1;
    }
    return 0;
}

static M4 model_transform(FbxNode *model) {
    double T[3], R[3], S[3], Rpre[3], Rpost[3], Roff[3], Rp[3], Soff[3], Sp[3];
    double GT[3], GR[3], GS[3];
    M4 L, G;
    int i;
    for (i = 0; i < 3; ++i) { T[i] = R[i] = Rpre[i] = Rpost[i] = Roff[i] = Rp[i] = Soff[i] = Sp[i] = GT[i] = GR[i] = 0.0; }
    S[0] = S[1] = S[2] = 1.0;
    GS[0] = GS[1] = GS[2] = 1.0;
    p70_vec(model, "Lcl Translation", T);
    p70_vec(model, "Lcl Rotation", R);
    if (!p70_vec(model, "Lcl Scaling", S)) { S[0] = S[1] = S[2] = 1.0; }
    p70_vec(model, "PreRotation", Rpre);
    p70_vec(model, "PostRotation", Rpost);
    p70_vec(model, "RotationOffset", Roff);
    p70_vec(model, "RotationPivot", Rp);
    p70_vec(model, "ScalingOffset", Soff);
    p70_vec(model, "ScalingPivot", Sp);
    p70_vec(model, "GeometricTranslation", GT);
    p70_vec(model, "GeometricRotation", GR);
    p70_vec(model, "GeometricScaling", GS);
    if (GS[0] == 0.0 && GS[1] == 0.0 && GS[2] == 0.0) { GS[0] = GS[1] = GS[2] = 1.0; }

    L = m4_trans(T[0], T[1], T[2]);
    L = m4_mul(L, m4_trans(Roff[0], Roff[1], Roff[2]));
    L = m4_mul(L, m4_trans(Rp[0], Rp[1], Rp[2]));
    L = m4_mul(L, m4_euler(Rpre[0], Rpre[1], Rpre[2]));
    L = m4_mul(L, m4_euler(R[0], R[1], R[2]));
    L = m4_mul(L, m4_rot_inv(m4_euler(Rpost[0], Rpost[1], Rpost[2])));
    L = m4_mul(L, m4_trans(-Rp[0], -Rp[1], -Rp[2]));
    L = m4_mul(L, m4_trans(Soff[0], Soff[1], Soff[2]));
    L = m4_mul(L, m4_trans(Sp[0], Sp[1], Sp[2]));
    L = m4_mul(L, m4_scale(S[0], S[1], S[2]));
    L = m4_mul(L, m4_trans(-Sp[0], -Sp[1], -Sp[2]));

    G = m4_trans(GT[0], GT[1], GT[2]);
    G = m4_mul(G, m4_euler(GR[0], GR[1], GR[2]));
    G = m4_mul(G, m4_scale(GS[0], GS[1], GS[2]));
    return m4_mul(L, G);
}

/* ------------------------------------------------------------------ loader */
#define FBX_MAX_POLY 256

typedef struct {
    long long id;
    FbxNode *node;
} FbxObj;

typedef struct {
    long long id;
    unsigned char rgb[3];
} FbxMat;

int fbx_load(const unsigned char *b, size_t n, int max_tris,
             RawMesh *out, char *err, int errsz) {
    FR r;
    FbxNode **tops = NULL;
    int ntop = 0, captop = 0;
    FbxNode *objects = NULL, *conns = NULL, *global = NULL;
    FbxObj *geos = NULL, *models = NULL;
    FbxMat *mats = NULL;
    int ngeo = 0, capgeo = 0, nmodel = 0, capmodel = 0, nmat = 0, capmat = 0;
    FVec V = {0};
    IVec T = {0};
    CVec F = {0};
    int has_colour = 0;
    int up_axis = 1;   /* FBX default: Y-up */
    int i, k;

    if (n < 27 || memcmp(b, "Kaydara FBX Binary  \x00", 21) != 0 || b[21] != 0x1A || b[22] != 0x00) {
        seterr(err, errsz, "not a binary FBX file (ASCII FBX is not supported)");
        return 0;
    }
    r.b = b; r.n = n; r.pos = 27; r.wide = (rd32(b + 23) >= 7500);
    r.nodes = 0;

    for (;;) {
        FbxNode *nd = NULL;
        if (!read_node(&r, &nd, 0)) break;
        if (ntop == captop) {
            int nc = captop ? captop * 2 : 8;
            FbxNode **np = (FbxNode **)realloc(tops, (size_t)nc * sizeof(FbxNode *));
            if (!np) { node_free(nd); break; }
            tops = np; captop = nc;
        }
        tops[ntop++] = nd;
    }
    for (i = 0; i < ntop; ++i) {
        if (!strcmp(tops[i]->name, "Objects")) objects = tops[i];
        else if (!strcmp(tops[i]->name, "Connections")) conns = tops[i];
        else if (!strcmp(tops[i]->name, "GlobalSettings")) global = tops[i];
    }
    if (!objects) {
        seterr(err, errsz, "FBX has no Objects section");
        goto fail;
    }

    if (global) {
        double v = 1.0;
        if (p70_num(global, "UpAxis", &v)) up_axis = (int)v;
    }

    /* collect objects */
    for (i = 0; i < objects->nkid; ++i) {
        FbxNode *nd = objects->kids[i];
        long long id = prop_i(nd, 0);
        if (!strcmp(nd->name, "Geometry")) {
            if (ngeo == capgeo) {
                int nc = capgeo ? capgeo * 2 : 16;
                FbxObj *np = (FbxObj *)realloc(geos, (size_t)nc * sizeof(FbxObj));
                if (!np) goto fail;
                geos = np; capgeo = nc;
            }
            geos[ngeo].id = id; geos[ngeo].node = nd; ++ngeo;
        } else if (!strcmp(nd->name, "Model")) {
            if (nmodel == capmodel) {
                int nc = capmodel ? capmodel * 2 : 16;
                FbxObj *np = (FbxObj *)realloc(models, (size_t)nc * sizeof(FbxObj));
                if (!np) goto fail;
                models = np; capmodel = nc;
            }
            models[nmodel].id = id; models[nmodel].node = nd; ++nmodel;
        } else if (!strcmp(nd->name, "Material")) {
            unsigned char rgb[3] = { 200, 200, 200 };
            FbxNode *p70 = find_child(nd, "Properties70");
            if (p70) {
                int j;
                for (j = 0; j < p70->nkid; ++j) {
                    FbxNode *p = p70->kids[j];
                    if (strcmp(p->name, "P") || strcmp(prop_s(p, 0), "DiffuseColor")) continue;
                    rgb[0] = (unsigned char)(prop_d(p, 4) * 255.0 + 0.5);
                    rgb[1] = (unsigned char)(prop_d(p, 5) * 255.0 + 0.5);
                    rgb[2] = (unsigned char)(prop_d(p, 6) * 255.0 + 0.5);
                    break;
                }
            }
            if (nmat == capmat) {
                int nc = capmat ? capmat * 2 : 16;
                FbxMat *np = (FbxMat *)realloc(mats, (size_t)nc * sizeof(FbxMat));
                if (!np) goto fail;
                mats = np; capmat = nc;
            }
            mats[nmat].id = id;
            mats[nmat].rgb[0] = rgb[0]; mats[nmat].rgb[1] = rgb[1]; mats[nmat].rgb[2] = rgb[2];
            ++nmat;
        }
    }

    /* emit every model that has a connected geometry */
    for (i = 0; i < nmodel; ++i) {
        FbxNode *geo = NULL;
        M4 xf;
        FbxNode *vtx, *pvi;
        const FbxProp *vp, *ip;
        int nv, ni, pi2, base;
        long long gid = 0;

        for (k = 0; k < ngeo; ++k)
            if (geos[k].id == models[i].id) { geo = geos[k].node; gid = geos[k].id; break; }
        if (!geo && conns) {
            /* find a Geometry child connected to this Model */
            int j;
            for (j = 0; j < conns->nkid; ++j) {
                FbxNode *c = conns->kids[j];
                if (strcmp(c->name, "C") || strcmp(prop_s(c, 0), "OO")) continue;
                if (prop_i(c, 2) != models[i].id) continue;
                for (k = 0; k < ngeo; ++k)
                    if (geos[k].id == prop_i(c, 1)) { geo = geos[k].node; gid = geos[k].id; break; }
                if (geo) break;
            }
        }
        if (!geo) continue;
        (void)gid;
        vtx = find_child(geo, "Vertices");
        pvi = find_child(geo, "PolygonVertexIndex");
        if (!vtx || !pvi) continue;
        vp = prop_at(vtx, 0);
        ip = prop_at(pvi, 0);
        if (!vp || !vp->a || !ip || !ip->a) continue;
        nv = vp->n / 3;
        ni = ip->n;
        if (nv <= 0 || ni <= 0) continue;

        xf = model_transform(models[i].node);

        /* material for this model (first connected Material) */
        {
            int mr = 200, mg = 200, mb = 200;
            int found = 0;
            if (conns) {
                int j;
                for (j = 0; j < conns->nkid; ++j) {
                    FbxNode *c = conns->kids[j];
                    long long cid;
                    if (strcmp(c->name, "C") || strcmp(prop_s(c, 0), "OO")) continue;
                    if (prop_i(c, 2) != models[i].id) continue;
                    cid = prop_i(c, 1);
                    for (k = 0; k < nmat; ++k)
                        if (mats[k].id == cid) {
                            mr = mats[k].rgb[0]; mg = mats[k].rgb[1]; mb = mats[k].rgb[2];
                            found = 1; break;
                        }
                    if (found) break;
                }
            }
            if (found) has_colour = 1;
            base = V.n / 3;
            if (base + nv < base) goto fail;

            /* vertices */
            for (k = 0; k < nv; ++k) {
                double x = arr_d(vp, k * 3 + 0), y = arr_d(vp, k * 3 + 1), z = arr_d(vp, k * 3 + 2);
                double ox, oy, oz;
                m4_apply(&xf, x, y, z, &ox, &oy, &oz);
                if (!fpush(&V, (float)ox) || !fpush(&V, (float)oy) || !fpush(&V, (float)oz)) goto fail;
            }

            /* polygons (negative index ends a polygon) */
            {
                int poly[FBX_MAX_POLY];
                int pn = 0;
                for (pi2 = 0; pi2 < ni; ++pi2) {
                    long long v = arr_i(ip, pi2);
                    int last = 0;
                    if (v < 0) { last = 1; v = -v - 1; }
                    if (pn < FBX_MAX_POLY) poly[pn++] = (int)v;
                    if (last) {
                        int t2;
                        for (t2 = 1; t2 + 1 < pn; ++t2) {
                            int a = poly[0], b2 = poly[t2], c2 = poly[t2 + 1];
                            if (a < 0 || b2 < 0 || c2 < 0 || a >= nv || b2 >= nv || c2 >= nv) continue;
                            if (!ipush(&T, (unsigned)(base + a)) ||
                                !ipush(&T, (unsigned)(base + b2)) ||
                                !ipush(&T, (unsigned)(base + c2))) goto fail;
                            if (!cpush(&F, (unsigned char)mr) ||
                                !cpush(&F, (unsigned char)mg) ||
                                !cpush(&F, (unsigned char)mb)) goto fail;
                        }
                        if (T.n / 3 > max_tris) goto done;
                        pn = 0;
                    }
                }
            }
        }
    }

done:
    if (V.n == 0 || T.n == 0) {
        seterr(err, errsz, "FBX contains no triangles");
        goto fail;
    }
    /* FBX is Y-up by convention; convert to the app's Z-up frame unless the
     * file says otherwise. */
    if (up_axis == 1) {
        for (i = 0; i < V.n / 3; ++i) {
            float x = V.p[i * 3 + 0], y = V.p[i * 3 + 1], z = V.p[i * 3 + 2];
            V.p[i * 3 + 0] = x;
            V.p[i * 3 + 1] = -z;
            V.p[i * 3 + 2] = y;
        }
    } else if (up_axis == 0) {
        for (i = 0; i < V.n / 3; ++i) {
            float x = V.p[i * 3 + 0], y = V.p[i * 3 + 1], z = V.p[i * 3 + 2];
            V.p[i * 3 + 0] = -z;
            V.p[i * 3 + 1] = y;
            V.p[i * 3 + 2] = x;
        }
    }

    out->nvert = V.n / 3;
    out->ntri = T.n / 3;
    out->verts = V.p;
    out->tris = T.p;
    if (has_colour && F.n >= 3) out->face_rgb = F.p;
    else { cfree(&F); out->face_rgb = NULL; }
    for (i = 0; i < ntop; ++i) node_free(tops[i]);
    free(tops); free(geos); free(models); free(mats);
    return 1;

fail:
    ffree(&V); ifree(&T); cfree(&F);
    for (i = 0; i < ntop; ++i) node_free(tops[i]);
    free(tops); free(geos); free(models); free(mats);
    return 0;
}
