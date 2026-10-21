#ifndef MESHBUF_H
#define MESHBUF_H

/* Small growable arrays shared by the mesh loaders. */

typedef struct { float         *p; int n, cap; } FVec;
typedef struct { unsigned int  *p; int n, cap; } IVec;
typedef struct { unsigned char *p; int n, cap; } CVec;

int  fpush(FVec *v, float x);
int  ipush(IVec *v, unsigned int x);
int  cpush(CVec *v, unsigned char x);
void ffree(FVec *v);
void ifree(IVec *v);
void cfree(CVec *v);

/* Little-endian readers shared by the binary loaders. */
static __inline unsigned rd16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static __inline unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static __inline unsigned long long rd64(const unsigned char *p) {
    return (unsigned long long)rd32(p) | ((unsigned long long)rd32(p + 4) << 32);
}

#endif /* MESHBUF_H */
