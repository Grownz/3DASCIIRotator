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

#endif /* MESHBUF_H */
