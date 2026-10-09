#ifndef EXPORT_H
#define EXPORT_H

#include <wchar.h>

/* Streaming animation writers (no external libraries).
 *
 * GIF: 256-colour palette (index 0 = transparent), LZW compressed.
 * APNG: RGBA8 truecolour with 8-bit alpha, zlib/deflate compressed.
 *
 * Frames are appended one at a time so the whole animation never has to be held
 * in memory. All functions return 1 on success and 0 on failure.
 */

typedef struct GifWriter GifWriter;
typedef struct ApngWriter ApngWriter;

/* palette_rgb: palette_n*3 bytes; palette_n is rounded up to a power of two.
 * index 0 is reserved for transparency. */
GifWriter *gif_begin(const wchar_t *path, int w, int h,
                     const unsigned char *palette_rgb, int palette_n);
int        gif_frame(GifWriter *g, const unsigned char *idx, int delay_cs);
int        gif_end(GifWriter *g);

ApngWriter *apng_begin(const wchar_t *path, int w, int h, int nframes);
int         apng_frame(ApngWriter *a, const unsigned char *rgba,
                       int delay_num, int delay_den);
int         apng_end(ApngWriter *a);

#endif /* EXPORT_H */
