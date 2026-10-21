/* ============================================================================
 * zip3mf.c - 3MF reader (OPC/ZIP package + XML model).
 *
 * Unpacks the ZIP (stored or deflated entries) with the local inflate, then
 * scans the model XML for objects/meshes, base materials and build items.
 * No external dependencies.
 * ==========================================================================*/
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include "loader.h"
#include "meshbuf.h"
#include "inflate.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* --------------------------------------------------------------- ZIP part */
static unsigned char *zip_extract(const unsigned char *b, size_t n, const char *suffix, size_t *outlen) {
    size_t eocd = (size_t)-1;
    size_t i;
    size_t cd_off, cd_n, cd_pos;
    size_t sl = strlen(suffix);

    if (n < 22) return NULL;
    for (i = n - 22 + 1; i-- > 0; ) {
        if (rd32(b + i) == 0x06054b50u) { eocd = i; break; }
    }
    if (eocd == (size_t)-1) return NULL;
    cd_off = rd32(b + eocd + 16);
    cd_n = rd16(b + eocd + 10);
    cd_pos = cd_off;

    for (i = 0; i < cd_n; ++i) {
        unsigned method, csize, fnlen, extralen, commentlen, lho;
        const char *name;
        size_t nl;
        if (cd_pos + 46 > n || rd32(b + cd_pos) != 0x02014b50u) return NULL;
        method = rd16(b + cd_pos + 10);
        csize = rd32(b + cd_pos + 20);
        fnlen = rd16(b + cd_pos + 28);
        extralen = rd16(b + cd_pos + 30);
        commentlen = rd16(b + cd_pos + 32);
        lho = rd32(b + cd_pos + 42);
        if ((size_t)cd_pos + 46 + (size_t)fnlen + extralen + commentlen > n) return NULL;
        name = (const char *)(b + cd_pos + 46);
        nl = fnlen;
        if (nl >= sl && _strnicmp(name + nl - sl, suffix, sl) == 0) {
            /* local header */
            unsigned lfn, lex;
            const unsigned char *data;
            if ((size_t)lho + 30 > n || rd32(b + lho) != 0x04034b50u) return NULL;
            lfn = rd16(b + lho + 26);
            lex = rd16(b + lho + 28);
            if ((size_t)lho + 30 + (size_t)lfn + lex > n) return NULL;
            data = b + (size_t)lho + 30 + lfn + lex;
            if ((unsigned long long)csize > (unsigned long long)(n - (size_t)(data - b))) return NULL;
            if (method == 0) {
                unsigned char *o = (unsigned char *)malloc(csize + 1);
                if (!o) return NULL;
                memcpy(o, data, csize);
                o[csize] = 0;
                *outlen = csize;
                return o;
            } else if (method == 8) {
                return inflate_raw(data, csize, outlen);
            }
            return NULL;
        }
        cd_pos += 46 + fnlen + extralen + commentlen;
    }
    return NULL;
}

/* --------------------------------------------------------------- XML tags */
static const char *next_tag(const char *s, const char *e, char *name, int nsz,
                            char *attrs, int asz, int *closing) {
    const char *t;
    for (;;) {
        while (s < e && *s != '<') ++s;
        if (s >= e) return NULL;
        if (e - s >= 4 && !strncmp(s, "<!--", 4)) {
            const char *q = strstr(s + 4, "-->");
            if (!q) return NULL;
            s = q + 3;
            continue;
        }
        if (e - s >= 9 && !strncmp(s, "<![CDATA[", 9)) {
            const char *q = strstr(s + 9, "]]>");
            if (!q) return NULL;
            s = q + 3;
            continue;
        }
        if (e - s >= 2 && s[1] == '?') {
            const char *q = strstr(s + 2, "?>");
            if (!q) return NULL;
            s = q + 2;
            continue;
        }
        break;
    }
    t = s + 1;
    *closing = 0;
    if (t < e && *t == '/') { *closing = 1; ++t; }
    {
        int k = 0;
        while (t < e && !isspace((unsigned char)*t) && *t != '>' && *t != '/' && k < nsz - 1)
            name[k++] = *t++;
        name[k] = 0;
    }
    {
        int k = 0;
        while (t < e && *t != '>' && k < asz - 1) attrs[k++] = *t++;
        attrs[k] = 0;
        if (t < e && *t == '>') ++t;
    }
    {
        int k = (int)strlen(attrs);
        while (k > 0 && (attrs[k - 1] == '/' || attrs[k - 1] == ' ')) attrs[--k] = 0;
    }
    return t;
}

static int attr_str(const char *attrs, const char *key, char *out, int outsz) {
    size_t kl = strlen(key);
    const char *p = attrs;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == attrs || isspace((unsigned char)p[-1]) || p[-1] == '/') && p[kl] == '=') {
            const char *q = p + kl + 1;
            int k = 0;
            if (*q == '"' || *q == '\'') {
                char qc = *q++;
                while (*q && *q != qc && k < outsz - 1) out[k++] = *q++;
            } else {
                while (*q && !isspace((unsigned char)*q) && *q != '/' && k < outsz - 1) out[k++] = *q++;
            }
            out[k] = 0;
            return 1;
        }
        p += kl;
    }
    return 0;
}
static int attr_num(const char *attrs, const char *key, double *out) {
    char tmp[64];
    if (!attr_str(attrs, key, tmp, sizeof(tmp))) return 0;
    *out = strtod(tmp, NULL);
    return 1;
}
static int attr_int(const char *attrs, const char *key, int *out) {
    char tmp[64];
    if (!attr_str(attrs, key, tmp, sizeof(tmp))) return 0;
    *out = atoi(tmp);
    return 1;
}

#define MF3_MAXOBJ 256
#define MF3_MAXMAT 128
#define MF3_MAXCOL 64

typedef struct { int objid, has; double m[12]; } Mf3Comp;

typedef struct {
    int id, used;
    FVec V;
    IVec T;
    IVec Mp, Mi;      /* per-triangle material id + index */
    int defpid, defp1;
    Mf3Comp comp[8];
    int ncomp;
} Mf3Obj;

typedef struct {
    int id, used, n;
    unsigned char col[MF3_MAXCOL * 3];
} Mf3Mat;

typedef struct { int objid, has; double m[12]; } Mf3Item;

/* Compose two 3MF 4x3 transforms: result = a * b (row-vector convention). */
static void mf3_mul(const double *a, const double *b, double *o) {
    int r, c, k;
    for (r = 0; r < 4; ++r) {
        for (c = 0; c < 3; ++c) {
            double sum = 0.0;
            for (k = 0; k < 3; ++k) sum += a[r * 3 + k] * b[k * 3 + c];
            if (r == 3) sum += b[9 + c];
            o[r * 3 + c] = sum;
        }
    }
}

/* Emit one object's mesh. 0 = OOM, 2 = triangle cap, 1 = ok. */
static int mf3_emit_mesh(Mf3Obj *o, const double *m, int hasm,
                         Mf3Mat *mats, int nmat,
                         FVec *V, IVec *T, CVec *F, int max_tris) {
    int base, vi, ti, k;
    if (!o || o->V.n < 3 || o->T.n < 3) return 1;
    base = V->n / 3;
    for (vi = 0; vi < o->V.n / 3; ++vi) {
        const float *p = &o->V.p[vi * 3];
        float x, y, z;
        if (hasm) {
            x = (float)(p[0] * m[0] + p[1] * m[3] + p[2] * m[6] + m[9]);
            y = (float)(p[0] * m[1] + p[1] * m[4] + p[2] * m[7] + m[10]);
            z = (float)(p[0] * m[2] + p[1] * m[5] + p[2] * m[8] + m[11]);
        } else { x = p[0]; y = p[1]; z = p[2]; }
        if (!fpush(V, x) || !fpush(V, y) || !fpush(V, z)) return 0;
    }
    for (ti = 0; ti < o->T.n / 3; ++ti) {
        unsigned a = o->T.p[ti * 3 + 0], b2 = o->T.p[ti * 3 + 1], c = o->T.p[ti * 3 + 2];
        int pid = (int)o->Mp.p[ti], p1 = (int)o->Mi.p[ti];
        unsigned char rgb[3] = { 200, 200, 200 };
        if (pid < 0) { pid = o->defpid; p1 = o->defp1; }
        if (pid > 0) {
            for (k = 0; k < nmat; ++k) {
                if (mats[k].id == pid && p1 >= 0 && p1 < mats[k].n) {
                    rgb[0] = mats[k].col[p1 * 3 + 0];
                    rgb[1] = mats[k].col[p1 * 3 + 1];
                    rgb[2] = mats[k].col[p1 * 3 + 2];
                    break;
                }
            }
        }
        if ((int)a >= o->V.n / 3 || (int)b2 >= o->V.n / 3 || (int)c >= o->V.n / 3) continue;
        if (!ipush(T, (unsigned)(base + a)) || !ipush(T, (unsigned)(base + b2)) ||
            !ipush(T, (unsigned)(base + c))) return 0;
        if (!cpush(F, rgb[0]) || !cpush(F, rgb[1]) || !cpush(F, rgb[2])) return 0;
        if (T->n / 3 > max_tris) return 2;
    }
    return 1;
}

static int mf3_emit_obj(Mf3Obj *objs, int nobj, Mf3Obj *o, const double *m, int hasm,
                        Mf3Mat *mats, int nmat, FVec *V, IVec *T, CVec *F,
                        int max_tris, int depth) {
    int k, rc;
    if (!o || depth > 16) return 1;
    rc = mf3_emit_mesh(o, m, hasm, mats, nmat, V, T, F, max_tris);
    if (rc != 1) return rc;
    for (k = 0; k < o->ncomp; ++k) {
        Mf3Obj *co = NULL;
        double cm[12];
        int chasm = 0, j;
        for (j = 0; j < nobj; ++j) if (objs[j].id == o->comp[k].objid) { co = &objs[j]; break; }
        if (!co) continue;
        if (o->comp[k].has) {
            if (hasm) mf3_mul(o->comp[k].m, m, cm);
            else { for (j = 0; j < 12; ++j) cm[j] = o->comp[k].m[j]; }
            chasm = 1;
        } else if (hasm) {
            for (j = 0; j < 12; ++j) cm[j] = m[j];
            chasm = 1;
        }
        rc = mf3_emit_obj(objs, nobj, co, cm, chasm, mats, nmat, V, T, F, max_tris, depth + 1);
        if (rc != 1) return rc;
    }
    return 1;
}

int mf3_load(const unsigned char *b, size_t n, int max_tris,
             RawMesh *out, char *err, int errsz) {
    unsigned char *xml;
    size_t xmllen = 0;
    const char *s, *e;
    Mf3Obj objs[MF3_MAXOBJ];
    Mf3Mat mats[MF3_MAXMAT];
    Mf3Item items[256];    int nobj = 0, nmat = 0, nitem = 0;
    Mf3Obj *cur = NULL;
    Mf3Mat *curmat = NULL;
    FVec V = {0};
    IVec T = {0};
    CVec F = {0};
    char name[64], attrs[2048];
    int closing, i, k;

    memset(objs, 0, sizeof(objs));
    memset(mats, 0, sizeof(mats));

    xml = zip_extract(b, n, ".model", &xmllen);
    if (!xml) { seterr(err, errsz, "not a 3MF package (no model part)"); return 0; }

    s = (const char *)xml;
    e = s + xmllen;
    while ((s = next_tag(s, e, name, sizeof(name), attrs, sizeof(attrs), &closing)) != NULL) {
        if (!closing) {
            if (!strcmp(name, "basematerials")) {
                if (nmat < MF3_MAXMAT) {
                    curmat = &mats[nmat];
                    curmat->id = 0; curmat->n = 0; curmat->used = 1;
                    attr_int(attrs, "id", &curmat->id);
                    ++nmat;
                } else {
                    curmat = NULL;
                }
            } else if (!strcmp(name, "base")) {
                if (curmat && curmat->n < MF3_MAXCOL) {
                    char dc[32];
                    if (attr_str(attrs, "displaycolor", dc, sizeof(dc)) && dc[0] == '#') {
                        unsigned r = 0, g = 0, b2 = 0;
                        if (sscanf(dc + 1, "%2x%2x%2x", &r, &g, &b2) == 3) {
                            curmat->col[curmat->n * 3 + 0] = (unsigned char)r;
                            curmat->col[curmat->n * 3 + 1] = (unsigned char)g;
                            curmat->col[curmat->n * 3 + 2] = (unsigned char)b2;
                            ++curmat->n;
                        }
                    }
                }
            } else if (!strcmp(name, "object")) {
                if (nobj < MF3_MAXOBJ) {
                    cur = &objs[nobj];
                    cur->id = 0; cur->defpid = 0; cur->defp1 = 0; cur->used = 1;
                    attr_int(attrs, "id", &cur->id);
                    attr_int(attrs, "pid", &cur->defpid);
                    attr_int(attrs, "pindex", &cur->defp1);
                    ++nobj;
                } else {
                    cur = NULL;
                }
            } else if (!strcmp(name, "vertex")) {
                double x = 0, y = 0, z = 0;
                if (cur) {
                    attr_num(attrs, "x", &x); attr_num(attrs, "y", &y); attr_num(attrs, "z", &z);
                    fpush(&cur->V, (float)x); fpush(&cur->V, (float)y); fpush(&cur->V, (float)z);
                }
            } else if (!strcmp(name, "triangle")) {
                int v1 = -1, v2 = -1, v3 = -1, pid = -1, p1 = -1;
                if (cur) {
                    attr_int(attrs, "v1", &v1); attr_int(attrs, "v2", &v2); attr_int(attrs, "v3", &v3);
                    if (!attr_int(attrs, "pid", &pid)) pid = -1;
                    if (!attr_int(attrs, "p1", &p1)) p1 = -1;
                    if (v1 >= 0 && v2 >= 0 && v3 >= 0) {
                        ipush(&cur->T, (unsigned)v1); ipush(&cur->T, (unsigned)v2); ipush(&cur->T, (unsigned)v3);
                        ipush(&cur->Mp, (unsigned)pid);
                        ipush(&cur->Mi, (unsigned)p1);
                    }
                }
            } else if (!strcmp(name, "component")) {
                if (cur && cur->ncomp < 8) {
                    char tr[512];
                    Mf3Comp *cp = &cur->comp[cur->ncomp];
                    cp->objid = 0; cp->has = 0;
                    attr_int(attrs, "objectid", &cp->objid);
                    if (attr_str(attrs, "transform", tr, sizeof(tr))) {
                        double m[12];
                        int got = sscanf(tr, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
                                         &m[0], &m[1], &m[2], &m[3], &m[4], &m[5],
                                         &m[6], &m[7], &m[8], &m[9], &m[10], &m[11]);
                        if (got == 12) { int j; for (j = 0; j < 12; ++j) cp->m[j] = m[j]; cp->has = 1; }
                    }
                    cur->ncomp++;
                }
            } else if (!strcmp(name, "item")) {
                if (nitem < 256) {
                    char tr[512];
                    items[nitem].objid = 0; items[nitem].has = 0;
                    attr_int(attrs, "objectid", &items[nitem].objid);
                    if (attr_str(attrs, "transform", tr, sizeof(tr))) {
                        double m[12];
                        int got = sscanf(tr, "%lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf",
                                         &m[0], &m[1], &m[2], &m[3], &m[4], &m[5],
                                         &m[6], &m[7], &m[8], &m[9], &m[10], &m[11]);
                        if (got == 12) {
                            for (k = 0; k < 12; ++k) items[nitem].m[k] = m[k];
                            items[nitem].has = 1;
                        }
                    }
                    ++nitem;
                }
            }
        } else {
            if (!strcmp(name, "basematerials")) curmat = NULL;
            else if (!strcmp(name, "object")) cur = NULL;
        }
    }

    /* emit objects (via build items, or all if there are none) */
    {
        int total_items = nitem > 0 ? nitem : nobj;
        int rc = 1;
        for (i = 0; i < total_items && rc == 1; ++i) {
            Mf3Obj *o;
            double m[12];
            int hasm;
            if (nitem > 0) {
                o = NULL;
                for (k = 0; k < nobj; ++k) if (objs[k].id == items[i].objid) { o = &objs[k]; break; }
                hasm = items[i].has;
                for (k = 0; k < 12; ++k) m[k] = items[i].m[k];
            } else {
                o = &objs[i];
                hasm = 0;
                for (k = 0; k < 12; ++k) m[k] = 0.0;
            }
            if (!o) continue;
            rc = mf3_emit_obj(objs, nobj, o, m, hasm, mats, nmat, &V, &T, &F, max_tris, 0);
        }
    }

    for (i = 0; i < nobj; ++i) { ffree(&objs[i].V); ifree(&objs[i].T); ifree(&objs[i].Mp); ifree(&objs[i].Mi); }
    free(xml);

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
