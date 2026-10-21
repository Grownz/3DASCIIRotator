/* ============================================================================
 * webcam.c - webcam capture for the ASCII renderer (Media Foundation).
 *
 * Every Media Foundation entry point is resolved with LoadLibrary/GetProcAddress,
 * so the executable keeps its minimal import table (no mfplat / mfreadwrite
 * imports). Only GUID data comes from mfuuid.lib, which is a static library.
 *
 * A capture thread keeps the latest frame in packed RGB; the renderer samples
 * it under a critical section.
 * ==========================================================================*/
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0601

#include "webcam.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#pragma comment(lib, "mfuuid.lib")
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- dynamic MF entry */
typedef HRESULT (WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *PFN_MFShutdown)(void);
typedef HRESULT (WINAPI *PFN_MFCreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT (WINAPI *PFN_MFCreateMediaType)(IMFMediaType **);
typedef HRESULT (WINAPI *PFN_MFEnumDeviceSources)(IMFAttributes *, IMFActivate ***, UINT32 *);
typedef HRESULT (WINAPI *PFN_MFCreateSourceReaderFromMediaSource)(IMFMediaSource *, IMFAttributes *, IMFSourceReader **);
typedef HRESULT (WINAPI *PFN_CoInitializeEx)(LPVOID, DWORD);
typedef void    (WINAPI *PFN_CoUninitialize)(void);
typedef void    (WINAPI *PFN_CoTaskMemFree)(LPVOID);

static HMODULE g_mfplat, g_mfreadwrite, g_mf, g_ole32;
static PFN_MFStartup pMFStartup;
static PFN_MFShutdown pMFShutdown;
static PFN_MFCreateAttributes pMFCreateAttributes;
static PFN_MFCreateMediaType pMFCreateMediaType;
static PFN_MFEnumDeviceSources pMFEnumDeviceSources;
static PFN_MFCreateSourceReaderFromMediaSource pMFCreateSourceReaderFromMediaSource;
static PFN_CoInitializeEx pCoInitializeEx;
static PFN_CoUninitialize pCoUninitialize;
static PFN_CoTaskMemFree pCoTaskMemFree;

static int g_mf_loaded;
static int g_mf_started;
static int g_com_inited;

/* ------------------------------------------------------------- capture state */
static IMFSourceReader *g_reader;
static IMFMediaSource *g_source;
static HANDLE g_thread;
static volatile LONG g_stop;
static CRITICAL_SECTION g_lock;
static int g_lock_init;
static unsigned char *g_front, *g_back;
static int g_w, g_h, g_is_rgb, g_has_frame, g_started;
static int g_locked;
static UINT32 g_stride;
static char g_device_name[128] = "webcam";

static void werr(char *e, int n, const char *m) {
    if (n > 0) { strncpy(e, m, (size_t)n - 1); e[n - 1] = '\0'; }
}

static int getfn(HMODULE h, const char *name, void *dst) {
    FARPROC fp = GetProcAddress(h, name);
    if (!fp) return 0;
    memcpy(dst, &fp, sizeof(fp));
    return 1;
}

static int mf_load(char *err, int errsz) {
    if (g_mf_loaded) return 1;
    g_mfplat = LoadLibraryA("mfplat.dll");
    if (!g_mfplat) { werr(err, errsz, "mfplat.dll is not available"); return 0; }
    g_mfreadwrite = LoadLibraryA("mfreadwrite.dll");
    if (!g_mfreadwrite) { werr(err, errsz, "mfreadwrite.dll is not available"); return 0; }
    g_mf = LoadLibraryA("mf.dll");
    if (!g_mf) { werr(err, errsz, "mf.dll is not available"); return 0; }
    g_ole32 = LoadLibraryA("ole32.dll");
    if (!g_ole32) g_ole32 = GetModuleHandleA("ole32.dll");
    if (!g_ole32) { werr(err, errsz, "ole32.dll is not available"); return 0; }
    if (!getfn(g_mfplat, "MFStartup", &pMFStartup) ||
        !getfn(g_mfplat, "MFShutdown", &pMFShutdown) ||
        !getfn(g_mfplat, "MFCreateAttributes", &pMFCreateAttributes) ||
        !getfn(g_mfplat, "MFCreateMediaType", &pMFCreateMediaType) ||
        !getfn(g_mf, "MFEnumDeviceSources", &pMFEnumDeviceSources) ||
        !getfn(g_mfreadwrite, "MFCreateSourceReaderFromMediaSource", &pMFCreateSourceReaderFromMediaSource) ||
        !getfn(g_ole32, "CoInitializeEx", &pCoInitializeEx) ||
        !getfn(g_ole32, "CoUninitialize", &pCoUninitialize) ||
        !getfn(g_ole32, "CoTaskMemFree", &pCoTaskMemFree)) {
        werr(err, errsz, "Media Foundation entry points not found");
        return 0;
    }
    g_mf_loaded = 1;
    return 1;
}

static int mf_global_start(char *err, int errsz) {
    HRESULT hr;
    if (g_mf_started) return 1;
    if (!mf_load(err, errsz)) return 0;
    hr = pCoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) g_com_inited = 1;
    else if (hr != RPC_E_CHANGED_MODE) { werr(err, errsz, "COM initialisation failed"); return 0; }
    hr = pMFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) { werr(err, errsz, "Media Foundation startup failed"); return 0; }
    g_mf_started = 1;
    return 1;
}

static HRESULT enum_devices(IMFActivate ***devs, UINT32 *count) {
    IMFAttributes *attr = NULL;
    HRESULT hr;
    *devs = NULL; *count = 0;
    hr = pMFCreateAttributes(&attr, 1);
    if (FAILED(hr)) return hr;
    hr = attr->lpVtbl->SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                    &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = pMFEnumDeviceSources(attr, devs, count);
    attr->lpVtbl->Release(attr);
    return hr;
}

int webcam_list(char names[WEBCAM_MAX_DEV][128], int maxdev, char *err, int errsz) {
    IMFActivate **devs = NULL;
    UINT32 count = 0, i;
    int n = 0;
    if (!mf_global_start(err, errsz)) return -1;
    if (FAILED(enum_devices(&devs, &count))) { werr(err, errsz, "cannot enumerate cameras"); return -1; }
    for (i = 0; i < count && n < maxdev; ++i) {
        WCHAR *fn = NULL;
        UINT32 flen = 0;
        names[n][0] = 0;
        if (SUCCEEDED(devs[i]->lpVtbl->GetAllocatedString(devs[i], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &fn, &flen)) && fn) {
            WideCharToMultiByte(CP_UTF8, 0, fn, -1, names[n], 128, NULL, NULL);
            names[n][127] = 0;
            pCoTaskMemFree(fn);
        }
        if (!names[n][0]) _snprintf(names[n], 128, "Camera %d", n);
        ++n;
    }
    for (i = 0; i < count; ++i) devs[i]->lpVtbl->Release(devs[i]);
    pCoTaskMemFree(devs);
    return n;
}

static int set_type(IMFSourceReader *r, const GUID *sub, int w, int h) {
    IMFMediaType *t = NULL;
    HRESULT hr;
    if (FAILED(pMFCreateMediaType(&t))) return 0;
    t->lpVtbl->SetGUID(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    t->lpVtbl->SetGUID(t, &MF_MT_SUBTYPE, sub);
    if (w > 0 && h > 0)
        t->lpVtbl->SetUINT64(t, &MF_MT_FRAME_SIZE, ((UINT64)(UINT32)w << 32) | (UINT32)h);
    hr = r->lpVtbl->SetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, t);
    t->lpVtbl->Release(t);
    return SUCCEEDED(hr);
}

static unsigned char clamp8(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void convert_to_rgb(const unsigned char *src, size_t len, unsigned char *dst) {
    int w = g_w, h = g_h, x, y;
    (void)len;
    if (g_is_rgb) {
        int stride = (int)g_stride;
        int astep = stride < 0 ? -stride : stride;
        if (astep < w * 4) astep = w * 4;
        for (y = 0; y < h; ++y) {
            int sy = (stride < 0) ? (h - 1 - y) : y;
            const unsigned char *row = src + (size_t)sy * (size_t)astep;
            unsigned char *o = dst + (size_t)y * (size_t)w * 3;
            for (x = 0; x < w; ++x) {
                o[x * 3 + 0] = row[x * 4 + 2];   /* R (source is BGRA) */
                o[x * 3 + 1] = row[x * 4 + 1];   /* G */
                o[x * 3 + 2] = row[x * 4 + 0];   /* B */
            }
        }
    } else {
        int stride = (int)g_stride;
        const unsigned char *yp;
        const unsigned char *uv;
        if (stride < w) stride = w;
        yp = src;
        uv = src + (size_t)stride * (size_t)h;
        for (y = 0; y < h; ++y) {
            const unsigned char *yr = yp + (size_t)y * (size_t)stride;
            const unsigned char *ur = uv + (size_t)(y >> 1) * (size_t)stride;
            unsigned char *o = dst + (size_t)y * (size_t)w * 3;
            for (x = 0; x < w; ++x) {
                int Y = (int)yr[x] - 16;
                int cb = (int)ur[(x >> 1) * 2 + 0] - 128;
                int cr = (int)ur[(x >> 1) * 2 + 1] - 128;
                int r, g, b;
                if (Y < 0) Y = 0;
                r = (298 * Y + 409 * cr + 128) >> 8;
                g = (298 * Y - 100 * cb - 208 * cr + 128) >> 8;
                b = (298 * Y + 516 * cb + 128) >> 8;
                o[x * 3 + 0] = clamp8(r);
                o[x * 3 + 1] = clamp8(g);
                o[x * 3 + 2] = clamp8(b);
            }
        }
    }
}

static DWORD WINAPI capture_thread(LPVOID p) {
    (void)p;
    pCoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (!g_stop) {
        DWORD flags = 0, stream = 0;
        LONGLONG ts = 0;
        IMFSample *sample = NULL;
        HRESULT hr;
        if (!g_reader) break;
        hr = g_reader->lpVtbl->ReadSample(g_reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                          0, &stream, &flags, &ts, &sample);
        if (FAILED(hr)) break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (sample) {
            IMFMediaBuffer *buf = NULL;
            if (SUCCEEDED(sample->lpVtbl->ConvertToContiguousBuffer(sample, &buf))) {
                BYTE *data = NULL;
                DWORD maxlen = 0, curlen = 0;
                if (SUCCEEDED(buf->lpVtbl->Lock(buf, &data, &maxlen, &curlen))) {
                    convert_to_rgb(data, (size_t)curlen, g_back);
                    buf->lpVtbl->Unlock(buf);
                    EnterCriticalSection(&g_lock);
                    {
                        unsigned char *tmp = g_front;
                        g_front = g_back;
                        g_back = tmp;
                        g_has_frame = 1;
                    }
                    LeaveCriticalSection(&g_lock);
                }
                buf->lpVtbl->Release(buf);
            }
            sample->lpVtbl->Release(sample);
        }
    }
    pCoUninitialize();
    return 0;
}

int webcam_start(int index, char *err, int errsz) {
    IMFActivate **devs = NULL;
    UINT32 count = 0;
    IMFMediaSource *source = NULL;
    IMFAttributes *rattr = NULL;
    IMFMediaType *cur = NULL;
    GUID sub;
    UINT64 fs = 0;
    UINT32 stride = 0;
    HRESULT hr;
    int i;

    if (g_started) return 1;
    if (!mf_global_start(err, errsz)) return 0;
    if (FAILED(enum_devices(&devs, &count)) || count == 0) {
        werr(err, errsz, "no camera found");
        goto fail;
    }
    if (index < 0 || index >= (int)count) index = 0;

    /* remember the friendly name */
    {
        WCHAR *fn = NULL;
        UINT32 flen = 0;
        g_device_name[0] = 0;
        if (SUCCEEDED(devs[index]->lpVtbl->GetAllocatedString(devs[index], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &fn, &flen)) && fn) {
            WideCharToMultiByte(CP_UTF8, 0, fn, -1, g_device_name, (int)sizeof(g_device_name), NULL, NULL);
            g_device_name[sizeof(g_device_name) - 1] = 0;
            pCoTaskMemFree(fn);
        }
        if (!g_device_name[0]) _snprintf(g_device_name, sizeof(g_device_name), "Camera %d", index);
    }

    hr = devs[index]->lpVtbl->ActivateObject(devs[index], &IID_IMFMediaSource, (void **)&source);
    if (FAILED(hr) || !source) {
        werr(err, errsz, "cannot open the camera (check the Windows camera privacy setting)");
        goto fail;
    }

    hr = pMFCreateAttributes(&rattr, 2);
    if (SUCCEEDED(hr)) {
        rattr->lpVtbl->SetUINT32(rattr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        hr = pMFCreateSourceReaderFromMediaSource(source, rattr, &g_reader);
    }
    if (FAILED(hr) || !g_reader) {
        werr(err, errsz, "cannot create the camera reader");
        goto fail;
    }

    g_reader->lpVtbl->SetStreamSelection(g_reader, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    g_reader->lpVtbl->SetStreamSelection(g_reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    if (!set_type(g_reader, &MFVideoFormat_RGB32, 640, 480) &&
        !set_type(g_reader, &MFVideoFormat_RGB32, 0, 0) &&
        !set_type(g_reader, &MFVideoFormat_NV12, 640, 480) &&
        !set_type(g_reader, &MFVideoFormat_NV12, 0, 0)) {
        werr(err, errsz, "no supported camera video format");
        goto fail;
    }

    hr = g_reader->lpVtbl->GetCurrentMediaType(g_reader, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    if (FAILED(hr) || !cur) {
        werr(err, errsz, "cannot read the camera format");
        goto fail;
    }
    cur->lpVtbl->GetGUID(cur, &MF_MT_SUBTYPE, &sub);
    cur->lpVtbl->GetUINT64(cur, &MF_MT_FRAME_SIZE, &fs);
    g_w = (int)(fs >> 32);
    g_h = (int)(fs & 0xFFFFFFFFu);
    cur->lpVtbl->GetUINT32(cur, &MF_MT_DEFAULT_STRIDE, &stride);
    g_is_rgb = IsEqualGUID(&sub, &MFVideoFormat_RGB32) ? 1 : 0;
    if (!g_is_rgb && !IsEqualGUID(&sub, &MFVideoFormat_NV12)) {
        werr(err, errsz, "unsupported camera video format");
        goto fail;
    }
    if (g_is_rgb) { if (stride == 0) stride = (UINT32)g_w * 4u; }
    else          { if (stride == 0) stride = (UINT32)g_w; }
    g_stride = stride;
    if (g_w <= 0 || g_h <= 0) {
        werr(err, errsz, "invalid camera frame size");
        goto fail;
    }

    g_front = (unsigned char *)malloc((size_t)g_w * (size_t)g_h * 3u);
    g_back  = (unsigned char *)malloc((size_t)g_w * (size_t)g_h * 3u);
    if (!g_front || !g_back) {
        werr(err, errsz, "out of memory");
        goto fail;
    }

    InitializeCriticalSection(&g_lock);
    g_lock_init = 1;
    g_has_frame = 0;
    g_locked = 0;
    g_stop = 0;
    g_thread = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    if (!g_thread) {
        werr(err, errsz, "cannot start the capture thread");
        goto fail;
    }

    g_source = source;          /* kept so webcam_stop() can call Shutdown() */
    g_started = 1;
    for (i = 0; i < (int)count; ++i) devs[i]->lpVtbl->Release(devs[i]);
    pCoTaskMemFree(devs);
    if (rattr) rattr->lpVtbl->Release(rattr);
    if (cur) cur->lpVtbl->Release(cur);
    return 1;

fail:
    if (g_thread) { InterlockedExchange(&g_stop, 1); WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = NULL; }
    if (g_reader) { g_reader->lpVtbl->Release(g_reader); g_reader = NULL; }
    if (g_source) { g_source->lpVtbl->Release(g_source); g_source = NULL; }
    if (g_lock_init) { DeleteCriticalSection(&g_lock); g_lock_init = 0; }
    free(g_front); g_front = NULL;
    free(g_back);  g_back = NULL;
    if (rattr) rattr->lpVtbl->Release(rattr);
    if (cur) cur->lpVtbl->Release(cur);
    if (source) source->lpVtbl->Release(source);
    for (i = 0; i < (int)count; ++i) devs[i]->lpVtbl->Release(devs[i]);
    pCoTaskMemFree(devs);
    return 0;
}

void webcam_stop(void) {
    DWORD w;
    if (!g_started) return;
    InterlockedExchange(&g_stop, 1);
    if (g_source) g_source->lpVtbl->Shutdown(g_source);   /* abort a pending ReadSample */
    if (g_thread) {
        w = WaitForSingleObject(g_thread, 5000);
        if (w != WAIT_OBJECT_0) {
            /* The capture thread is stuck; do not free resources it may still use. */
            g_started = 0;
            return;
        }
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_reader) { g_reader->lpVtbl->Release(g_reader); g_reader = NULL; }
    if (g_source) { g_source->lpVtbl->Release(g_source); g_source = NULL; }
    if (g_lock_init) { DeleteCriticalSection(&g_lock); g_lock_init = 0; }
    free(g_front); g_front = NULL;
    free(g_back);  g_back = NULL;
    g_started = 0;
    g_locked = 0;
    g_has_frame = 0;
}

void webcam_global_shutdown(void) {
    if (g_started) webcam_stop();
    if (g_mf_started) { pMFShutdown(); g_mf_started = 0; }
    if (g_com_inited) { pCoUninitialize(); g_com_inited = 0; }
}

int webcam_wait(int ms) {
    int waited = 0;
    if (!g_started) return 0;
    for (;;) {
        int ok;
        EnterCriticalSection(&g_lock);
        ok = g_has_frame;
        LeaveCriticalSection(&g_lock);
        if (ok) return 1;
        if (waited >= ms) return 0;
        Sleep(10);
        waited += 10;
    }
}

const unsigned char *webcam_lock(int *w, int *h) {
    if (!g_started) return NULL;
    EnterCriticalSection(&g_lock);
    if (!g_has_frame) { LeaveCriticalSection(&g_lock); return NULL; }
    g_locked = 1;
    if (w) *w = g_w;
    if (h) *h = g_h;
    return g_front;
}

void webcam_unlock(void) {
    if (g_locked) { g_locked = 0; LeaveCriticalSection(&g_lock); }
}

const char *webcam_device_name(void) {
    return g_device_name[0] ? g_device_name : "webcam";
}
