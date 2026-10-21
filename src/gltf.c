/* ============================================================================
 * gltf.c - glTF 2.0 (.gltf) and binary glTF (.glb) reader.
 *
 * Parses the JSON (a small DOM), decodes accessors/bufferViews (buffers may be
 * the GLB BIN chunk, data: base64 URIs or external files), walks the node
 * hierarchy applying transforms, and emits triangle primitives with optional
 * material base colours. No external dependencies.
 * ==========================================================================*/
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include "loader.h"
#include "meshbuf.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------- helpers */
static unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned rd16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned char *read_file_w(const wchar_t *path, size_t *out_len) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER sz;
    unsigned char *b;
    DWORD total = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (128ull << 20)) { CloseHandle(h); return NULL; }
    b = (unsigned char *)malloc((size_t)sz.QuadPart);
    if (!b) { CloseHandle(h); return NULL; }
    while (total < (DWORD)sz.QuadPart) {
        DWORD got = 0;
        if (!ReadFile(h, b + total, (DWORD)(sz.QuadPart - total), &got, NULL) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    if (total != (DWORD)sz.QuadPart) { free(b); return NULL; }
    *out_len = (size_t)sz.QuadPart;
    return b;
}

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static unsigned char *b64_decode(const char *s, size_t n, size_t *out_len) {
    unsigned char *o = (unsigned char *)malloc(n / 4 * 3 + 4);
    size_t on = 0, i = 0;
    int quad[4], qn = 0;
    if (!o) return NULL;
    for (i = 0; i < n; ++i) {
        int v;
        if (s[i] == '=') break;
        v = b64val((unsigned char)s[i]);
        if (v < 0) continue;
        quad[qn++] = v;
        if (qn == 4) {
            o[on++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
            o[on++] = (unsigned char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
            o[on++] = (unsigned char)(((quad[2] & 3) << 6) | quad[3]);
            qn = 0;
        }
    }
    if (qn == 2) { o[on++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4)); }
    else if (qn == 3) {
        o[on++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
        o[on++] = (unsigned char)(((quad[1] & 15) << 4) | (quad[2] >> 2));
    }
    *out_len = on;
    return o;
}

/* ------------------------------------------------------------- JSON (DOM) */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal {
    JType type;
    double num;
    int boolean;
    char *str;
    struct JVal **items;
    char **keys;
    int n, cap;
} JVal;

static void jfree(JVal *v) {
    int i;
    if (!v) return;
    if (v->str) free(v->str);
    for (i = 0; i < v->n; ++i) jfree(v->items[i]);
    if (v->keys) { for (i = 0; i < v->n; ++i) free(v->keys[i]); free(v->keys); }
    free(v->items);
    free(v);
}
static JVal *jnew(JType t) { JVal *v = (JVal *)calloc(1, sizeof(JVal)); if (v) v->type = t; return v; }
static int jadd(JVal *v, char *key, JVal *item) {
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        JVal **ni = (JVal **)realloc(v->items, (size_t)nc * sizeof(JVal *));
        if (!ni) return 0;
        v->items = ni;
        if (v->type == J_OBJ) {
            char **nk = (char **)realloc(v->keys, (size_t)nc * sizeof(char *));
            if (!nk) return 0;
            v->keys = nk;
        }
        v->cap = nc;
    }
    if (v->type == J_OBJ) v->keys[v->n] = key;
    v->items[v->n++] = item;
    return 1;
}

typedef struct { const char *s, *e; } JP;
static void jws(JP *p) { while (p->s < p->e && (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')) ++p->s; }

static char *jstring(JP *p) {
    char *o;
    size_t cap = 32, n = 0;
    if (p->s >= p->e || *p->s != '"') return NULL;
    ++p->s;
    o = (char *)malloc(cap);
    if (!o) return NULL;
    while (p->s < p->e && *p->s != '"') {
        char c = *p->s++;
        if (c == '\\' && p->s < p->e) {
            char e = *p->s++;
            switch (e) {
            case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
            case 'b': c = '\b'; break; case 'f': c = '\f'; break;
            case '/': c = '/'; break; case '\\': c = '\\'; break; case '"': c = '"'; break;
            case 'u': {
                unsigned cp = 0; int k;
                for (k = 0; k < 4 && p->s < p->e; ++k) {
                    int h = p->s[0]; ++p->s;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                }
                /* encode as UTF-8 (BMP only) */
                if (cp < 0x80) c = (char)cp;
                else {
                    char tmp[3];
                    int tn = 0;
                    if (cp < 0x800) { tmp[tn++] = (char)(0xC0 | (cp >> 6)); tmp[tn++] = (char)(0x80 | (cp & 0x3F)); }
                    else { tmp[tn++] = (char)(0xE0 | (cp >> 12)); tmp[tn++] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[tn++] = (char)(0x80 | (cp & 0x3F)); }
                    { int kk; for (kk = 0; kk < tn; ++kk) { if (n + 1 >= cap) { cap *= 2; o = (char *)realloc(o, cap); } o[n++] = tmp[kk]; } }
                    continue;
                }
                break;
            }
            default: c = e; break;
            }
        }
        if (n + 1 >= cap) { cap *= 2; o = (char *)realloc(o, cap); if (!o) return NULL; }
        o[n++] = c;
    }
    if (p->s < p->e && *p->s == '"') ++p->s;
    o[n] = '\0';
    return o;
}

static JVal *jvalue(JP *p, int depth);

static JVal *jarray(JP *p, int depth) {
    JVal *v = jnew(J_ARR);
    ++p->s;                       /* '[' */
    jws(p);
    if (p->s < p->e && *p->s == ']') { ++p->s; return v; }
    for (;;) {
        JVal *it = jvalue(p, depth + 1);
        if (!it) { jfree(v); return NULL; }
        jadd(v, NULL, it);
        jws(p);
        if (p->s < p->e && *p->s == ',') { ++p->s; jws(p); continue; }
        if (p->s < p->e && *p->s == ']') { ++p->s; break; }
        jfree(v); return NULL;
    }
    return v;
}

static JVal *jobject(JP *p, int depth) {
    JVal *v = jnew(J_OBJ);
    ++p->s;                       /* '{' */
    jws(p);
    if (p->s < p->e && *p->s == '}') { ++p->s; return v; }
    for (;;) {
        char *key = jstring(p);
        JVal *it;
        if (!key) { jfree(v); return NULL; }
        jws(p);
        if (p->s >= p->e || *p->s != ':') { free(key); jfree(v); return NULL; }
        ++p->s; jws(p);
        it = jvalue(p, depth + 1);
        if (!it) { free(key); jfree(v); return NULL; }
        jadd(v, key, it);
        jws(p);
        if (p->s < p->e && *p->s == ',') { ++p->s; jws(p); continue; }
        if (p->s < p->e && *p->s == '}') { ++p->s; break; }
        jfree(v); return NULL;
    }
    return v;
}

static JVal *jvalue(JP *p, int depth) {
    if (depth > 128) return NULL;
    jws(p);
    if (p->s >= p->e) return NULL;
    switch (*p->s) {
    case '{': return jobject(p, depth);
    case '[': return jarray(p, depth);
    case '"': { JVal *v = jnew(J_STR); v->str = jstring(p); if (!v->str) { jfree(v); return NULL; } return v; }
    case 't': if (p->e - p->s >= 4 && !strncmp(p->s, "true", 4)) { JVal *v = jnew(J_BOOL); v->boolean = 1; p->s += 4; return v; } return NULL;
    case 'f': if (p->e - p->s >= 5 && !strncmp(p->s, "false", 5)) { JVal *v = jnew(J_BOOL); v->boolean = 0; p->s += 5; return v; } return NULL;
    case 'n': if (p->e - p->s >= 4 && !strncmp(p->s, "null", 4)) { JVal *v = jnew(J_NULL); p->s += 4; return v; } return NULL;
    default: {
        char *end;
        double d = strtod(p->s, &end);
        if (end == p->s) return NULL;
        p->s = end;
        { JVal *v = jnew(J_NUM); v->num = d; return v; }
    }
    }
}

static JVal *jget(JVal *o, const char *key) {
    int i;
    if (!o || o->type != J_OBJ) return NULL;
    for (i = 0; i < o->n; ++i) if (o->keys[i] && !strcmp(o->keys[i], key)) return o->items[i];
    return NULL;
}
static JVal *jidx(JVal *a, int i) {
    if (!a || a->type != J_ARR || i < 0 || i >= a->n) return NULL;
    return a->items[i];
}
static double jnum(JVal *v, double dflt) { return (v && v->type == J_NUM) ? v->num : dflt; }
static int jint(JVal *v, int dflt) { return (v && v->type == J_NUM) ? (int)v->num : dflt; }

/* ----------------------------------------------------------------- buffers */
typedef struct { unsigned char *data; size_t len; int owned; } Gbuf;

static int comp_size(int ct) {
    switch (ct) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    }
    return 0;
}
static int type_comps(const char *t) {
    if (!t) return 0;
    if (!strcmp(t, "SCALAR")) return 1;
    if (!strcmp(t, "VEC2")) return 2;
    if (!strcmp(t, "VEC3")) return 3;
    if (!strcmp(t, "VEC4")) return 4;
    if (!strcmp(t, "MAT2")) return 4;
    if (!strcmp(t, "MAT3")) return 9;
    if (!strcmp(t, "MAT4")) return 16;
    return 0;
}
static float comp_read(const unsigned char *p, int ct, int normalized) {
    switch (ct) {
    case 5120: { signed char v = (signed char)p[0]; return normalized ? (float)v / 127.0f : (float)v; }
    case 5121: return normalized ? (float)p[0] / 255.0f : (float)p[0];
    case 5122: { short v; memcpy(&v, p, 2); return normalized ? (float)v / 32767.0f : (float)v; }
    case 5123: { unsigned short v; memcpy(&v, p, 2); return normalized ? (float)v / 65535.0f : (float)v; }
    case 5125: { unsigned int v; memcpy(&v, p, 4); return (float)v; }
    case 5126: { float v; memcpy(&v, p, 4); return v; }
    }
    return 0.0f;
}

/* Read an accessor into a float array (count*comps) or an unsigned array. */
static float *acc_floats(JVal *root, Gbuf *bufs, int nb, int acc, int *count_out, int *comps_out) {
    JVal *accessors = jget(root, "accessors");
    JVal *bufferViews = jget(root, "bufferViews");
    JVal *a = jidx(accessors, acc);
    int ct, comps, count, bv_i, stride, cs;
    JVal *bv;
    float *out;
    const unsigned char *base;
    int i, k;
    if (!a) return NULL;
    ct = jint(jget(a, "componentType"), 0);
    comps = type_comps(jget(a, "type") ? jget(a, "type")->str : NULL);
    count = jint(jget(a, "count"), 0);
    bv_i = jint(jget(a, "bufferView"), -1);
    cs = comp_size(ct);
    if (cs == 0 || comps == 0 || count <= 0) return NULL;
    if (count_out) *count_out = count;
    if (comps_out) *comps_out = comps;
    out = (float *)calloc((size_t)count * comps, sizeof(float));
    if (!out) return NULL;
    if (bv_i < 0) return out;                     /* no data -> zeros */
    bv = jidx(bufferViews, bv_i);
    if (!bv) return out;
    {
        int buffer = jint(jget(bv, "buffer"), -1);
        int bv_off = jint(jget(bv, "byteOffset"), 0);
        int acc_off = jint(jget(a, "byteOffset"), 0);
        int bv_stride = jint(jget(bv, "byteStride"), 0);
        int bv_len = jint(jget(bv, "byteLength"), 0);
        int normalized = jint(jget(a, "normalized"), 0);
        if (buffer < 0 || buffer >= nb || !bufs[buffer].data) return out;
        stride = bv_stride ? bv_stride : comps * cs;
        base = bufs[buffer].data + bv_off + acc_off;
        if ((size_t)(bv_off + acc_off) > bufs[buffer].len) return out;
        (void)bv_len;
        for (i = 0; i < count; ++i) {
            const unsigned char *e = base + (size_t)i * stride;
            if ((size_t)(bv_off + acc_off + (size_t)i * stride + comps * cs) > bufs[buffer].len) break;
            for (k = 0; k < comps; ++k)
                out[i * comps + k] = comp_read(e + (size_t)k * cs, ct, normalized);
        }
    }
    return out;
}

static unsigned int *acc_uints(JVal *root, Gbuf *bufs, int nb, int acc, int *count_out) {
    JVal *accessors = jget(root, "accessors");
    JVal *bufferViews = jget(root, "bufferViews");
    JVal *a = jidx(accessors, acc);
    int ct, count, bv_i, cs;
    JVal *bv;
    unsigned int *out;
    const unsigned char *base;
    int i;
    if (!a) return NULL;
    ct = jint(jget(a, "componentType"), 0);
    count = jint(jget(a, "count"), 0);
    bv_i = jint(jget(a, "bufferView"), -1);
    cs = comp_size(ct);
    if (cs == 0 || count <= 0) return NULL;
    if (count_out) *count_out = count;
    out = (unsigned int *)calloc((size_t)count, sizeof(unsigned int));
    if (!out) return NULL;
    if (bv_i < 0) return out;
    bv = jidx(bufferViews, bv_i);
    if (!bv) return out;
    {
        int buffer = jint(jget(bv, "buffer"), -1);
        int bv_off = jint(jget(bv, "byteOffset"), 0);
        int acc_off = jint(jget(a, "byteOffset"), 0);
        int bv_stride = jint(jget(bv, "byteStride"), 0);
        int stride;
        if (buffer < 0 || buffer >= nb || !bufs[buffer].data) return out;
        stride = bv_stride ? bv_stride : cs;
        base = bufs[buffer].data + bv_off + acc_off;
        if ((size_t)(bv_off + acc_off) > bufs[buffer].len) return out;
        for (i = 0; i < count; ++i) {
            const unsigned char *e = base + (size_t)i * stride;
            if ((size_t)(bv_off + acc_off + (size_t)i * stride + cs) > bufs[buffer].len) break;
            switch (ct) {
            case 5121: out[i] = e[0]; break;
            case 5123: out[i] = rd16(e); break;
            case 5125: out[i] = rd32(e); break;
            default: out[i] = 0; break;
            }
        }
    }
    return out;
}

/* ------------------------------------------------------------------ matrix */
static void mat4_ident(double *m) { memset(m, 0, 16 * sizeof(double)); m[0] = m[5] = m[10] = m[15] = 1.0; }
static void mat4_mul(const double *A, const double *B, double *R) {
    int i, j, k;
    for (j = 0; j < 4; ++j)
        for (i = 0; i < 4; ++i) {
            double s = 0;
            for (k = 0; k < 4; ++k) s += A[k * 4 + i] * B[j * 4 + k];
            R[j * 4 + i] = s;
        }
}
static void trs_matrix(const double *t, const double *q, const double *s, double *m) {
    double x = q[0], y = q[1], z = q[2], w = q[3];
    double x2 = x + x, y2 = y + y, z2 = z + z;
    double xx = x * x2, xy = x * y2, xz = x * z2, yy = y * y2, yz = y * z2, zz = z * z2;
    double wx = w * x2, wy = w * y2, wz = w * z2;
    double r00 = 1 - (yy + zz), r01 = xy - wz, r02 = xz + wy;
    double r10 = xy + wz, r11 = 1 - (xx + zz), r12 = yz - wx;
    double r20 = xz - wy, r21 = yz + wx, r22 = 1 - (xx + yy);
    m[0] = r00 * s[0]; m[1] = r10 * s[0]; m[2] = r20 * s[0]; m[3] = 0;
    m[4] = r01 * s[1]; m[5] = r11 * s[1]; m[6] = r21 * s[1]; m[7] = 0;
    m[8] = r02 * s[2]; m[9] = r12 * s[2]; m[10] = r22 * s[2]; m[11] = 0;
    m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
}

/* ------------------------------------------------------------------ glTF */
typedef struct {
    JVal *root;
    Gbuf *bufs;
    int   nb;
    FVec *V;
    IVec *T;
    CVec *F;
    int   max_tris;
    long  budget;      /* node-visit budget (guards against DAG explosions) */
} Gctx;

static void emit_primitive(Gctx *g, JVal *prim, const double *world, const unsigned char *rgb) {
    JVal *attrs = jget(prim, "attributes");
    JVal *posacc = jget(attrs, "POSITION");
    int mode = jint(jget(prim, "mode"), 4);
    float *pos;
    int pcount = 0, pcomps = 0;
    unsigned int *idx = NULL;
    int icount = 0;
    int base, i;

    if (!posacc || (mode != 4 && mode != 5 && mode != 6)) return;
    if (g->T->n / 3 >= g->max_tris) return;
    pos = acc_floats(g->root, g->bufs, g->nb, jint(posacc, -1), &pcount, &pcomps);
    if (!pos || pcomps < 3) { free(pos); return; }
    if (pcount <= 0 || pcount > g->max_tris * 4) { free(pos); return; }

    base = g->V->n / 3;
    for (i = 0; i < pcount; ++i) {
        const float *p = &pos[i * pcomps];
        float o[3];
        o[0] = (float)(world[0] * p[0] + world[4] * p[1] + world[8] * p[2] + world[12]);
        o[1] = (float)(world[1] * p[0] + world[5] * p[1] + world[9] * p[2] + world[13]);
        o[2] = (float)(world[2] * p[0] + world[6] * p[1] + world[10] * p[2] + world[14]);
        if (!fpush(g->V, o[0]) || !fpush(g->V, o[1]) || !fpush(g->V, o[2])) break;
    }
    free(pos);

    {
        JVal *idacc = jget(prim, "indices");
        if (idacc) idx = acc_uints(g->root, g->bufs, g->nb, jint(idacc, -1), &icount);
    }
    {
        int nv = idx ? icount : pcount;
        for (i = 0; i + 2 < nv; i += (mode == 4 ? 3 : 1)) {
            int a, b, c;
            unsigned va, vb, vc;
            if (mode == 4) { a = i; b = i + 1; c = i + 2; }
            else if (mode == 5) { if (i & 1) { a = i + 1; b = i; c = i + 2; } else { a = i; b = i + 1; c = i + 2; } }
            else { a = 0; b = i + 1; c = i + 2; }
            va = idx ? idx[a] : (unsigned)a;
            vb = idx ? idx[b] : (unsigned)b;
            vc = idx ? idx[c] : (unsigned)c;
            if (va >= (unsigned)pcount || vb >= (unsigned)pcount || vc >= (unsigned)pcount) continue;
            if (!ipush(g->T, (unsigned)(base + va)) ||
                !ipush(g->T, (unsigned)(base + vb)) ||
                !ipush(g->T, (unsigned)(base + vc))) break;
            if (rgb) { cpush(g->F, rgb[0]); cpush(g->F, rgb[1]); cpush(g->F, rgb[2]); }
            if (g->T->n / 3 > g->max_tris) break;
        }
    }
    free(idx);
}

static void node_colour(Gctx *g, JVal *prim, unsigned char *rgb) {
    JVal *mats = jget(g->root, "materials");
    JVal *m = jidx(mats, jint(jget(prim, "material"), -1));
    JVal *pbr;
    JVal *bc;
    rgb[0] = rgb[1] = rgb[2] = 200;
    if (!m) return;
    pbr = jget(m, "pbrMetallicRoughness");
    bc = pbr ? jget(pbr, "baseColorFactor") : NULL;
    if (bc && bc->type == J_ARR && bc->n >= 3) {
        rgb[0] = (unsigned char)(jnum(jidx(bc, 0), 1.0) * 255.0 + 0.5);
        rgb[1] = (unsigned char)(jnum(jidx(bc, 1), 1.0) * 255.0 + 0.5);
        rgb[2] = (unsigned char)(jnum(jidx(bc, 2), 1.0) * 255.0 + 0.5);
    }
}

static void walk_node(Gctx *g, JVal *nodes, int node_idx, const double *parent, int depth) {
    JVal *node = jidx(nodes, node_idx);
    double local[16], world[16];
    JVal *mesh;
    int i;

    if (!node || depth > 64) return;
    if (--g->budget < 0) return;
    {
        JVal *mat = jget(node, "matrix");
        if (mat && mat->type == J_ARR && mat->n >= 16) {
            for (i = 0; i < 16; ++i) local[i] = jnum(jidx(mat, i), 0.0);
        } else {
            double t[3] = { 0, 0, 0 }, q[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };
            JVal *v;
            v = jget(node, "translation");
            if (v && v->type == J_ARR) for (i = 0; i < 3; ++i) t[i] = jnum(jidx(v, i), 0.0);
            v = jget(node, "rotation");
            if (v && v->type == J_ARR) for (i = 0; i < 4; ++i) q[i] = jnum(jidx(v, i), 0.0);
            v = jget(node, "scale");
            if (v && v->type == J_ARR) for (i = 0; i < 3; ++i) s[i] = jnum(jidx(v, i), 1.0);
            trs_matrix(t, q, s, local);
        }
    }
    mat4_mul(parent, local, world);

    mesh = jget(node, "mesh");
    if (mesh && mesh->type == J_NUM) {
        JVal *meshes = jget(g->root, "meshes");
        JVal *m = jidx(meshes, (int)mesh->num);
        JVal *prims = m ? jget(m, "primitives") : NULL;
        if (prims && prims->type == J_ARR) {
            int k;
            for (k = 0; k < prims->n; ++k) {
                unsigned char rgb[3];
                node_colour(g, prims->items[k], rgb);
                emit_primitive(g, prims->items[k], world, rgb);
            }
        }
    }
    {
        JVal *ch = jget(node, "children");
        if (ch && ch->type == J_ARR) {
            int k;
            for (k = 0; k < ch->n; ++k) walk_node(g, nodes, jint(ch->items[k], -1), world, depth + 1);
        }
    }
}

int gltf_load(const unsigned char *b, size_t n, const wchar_t *path,
              int max_tris, RawMesh *out, char *err, int errsz) {
    const char *json = NULL;
    size_t jsonlen = 0;
    const unsigned char *bin = NULL;
    size_t binlen = 0;
    JP p;
    JVal *root = NULL;
    JVal *buffers, *scenes, *nodes;
    Gbuf bufs[64];
    int nb = 0, i;
    FVec V = {0};
    IVec T = {0};
    CVec F = {0};
    Gctx g;
    double ident[16];

    if (n >= 12 && b[0] == 'g' && b[1] == 'l' && b[2] == 'T' && b[3] == 'F') {
        size_t off = 12;
        while (off + 8 <= n) {
            unsigned clen = rd32(b + off), ctype = rd32(b + off + 4);
            off += 8;
            if (off + clen > n) break;
            if (ctype == 0x4E4F534A) { json = (const char *)(b + off); jsonlen = clen; }
            else if (ctype == 0x004E4942) { bin = b + off; binlen = clen; }
            off += clen;
        }
    } else {
        json = (const char *)b;
        jsonlen = n;
    }
    if (!json) { seterr(err, errsz, "not a glTF/GLB file"); return 0; }

    p.s = json; p.e = json + jsonlen;
    root = jvalue(&p, 0);
    if (!root || root->type != J_OBJ) { jfree(root); seterr(err, errsz, "bad glTF JSON"); return 0; }
    {
        JVal *req = jget(root, "extensionsRequired");
        if (req && req->type == J_ARR) {
            int k;
            for (k = 0; k < req->n; ++k) {
                JVal *e = jidx(req, k);
                if (e && e->type == J_STR &&
                    (!strcmp(e->str, "KHR_draco_mesh_compression") ||
                     !strcmp(e->str, "EXT_meshopt_compression"))) {
                    jfree(root);
                    seterr(err, errsz, "compressed glTF is not supported");
                    return 0;
                }
            }
        }
    }

    memset(bufs, 0, sizeof(bufs));
    buffers = jget(root, "buffers");
    if (buffers && buffers->type == J_ARR) {
        for (i = 0; i < buffers->n && i < 64; ++i) {
            JVal *buf = buffers->items[i];
            JVal *uri = jget(buf, "uri");
            if (!uri || !uri->str || !uri->str[0]) {
                bufs[i].data = (unsigned char *)bin;   /* GLB BIN chunk */
                bufs[i].len = binlen;
                bufs[i].owned = 0;
            } else if (!strncmp(uri->str, "data:", 5)) {
                const char *comma = strchr(uri->str, ',');
                if (comma) {
                    size_t dl;
                    bufs[i].data = b64_decode(comma + 1, strlen(comma + 1), &dl);
                    bufs[i].len = dl;
                    bufs[i].owned = 1;
                }
            } else {
                /* external file, relative to the glTF file */
                wchar_t wp[MAX_PATH], wname[256];
                const wchar_t *slash = wcsrchr(path, L'\\');
                size_t dlen = slash ? (size_t)(slash - path + 1) : 0;
                size_t j;
                if (dlen < MAX_PATH - 2) {
                    wcsncpy(wp, path, dlen); wp[dlen] = 0;
                    for (j = 0; uri->str[j] && j < 255; ++j) wname[j] = (wchar_t)(unsigned char)uri->str[j];
                    wname[j] = 0;
                    if (wcslen(wp) + wcslen(wname) < MAX_PATH) {
                        size_t dl = 0;
                        wcscat(wp, wname);
                        bufs[i].data = read_file_w(wp, &dl);
                        bufs[i].len = dl;
                        bufs[i].owned = 1;
                    }
                }
            }
            nb = i + 1;
        }
    }

    g.root = root; g.bufs = bufs; g.nb = nb; g.V = &V; g.T = &T; g.F = &F; g.max_tris = max_tris;
    { JVal *nn = jget(root, "nodes"); g.budget = (long)(nn && nn->type == J_ARR ? nn->n : 0) * 32 + 1024; }
    mat4_ident(ident);
    nodes = jget(root, "nodes");
    scenes = jget(root, "scenes");
    {
        JVal *scene = jget(root, "scene");
        JVal *snodes = NULL;
        if (scenes && scenes->type == J_ARR) {
            JVal *sc = jidx(scenes, jint(scene, 0));
            if (sc) snodes = jget(sc, "nodes");
        }
        if (snodes && snodes->type == J_ARR) {
            int k;
            for (k = 0; k < snodes->n; ++k) walk_node(&g, nodes, jint(snodes->items[k], -1), ident, 0);
        } else if (nodes && nodes->type == J_ARR) {
            char *child = (char *)calloc((size_t)nodes->n, 1);
            if (child) {
                int k;
                for (i = 0; i < nodes->n; ++i) {
                    JVal *ch = jget(nodes->items[i], "children");
                    if (ch && ch->type == J_ARR)
                        for (k = 0; k < ch->n; ++k) {
                            int ci = jint(ch->items[k], -1);
                            if (ci >= 0 && ci < nodes->n) child[ci] = 1;
                        }
                }
                for (i = 0; i < nodes->n; ++i) if (!child[i]) walk_node(&g, nodes, i, ident, 0);
                free(child);
            } else {
                for (i = 0; i < nodes->n; ++i) walk_node(&g, nodes, i, ident, 0);
            }
        }
    }

    for (i = 0; i < nb; ++i) if (bufs[i].owned) free(bufs[i].data);
    jfree(root);

    out->nvert = V.n / 3;
    out->ntri = T.n / 3;
    if (out->nvert == 0 || out->ntri == 0) {
        ffree(&V); ifree(&T); cfree(&F);
        seterr(err, errsz, "no triangles");
        return 0;
    }
    out->verts = V.p;
    out->tris = T.p;
    if (F.n >= 3) out->face_rgb = F.p; else { cfree(&F); out->face_rgb = NULL; }
    return 1;
}
