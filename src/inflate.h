#ifndef INFLATE_H
#define INFLATE_H

#include <stddef.h>

/* Raw DEFLATE decompression (no zlib header). Returns a malloc'd buffer and
 * sets *outlen, or returns NULL on error. */
unsigned char *inflate_raw(const unsigned char *in, size_t inlen, size_t *outlen);

#endif /* INFLATE_H */
