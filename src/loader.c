/* ============================================================================
 * loader.c - minimal, bounds-checked readers for STL / OBJ / PLY.
 *
 * Everything is parsed into RawMesh (float verts, uint32 indices). Parsing is
 * defensive: counters are validated before allocation and everything is capped
 * by max_tris. No external dependencies.
 * ==========================================================================*/
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include "loader.h"

#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BYTES (128ull * 1024ull * 1024ull)

static void seterr(char *e, int n, const char *m) {
    if (n > 0) { strncpy(e, m, (size_t)n - 1); e[n - 1] = '\0'; }
}

/* --------------------------------------------------------- growable arrays */
typedef struct { float        *p; int n, cap; } FVec;
typedef struct { unsigned int *p; int n, cap; } IVec;

static int fpush(FVec *v, float x) {
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8192;
        float *np = (float *)realloc(v->p, (size_t)nc * sizeof(float));
        if (!np) return 0;
        v->p = np; v->cap = nc;
    }
    v->p[v->n++] = x;
    return 1;
}

static int ipush(IVec *v, unsigned int x) {
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8192;
        unsigned int *np = (unsigned int *)realloc(v->p, (size_t)nc * sizeof(unsigned int));
        if (!np) return 0;
        v->p = np; v->cap = nc;
    }
    v->p[v->n++] = x;
    return 1;
}

static void ffree(FVec *v) { free(v->p); v->p = NULL; v->n = v->cap = 0; }
static void ifree(IVec *v) { free(v->p); v->p = NULL; v->n = v->cap = 0; }

/* ------------------------------------------------------------- file access */
static unsigned char *read_file(const wchar_t *path, size_t *out_size,
                                char *err, int errsz) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER sz;
    unsigned char *b;
    DWORD total = 0;

    if (h == INVALID_HANDLE_VALUE) { seterr(err, errsz, "cannot open file"); return NULL; }
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0) {
        CloseHandle(h); seterr(err, errsz, "empty or unreadable file"); return NULL;
    }
    if ((unsigned long long)sz.QuadPart > MAX_BYTES) {
        CloseHandle(h); seterr(err, errsz, "file larger than 128 MB"); return NULL;
    }
    b = (unsigned char *)malloc((size_t)sz.QuadPart + 1);
    if (!b) { CloseHandle(h); seterr(err, errsz, "out of memory"); return NULL; }
    while (total < (DWORD)sz.QuadPart) {
        DWORD got = 0;
        if (!ReadFile(h, b + total, (DWORD)(sz.QuadPart - total), &got, NULL) || got == 0) break;
        total += got;
    }
    CloseHandle(h);
    if (total != (DWORD)sz.QuadPart) { free(b); seterr(err, errsz, "short read"); return NULL; }
    b[sz.QuadPart] = 0;
    *out_size = (size_t)sz.QuadPart;
    return b;
}

/* ------------------------------------------------------------------- STL */
static int parse_stl(const unsigned char *b, size_t n, int max_tris,
                     RawMesh *out, char *err, int errsz) {
    unsigned int cnt = 0;
    int binary = 0;
    if (n >= 84) {
        memcpy(&cnt, b + 80, 4);
        if ((size_t)cnt <= (n - 84) / 50) binary = 1;   /* plausible count */
    }
    if (binary) {
        unsigned int i;
        if ((int)cnt > max_tris) { seterr(err, errsz, "too many triangles"); return 0; }
        out->ntri = (int)cnt;
        out->nvert = (int)cnt * 3;
        out->verts = (float *)malloc(sizeof(float) * 9 * (size_t)cnt);
        out->tris = (unsigned int *)malloc(sizeof(unsigned int) * 3 * (size_t)cnt);
        if (!out->verts || !out->tris) { seterr(err, errsz, "out of memory"); return 0; }
        for (i = 0; i < cnt; ++i) {
            float f[12];
            memcpy(f, b + 84 + (size_t)i * 50, 48);     /* skip the stored normal */
            out->verts[i * 9 + 0] = f[3]; out->verts[i * 9 + 1] = f[4]; out->verts[i * 9 + 2] = f[5];
            out->verts[i * 9 + 3] = f[6]; out->verts[i * 9 + 4] = f[7]; out->verts[i * 9 + 5] = f[8];
            out->verts[i * 9 + 6] = f[9]; out->verts[i * 9 + 7] = f[10]; out->verts[i * 9 + 8] = f[11];
            out->tris[i * 3 + 0] = i * 3 + 0;
            out->tris[i * 3 + 1] = i * 3 + 1;
            out->tris[i * 3 + 2] = i * 3 + 2;
        }
        return 1;
    }

    /* ASCII STL */
    {
        FVec V = {0};
        const char *s = (const char *)b;
        while (*s) {
            const char *tok;
            size_t len;
            while (*s && isspace((unsigned char)*s)) ++s;
            if (!*s) break;
            tok = s;
            while (*s && !isspace((unsigned char)*s)) ++s;
            len = (size_t)(s - tok);
            if (len == 6 && _strnicmp(tok, "vertex", 6) == 0) {
                float x, y, z;
                char *end;
                x = strtof(s, &end); s = end;
                y = strtof(s, &end); s = end;
                z = strtof(s, &end); s = end;
                if (!fpush(&V, x) || !fpush(&V, y) || !fpush(&V, z)) {
                    ffree(&V); seterr(err, errsz, "out of memory"); return 0;
                }
                if (V.n / 9 > max_tris) { ffree(&V); seterr(err, errsz, "too many triangles"); return 0; }
            }
        }
        out->nvert = V.n / 3;
        out->ntri = out->nvert / 3;
        if (out->ntri == 0) { ffree(&V); seterr(err, errsz, "no triangles"); return 0; }
        out->nvert = out->ntri * 3;
        V.n = out->nvert * 3;
        out->verts = V.p;
        out->tris = (unsigned int *)malloc(sizeof(unsigned int) * 3 * (size_t)out->ntri);
        if (!out->tris) { ffree(&V); seterr(err, errsz, "out of memory"); return 0; }
        {
            int i;
            for (i = 0; i < out->ntri * 3; ++i) out->tris[i] = (unsigned int)i;
        }
        return 1;
    }
}

/* ------------------------------------------------------------------- OBJ */
/* Note: the buffer is modified in place (newlines are turned into NULs), so it
 * is taken non-const. It is the private read buffer, freed by the caller. */
static int parse_obj(unsigned char *b, size_t n, int max_tris,
                     RawMesh *out, char *err, int errsz) {
    FVec V = {0};
    IVec T = {0};
    char *buf = (char *)b;
    char *line, *next;
    if (n == 0) { seterr(err, errsz, "empty file"); return 0; }

    for (line = buf; line && *line; line = next) {
        char *p = line;
        next = strchr(line, '\n');
        if (next) *next++ = '\0';
        while (*p && (*p == ' ' || *p == '\t' || *p == '\r')) ++p;
        if (p[0] == 'v' && (p[1] == ' ' || p[1] == '\t')) {
            float x, y, z; char *end;
            x = strtof(p + 1, &end); y = strtof(end, &end); z = strtof(end, &end);
            if (!fpush(&V, x) || !fpush(&V, y) || !fpush(&V, z)) {
                ffree(&V); ifree(&T); seterr(err, errsz, "out of memory"); return 0;
            }
        } else if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
            unsigned int idx[64];
            int k = 0, i;
            char *t = p + 1;
            int nv = V.n / 3;
            while (*t) {
                long vi; char *end;
                while (*t == ' ' || *t == '\t') ++t;
                if (!*t) break;
                vi = strtol(t, &end, 10);
                if (end == t) { ++t; continue; }
                if (vi > 0) vi = vi - 1;
                else if (vi < 0) vi = nv + vi;
                else { t = end; continue; }
                if (vi < 0 || vi >= nv) { t = end; continue; }
                if (k < 64) idx[k++] = (unsigned int)vi;
                t = end;
            }
            for (i = 2; i < k; ++i) {              /* fan triangulate */
                if (!ipush(&T, idx[0]) || !ipush(&T, idx[i - 1]) || !ipush(&T, idx[i])) {
                    ffree(&V); ifree(&T); seterr(err, errsz, "out of memory"); return 0;
                }
                if (T.n / 3 > max_tris) {
                    ffree(&V); ifree(&T); seterr(err, errsz, "too many triangles"); return 0;
                }
            }
        }
    }
    out->nvert = V.n / 3;
    out->ntri = T.n / 3;
    if (out->nvert == 0 || out->ntri == 0) {
        ffree(&V); ifree(&T); seterr(err, errsz, "no triangles"); return 0;
    }
    out->verts = V.p;
    out->tris = T.p;
    return 1;
}

/* ------------------------------------------------------------------- PLY */
static int ply_type_size(const char *t) {
    if (!strcmp(t, "char") || !strcmp(t, "int8") || !strcmp(t, "uchar") || !strcmp(t, "uint8")) return 1;
    if (!strcmp(t, "short") || !strcmp(t, "int16") || !strcmp(t, "ushort") || !strcmp(t, "uint16")) return 2;
    if (!strcmp(t, "int") || !strcmp(t, "int32") || !strcmp(t, "uint") || !strcmp(t, "uint32")
        || !strcmp(t, "float") || !strcmp(t, "float32")) return 4;
    if (!strcmp(t, "double") || !strcmp(t, "float64")) return 8;
    return 0;
}

static double ply_bin_value(const unsigned char *p, const char *t) {
    if (!strcmp(t, "float") || !strcmp(t, "float32")) { float v; memcpy(&v, p, 4); return v; }
    if (!strcmp(t, "double") || !strcmp(t, "float64")) { double v; memcpy(&v, p, 8); return v; }
    if (!strcmp(t, "char") || !strcmp(t, "int8")) return (double)(signed char)p[0];
    if (!strcmp(t, "uchar") || !strcmp(t, "uint8")) return (double)p[0];
    if (!strcmp(t, "short") || !strcmp(t, "int16")) { short v; memcpy(&v, p, 2); return v; }
    if (!strcmp(t, "ushort") || !strcmp(t, "uint16")) { unsigned short v; memcpy(&v, p, 2); return v; }
    if (!strcmp(t, "int") || !strcmp(t, "int32")) { int v; memcpy(&v, p, 4); return v; }
    if (!strcmp(t, "uint") || !strcmp(t, "uint32")) { unsigned int v; memcpy(&v, p, 4); return v; }
    return 0.0;
}

static int parse_ply(const unsigned char *b, size_t n, int max_tris,
                     RawMesh *out, char *err, int errsz) {
    /* header */
    const char *hdr = (const char *)b;
    const char *data = NULL;
    const char *e;
    int ascii = -1;
    int vcount = -1, fcount = -1;
    char vtype[64][16];
    char vname[64][32];
    int  vn = 0, vstride = 0;
    int vx = -1, vy = -1, vz = -1, xoff = 0, yoff = 0, zoff = 0;
    char fctype[16] = "uchar", fittype[16] = "int";
    int in_vertex = 0, in_face = 0;

    e = strstr(hdr, "end_header");
    if (!e || (size_t)(e - hdr) >= n) { seterr(err, errsz, "bad PLY header"); return 0; }
    data = e + strlen("end_header");
    if (data < (const char *)b + n && *data == '\r') ++data;
    if (data < (const char *)b + n && *data == '\n') ++data;

    /* walk header lines */
    {
        char hbuf[256];
        size_t pos = 0;
        while (pos < n) {
            size_t len = 0;
            const char *nl = strchr(hdr + pos, '\n');
            if (!nl || (size_t)(nl - hdr) > n) break;
            len = (size_t)(nl - hdr) - pos;
            if (len >= sizeof(hbuf)) len = sizeof(hbuf) - 1;
            memcpy(hbuf, hdr + pos, len); hbuf[len] = '\0';
            pos = (size_t)(nl - hdr) + 1;
            if (strncmp(hbuf, "end_header", 10) == 0) break;
            if (strncmp(hbuf, "format", 6) == 0) {
                if (strstr(hbuf, "ascii")) ascii = 1;
                else if (strstr(hbuf, "binary_little_endian")) ascii = 0;
                else { seterr(err, errsz, "unsupported PLY format"); return 0; }
            } else if (strncmp(hbuf, "element", 7) == 0) {
                char name[32]; int cnt = 0;
                if (sscanf(hbuf, "element %31s %d", name, &cnt) == 2) {
                    in_vertex = (strcmp(name, "vertex") == 0);
                    in_face = (strcmp(name, "face") == 0);
                    if (in_vertex) vcount = cnt;
                    else if (in_face) fcount = cnt;
                }
            } else if (strncmp(hbuf, "property", 8) == 0 && in_vertex) {
                char a[16], b2[32];
                if (sscanf(hbuf, "property %15s %31s", a, b2) == 2 && vn < 64) {
                    int ts = ply_type_size(a);
                    strcpy(vtype[vn], a); strcpy(vname[vn], b2);
                    if (strcmp(b2, "x") == 0) { vx = vn; xoff = vstride; }
                    else if (strcmp(b2, "y") == 0) { vy = vn; yoff = vstride; }
                    else if (strcmp(b2, "z") == 0) { vz = vn; zoff = vstride; }
                    vstride += ts;
                    ++vn;
                }
            } else if (strncmp(hbuf, "property", 8) == 0 && in_face) {
                char a[16], b2[16], c[32];
                if (sscanf(hbuf, "property list %15s %15s %31s", a, b2, c) == 3) {
                    strcpy(fctype, a); strcpy(fittype, b2);
                }
            }
        }
    }
    if (ascii < 0 || vcount < 0 || fcount < 0 || vx < 0 || vy < 0 || vz < 0) {
        seterr(err, errsz, "unsupported PLY layout"); return 0;
    }
    if (fcount > max_tris) { seterr(err, errsz, "too many triangles"); return 0; }

    out->nvert = vcount;
    out->ntri = 0;
    out->verts = (float *)malloc(sizeof(float) * 3 * (size_t)vcount);
    out->tris = (unsigned int *)malloc(sizeof(unsigned int) * 3 * (size_t)fcount);
    if (!out->verts || !out->tris) { seterr(err, errsz, "out of memory"); return 0; }

    if (ascii) {
        const char *s = data;
        int i, k;
        for (i = 0; i < vcount; ++i) {
            float xyz[3] = {0, 0, 0};
            for (k = 0; k < vn; ++k) {
                char *end;
                float val = strtof(s, &end);
                s = end;
                if (k == vx) xyz[0] = val;
                else if (k == vy) xyz[1] = val;
                else if (k == vz) xyz[2] = val;
            }
            out->verts[i * 3 + 0] = xyz[0];
            out->verts[i * 3 + 1] = xyz[1];
            out->verts[i * 3 + 2] = xyz[2];
        }
        for (i = 0; i < fcount; ++i) {
            char *end;
            long c = strtol(s, &end, 10);
            unsigned int idx[64];
            s = end;
            if (c > 64) c = 64;
            for (k = 0; k < c; ++k) { idx[k] = (unsigned int)strtol(s, &end, 10); s = end; }
            for (k = 2; k < c; ++k) {
                out->tris[out->ntri * 3 + 0] = idx[0];
                out->tris[out->ntri * 3 + 1] = idx[k - 1];
                out->tris[out->ntri * 3 + 2] = idx[k];
                ++out->ntri;
            }
        }
    } else {
        const unsigned char *p = (const unsigned char *)data;
        int i;
        for (i = 0; i < vcount; ++i) {
            const unsigned char *rec = p + (size_t)i * vstride;
            out->verts[i * 3 + 0] = (float)ply_bin_value(rec + xoff, vtype[vx]);
            out->verts[i * 3 + 1] = (float)ply_bin_value(rec + yoff, vtype[vy]);
            out->verts[i * 3 + 2] = (float)ply_bin_value(rec + zoff, vtype[vz]);
        }
        p += (size_t)vcount * vstride;
        {
            int cts = ply_type_size(fctype), its = ply_type_size(fittype);
            for (i = 0; i < fcount; ++i) {
                int c = (int)ply_bin_value(p, fctype); p += cts;
                unsigned int idx[64];
                int k;
                if (c > 64) c = 64;
                for (k = 0; k < c; ++k) { idx[k] = (unsigned int)ply_bin_value(p, fittype); p += its; }
                for (k = 2; k < c; ++k) {
                    out->tris[out->ntri * 3 + 0] = idx[0];
                    out->tris[out->ntri * 3 + 1] = idx[k - 1];
                    out->tris[out->ntri * 3 + 2] = idx[k];
                    ++out->ntri;
                }
            }
        }
    }
    if (out->ntri == 0) { seterr(err, errsz, "no triangles"); return 0; }
    return 1;
}

/* --------------------------------------------------------------- dispatch */
int loader_load(const wchar_t *path, int max_tris, RawMesh *out, char *err, int errsz) {
    const wchar_t *ext = wcsrchr(path, L'.');
    unsigned char *b;
    size_t n = 0;
    int ok = 0;

    out->verts = NULL; out->tris = NULL; out->nvert = out->ntri = 0;
    if (!ext) { seterr(err, errsz, "no extension"); return 0; }
    b = read_file(path, &n, err, errsz);
    if (!b) return 0;

    if (_wcsicmp(ext, L".stl") == 0)       ok = parse_stl(b, n, max_tris, out, err, errsz);
    else if (_wcsicmp(ext, L".obj") == 0)  ok = parse_obj(b, n, max_tris, out, err, errsz);
    else if (_wcsicmp(ext, L".ply") == 0)  ok = parse_ply(b, n, max_tris, out, err, errsz);
    else seterr(err, errsz, "unsupported format");

    free(b);
    if (!ok) { free(out->verts); free(out->tris); out->verts = NULL; out->tris = NULL; }
    return ok;
}
