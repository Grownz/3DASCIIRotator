/* libFuzzer harness for the model loaders. Built once per format with
 * /DFUZZ_EXT=L".obj" (see build_fuzz.bat). Every input is parsed as that
 * format with ASan watching for out-of-bounds access, leaks and use-after-free. */
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "loader.h"

#ifndef FUZZ_EXT
#define FUZZ_EXT L".obj"
#endif

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    unsigned char *b;
    RawMesh m;
    char err[128];

    if (size == 0 || size > (1u << 20)) return 0;
    b = (unsigned char *)malloc(size + 1);      /* loaders expect a NUL after the data */
    if (!b) return 0;
    memcpy(b, data, size);
    b[size] = 0;
    /* a small triangle cap keeps each run fast; the path is a dummy for sidecar lookups */
    if (loader_load_buf(b, size, FUZZ_EXT, L"fuzz_input" FUZZ_EXT, 4000, &m, err, sizeof(err))) {
        free(m.verts); free(m.tris); free(m.face_rgb);
    }
    free(b);
    return 0;
}
