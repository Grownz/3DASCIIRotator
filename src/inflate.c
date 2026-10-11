/* ============================================================================
 * inflate.c - raw DEFLATE decompressor (RFC 1951).
 *
 * Decodes stored, fixed-Huffman and dynamic-Huffman blocks. Used by the 3MF
 * reader to unpack the OPC/ZIP package. No external dependencies.
 * ==========================================================================*/
#include "inflate.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const unsigned char *in;
    size_t inlen, incnt;
    unsigned bitbuf;
    int bitcnt;
} State;

static int bits(State *s, int need, unsigned *val) {
    unsigned v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) return -1;
        v |= (unsigned)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    *val = v & ((1u << need) - 1u);
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return 0;
}

typedef struct { short count[16]; short symbol[288]; } Huff;

static int huff_build(Huff *h, const short *lengths, int n) {
    int i, len, left;
    for (i = 0; i < 16; ++i) h->count[i] = 0;
    for (i = 0; i < n; ++i) h->count[lengths[i]]++;
    if (h->count[0] == n) return 0;
    left = 1;
    for (len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    {
        short offs[16];
        offs[1] = 0;
        for (len = 1; len < 15; ++len) offs[len + 1] = (short)(offs[len] + h->count[len]);
        for (i = 0; i < n; ++i) if (lengths[i]) h->symbol[offs[lengths[i]]++] = (short)i;
    }
    return left;
}

static int huff_decode(State *s, const Huff *h) {
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; ++len) {
        unsigned b;
        if (bits(s, 1, &b)) return -1;
        code |= (int)b;
        {
            int count = h->count[len];
            if (code - count < first) return h->symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
    }
    return -1;
}

static const short LENS[29]  = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short LEXT[29]  = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short DISTS[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short DEXT[30]  = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

typedef struct { unsigned char *p; size_t n, cap; } Out;
static int oput(Out *o, unsigned char c) {
    if (o->n == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 65536;
        unsigned char *np = (unsigned char *)realloc(o->p, nc);
        if (!np) return 0;
        o->p = np; o->cap = nc;
    }
    o->p[o->n++] = c;
    return 1;
}

static int inflate_block(State *s, const Huff *lc, const Huff *dc, Out *o) {
    for (;;) {
        int sym = huff_decode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (!oput(o, (unsigned char)sym)) return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            unsigned len, dist;
            int li = sym - 257;
            int di;
            size_t start;
            if (li >= 29) return -1;
            if (bits(s, LEXT[li], &len)) return -1;
            len += (unsigned)LENS[li];
            di = huff_decode(s, dc);
            if (di < 0 || di >= 30) return -1;
            if (bits(s, DEXT[di], &dist)) return -1;
            dist += (unsigned)DISTS[di];
            if (dist > o->n) return -1;
            start = o->n - dist;
            while (len--) {
                unsigned char c = o->p[start++];
                if (!oput(o, c)) return -1;
            }
        }
    }
}

unsigned char *inflate_raw(const unsigned char *in, size_t inlen, size_t *outlen) {
    State s;
    Out o;
    unsigned last = 0;
    static const short lfix[288] = {
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
        9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
        9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
        9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
        9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
        7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7,
        8,8,8,8,8,8,8,8
    };
    static const short dfix[30] = {
        5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5, 5,5,5,5,5,5,5,5,5,5,5,5,5,5
    };

    s.in = in; s.inlen = inlen; s.incnt = 0; s.bitbuf = 0; s.bitcnt = 0;
    o.p = NULL; o.n = 0; o.cap = 0;

    do {
        unsigned type;
        if (bits(&s, 1, &last)) goto fail;
        if (bits(&s, 2, &type)) goto fail;
        if (type == 0) {
            unsigned len, nlen;
            s.bitbuf = 0; s.bitcnt = 0;              /* align to byte */
            if (s.incnt + 4 > s.inlen) goto fail;
            len = (unsigned)s.in[s.incnt] | ((unsigned)s.in[s.incnt + 1] << 8);
            nlen = (unsigned)s.in[s.incnt + 2] | ((unsigned)s.in[s.incnt + 3] << 8);
            s.incnt += 4;
            if ((len ^ 0xFFFFu) != nlen) goto fail;
            if (s.incnt + len > s.inlen) goto fail;
            {
                unsigned k;
                for (k = 0; k < len; ++k) if (!oput(&o, s.in[s.incnt + k])) goto fail;
                s.incnt += len;
            }
        } else if (type == 1) {
            Huff lc, dc;
            huff_build(&lc, lfix, 288);
            huff_build(&dc, dfix, 30);
            if (inflate_block(&s, &lc, &dc, &o)) goto fail;
        } else if (type == 2) {
            Huff lc, dc;
            short lengths[320];
            unsigned hlit, hdist, hclen, i;
            static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            short clen[19];
            Huff cl;
            if (bits(&s, 5, &hlit)) goto fail;
            if (bits(&s, 5, &hdist)) goto fail;
            if (bits(&s, 4, &hclen)) goto fail;
            hlit += 257; hdist += 1; hclen += 4;
            if (hlit > 286 || hdist > 30) goto fail;
            for (i = 0; i < 19; ++i) clen[i] = 0;
            for (i = 0; i < hclen; ++i) { unsigned v; if (bits(&s, 3, &v)) goto fail; clen[order[i]] = (short)v; }
            if (huff_build(&cl, clen, 19) < 0) goto fail;
            {
                unsigned n = 0;
                while (n < hlit + hdist) {
                    int sym = huff_decode(&s, &cl);
                    if (sym < 0) goto fail;
                    if (sym < 16) {
                        lengths[n++] = (short)sym;
                    } else if (sym == 16) {
                        unsigned r, v;
                        if (n == 0) goto fail;
                        if (bits(&s, 2, &r)) goto fail;
                        r += 3;
                        v = (unsigned)lengths[n - 1];
                        while (r-- && n < hlit + hdist) lengths[n++] = (short)v;
                    } else if (sym == 17) {
                        unsigned r;
                        if (bits(&s, 3, &r)) goto fail;
                        r += 3;
                        while (r-- && n < hlit + hdist) lengths[n++] = 0;
                    } else {
                        unsigned r;
                        if (bits(&s, 7, &r)) goto fail;
                        r += 11;
                        while (r-- && n < hlit + hdist) lengths[n++] = 0;
                    }
                }
            }
            if (huff_build(&lc, lengths, (int)hlit) < 0) goto fail;
            if (huff_build(&dc, lengths + hlit, (int)hdist) < 0) goto fail;
            if (inflate_block(&s, &lc, &dc, &o)) goto fail;
        } else {
            goto fail;
        }
    } while (!last);

    *outlen = o.n;
    return o.p;

fail:
    free(o.p);
    return NULL;
}
