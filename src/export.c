/* ============================================================================
 * export.c - streaming animated GIF and APNG writers, no external libraries.
 *
 *  - GIF: 256-entry palette (index 0 = transparent), LZW compressed.
 *  - APNG: RGBA8 truecolour, each frame deflate-compressed with fixed Huffman
 *    codes and an LZ77 matcher (zlib wrapper + Adler-32).
 *
 * Frames are streamed one at a time so nothing large is held in memory.
 * ==========================================================================*/
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS

#include "export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ------------------------------------------------------------- byte buffer */
typedef struct { unsigned char *p; size_t n, cap; } Buf;

static int buf_need(Buf *b, size_t extra) {
    if (b->n + extra <= b->cap) return 1;
    {
        size_t nc = b->cap ? b->cap * 2 : 65536;
        while (nc < b->n + extra) nc *= 2;
        {
            unsigned char *np = (unsigned char *)realloc(b->p, nc);
            if (!np) return 0;
            b->p = np; b->cap = nc;
        }
    }
    return 1;
}
static int buf_u8(Buf *b, unsigned v) {
    if (!buf_need(b, 1)) return 0;
    b->p[b->n++] = (unsigned char)v;
    return 1;
}
static int buf_u16le(Buf *b, unsigned v) {
    return buf_u8(b, v & 0xFF) && buf_u8(b, (v >> 8) & 0xFF);
}
static int buf_u32be(Buf *b, unsigned v) {
    return buf_u8(b, (v >> 24) & 0xFF) && buf_u8(b, (v >> 16) & 0xFF) &&
           buf_u8(b, (v >> 8) & 0xFF) && buf_u8(b, v & 0xFF);
}
static int buf_write(Buf *b, const unsigned char *d, size_t n) {
    if (!buf_need(b, n)) return 0;
    memcpy(b->p + b->n, d, n);
    b->n += n;
    return 1;
}

/* ==========================================================================
 * GIF
 * ========================================================================*/
struct GifWriter {
    FILE *f;
    int   w, h, mcs, table_size;
};

/* LZW-compress `px` (n indices) and write the sub-blocked data. */
static void gif_lzw(FILE *f, const unsigned char *px, int n, int mcs) {
    static int htab[5003];
    static int codetab[5003];
    int clear = 1 << mcs, eoi = clear + 1;
    int code_size = mcs + 1, next = eoi + 1;
    int i, prefix = -1;
    unsigned bitbuf = 0;
    int bitcnt = 0;
    unsigned char sub[255];
    int subn = 0;

    memset(htab, -1, sizeof(htab));

#define GBYTE(c) do { sub[subn++] = (unsigned char)(c); if (subn == 255) { fputc(255, f); fwrite(sub, 1, 255, f); subn = 0; } } while (0)
#define GCODE(c, sz) do { bitbuf |= (unsigned)(c) << bitcnt; bitcnt += (sz); \
        while (bitcnt >= 8) { GBYTE(bitbuf & 0xFF); bitbuf >>= 8; bitcnt -= 8; } } while (0)

    fputc(mcs, f);
    GCODE(clear, code_size);
    for (i = 0; i < n; ++i) {
        int k = px[i];
        if (prefix < 0) { prefix = k; continue; }
        {
            unsigned long key = ((unsigned long)prefix << 8) | (unsigned long)k;
            int h = (int)((key * 2654435761u) % 5003u);
            int found = -1;
            while (htab[h] >= 0) {
                if (htab[h] == (int)key) { found = codetab[h]; break; }
                h = (h + 1) % 5003;
            }
            if (found >= 0) { prefix = found; continue; }
            GCODE(prefix, code_size);
            if (next < 4096) {
                /* grow the code size to match the decoder, which increases it
                 * after adding the entry with value `next` */
                if (next > (1 << code_size) - 1 && code_size < 12) ++code_size;
                htab[h] = (int)key;
                codetab[h] = next++;
            } else {
                GCODE(clear, code_size);
                memset(htab, -1, sizeof(htab));
                code_size = mcs + 1;
                next = eoi + 1;
            }
            prefix = k;
        }
    }
    if (prefix >= 0) GCODE(prefix, code_size);
    if (next > (1 << code_size) - 1 && code_size < 12) ++code_size;  /* final decoder step */
    GCODE(eoi, code_size);
    if (bitcnt > 0) { GBYTE(bitbuf & 0xFF); bitbuf = 0; bitcnt = 0; }
    if (subn > 0) { fputc(subn, f); fwrite(sub, 1, subn, f); subn = 0; }
    fputc(0, f);                              /* block terminator */
#undef GBYTE
#undef GCODE
}

GifWriter *gif_begin(const wchar_t *file, int w, int h,
                     const unsigned char *palette_rgb, int palette_n) {
    GifWriter *g;
    FILE *f;
    int bits = 1, i;

    f = _wfopen(file, L"wb");
    if (!f) return NULL;
    g = (GifWriter *)calloc(1, sizeof(*g));
    if (!g) { fclose(f); return NULL; }
    while ((1 << bits) < palette_n) ++bits;
    if (bits < 2) bits = 2;
    g->f = f; g->w = w; g->h = h; g->mcs = bits; g->table_size = 1 << bits;

    fwrite("GIF89a", 1, 6, f);
    fputc(w & 0xFF, f); fputc((w >> 8) & 0xFF, f);
    fputc(h & 0xFF, f); fputc((h >> 8) & 0xFF, f);
    fputc(0x80 | (0x07 << 4) | (bits - 1), f);   /* GCT, colour res, size */
    fputc(0, f);                                  /* background index */
    fputc(0, f);                                  /* aspect ratio */
    for (i = 0; i < g->table_size; ++i) {         /* global colour table */
        if (i < palette_n) { fputc(palette_rgb[i * 3 + 0], f); fputc(palette_rgb[i * 3 + 1], f); fputc(palette_rgb[i * 3 + 2], f); }
        else { fputc(0, f); fputc(0, f); fputc(0, f); }
    }
    /* Netscape looping extension */
    fputc(0x21, f); fputc(0xFF, f); fputc(0x0B, f);
    fwrite("NETSCAPE2.0", 1, 11, f);
    fputc(0x03, f); fputc(0x01, f); fputc(0x00, f); fputc(0x00, f); fputc(0x00, f);
    return g;
}

int gif_frame(GifWriter *g, const unsigned char *idx, int delay_cs) {
    if (!g) return 0;
    /* graphic control extension: disposal=2 (restore bg), transparent index 0 */
    fputc(0x21, g->f); fputc(0xF9, g->f); fputc(0x04, g->f);
    fputc((2 << 2) | 0x01, g->f);
    fputc(delay_cs & 0xFF, g->f); fputc((delay_cs >> 8) & 0xFF, g->f);
    fputc(0, g->f);                              /* transparent colour index */
    fputc(0, g->f);
    /* image descriptor */
    fputc(0x2C, g->f);
    fputc(0, g->f); fputc(0, g->f);
    fputc(0, g->f); fputc(0, g->f);
    fputc(g->w & 0xFF, g->f); fputc((g->w >> 8) & 0xFF, g->f);
    fputc(g->h & 0xFF, g->f); fputc((g->h >> 8) & 0xFF, g->f);
    fputc(0, g->f);                              /* no local colour table */
    gif_lzw(g->f, idx, g->w * g->h, g->mcs);
    return 1;
}

int gif_end(GifWriter *g) {
    if (!g) return 0;
    fputc(0x3B, g->f);
    if (fclose(g->f) != 0) { free(g); return 0; }
    free(g);
    return 1;
}

/* ==========================================================================
 * Deflate (fixed Huffman + LZ77) for APNG
 * ========================================================================*/
typedef struct { Buf out; unsigned bitbuf; int bitcnt; } Bits;

static void bit_raw(Bits *b, unsigned v, int n) {          /* LSB first */
    b->bitbuf |= v << b->bitcnt;
    b->bitcnt += n;
    while (b->bitcnt >= 8) { buf_u8(&b->out, b->bitbuf & 0xFF); b->bitbuf >>= 8; b->bitcnt -= 8; }
}
static void bit_huff(Bits *b, unsigned code, int n) {      /* MSB first */
    int i;
    for (i = n - 1; i >= 0; --i) bit_raw(b, (code >> i) & 1, 1);
}

static const int LEN_BASE[29]  = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const int LEN_EXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const int DIST_BASE[30]  = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const int DIST_EXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void emit_literal(Bits *b, unsigned v) {
    if (v <= 143) bit_huff(b, 0x30 + v, 8);
    else          bit_huff(b, 0x190 + (v - 144), 9);
}
static void emit_length(Bits *b, int len) {
    int i;
    for (i = 0; i < 28 && len >= LEN_BASE[i + 1]; ++i) ;
    {
        int code = 257 + i;
        if (code <= 279) bit_huff(b, code - 256, 7);
        else             bit_huff(b, 0xC0 + (code - 280), 8);
        if (LEN_EXTRA[i]) bit_raw(b, (unsigned)(len - LEN_BASE[i]), LEN_EXTRA[i]);
    }
}
static void emit_distance(Bits *b, int dist) {
    int i;
    for (i = 0; i < 29 && dist >= DIST_BASE[i + 1]; ++i) ;
    bit_huff(b, (unsigned)i, 5);
    if (DIST_EXTRA[i]) bit_raw(b, (unsigned)(dist - DIST_BASE[i]), DIST_EXTRA[i]);
}

#define DL_WSIZE 32768
#define DL_WMASK (DL_WSIZE - 1)
#define DL_HSIZE 32768
#define DL_HMASK (DL_HSIZE - 1)
#define DL_CHAIN 48

static int dl_head[DL_HSIZE];
static int dl_prev[DL_WSIZE];

static unsigned dl_hash(const unsigned char *p) {
    return ((unsigned)p[0] << 10 ^ (unsigned)p[1] << 5 ^ (unsigned)p[2]) & DL_HMASK;
}

/* deflate `data` (n bytes) with fixed Huffman into b->out. */
static int deflate_fixed(Bits *b, const unsigned char *data, size_t n) {
    size_t i = 0;
    memset(dl_head, -1, sizeof(dl_head));
    bit_raw(b, 1, 1);                 /* BFINAL = 1 */
    bit_raw(b, 1, 2);                 /* BTYPE  = 01 (fixed Huffman) */
    while (i < n) {
        int best_len = 0, best_dist = 0;
        if (i + 2 < n) {
            unsigned h = dl_hash(data + i);
            int cur = dl_head[h];
            int chain = DL_CHAIN;
            int maxlen = (n - i) < 258 ? (int)(n - i) : 258;
            while (cur >= 0 && chain-- > 0 && (int)(i - (size_t)cur) <= DL_WSIZE) {
                int l = 0;
                const unsigned char *a = data + cur, *c = data + i;
                while (l < maxlen && a[l] == c[l]) ++l;
                if (l > best_len) { best_len = l; best_dist = (int)(i - (size_t)cur); if (l == maxlen) break; }
                cur = dl_prev[cur & DL_WMASK];
            }
            dl_prev[i & DL_WMASK] = dl_head[h];
            dl_head[h] = (int)i;
        }
        if (best_len >= 3) {
            size_t end = i + (size_t)best_len;
            size_t k;
            emit_length(b, best_len);
            emit_distance(b, best_dist);
            for (k = i + 1; k < end && k + 2 < n; ++k) {
                unsigned h2 = dl_hash(data + k);
                dl_prev[k & DL_WMASK] = dl_head[h2];
                dl_head[h2] = (int)k;
            }
            i = end;
        } else {
            emit_literal(b, data[i]);
            ++i;
        }
    }
    bit_huff(b, 0, 7);                /* end of block (256) */
    if (b->bitcnt > 0) { buf_u8(&b->out, b->bitbuf & 0xFF); b->bitbuf = 0; b->bitcnt = 0; }
    return 1;
}

static unsigned adler32(const unsigned char *d, size_t n) {
    unsigned a = 1, bb = 0;
    size_t i;
    for (i = 0; i < n; ++i) { a = (a + d[i]) % 65521; bb = (bb + a) % 65521; }
    return (bb << 16) | a;
}

/* ==========================================================================
 * APNG
 * ========================================================================*/
struct ApngWriter {
    FILE *f;
    int   w, h;
    unsigned seq;
};

static unsigned crc_table[256];
static int crc_ready = 0;
static void crc_init(void) {
    unsigned c;
    int n, k;
    for (n = 0; n < 256; ++n) {
        c = (unsigned)n;
        for (k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}
static unsigned crc_update(unsigned c, const unsigned char *d, size_t n) {
    size_t i;
    if (!crc_ready) crc_init();
    for (i = 0; i < n; ++i) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static unsigned crc_of(const char *type, const unsigned char *data, size_t n) {
    unsigned c = crc_update(0xFFFFFFFFu, (const unsigned char *)type, 4);
    if (n) c = crc_update(c, data, n);
    return c ^ 0xFFFFFFFFu;
}

/* Write a PNG chunk: length, type, data, crc. */
static int png_chunk(FILE *f, const char *type, const unsigned char *data, size_t n) {
    unsigned c = crc_of(type, data, n);
    fputc((n >> 24) & 0xFF, f); fputc((n >> 16) & 0xFF, f);
    fputc((n >> 8) & 0xFF, f);  fputc(n & 0xFF, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    fputc((c >> 24) & 0xFF, f); fputc((c >> 16) & 0xFF, f);
    fputc((c >> 8) & 0xFF, f);  fputc(c & 0xFF, f);
    return 1;
}

ApngWriter *apng_begin(const wchar_t *file, int w, int h, int nframes,
                       const unsigned char *palette_rgb,
                       const unsigned char *trns, int palette_n) {
    ApngWriter *a;
    FILE *f;
    Buf ihdr, actl;
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

    f = _wfopen(file, L"wb");
    if (!f) return NULL;
    a = (ApngWriter *)calloc(1, sizeof(*a));
    if (!a) { fclose(f); return NULL; }
    a->f = f; a->w = w; a->h = h; a->seq = 0;

    fwrite(sig, 1, 8, f);
    memset(&ihdr, 0, sizeof(ihdr));
    buf_u32be(&ihdr, (unsigned)w); buf_u32be(&ihdr, (unsigned)h);
    buf_u8(&ihdr, 8);        /* bit depth */
    buf_u8(&ihdr, 3);        /* colour type: palette */
    buf_u8(&ihdr, 0); buf_u8(&ihdr, 0); buf_u8(&ihdr, 0);
    png_chunk(f, "IHDR", ihdr.p, ihdr.n);
    free(ihdr.p);

    png_chunk(f, "PLTE", palette_rgb, (size_t)palette_n * 3);
    if (trns) png_chunk(f, "tRNS", trns, (size_t)palette_n);

    memset(&actl, 0, sizeof(actl));
    buf_u32be(&actl, (unsigned)(nframes > 0 ? nframes : 1));
    buf_u32be(&actl, 0);     /* num_plays: 0 = loop forever */
    png_chunk(f, "acTL", actl.p, actl.n);
    free(actl.p);
    return a;
}

int apng_frame(ApngWriter *a, const unsigned char *idx, int delay_num, int delay_den) {
    Buf raw, fcTL, comp;
    Bits bits;
    int y;

    if (!a) return 0;
    memset(&raw, 0, sizeof(raw));
    for (y = 0; y < a->h; ++y) {
        buf_u8(&raw, 0);                                  /* filter: none */
        buf_write(&raw, idx + (size_t)y * a->w, (size_t)a->w);
    }

    memset(&comp, 0, sizeof(comp));
    memset(&bits, 0, sizeof(bits));
    /* zlib header */
    buf_u8(&comp, 0x78); buf_u8(&comp, 0x9C);
    deflate_fixed(&bits, raw.p, raw.n);
    buf_write(&comp, bits.out.p, bits.out.n);
    {
        unsigned ad = adler32(raw.p, raw.n);
        buf_u8(&comp, (ad >> 24) & 0xFF); buf_u8(&comp, (ad >> 16) & 0xFF);
        buf_u8(&comp, (ad >> 8) & 0xFF);  buf_u8(&comp, ad & 0xFF);
    }

    /* fcTL */
    memset(&fcTL, 0, sizeof(fcTL));
    buf_u32be(&fcTL, a->seq++);
    buf_u32be(&fcTL, (unsigned)a->w); buf_u32be(&fcTL, (unsigned)a->h);
    buf_u32be(&fcTL, 0); buf_u32be(&fcTL, 0);            /* x, y offset */
    buf_u16le(&fcTL, (unsigned)delay_num); buf_u16le(&fcTL, (unsigned)delay_den);
    buf_u8(&fcTL, 0);                                     /* dispose: none */
    buf_u8(&fcTL, 0);                                     /* blend: source */
    png_chunk(a->f, "fcTL", fcTL.p, fcTL.n);

    if (a->seq == 1) {
        png_chunk(a->f, "IDAT", comp.p, comp.n);          /* first frame */
    } else {
        Buf fdat;
        memset(&fdat, 0, sizeof(fdat));
        buf_u32be(&fdat, a->seq++);                       /* fdAT sequence number */
        buf_write(&fdat, comp.p, comp.n);
        png_chunk(a->f, "fdAT", fdat.p, fdat.n);
        free(fdat.p);
    }

    free(raw.p); free(comp.p); free(bits.out.p); free(fcTL.p);
    return 1;
}

int apng_end(ApngWriter *a) {
    if (!a) return 0;
    png_chunk(a->f, "IEND", NULL, 0);
    if (fclose(a->f) != 0) { free(a); return 0; }
    free(a);
    return 1;
}
