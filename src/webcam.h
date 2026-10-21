#ifndef WEBCAM_H
#define WEBCAM_H

/* Webcam capture for the ASCII renderer (Media Foundation).
 *
 * The Media Foundation entry points are resolved dynamically, so the executable
 * keeps its minimal import table. Only GUID data is linked (mfuuid.lib).
 */

#define WEBCAM_MAX_DEV 16

/* Enumerate video capture devices into `names` (UTF-8). Returns the number of
 * devices, or -1 on error (message in err). */
int webcam_list(char names[WEBCAM_MAX_DEV][128], int maxdev, char *err, int errsz);

/* Start capture from device `index` (0-based). Returns 1 on success. */
int webcam_start(int index, char *err, int errsz);

/* Stop capture and release all resources. */
void webcam_stop(void);

/* Wait until a first frame is available (up to `ms`). Returns 1 if ready. */
int webcam_wait(int ms);

/* Lock the latest frame (packed RGB, w*h*3 bytes). Returns NULL if none is
 * available. Pair a non-NULL result with webcam_unlock(); calling
 * webcam_unlock() after a NULL result is harmless. */
const unsigned char *webcam_lock(int *w, int *h);
void webcam_unlock(void);

/* The friendly name of the active device (or "webcam"). */
const char *webcam_device_name(void);

/* Non-zero if the capture thread stopped because of an error. */
int webcam_error(void);

/* Release the Media Foundation platform (call once before exit). */
void webcam_global_shutdown(void);

#endif /* WEBCAM_H */
