/* ============================================================================
 * meshbuf.c - small growable arrays shared by the mesh loaders.
 * ==========================================================================*/
#include "meshbuf.h"

#include <stdlib.h>

int fpush(FVec *v, float x) {
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8192;
        float *np = (float *)realloc(v->p, (size_t)nc * sizeof(float));
        if (!np) return 0;
        v->p = np; v->cap = nc;
    }
    v->p[v->n++] = x;
    return 1;
}

int ipush(IVec *v, unsigned int x) {
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8192;
        unsigned int *np = (unsigned int *)realloc(v->p, (size_t)nc * sizeof(unsigned int));
        if (!np) return 0;
        v->p = np; v->cap = nc;
    }
    v->p[v->n++] = x;
    return 1;
}

int cpush(CVec *v, unsigned char x) {
    if (v->n == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8192;
        unsigned char *np = (unsigned char *)realloc(v->p, (size_t)nc);
        if (!np) return 0;
        v->p = np; v->cap = nc;
    }
    v->p[v->n++] = x;
    return 1;
}

void ffree(FVec *v) { free(v->p); v->p = NULL; v->n = v->cap = 0; }
void ifree(IVec *v) { free(v->p); v->p = NULL; v->n = v->cap = 0; }
void cfree(CVec *v) { free(v->p); v->p = NULL; v->n = v->cap = 0; }
