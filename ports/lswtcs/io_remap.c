/* Path remapping for the four big asset packs.
 *
 * On Android the game opens its data via hardcoded paths like
 *   /data/user/0/com.wb.lego.tcs/files/assetpacks/asset_Audio/.../Audio.dat
 * which obviously don't exist on a Linux box. We intercept fopen/open by
 * patching the stdio_bridge's lookup table at startup, rewriting any path
 * matching one of the known prefixes onto tt_data_path()/<Pack>.dat.
 *
 * The actual rewriting is centralized in tt_path_remap() so other bridges
 * (e.g. anything that calls stat()/access()) can route through it too. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libtt.h"

#define INTERNAL_PATH "/data/user/0/com.wb.lego.tcs/files"

struct remap_entry {
    const char *prefix;
    const char *replacement;
};

static const struct remap_entry remaps[] = {
    {INTERNAL_PATH "/assetpacks/asset_Audio/",    "Audio.dat"},
    {INTERNAL_PATH "/assetpacks/asset_Levels/",   "Levels.dat"},
    {INTERNAL_PATH "/assetpacks/asset_Others/",   "Others.dat"},
    {INTERNAL_PATH "/assetpacks/asset_Textures/", "Textures.dat"},
    {NULL, NULL}
};

/* Returns a static buffer; caller must NOT free. Returns the original
 * pointer when the path is not one we want to rewrite, which keeps the
 * common case zero-cost. */
const char *tt_path_remap(const char *path)
{
    static char buf[1024];
    if (!path) return path;

    for (int i = 0; remaps[i].prefix; i++) {
        size_t n = strlen(remaps[i].prefix);
        if (strncmp(path, remaps[i].prefix, n) == 0) {
            snprintf(buf, sizeof(buf), "%s%s", tt_data_path(), remaps[i].replacement);
            return buf;
        }
    }

    /* General fallback: rewrite any reference to the Android internal
     * dir onto the host data path. Keeps savegames and per-user config
     * files reachable. */
    if (strncmp(path, INTERNAL_PATH, strlen(INTERNAL_PATH)) == 0) {
        snprintf(buf, sizeof(buf), "%s%s", tt_data_path(),
                 path + strlen(INTERNAL_PATH) + 1);
        return buf;
    }
    return path;
}

/* Soloader-flavored fopen/open wrappers that consult the remapper before
 * falling through to libc. Crucially fopen MUST go through the stdio
 * bridge's bridge_fopen so the returned handle is wrapped in BRIDGE_FILE -
 * otherwise fseek/fread/etc. dereference fp->real on a raw glibc FILE*
 * and segfault. open() returns a plain int fd, so it can stay simple. */
#include "stdio_bridge.h"
extern BRIDGE_FILE *bridge_fopen(const char *path, const char *mode);

BRIDGE_FILE *tt_fopen(const char *path, const char *mode)
{
    return bridge_fopen(tt_path_remap(path), mode);
}

int tt_open(const char *path, int flags, int mode)
{
    extern int open(const char *, int, ...);
    return open(tt_path_remap(path), flags, mode);
}
