/* Minimal AAssetManager / ANativeWindow shims.
 *
 * libTTapp.so was built against the Android NDK and pulls in symbols from
 * libandroid.so (AAssetManager_*, ANativeWindow_*). We don't have real
 * Android assets on disk - everything was pre-extracted by the player
 * into the data directory - so the asset manager is a tiny wrapper around
 * fopen() relative to tt_data_path(). The native window stubs just return
 * the configured display size; the game uses them to size its swap chain
 * via egl. We trust the .so's EGL path to no-op gracefully now that the
 * SDL2 GL context is already live. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "platform.h"
#include "libtt.h"

typedef struct AAssetManager {
    int placeholder;
} AAssetManager;

typedef struct AAsset {
    FILE *fp;
    off_t size;
} AAsset;

typedef struct ANativeWindow {
    int width;
    int height;
    int format;
} ANativeWindow;

static AAssetManager g_asset_manager;
static ANativeWindow g_native_window = {.width = 960, .height = 544, .format = 0};

const char *tt_data_path(void)
{
    static char buf[512];
    if (buf[0]) return buf;

    const char *env = getenv("LSWTCS_DATA_PATH");
    if (env && env[0]) {
        snprintf(buf, sizeof(buf), "%s%s", env,
                 env[strlen(env) - 1] == '/' ? "" : "/");
    } else {
        snprintf(buf, sizeof(buf), "./lswtcs_data/");
    }
    return buf;
}

void *AAssetManager_create(void)
{
    return &g_asset_manager;
}

ABI_ATTR AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode)
{
    (void)mgr; (void)mode;
    /* Try a couple of candidate layouts:
     *   1. tt_data_path()/<filename>          (Audio.dat at the data root)
     *   2. tt_data_path()/assets/<filename>   (mirrors the APK 'assets/' subdir)
     *   3. <filename> verbatim                (already absolute)
     * The Vita port pre-extracted everything to the data root, so case 1 hits
     * for the .dat packs; case 2 covers anything that genuinely lived under
     * an assets/ subfolder in the APK. */
    char path[1024];
    FILE *fp = NULL;

    snprintf(path, sizeof(path), "%s%s", tt_data_path(), filename);
    fp = fopen(path, "rb");
    if (!fp) {
        snprintf(path, sizeof(path), "%sassets/%s", tt_data_path(), filename);
        fp = fopen(path, "rb");
    }
    if (!fp) {
        fp = fopen(filename, "rb");
    }
    if (!fp) {
        warning("AAssetManager_open: could not open '%s'\n", filename);
        return NULL;
    }

    AAsset *a = calloc(1, sizeof(*a));
    a->fp = fp;
    fseek(fp, 0, SEEK_END);
    a->size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    return a;
}

ABI_ATTR int AAsset_read(AAsset *a, void *buf, size_t count)
{
    if (!a || !a->fp) return -1;
    return (int)fread(buf, 1, count, a->fp);
}

ABI_ATTR off_t AAsset_seek(AAsset *a, off_t offset, int whence)
{
    if (!a || !a->fp) return -1;
    if (fseek(a->fp, offset, whence) != 0) return -1;
    return ftell(a->fp);
}

ABI_ATTR off_t AAsset_getLength(AAsset *a)
{
    return a ? a->size : 0;
}

ABI_ATTR off_t AAsset_getRemainingLength(AAsset *a)
{
    if (!a || !a->fp) return 0;
    return a->size - ftell(a->fp);
}

ABI_ATTR int AAsset_openFileDescriptor(AAsset *a, off_t *outStart, off_t *outLength)
{
    /* The game uses this for mmap-style streaming; we just say "no" and
     * force it onto the read() path, which is fine for an ARM desktop. */
    (void)a; (void)outStart; (void)outLength;
    return -1;
}

ABI_ATTR void AAsset_close(AAsset *a)
{
    if (!a) return;
    if (a->fp) fclose(a->fp);
    free(a);
}

/* ---- ANativeWindow ------------------------------------------------- */

void *tt_native_window(int w, int h)
{
    g_native_window.width = w;
    g_native_window.height = h;
    return &g_native_window;
}

ABI_ATTR int ANativeWindow_getWidth(ANativeWindow *w)  { return w ? w->width  : 0; }
ABI_ATTR int ANativeWindow_getHeight(ANativeWindow *w) { return w ? w->height : 0; }
ABI_ATTR int ANativeWindow_getFormat(ANativeWindow *w) { return w ? w->format : 1; }
ABI_ATTR int ANativeWindow_setBuffersGeometry(ANativeWindow *w, int width, int height, int format)
{
    if (!w) return -1;
    if (width)  w->width  = width;
    if (height) w->height = height;
    if (format) w->format = format;
    return 0;
}
ABI_ATTR void ANativeWindow_acquire(ANativeWindow *w) { (void)w; }
ABI_ATTR void ANativeWindow_release(ANativeWindow *w) { (void)w; }
